ROOT=../..
CC ?= cc
CFLAGS = -O2 -std=c11 -Wall -Wextra -D_POSIX_C_SOURCE=200809L -I$(ROOT)

SRC = bench.c \
      $(ROOT)/tinydng_api.c $(ROOT)/tinydng_io.c $(ROOT)/tinydng_tiff.c \
      $(ROOT)/tinydng_dng.c $(ROOT)/tinydng_codec.c $(ROOT)/tinydng_write.c \
      $(ROOT)/tinydng_psd.c $(ROOT)/tinydng_psd_write.c \
      $(ROOT)/tinydng_miniz.c $(ROOT)/tinydng_stb_image.c \
      $(ROOT)/tiny_dng_ljpeg92_v2.c

all: bench

bench: $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $@ $(LDFLAGS)

clean:
	rm -f bench

.PHONY: all clean
