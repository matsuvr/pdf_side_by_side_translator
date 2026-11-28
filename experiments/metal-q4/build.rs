#[cfg(feature = "reference")]
mod reference {
    include!("reference/llama-build.rs");
    pub fn prepare() {
        main()
    }
}

use std::{
    path::{Path, PathBuf},
    process::Command,
};

fn find_cryptex_tool(tool: &str) -> Option<PathBuf> {
    let dir = Path::new("/var/run/com.apple.security.cryptexd/mnt");
    for entry in std::fs::read_dir(dir).ok()?.flatten() {
        let name = entry.file_name();
        if name.to_string_lossy().starts_with("com.apple.MobileAsset.MetalToolchain") {
            let candidate = entry.path().join("Metal.xctoolchain/usr/bin").join(tool);
            if candidate.is_file() {
                return Some(candidate);
            }
        }
    }
    None
}

fn run_metal_tool(tool: &str, args: &[&str], inputs: &[PathBuf], output: &Path) {
    let status = Command::new("xcrun")
        .env("XCRUN_NO_CACHE", "1")
        .args(["-sdk", "macosx", tool])
        .args(args)
        .args(inputs)
        .arg("-o")
        .arg(output)
        .status();
    if matches!(status, Ok(s) if s.success()) {
        return;
    }
    if let Some(bin) = find_cryptex_tool(tool) {
        let fallback = Command::new(bin)
            .args(args)
            .args(inputs)
            .arg("-o")
            .arg(output)
            .status()
            .unwrap();
        assert!(
            fallback.success(),
            "Metal {tool} failed for {}",
            output.display()
        );
        return;
    }
    panic!("Metal {tool} failed for {}", output.display());
}

fn main() {
    #[cfg(feature = "reference")]
    reference::prepare();
    println!("cargo:rerun-if-changed=src/kernels.metal");
    let out_dir = PathBuf::from(std::env::var_os("OUT_DIR").unwrap());
    let src = PathBuf::from("src/kernels.metal");
    let air = out_dir.join("kernels.air");
    let lib = out_dir.join("plamo.metallib");

    run_metal_tool(
        "metal",
        &[
            "-std=metal3.1",
            "-mmacosx-version-min=14.0",
            "-O3",
            "-c",
        ],
        &[src],
        &air,
    );
    run_metal_tool("metallib", &[], &[air], &lib);
}
