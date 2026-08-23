cmake_minimum_required(VERSION 3.16)

project(tinydng C)

option(TINYDNG_WERROR "Treat warnings as errors for tinydng core sources" OFF)

# cmake modules
list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake/sanitizers)
find_package(Sanitizers) # Address sanitizer (-DSANITIZE_ADDRESS=ON)

# ----------------------------------------------------------------------
# tinydng v3 (pure C11 DNG/TIFF/PSD loader and writer)
# ----------------------------------------------------------------------
set(TINYDNG_V3_CORE_SRCS
  tinydng_api.c
  tinydng_io.c
  tinydng_tiff.c
  tinydng_dng.c
  tinydng_codec.c
  tinydng_write.c
  tinydng_psd.c
  tinydng_psd_write.c)

set(TINYDNG_V3_VENDOR_SRCS
  tinydng_miniz.c
  tinydng_stb_image.c)

set(TINYDNG_V3_CODEC_SRCS tiny_dng_ljpeg92_v2.c)

add_library(tinydng STATIC
  ${TINYDNG_V3_CORE_SRCS}
  ${TINYDNG_V3_VENDOR_SRCS}
  ${TINYDNG_V3_CODEC_SRCS})
target_include_directories(tinydng PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_features(tinydng PUBLIC c_std_11)

# Multi-threaded segment decode uses pthreads (POSIX) / Win32 threads. Link the
# platform threading library; consumers inherit it via the PUBLIC dependency.
# Define TINYDNG_DISABLE_THREADS to build a fully serial, dependency-free lib.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads)
if(Threads_FOUND)
  target_link_libraries(tinydng PUBLIC Threads::Threads)
endif()

# Vendored third-party TUs: silence their warnings (not our code) and exclude
# them from the alignment sanitizer (miniz/stb use intentional, x86-safe
# unaligned fast-path loads that are correct but trip UBSan).
if(NOT MSVC)
  set_source_files_properties(${TINYDNG_V3_VENDOR_SRCS}
    PROPERTIES COMPILE_OPTIONS "-w;-fno-sanitize=alignment")
endif()

if(NOT MSVC)
  set(TINYDNG_V3_FLAGS -Wall -Wextra -Wpedantic -Wshadow)
  if(TINYDNG_WERROR)
    list(APPEND TINYDNG_V3_FLAGS -Werror)
  endif()
  set_source_files_properties(${TINYDNG_V3_CORE_SRCS} ${TINYDNG_V3_CODEC_SRCS}
    PROPERTIES COMPILE_OPTIONS "${TINYDNG_V3_FLAGS}")
endif()

# ----------------------------------------------------------------------
# Smoke test
# ----------------------------------------------------------------------
add_executable(test_dng_v3 test_v3.c)
target_link_libraries(test_dng_v3 PRIVATE tinydng)
add_sanitizers(test_dng_v3)

# ----------------------------------------------------------------------
# Tests
# ----------------------------------------------------------------------
enable_testing()

set(TINYDNG_V3_TESTS test_v3_roundtrip test_v3_decode test_v3_metadata
                     test_v3_security test_v3_mt test_v3_psd test_v3_streamwrite
                     test_lj92)
foreach(t ${TINYDNG_V3_TESTS})
  add_executable(${t} tests/v3_test/${t}.c)
  target_link_libraries(${t} PRIVATE tinydng)
  add_sanitizers(${t})
endforeach()

add_test(NAME v3_roundtrip COMMAND test_v3_roundtrip)
add_test(NAME v3_decode COMMAND test_v3_decode "${CMAKE_SOURCE_DIR}")
add_test(NAME v3_metadata COMMAND test_v3_metadata "${CMAKE_SOURCE_DIR}")
add_test(NAME v3_security COMMAND test_v3_security)
add_test(NAME v3_mt COMMAND test_v3_mt "${CMAKE_SOURCE_DIR}")
add_test(NAME v3_psd COMMAND test_v3_psd)
add_test(NAME v3_streamwrite COMMAND test_v3_streamwrite "${CMAKE_SOURCE_DIR}")
add_test(NAME lj92 COMMAND test_lj92)
