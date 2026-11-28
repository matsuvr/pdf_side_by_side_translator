use anyhow::{Context, Result, ensure};
use dispatch2::DispatchData;
use objc2::{rc::Retained, runtime::ProtocolObject};
use objc2_foundation::NSString;
use objc2_metal::*;
use std::ptr::NonNull;

type Object<T> = Retained<ProtocolObject<T>>;
pub type Pipeline = Object<dyn MTLComputePipelineState>;

pub struct Buffer(Object<dyn MTLBuffer>);

/// A slice view into a Metal buffer at a 16-byte-aligned byte offset.
#[derive(Clone, Copy)]
pub struct BufferSlice<'a> {
    pub buffer: &'a Buffer,
    pub offset: usize,
}

impl Buffer {
    pub fn slice(&self, offset: usize) -> BufferSlice<'_> {
        BufferSlice {
            buffer: self,
            offset,
        }
    }
    pub fn as_slice(&self) -> BufferSlice<'_> {
        BufferSlice {
            buffer: self,
            offset: 0,
        }
    }
    /// Safety: Caller must ensure no GPU work is currently accessing this buffer.
    pub unsafe fn bytes_mut(&self) -> &mut [u8] {
        let ptr = self.0.contents().cast::<u8>();
        unsafe { std::slice::from_raw_parts_mut(ptr.as_ptr(), self.0.length()) }
    }
    /// Safety: Caller must ensure no GPU work is currently writing to this buffer.
    pub unsafe fn bytes(&self) -> &[u8] {
        let ptr = self.0.contents().cast::<u8>();
        unsafe { std::slice::from_raw_parts(ptr.as_ptr(), self.0.length()) }
    }
    pub fn write_slice<T: bytemuck::Pod>(&self, data: &[T]) {
        let src = bytemuck::cast_slice::<T, u8>(data);
        assert!(src.len() <= self.0.length());
        unsafe {
            self.bytes_mut()[..src.len()].copy_from_slice(src);
        }
    }
    pub fn read_slice<T: bytemuck::Pod>(&self, count: usize) -> Vec<T> {
        let byte_len = count * std::mem::size_of::<T>();
        assert!(byte_len <= self.0.length());
        let bytes = unsafe { &self.bytes()[..byte_len] };
        bytemuck::cast_slice(bytes).to_vec()
    }
    pub fn read_u32_at(&self, index: usize) -> u32 {
        let ptr = self.0.contents().cast::<u32>();
        assert!((index + 1) * 4 <= self.0.length());
        unsafe { *ptr.as_ptr().add(index) }
    }
    pub fn write_u32_at(&self, index: usize, value: u32) {
        let ptr = self.0.contents().cast::<u32>();
        assert!((index + 1) * 4 <= self.0.length());
        unsafe {
            *ptr.as_ptr().add(index) = value;
        }
    }
    pub fn read_scores(&self) -> Vec<f32> {
        self.read_slice(self.0.length() / 4)
    }
}

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
            .newFunctionWithName(&NSString::from_str(name))
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
                .newBufferWithLength_options(bytes.max(16), MTLResourceOptions::StorageModeShared)
                .context("Metal buffer allocation failed")?,
        ))
    }

    pub fn empty_private(&self, bytes: usize) -> Result<Buffer> {
        Ok(Buffer(
            self.device
                .newBufferWithLength_options(bytes.max(16), MTLResourceOptions::StorageModePrivate)
                .context("Metal private buffer allocation failed")?,
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

pub struct Commands(Object<dyn MTLComputeCommandEncoder>);

impl Commands {
    pub fn dispatch<P: bytemuck::Pod>(
        &self,
        pipeline: &Pipeline,
        groups: (usize, usize),
        threads: usize,
        buffers: &[BufferSlice<'_>],
        params: Option<&P>,
    ) {
        self.0.setComputePipelineState(pipeline);
        for (i, b) in buffers.iter().enumerate() {
            unsafe {
                self.0.setBuffer_offset_atIndex(Some(&b.buffer.0), b.offset, i);
            }
        }
        if let Some(p) = params {
            let bytes = bytemuck::bytes_of(p);
            unsafe {
                self.0.setBytes_length_atIndex(
                    NonNull::from(bytes).cast(),
                    bytes.len(),
                    buffers.len(),
                );
            }
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
