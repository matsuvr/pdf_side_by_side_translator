#[cfg(feature = "reference")]
mod reference {
    include!("reference/llama-build.rs");
    pub fn prepare() {
        main()
    }
}
#[path = "src/kernels.rs"]
mod kernels;
use cubecl::{Compiler, CubeKernel, frontend::BufferCompilationArg, prelude::*};
use cubecl_cpp::{
    shared::{CompilationOptions, CppCompiler},
    target::Metal,
};
use std::{
    path::{Path, PathBuf},
    process::Command,
};

fn xcrun(args: &[&str], files: &[PathBuf], output: &Path) {
    let status = Command::new("xcrun")
        .args(args)
        .args(files)
        .arg("-o")
        .arg(output)
        .status()
        .unwrap();
    assert!(
        status.success(),
        "Metal compiler failed for {}",
        output.display()
    );
}

// Describe the fixed Apple7 kernel contract while building. This generates
// the same code on each build machine, without opening a Metal device.
fn apple_gpu() -> DeviceProperties {
    use cubecl::ir::{
        DeviceIdentity, FloatKind, HardwareProperties, IntKind, MemoryDeviceProperties, UIntKind,
        features::{Plane, TypeUsage},
    };
    let hw = HardwareProperties {
        load_width: 128,
        vector_register_count: None,
        plane_size_min: 32,
        plane_size_max: 32,
        max_bindings: 30,
        max_shared_memory_size: 32768,
        max_cube_count: (u32::MAX, u32::MAX, u32::MAX),
        max_units_per_cube: 256,
        max_cube_dim: (256, 1, 1),
        num_streaming_multiprocessors: None,
        num_cpu_cores: None,
        last_level_cache_size: None,
        num_tensor_cores: None,
        min_tensor_cores_dim: None,
        max_vector_size: 4,
        cube_mma_reserved_shared_memory: 0,
    };
    let mut props = DeviceProperties::new(
        Default::default(),
        MemoryDeviceProperties::new(256 << 20, 256),
        hw,
        cubecl::profile::TimingMethod::Device,
        DeviceIdentity::default(),
    );
    props.register_address_type(AddressType::U32);
    for ty in [
        ElemType::Bool,
        ElemType::UInt(UIntKind::U32),
        ElemType::Int(IntKind::I32),
        ElemType::Float(FloatKind::F16),
        ElemType::Float(FloatKind::F32),
    ] {
        props.register_type_usage(ty, TypeUsage::all());
    }
    use cubecl_cpp::metal::{arch::MetalArchitecture, supported_cmma_combinations_metal};
    cubecl_cpp::shared::register_wmma_features(
        supported_cmma_combinations_metal(&MetalArchitecture::Metal3),
        &mut props,
    );
    props.features.plane.insert(Plane::Ops);
    props.features.plane.insert(Plane::Sync);
    props
}

fn main() {
    #[cfg(feature = "reference")]
    reference::prepare();
    println!("cargo:rerun-if-changed=src/kernels.rs");
    let dir = PathBuf::from(std::env::var_os("OUT_DIR").unwrap());
    let device = std::sync::Arc::new(apple_gpu());
    let props = || device.clone();
    let target = || std::sync::Arc::new(TargetProperties::default());
    let settings = |threads| {
        KernelSettings::new(
            CubeDim::new_1d(threads).into(),
            ExecutionMode::Unchecked,
            AddressType::U32,
        )
    };
    let buf = || BufferCompilationArg { inplace: None };
    let mut objects = Vec::new();
    let mut names = String::from("fn symbol(name:&str)->Option<&'static str>{match name {");
    let mut emit = |name: String, kernel: &dyn CubeKernel| {
        let def = kernel.define();
        let symbol = def.settings.kernel_name.clone();
        let compiled = CppCompiler::<Metal>::default()
            .compile(def, &CompilationOptions::default())
            .unwrap();
        let src = dir.join(format!("{name}.metal"));
        let air = dir.join(format!("{name}.air"));
        names.push_str(&format!("{name:?}=>Some({:?}),", symbol));
        std::fs::write(&src, compiled.to_string()).unwrap();
        xcrun(
            &[
                "-sdk",
                "macosx",
                "metal",
                "-std=metal3.1",
                "-mmacosx-version-min=14.0",
                "-O3",
                "-c",
            ],
            &[src],
            &air,
        );
        objects.push(air);
    };
    use kernels::*;
    emit(
        "embed".into(),
        &embed::Embed::new(settings(128), props(), target(), buf(), buf(), buf(), buf()),
    );
    emit(
        "swiglu".into(),
        &swiglu::Swiglu::new(settings(128), props(), target(), buf(), buf()),
    );
    emit(
        "prepare_qkv".into(),
        &prepare_qkv::PrepareQkv::new(
            settings(32),
            props(),
            target(),
            buf(),
            buf(),
            buf(),
            buf(),
            buf(),
            buf(),
            buf(),
            (),
        ),
    );
    for flag in [false, true] {
        emit(
            format!("norm_{}", flag as u8),
            &norm::Norm::new(
                settings(32),
                props(),
                target(),
                buf(),
                buf(),
                buf(),
                buf(),
                (),
                flag,
            ),
        );
        emit(
            format!("attention_{}", flag as u8),
            &attention::Attention::new(
                settings(256),
                props(),
                target(),
                buf(),
                buf(),
                buf(),
                buf(),
                (),
                flag,
            ),
        );
    }
    for (cols, rows) in [
        (1536, 2304),
        (2048, 1536),
        (1536, 12288),
        (6144, 1536),
        (1536, 107520),
    ] {
        emit(
            format!("mv_{cols}_{rows}"),
            &matvec::Matvec::new(
                settings(128),
                props(),
                target(),
                buf(),
                buf(),
                buf(),
                buf(),
                cols,
                rows,
            ),
        );
        if rows != 107520 {
            emit(
                format!("mm_{cols}_{rows}"),
                &matmul::Matmul::new(
                    settings(256),
                    props(),
                    target(),
                    buf(),
                    buf(),
                    buf(),
                    buf(),
                    (),
                    cols,
                    rows,
                ),
            );
        }
    }
    names.push_str("_=>None}}");
    std::fs::write(dir.join("symbols.rs"), names).unwrap();
    xcrun(
        &["-sdk", "macosx", "metallib"],
        &objects,
        &dir.join("plamo.metallib"),
    );
}
