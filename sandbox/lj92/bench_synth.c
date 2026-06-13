/* Synthetic decode benchmark to isolate where decode-side SIMD matters.
 * Predictor 1 (horizontal) reconstruction is a prefix sum and has SSE2/AVX2
 * kernels; predictor 7 is a 2D recurrence and stays scalar. We encode the same
 * realistic image with predictor 1 (mono) and predictor 7 (mono), then time
 * decoding each under every forced SIMD path. The gap between them shows how
 * much of decode is reconstruction (SIMD-able) vs serial Huffman (not). */
#if defined(__unix__) || defined(__APPLE__)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lj92.h"

static double now_ms(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}
static uint32_t rng = 0x2545f491u;
static uint32_t xr(void){ rng^=rng<<13; rng^=rng>>17; rng^=rng<<5; return rng; }

int main(int argc, char** argv) {
  int W = argc >= 2 ? atoi(argv[1]) : 2048;
  int H = argc >= 3 ? atoi(argv[2]) : 2048;
  int iters = argc >= 4 ? atoi(argv[3]) : 30;
  int bd = 14;

  /* Realistic-ish: smooth gradient + small noise so residuals are small. */
  uint16_t* img = (uint16_t*)malloc((size_t)W * H * sizeof(uint16_t));
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      int base = ((x + y) * 8) & ((1 << bd) - 1);
      int noise = (int)(xr() % 7) - 3;
      int v = base + noise; if (v < 0) v = 0; if (v >= (1 << bd)) v = (1 << bd) - 1;
      img[(size_t)y * W + x] = (uint16_t)v;
    }

  double mpix = (double)W * H * iters / 1e6;
  struct { lj92_simd s; const char* n; } paths[] = {
      {LJ92_SIMD_SCALAR, "scalar"}, {LJ92_SIMD_SSE2, "sse2"},
      {LJ92_SIMD_SSE41, "sse4.1"}, {LJ92_SIMD_AVX2, "avx2"}};

  int preds[] = {1, 7};
  for (int pi = 0; pi < 2; pi++) {
    int pred = preds[pi];
    uint8_t* enc = NULL; int el = 0;
    if (lj92_encode(img, W, H, bd, 1, pred, W, 0, NULL, 0, &enc, &el) != LJ92_OK) {
      fprintf(stderr, "encode failed\n"); return 1;
    }
    printf("\n== mono predictor %d, %dx%d bd%d, stream=%.1fMB ==\n", pred, W, H, bd,
           (double)el / 1e6);
    uint16_t* out = (uint16_t*)malloc((size_t)W * H * sizeof(uint16_t));
    double base = 0;
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
      if (lj92_simd_force(paths[i].s) != 0) { printf("  %-8s (unavailable)\n", paths[i].n); continue; }
      /* warmup */
      lj92_dec d; int w,h,b,c;
      lj92_decode_open(&d, enc, el, &w,&h,&b,&c); lj92_decode_run(d, out, w, 0, NULL, 0); lj92_decode_close(d);
      double t0 = now_ms();
      for (int it = 0; it < iters; it++) {
        lj92_decode_open(&d, enc, el, &w,&h,&b,&c);
        lj92_decode_run(d, out, w, 0, NULL, 0);
        lj92_decode_close(d);
      }
      double ms = now_ms() - t0;
      double thr = mpix * 1000.0 / ms;
      if (i == 0) base = thr;
      printf("  %-8s %8.2f MPix/s  (x%.3f)\n", paths[i].n, thr, thr / base);
    }
    free(out); free(enc);
  }
  free(img);
  return 0;
}
