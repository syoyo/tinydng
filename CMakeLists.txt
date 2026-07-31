cmake_minimum_required(VERSION 3.16)

set(EXE_TARGET "test_dng_loader")
set(PY_TARGET "tinydng_ext")
project(${EXE_TARGET} C CXX)

option(TINYDNG_WITH_PYTHON "Build Python module(For developer)." Off)
option(
    TINYDNG_PREFER_LOCAL_PYTHON_INSTALLATION
    "Prefer locally-installed Python interpreter than system or conda/brew installed Python. Please specify your Python interpreter with `Python3_EXECUTABLE` cmake option if you enable this option."
    OFF)


# cmake modules
list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake/sanitizers)
find_package(Sanitizers) # Address sanitizer (-DSANITIZE_ADDRESS=ON)

# Build standalone .so(for developer)
if (TINYDNG_WITH_PYTHON)

  if(TINYDNG_PREFER_LOCAL_PYTHON_INSTALLATION)
    #message(STATUS "Local Python")
    set(Python3_FIND_FRAMEWORK NEVER) # Do not search framework python
    set(Python3_FIND_STRATEGY LOCATION)
    set(Python3_FIND_REGISTRY NEVER) # Windows only
  else()
    set(Python3_FIND_FRAMEWORK LAST
    )# Prefer Brew/Conda to Apple framework python
  endif()

  find_package(
    Python3
    COMPONENTS Interpreter Development
    REQUIRED)

  find_package(pybind11 CONFIG)

endif()


set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(${EXE_TARGET}
  test_loader.cc
  )
add_sanitizers(${EXE_TARGET})

add_library(tinydng_v2 STATIC
  tiny_dng_v2.c
  tiny_dng_ljpeg92_v2.c)
target_include_directories(tinydng_v2 PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_features(tinydng_v2 PUBLIC c_std_11)

add_executable(test_dng_v2 test_v2.c)
target_link_libraries(test_dng_v2 PRIVATE tinydng_v2)

add_executable(test_v2_streaming tests/v2_test/test_v2_streaming.c)
target_link_libraries(test_v2_streaming PRIVATE tinydng_v2)
add_test(NAME v2_streaming COMMAND test_v2_streaming)
if(EXISTS "${CMAKE_SOURCE_DIR}/sandbox/lj92/testdata")
  add_test(NAME v2_streaming_tiles
           COMMAND test_v2_streaming "${CMAKE_SOURCE_DIR}/sandbox/lj92/testdata")
endif()

add_executable(test_v2_streamencode tests/v2_test/test_v2_streamencode.c)
target_link_libraries(test_v2_streamencode PRIVATE tinydng_v2)
add_test(NAME v2_streamencode COMMAND test_v2_streamencode)

# ----------------------------------------------------------------------
# tinydng v3 (clean-room pure C11 loader)
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

add_library(tinydng STATIC
  ${TINYDNG_V3_CORE_SRCS}
  ${TINYDNG_V3_VENDOR_SRCS}
  tiny_dng_ljpeg92_v2.c)
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

option(TINYDNG_WERROR "Treat warnings as errors for tinydng core sources" OFF)
if(NOT MSVC)
  set(TINYDNG_V3_FLAGS -Wall -Wextra -Wpedantic -Wshadow)
  if(TINYDNG_WERROR)
    list(APPEND TINYDNG_V3_FLAGS -Werror)
  endif()
  set_source_files_properties(${TINYDNG_V3_CORE_SRCS}
    PROPERTIES COMPILE_OPTIONS "${TINYDNG_V3_FLAGS}")
endif()

add_executable(test_dng_v3 test_v3.c)
target_link_libraries(test_dng_v3 PRIVATE tinydng)
add_sanitizers(test_dng_v3)

enable_testing()

# v3 unit tests (link the tinydng library; honor sanitizer options).
set(TINYDNG_V3_TESTS test_v3_roundtrip test_v3_decode test_v3_metadata
                     test_v3_mt test_v3_psd test_v3_streamwrite)
foreach(t ${TINYDNG_V3_TESTS})
  add_executable(${t} tests/v3_test/${t}.c)
  target_link_libraries(${t} PRIVATE tinydng)
  add_sanitizers(${t})
endforeach()

add_test(NAME v3_roundtrip COMMAND test_v3_roundtrip)
add_test(NAME v3_decode COMMAND test_v3_decode "${CMAKE_SOURCE_DIR}")
add_test(NAME v3_metadata COMMAND test_v3_metadata "${CMAKE_SOURCE_DIR}")
add_test(NAME v3_mt COMMAND test_v3_mt "${CMAKE_SOURCE_DIR}")
add_test(NAME v3_psd COMMAND test_v3_psd)
add_test(NAME v3_streamwrite COMMAND test_v3_streamwrite)

if (TINYDNG_WITH_PYTHON)
  # pybind11 method:
  pybind11_add_module(${PY_TARGET} python/python-bindings.cc)

  # repo top directory.
  target_include_directories(${PY_TARGET} PRIVATE ${CMAKE_SOURCE_DIR})

  # copy .so to python/tinydng
  add_custom_command(
    TARGET ${PY_TARGET}
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:${PY_TARGET}>"
            "${CMAKE_SOURCE_DIR}/python/tinydng/$<TARGET_FILE_NAME:${PY_TARGET}>"
    COMMENT "copying tinydng python native module file to python/tinydng/"
    VERBATIM)

endif()

# [VisualStudio]
if(WIN32)
  # Set ${EXE_TARGET} as a startup project for VS IDE
  set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT ${EXE_TARGET})

  # For easier debugging in VS IDE(cmake 3.8.0 or later required) Set working
  # directory where CMakeLists.txt is placed.
  if(CMAKE_VERSION VERSION_GREATER 3.8.0)
    set_target_properties(
      ${EXE_TARGET} PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY
                                           "${CMAKE_CURRENT_SOURCE_DIR}")
  endif()
endif()
