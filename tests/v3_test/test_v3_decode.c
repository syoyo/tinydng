/* Decode-path tests: BigTIFF, horizontal predictor (2), float predictor (3),
   and cross-encoder codec fixtures (LZW/Deflate/PackBits/planar from libtiff).
   The in-memory cases are self-contained; the fixtures live in data/.
   argv[1] = repository source dir. */
#include "td_test_util.h"

/* Build a minimal little-endian classic TIFF, single strip, spp=1. */
static uint8_t *make_classic(uint32_t w, uint32_t h, uint16_t bps,
                             uint16_t sfmt, uint16_t predictor, uint16_t comp,
                             const uint8_t *strip, size_t strip_len,
                             size_t *out_size) {
  /* entries (ascending): 256,257,258,259,262,273,277,278,279,317,339 */
  const int NENT = 11;
  size_t strip_off = 8;
  size_t strip_pad = strip_len & 1u;
  size_t ifd_off = strip_off + strip_len + strip_pad;
  size_t total = ifd_off + 2u + (size_t)NENT * 12u + 4u;
  uint8_t *buf = (uint8_t *)calloc(1, total);
  uint8_t *e;
  int n = 0;
  if (!buf) {
    return NULL;
  }
  buf[0] = 'I';
  buf[1] = 'I';
  tdt_pu16(buf + 2, 42, 0);
  tdt_pu32(buf + 4, (uint32_t)ifd_off, 0);
  memcpy(buf + strip_off, strip, strip_len);
  tdt_pu16(buf + ifd_off, (uint16_t)NENT, 0);
  e = buf + ifd_off + 2u;
#define ENT(tag, type, cnt, val)             \
  do {                                       \
    tdt_pu16(e + n * 12 + 0, (tag), 0);      \
    tdt_pu16(e + n * 12 + 2, (type), 0);     \
    tdt_pu32(e + n * 12 + 4, (cnt), 0);      \
    tdt_pu32(e + n * 12 + 8, (val), 0);      \
    n++;                                     \
  } while (0)
  ENT(256, 4, 1, w);
  ENT(257, 4, 1, h);
  ENT(258, 3, 1, bps);
  ENT(259, 3, 1, comp);
  ENT(262, 3, 1, 1);
  ENT(273, 4, 1, (uint32_t)strip_off);
  ENT(277, 3, 1, 1);
  ENT(278, 4, 1, h);
  ENT(279, 4, 1, (uint32_t)strip_len);
  ENT(317, 3, 1, predictor);
  ENT(339, 3, 1, sfmt);
#undef ENT
  tdt_pu32(buf + ifd_off + 2u + (size_t)NENT * 12u, 0, 0); /* next IFD */
  *out_size = total;
  return buf;
}

static int test_bigtiff(tinydng_context *ctx) {
  uint32_t w = 40, h = 30, i;
  size_t strip_len = (size_t)w * h;
  uint8_t *img = (uint8_t *)malloc(strip_len);
  size_t strip_off = 16, ifd_off, total;
  uint8_t *buf, *e;
  int n = 0;
  tinydng_error err;
  tinydng_document *doc = NULL;
  tinydng_pixels px;
  int rc = 0;
  const int NENT = 9;

  for (i = 0; i < strip_len; i++) {
    img[i] = (uint8_t)((i * 97u + 13u) & 0xffu);
  }
  ifd_off = strip_off + strip_len + (strip_len & 1u);
  total = ifd_off + 8u + (size_t)NENT * 20u + 8u;
  buf = (uint8_t *)calloc(1, total);
  buf[0] = 'I';
  buf[1] = 'I';
  tdt_pu16(buf + 2, 43, 0);
  tdt_pu16(buf + 4, 8, 0);
  tdt_pu16(buf + 6, 0, 0);
  tdt_pu64(buf + 8, ifd_off, 0);
  memcpy(buf + strip_off, img, strip_len);
  tdt_pu64(buf + ifd_off, (uint64_t)NENT, 0);
  e = buf + ifd_off + 8u;
#define BENT(tag, type, cnt, val)             \
  do {                                        \
    tdt_pu16(e + n * 20 + 0, (tag), 0);       \
    tdt_pu16(e + n * 20 + 2, (type), 0);      \
    tdt_pu64(e + n * 20 + 4, (cnt), 0);       \
    tdt_pu64(e + n * 20 + 12, (val), 0);      \
    n++;                                      \
  } while (0)
  BENT(256, 4, 1, w);
  BENT(257, 4, 1, h);
  BENT(258, 3, 1, 8);
  BENT(259, 3, 1, 1);
  BENT(262, 3, 1, 1);
  BENT(273, 16, 1, strip_off); /* StripOffsets as LONG8 */
  BENT(277, 3, 1, 1);
  BENT(278, 4, 1, h);
  BENT(279, 16, 1, strip_len); /* StripByteCounts as LONG8 */
#undef BENT
  tdt_pu64(buf + ifd_off + 8u + (size_t)NENT * 20u, 0, 0);

  if (tinydng_open_memory(ctx, buf, total, NULL, &doc, &err) == TINYDNG_OK &&
      tinydng_decode_image(ctx, doc, 0, NULL, &px, &err) == TINYDNG_OK) {
    CHECK(px.width == w && px.height == h && px.size == strip_len, "bigtiff dims");
    CHECK(memcmp(px.data, img, strip_len) == 0, "bigtiff pixels");
    printf("  BigTIFF (v43, LONG8) %ux%u decoded OK\n", w, h);
    tinydng_pixels_free(ctx, &px);
    tinydng_document_destroy(ctx, doc);
  } else {
    CHECK(0, "bigtiff open/decode: %s", err.message);
    rc = 1;
  }
  free(buf);
  free(img);
  return rc;
}

static int test_predictor2(tinydng_context *ctx) {
  uint32_t w = 50, h = 8, x, y;
  uint16_t *orig = (uint16_t *)malloc((size_t)w * h * 2);
  uint8_t *strip = (uint8_t *)malloc((size_t)w * h * 2);
  uint8_t *buf;
  size_t total;
  tinydng_error err;
  tinydng_document *doc = NULL;
  tinydng_pixels px;
  int rc = 0;
  for (y = 0; y < h; y++) {
    for (x = 0; x < w; x++) {
      orig[y * w + x] = (uint16_t)((x * 311u + y * 1009u) & 0xffffu);
    }
  }
  /* Encode horizontal differences per row, store LE. */
  for (y = 0; y < h; y++) {
    uint16_t prev = 0;
    for (x = 0; x < w; x++) {
      uint16_t cur = orig[y * w + x];
      uint16_t d = (uint16_t)(cur - prev);
      tdt_pu16(strip + ((size_t)y * w + x) * 2u, d, 0);
      prev = cur;
    }
  }
  buf = make_classic(w, h, 16, TINYDNG_SAMPLEFORMAT_UINT, 2,
                     TINYDNG_COMPRESSION_NONE, strip, (size_t)w * h * 2, &total);
  if (tinydng_open_memory(ctx, buf, total, NULL, &doc, &err) == TINYDNG_OK &&
      tinydng_decode_image(ctx, doc, 0, NULL, &px, &err) == TINYDNG_OK) {
    CHECK(memcmp(px.data, orig, (size_t)w * h * 2) == 0,
          "predictor 2 (horizontal, 16-bit)");
    printf("  predictor 2 (horizontal 16-bit) OK\n");
    tinydng_pixels_free(ctx, &px);
    tinydng_document_destroy(ctx, doc);
  } else {
    CHECK(0, "predictor2 open/decode: %s", err.message);
    rc = 1;
  }
  free(buf);
  free(strip);
  free(orig);
  return rc;
}

static int test_predictor3(tinydng_context *ctx) {
  uint32_t w = 23, h = 5, x, y;
  size_t bps = 4, wc = w, cc = wc * bps;
  float *orig = (float *)malloc((size_t)w * h * sizeof(float));
  uint8_t *strip = (uint8_t *)malloc((size_t)w * h * 4);
  uint8_t *tmp = (uint8_t *)malloc(cc);
  uint8_t *buf;
  size_t total, n, b;
  tinydng_error err;
  tinydng_document *doc = NULL;
  tinydng_pixels px;
  int rc = 0;
  for (y = 0; y < h; y++) {
    for (x = 0; x < w; x++) {
      orig[y * w + x] = (float)((double)(x + 1) * 0.5 - (double)y * 1.25);
    }
  }
  /* libtiff float-predictor ENCODE (inverse of decode), per row. */
  for (y = 0; y < h; y++) {
    uint8_t inter[4];
    uint8_t *row = strip + (size_t)y * cc;
    /* de-interleave float bytes into planes (LE host source) */
    for (n = 0; n < wc; n++) {
      float f = orig[y * w + n];
      memcpy(inter, &f, 4); /* host LE bytes */
      for (b = 0; b < bps; b++) {
        tmp[(bps - 1u - b) * wc + n] = inter[b];
      }
    }
    /* horizontal byte difference, stride = spp = 1 */
    for (n = cc - 1u; n >= 1u; n--) {
      row[n] = (uint8_t)(tmp[n] - tmp[n - 1u]);
      if (n == 1u) {
        break;
      }
    }
    row[0] = tmp[0];
  }
  buf = make_classic(w, h, 32, TINYDNG_SAMPLEFORMAT_IEEEFP, 3,
                     TINYDNG_COMPRESSION_NONE, strip, (size_t)w * h * 4, &total);
  if (tinydng_open_memory(ctx, buf, total, NULL, &doc, &err) == TINYDNG_OK &&
      tinydng_decode_image(ctx, doc, 0, NULL, &px, &err) == TINYDNG_OK) {
    CHECK(memcmp(px.data, orig, (size_t)w * h * 4) == 0,
          "predictor 3 (float 32-bit)");
    printf("  predictor 3 (float 32-bit) OK\n");
    tinydng_pixels_free(ctx, &px);
    tinydng_document_destroy(ctx, doc);
  } else {
    CHECK(0, "predictor3 open/decode: %s", err.message);
    rc = 1;
  }
  free(buf);
  free(strip);
  free(tmp);
  free(orig);
  return rc;
}

/* Decode a fixture and compare to the uncompressed baseline (both via our
   reader). Exercises external-encoder (libtiff) compressed streams. */
static int test_fixture(tinydng_context *ctx, const char *root,
                        const char *name, const tinydng_pixels *baseline) {
  char path[1024];
  unsigned char *blob;
  size_t blob_len = 0;
  tinydng_error err;
  tinydng_document *doc = NULL;
  tinydng_pixels px;
  int rc = 0;
  snprintf(path, sizeof(path), "%s/tests/v3_test/data/%s", root, name);
  blob = tdt_slurp(path, &blob_len);
  if (!blob) {
    CHECK(0, "fixture missing: %s", path);
    return 1;
  }
  if (tinydng_open_memory(ctx, blob, blob_len, NULL, &doc, &err) == TINYDNG_OK &&
      tinydng_decode_image(ctx, doc, 0, NULL, &px, &err) == TINYDNG_OK) {
    CHECK(px.size == baseline->size &&
              memcmp(px.data, baseline->data, px.size) == 0,
          "%s matches baseline", name);
    if (px.size == baseline->size &&
        memcmp(px.data, baseline->data, px.size) == 0) {
      printf("  %-22s matches uncompressed baseline\n", name);
    }
    tinydng_pixels_free(ctx, &px);
    tinydng_document_destroy(ctx, doc);
  } else {
    CHECK(0, "%s open/decode: %s", name, err.message);
    rc = 1;
  }
  free(blob);
  return rc;
}

static int test_codec_fixtures(tinydng_context *ctx, const char *root) {
  char path[1024];
  unsigned char *blob;
  size_t blob_len = 0;
  tinydng_error err;
  tinydng_document *doc = NULL;
  tinydng_pixels baseline;
  int rc = 0;
  snprintf(path, sizeof(path), "%s/tests/v3_test/data/baseline_rgb8.tiff", root);
  blob = tdt_slurp(path, &blob_len);
  if (!blob ||
      tinydng_open_memory(ctx, blob, blob_len, NULL, &doc, &err) != TINYDNG_OK ||
      tinydng_decode_image(ctx, doc, 0, NULL, &baseline, &err) != TINYDNG_OK) {
    CHECK(0, "baseline fixture: %s", blob ? err.message : "missing");
    free(blob);
    return 1;
  }
  rc |= test_fixture(ctx, root, "lzw_rgb8.tiff", &baseline);
  rc |= test_fixture(ctx, root, "deflate_rgb8.tiff", &baseline);
  rc |= test_fixture(ctx, root, "packbits_rgb8.tiff", &baseline);
  rc |= test_fixture(ctx, root, "planar_lzw_rgb8.tiff", &baseline);
  tinydng_pixels_free(ctx, &baseline);
  tinydng_document_destroy(ctx, doc);
  free(blob);
  return rc;
}

int main(int argc, char **argv) {
  tinydng_context *ctx = tinydng_context_create(NULL, NULL);
  const char *root = (argc > 1) ? argv[1] : ".";
  if (!ctx) {
    return 1;
  }
  printf("== decode tests ==\n");
  test_bigtiff(ctx);
  test_predictor2(ctx);
  test_predictor3(ctx);
  printf("== cross-encoder codec fixtures ==\n");
  test_codec_fixtures(ctx, root);

  if (tinydng_context_memory_used(ctx) != 0u) {
    fprintf(stderr, "  FAIL: leak, used=%zu\n",
            tinydng_context_memory_used(ctx));
    g_fail = 1;
  }
  tinydng_context_destroy(ctx);
  printf(g_fail ? "DECODE: FAILURES\n" : "DECODE: ALL PASS\n");
  return g_fail;
}
