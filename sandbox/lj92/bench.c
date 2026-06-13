/* Standalone decode benchmark over extracted .lj92 tiles. Reports MPix/s for
 * the reference v2 decoder and for the new lj92 decoder under each forced SIMD
 * path. Mirrors the v2 benchmark methodology: open+decode each tile per
 * iteration (so header parse + Huffman build + entropy decode are all timed). */
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

typedef struct { uint8_t* data; int len; int w, h, bits, comps; } tile;

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static uint8_t* read_file(const char* path, int* len) {
  FILE* f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t* buf = (uint8_t*)malloc((size_t)n);
  if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
  fclose(f);
  *len = (int)n;
  return buf;
}

static uint64_t g_pixels;        /* pixels per iteration (sum over tiles) */
static int g_maxw, g_maxh, g_maxc;

/* ---- v2 reference ---- */
static double bench_v2(tile* tiles, int n, int iters, uint16_t* out, uint64_t* csum) {
  double t0 = now_ms();
  uint64_t cs = 0;
  for (int it = 0; it < iters; it++) {
    for (int t = 0; t < n; t++) {
      tdng_lj92 lj = NULL;
      int w, h, b, c;
      if (tdng_lj92_open(&lj, tiles[t].data, tiles[t].len, &w, &h, &b, &c) != TDNG_LJ92_ERROR_NONE) { fprintf(stderr, "v2 open fail\n"); exit(2); }
      if (tdng_lj92_decode(lj, out, w * c, 0, NULL, 0) != TDNG_LJ92_ERROR_NONE) { fprintf(stderr, "v2 dec fail\n"); exit(2); }
      tdng_lj92_close(lj);
      cs += out[0] + out[(size_t)w * h * c - 1];
    }
  }
  *csum = cs;
  return now_ms() - t0;
}

/* ---- new lj92 ---- */
static double bench_new(tile* tiles, int n, int iters, uint16_t* out, uint64_t* csum) {
  double t0 = now_ms();
  uint64_t cs = 0;
  for (int it = 0; it < iters; it++) {
    for (int t = 0; t < n; t++) {
      lj92_dec d = NULL;
      int w, h, b, c;
      if (lj92_decode_open(&d, tiles[t].data, tiles[t].len, &w, &h, &b, &c) != LJ92_OK) { fprintf(stderr, "new open fail\n"); exit(2); }
      if (lj92_decode_run(d, out, w * c, 0, NULL, 0) != LJ92_OK) { fprintf(stderr, "new dec fail\n"); exit(2); }
      lj92_decode_close(d);
      cs += out[0] + out[(size_t)w * h * c - 1];
    }
  }
  *csum = cs;
  return now_ms() - t0;
}

static void report(const char* label, double ms, int iters, uint64_t csum, double base_mpix) {
  double mpix = (double)g_pixels * iters / 1e6;
  double thr = ms > 0 ? mpix * 1000.0 / ms : 0;
  if (base_mpix > 0)
    printf("  %-14s %8.2f MPix/s  (x%.3f)   per_iter=%.2fms  csum=%llu\n",
           label, thr, thr / base_mpix, ms / iters, (unsigned long long)csum);
  else
    printf("  %-14s %8.2f MPix/s              per_iter=%.2fms  csum=%llu\n",
           label, thr, ms / iters, (unsigned long long)csum);
}

int main(int argc, char** argv) {
  const char* dir = argc >= 2 ? argv[1] : "testdata";
  int ntiles = argc >= 3 ? atoi(argv[2]) : 48;
  int iters = argc >= 4 ? atoi(argv[3]) : 20;

  tile* tiles = (tile*)calloc((size_t)ntiles, sizeof(tile));
  int loaded = 0;
  for (int t = 0; t < ntiles; t++) {
    char path[512];
    snprintf(path, sizeof(path), "%s/tile_%03d.lj92", dir, t);
    tiles[t].data = read_file(path, &tiles[t].len);
    if (!tiles[t].data) break;
    lj92_dec d = NULL;
    int w, h, b, c;
    if (lj92_decode_open(&d, tiles[t].data, tiles[t].len, &w, &h, &b, &c) != LJ92_OK) { fprintf(stderr, "open %s failed\n", path); break; }
    lj92_decode_close(d);
    tiles[t].w = w; tiles[t].h = h; tiles[t].bits = b; tiles[t].comps = c;
    g_pixels += (uint64_t)w * h;
    if (w > g_maxw) g_maxw = w;
    if (h > g_maxh) g_maxh = h;
    if (c > g_maxc) g_maxc = c;
    loaded++;
  }
  if (loaded == 0) { fprintf(stderr, "no tiles loaded from %s\n", dir); return 1; }

  uint16_t* out = (uint16_t*)malloc(((size_t)g_maxw * g_maxh * g_maxc + 1024) * sizeof(uint16_t));
  if (!out) { fprintf(stderr, "oom\n"); return 1; }

  printf("tiles=%d  per-tile=%dx%dx%d bits=%d  pixels/iter=%.2f M  iters=%d\n",
         loaded, g_maxw, g_maxh, g_maxc, tiles[0].bits,
         (double)g_pixels / 1e6, iters);

  uint64_t csum;
  /* warmup */
  bench_new(tiles, loaded, 1, out, &csum);

  /* Single-path mode for profiling: bench <dir> <ntiles> <iters> <path>. */
  if (argc >= 5) {
    const char* want = argv[4];
    if (strcmp(want, "v2") == 0) {
      double ms = bench_v2(tiles, loaded, iters, out, &csum);
      report("v2", ms, iters, csum, 0);
    } else {
      lj92_simd s = strcmp(want, "scalar") == 0 ? LJ92_SIMD_SCALAR
                  : strcmp(want, "sse2") == 0   ? LJ92_SIMD_SSE2
                  : strcmp(want, "sse4.1") == 0 ? LJ92_SIMD_SSE41
                                                : LJ92_SIMD_AVX2;
      if (lj92_simd_force(s) != 0) { fprintf(stderr, "path %s unavailable\n", want); return 1; }
      double ms = bench_new(tiles, loaded, iters, out, &csum);
      report(want, ms, iters, csum, 0);
    }
    for (int t = 0; t < loaded; t++) free(tiles[t].data);
    free(tiles); free(out);
    return 0;
  }

  double v2_ms = bench_v2(tiles, loaded, iters, out, &csum);
  double v2_mpix = (double)g_pixels * iters / 1e6 * 1000.0 / v2_ms;
  printf("\n-- reference --\n");
  report("v2", v2_ms, iters, csum, 0);

  printf("\n-- new lj92 (vs v2) --\n");
  struct { lj92_simd s; const char* name; } paths[] = {
      {LJ92_SIMD_SCALAR, "new/scalar"},
      {LJ92_SIMD_SSE2, "new/sse2"},
      {LJ92_SIMD_SSE41, "new/sse4.1"},
      {LJ92_SIMD_AVX2, "new/avx2"},
  };
  for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
    if (lj92_simd_force(paths[i].s) != 0) { printf("  %-14s (unavailable)\n", paths[i].name); continue; }
    double ms = bench_new(tiles, loaded, iters, out, &csum);
    report(paths[i].name, ms, iters, csum, v2_mpix);
  }

  for (int t = 0; t < loaded; t++) free(tiles[t].data);
  free(tiles);
  free(out);
  return 0;
}
