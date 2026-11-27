use anyhow::{Context, Result, ensure};
use dispatch2::DispatchData;
use objc2::{rc::Retained, runtime::ProtocolObject};
use objc2_foundation::NSString;
use objc2_metal::*;
use std::ptr::NonNull;

include!(concat!(env!("OUT_DIR"), "/symbols.rs"));

type Object<T> = Retained<ProtocolObject<T>>;
pub type Pipeline = Object<dyn MTLComputePipelineState>;
pub struct Buffer(Object<dyn MTLBuffer>);

pub struct Gpu {
    device: Object<dyn MTLDevice>,
    library: Object<dyn MTLLibrary>,
    queue: Object<dyn MTLCommandQueue>,
    states: std::cell::RefCell<std::collections::HashMap<String, Pipeline>>,
}

impl Gpu {
    pub fn open() -> Result<Self> {
        let device = MTLCreateSystemDefaultDevice().context("Metal GPU が見つかりません。")?;
        ensure!(
            device.supportsFamily(MTLGPUFamily::Apple7),
            "Apple Silicon の Metal GPU が必要です。"
        );
        let library = device
            .newLibraryWithData_error(&DispatchData::from_static_bytes(include_bytes!(concat!(
                env!("OUT_DIR"),
                "/plamo.metallib"
            ))))
            .map_err(|e| anyhow::anyhow!("{e}"))?;
        let queue = device
            .newCommandQueue()
            .context("Metal command queue unavailable")?;
        Ok(Self {
            device,
            queue,
            library,
            states: Default::default(),
        })
    }
    pub fn pipeline(&self, name: &str) -> Result<Pipeline> {
        if let Some(state) = self.states.borrow().get(name) {
            return Ok(state.clone());
        }
        let fun = self
            .library
            .newFunctionWithName(&NSString::from_str(
                symbol(name).context("unknown Metal kernel")?,
            ))
            .with_context(|| format!("missing Metal function {name}"))?;
        let state = self
            .device
            .newComputePipelineStateWithFunction_error(&fun)
            .map_err(|e| anyhow::anyhow!("{e}"))?;
        ensure!(
            state.threadExecutionWidth() == 32,
            "unsupported Metal SIMD group"
        );
        self.states
            .borrow_mut()
            .insert(name.to_owned(), state.clone());
        Ok(state)
    }
    pub fn upload(&self, bytes: &[u8]) -> Result<Buffer> {
        // Metal copies the entire slice before returning. Shared storage is
        // CPU-visible on Apple Silicon and remains owned by the Metal buffer.
        let ptr = NonNull::from(bytes).cast();
        Ok(Buffer(
            unsafe {
                self.device.newBufferWithBytes_length_options(
                    ptr,
                    bytes.len(),
                    MTLResourceOptions::StorageModeShared,
                )
            }
            .context("Metal upload allocation failed")?,
        ))
    }
    pub fn empty(&self, bytes: usize) -> Result<Buffer> {
        Ok(Buffer(
            self.device
                .newBufferWithLength_options(bytes, MTLResourceOptions::StorageModeShared)
                .context("Metal buffer allocation failed")?,
        ))
    }
    pub fn submit(&self, record: impl FnOnce(&Commands)) -> Result<()> {
        objc2::rc::autoreleasepool(|_| {
            let command = self
                .queue
                .commandBuffer()
                .context("Metal command buffer unavailable")?;
            let encoder = command
                .computeCommandEncoder()
                .context("Metal encoder unavailable")?;
            // The default serial encoder enforces all producer/consumer dependencies.
            record(&Commands(encoder.clone()));
            encoder.endEncoding();
            command.commit();
            command.waitUntilCompleted();
            if let Some(error) = command.error() {
                anyhow::bail!("Metal command failed: {error}");
            }
            ensure!(command.status() == MTLCommandBufferStatus::Completed);
            Ok(())
        })
    }
}
impl Buffer {
    // Only called after the submitting command has completed, with exclusive
    // access to this translation session. Each buffer was allocated at offset zero.
    pub fn read_scores(&self) -> Vec<f32> {
        let ptr = self.0.contents().cast::<f32>();
        unsafe { std::slice::from_raw_parts(ptr.as_ptr(), self.0.length() / 4) }.to_vec()
    }
}

pub struct Commands(Object<dyn MTLComputeCommandEncoder>);
impl Commands {
    pub fn dispatch(
        &self,
        pipeline: &Pipeline,
        groups: (usize, usize),
        threads: usize,
        buffers: &[&Buffer],
        scalar: Option<u32>,
    ) {
        self.0.setComputePipelineState(pipeline);
        for (i, b) in buffers.iter().enumerate() {
            // Bindings follow the argument order of the generated kernel.
            unsafe {
                self.0.setBuffer_offset_atIndex(Some(&b.0), 0, i);
            }
        }
        // CubeCL groups scalar arguments into an 8-byte-aligned info block.
        // The fixed-grid kernels compute all addresses from validated model
        // dimensions and never index variable tensor metadata.
        let mut info = [0u32; 10];
        info[0] = scalar.unwrap_or(0);
        unsafe {
            self.0.setBytes_length_atIndex(
                NonNull::from(&info).cast(),
                std::mem::size_of_val(&info),
                buffers.len(),
            );
        }
        self.0.dispatchThreadgroups_threadsPerThreadgroup(
            MTLSize {
                width: groups.0,
                height: groups.1,
                depth: 1,
            },
            MTLSize {
                width: threads,
                height: 1,
                depth: 1,
            },
        );
    }
}
