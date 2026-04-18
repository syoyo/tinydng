# ljpeg92 v2 decode benchmark

Standalone decoder benchmark for `tiny_dng_ljpeg92_v2`.

## Build

```bash
make -C benchmark/ljpeg92-v2 clean all
```

This produces:

- `bench-scalar` : scalar fast path
- `bench-sse2` : scalar + SSE2 prefix reconstruction (`-DTINY_DNG_LJPEG92_V2_USE_SSE2`)
- `bench-avx2` : scalar + AVX2 prefix reconstruction (`-DTINY_DNG_LJPEG92_V2_USE_AVX2`)

## Run

```bash
benchmark/ljpeg92-v2/bench-scalar <dng_or_raw_ljpeg_file> [iterations]
benchmark/ljpeg92-v2/bench-sse2   <dng_or_raw_ljpeg_file> [iterations]
benchmark/ljpeg92-v2/bench-avx2   <dng_or_raw_ljpeg_file> [iterations]
benchmark/ljpeg92-v2/run_compare.sh <dng_or_raw_ljpeg_file> [iterations]
```

The input can be:

- DNG containing a lossless JPEG stream (Compression 6/7), or
- raw lossless JPEG bitstream.

The benchmark reports elapsed time, per-iteration time, MPix/s throughput, and a checksum.

If the input stream is not lossless JPEG (1992) supported by `tdng_lj92_open`,
the benchmark exits with an explicit error.
