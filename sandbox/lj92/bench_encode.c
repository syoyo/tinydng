/* Encode benchmark. Decodes the ProRAW tiles to recover realistic 3-component
 * 10-bit source images, then times re-encoding them (predictor 7, matching the
 * source) with the reference v2 encoder and the new lj92 encoder. Reports
 * MPix/s and the compressed size so the rate is comparable. */
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
#include "../../tiny_dng_ljpeg92_v2.h"

typedef struct { uint16_t* img; int w, h, bits, comps; } src_image;

static double now_ms(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}
static uint8_t* read_file(const char* path, int* len) {
  FILE* f = fopen(path, "rb"); if (!f) return NULL;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t* b = (uint8_t*)malloc((size_t)n);
  if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
  fclose(f); *len = (int)n; return b;
}

static uint64_t g_pixels;

static double bench_enc_v2(src_image* im, int n, int iters, uint64_t* bytes) {
  uint64_t tot = 0; double t0 = now_ms();
  for (int it = 0; it < iters; it++) {
    tot = 0;
    for (int t = 0; t < n; t++) {
      uint8_t* enc = NULL; int el = 0;
      if (tdng_lj92_encode_ex(im[t].img, im[t].w, im[t].h, im[t].bits, im[t].comps,
                              7, im[t].w * im[t].comps, 0, NULL, 0, &enc, &el) != TDNG_LJ92_ERROR_NONE) {
        fprintf(stderr, "v2 enc fail\n"); exit(2);
      }
      tot += (uint64_t)el; free(enc);
    }
  }
  *bytes = tot; return now_ms() - t0;
}
static double bench_enc_new(src_image* im, int n, int iters, uint64_t* bytes) {
  uint64_t tot = 0; double t0 = now_ms();
  for (int it = 0; it < iters; it++) {
    tot = 0;
    for (int t = 0; t < n; t++) {
      uint8_t* enc = NULL; int el = 0;
      if (lj92_encode(im[t].img, im[t].w, im[t].h, im[t].bits, im[t].comps, 7,
                      im[t].w * im[t].comps, 0, NULL, 0, &enc, &el) != LJ92_OK) {
        fprintf(stderr, "new enc fail\n"); exit(2);
      }
      tot += (uint64_t)el; free(enc);
    }
  }
  *bytes = tot; return now_ms() - t0;
}

int main(int argc, char** argv) {
  const char* dir = argc >= 2 ? argv[1] : "testdata";
  int ntiles = argc >= 3 ? atoi(argv[2]) : 48;
  int iters = argc >= 4 ? atoi(argv[3]) : 20;

  src_image* im = (src_image*)calloc((size_t)ntiles, sizeof(src_image));
  int loaded = 0;
  for (int t = 0; t < ntiles; t++) {
    char path[512]; snprintf(path, sizeof(path), "%s/tile_%03d.lj92", dir, t);
    int len = 0; uint8_t* data = read_file(path, &len);
    if (!data) break;
    lj92_dec d = NULL; int w, h, b, c;
    if (lj92_decode_open(&d, data, len, &w, &h, &b, &c) != LJ92_OK) { free(data); break; }
    im[t].img = (uint16_t*)malloc((size_t)w * h * c * sizeof(uint16_t));
    lj92_decode_run(d, im[t].img, w * c, 0, NULL, 0);
    lj92_decode_close(d); free(data);
    im[t].w = w; im[t].h = h; im[t].bits = b; im[t].comps = c;
    g_pixels += (uint64_t)w * h; loaded++;
  }
  if (!loaded) { fprintf(stderr, "no tiles\n"); return 1; }
  printf("tiles=%d per-tile=%dx%dx%d bits=%d pixels/iter=%.2fM iters=%d\n",
         loaded, im[0].w, im[0].h, im[0].comps, im[0].bits, (double)g_pixels / 1e6, iters);

  uint64_t bytes; double mpix = (double)g_pixels * iters / 1e6;
  /* warmup */ bench_enc_new(im, loaded, 1, &bytes);

  double v2ms = bench_enc_v2(im, loaded, iters, &bytes);
  double v2thr = mpix * 1000.0 / v2ms;
  printf("\n-- encode --\n");
  printf("  %-12s %8.2f MPix/s             per_iter=%.2fms  out=%.1fMB\n",
         "v2", v2thr, v2ms / iters, (double)bytes / 1e6);

  struct { lj92_simd s; const char* n; } paths[] = {
      {LJ92_SIMD_SCALAR, "new/scalar"}, {LJ92_SIMD_SSE2, "new/sse2"},
      {LJ92_SIMD_SSE41, "new/sse4.1"}, {LJ92_SIMD_AVX2, "new/avx2"}};
  for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
    if (lj92_simd_force(paths[i].s) != 0) { printf("  %-12s (unavailable)\n", paths[i].n); continue; }
    double ms = bench_enc_new(im, loaded, iters, &bytes);
    double thr = mpix * 1000.0 / ms;
    printf("  %-12s %8.2f MPix/s  (x%.3f)  per_iter=%.2fms  out=%.1fMB\n",
           paths[i].n, thr, thr / v2thr, ms / iters, (double)bytes / 1e6);
  }
  for (int t = 0; t < loaded; t++) free(im[t].img);
  free(im);
  return 0;
}
