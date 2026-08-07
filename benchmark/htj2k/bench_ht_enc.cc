// Validate the clean-room HTJ2K encoder against OpenJPH.
//
// For each block: encode with ours -> (decode with ours) | (decode with
// OpenJPH); decoded integer coefficients must match the input. Also compare
// our encoded bytes against OpenJPH's encoder output (should be identical
// given the same codebook selection).
//
// Build:  make -C benchmark/htj2k bench_ht_enc
// Run:    benchmark/htj2k/bench_ht_enc [iters]
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
      long limit = (1L << (K_max - 2)) - 1;
      if (mag > limit) mag = limit;
      ui32 sign = iv < 0 ? 0x80000000u : 0u;
      buf[y * stride + x] = sign | (ui32)((ui64)mag << shift);
    }
}

int main(int argc, char **argv) {
  initialize_block_encoder_tables();
  int iters = argc > 1 ? atoi(argv[1]) : 10;
  const uint32_t sizes[][2] = {{64, 64}, {32, 32}, {64, 40}, {16, 16}};
  const uint32_t K_list[] = {9, 12, 16};
  const int num_blocks = 32;
  uint64_t total_fail = 0;
  double t_enc = 0, t_ojph = 0;

  for (auto &sz : sizes) {
    for (uint32_t K_max : K_list) {
      uint32_t w = sz[0], h = sz[1];
      uint32_t m = K_max - 1;
      size_t npx = (size_t)w * h;
      std::vector<ui32> coef(num_blocks * npx);
      std::vector<ui32> ours_dec(num_blocks * npx);
      std::vector<ui32> ref_dec(num_blocks * npx);
      std::vector<ui8> ours_buf(num_blocks * (npx * 2 + 4096));
      std::vector<ui8> ojph_buf(num_blocks * (npx * 2 + 4096));
      std::vector<ui32> our_len(num_blocks), ojph_len(num_blocks);

      for (int b = 0; b < num_blocks; ++b)
        fill(coef.data() + b * npx, w, h, w, K_max, 7 + b);

      // our encoder (out=NULL => function allocates)
      double t0 = std::chrono::duration<double>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();
      for (int b = 0; b < num_blocks; ++b) {
        uint8_t *op = NULL;
        size_t cap = 0;
        uint32_t len = 0;
        int r = tdng_htj2k_encode_codeblock32(coef.data() + b * npx, m, w, h,
                                              w, &len, &op, &cap);
        if (r != TDNG_HTJ2K_OK) {
          fprintf(stderr, "our encode failed b=%d %ux%u K=%u r=%d\n", b, w, h,
                  K_max, r);
          total_fail += 1000000;
        }
        our_len[b] = len;
        if (len > npx * 2 + 4096) { fprintf(stderr, "len too big\n"); }
        memcpy(ours_buf.data() + b * (npx * 2 + 4096), op, len);
        free(op);
      }
      t_enc += std::chrono::duration<double>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count() -
               t0;

      // OpenJPH encoder
      t0 = std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch())
               .count();
      for (int b = 0; b < num_blocks; ++b) {
        mem_elastic_allocator ea(1 << 20);
        coded_lists *c = nullptr;
        ui32 lens[3] = {0, 0, 0};
        ojph_encode_codeblock32(coef.data() + b * npx, m, 1, w, h, w, lens,
                                &ea, c);
        memcpy(ojph_buf.data() + b * (npx * 2 + 4096), c->buf, lens[0]);
        ojph_len[b] = lens[0];
      }
      t_ojph += std::chrono::duration<double>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count() -
                t0;

      // byte-exactness vs OpenJPH
      uint64_t byte_mism = 0;
      for (int b = 0; b < num_blocks; ++b) {
        if (our_len[b] != ojph_len[b] ||
            memcmp(ours_buf.data() + b * (npx * 2 + 4096),
                   ojph_buf.data() + b * (npx * 2 + 4096), our_len[b]) != 0) {
          byte_mism++;
          if (byte_mism <= 3)
            fprintf(stderr, "  byte mismatch b=%d our_len=%u ojph_len=%u\n", b,
                    our_len[b], ojph_len[b]);
        }
      }

      // decode ours with our decoder and with OpenJPH
      uint64_t dec_fail = 0;
      for (int b = 0; b < num_blocks; ++b) {
        const ui8 *cb = ours_buf.data() + b * (npx * 2 + 4096);
        int r = tdng_htj2k_decode_codeblock32(cb, ours_dec.data() + b * npx, m,
                                              1, our_len[b], 0, w, h, w, 0);
        bool rok = ojph_decode_codeblock32((ui8 *)cb, ref_dec.data() + b * npx,
                                           m, 1, our_len[b], 0, w, h, w, false);
        if (r != TDNG_HTJ2K_OK || !rok) {
          dec_fail++;
          if (dec_fail <= 3)
            fprintf(stderr, "  decode fail b=%d ours=%d ojph=%d\n", b, r, rok);
        }
        // compare decoded integers
        if (r == TDNG_HTJ2K_OK && rok) {
          const uint32_t p = 31 - K_max; /* block-domain magnitude shift */
          for (size_t i = 0; i < npx; ++i) {
            /* block domain is sign-magnitude, not two's complement */
            uint32_t v = coef[b * npx + i];
            int32_t expect =
                (v & 0x80000000u) ? -(int32_t)((v & 0x7FFFFFFFu) >> p)
                                  : (int32_t)((v & 0x7FFFFFFFu) >> p);
            uint32_t os = (ours_dec[b * npx + i] >> 31) & 1u;
            uint32_t om = (ours_dec[b * npx + i] & 0x7FFFFFFFu) >> p;
            int32_t got = os ? -(int32_t)om : (int32_t)om;
            uint32_t ros = (ref_dec[b * npx + i] >> 31) & 1u;
            uint32_t rom = (ref_dec[b * npx + i] & 0x7FFFFFFFu) >> p;
            int32_t gotr = ros ? -(int32_t)rom : (int32_t)rom;
            if (got != expect || gotr != expect) {
              dec_fail += 1000;
              if (dec_fail < 10000)
                fprintf(stderr, "  value K=%u b=%d i=%zu expect=%d ours=%d ojph=%d raw=%08x\n",
                        K_max, b, i, expect, got, gotr, ours_dec[b * npx + i]);
            }
          }
        }
      }

      printf("%ux%-3u K=%2u  byte_mism=%llu/%d  dec_fail=%llu\n", w, h, K_max,
             (unsigned long long)byte_mism, num_blocks,
             (unsigned long long)dec_fail);
      total_fail += byte_mism + dec_fail;
    }
  }
  printf("total_fail=%llu (our_enc=%.3fs, ojph_enc=%.3fs)\n",
         (unsigned long long)total_fail, t_enc, t_ojph);
  (void)iters;
  return total_fail ? 1 : 0;
}
