# The release PDFium archive and the other viewer libraries use the normal
# static MSVC CRT. Keep its STL annotation ABI in this mixed-instrumentation build.
if(NOT (MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang"))
    message(FATAL_ERROR "OPT_OCR_ASAN requires the Windows clang-cl toolchain")
endif()
execute_process(COMMAND "${CMAKE_CXX_COMPILER}" /clang:-print-resource-dir
    OUTPUT_VARIABLE asan_dir OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
set(asan_runtime "${asan_dir}/lib/windows/clang_rt.asan_dynamic-x86_64")
set(asan_thunk "${asan_dir}/lib/windows/clang_rt.asan_static_runtime_thunk-x86_64.lib")
# ICF can fold distinct ASan globals and their redzones in this PE/COFF link.
# Preserve the correspondence between each registration and its object.
foreach(target libtesseract leptonica opt_cld2 opt_image_reading opt_pdf image_reading_test)
    target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:/fsanitize=address;/Z7>")
    target_compile_definitions(${target} PRIVATE _DISABLE_STL_ANNOTATION)
endforeach()
foreach(target opt_image_reading opt_pdf)
    target_link_options(${target} INTERFACE /OPT:REF,NOICF /DEBUG /INCREMENTAL:NO "/WHOLEARCHIVE:${asan_thunk}")
    target_link_libraries(${target} INTERFACE "${asan_runtime}.lib")
endforeach()
configure_file("${asan_runtime}.dll" "${CMAKE_CURRENT_BINARY_DIR}/clang_rt.asan_dynamic-x86_64.dll" COPYONLY)
