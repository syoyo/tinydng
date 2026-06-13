cmake_minimum_required(VERSION 3.16)
project(tinyDngV3Test C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Repository root (two levels up).
set(TINYDNG_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
include_directories(${TINYDNG_ROOT})

add_library(tinydng_v3 STATIC
    ${TINYDNG_ROOT}/tinydng_api.c
    ${TINYDNG_ROOT}/tinydng_io.c
    ${TINYDNG_ROOT}/tinydng_tiff.c
    ${TINYDNG_ROOT}/tinydng_dng.c
    ${TINYDNG_ROOT}/tinydng_codec.c
    ${TINYDNG_ROOT}/tinydng_write.c
    ${TINYDNG_ROOT}/tinydng_miniz.c
    ${TINYDNG_ROOT}/tinydng_stb_image.c
    ${TINYDNG_ROOT}/tiny_dng_ljpeg92_v2.c)

enable_testing()

foreach(t test_v3_roundtrip test_v3_decode test_v3_metadata test_v3_security)
  add_executable(${t} ${t}.c)
  target_link_libraries(${t} PRIVATE tinydng_v3)
endforeach()

add_test(NAME v3_roundtrip COMMAND test_v3_roundtrip)
add_test(NAME v3_decode COMMAND test_v3_decode ${TINYDNG_ROOT})
add_test(NAME v3_metadata COMMAND test_v3_metadata ${TINYDNG_ROOT})
add_test(NAME v3_security COMMAND test_v3_security)
