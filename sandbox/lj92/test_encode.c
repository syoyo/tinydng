/* Encoder tests:
 *   (a) round-trip:  new encode -> new decode == original (lossless)
 *   (b) interop A:   new encode -> v2 decode  == original (our stream is valid)
 *   (c) interop B:   v2  encode -> new decode == original (we read v2 streams)
 *   (d) real data:   decode a ProRAW tile, re-encode (pred 1/7), round-trip
 * across bitdepths, component counts, and predictors. */
#if defined(__unix__) || defined(__APPLE__)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lj92.h"
#include "../../tiny_dng_ljpeg92_v2.h"

static uint32_t rng = 0x12345678u;
static uint32_t xr(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static uint16_t* gen_image(int w, int h, int nc, int bitdepth, int mode) {
  uint16_t* img = (uint16_t*)malloc((size_t)w * h * nc * sizeof(uint16_t));
  int maxv = (1 << bitdepth) - 1;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      for (int c = 0; c < nc; c++) {
        int v;
        if (mode == 0) v = (x + y * 3 + c * 7) & maxv;        /* gradient */
        else if (mode == 1) v = (int)(xr() & (uint32_t)maxv); /* random */
        else v = ((x ^ y) * (c + 1)) & maxv;                  /* xor pattern */
        img[((size_t)y * w + x) * nc + c] = (uint16_t)v;
      }
  return img;
}

static int rt_new_new(const uint16_t* img, int w, int h, int bd, int nc, int pred) {
  uint8_t* enc = NULL; int enclen = 0;
  if (lj92_encode(img, w, h, bd, nc, pred, w * nc, 0, NULL, 0, &enc, &enclen) != LJ92_OK)
    return 1;
  lj92_dec d = NULL; int w1, h1, b1, c1;
  int ok = 0;
  if (lj92_decode_open(&d, enc, enclen, &w1, &h1, &b1, &c1) == LJ92_OK) {
    uint16_t* out = (uint16_t*)malloc((size_t)w1 * h1 * c1 * sizeof(uint16_t));
    if (lj92_decode_run(d, out, w1 * c1, 0, NULL, 0) == LJ92_OK &&
        w1 == w && h1 == h && c1 == nc &&
        memcmp(out, img, (size_t)w * h * nc * sizeof(uint16_t)) == 0)
      ok = 1;
    free(out);
    lj92_decode_close(d);
  }
  free(enc);
  return ok ? 0 : 1;
}

static int rt_new_v2(const uint16_t* img, int w, int h, int bd, int nc, int pred) {
  uint8_t* enc = NULL; int enclen = 0;
  if (lj92_encode(img, w, h, bd, nc, pred, w * nc, 0, NULL, 0, &enc, &enclen) != LJ92_OK)
    return 1;
  tdng_lj92 d = NULL; int w1, h1, b1, c1;
  int ok = 0;
  if (tdng_lj92_open(&d, enc, enclen, &w1, &h1, &b1, &c1) == TDNG_LJ92_ERROR_NONE) {
    uint16_t* out = (uint16_t*)malloc((size_t)w1 * h1 * c1 * sizeof(uint16_t));
    if (tdng_lj92_decode(d, out, w1 * c1, 0, NULL, 0) == TDNG_LJ92_ERROR_NONE &&
        memcmp(out, img, (size_t)w * h * nc * sizeof(uint16_t)) == 0)
      ok = 1;
    free(out);
    tdng_lj92_close(d);
  }
  free(enc);
  return ok ? 0 : 1;
}

static int rt_v2_new(const uint16_t* img, int w, int h, int bd, int nc, int pred) {
  /* v2 encode (it supports 1..4 comps, pred 1/2/7 via _ex). */
  uint8_t* enc = NULL; int enclen = 0;
  if (tdng_lj92_encode_ex((uint16_t*)img, w, h, bd, nc, pred, w * nc, 0, NULL, 0,
                          &enc, &enclen) != TDNG_LJ92_ERROR_NONE)
    return 1;
  lj92_dec d = NULL; int w1, h1, b1, c1;
  int ok = 0;
  if (lj92_decode_open(&d, enc, enclen, &w1, &h1, &b1, &c1) == LJ92_OK) {
    uint16_t* out = (uint16_t*)malloc((size_t)w1 * h1 * c1 * sizeof(uint16_t));
    if (lj92_decode_run(d, out, w1 * c1, 0, NULL, 0) == LJ92_OK &&
        memcmp(out, img, (size_t)w * h * nc * sizeof(uint16_t)) == 0)
      ok = 1;
    free(out);
    lj92_decode_close(d);
  }
  free(enc);
  return ok ? 0 : 1;
}

static uint8_t* read_file(const char* path, int* len) {
  FILE* f = fopen(path, "rb"); if (!f) return NULL;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t* b = (uint8_t*)malloc((size_t)n);
  if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
  fclose(f); *len = (int)n; return b;
}

int main(void) {
  int fail = 0, pass = 0;
  int bds[] = {8, 10, 12, 14, 16};
  int ncs[] = {1, 2, 3, 4};
  int preds[] = {1, 2, 4, 5, 6, 7};
  int sizes[][2] = {{1, 1}, {7, 1}, {1, 5}, {17, 13}, {64, 48}, {100, 1}};

  /* Run the whole suite under each forced encode-SIMD path so the scalar,
   * SSE2 and AVX2 enc_diff_row kernels are all exercised (incl. scalar tails
   * via odd widths like 7/17/100). */
  lj92_simd encpaths[] = {LJ92_SIMD_SCALAR, LJ92_SIMD_SSE2, LJ92_SIMD_SSE41, LJ92_SIMD_AVX2};
  const char* encnames[] = {"scalar", "sse2", "sse4.1", "avx2"};
  for (size_t ep = 0; ep < sizeof(encpaths)/sizeof(encpaths[0]); ep++) {
   if (lj92_simd_force(encpaths[ep]) != 0) continue;
   int before = fail;
   for (size_t si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
    int w = sizes[si][0], h = sizes[si][1];
    for (size_t bi = 0; bi < sizeof(bds) / sizeof(bds[0]); bi++) {
      for (size_t ci = 0; ci < sizeof(ncs) / sizeof(ncs[0]); ci++) {
        for (size_t pi = 0; pi < sizeof(preds) / sizeof(preds[0]); pi++) {
          for (int mode = 0; mode < 3; mode++) {
            int bd = bds[bi], nc = ncs[ci], pred = preds[pi];
            uint16_t* img = gen_image(w, h, nc, bd, mode);
            int r1 = rt_new_new(img, w, h, bd, nc, pred);
            int r2 = rt_new_v2(img, w, h, bd, nc, pred);
            if (r1) { fprintf(stderr, "RT new->new FAIL %dx%d bd%d nc%d p%d m%d\n", w, h, bd, nc, pred, mode); fail++; }
            else pass++;
            if (r2) { fprintf(stderr, "RT new->v2  FAIL %dx%d bd%d nc%d p%d m%d\n", w, h, bd, nc, pred, mode); fail++; }
            else pass++;
            /* v2 encode supports pred 1/2/7 only */
            if (pred == 1 || pred == 2 || pred == 7) {
              int r3 = rt_v2_new(img, w, h, bd, nc, pred);
              if (r3) { fprintf(stderr, "RT v2->new  FAIL %dx%d bd%d nc%d p%d m%d\n", w, h, bd, nc, pred, mode); fail++; }
              else pass++;
            }
            free(img);
          }
        }
      }
    }
   }
   printf("  encode path %-7s: %s\n", encnames[ep], fail == before ? "OK" : "FAIL");
  }
  lj92_simd_force(LJ92_SIMD_AUTO);

  /* Real data: decode tile 0, re-encode + round trip with pred 1 and 7. */
  {
    int len = 0;
    uint8_t* data = read_file("testdata/tile_000.lj92", &len);
    if (data) {
      lj92_dec d = NULL; int w, h, b, c;
      if (lj92_decode_open(&d, data, len, &w, &h, &b, &c) == LJ92_OK) {
        uint16_t* img = (uint16_t*)malloc((size_t)w * h * c * sizeof(uint16_t));
        lj92_decode_run(d, img, w * c, 0, NULL, 0);
        lj92_decode_close(d);
        int p1 = rt_new_new(img, w, h, b, c, 1);
        int p7 = rt_new_new(img, w, h, b, c, 7);
        int pv = rt_new_v2(img, w, h, b, c, 7);
        if (p1 || p7 || pv) { fprintf(stderr, "REAL re-encode FAIL (p1=%d p7=%d v2=%d) %dx%dx%d b%d\n", p1, p7, pv, w, h, c, b); fail++; }
        else { pass += 3; printf("real tile re-encode: OK (%dx%dx%d b%d)\n", w, h, c, b); }
        free(img);
      }
      free(data);
    }
  }

  printf("encode tests: %d pass, %d fail\n", pass, fail);
  return fail ? 1 : 0;
}
