all:
	clang++ -g main.cc

fuzz:
	clang++ -fsanitize=address, fuzzer -g -O2 main.cc
