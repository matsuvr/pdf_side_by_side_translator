#[cfg(target_os = "macos")]
fn main() {
    use std::{env, path::PathBuf, process::Command};
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../.work/llama-install");
    let header = root.join("include/llama.h");
    assert!(
        header.is_file(),
        "Prepare the locked upstream with python3 scripts/prepare_llama.py"
    );
    let sdk = Command::new("xcrun")
        .arg("--show-sdk-path")
        .output()
        .expect("macOS SDK");
    bindgen::Builder::default()
        .header(header.to_str().unwrap()).clang_arg(format!("-I{}",root.join("include").display()))
        .clang_arg("-isysroot").clang_arg(String::from_utf8(sdk.stdout).unwrap().trim())
        .allowlist_function("llama_(supports_gpu_offload|backend_(init|free)|model_(default_params|load_from_file_ptr|free|get_vocab|n_ctx_train)|context_default_params|init_from_model|free|tokenize|token_to_piece|batch_get_one|decode|get_logits_ith|get_embeddings_ith|vocab_(n_tokens|is_control|is_eog)|log_set)")
        .allowlist_function("ggml_log_set")
        .layout_tests(false).generate_comments(false)
        .generate().expect("generate llama C bindings").write_to_file(PathBuf::from(env::var_os("OUT_DIR").unwrap()).join("llama.rs")).unwrap();
    println!("cargo:rerun-if-changed={}", root.join("include").display());
    println!("cargo:rerun-if-changed={}", root.join("lib").display());
    println!(
        "cargo:rustc-link-search=native={}",
        root.join("lib").display()
    );
    for lib in ["llama", "ggml", "ggml-cpu", "ggml-metal", "ggml-base"] {
        println!("cargo:rustc-link-lib=static={lib}");
    }
    let clang = Command::new("xcrun")
        .args(["clang", "--print-resource-dir"])
        .output()
        .unwrap();
    println!(
        "cargo:rustc-link-search=native={}/lib/darwin",
        String::from_utf8(clang.stdout).unwrap().trim()
    );
    println!("cargo:rustc-link-lib=static=clang_rt.osx");
    for fw in ["Metal", "MetalKit", "Accelerate", "Foundation"] {
        println!("cargo:rustc-link-lib=framework={fw}");
    }
    println!("cargo:rustc-link-lib=c++");
}
#[cfg(not(target_os = "macos"))]
fn main() {}
