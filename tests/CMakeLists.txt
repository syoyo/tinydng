cmake_minimum_required(VERSION 3.26)
project(tinyDngTest)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Deps build is broken, skipping
# include(loadDeps.cmake)
# include_directories(deps/tinydng)
# include_directories(deps)
# add_executable(tinyDngTest repackTest.cpp)

add_subdirectory(v2_test)
