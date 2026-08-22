ROOT=../..
CC ?= cc
CFLAGS_BASE = -O3 -DNDEBUG -std=c11 -Wall -Wextra -I$(ROOT) -I$(ROOT)/attic/v2
LDFLAGS_BASE =

SRC = benchmark_decode.c $(ROOT)/tiny_dng_ljpeg92_v2.c $(ROOT)/attic/v2/tiny_dng_v2.c

all: bench-scalar bench-sse2 bench-avx2

bench-scalar: $(SRC)
	$(CC) $(CFLAGS_BASE) $(SRC) -o $@ $(LDFLAGS_BASE)

bench-sse2: $(SRC)
	$(CC) $(CFLAGS_BASE) -DTINY_DNG_LJPEG92_V2_USE_SSE2 -msse2 $(SRC) -o $@ $(LDFLAGS_BASE)

bench-avx2: $(SRC)
	$(CC) $(CFLAGS_BASE) -DTINY_DNG_LJPEG92_V2_USE_AVX2 -mavx2 $(SRC) -o $@ $(LDFLAGS_BASE)

clean:
	rm -f bench-scalar bench-sse2 bench-avx2

.PHONY: all clean
