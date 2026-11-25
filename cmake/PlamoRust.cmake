# Windows only. Rust is a static library; the app neither spawns a CLI nor
# needs a Rust runtime, model server, GPU driver or extra DLL.
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64)$" OR NOT MSVC)
    message(FATAL_ERROR "Experimental PLaMo Rust backend requires Windows x64 MSVC-compatible toolchain")
endif()
find_program(OPT_CARGO cargo REQUIRED)
set(opt_rust_src "${PROJECT_SOURCE_DIR}/src/platform/windows/plamo-rs")
set(opt_rust_target "${CMAKE_CURRENT_BINARY_DIR}/plamo-rust")
set(opt_rust_lib "${opt_rust_target}/x86_64-pc-windows-msvc/release/plamo3_translate_rs.lib")
file(GLOB opt_rust_sources CONFIGURE_DEPENDS "${opt_rust_src}/src/*.rs")
add_custom_command(OUTPUT "${opt_rust_lib}"
    COMMAND "${CMAKE_COMMAND}" -E env "RUSTFLAGS=-C target-feature=+crt-static -C control-flow-guard=yes"
        "${OPT_CARGO}" build --release --locked --offline --lib
        --target x86_64-pc-windows-msvc --target-dir "${opt_rust_target}"
        --manifest-path "${opt_rust_src}/Cargo.toml"
    DEPENDS "${opt_rust_src}/Cargo.toml" "${opt_rust_src}/Cargo.lock" "${opt_rust_src}/rust-toolchain.toml" ${opt_rust_sources}
    WORKING_DIRECTORY "${opt_rust_src}" VERBATIM)
add_custom_target(opt_plamo_rust_build DEPENDS "${opt_rust_lib}")
add_library(opt_plamo_rust STATIC IMPORTED GLOBAL)
set_target_properties(opt_plamo_rust PROPERTIES IMPORTED_LOCATION "${opt_rust_lib}")
add_dependencies(opt_plamo_rust opt_plamo_rust_build)
target_link_libraries(opt_inference PRIVATE opt_plamo_rust advapi32 bcrypt kernel32 ntdll userenv ws2_32 dbghelp synchronization)
# Rust release static CRT and C++ debug CRT cannot share default libraries.
# The C ABI never transfers allocation ownership across languages.
target_link_options(opt_inference INTERFACE "$<$<CONFIG:Debug>:/NODEFAULTLIB:libcmt>")
