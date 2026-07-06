# libFuzzer harnesses. `make` builds the legacy loader fuzzer; the v3 targets
# below build the clean-room reader/writer/PSD fuzzers with ASan+UBSan+fuzzer.
CLANG ?= clang
CFLAGS ?= -std=c11 -g -O1 -fsanitize=address,undefined,fuzzer \
          -fno-sanitize-recover=all -I..

V3_SRCS = ../tinydng_api.c ../tinydng_io.c ../tinydng_tiff.c ../tinydng_dng.c \
          ../tinydng_codec.c ../tinydng_write.c ../tinydng_miniz.c \
          ../tinydng_stb_image.c ../tiny_dng_ljpeg92_v2.c
PSD_SRCS = ../tinydng_psd.c ../tinydng_psd_write.c

all:
	clang++ -std=c++11 -fsanitize=address,fuzzer -g -O0  -o fuzzer_loader -I../ fuzz-loader.cc -pthread
	#clang++ -std=c++11 -fsanitize=address,fuzzer -g -O0  -o fuzzer_writer -I../ fuzz-writer.cc -pthread

.PHONY: v3 v3-read v3-write v3-psd v3-psd-write psd-seeds clean-v3

v3: v3-read v3-write v3-psd v3-psd-write

v3-read:
	$(CLANG) $(CFLAGS) fuzz-v3.c $(V3_SRCS) $(PSD_SRCS) -lpthread -o fuzz-v3

v3-write:
	$(CLANG) $(CFLAGS) fuzz-v3-write.c $(V3_SRCS) $(PSD_SRCS) -lpthread -o fuzz-v3-write

v3-psd:
	$(CLANG) $(CFLAGS) fuzz-v3-psd.c $(V3_SRCS) $(PSD_SRCS) -lpthread -o fuzz-v3-psd

v3-psd-write:
	$(CLANG) $(CFLAGS) fuzz-v3-psd-write.c $(V3_SRCS) $(PSD_SRCS) -lpthread \
	    -o fuzz-v3-psd-write

# Seed corpus for the PSD fuzzers.
psd-seeds:
	$(CLANG) -std=c11 -I.. gen_v3_psd_corpus.c $(V3_SRCS) $(PSD_SRCS) \
	    -lpthread -o gen_psd
	mkdir -p seeds-psd && ./gen_psd seeds-psd

clean-v3:
	rm -f fuzz-v3 fuzz-v3-write fuzz-v3-psd fuzz-v3-psd-write gen_psd
