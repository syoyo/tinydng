# Standalone lj92 codec — build, test, benchmark.
#
# The library (lj92.c) is portable C11. On x86 with GCC/Clang the SSE2/SSE4.1/
# AVX2 kernels are compiled in unconditionally (the hot functions carry
# __attribute__((target(...)))) and selected at runtime via CPUID. Pass
# LJ92_DISABLE_SIMD=1 to build a scalar-only library.
#
# The test/benchmark drivers link the reference v2 decoder/encoder from the
# parent tinydng tree for cross-checking and head-to-head numbers.

ROOT   = ../..
CC    ?= cc
CSTD   = -std=c11
WARN   = -Wall -Wextra
OPT    = -O3 -DNDEBUG
INC    = -I$(ROOT) -I.

ifdef LJ92_DISABLE_SIMD
OPT += -DLJ92_DISABLE_SIMD
endif

LJ92_SRC = lj92.c
REF_SRC  = $(ROOT)/tiny_dng_ljpeg92_v2.c
DNG_SRC  = $(ROOT)/tiny_dng_v2.c

BINS = bench bench_mt test_correct test_encode extract_streams

all: $(BINS)

bench: bench.c $(LJ92_SRC) $(REF_SRC)
	$(CC) $(OPT) $(CSTD) $(INC) $^ -o $@

# Multi-threaded tile-decode scaling benchmark (codec only; no v2 reference).
bench_mt: bench_mt.c $(LJ92_SRC)
	$(CC) $(OPT) $(CSTD) $(WARN) $(INC) -pthread $^ -o $@

test_correct: test_correct.c $(LJ92_SRC) $(REF_SRC)
	$(CC) $(OPT) $(CSTD) $(WARN) $(INC) $^ -o $@

test_encode: test_encode.c $(LJ92_SRC) $(REF_SRC)
	$(CC) $(OPT) $(CSTD) $(WARN) $(INC) $^ -o $@

# One-time: extract LJPEG tile streams from a DNG into testdata/.
extract_streams: extract_streams.c $(REF_SRC) $(DNG_SRC)
	$(CC) $(OPT) $(CSTD) $(INC) $^ -o $@

# ASAN/UBSAN build of the correctness test.
test_correct_asan: test_correct.c $(LJ92_SRC) $(REF_SRC)
	$(CC) -O1 -g $(CSTD) $(WARN) -fsanitize=address,undefined $(INC) $^ -o $@

testdata: extract_streams
	@mkdir -p testdata
	./extract_streams $(ROOT)/proraw-48mp-01.dng testdata

check: test_correct test_encode
	./test_correct testdata 48
	./test_encode

run-bench: bench
	./bench testdata 48 20

run-bench-mt: bench_mt
	./bench_mt testdata 48 20

clean:
	rm -f $(BINS) test_correct_asan

.PHONY: all check run-bench run-bench-mt clean testdata
