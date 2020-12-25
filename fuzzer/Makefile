all:
	clang++-11 -std=c++11 -fsanitize=address,fuzzer -g -O0  -o fuzzer -I../ fuzz.cc /usr/lib/llvm-11/lib/libFuzzer.a -pthread
