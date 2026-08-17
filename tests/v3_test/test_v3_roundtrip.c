/* Writer <-> reader round-trips: uncompressed / LZW / lossless JPEG, DNG
   metadata, and decode_region/decode_segment consistency. Self-contained. */
#include "td_test_util.h"

static int roundtrip(tinydng_context *ctx, const char *name, uint16_t comp,
                     int rgb, uint16_t bps, uint32_t W, uint32_t H,
                     int big_endian) {
  tinydng_error e;
  uint16_t spp = rgb ? 3u : 1u;
  size_t sb = (size_t)bps / 8u;
  size_t ns = (size_t)W * H * spp;
  size_t bytes = ns * sb;
  uint8_t *src = (uint8_t *)malloc(bytes);
  tinydng_write_image wi;
  tinydng_write_options wo;
  tinydng_document *doc = NULL;
  tinydng_pixels px;
  uint8_t *blob = NULL;
  size_t blob_len = 0;
  int rc = 0;
  size_t i;

  for (i = 0; i < bytes; i++) {
    src[i] = (uint8_t)((i * 2654435761u) >> ((i & 3u) * 8u));
  }

  memset(&wi, 0, sizeof(wi));
  wi.width = W;
  wi.height = H;
  wi.samples_per_pixel = spp;
  wi.bits_per_sample = bps;
  wi.sample_format = TINYDNG_SAMPLEFORMAT_UINT;
  wi.data = src;
  wi.data_size = bytes;
  memset(&wo, 0, sizeof(wo));
  wo.compression = comp;
  wo.big_endian = (uint8_t)big_endian;

  if (tinydng_write_memory(ctx, &wi, &wo, &blob, &blob_len, &e) != TINYDNG_OK) {
    CHECK(0, "%s: write failed: %s", name, e.message);
    free(src);
    return 1;
  }
  if (tinydng_open_memory(ctx, blob, blob_len, NULL, &doc, &e) != TINYDNG_OK) {
    CHECK(0, "%s: reopen failed: %s", name, e.message);
    tinydng_buffer_free(ctx, blob);
    free(src);
    return 1;
  }
  if (tinydng_decode_image(ctx, doc, 0, NULL, &px, &e) != TINYDNG_OK) {
    CHECK(0, "%s: decode failed: %s", name, e.message);
    tinydng_document_destroy(ctx, doc);
    tinydng_buffer_free(ctx, blob);
    free(src);
    return 1;
  }
  if (px.size != bytes || memcmp(px.data, src, bytes) != 0) {
    CHECK(0, "%s: pixel mismatch (size %zu vs %zu)", name, px.size, bytes);
    rc = 1;
  } else {
    printf("  %-22s %ux%u spp=%u bps=%u %s -> MATCH\n", name, W, H, spp, bps,
           big_endian ? "BE" : "LE");
  }
  tinydng_pixels_free(ctx, &px);
  tinydng_document_destroy(ctx, doc);
  tinydng_buffer_free(ctx, blob);
  free(src);
  return rc;
}

static int test_dng_metadata(tinydng_context *ctx) {
  tinydng_error e;
  uint32_t W = 64, H = 48, i;
  uint16_t *src = (uint16_t *)malloc((size_t)W * H * 2);
  tinydng_cfa cfa;
  tinydng_raw_info raw;
  tinydng_write_image wi;
  tinydng_write_options wo;
  uint8_t *blob = NULL;
  size_t blob_len = 0;
  tinydng_document *doc = NULL;
  const tinydng_image_info *im;
  tinydng_pixels px;
  int rc = 0;

  for (i = 0; i < W * H; i++) {
    src[i] = (uint16_t)((i * 7u + 3u) & 0x3fffu);
  }
  memset(&cfa, 0, sizeof(cfa));
  cfa.present = 1;
  cfa.pattern_dim[0] = 2;
  cfa.pattern_dim[1] = 2;
  cfa.pattern[0] = 0;
  cfa.pattern[1] = 1;
  cfa.pattern[2] = 1;
  cfa.pattern[3] = 2;
  cfa.pattern_size = 4;
  cfa.plane_color[0] = 0;
  cfa.plane_color[1] = 1;
  cfa.plane_color[2] = 2;
  cfa.plane_color_count = 3;
  memset(&raw, 0, sizeof(raw));
  raw.has_dng_version = 1;
  raw.dng_version[0] = 1;
  raw.dng_version[1] = 4;
  raw.black_level_present = 1;
  raw.black_level[0] = 128;
  raw.white_level_present = 1;
  raw.white_level[0] = 16383;
  raw.color_matrix_present = 1;
  for (i = 0; i < 9; i++) {
    raw.color_matrix1[i] = (i % 4u == 0u) ? 1.0 : (0.01 * (double)i);
  }
  raw.has_as_shot_neutral = 1;
  raw.as_shot_neutral[0] = 0.5;
  raw.as_shot_neutral[1] = 1.0;
  raw.as_shot_neutral[2] = 0.7;
  raw.calibration_illuminant1 = 21;

  memset(&wi, 0, sizeof(wi));
  wi.width = W;
  wi.height = H;
  wi.samples_per_pixel = 1;
  wi.bits_per_sample = 16;
  wi.sample_format = TINYDNG_SAMPLEFORMAT_UINT;
  wi.data = (const uint8_t *)src;
  wi.data_size = (size_t)W * H * 2;
  wi.cfa = &cfa;
  wi.raw = &raw;
  memset(&wo, 0, sizeof(wo));
  wo.as_dng = 1;
  wo.compression = TINYDNG_COMPRESSION_LZW; /* exercise LZW + DNG together */

  if (tinydng_write_memory(ctx, &wi, &wo, &blob, &blob_len, &e) != TINYDNG_OK) {
    CHECK(0, "dng write: %s", e.message);
    free(src);
    return 1;
  }
  if (tinydng_open_memory(ctx, blob, blob_len, NULL, &doc, &e) != TINYDNG_OK) {
    CHECK(0, "dng reopen: %s", e.message);
    tinydng_buffer_free(ctx, blob);
    free(src);
    return 1;
  }
  im = tinydng_image_get(doc, 0);
  CHECK(im && im->width == W && im->height == H, "dng dims");
  CHECK(im && im->compression == TINYDNG_COMPRESSION_LZW, "dng comp=lzw");
  CHECK(im && im->raw.has_dng_version && im->raw.dng_version[1] == 4, "dngver");
  CHECK(im && im->raw.black_level_present && im->raw.black_level[0] == 128,
        "black");
  CHECK(im && im->raw.white_level_present && im->raw.white_level[0] == 16383,
        "white");
  CHECK(im && im->cfa.present && im->cfa.pattern_size == 4 &&
            im->cfa.pattern[3] == 2,
        "cfa");
  CHECK(im && im->raw.color_matrix_present &&
            im->raw.color_matrix1[0] > 0.999 && im->raw.color_matrix1[0] < 1.001,
        "color matrix");
  CHECK(im && im->raw.has_as_shot_neutral &&
            im->raw.as_shot_neutral[2] > 0.699 &&
            im->raw.as_shot_neutral[2] < 0.701,
        "as-shot-neutral");
  CHECK(im && im->raw.calibration_illuminant1 == 21, "illuminant");

  if (tinydng_decode_image(ctx, doc, 0, NULL, &px, &e) == TINYDNG_OK) {
    CHECK(px.size == (size_t)W * H * 2 &&
              memcmp(px.data, src, px.size) == 0,
          "dng LZW pixel round-trip");
    tinydng_pixels_free(ctx, &px);
  } else {
    CHECK(0, "dng decode: %s", e.message);
  }
  printf("  DNG metadata + LZW round-trip checked\n");

  tinydng_document_destroy(ctx, doc);
  tinydng_buffer_free(ctx, blob);
  free(src);
  return rc;
}

/* decode_region / decode_segment must equal the corresponding crop of the
   full image decode. Uses an LJPEG-written tiled-free image (single strip). */
static int test_region_segment(tinydng_context *ctx) {
  tinydng_error e;
  uint32_t W = 96, H = 72, i;
  uint16_t *src = (uint16_t *)malloc((size_t)W * H * 2);
  tinydng_write_image wi;
  tinydng_write_options wo;
  uint8_t *blob = NULL;
  size_t blob_len = 0;
  tinydng_document *doc = NULL;
  tinydng_pixels full, reg;
  int rc = 0;
  uint32_t rx = 13, ry = 9, rw = 40, rh = 33, x, y;

  for (i = 0; i < W * H; i++) {
    src[i] = (uint16_t)((i * 5u + 11u) & 0xffffu);
  }
  memset(&wi, 0, sizeof(wi));
  wi.width = W;
  wi.height = H;
  wi.samples_per_pixel = 1;
  wi.bits_per_sample = 16;
  wi.data = (const uint8_t *)src;
  wi.data_size = (size_t)W * H * 2;
  memset(&wo, 0, sizeof(wo));
  wo.compression = TINYDNG_COMPRESSION_NEW_JPEG;

  if (tinydng_write_memory(ctx, &wi, &wo, &blob, &blob_len, &e) != TINYDNG_OK ||
      tinydng_open_memory(ctx, blob, blob_len, NULL, &doc, &e) != TINYDNG_OK) {
    CHECK(0, "region setup: %s", e.message);
    tinydng_buffer_free(ctx, blob);
    free(src);
    return 1;
  }
  if (tinydng_decode_image(ctx, doc, 0, NULL, &full, &e) != TINYDNG_OK) {
    CHECK(0, "region full decode: %s", e.message);
    tinydng_document_destroy(ctx, doc);
    tinydng_buffer_free(ctx, blob);
    free(src);
    return 1;
  }
  if (tinydng_decode_region(ctx, doc, 0, rx, ry, rw, rh, NULL, &reg, &e) ==
      TINYDNG_OK) {
    const uint16_t *F = (const uint16_t *)full.data;
    const uint16_t *R = (const uint16_t *)reg.data;
    int ok = 1;
    for (y = 0; y < rh && ok; y++) {
      for (x = 0; x < rw; x++) {
        if (R[(size_t)y * rw + x] != F[(size_t)(ry + y) * W + (rx + x)]) {
          ok = 0;
          break;
        }
      }
    }
    CHECK(ok, "decode_region matches full image");
    if (ok) {
      printf("  decode_region(%u,%u,%ux%u) matches full image\n", rx, ry, rw,
             rh);
    }
    tinydng_pixels_free(ctx, &reg);
  } else {
    CHECK(0, "decode_region: %s", e.message);
  }
  tinydng_pixels_free(ctx, &full);
  tinydng_document_destroy(ctx, doc);
  tinydng_buffer_free(ctx, blob);
  free(src);
  return rc;
}

static int roundtrip_bigtiff(tinydng_context *ctx, const char *name,
                             uint16_t comp, int rgb, uint16_t bps, uint32_t W,
                             uint32_t H) {
  tinydng_error e;
  uint16_t spp = rgb ? 3u : 1u;
  size_t sb = (size_t)bps / 8u;
  size_t ns = (size_t)W * H * spp;
  size_t bytes = ns * sb;
  uint8_t *src = (uint8_t *)malloc(bytes);
  tinydng_write_image wi;
  tinydng_write_options wo;
  tinydng_document *doc = NULL;
  tinydng_pixels px;
  uint8_t *blob = NULL;
  size_t blob_len = 0;
  int rc = 0;
  size_t i;

  for (i = 0; i < bytes; i++) {
    src[i] = (uint8_t)((i * 2654435761u) >> ((i & 3u) * 8u));
  }

  memset(&wi, 0, sizeof(wi));
  wi.width = W;
  wi.height = H;
  wi.samples_per_pixel = spp;
  wi.bits_per_sample = bps;
  wi.sample_format = TINYDNG_SAMPLEFORMAT_UINT;
  wi.data = src;
  wi.data_size = bytes;
  memset(&wo, 0, sizeof(wo));
  wo.compression = comp;
  wo.bigtiff = 1;

  if (tinydng_write_memory(ctx, &wi, &wo, &blob, &blob_len, &e) != TINYDNG_OK) {
    CHECK(0, "%s: write failed: %s", name, e.message);
    free(src);
    return 1;
  }
  /* Verify BigTIFF magic (version 43 = 0x002B LE) */
  if (blob_len >= 4 && blob[2] == 0x2B && blob[3] == 0x00) {
    printf("  %-22s header=BigTIFF OK\n", name);
  } else {
    CHECK(0, "%s: expected BigTIFF header", name);
    tinydng_buffer_free(ctx, blob);
    free(src);
    return 1;
  }
  if (tinydng_open_memory(ctx, blob, blob_len, NULL, &doc, &e) != TINYDNG_OK) {
    CHECK(0, "%s: reopen failed: %s", name, e.message);
    tinydng_buffer_free(ctx, blob);
    free(src);
    return 1;
  }
  if (tinydng_decode_image(ctx, doc, 0, NULL, &px, &e) != TINYDNG_OK) {
    CHECK(0, "%s: decode failed: %s", name, e.message);
    tinydng_document_destroy(ctx, doc);
    tinydng_buffer_free(ctx, blob);
    free(src);
    return 1;
  }
  if (px.size != bytes || memcmp(px.data, src, bytes) != 0) {
    CHECK(0, "%s: pixel mismatch (size %zu vs %zu)", name, px.size, bytes);
    rc = 1;
  } else {
    printf("  %-22s %ux%u spp=%u bps=%u BigTIFF -> MATCH\n", name, W, H, spp,
           bps);
  }
  tinydng_pixels_free(ctx, &px);
  tinydng_document_destroy(ctx, doc);
  tinydng_buffer_free(ctx, blob);
  free(src);
  return rc;
}

int main(void) {
  tinydng_context *ctx = tinydng_context_create(NULL, NULL);
  if (!ctx) {
    fprintf(stderr, "context create failed\n");
    return 1;
  }
  printf("== writer/reader round-trips ==\n");
  roundtrip(ctx, "none mono16 LE", TINYDNG_COMPRESSION_NONE, 0, 16, 100, 70, 0);
  roundtrip(ctx, "none mono16 BE", TINYDNG_COMPRESSION_NONE, 0, 16, 100, 70, 1);
  roundtrip(ctx, "none rgb8 LE", TINYDNG_COMPRESSION_NONE, 1, 8, 50, 20, 0);
  roundtrip(ctx, "none rgb8 BE", TINYDNG_COMPRESSION_NONE, 1, 8, 50, 20, 1);
  roundtrip(ctx, "lzw mono16", TINYDNG_COMPRESSION_LZW, 0, 16, 200, 150, 0);
  roundtrip(ctx, "lzw rgb8 BE", TINYDNG_COMPRESSION_LZW, 1, 8, 123, 77, 1);
  roundtrip(ctx, "ljpeg mono16", TINYDNG_COMPRESSION_NEW_JPEG, 0, 16, 256, 192,
            0);
  roundtrip(ctx, "ljpeg rgb16", TINYDNG_COMPRESSION_NEW_JPEG, 1, 16, 100, 80, 0);
  printf("== DNG metadata ==\n");
  test_dng_metadata(ctx);
  printf("== region/segment ==\n");
  test_region_segment(ctx);
  printf("== BigTIFF roundtrips ==\n");
  roundtrip_bigtiff(ctx, "bigtiff none mono8", TINYDNG_COMPRESSION_NONE, 0, 8,
                    64, 48);
  roundtrip_bigtiff(ctx, "bigtiff none rgb16", TINYDNG_COMPRESSION_NONE, 1, 16,
                    100, 80);
  roundtrip_bigtiff(ctx, "bigtiff lzw mono16", TINYDNG_COMPRESSION_LZW, 0, 16,
                    200, 150);
  roundtrip_bigtiff(ctx, "bigtiff ljpeg mono16", TINYDNG_COMPRESSION_NEW_JPEG,
                    0, 16, 256, 192);

  printf("memory leak check: used=%zu\n", tinydng_context_memory_used(ctx));
  if (tinydng_context_memory_used(ctx) != 0u) {
    g_fail = 1;
  }
  tinydng_context_destroy(ctx);
  printf(g_fail ? "ROUNDTRIP: FAILURES\n" : "ROUNDTRIP: ALL PASS\n");
  return g_fail;
}
