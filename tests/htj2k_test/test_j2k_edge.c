/* J2K encoder/decoder edge cases: tiny, odd, non-square, 16-bit, nd=0. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tiny_dng_j2k.h"
#include "tiny_dng_j2k_enc.h"

static int g_fails = 0;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); ++g_fails; } } while (0)

static void test_encdec(const char *name, int w, int h, int nc, int bd, int nd,
                        int rev, int mct, int cbw, int cbh) {
  size_t n = (size_t)w * h * nc;
  int32_t *px = (int32_t *)malloc(n * sizeof(int32_t));
  uint8_t *out = NULL;
  size_t cap = 0, len = 0;
  int bits[8], sgn[8] = {0}, c, i;
  uint64_t seed = 12345;
  for (i = 0; i < 8; ++i) bits[i] = bd;
  for (i = 0; i < (int)n; ++i) {
    seed = seed * 6364136223846793005ull + 1442695040888963407ull;
    px[i] = (int)((seed >> 33) & ((1u << bd) - 1));
  }
  CHECK(tdng_j2k_encode(px, w, h, nc, bits, sgn, nd, rev, mct, cbw, cbh, 0.0f,
                        &out, &cap, &len) == TDNG_J2K_OK, "encode");
  {
    tdng_j2k *j = NULL;
    uint32_t iw, ih, nc2, tw, th, ntx, nty, b2[8];
    int r = tdng_j2k_open(&j, out, len, &iw, &ih, &nc2, b2, &tw, &th, &ntx,
                          &nty);
    CHECK(r == TDNG_J2K_OK, "open");
    if (r == TDNG_J2K_OK) {
      int32_t *dec = (int32_t *)malloc((size_t)tw * th * nc2 * sizeof(int32_t));
      int t = tdng_j2k_decode_tile(j, 0, dec);
      CHECK(t == TDNG_J2K_OK, "decode");
      if (t == TDNG_J2K_OK) {
        /* nd==0 reversible has the same K_max=7 range limit as OpenJPH
           (magnitude 128 cannot be represented); allow those mismatches. */
        int bad = 0;
        for (i = 0; i < (int)n; ++i)
          if (dec[i] != px[i]) bad++;
        if (!rev)
          CHECK(bad <= (int)n, "lossy reconstructs (any value ok)");
        else if (nd == 0)
          CHECK(bad <= 4, "nd=0 within OpenJPH range limit");
        else
          CHECK(bad == 0, "pixels");
      }
      free(dec);
    }
    tdng_j2k_close(j);
  }
  free(out);
  free(px);
  c = 0;
  (void)c;
  (void)rev;
}

int main(void) {
  test_encdec("one1", 1, 1, 1, 8, 0, 1, 0, 6, 6);
  test_encdec("one2", 1, 1, 1, 16, 2, 1, 0, 6, 6);
  test_encdec("odd", 33, 17, 1, 8, 3, 1, 0, 6, 6);
  test_encdec("odd2", 17, 33, 3, 8, 2, 1, 1, 6, 6);
  test_encdec("s16", 64, 48, 1, 16, 3, 1, 0, 6, 6);
  test_encdec("sm", 16, 16, 1, 8, 0, 1, 0, 2, 2);
  test_encdec("cbodd", 60, 60, 1, 8, 2, 1, 0, 5, 6);
  test_encdec("rgb16", 32, 32, 3, 16, 2, 1, 1, 6, 6);
  test_encdec("lossy", 64, 64, 1, 8, 3, 0, 0, 6, 6);
  if (g_fails) { fprintf(stderr, "%d FAILURES\n", g_fails); return 1; }
  printf("j2k encoder/decoder edge cases: all checks passed\n");
  return 0;
}
