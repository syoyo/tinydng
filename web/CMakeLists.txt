cmake_minimum_required(VERSION 3.16)

project(tinydng_web C)

if(NOT EMSCRIPTEN)
  message(FATAL_ERROR "The web target must be configured with emcmake.")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(TINYDNG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

# Decode-only build. The writer and PSD implementation are intentionally left
# out of the browser binary; DNG/TIFF decode still includes LJPEG, baseline
# JPEG, LZW, PackBits, and ZIP support.
set(TINYDNG_WEB_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/wasm/tinydng_web.c"
  "${TINYDNG_ROOT}/tinydng_api.c"
  "${TINYDNG_ROOT}/tinydng_io.c"
  "${TINYDNG_ROOT}/tinydng_tiff.c"
  "${TINYDNG_ROOT}/tinydng_dng.c"
  "${TINYDNG_ROOT}/tinydng_codec.c"
  "${TINYDNG_ROOT}/tinydng_miniz.c"
  "${TINYDNG_ROOT}/tinydng_stb_image.c"
  "${TINYDNG_ROOT}/tiny_dng_ljpeg92_v2.c")

add_executable(tinydng ${TINYDNG_WEB_SOURCES})
target_include_directories(tinydng PRIVATE "${TINYDNG_ROOT}")
target_compile_definitions(tinydng PRIVATE
  TINYDNG_NO_PSD
  TINYDNG_DISABLE_THREADS)
target_compile_options(tinydng PRIVATE -O3)

set_target_properties(tinydng PROPERTIES
  OUTPUT_NAME "tinydng"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/js/dist")

target_link_options(tinydng PRIVATE
  "--no-entry"
  "-sMODULARIZE=1"
  "-sEXPORT_ES6=1"
  "-sEXPORT_NAME=TinyDngModule"
  "-sENVIRONMENT=web,node"
  "-sALLOW_MEMORY_GROWTH=1"
  "-sINITIAL_MEMORY=67108864"
  "-sSTACK_SIZE=1048576"
  "-sNO_EXIT_RUNTIME=1"
  "-sFILESYSTEM=0"
  "-sASSERTIONS=0"
  "-sEXPORTED_FUNCTIONS=['_malloc','_free','_tinydng_web_info_size','_tinydng_web_open','_tinydng_web_close','_tinydng_web_is_valid','_tinydng_web_error','_tinydng_web_error_status','_tinydng_web_image_count','_tinydng_web_get_image_info','_tinydng_web_decode','_tinydng_web_decode_region']"
  "-sEXPORTED_RUNTIME_METHODS=['HEAPU8','UTF8ToString']")
