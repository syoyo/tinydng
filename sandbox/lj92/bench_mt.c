/* Multi-threaded tile-decode benchmark. The ProRAW image is 48 independent
 * 1008x1008 LJPEG tiles, so decoding scales across cores. Each worker pulls
 * tiles from a shared atomic counter (dynamic load balancing -- tiles differ in
 * entropy size), decodes into its own buffer, and accumulates a checksum; the
 * total checksum must match the single-threaded run for every thread count.
 * Reports MPix/s and speedup. The codec itself is unchanged -- this just drives
 * lj92_decode_* concurrently after a one-time lj92_init(). */
#if defined(__unix__) || defined(__APPLE__)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lj92.h"

#define MAX_TILES 256
#define MAX_THREADS 64

typedef struct { uint8_t* data; int len; int w, h, bits, comps; } tile;

static tile g_tiles[MAX_TILES];
static int g_ntiles;
static uint64_t g_pixels;       /* pixels per full pass (sum over tiles) */
static int g_maxw, g_maxh, g_maxc;
static long g_total;            /* total work items = ntiles * iters */
static atomic_long g_next;

typedef struct { uint16_t* out; uint64_t csum; } worker_arg;

static double now_ms(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}
static uint8_t* read_file(const char* p, int* len) {
  FILE* f = fopen(p, "rb"); if (!f) return NULL;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t* b = (uint8_t*)malloc((size_t)n);
  if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
  fclose(f); *len = (int)n; return b;
}

static void* worker(void* p) {
  worker_arg* a = (worker_arg*)p;
  long k;
  while ((k = atomic_fetch_add(&g_next, 1)) < g_total) {
    int i = (int)(k % g_ntiles);
    lj92_dec d = NULL;
    int w, h, b, c;
    if (lj92_decode_open(&d, g_tiles[i].data, g_tiles[i].len, &w, &h, &b, &c) != LJ92_OK) {
      fprintf(stderr, "open tile %d failed\n", i); exit(2);
    }
    if (lj92_decode_run(d, a->out, w * c, 0, NULL, 0) != LJ92_OK) {
      fprintf(stderr, "decode tile %d failed\n", i); exit(2);
    }
    lj92_decode_close(d);
    /* Strided FNV-ish hash over the whole tile: O(samples/256) so it doesn't
     * distort the throughput number, but spatially covers the buffer so any
     * interior cross-thread corruption (not just the endpoints) perturbs the
     * checksum. Order-independent across threads since each tile is decoded the
     * same number of times regardless of work distribution. */
    {
      size_t n = (size_t)w * h * c, j;
      uint64_t hsh = 1469598103934665603ull ^ (uint64_t)i;
      for (j = 0; j < n; j += 256) hsh = (hsh ^ a->out[j]) * 1099511628211ull;
      hsh = (hsh ^ a->out[n - 1u]) * 1099511628211ull;
      a->csum += hsh;
    }
  }
  return NULL;
}

/* Run `iters` full passes spread over `nthreads`; return elapsed ms, set csum. */
static double run(int nthreads, int iters, uint16_t** bufs, uint64_t* csum) {
  pthread_t th[MAX_THREADS];
  worker_arg args[MAX_THREADS];
  int t;
  double t0;
  g_total = (long)g_ntiles * iters;
  atomic_store(&g_next, 0);
  t0 = now_ms();
  for (t = 0; t < nthreads; t++) {
    args[t].out = bufs[t]; args[t].csum = 0;
    if (pthread_create(&th[t], NULL, worker, &args[t]) != 0) {
      fprintf(stderr, "pthread_create failed\n"); exit(3);
    }
  }
  *csum = 0;
  for (t = 0; t < nthreads; t++) { pthread_join(th[t], NULL); *csum += args[t].csum; }
  return now_ms() - t0;
}

int main(int argc, char** argv) {
  const char* dir = argc >= 2 ? argv[1] : "testdata";
  int maxtiles = argc >= 3 ? atoi(argv[2]) : 48;
  int iters = argc >= 4 ? atoi(argv[3]) : 10;
  int counts[] = {1, 2, 4, 8, 16, 32};
  uint16_t* bufs[MAX_THREADS];
  int t, loaded = 0;
  uint64_t ref_csum = 0;
  double base_mpix = 0;

  if (maxtiles > MAX_TILES) maxtiles = MAX_TILES;
  for (int i = 0; i < maxtiles; i++) {
    char path[512]; snprintf(path, sizeof(path), "%s/tile_%03d.lj92", dir, i);
    int len = 0; uint8_t* data = read_file(path, &len);
    if (!data) break;
    lj92_dec d = NULL; int w, h, b, c;
    if (lj92_decode_open(&d, data, len, &w, &h, &b, &c) != LJ92_OK) { free(data); break; }
    lj92_decode_close(d);
    g_tiles[loaded].data = data; g_tiles[loaded].len = len;
    g_tiles[loaded].w = w; g_tiles[loaded].h = h; g_tiles[loaded].comps = c;
    g_pixels += (uint64_t)w * h;
    if (w > g_maxw) g_maxw = w;
    if (h > g_maxh) g_maxh = h;
    if (c > g_maxc) g_maxc = c;
    loaded++;
  }
  if (!loaded) { fprintf(stderr, "no tiles in %s\n", dir); return 1; }
  g_ntiles = loaded;

  lj92_init(); /* one-time SIMD dispatch before threads (thread-safety contract) */

  /* one output buffer per worker thread */
  size_t bufcap = ((size_t)g_maxw * g_maxh * g_maxc + 1024u) * sizeof(uint16_t);
  for (t = 0; t < MAX_THREADS; t++) {
    bufs[t] = (uint16_t*)malloc(bufcap);
    if (!bufs[t]) { fprintf(stderr, "oom\n"); return 1; }
  }

  printf("tiles=%d per-tile=%dx%dx%d  pixels/pass=%.2fM  iters=%d  simd=%s\n",
         g_ntiles, g_maxw, g_maxh, g_maxc, (double)g_pixels / 1e6, iters,
         lj92_simd_name());
  /* warmup */
  { uint64_t c; run(1, 1, bufs, &c); }

  for (size_t ci = 0; ci < sizeof(counts) / sizeof(counts[0]); ci++) {
    int n = counts[ci];
    uint64_t csum;
    double ms = run(n, iters, bufs, &csum);
    double mpix = (double)g_pixels * iters / 1e6;
    double thr = ms > 0 ? mpix * 1000.0 / ms : 0;
    if (n == 1) { base_mpix = thr; ref_csum = csum; }
    printf("  %2d thread%s %9.1f MPix/s  speedup x%5.2f  %s\n", n,
           n == 1 ? " " : "s", thr, base_mpix > 0 ? thr / base_mpix : 1.0,
           csum == ref_csum ? "ok" : "CHECKSUM MISMATCH");
    if (csum != ref_csum) { fprintf(stderr, "  mismatch: %llu vs %llu\n",
        (unsigned long long)csum, (unsigned long long)ref_csum); }
  }

  for (t = 0; t < MAX_THREADS; t++) free(bufs[t]);
  for (t = 0; t < g_ntiles; t++) free(g_tiles[t].data);
  return 0;
}
