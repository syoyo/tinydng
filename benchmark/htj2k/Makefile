# OpenJPH baseline / tiny_dng HTJ2K benchmarks
#
# Targets:
#   bench_ojph_block    block-level codec microbench against OpenJPH lib
#   bench_tinydng_block block-level codec microbench against our codec
#
# If OJPH_ROOT is set, bench_ojph_block links against that OpenJPH build.

CXX      ?= c++
CC       ?= cc
CXXFLAGS ?= -O3 -std=c++14
CFLAGS   ?= -O3 -std=c11
WARN      = -Wall -Wextra
OJPH_ROOT ?= $(HOME)/work/OpenJPH/build

OJPH_INC   = -I$(HOME)/work/OpenJPH/src/core -I$(HOME)/work/OpenJPH/src/core/coding -I$(HOME)/work/OpenJPH/src/core/openjph
OJPH_LIB   = $(OJPH_ROOT)/src/core/libopenjph.so
LDLIBS     = -L$(OJPH_ROOT)/src/core -lopenjph

all: bench_ojph_block bench_ojph_expand bench_tinydng_block bench_tinydng_expand bench_ht_cross bench_ht_enc bench_ht_validate bench_ht_validate64

bench_ojph_block: bench_ojph_block.cc
	$(CXX) $(CXXFLAGS) $(WARN) $(OJPH_INC) $< -o $@ $(LDLIBS) -Wl,-rpath,$(OJPH_ROOT)/src/core

bench_ojph_expand: bench_ojph_expand.cc
	$(CXX) $(CXXFLAGS) $(WARN) $(OJPH_INC) $< -o $@ $(LDLIBS) -Wl,-rpath,$(OJPH_ROOT)/src/core

bench_tinydng_expand: bench_tinydng_expand.cc ../../tiny_dng_htj2k.c ../../tiny_dng_j2k.c
	$(CXX) $(CXXFLAGS) $(WARN) -I../.. $< ../../tiny_dng_htj2k.c ../../tiny_dng_j2k.c -o $@ -lm

bench_tinydng_block: bench_tinydng_block.cc ../../tiny_dng_htj2k.c
	$(CXX) $(CXXFLAGS) $(WARN) -I../.. $< ../../tiny_dng_htj2k.c -o $@ -lm

bench_ht_cross: bench_ht_cross.cc ../../tiny_dng_htj2k.c
	$(CXX) $(CXXFLAGS) $(WARN) $(OJPH_INC) -I../.. $< ../../tiny_dng_htj2k.c -o $@ $(LDLIBS) -Wl,-rpath,$(OJPH_ROOT)/src/core -lm

# OJPH_PATCH points at a patched OpenJPH build that dumps codeblocks
OJPH_PATCH_INC = -I/tmp/opencode/ojph-full/src/core -I/tmp/opencode/ojph-full/src/core/coding -I/tmp/opencode/ojph-full/src/core/openjph
OJPH_PATCH_LIB = /tmp/opencode/ojph-full/build/src/core/libopenjph.so

bench_ht_validate: bench_ht_validate.cc ../../tiny_dng_htj2k.c
	$(CXX) $(CXXFLAGS) $(WARN) $(OJPH_PATCH_INC) -I../.. $< ../../tiny_dng_htj2k.c -o $@ -L/tmp/opencode/ojph-full/build/src/core -lopenjph -Wl,-rpath,/tmp/opencode/ojph-full/build/src/core -lm

bench_ht_enc: bench_ht_enc.cc ../../tiny_dng_htj2k.c
	$(CXX) $(CXXFLAGS) $(WARN) $(OJPH_INC) -I../.. $< ../../tiny_dng_htj2k.c -o $@ $(LDLIBS) -Wl,-rpath,$(OJPH_ROOT)/src/core -lm

bench_ht_validate64: bench_ht_validate64.cc ../../tiny_dng_htj2k.c
	$(CXX) $(CXXFLAGS) $(WARN) $(OJPH_PATCH_INC) -I../.. $< ../../tiny_dng_htj2k.c -o $@ -L/tmp/opencode/ojph-full/build/src/core -lopenjph -Wl,-rpath,/tmp/opencode/ojph-full/build/src/core -lm

clean:
	rm -f bench_ojph_block bench_tinydng_block bench_ojph_expand bench_tinydng_expand bench_ht_cross bench_ht_enc bench_ht_validate bench_ht_validate64

.PHONY: all clean
