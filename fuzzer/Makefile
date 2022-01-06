all:
	clang++ -std=c++11 -fsanitize=address,fuzzer -g -O0  -o fuzzer_loader -I../ fuzz-loader.cc -pthread
	#clang++ -std=c++11 -fsanitize=address,fuzzer -g -O0  -o fuzzer_writer -I../ fuzz-writer.cc -pthread
