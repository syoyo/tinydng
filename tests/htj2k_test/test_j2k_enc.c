/* Self-contained lossless J2K encoder round-trip test (no OpenJPH). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tiny_dng_j2k.h"
#include "tiny_dng_j2k_enc.h"

static int g_fails = 0;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); ++g_fails; } } while (0)

int main(void) {
  int w = 64, h = 48, nc = 3, nd = 3;
  int bits[8] = {8, 8, 8, 0, 0, 0, 0, 0};
  int sgn[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  int32_t *px = (int32_t *)malloc((size_t)w * h * nc * sizeof(int32_t));
  uint8_t *out = NULL;
  size_t cap = 0, len = 0;
  int i;
  for (i = 0; i < w * h * nc; ++i) px[i] = (i * 73 + 11) & 0xFF;
  CHECK(tdng_j2k_encode(px, w, h, nc, bits, sgn, nd, 1, 1, 6, 6, 0.0f, &out, &cap, &len) == TDNG_J2K_OK, "encode");
  {
    tdng_j2k *j = NULL;
    uint32_t iw, ih, c2, tw, th, ntx, nty, b2[8];
    int r = tdng_j2k_open(&j, out, len, &iw, &ih, &c2, b2, &tw, &th, &ntx, &nty);
    CHECK(r == TDNG_J2K_OK, "open");
    CHECK(iw == w && ih == h && c2 == nc, "dims");
    if (r == TDNG_J2K_OK) {
      int32_t *dec = (int32_t *)malloc((size_t)tw * th * c2 * sizeof(int32_t));
      int t = tdng_j2k_decode_tile(j, 0, dec);
      CHECK(t == TDNG_J2K_OK, "decode");
      if (t == TDNG_J2K_OK)
        for (i = 0; i < w * h * nc; ++i)
          CHECK(dec[i] == px[i], "pixel");
      free(dec);
    }
    tdng_j2k_close(j);
  }
  free(out);
  free(px);
  if (g_fails) { fprintf(stderr, "%d FAILURES\n", g_fails); return 1; }
  printf("j2k encoder round-trip: all checks passed\n");
  return 0;
}
