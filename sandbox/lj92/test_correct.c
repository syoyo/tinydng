/* Correctness test: decode every extracted .lj92 tile with both the reference
 * v2 decoder and the new lj92 decoder, compare outputs byte-for-byte. Also
 * exercises the new decoder under each forced SIMD path (output must be
 * identical regardless of SIMD level). */
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

static uint8_t* read_file(const char* path, int* len) {
  FILE* f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t* buf = (uint8_t*)malloc((size_t)n);
  if (!buf) { fclose(f); return NULL; }
  if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
  fclose(f);
  *len = (int)n;
  return buf;
}

/* Decode with reference v2. */
static uint16_t* decode_v2(const uint8_t* data, int len, int* w, int* h,
                           int* bits, int* comps) {
  tdng_lj92 lj = NULL;
  if (tdng_lj92_open(&lj, data, len, w, h, bits, comps) != TDNG_LJ92_ERROR_NONE)
    return NULL;
  uint16_t* out = (uint16_t*)malloc((size_t)*w * *h * *comps * sizeof(uint16_t));
  if (!out) { tdng_lj92_close(lj); return NULL; }
  int r = tdng_lj92_decode(lj, out, *w * *comps, 0, NULL, 0);
  tdng_lj92_close(lj);
  if (r != TDNG_LJ92_ERROR_NONE) { free(out); return NULL; }
  return out;
}

/* Decode with the new lj92. */
static uint16_t* decode_new(const uint8_t* data, int len, int* w, int* h,
                            int* bits, int* comps) {
  lj92_dec d = NULL;
  if (lj92_decode_open(&d, data, len, w, h, bits, comps) != LJ92_OK) return NULL;
  uint16_t* out = (uint16_t*)malloc((size_t)*w * *h * *comps * sizeof(uint16_t));
  if (!out) { lj92_decode_close(d); return NULL; }
  int r = lj92_decode_run(d, out, *w * *comps, 0, NULL, 0);
  lj92_decode_close(d);
  if (r != LJ92_OK) { free(out); return NULL; }
  return out;
}

int main(int argc, char** argv) {
  const char* dir = argc >= 2 ? argv[1] : "testdata";
  int ntiles = argc >= 3 ? atoi(argv[2]) : 48;

  int fail = 0, pass = 0;
  lj92_simd paths[] = {LJ92_SIMD_SCALAR, LJ92_SIMD_SSE2, LJ92_SIMD_SSE41,
                       LJ92_SIMD_AVX2};
  const char* path_names[] = {"scalar", "sse2", "sse4.1", "avx2"};

  for (int t = 0; t < ntiles; t++) {
    char path[512];
    snprintf(path, sizeof(path), "%s/tile_%03d.lj92", dir, t);
    int len = 0;
    uint8_t* data = read_file(path, &len);
    if (!data) { fprintf(stderr, "cannot read %s\n", path); continue; }

    int w0, h0, b0, c0;
    uint16_t* ref = decode_v2(data, len, &w0, &h0, &b0, &c0);
    if (!ref) {
      fprintf(stderr, "[tile %d] v2 decode FAILED\n", t);
      fail++; free(data); continue;
    }
    size_t nsamp = (size_t)w0 * h0 * c0;

    for (size_t pi = 0; pi < sizeof(paths) / sizeof(paths[0]); pi++) {
      if (lj92_simd_force(paths[pi]) != 0) continue;  /* not available */
      int w1, h1, b1, c1;
      uint16_t* got = decode_new(data, len, &w1, &h1, &b1, &c1);
      if (!got) {
        fprintf(stderr, "[tile %d/%s] new decode FAILED\n", t, path_names[pi]);
        fail++; continue;
      }
      if (w1 != w0 || h1 != h0 || b1 != b0 || c1 != c0) {
        fprintf(stderr, "[tile %d/%s] geometry mismatch %dx%dx%d b%d vs %dx%dx%d b%d\n",
                t, path_names[pi], w1, h1, c1, b1, w0, h0, c0, b0);
        fail++; free(got); continue;
      }
      if (memcmp(ref, got, nsamp * sizeof(uint16_t)) != 0) {
        size_t first = 0;
        for (; first < nsamp; first++) if (ref[first] != got[first]) break;
        fprintf(stderr, "[tile %d/%s] DATA mismatch at sample %zu: ref=%u new=%u\n",
                t, path_names[pi], first, ref[first], got[first]);
        fail++; free(got); continue;
      }
      pass++;
      free(got);
    }
    free(ref);
    free(data);
  }

  /* Linearize path coverage: decode tile 0 with an identity linearize table and
   * confirm it matches a plain decode (exercises run_scan LIN=1). */
  lj92_simd_force(LJ92_SIMD_AUTO);
  {
    char path[512];
    snprintf(path, sizeof(path), "%s/tile_000.lj92", dir);
    int len = 0;
    uint8_t* data = read_file(path, &len);
    if (data) {
      lj92_dec d = NULL;
      int w, h, b, c;
      if (lj92_decode_open(&d, data, len, &w, &h, &b, &c) == LJ92_OK) {
        size_t n = (size_t)w * h * c;
        uint16_t* plain = (uint16_t*)malloc(n * sizeof(uint16_t));
        lj92_decode_run(d, plain, w * c, 0, NULL, 0);
        lj92_decode_close(d);

        uint16_t* idlut = (uint16_t*)malloc(65536 * sizeof(uint16_t));
        for (int i = 0; i < 65536; i++) idlut[i] = (uint16_t)i;
        uint16_t* lined = (uint16_t*)malloc(n * sizeof(uint16_t));
        lj92_decode_open(&d, data, len, &w, &h, &b, &c);
        int r = lj92_decode_run(d, lined, w * c, 0, idlut, 65535);
        lj92_decode_close(d);
        if (r == LJ92_OK && memcmp(plain, lined, n * sizeof(uint16_t)) == 0) {
          pass++; printf("linearize(identity) path: OK\n");
        } else {
          fail++; fprintf(stderr, "linearize path MISMATCH (r=%d)\n", r);
        }
        free(plain); free(idlut); free(lined);
      }
      free(data);
    }
  }

  printf("correctness: %d pass, %d fail\n", pass, fail);
  return fail ? 1 : 0;
}
