#CC=/mnt/data/local/llvm-mingw-20200325-ubuntu-18.04/bin/x86_64-w64-mingw32-clang
#CXX=/mnt/data/local/llvm-mingw-20200325-ubuntu-18.04/bin/x86_64-w64-mingw32-clang

CC=clang
CXX=clang++

all:
	$(CC) -c miniz.c
	$(CXX) -std=c++11 -static -Weverything -Wno-c++98-compat -Wno-c++98-compat-pedantic -Werror -DTINY_DNG_LOADER_ENABLE_ZIP -o test -O0 -g test_loader.cc miniz.o -lstdc++ -pthread
