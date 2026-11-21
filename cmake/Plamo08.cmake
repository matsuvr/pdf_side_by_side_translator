# Sources are fetched by scripts/bootstrap.py, never during CMake configuration.
foreach(dep mlx json fmt metal-cpp)
    if(NOT IS_DIRECTORY "${OPT_DEPENDENCIES}/${dep}")
        message(FATAL_ERROR "Missing ${dep}: run scripts/prepare_mlx.py --dependencies-only")
    endif()
endforeach()
set(FETCHCONTENT_SOURCE_DIR_JSON "${OPT_DEPENDENCIES}/json" CACHE PATH "" FORCE)
set(FETCHCONTENT_SOURCE_DIR_FMT "${OPT_DEPENDENCIES}/fmt" CACHE PATH "" FORCE)
set(FETCHCONTENT_SOURCE_DIR_METAL_CPP "${OPT_DEPENDENCIES}/metal-cpp" CACHE PATH "" FORCE)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
foreach(option TESTS EXAMPLES BENCHMARKS PYTHON_BINDINGS PYTHON_STUBS GGUF CUDA CPU)
    set(MLX_BUILD_${option} OFF CACHE BOOL "" FORCE)
endforeach()
set(MLX_BUILD_SAFETENSORS ON CACHE BOOL "" FORCE)
set(MLX_BUILD_METAL ON CACHE BOOL "" FORCE)
set(MLX_METAL_JIT OFF CACHE BOOL "" FORCE)
set(MLX_USE_CCACHE OFF CACHE BOOL "" FORCE)
set(MLX_METAL_PATH "${CMAKE_BINARY_DIR}/mlx-kernels" CACHE PATH "" FORCE)
add_subdirectory("${OPT_DEPENDENCIES}/mlx" third_party/mlx EXCLUDE_FROM_ALL)
add_library(plamo08 STATIC
    third_party/plamo08/src/model.cpp
    third_party/plamo08/src/kernels.cpp
    third_party/plamo08/src/tokenizer.cpp)
target_include_directories(plamo08 PUBLIC "${PROJECT_SOURCE_DIR}/third_party/plamo08/include")
target_link_libraries(plamo08 PUBLIC mlx nlohmann_json::nlohmann_json)
target_compile_features(plamo08 PUBLIC cxx_std_20)

function(opt_copy_mlx_metallib target)
    add_dependencies(${target} mlx-metallib)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MLX_METAL_PATH}/mlx.metallib" "$<TARGET_FILE_DIR:${target}>/mlx.metallib"
        VERBATIM)
endfunction()
