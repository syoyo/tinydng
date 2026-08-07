# HTJ2K benchmark: OpenJPH baseline vs tiny_dng clean-room codec

Standalone benchmarks measuring HTJ2K (JPEG 2000 Part 15) throughput on
this machine (AMD Ryzen Threadripper 1950X, Zen1, AVX2, no AVX-512).

## Build

```bash
make -C benchmark/htj2k all
```

Requires a built OpenJPH (v0.31) with native-arch flags:

```bash
cmake -B ~/work/OpenJPH/build -S ~/work/OpenJPH \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE="-O3 -march=znver1"
cmake --build ~/work/OpenJPH/build -j
```

## Targets

- `bench_ojph_block` : block-level codec microbench against libopenjph
  (MPix/s for encode + decode, generic and AVX2 variants). NB: OpenJPH's
  block API operates on sign-magnitude coefficients with the magnitude
  shifted left by `31-K_max` (`missing_msbs = K_max-1`); single coding pass.
- `bench_ojph_expand` : library-level decode of a real `.j2c` through
  `ojph::codestream` (no file I/O). The metric our decoder is measured against.
- `bench_tinydng_block` / `bench_tinydng_expand` : same workloads against the
  clean-room C11 codec (added once it lands).
- `run_bench.sh` : end-to-end `ojph_compress` / `ojph_expand` on the corpus
  (lossless reversible 5/3 and lossy irreversible 9/7, colour transform).

## Corpus (generated from existing assets, `data/`)

- `test48mp-gray.pgm` : 8064x6048 8-bit grayscale (from test.png)
- `pia23128-rgb.ppm`  : 5944x3342 8-bit RGB (from PIA23128.tif)
- `skin8k-rgb.ppm`    : 8192x8192 8-bit RGB (from skin_dif.tif)
- `shirts4k-rgb.ppm`  : 4096x4096 8-bit RGB (from shirts_dif.tif)

## Run

```bash
make -C benchmark/htj2k all
benchmark/htj2k/bench_ojph_expand benchmark/htj2k/out/test48mp-gray.lossless.j2c [iters]
benchmark/htj2k/run_bench.sh [OJPH_BIN_DIR]
```

## Phase 0 baseline (TR 1950X, single core, gcc 13.3, `-march=znver1`)

### End-to-end (`ojph_compress` / `ojph_expand`, includes file I/O)

| image        | mode    | enc MPix/s | dec MPix/s |
|--------------|---------|-----------:|-----------:|
| test48mp-gray| lossless|      113.5 |      127.7 |
| pia23128-rgb | lossless|       42.3 |       51.3 |
| skin8k-rgb   | lossless|       49.4 |       61.3 |
| shirts4k-rgb | lossless|       40.6 |       49.5 |
| test48mp-gray| lossy   |      205.9 |      177.3 |
| pia23128-rgb | lossy   |       84.7 |       82.3 |
| skin8k-rgb   | lossy   |       90.8 |       88.2 |
| shirts4k-rgb | lossy   |       88.8 |       84.0 |

### Library-level decode (`bench_ojph_expand`, codestream in memory)

| image        | mode    | MPix/s (pixels) | MPix/s (samples) |
|--------------|---------|----------------:|-----------------:|
| test48mp-gray| lossless|            209.0 |             209.0 |
| test48mp-gray| lossy   |            330.8 |             330.8 |
| pia23128-rgb | lossless|             69.5 |             208.6 |
| pia23128-rgb | lossy   |            141.0 |             422.9 |
| skin8k-rgb   | lossy   |            151.4 |             454.1 |

### OpenJPH hot spots (`perf record`, cycles)

Lossless gray decode:
- `decode_cb_step1_vlc` (VLC decode)          33.0%
- `decode_cb_step2_16bit` (MagSgn decode)     28.0%
- `avx2_rev_tx_from_cb32` + `avx2_rev_horz_syn` (inv. 5/3) ~13%

Lossy RGB decode (irreversible 9/7 float wavelet):
- `avx_irv_horz_syn` 27.6%, `avx_irv_vert_step` 17.3%,
  `avx2_irv_convert_to_integer` 13.4%

Lossless gray encode:
- `ojph_encode_codeblock_avx2` 28.1%, `proc_ms_encode` 18.3%,
  `proc_pixel` 15.3%  (HT block encoder ~62%)
- `avx2_rev_horz_ana` + `avx2_rev_tx_to_cb32` + `avx2_rev_vert_step`
  (fwd 5/3) ~13%

Optimization budget: HT block decode (VLC step1 + MagSgn step2) and the
5/3 lifting dominate lossless; the float 9/7 inverse wavelet dominates
lossy; the HT block encoder dominates encode.

## Current status (Phases 1-3)

Clean-room C11 HTJ2K codec (block codec + J2K codestream decoder) is built
and validated against OpenJPH:

- **Block decoder**: byte-exact vs OpenJPH on real codeblocks (48MP lossless,
  lossy 9/7, 16-bit, 29-bit). Scalar throughput ~0.97x of OpenJPH's AVX2
  decoder on TR 1950X (205 vs 211 MPix/s on the 8K RGB lossless blocks).
- **Block encoder**: byte-identical to OpenJPH across sizes/K (bench_ht_enc).
- **Codestream decoder** (tiny_dng_j2k.c): decodes lossless 5/3 and lossy 9/7
  gray/RGB codestreams byte-exact vs OpenJPH (bench_j2k_validate on 256x256
  through 8K RGB). AVX2 synthesis kernels active on Zen1.
- **tinydng integration**: DNG Compression=34712 (JPEG2000) tiles decode via
  `td_decode_block_j2k`; a generated J2K DNG round-trips exactly.

Remaining optimization gap vs OpenJPH is in the codestream/wavelet layer
(scalar, whole-buffer synthesis); the block codec is already at parity.

## Phase 4-5 status

- Block encoder now measurably faster than OpenJPH (1.06x on 64x64 K=12
  synthetic blocks: 47 vs 45 MPix/s); decoder ~0.97x scalar.
- Full-pipeline decode 16.4 -> ~20 MPix/s (0.26x -> 0.32x of OpenJPH) via
  AVX2 sign-magnitude->int/float conversions and a per-pixel interleaved pack
  with the DC level shift folded in. Remaining gap is the scalar whole-buffer
  5/3 synthesis; the block codec itself is at parity.
- AVX2 synthesis bug fixed (the 32-bit interleave used `_mm256_unpacklo_epi32`
  which is per-lane, not a full 8-element interleave; fixed with 128-bit
  unrolls). All files now decode byte-exact with AVX2 synthesis active.
- Robustness: 300+ truncated/flipped codestreams under ASan - no crashes, no
  infinite loops (corrupt streams near the end decode most of the image then
  fail gracefully).
- `ctest` includes `htj2k_self` (self-contained block+j2k test).

## Phase 4 optimization results (TR 1950X, single core)

- AVX2 sign-magnitude -> int/float conversions in `decode_blocks`
  (branchless `(m^s)-s`), plus a fixed float-negation (sign-bit flip).
- Per-pixel interleaved pack with the DC level shift folded in; a 3-way
  AVX2 interleave pack for the RGB case (`tdj_pack3_avx2`, permute/blend
  with cross-lane combine; the a-permute for the third 8-wide store needs
  dedicated indices because its selected lanes are {2,5} not {0,3,6}).
- Vertical 5/3 synthesis writes directly into the component recon buffer
  (`tdj_synth_53_vert_to`), eliminating the full-size tmp->recon copy at
  every resolution level.
- Full-pipeline decode improved 16.4 -> ~24 MPix/s (0.26x -> ~0.39x of
  OpenJPH) before system load noise. Remaining gap is the bit-serial HT
  block decode (~24% of pipeline) and page faults from the whole-buffer
  design.
- Note: `memcpy`/`memset` for the horizontal row copies made synthesis
  2x slower (libc call overhead) and was reverted to the scalar loop.

## J2K codestream encoder

- `tiny_dng_j2k_enc.c/h`: single-tile lossless HTJ2K encoder (RPCL, 5/3 +
  RCT) that is **byte-identical to OpenJPH's `ojph_compress`** on gray/RGB,
  square/non-square, and multi-codeblock images (COM comment differs by
  design). Verified by `cmp` (COM stripped) on t16, r16, g256, g512x256,
  c64 (RGB) and by cross-decode with both decoders.
- Key findings that had to match OpenJPH exactly:
  - Row-filter-then-column-filter analysis order (rounding is not
    commutative).
  - Packet-header 0xFF handling: next byte's MSB left 0 (7 usable bits), not
    a 0xFF00 stuffing byte.
  - Actual (unpadded) codeblock sizes; bounds-checked tag-tree min.
  - Psot excludes the 2-byte SOT marker; CAP Pcap=0x00020000.
- The lossy 9/7 encoder (expounded QCD, delta_inv quantization, ICT, qstep
  parameter) is **byte-identical to OpenJPH** across gray/RGB, odd dims,
  8/16-bit, and mct 0/1. (Earlier reports of divergence traced to a test
  harness PPM-header offset bug, not the encoder.)

## tinydng writer integration

- `tinydng_writer_create` with `opts.compression = TINYDNG_COMPRESSION_JPEG2000`
  writes DNG Compression=34712: each tile/strip becomes an independent lossless
  HTJ2K codestream (host-order samples, 1..8 comps, 1..31 bits). Verified by
  `v3_j2k_write` ctest (RGB write -> decode round trip) and 16-bit gray.

## Robustness fixes (edge cases)

- Block encoder `e_alloc` was one slot short for width==1 (the trailing
  `lep[1]` write went out of bounds) - fixed to `width/2 + 4`.
- 5/3 synthesis boundaries for odd widths: the last high's predict must use
  `low[n_high]` when `n_high < n_low`, and the update's `high[i]` must clamp
  to `high[n_high-1]`. The AVX2 synthesis kernels are only used when
  `n_low == n_high` (the odd-width AVX update reads past `high`).
- Vertical synthesis: an `out_h == 1` guard (single row: copy, no filtering)
  prevents out-of-bounds neighbor-row reads for tiny components.
- `synth_tmp` is sized `max(w,h)^2` (was `w^2`), fixing non-square images
  taller than wide.
- `j2k_edge` ctest covers 1x1, odd dims, 16-bit, non-standard codeblocks,
  nd=0 (within OpenJPH's inherent K_max range limit) and lossy reconstruction.
