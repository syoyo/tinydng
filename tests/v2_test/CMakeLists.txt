cmake_minimum_required(VERSION 3.26)
project(tinyDngV2Test)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include_directories(../..)

add_library(tinydng_v2 STATIC
    ../../tiny_dng_v2.c
    ../../tiny_dng_ljpeg92_v2.c)

add_executable(test_v2_metadata test_v2_metadata.c)
target_link_libraries(test_v2_metadata tinydng_v2)

add_executable(test_v2_streaming test_v2_streaming.c)
target_link_libraries(test_v2_streaming tinydng_v2)

add_executable(test_v2_streamencode test_v2_streamencode.c)
target_link_libraries(test_v2_streamencode tinydng_v2)