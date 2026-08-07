// Cross-validate the clean-room C11 HTJ2K decoder against OpenJPH.
//
// Pipeline per block: OpenJPH encodes -> (OpenJPH decodes) | (our decoder
// decodes); outputs must be bit-identical.
//
// Build:  make -C benchmark/htj2k bench_ht_cross
// Run:    benchmark/htj2k/bench_ht_cross [iters]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "ojph_block_decoder.h"
#include "ojph_block_encoder.h"
#include "ojph_mem.h"

#include "tiny_dng_htj2k.h"

using namespace ojph::local;
using namespace ojph;

typedef uint32_t ui32;

static double now_s() {
  auto t = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(t.time_since_epoch()).count();
}

static void fill(ui32 *buf, ui32 w, ui32 h, ui32 stride, uint32_t K_max,
                 uint64_t seed) {
  const uint32_t shift = 31 - K_max;
  std::mt19937_64 rng(seed);
  std::uniform_real_distribution<double> u(0.0, 1.0);
  double lam = 0.35;
  for (ui32 y = 0; y < h; ++y)
    for (ui32 x = 0; x < w; ++x) {
      double uu = u(rng);
      double v = -std::log(1.0 - uu) / lam;
      if (rng() & 1) v = -v;
      if ((rng() & 3u) == 0) v = 0.0;
      long iv = std::lround(v);
      long mag = iv < 0 ? -iv : iv;
      long limit = (1L << (K_max - 1)) - 1;
      if (mag > limit) mag = limit;
      ui32 sign = iv < 0 ? 0x80000000u : 0u;
      buf[y * stride + x] = sign | (ui32)((ui64)mag << shift);
    }
}

int main(int argc, char **argv) {
  int iters = argc > 1 ? atoi(argv[1]) : 20;
  const uint32_t sizes[][2] = {{64, 64}, {32, 32}, {128, 128}, {16, 48}};
  const uint32_t K_list[] = {9, 12, 16, 18};
  const int num_blocks = 128;
  uint64_t total_fail = 0, total_blocks = 0;
  double t_mine = 0, t_ref = 0;

  for (auto &sz : sizes) {
    for (uint32_t K_max : K_list) {
      uint32_t w = sz[0], h = sz[1];
      uint32_t m = K_max - 1;
      size_t npx = (size_t)w * h;
      std::vector<ui32> coef(num_blocks * npx);
      std::vector<ui32> ref(num_blocks * npx);
      std::vector<ui32> mine(num_blocks * npx);
      std::vector<ui32> lengths(num_blocks * 3);
      std::vector<ui8> coded((size_t)num_blocks * (npx * 2 + 4096));

      for (int b = 0; b < num_blocks; ++b)
        fill(coef.data() + b * npx, w, h, w, K_max, 42 + b);

      ui8 *dst = coded.data();
      for (int b = 0; b < num_blocks; ++b) {
        mem_elastic_allocator ea(1 << 20);
        coded_lists *c = nullptr;
        ojph_encode_codeblock32(coef.data() + b * npx, m, 1, w, h, w,
                                lengths.data() + 3 * b, &ea, c);
        memcpy(dst, c->buf, lengths[3 * b]);
        dst += lengths[3 * b];
      }

      // reference decode (OpenJPH)
      double t0 = now_s();
      const ui8 *src = coded.data();
      for (int b = 0; b < num_blocks; ++b) {
        ui32 len = lengths[3 * b];
        if (!ojph_decode_codeblock32((ui8 *)src, ref.data() + b * npx, m, 1,
                                     len, 0, w, h, w, true)) {
          fprintf(stderr, "ojph decode failed block=%d %ux%u K=%u\n", b, w, h,
                  K_max);
          return 1;
        }
        src += len;
      }
      t_ref += now_s() - t0;

      // our decoder
      t0 = now_s();
      src = coded.data();
      for (int b = 0; b < num_blocks; ++b) {
        ui32 len = lengths[3 * b];
        int r = tdng_htj2k_decode_codeblock32(src, mine.data() + b * npx, m, 1,
                                              len, 0, w, h, w, 1);
        if (r != TDNG_HTJ2K_OK) {
          fprintf(stderr, "our decode failed block=%d %ux%u K=%u r=%d\n", b, w,
                  h, K_max, r);
          total_fail += 1000000;
          continue;
        }
        src += len;
      }
      t_mine += now_s() - t0;

      uint64_t fails = 0;
      for (int b = 0; b < num_blocks; ++b)
        for (size_t i = 0; i < npx; ++i)
          if (ref[b * npx + i] != mine[b * npx + i]) {
            if (fails < 5)
              fprintf(stderr, "  MISMATCH block=%d idx=%zu ref=%08x mine=%08x\n",
                      b, i, ref[b * npx + i], mine[b * npx + i]);
            fails++;
          }
      if (fails) {
        total_fail += fails;
        printf("FAIL %ux%u K=%u : %llu mismatches\n", w, h, K_max,
               (unsigned long long)fails);
      } else {
        printf("OK   %ux%u K=%u\n", w, h, K_max);
      }
      total_blocks += num_blocks;
    }
  }
  printf("total_fail=%llu  (ref_dec=%.3fs, ours=%.3fs)\n",
         (unsigned long long)total_fail, t_ref, t_mine);
  (void)iters;
  return total_fail ? 1 : 0;
}
