# Only the selected ggml kernels receive ISA flags. The dispatcher, backend
# registration and the rest of the executable keep their portable baseline.
function(opt_add_windows_cpu_kernels)
    set(dispatch_dir "${PROJECT_SOURCE_DIR}/src/platform/windows")
    set(kernel_list "${dispatch_dir}/quant_kernels.inc")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${kernel_list}")
    file(STRINGS "${kernel_list}" entries REGEX "^OPT_(ROW|DOT)\\(")
    foreach(entry IN LISTS entries)
        string(REGEX REPLACE "^OPT_[A-Z]+\\(([A-Za-z0-9_]+)\\)$" "\\1" name "${entry}")
        list(APPEND portable_names "${name}=${name}_portable")
        list(APPEND avx2_names "${name}=${name}_avx2")
    endforeach()

    set(cpu_dir "${OPT_DEPENDENCIES}/llama.cpp/ggml/src/ggml-cpu")
    set(isa_flags "/clang:-mavx2;/clang:-mfma;/clang:-mf16c")
    set(avx2 "${CMAKE_CURRENT_BINARY_DIR}/windows_quants_avx2.c")
    file(CONFIGURE OUTPUT "${avx2}" CONTENT "#include \"arch/x86/quants.c\"\n")
    target_sources(ggml-cpu PRIVATE "${avx2}" "${dispatch_dir}/cpu_dispatch.cpp")
    set_source_files_properties("${cpu_dir}/arch/x86/quants.c" TARGET_DIRECTORY ggml-cpu
        PROPERTIES COMPILE_DEFINITIONS "${portable_names}")
    set_source_files_properties("${avx2}" TARGET_DIRECTORY ggml-cpu
        PROPERTIES COMPILE_DEFINITIONS "${avx2_names}"
                   COMPILE_OPTIONS "${isa_flags}")
    # ggml calls these packed GEMV/GEMM kernels only for tensors assigned
    # packed storage by its baseline selector. Use the same runtime gate.
    set_property(SOURCE "${cpu_dir}/repack.cpp" TARGET_DIRECTORY ggml-cpu
        APPEND PROPERTY COMPILE_DEFINITIONS ggml_cpu_has_avx2=opt_windows_has_avx2_kernels)
    set_property(SOURCE "${cpu_dir}/arch/x86/repack.cpp" TARGET_DIRECTORY ggml-cpu
        APPEND PROPERTY COMPILE_OPTIONS ${isa_flags})

    # Flash attention does both tiled prompt evaluation and FP16 accumulation
    # over the KV cache. Keep its ggml implementation and workspace layout.
    # Prefix symbols in the second copy of the pinned sources. Only flash
    # attention, the FP16/FP32 dot product, and SwiGLU are linked by the dispatcher.
    set(ops "${cpu_dir}/ops.cpp")
    set(vec "${cpu_dir}/vec.cpp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ops}" "${vec}")
    file(STRINGS "${ops}" op_definitions REGEX "^void ggml_compute_forward_[A-Za-z0-9_]+\\(")
    file(STRINGS "${vec}" vec_definitions REGEX "^(void|ggml_float|ggml_fp16_t) ggml_(vec_|table_)")
    foreach(definition IN LISTS op_definitions vec_definitions)
        string(REGEX MATCH "ggml_(compute_forward|vec|table)_[A-Za-z0-9_]+" symbol "${definition}")
        list(APPEND fast_ops "${symbol}=${symbol}_avx2")
    endforeach()
    set(attention "${CMAKE_CURRENT_BINARY_DIR}/windows_attention_avx2.cpp")
    file(CONFIGURE OUTPUT "${attention}" CONTENT "#include \"ggml-cpu/vec.cpp\"\n#include \"ggml-cpu/ops.cpp\"\n")
    target_sources(ggml-cpu PRIVATE "${attention}")
    set_source_files_properties("${attention}" TARGET_DIRECTORY ggml-cpu PROPERTIES
        COMPILE_DEFINITIONS "${fast_ops}" COMPILE_OPTIONS "${isa_flags};/Gy")
    set_property(SOURCE "${ops}" TARGET_DIRECTORY ggml-cpu APPEND PROPERTY COMPILE_DEFINITIONS
        ggml_compute_forward_flash_attn_ext=ggml_compute_forward_flash_attn_ext_portable)
    set_property(SOURCE "${vec}" TARGET_DIRECTORY ggml-cpu APPEND PROPERTY COMPILE_DEFINITIONS
        ggml_vec_dot_f16_f32=ggml_vec_dot_f16_f32_portable ggml_vec_swiglu_f32=ggml_vec_swiglu_f32_portable)
endfunction()
opt_add_windows_cpu_kernels()
