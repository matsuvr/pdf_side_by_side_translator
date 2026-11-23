# No codecs, network client, OpenMP runtime, training tools or GUI server.
foreach(dep Leptonica Tesseract)
    if(NOT EXISTS "${OPT_DEPENDENCIES}/${dep}/CMakeLists.txt")
        message(FATAL_ERROR "Missing ${dep}: run python scripts/bootstrap_image_reading.py")
    endif()
endforeach()
foreach(codec ZLIB PNG GIF JPEG TIFF WEBP OPENJPEG)
    set(ENABLE_${codec} OFF CACHE BOOL "Disabled for memory-only image reading" FORCE)
endforeach()
set(SW_BUILD OFF CACHE BOOL "" FORCE)
set(BUILD_PROG OFF CACHE BOOL "" FORCE)
add_subdirectory("${OPT_DEPENDENCIES}/Leptonica" third_party/image-lept EXCLUDE_FROM_ALL)

# Tesseract consumes a Leptonica config when both are built in the same graph.
set(image_lept_inc "${CMAKE_CURRENT_BINARY_DIR}/image-lept-include")
set(image_lept_config "${CMAKE_CURRENT_BINARY_DIR}/image-lept-config")
file(MAKE_DIRECTORY "${image_lept_inc}/leptonica" "${image_lept_config}")
file(GLOB image_lept_headers "${OPT_DEPENDENCIES}/Leptonica/src/*.h")
file(COPY ${image_lept_headers} DESTINATION "${image_lept_inc}/leptonica")
file(COPY "${CMAKE_CURRENT_BINARY_DIR}/third_party/image-lept/src/endianness.h" DESTINATION "${image_lept_inc}/leptonica")
file(WRITE "${image_lept_config}/LeptonicaConfig.cmake"
    "set(Leptonica_VERSION 1.85.0)\nset(Leptonica_INCLUDE_DIRS \"${image_lept_inc};${image_lept_inc}/leptonica\")\nset(Leptonica_LIBRARIES leptonica)\n")
include(CMakePackageConfigHelpers)
write_basic_package_version_file("${image_lept_config}/LeptonicaConfigVersion.cmake"
    VERSION 1.85.0 COMPATIBILITY AnyNewerVersion)
set(Leptonica_DIR "${image_lept_config}" CACHE PATH "" FORCE)
foreach(flag DISABLE_TIFF DISABLE_ARCHIVE DISABLE_CURL GRAPHICS_DISABLED WIN32_MT_BUILD)
    set(${flag} ON CACHE BOOL "" FORCE)
endforeach()
foreach(flag DISABLED_LEGACY_ENGINE OPENMP_BUILD BUILD_TRAINING_TOOLS BUILD_TESTS ENABLE_NATIVE ENABLE_LTO INSTALL_CONFIGS)
    set(${flag} OFF CACHE BOOL "" FORCE)
endforeach()
add_subdirectory("${OPT_DEPENDENCIES}/Tesseract" third_party/image-tess EXCLUDE_FROM_ALL)
# Keep the upstream full-page segmentation code. Route every caller of its
# optional disk-based zone reader to the viewer's memory-only implementation.
set_property(SOURCE "${OPT_DEPENDENCIES}/Tesseract/src/ccstruct/blread.cpp"
    TARGET_DIRECTORY libtesseract APPEND PROPERTY COMPILE_DEFINITIONS read_unlv_file=unused_disk_zone_reader)
target_sources(libtesseract PRIVATE "${PROJECT_SOURCE_DIR}/src/ocr/memory_zones.cpp")
if(MSVC)
    # Tesseract passes a char buffer to GetModuleFileName in this file.
    set_property(SOURCE "${OPT_DEPENDENCIES}/Tesseract/src/ccutil/ccutil.cpp"
        TARGET_DIRECTORY libtesseract APPEND PROPERTY COMPILE_OPTIONS /UUNICODE /U_UNICODE)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        # Parse clang-cl's vector types before Tesseract defines __attribute__ away.
        target_compile_options(libtesseract PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/FIintrin.h>")
    endif()
endif()
if(WIN32)
    set(image_platform windows)
else()
    set(image_platform macos)
    enable_language(ASM)
endif()
add_library(opt_image_reading STATIC src/ocr/image_reading.cpp src/platform/${image_platform}/image_worker.cpp)
target_include_directories(opt_image_reading PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../src")
target_link_libraries(opt_image_reading PRIVATE libtesseract)
target_include_directories(opt_image_reading PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/third_party/image-tess/include")

set(cld_root "${OPT_DEPENDENCIES}/CLD2")
if(NOT EXISTS "${cld_root}/public/compact_lang_det.h")
    message(FATAL_ERROR "Missing CLD2: run python scripts/bootstrap_image_reading.py")
endif()
# Upstream compile_libs.sh, full statically linked language tables.
set(cld_sources cldutil cldutil_shared compact_lang_det compact_lang_det_hint_code
    debug fixunicodevalue generated_entities generated_language generated_ulscript
    getonescriptspan lang_script offsetmap scoreonescriptspan tote utf8statetable
    cld_generated_cjk_uni_prop_80 cld2_generated_cjk_compatible cld_generated_cjk_delta_bi_32
    generated_distinct_bi_0 cld2_generated_quad0122 cld2_generated_deltaocta0122
    cld2_generated_distinctocta0122 cld_generated_score_quad_octa_0122)
list(TRANSFORM cld_sources PREPEND "${cld_root}/internal/")
list(TRANSFORM cld_sources APPEND ".cc")
# CLD2 uses 0xffff for an empty score slot. Keep it out of the Language enum.
# Apply the common fix to a build-local copy of the pinned translation unit.
set(cld_fix_dir "${CMAKE_CURRENT_BINARY_DIR}/cld2")
set(cld_impl "compact_lang_det_impl.cc")
set(cld_patch "${PROJECT_SOURCE_DIR}/deps/patches/cld2-skip-empty-language-slots.patch")
file(MAKE_DIRECTORY "${cld_fix_dir}")
add_custom_command(OUTPUT "${cld_fix_dir}/${cld_impl}"
    COMMAND "${CMAKE_COMMAND}" -E copy "${cld_root}/internal/${cld_impl}" "${cld_fix_dir}/${cld_impl}"
    COMMAND "${CMAKE_COMMAND}" -E env "GIT_CEILING_DIRECTORIES=${CMAKE_CURRENT_BINARY_DIR}"
        "${GIT_EXECUTABLE}" apply --no-index --ignore-space-change "${cld_patch}"
    WORKING_DIRECTORY "${cld_fix_dir}"
    DEPENDS "${cld_root}/internal/${cld_impl}" "${cld_patch}" VERBATIM)
add_library(opt_cld2 STATIC ${cld_sources} "${cld_fix_dir}/${cld_impl}")
if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(opt_cld2 PRIVATE /clang:-Wno-c++11-narrowing)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(opt_cld2 PRIVATE -Wno-narrowing)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(opt_cld2 PRIVATE -Wno-c++11-narrowing)
endif()
target_include_directories(opt_cld2 PRIVATE "${cld_root}/internal")
target_include_directories(opt_cld2 INTERFACE "${cld_root}")
target_link_libraries(opt_image_reading PRIVATE opt_cld2)

set(OPT_IMAGE_MODEL_DIR "${PROJECT_SOURCE_DIR}/.work/resources/tessdata" CACHE PATH "Verified OCR data")
set(image_asset_root "${CMAKE_CURRENT_LIST_DIR}/..")
set(image_generated "${CMAKE_CURRENT_BINARY_DIR}/image-reading")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
file(STRINGS "${image_asset_root}/deps/image_reading_models.tsv" image_model_rows)
set(image_model_files)
foreach(row IN LISTS image_model_rows)
    if(row MATCHES "^([a-zA-Z_/]+)[ 	]+[0-9a-f]+")
        list(APPEND image_model_files "${OPT_IMAGE_MODEL_DIR}/${CMAKE_MATCH_1}.traineddata")
    endif()
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${image_asset_root}/scripts/image_reading_assets.py"
    "${image_asset_root}/deps/image_reading_languages.tsv"
    "${image_asset_root}/deps/image_reading_models.tsv" ${image_model_files})
execute_process(COMMAND "${Python3_EXECUTABLE}" "${image_asset_root}/scripts/image_reading_assets.py"
    --out "${image_generated}" --model-dir "${OPT_IMAGE_MODEL_DIR}" --platform "${image_platform}"
    COMMAND_ERROR_IS_FATAL ANY)
target_include_directories(opt_image_reading PUBLIC "${image_generated}")
if(WIN32)
    set(image_rc "${image_generated}/windows-image-reading.rc")
    set_source_files_properties("${image_rc}" PROPERTIES OBJECT_DEPENDS "${image_model_files}")
    set(image_model_sources src/platform/windows/image_models.cpp "${image_rc}")
else()
    set(image_asm "${image_generated}/macos-image-reading.s")
    set_source_files_properties("${image_asm}" PROPERTIES OBJECT_DEPENDS "${image_model_files}")
    target_sources(opt_image_reading PRIVATE src/platform/macos/image_models.cpp "${image_asm}")
endif()
