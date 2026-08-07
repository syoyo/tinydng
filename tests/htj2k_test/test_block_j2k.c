/* Self-contained HTJ2K block codec + J2K codestream test.
   No OpenJPH dependency: the block encoder/decoder round-trip is checked
   against itself, and a tiny embedded lossless codestream (produced by
   OpenJPH ojph_compress) is decoded with expected pixel values. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "tiny_dng_htj2k.h"
#include "tiny_dng_j2k.h"

static int g_fails = 0;
#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    if (!(cond)) {                                                           \
      fprintf(stderr, "FAIL: %s\n", msg);                                    \
      ++g_fails;                                                             \
    }                                                                        \
  } while (0)

/* ---- block codec round-trip ---- */
static void test_block_roundtrip(void) {
  const int sizes[][2] = {{1, 1}, {2, 2}, {4, 4}, {8, 8},  {16, 16},
                          {32, 32}, {16, 4}, {4, 16}, {64, 64}};
  const int kmaxs[] = {4, 8, 12, 16, 20, 24};
  int s, k;
  for (s = 0; s < (int)(sizeof(sizes) / sizeof(sizes[0])); ++s) {
    int w = sizes[s][0], h = sizes[s][1];
    for (k = 0; k < (int)(sizeof(kmaxs) / sizeof(kmaxs[0])); ++k) {
      int K_max = kmaxs[k];
      if (K_max < 2) continue;
      int m = K_max - 1;
      uint32_t p = 31 - K_max;
      size_t n = (size_t)w * h;
      int32_t *coef = (int32_t *)malloc(n * sizeof(int32_t));
      int32_t *dec = (int32_t *)malloc(n * sizeof(int32_t));
      uint8_t *enc = NULL;
      size_t enc_cap = 0;
      uint32_t enc_len = 0;
      size_t i;
      uint64_t seed = (uint64_t)(w * 1000 + h) * 31 + (unsigned)K_max;
      for (i = 0; i < n; ++i) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        int32_t v = (int32_t)((seed >> 33) % (1u << (K_max - 1) < 4 ? 4u : 8u));
        if ((seed >> 41) & 1) v = -v;
        if ((seed >> 49) % 5 == 0) v = 0;
        coef[i] = (v < 0 ? 0x80000000u : 0u) |
                  (uint32_t)((uint64_t)(v < 0 ? -v : v) << p);
      }
      uint32_t lengths[3] = {0, 0, 0};
      int r = tdng_htj2k_encode_codeblock32(coef, m, w, h, w, lengths, &enc,
                                            &enc_cap);
      CHECK(r == TDNG_HTJ2K_OK, "encode");
      enc_len = lengths[0];
      r = tdng_htj2k_decode_codeblock32(enc, dec, m, 1, enc_len, 0, w, h, w,
                                        0);
      CHECK(r == TDNG_HTJ2K_OK, "decode");
      for (i = 0; i < n; ++i) {
        /* decoder output carries a half-bin at bit p-1; extract integer */
        int32_t dv = (int32_t)((uint32_t)dec[i] & 0x80000000u)
                         ? -(int32_t)(((uint32_t)dec[i] & 0x7FFFFFFFu) >> p)
                         : (int32_t)(((uint32_t)dec[i] & 0x7FFFFFFFu) >> p);
        int32_t cv = (coef[i] & 0x80000000u)
                         ? -(int32_t)(((uint32_t)coef[i] & 0x7FFFFFFFu) >> p)
                         : (int32_t)(((uint32_t)coef[i] & 0x7FFFFFFFu) >> p);
        char msg[128];
        snprintf(msg, sizeof(msg), "roundtrip %dx%d K=%d idx=%zu", w, h,
                 K_max, i);
        CHECK(dv == cv, msg);
        if (dv != cv) break;
      }
      free(coef);
      free(dec);
      free(enc);
    }
  }
}

/* ---- embedded codestream decode ---- */
static const unsigned char t16_j2c[193] = {
    0xff, 0x4f, 0xff, 0x51, 0x00, 0x29, 0x40, 0x00, 0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x07, 0x01, 0x01, 0xff, 0x50, 0x00,
    0x08, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0xff, 0x52, 0x00, 0x0c, 0x00,
    0x02, 0x00, 0x01, 0x00, 0x02, 0x04, 0x04, 0x40, 0x01, 0xff, 0x5c, 0x00,
    0x0a, 0x20, 0x48, 0x50, 0x50, 0x50, 0x48, 0x48, 0x48, 0xff, 0x64, 0x00,
    0x17, 0x00, 0x01, 0x4f, 0x70, 0x65, 0x6e, 0x4a, 0x50, 0x48, 0x20, 0x56,
    0x65, 0x72, 0x20, 0x30, 0x2e, 0x33, 0x31, 0x2e, 0x30, 0x2e, 0xff, 0x90,
    0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x55, 0x00, 0x01, 0xff, 0x93,
    0xc0, 0x2d, 0x60, 0xff, 0x6b, 0xe7, 0xbf, 0xe7, 0x69, 0xb6, 0x78, 0x05,
    0x5e, 0xd2, 0xff, 0x68, 0x71, 0xed, 0x04, 0x67, 0x00, 0x00, 0x31, 0xb7,
    0x00, 0xc0, 0x13, 0xc0, 0x15, 0x00, 0x24, 0xf9, 0xa0, 0xa7, 0x42, 0x75,
    0x00, 0x94, 0x88, 0xfe, 0xc1, 0xc9, 0xec, 0xb5, 0x00, 0xc0, 0x2a, 0x60,
    0x15, 0x80, 0x00, 0x00, 0xe7, 0xe9, 0xe9, 0xe9, 0xe2, 0x77, 0x00, 0x08,
    0x00, 0x00, 0xfe, 0xfd, 0x92, 0x7b, 0xf6, 0x49, 0xec, 0xb8, 0x00, 0xff,
    0xd9};

static void test_codestream(void) {
  tdng_j2k *j2k = NULL;
  uint32_t iw, ih, nc, tw, th, ntx, nty;
  uint32_t bits[8];
  int r = tdng_j2k_open(&j2k, t16_j2c, sizeof(t16_j2c), &iw, &ih, &nc, bits,
                        &tw, &th, &ntx, &nty);
  CHECK(r == TDNG_J2K_OK, "open embedded stream");
  CHECK(iw == 16 && ih == 16, "dims");
  CHECK(nc == 1, "comps");
  CHECK(bits[0] == 8, "bitdepth");
  int32_t out[256];
  int32_t expect[256];
  int x, y;
  for (y = 0; y < 16; ++y)
    for (x = 0; x < 16; ++x) expect[y * 16 + x] = (x * 3 + y * 5) % 251;
  r = tdng_j2k_decode_tile(j2k, 0, out);
  CHECK(r == TDNG_J2K_OK, "decode tile");
  for (y = 0; y < 16; ++y)
    for (x = 0; x < 16; ++x) {
      char msg[128];
      snprintf(msg, sizeof(msg), "pixel %d,%d", x, y);
      CHECK(out[y * 16 + x] == expect[y * 16 + x], msg);
    }
  tdng_j2k_close(j2k);
}

/* ---- corrupt-input robustness (must not crash) ---- */
static void test_corrupt(void) {
  uint8_t data[64];
  int i;
  memcpy(data, t16_j2c, 64);
  for (i = 0; i < 64; ++i) data[i] ^= (uint8_t)(i + 1);
  tdng_j2k *j2k = NULL;
  uint32_t iw, ih, nc, tw, th, ntx, nty;
  uint32_t bits[8];
  int r = tdng_j2k_open(&j2k, data, 64, &iw, &ih, &nc, bits, &tw, &th, &ntx,
                        &nty);
  if (r == TDNG_J2K_OK && j2k) {
    if ((uint64_t)tw * th * nc < 64u * 1024u * 1024u) {
      int32_t *out = (int32_t *)malloc((size_t)tw * th * nc * 4);
      for (uint32_t t = 0; t < ntx * nty; ++t)
        tdng_j2k_decode_tile(j2k, t, out);
      free(out);
    }
    tdng_j2k_close(j2k);
  }
  CHECK(1, "no crash on corrupt input");
}

int main(void) {
  test_block_roundtrip();
  test_codestream();
  test_corrupt();
  if (g_fails) {
    fprintf(stderr, "%d FAILURES\n", g_fails);
    return 1;
  }
  printf("htj2k/j2k self-test: all checks passed\n");
  return 0;
}
