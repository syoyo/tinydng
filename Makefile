CXX=clang++
CXXFLAGS=-Wall -DTINY_DNG_USE_WUFFS_IMAGE_LOADER=1  -DTINY_DNG_LOADER_USE_THREAD=1 -DTINY_DNG_LOADER_ENABLE_ZIP=1

all:
	$(CXX) -o test -std=c++11 -O0 -g $(CXXFLAGS) test_loader.cc miniz.c -pthread
