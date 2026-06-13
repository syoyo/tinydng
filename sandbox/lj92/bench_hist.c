/* Isolated micro-benchmark for the encoder frequency-scan (histogram) pass.
 * Decodes the ProRAW tiles to realistic 3-comp 10-bit images, computes the
 * predictor-7 residuals once, then times the per-row "ssss + per-component
 * histogram" inner loop under several strategies:
 *   A. naive   : scalar clz ssss + single histogram (what lj92.c does today)
 *   B. priv    : scalar ssss + K privatized histograms (breaks the RMW chain)
 *   C. simd    : SSE4.1-vectorized ssss + K privatized histograms
 * All produce identical histograms (verified). Reports Msamples/s. */
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
#include <immintrin.h>

#include "lj92.h"

#define NSYM 17

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
static inline int ssss_scalar(int diff) {
  int a = diff < 0 ? -diff : diff;
  return a == 0 ? 0 : 32 - __builtin_clz((unsigned)a);
}

/* ---- A: naive (current) ---- accumulate straight into hist */
static void hist_naive(const int16_t* diff, int W, int NC, int hist[][NSYM]) {
  for (int col = 0; col < W; col++)
    for (int c = 0; c < NC; c++)
      hist[c][ssss_scalar(diff[col * NC + c])]++;
}

/* ---- B: scalar ssss into 4 privatized copies (caller owns/zeros/sums h4) ---- */
static void hist_priv(const int16_t* diff, int W, int NC, int h4[4][16][NSYM]) {
  int col = 0;
  for (; col + 4 <= W; col += 4)
    for (int k = 0; k < 4; k++)
      for (int c = 0; c < NC; c++)
        h4[k][c][ssss_scalar(diff[(col + k) * NC + c])]++;
  for (; col < W; col++)
    for (int c = 0; c < NC; c++)
      h4[0][c][ssss_scalar(diff[col * NC + c])]++;
}

/* SSE4.1 vectorized ssss for 8 int16 residuals -> 8 bytes of category. */
__attribute__((target("sse4.1"))) static inline void ssss8(const int16_t* d, uint8_t* out) {
  __m128i v = _mm_loadu_si128((const __m128i*)d);
  __m128i a = _mm_abs_epi16(v);                       /* |diff| */
  __m128i z = _mm_setzero_si128();
  __m128i lo = _mm_unpacklo_epi16(a, z);              /* 4x u32 */
  __m128i hi = _mm_unpackhi_epi16(a, z);
  __m128i elo = _mm_srli_epi32(_mm_castps_si128(_mm_cvtepi32_ps(lo)), 23);
  __m128i ehi = _mm_srli_epi32(_mm_castps_si128(_mm_cvtepi32_ps(hi)), 23);
  __m128i bias = _mm_set1_epi32(126);
  elo = _mm_max_epi32(_mm_sub_epi32(elo, bias), z);   /* ssss = exp-126, 0 if v==0 */
  ehi = _mm_max_epi32(_mm_sub_epi32(ehi, bias), z);
  __m128i p16 = _mm_packs_epi32(elo, ehi);            /* 8x u16 in [0,16] */
  __m128i p8 = _mm_packus_epi16(p16, p16);            /* low 8 bytes valid */
  _mm_storel_epi64((__m128i*)out, p8);
}

/* SIMD-compute the row's ssss categories into sbuf. */
__attribute__((target("sse4.1"))) static void ssss_row(const int16_t* diff, int n, uint8_t* sbuf) {
  int i = 0;
  for (; i + 8 <= n; i += 8) ssss8(diff + i, sbuf + i);
  for (; i < n; i++) sbuf[i] = (uint8_t)ssss_scalar(diff[i]);
}
/* ---- C: SIMD ssss + single histogram ---- */
static void hist_simd_single(const int16_t* diff, int W, int NC, int hist[][NSYM], uint8_t* sbuf) {
  ssss_row(diff, W * NC, sbuf);
  for (int col = 0; col < W; col++)
    for (int c = 0; c < NC; c++)
      hist[c][sbuf[col * NC + c]]++;
}
/* ---- D: SIMD ssss + 4 privatized copies (caller owns/zeros/sums h4) ---- */
static void hist_simd_priv(const int16_t* diff, int W, int NC, int h4[4][16][NSYM], uint8_t* sbuf) {
  ssss_row(diff, W * NC, sbuf);
  int col = 0;
  for (; col + 4 <= W; col += 4)
    for (int k = 0; k < 4; k++)
      for (int c = 0; c < NC; c++)
        h4[k][c][sbuf[(col + k) * NC + c]]++;
  for (; col < W; col++)
    for (int c = 0; c < NC; c++)
      h4[0][c][sbuf[col * NC + c]]++;
}

int main(int argc, char** argv) {
  const char* dir = argc >= 2 ? argv[1] : "testdata";
  int ntiles = argc >= 3 ? atoi(argv[2]) : 48;
  int iters = argc >= 4 ? atoi(argv[3]) : 20;

  int W = 0, H = 0, NC = 0;
  int16_t** diffs = (int16_t**)calloc((size_t)ntiles, sizeof(int16_t*));
  int loaded = 0;
  uint64_t samples = 0;
  for (int t = 0; t < ntiles; t++) {
    char p[512]; snprintf(p, sizeof(p), "%s/tile_%03d.lj92", dir, t);
    int len = 0; uint8_t* data = read_file(p, &len); if (!data) break;
    lj92_dec d = NULL; int w, h, b, c;
    if (lj92_decode_open(&d, data, len, &w, &h, &b, &c) != LJ92_OK) { free(data); break; }
    uint16_t* img = (uint16_t*)malloc((size_t)w * h * c * sizeof(uint16_t));
    lj92_decode_run(d, img, w * c, 0, NULL, 0);
    lj92_decode_close(d); free(data);
    W = w; H = h; NC = c;
    int16_t* df = (int16_t*)malloc((size_t)w * h * c * sizeof(int16_t));
    /* predictor-7 residuals from original pixels (encoder semantics) */
    int initpx = 1 << (b - 1);
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++)
        for (int cc = 0; cc < c; cc++) {
          int idx = (y * w + x) * c + cc;
          int left = x ? img[idx - c] : 0, above = y ? img[idx - w * c] : 0;
          int Px;
          if (y == 0 && x == 0) Px = initpx;
          else if (y == 0) Px = left;
          else if (x == 0) Px = above;
          else Px = (left + above) >> 1;
          df[idx] = (int16_t)(img[idx] - Px);
        }
    free(img);
    diffs[t] = df; samples += (uint64_t)w * h * c; loaded++;
  }
  if (!loaded) { fprintf(stderr, "no tiles\n"); return 1; }
  printf("tiles=%d %dx%dx%d samples/iter=%.1fM iters=%d\n", loaded, W, H, NC,
         (double)samples / 1e6, iters);

  uint8_t* sbuf = (uint8_t*)malloc((size_t)W * NC);
  int ref[16][NSYM], got[16][NSYM];
  static int h4[4][16][NSYM];

  /* mode: 0=naive single, 1=scalar+priv, 2=simd single, 3=simd+priv */
  struct { const char* name; int mode; } V[] = {
      {"A naive(scalar,single)", 0}, {"B scalar+priv", 1},
      {"C simd-ssss,single", 2},     {"D simd-ssss+priv", 3}};
  double base = 0;
  for (int vi = 0; vi < 4; vi++) {
    int priv = (V[vi].mode == 1 || V[vi].mode == 3);
    /* one correctness run + timing runs share this body */
    double t0 = 0; int okc = 1;
    for (int phase = 0; phase < 2; phase++) {       /* 0 = verify, 1 = time */
      int reps = phase == 0 ? 1 : iters;
      if (phase == 1) t0 = now_ms();
      for (int it = 0; it < reps; it++) {
        memset(got, 0, sizeof(got));
        if (priv) memset(h4, 0, sizeof(h4));
        for (int t = 0; t < loaded; t++)
          for (int y = 0; y < H; y++) {
            const int16_t* row = diffs[t] + (size_t)y * W * NC;
            switch (V[vi].mode) {
              case 0: hist_naive(row, W, NC, got); break;
              case 1: hist_priv(row, W, NC, h4); break;
              case 2: hist_simd_single(row, W, NC, got, sbuf); break;
              default: hist_simd_priv(row, W, NC, h4, sbuf); break;
            }
          }
        if (priv)
          for (int c = 0; c < NC; c++)
            for (int s = 0; s < NSYM; s++)
              got[c][s] = h4[0][c][s] + h4[1][c][s] + h4[2][c][s] + h4[3][c][s];
      }
      if (phase == 0) {
        if (vi == 0) memcpy(ref, got, sizeof(ref));
        okc = memcmp(ref, got, sizeof(ref)) == 0;
      }
    }
    double ms = now_ms() - t0;
    double msps = (double)samples * iters / 1e6 * 1000.0 / ms;
    if (vi == 0) base = msps;
    printf("  %-24s %8.1f Msamp/s  (x%.3f)  %s\n", V[vi].name, msps, msps / base,
           okc ? "ok" : "MISMATCH");
  }
  for (int t = 0; t < loaded; t++) free(diffs[t]);
  free(diffs); free(sbuf);
  return 0;
}
