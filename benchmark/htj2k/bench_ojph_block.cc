// Block-level HTJ2K microbenchmark against the OpenJPH library.
//
// Measures raw codeblock throughput (MPix/s) for decode and encode, generic
// vs AVX2 variants, on synthetic wavelet-like coefficient blocks. This is
// the primary "codec core" number our clean-room implementation must beat.
//
// OpenJPH 0.31's block encoder emits a single coding pass (cleanup pass),
// so encode+decode here is single-pass; full multi-pass data comes from real
// codestreams in the end-to-end benchmark.
//
// Build:  make -C benchmark/htj2k
// Run:    benchmark/htj2k/bench_ojph_block [iterations]
//
// Links against ~/work/OpenJPH/build/src/core/libopenjph.so
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "ojph_block_decoder.h"
#include "ojph_block_encoder.h"
#include "ojph_mem.h"

using namespace ojph::local;
using namespace ojph;

typedef uint32_t ui32;
typedef uint64_t ui64;
typedef uint8_t ui8;

static double now_s() {
  auto t = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(t.time_since_epoch()).count();
}

// Laplacian-like wavelet coefficient distribution: mostly small/near zero,
// a few large significant samples. Mirrors real 5/3 subband data.
static void fill_blocks(ui32 *buf, ui32 w, ui32 h, ui32 stride, uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::uniform_real_distribution<double> u(0.0, 1.0);
  double lam = 0.35;
  for (ui32 y = 0; y < h; ++y)
    for (ui32 x = 0; x < w; ++x) {
      double uu = u(rng);
      double v = -std::log(1.0 - uu) / lam;
      if (rng() & 1) v = -v;
      if ((rng() & 3u) == 0) v = 0.0;
      buf[y * stride + x] = (ui32)((int64_t)std::lround(v));
    }
}

int main(int argc, char **argv) {
  int iters = argc > 1 ? atoi(argv[1]) : 20;
  const ui32 sizes[][2] = {{64, 64}, {128, 128}, {32, 32}};
  const ui32 mbps_list[] = {3, 6, 10};
  const ui32 num_blocks = 256;
  const ui32 num_passes = 1;

  for (auto &sz : sizes) {
    ui32 w = sz[0], h = sz[1];
    ui32 stride = w;
    size_t npx = (size_t)w * h;

    std::vector<ui32> coef(num_blocks * npx);
    std::vector<ui32> dec(num_blocks * npx);
    std::vector<ui32> lengths(num_blocks * 3);
    std::vector<ui8> coded((size_t)num_blocks * (npx * 2 + 1024));

    for (ui32 b = 0; b < num_blocks; ++b)
      fill_blocks(coef.data() + (size_t)b * npx, w, h, stride, 1234 + b);

    for (ui32 m : mbps_list) {
      // ---- encode ----
      double t0 = now_s();
      ui8 *dst = coded.data();
      ui32 total_len = 0;
      for (ui32 b = 0; b < num_blocks; ++b) {
        ui32 *buf = coef.data() + (size_t)b * npx;
        mem_elastic_allocator ea(1 << 20);
        coded_lists *c = nullptr;
        ojph_encode_codeblock32(buf, m, num_passes, w, h, stride,
                                lengths.data() + 3 * b, &ea, c);
        ui32 len = lengths[3 * b];
        memcpy(dst, c->buf, len);
        dst += len;
        total_len += len;
      }
      double t1 = now_s();

      // ---- decode ----
      double t2 = now_s();
      ui64 sum = 0;
      for (int it = 0; it < iters; ++it) {
        const ui8 *src = coded.data();
        for (ui32 b = 0; b < num_blocks; ++b) {
          ui32 len = lengths[3 * b];
          ui32 *out = dec.data() + (size_t)b * npx;
          if (!ojph_decode_codeblock32((ui8 *)src, out, m, num_passes, len, 0,
                                       w, h, stride, true)) {
            fprintf(stderr, "decode failed block=%u\n", b);
            return 1;
          }
          sum += out[0] + out[npx - 1];
          src += len;
        }
      }
      double t3 = now_s();

      double enc_mpix = (double)(npx * num_blocks) / (t1 - t0) / 1e6;
      double dec_mpix = (double)(npx * num_blocks) * iters / (t3 - t2) / 1e6;
      double enc_mb = (double)total_len / (t1 - t0) / 1e6;
      double dec_mb = (double)total_len * iters / (t3 - t2) / 1e6;

      printf("block=%ux%-3u mbps=%u  enc=%9.1f MPix/s (%7.1f MB/s)  dec=%9.1f MPix/s (%7.1f MB/s)  bytes/blk=%u\n",
             w, h, m, enc_mpix, enc_mb, dec_mpix, dec_mb,
             (ui32)(total_len / num_blocks));
      (void)sum;
    }
  }

  // ---- AVX2 decode / encode variants (64x64, mbps=6) ----
  {
    ui32 w = 64, h = 64, m = 6, stride = 64;
    size_t npx = w * h;
    std::vector<ui32> coef(num_blocks * npx);
    std::vector<ui32> dec(num_blocks * npx);
    std::vector<ui32> lengths(num_blocks * 3);
    std::vector<ui8> coded((size_t)num_blocks * (npx * 2 + 1024));
    for (ui32 b = 0; b < num_blocks; ++b)
      fill_blocks(coef.data() + (size_t)b * npx, w, h, stride, 999 + b);

    double t0 = now_s();
    ui8 *dst = coded.data();
    ui32 total_len = 0;
    for (ui32 b = 0; b < num_blocks; ++b) {
      mem_elastic_allocator ea(1 << 20);
      coded_lists *c = nullptr;
      ojph_encode_codeblock_avx2(coef.data() + (size_t)b * npx, m, 1, w, h,
                                 stride, lengths.data() + 3 * b, &ea, c);
      ui32 len = lengths[3 * b];
      memcpy(dst, c->buf, len);
      dst += len;
      total_len += len;
    }
    double t1 = now_s();
    double t2 = now_s();
    ui64 sum = 0;
    for (int it = 0; it < iters; ++it) {
      const ui8 *src = coded.data();
      for (ui32 b = 0; b < num_blocks; ++b) {
        ui32 len = lengths[3 * b];
        if (!ojph_decode_codeblock_avx2((ui8 *)src, dec.data() + (size_t)b * npx,
                                        m, 1, len, 0, w, h, stride, true)) {
          fprintf(stderr, "avx2 decode failed\n");
          return 1;
        }
        sum += dec[0] + dec[npx - 1];
        src += len;
      }
    }
    double t3 = now_s();
    printf("AVX2 64x64 mbps=6   enc=%9.1f MPix/s (%7.1f MB/s)  dec=%9.1f MPix/s (%7.1f MB/s)  bytes/blk=%u\n",
           (double)(npx * num_blocks) / (t1 - t0) / 1e6,
           (double)total_len / (t1 - t0) / 1e6,
           (double)(npx * num_blocks) * iters / (t3 - t2) / 1e6,
           (double)total_len * iters / (t3 - t2) / 1e6,
           (ui32)(total_len / num_blocks));
    (void)sum;
  }
  return 0;
}
