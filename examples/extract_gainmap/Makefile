CC=clang
CXX=clang++

all:
	$(CC) -c ../../miniz.c
	$(CXX) -I../../ -std=c++03 -static -Weverything -Wno-c++98-compat -Werror -DTINY_DNG_LOADER_ENABLE_ZIP -o extract_gainmap -O0 -g extract_gainmap.cc miniz.o -lstdc++ -pthread
