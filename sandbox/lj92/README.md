# lj92 — fast clean-room Lossless JPEG (ITU-T T.81 Annex H) codec

A fresh, pure-C11 implementation of a lossless JPEG ("LJPEG-1992") decoder and
encoder, written from scratch and tuned for throughput. It is API-independent
of the existing `tiny_dng_ljpeg92_v2` codec in the parent tree; that codec is
used here only as a correctness reference and a benchmark opponent.

Target workload: the LJPEG tiles inside Apple **ProRAW** DNGs
(`proraw-48mp-01.dng`): 8064×6048, **48 tiles** of 1008×1008, **3 interleaved
components**, **10-bit**, **predictor 7**, one Huffman table per component.

## Results (AMD Ryzen Threadripper 1950X, Zen 1, gcc 13.3, single thread, core-pinned)

Workload: all 48 ProRAW tiles per iteration (48.77 MPix), open+decode (or
encode) each tile every iteration. Figures are steady-state over 20–30 iters.

### Decode — ProRAW tiles (predictor 7, 3-comp, 10-bit)

| build       | MPix/s | vs v2 |
|-------------|-------:|------:|
| v2 (ref)    |   61.4 | 1.00× |
| new scalar  |   99.3 | 1.62× |
| new sse2    |  100.8 | 1.64× |
| new sse4.1  |  100.3 | 1.63× |
| new avx2    |   99.5 | 1.62× |

All SIMD paths match scalar (±2%, noise) — decode is entropy-bound (see below).

### Encode — same tiles (predictor 7)

| build       | MPix/s | vs v2 |
|-------------|-------:|------:|
| v2 (ref)    |   18.8 | 1.00× |
| new scalar  |   24.8 | 1.32× |
| new sse2    |   41.6 | 2.21× |
| new sse4.1  |   41.4 | 2.20× |
| new avx2    |   41.4 | 2.20× |

Output size is byte-for-byte the same as v2 (44.1 MB — identical compression).
The SSE2→AVX2 jump from ~28 to ~41 MPix/s came from vectorizing the SSSS
category computation in the frequency-scan/histogram pass (see below).

## The headline finding: LJPEG decode is entropy-bound, not SIMD-bound

The decode speedup is **entirely from a tighter scalar entropy decoder**, not
from SIMD. Lossless JPEG decoding is a single serial Huffman bitstream with a
per-sample data dependency through the bit buffer, so it cannot be vectorized.
SIMD can only help the *reconstruction* (turning residuals into pixels):

* **Predictor 7** (`Px=(left+above)>>1`, what ProRAW uses) is a 2-D recurrence —
  not vectorizable at all.
* **Predictor 1** (`Px=left`) reconstruction is a prefix sum, which *does* have
  SSE2/AVX2 kernels here — but `bench_synth` shows scalar and AVX2 within ~2%,
  because reconstruction is a tiny fraction of decode next to the serial Huffman
  work. In fact the two-pass "decode-then-SIMD-prefix-sum" layout (~242 MPix/s
  on the synthetic mono image) is *slower* than fusing decode+reconstruct
  (~296 MPix/s), because fusing lets the cheap reconstruction hide in the shadow
  of the Huffman LUT-load latency. So the default fuses pred-1 too; the two-pass
  prefix-sum path is kept only behind `-DLJ92_USE_PSUM_PRED1` for comparison.

Where the scalar decode wins came from (≈60 → ≈97 MPix/s):

1. **Branchless combined-shift symbol decode.** The Huffman LUT entry packs
   `(ssss<<8) | total`, where `total = code_length + ssss`. One shift of the bit
   accumulator consumes the Huffman code *and* its residual together; residual
   sign-extension is branchless and yields 0 for `ssss==0` with no special case.
2. **Accumulator bit reader with amortized refill.** A 64-bit left-justified
   accumulator with the branchless "or-in / advance / `|=56`" refill, invoked
   only when `<32` bits remain — one LUT load on the per-symbol critical path.
   (A windowed reader that reloads each symbol was tried and is ~30% slower: it
   puts two dependent loads on the critical path.)
3. **Unified peek width.** All component Huffman LUTs are expanded to one common
   index width so the per-symbol peek is a single shift and one fewer live
   register, easing pressure in the 3-component interleaved loop.
4. **Fused decode + predict + store**, specialized at compile time per
   `(predictor, components, linearize)` via an `always_inline` template, with
   `restrict` on the bit-reader so output stores don't force reloads of the bit
   state.

## Where SIMD does pay off: the encoder

The encoder's predictor reads *original* neighbor pixels, so the per-row
residual computation has **no recurrence** and vectorizes cleanly. The
`enc_diff_row` kernel has SSE2/AVX2 versions (predictors 1/2/3/4/7; pred 7 uses
`_mm_avg_epu16` minus a parity correction for an exact floor average). That plus
a no-copy direct-from-image path and a 64-bit bit-packer that emits each
sample's Huffman code+residual in a single push took encode from ~19 (v2) to
~25 (scalar) to ~29 MPix/s (SSE2/AVX2); vectorizing the SSSS computation (next
section) then took the SIMD paths to ~41 MPix/s.

### Profiling and fixing the frequency-scan (histogram) pass

`perf annotate` on `lj92_encode` showed the encoder's *biggest* single cost was
not the histogram scatter at all — it was the scalar `clz`-based **SSSS-category
computation** (`enc_ssss`, ~22% of encode, run once per sample in *both*
passes). The `hist[c][ssss]++` increment itself was only ~6%.

`bench_hist.c` isolates the pass over realistic ProRAW residuals and compares
strategies (Msamples/s on Zen1):

| strategy | Msamp/s | vs naive |
|----------|--------:|---------:|
| scalar `clz` + single histogram (original) | 214 | 1.00× |
| scalar `clz` + 4 privatized histograms     | 188 | 0.88× |
| **SIMD ssss + single histogram**           | **1024** | **4.78×** |
| SIMD ssss + 4 privatized histograms        | 887 | 4.14× |

Two lessons: (1) histogram **privatization made it slower** — the scatter into a
1 KB L1-resident table was never the bottleneck, so extra copies just add memory
traffic and a merge; (2) the win is **vectorizing the bit-length**: `ssss_row`
computes `|residual|` → float32 (exact for ≤16-bit) and reads the exponent
field (`ssss = exp − 126`, clamped to 0), 8 lanes at a time with SSE2-only ops
(emulated abs + `andnot` clamp), so one kernel serves all SIMD tiers. It's used
in both passes and produces bit-identical SSSS to the scalar `clz`, so encoder
output is unchanged. This lifted full encode another step (see table above).

## SIMD selection

Compile-time gating (`LJ92_HAVE_{SSE2,SSE41,AVX2}`, auto on x86 GCC/Clang;
`-DLJ92_DISABLE_SIMD` forces scalar) plus **runtime CPUID dispatch**: the SIMD
kernels carry `__attribute__((target(...)))` so a single default-built binary
contains them and picks the best at runtime via `__builtin_cpu_supports`.
`lj92_simd_force()` pins a path for benchmarking; `lj92_simd_name()` reports the
active one. (SSE4.1 shares the SSE2 kernels here — the hot kernels need no
`pshufb`/blend; the tier is distinguished only so it can be targeted.)

## Files

| file | purpose |
|------|---------|
| `lj92.h` / `lj92.c` | the codec (decoder + encoder, ~1.4k LOC) |
| `extract_streams.c` | one-time: dump LJPEG tiles from a DNG into `testdata/` |
| `test_correct.c` | decode every tile, compare byte-for-byte vs v2, all SIMD paths |
| `test_encode.c` | round-trip + interop vs v2 + real-data re-encode, all SIMD paths |
| `bench.c` | decode benchmark (new vs v2, per SIMD path) |
| `bench_encode.c` | encode benchmark (new vs v2, per SIMD path) |
| `bench_synth.c` | synthetic decode benchmark isolating predictor-1 SIMD |
| `bench_hist.c` | isolates the encoder frequency-scan (histogram) pass; compares scalar/privatized/SIMD-ssss strategies |

## Build & run

```bash
make                 # builds everything (needs ../../tiny_dng_v2.* for the drivers)
make testdata        # extracts tiles from ../../proraw-48mp-01.dng
make check           # correctness: test_correct + test_encode
make run-bench       # decode throughput
./bench_encode testdata 48 20
./bench_synth 2048 2048 40
```

## Correctness

* Decoder output is **byte-identical to the v2 reference** on all 48 ProRAW
  tiles, under every SIMD path (192 checks).
* Encoder: lossless round-trip and cross-decode with v2 both directions, plus
  re-encoding real decoded tiles — 21603 checks across bitdepths 8–16,
  1–4 components, predictors 1/2/4/5/6/7, and all SIMD paths.
* Clean under `-fsanitize=address,undefined`.
