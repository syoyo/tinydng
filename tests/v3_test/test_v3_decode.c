/* Decode-path tests: BigTIFF, horizontal predictor (2), float predictor (3),
   and cross-encoder codec fixtures (LZW/Deflate/PackBits/planar from libtiff).
   The in-memory cases are self-contained; the fixtures live in data/.
   argv[1] = repository source dir. */
#include "td_test_util.h"
#include "tiny_dng_ljpeg92_v2.h"

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

  /* The stdio backend materializes the bulk IFD window in a scratch buffer.
   * Keep this fixture exactly sized so a short next-IFD reservation cannot be
   * masked by unrelated bytes after the file. */
  {
    const char *path = "test_v3_bigtiff_stdio.tif";
    FILE *fp = fopen(path, "wb");
    tinydng_document *file_doc = NULL;
    tinydng_status st = TINYDNG_E_IO;
    if (fp) {
      if (fwrite(buf, 1, total, fp) == total && fclose(fp) == 0) {
        fp = NULL;
        st = tinydng_open_file(ctx, path, NULL, &file_doc, &err);
      } else {
        fclose(fp);
        fp = NULL;
      }
    }
    CHECK(st == TINYDNG_OK, "bigtiff stdio open: %s", err.message);
    if (st == TINYDNG_OK) {
      tinydng_pixels fpx;
      tinydng_status dst = tinydng_decode_image(ctx, file_doc, 0, NULL,
                                                &fpx, &err);
      CHECK(dst == TINYDNG_OK, "bigtiff stdio decode: %s", err.message);
      if (dst == TINYDNG_OK) {
        CHECK(fpx.width == w && fpx.height == h && fpx.size == strip_len,
              "bigtiff stdio dims");
        CHECK(memcmp(fpx.data, img, strip_len) == 0,
              "bigtiff stdio pixels");
        tinydng_pixels_free(ctx, &fpx);
      }
      tinydng_document_destroy(ctx, file_doc);
    }
    remove(path);
  }
  free(buf);
  free(img);
  return rc;
}

/* BigTIFF big-endian byte order. */
static int test_bigtiff_be(tinydng_context *ctx) {
  uint32_t w = 32, h = 24, i;
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
    img[i] = (uint8_t)((i * 73u + 5u) & 0xffu);
  }
  ifd_off = strip_off + strip_len + (strip_len & 1u);
  total = ifd_off + 8u + (size_t)NENT * 20u + 8u;
  buf = (uint8_t *)calloc(1, total);
  buf[0] = 'M';
  buf[1] = 'M';
  tdt_pu16(buf + 2, 43, 1);
  tdt_pu16(buf + 4, 8, 1);
  tdt_pu16(buf + 6, 0, 1);
  tdt_pu64(buf + 8, ifd_off, 1);
  memcpy(buf + strip_off, img, strip_len);
  tdt_pu64(buf + ifd_off, (uint64_t)NENT, 1);
  e = buf + ifd_off + 8u;
#define BENT_BE(tag, type, cnt, val)            \
  do {                                          \
    tdt_pu16(e + n * 20 + 0, (tag), 1);         \
    tdt_pu16(e + n * 20 + 2, (type), 1);        \
    tdt_pu64(e + n * 20 + 4, (cnt), 1);         \
    tdt_pu64(e + n * 20 + 12, (val), 1);        \
    n++;                                        \
  } while (0)
  /* All values use LONG8 (type 16) to avoid endianness alignment issues
     when writing via tdt_pu64. */
  BENT_BE(256, 16, 1, w);
  BENT_BE(257, 16, 1, h);
  BENT_BE(258, 16, 1, 8);
  BENT_BE(259, 16, 1, 1);
  BENT_BE(262, 16, 1, 1);
  BENT_BE(273, 16, 1, strip_off);
  BENT_BE(277, 16, 1, 1);
  BENT_BE(278, 16, 1, h);
  BENT_BE(279, 16, 1, strip_len);
#undef BENT_BE
  tdt_pu64(buf + ifd_off + 8u + (size_t)NENT * 20u, 0, 1);

  if (tinydng_open_memory(ctx, buf, total, NULL, &doc, &err) == TINYDNG_OK &&
      tinydng_decode_image(ctx, doc, 0, NULL, &px, &err) == TINYDNG_OK) {
    CHECK(px.width == w && px.height == h && px.size == strip_len,
          "bigtiff BE dims");
    CHECK(memcmp(px.data, img, strip_len) == 0, "bigtiff BE pixels");
    printf("  BigTIFF BE (v43, LONG8) %ux%u decoded OK\n", w, h);
    tinydng_pixels_free(ctx, &px);
    tinydng_document_destroy(ctx, doc);
  } else {
    CHECK(0, "bigtiff BE open/decode: %s", err.message);
    rc = 1;
  }
  free(buf);
  free(img);
  return rc;
}

/* BigTIFF with tiled layout (TileWidth/TileLength + tile offsets as LONG8). */
static int test_bigtiff_tiled(tinydng_context *ctx) {
  uint32_t W = 48, H = 40, TW = 16, TL = 8;
  uint32_t across = (W + TW - 1u) / TW;
  uint32_t down = (H + TL - 1u) / TL;
  uint32_t ntiles = across * down;
  size_t tile_bytes = (size_t)TW * TL;
  size_t img_bytes = (size_t)W * H;
  uint8_t *img = (uint8_t *)calloc(1, img_bytes);
  uint32_t t;
  /* IFD: 10 entries */
  const int NENT = 10;
  /* Layout: header(16) | tile data | tile_offsets(ntiles*8) |
     tile_bytecounts(ntiles*8) | IFD(8+NENT*20+8) */
  size_t data_off = 16;
  size_t toff_off, tbc_off, ifd_off, total;
  uint8_t *buf, *e;
  int n = 0;
  tinydng_error err;
  tinydng_document *doc = NULL;
  tinydng_pixels px;
  int rc = 0;

  for (t = 0; t < (uint32_t)img_bytes; t++) {
    img[t] = (uint8_t)((t * 41u + 7u) & 0xffu);
  }

  /* tile data starts at data_off, followed by offset/count arrays, then IFD */
  toff_off = data_off + (size_t)ntiles * tile_bytes;
  toff_off = (toff_off + 7u) & ~(size_t)7u; /* align to 8 */
  tbc_off = toff_off + (size_t)ntiles * 8u;
  ifd_off = tbc_off + (size_t)ntiles * 8u;
  ifd_off = (ifd_off + 7u) & ~(size_t)7u; /* align to 8 */
  total = ifd_off + 8u + (size_t)NENT * 20u + 8u;
  buf = (uint8_t *)calloc(1, total);

  /* Write header */
  buf[0] = 'I'; buf[1] = 'I';
  tdt_pu16(buf + 2, 43, 0);
  tdt_pu16(buf + 4, 8, 0);
  tdt_pu16(buf + 6, 0, 0);
  tdt_pu64(buf + 8, ifd_off, 0);

  /* Write tiles (zero-padded to full tile dims) */
  for (t = 0; t < ntiles; t++) {
    uint32_t tx = (t % across) * TW;
    uint32_t ty = (t / across) * TL;
    uint32_t tw = TW, th = TL;
    uint32_t x, y;
    if (tx + tw > W) tw = W - tx;
    if (ty + th > H) th = H - ty;
    for (y = 0; y < th; y++) {
      for (x = 0; x < tw; x++) {
        uint32_t gx = tx + x, gy = ty + y;
        buf[data_off + (size_t)t * tile_bytes + (size_t)y * TW + x] =
            img[gy * W + gx];
      }
    }
  }

  /* Write tile offset and byte count arrays */
  for (t = 0; t < ntiles; t++) {
    uint64_t off = data_off + (size_t)t * tile_bytes;
    uint64_t bc = tile_bytes;
    tdt_pu64(buf + toff_off + (size_t)t * 8u, off, 0);
    tdt_pu64(buf + tbc_off + (size_t)t * 8u, bc, 0);
  }

  /* IFD */
  tdt_pu64(buf + ifd_off, (uint64_t)NENT, 0);
  e = buf + ifd_off + 8u;
#define BTENT(tag, type, cnt, val)             \
  do {                                        \
    tdt_pu16(e + n * 20 + 0, (tag), 0);       \
    tdt_pu16(e + n * 20 + 2, (type), 0);      \
    tdt_pu64(e + n * 20 + 4, (cnt), 0);       \
    tdt_pu64(e + n * 20 + 12, (val), 0);      \
    n++;                                      \
  } while (0)
  BTENT(256, 4, 1, W);
  BTENT(257, 4, 1, H);
  BTENT(258, 3, 1, 8);
  BTENT(259, 3, 1, 1);
  BTENT(262, 3, 1, 1);
  BTENT(322, 4, 1, TW);  /* TileWidth */
  BTENT(323, 4, 1, TL);  /* TileLength */
  BTENT(324, 16, ntiles, toff_off); /* TileOffsets LONG8 */
  BTENT(325, 16, ntiles, tbc_off); /* TileByteCounts LONG8 */
  BTENT(339, 3, 1, 1);   /* SampleFormat UINT */
#undef BTENT
  tdt_pu64(buf + ifd_off + 8u + (size_t)NENT * 20u, 0, 0);

  if (tinydng_open_memory(ctx, buf, total, NULL, &doc, &err) == TINYDNG_OK &&
      tinydng_decode_image(ctx, doc, 0, NULL, &px, &err) == TINYDNG_OK) {
    CHECK(px.width == W && px.height == H && px.size == img_bytes,
          "bigtiff tiled dims");
    CHECK(memcmp(px.data, img, img_bytes) == 0, "bigtiff tiled pixels");
    printf("  BigTIFF tiled %ux%u (%ux%u tiles, %u total) decoded OK\n",
           W, H, TW, TL, ntiles);
    tinydng_pixels_free(ctx, &px);
    tinydng_document_destroy(ctx, doc);
  } else {
    CHECK(0, "bigtiff tiled open/decode: %s", err.message);
    rc = 1;
  }
  free(buf);
  free(img);
  return rc;
}

/* BigTIFF with LONG (type 4) strip offsets instead of LONG8 (type 16).
   Verifies the reader handles mixed classic values inside a BigTIFF file. */
static int test_bigtiff_long_offsets(tinydng_context *ctx) {
  uint32_t w = 20, h = 16, i;
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
    img[i] = (uint8_t)((i * 53u + 11u) & 0xffu);
  }
  ifd_off = strip_off + strip_len + (strip_len & 1u);
  total = ifd_off + 8u + (size_t)NENT * 20u + 8u;
  buf = (uint8_t *)calloc(1, total);
  buf[0] = 'I'; buf[1] = 'I';
  tdt_pu16(buf + 2, 43, 0);
  tdt_pu16(buf + 4, 8, 0);
  tdt_pu16(buf + 6, 0, 0);
  tdt_pu64(buf + 8, ifd_off, 0);
  memcpy(buf + strip_off, img, strip_len);
  tdt_pu64(buf + ifd_off, (uint64_t)NENT, 0);
  e = buf + ifd_off + 8u;
  /* Use LONG (type 4) for strip offsets/counts — classic value type in a
     BigTIFF container. The reader must accept both. */
#define BENT_L(tag, cnt, val)                    \
  do {                                           \
    tdt_pu16(e + n * 20 + 0, (tag), 0);          \
    tdt_pu16(e + n * 20 + 2, 4, 0);  /* LONG */ \
    tdt_pu64(e + n * 20 + 4, (cnt), 0);          \
    tdt_pu64(e + n * 20 + 12, (val), 0);         \
    n++;                                         \
  } while (0)
  BENT_L(256, 1, w);
  BENT_L(257, 1, h);
  BENT_L(258, 1, 8);     /* BitsPerSample SHORT */
  BENT_L(259, 1, 1);     /* Compression NONE SHORT */
  BENT_L(262, 1, 1);     /* Photometric MINISBLACK SHORT */
  BENT_L(273, 1, strip_off);  /* StripOffsets LONG */
  BENT_L(277, 1, 1);     /* SamplesPerPixel SHORT */
  BENT_L(278, 1, h);     /* RowsPerStrip LONG */
  BENT_L(279, 1, strip_len);  /* StripByteCounts LONG */
#undef BENT_L
  tdt_pu64(buf + ifd_off + 8u + (size_t)NENT * 20u, 0, 0);

  if (tinydng_open_memory(ctx, buf, total, NULL, &doc, &err) == TINYDNG_OK &&
      tinydng_decode_image(ctx, doc, 0, NULL, &px, &err) == TINYDNG_OK) {
    CHECK(px.width == w && px.height == h && px.size == strip_len,
          "bigtiff long-offsets dims");
    CHECK(memcmp(px.data, img, strip_len) == 0, "bigtiff long-offsets pixels");
    printf("  BigTIFF LONG-offsets %ux%u decoded OK\n", w, h);
    tinydng_pixels_free(ctx, &px);
    tinydng_document_destroy(ctx, doc);
  } else {
    CHECK(0, "bigtiff long-offsets open/decode: %s", err.message);
    rc = 1;
  }
  free(buf);
  free(img);
  return rc;
}

/* BigTIFF with predictor=2 (horizontal differencing). */
static int test_bigtiff_predictor2(tinydng_context *ctx) {
  uint32_t w = 40, h = 20, x, y;
  uint16_t *orig = (uint16_t *)malloc((size_t)w * h * 2);
  uint8_t *strip = (uint8_t *)malloc((size_t)w * h * 2);
  size_t strip_len = (size_t)w * h * 2;
  size_t strip_off = 16, ifd_off, total;
  uint8_t *buf, *e;
  int n = 0;
  tinydng_error err;
  tinydng_document *doc = NULL;
  tinydng_pixels px;
  int rc = 0;
  const int NENT = 10;

  for (y = 0; y < h; y++) {
    for (x = 0; x < w; x++) {
      orig[y * w + x] = (uint16_t)((x * 311u + y * 1009u + 42u) & 0xffffu);
    }
  }
  /* Encode horizontal differences. */
  for (y = 0; y < h; y++) {
    uint16_t prev = 0;
    for (x = 0; x < w; x++) {
      uint16_t cur = orig[y * w + x];
      tdt_pu16(strip + ((size_t)y * w + x) * 2u, (uint16_t)(cur - prev), 0);
      prev = cur;
    }
  }

  ifd_off = strip_off + strip_len + (strip_len & 1u);
  total = ifd_off + 8u + (size_t)NENT * 20u + 8u;
  buf = (uint8_t *)calloc(1, total);
  buf[0] = 'I'; buf[1] = 'I';
  tdt_pu16(buf + 2, 43, 0);
  tdt_pu16(buf + 4, 8, 0);
  tdt_pu16(buf + 6, 0, 0);
  tdt_pu64(buf + 8, ifd_off, 0);
  memcpy(buf + strip_off, strip, strip_len);
  tdt_pu64(buf + ifd_off, (uint64_t)NENT, 0);
  e = buf + ifd_off + 8u;
#define BENT_P(tag, type, cnt, val)            \
  do {                                         \
    tdt_pu16(e + n * 20 + 0, (tag), 0);        \
    tdt_pu16(e + n * 20 + 2, (type), 0);       \
    tdt_pu64(e + n * 20 + 4, (cnt), 0);        \
    tdt_pu64(e + n * 20 + 12, (val), 0);       \
    n++;                                       \
  } while (0)
  BENT_P(256, 4, 1, w);
  BENT_P(257, 4, 1, h);
  BENT_P(258, 3, 1, 16);
  BENT_P(259, 3, 1, 1);
  BENT_P(262, 3, 1, 1);
  BENT_P(273, 16, 1, strip_off);
  BENT_P(277, 3, 1, 1);
  BENT_P(278, 4, 1, h);
  BENT_P(279, 16, 1, strip_len);
  BENT_P(317, 3, 1, 2);  /* Predictor = 2 */
#undef BENT_P
  tdt_pu64(buf + ifd_off + 8u + (size_t)NENT * 20u, 0, 0);

  if (tinydng_open_memory(ctx, buf, total, NULL, &doc, &err) == TINYDNG_OK &&
      tinydng_decode_image(ctx, doc, 0, NULL, &px, &err) == TINYDNG_OK) {
    CHECK(px.width == w && px.height == h && px.size == strip_len,
          "bigtiff predictor2 dims");
    CHECK(memcmp(px.data, orig, strip_len) == 0, "bigtiff predictor2 pixels");
    printf("  BigTIFF predictor 2 %ux%u decoded OK\n", w, h);
    tinydng_pixels_free(ctx, &px);
    tinydng_document_destroy(ctx, doc);
  } else {
    CHECK(0, "bigtiff predictor2 open/decode: %s", err.message);
    rc = 1;
  }
  free(buf);
  free(strip);
  free(orig);
  return rc;
}

/* BigTIFF with a SubIFD: IFD0 has SubIFD tag 330 pointing to a second IFD
   with the strip data. Exercises the SubIFD parser inside a BigTIFF file. */
static int test_bigtiff_subifds(tinydng_context *ctx) {
  uint32_t w = 24, h = 18, i;
  size_t strip_len = (size_t)w * h;
  uint8_t *img = (uint8_t *)malloc(strip_len);
  /* IFD0: 8 entries (incl. SubIFD tag 330). SubIFD: 7 entries. */
  const int N0 = 8, N1 = 7;
  size_t strip_off = 16;
  size_t sub_ifd_off, ifd0_off, total;
  uint8_t *buf, *e;
  int n;
  tinydng_error err;
  tinydng_document *doc = NULL;
  tinydng_pixels px;
  int rc = 0;
  tinydng_open_options opts;

  for (i = 0; i < strip_len; i++) {
    img[i] = (uint8_t)((i * 37u + 19u) & 0xffu);
  }

  /* Layout: header(16) | strip(1296) | pad | subIFD(8+N1*20+8) | IFD0(8+N0*20+8) */
  sub_ifd_off = strip_off + strip_len + (strip_len & 1u);
  ifd0_off = sub_ifd_off + 8u + (size_t)N1 * 20u + 8u;
  total = ifd0_off + 8u + (size_t)N0 * 20u + 8u;
  buf = (uint8_t *)calloc(1, total);

  /* Header */
  buf[0] = 'I'; buf[1] = 'I';
  tdt_pu16(buf + 2, 43, 0);
  tdt_pu16(buf + 4, 8, 0);
  tdt_pu16(buf + 6, 0, 0);
  tdt_pu64(buf + 8, ifd0_off, 0);
  memcpy(buf + strip_off, img, strip_len);

  /* SubIFD (with strip data) */
  n = 0;
  tdt_pu64(buf + sub_ifd_off, (uint64_t)N1, 0);
  e = buf + sub_ifd_off + 8u;
#define SIFD(tag, type, cnt, val)              \
  do {                                         \
    tdt_pu16(e + n * 20 + 0, (tag), 0);        \
    tdt_pu16(e + n * 20 + 2, (type), 0);       \
    tdt_pu64(e + n * 20 + 4, (cnt), 0);        \
    tdt_pu64(e + n * 20 + 12, (val), 0);       \
    n++;                                       \
  } while (0)
  SIFD(256, 4, 1, w);
  SIFD(257, 4, 1, h);
  SIFD(258, 3, 1, 8);
  SIFD(259, 3, 1, 1);
  SIFD(262, 3, 1, 1);
  SIFD(273, 16, 1, strip_off);
  SIFD(279, 16, 1, strip_len);
#undef SIFD
  tdt_pu64(buf + sub_ifd_off + 8u + (size_t)N1 * 20u, 0, 0);

  /* IFD0 (minimal: points to SubIFD, has NewSubfileType=4 for SubIFD) */
  n = 0;
  tdt_pu64(buf + ifd0_off, (uint64_t)N0, 0);
  e = buf + ifd0_off + 8u;
#define IFD0(tag, type, cnt, val)              \
  do {                                         \
    tdt_pu16(e + n * 20 + 0, (tag), 0);        \
    tdt_pu16(e + n * 20 + 2, (type), 0);       \
    tdt_pu64(e + n * 20 + 4, (cnt), 0);        \
    tdt_pu64(e + n * 20 + 12, (val), 0);       \
    n++;                                       \
  } while (0)
  IFD0(256, 4, 1, w);
  IFD0(257, 4, 1, h);
  IFD0(258, 3, 1, 8);
  IFD0(259, 3, 1, 1);
  IFD0(262, 3, 1, 1);
  IFD0(273, 16, 1, strip_off);  /* IFD0 also points at the strip (as fallback) */
  IFD0(279, 16, 1, strip_len);
  IFD0(330, 18, 1, sub_ifd_off);  /* SubIFDs tag, type IFD8, count 1 */
#undef IFD0
  tdt_pu64(buf + ifd0_off + 8u + (size_t)N0 * 20u, 0, 0);

  memset(&opts, 0, sizeof(opts));
  opts.flags = TINYDNG_OPEN_PARSE_SUBIFDS;
  if (tinydng_open_memory(ctx, buf, total, &opts, &doc, &err) == TINYDNG_OK &&
      tinydng_decode_image(ctx, doc, 0, NULL, &px, &err) == TINYDNG_OK) {
    CHECK(px.width == w && px.height == h && px.size == strip_len,
          "bigtiff subifds dims");
    CHECK(memcmp(px.data, img, strip_len) == 0, "bigtiff subifds pixels");
    printf("  BigTIFF SubIFDs %ux%u decoded OK\n", w, h);
    tinydng_pixels_free(ctx, &px);
    tinydng_document_destroy(ctx, doc);
  } else {
    CHECK(0, "bigtiff subifds open/decode: %s", err.message);
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

/* Packed sub-byte samples (12-bit, MSB-first, row byte-aligned): the
   stripped bytes are decoded to 16-bit samples. */
static int test_packed12(tinydng_context *ctx) {
  uint32_t w = 37, h = 9; /* row = 37*12 bits = 55.5 bytes -> 56 bytes */
  size_t samples = (size_t)w * h;
  size_t row_bytes = ((size_t)w * 12u + 7u) / 8u;
  size_t strip_len = row_bytes * h;
  uint16_t *orig = (uint16_t *)malloc(samples * sizeof(uint16_t));
  uint8_t *strip = (uint8_t *)calloc(1, strip_len);
  uint8_t *buf;
  size_t total;
  size_t s, y, x;
  tinydng_error err;
  tinydng_document *doc = NULL;
  tinydng_pixels px;
  int rc = 0;

  for (s = 0; s < samples; s++) {
    orig[s] = (uint16_t)((s * 0x2C7u + 0x1Au) & 0xFFFu);
  }
  /* Pack MSB-first into the strip. */
  for (y = 0; y < h; y++) {
    size_t bitpos = 0;
    for (x = 0; x < w; x++) {
      uint16_t v = orig[y * w + x];
      int k;
      for (k = 11; k >= 0; k--) {
        size_t byte_i = bitpos >> 3;
        size_t bit_i = 7u - (bitpos & 7u);
        strip[y * row_bytes + byte_i] |=
            (uint8_t)(((v >> k) & 1u) << bit_i);
        bitpos++;
      }
    }
  }
  buf = make_classic(w, h, 12, TINYDNG_SAMPLEFORMAT_UINT, 1,
                     TINYDNG_COMPRESSION_NONE, strip, strip_len, &total);
  if (tinydng_open_memory(ctx, buf, total, NULL, &doc, &err) == TINYDNG_OK &&
      tinydng_decode_image(ctx, doc, 0, NULL, &px, &err) == TINYDNG_OK) {
    CHECK(px.bits_per_sample == 16 && px.size == samples * 2u,
          "packed 12-bit dims");
    CHECK(memcmp(px.data, orig, samples * sizeof(uint16_t)) == 0,
          "packed 12-bit samples");
    printf("  packed 12-bit (%ux%u) OK\n", w, h);
    tinydng_pixels_free(ctx, &px);

    /* KEEP_PACKED: output must be the raw packed bytes, bit-identical to the
       source strip (predictor already applied at the sample level). */
    {
      tinydng_decode_options opts;
      memset(&opts, 0, sizeof(opts));
      opts.flags = TINYDNG_DEC_KEEP_PACKED;
      if (tinydng_decode_image(ctx, doc, 0, &opts, &px, &err) == TINYDNG_OK) {
        CHECK(px.bits_per_sample == 12 && px.size == strip_len,
              "keep-packed dims");
        CHECK(memcmp(px.data, strip, strip_len) == 0,
              "keep-packed bytes match stored strip");
        printf("  keep-packed (%ux%u) OK\n", w, h);
        tinydng_pixels_free(ctx, &px);

        /* Sub-window KEEP_PACKED must bit-copy the correct sample range. */
        {
          uint32_t rx = 3, ry = 2, rw = 10, rh = 4;
          tinydng_pixels rpx;
          size_t rrow = ((size_t)rw * 12u + 7u) / 8u;
          size_t rsize = rrow * rh;
          uint8_t *exp = (uint8_t *)calloc(1, rsize);
          size_t yy, xx;
          for (yy = 0; yy < rh; yy++) {
            size_t bitpos = 0;
            for (xx = 0; xx < rw; xx++) {
              uint16_t v = orig[(ry + yy) * w + (rx + xx)];
              int k;
              for (k = 11; k >= 0; k--) {
                size_t byte_i = bitpos >> 3;
                size_t bit_i = 7u - (bitpos & 7u);
                exp[yy * rrow + byte_i] |=
                    (uint8_t)(((v >> k) & 1u) << bit_i);
                bitpos++;
              }
            }
          }
          if (tinydng_decode_region(ctx, doc, 0, rx, ry, rw, rh, &opts, &rpx,
                                    &err) == TINYDNG_OK) {
            CHECK(rpx.bits_per_sample == 12 && rpx.size == rsize,
                  "keep-packed region dims");
            CHECK(memcmp(rpx.data, exp, rsize) == 0,
                  "keep-packed region bytes");
            printf("  keep-packed region (%ux%u @ %u,%u) OK\n", rw, rh, rx, ry);
            tinydng_pixels_free(ctx, &rpx);
          } else {
            CHECK(0, "keep-packed region decode: %s", err.message);
            rc = 1;
          }
          free(exp);
        }
      } else {
        CHECK(0, "keep-packed decode: %s", err.message);
        rc = 1;
      }
    }
    tinydng_document_destroy(ctx, doc);
  } else {
    CHECK(0, "packed12 open/decode: %s", err.message);
    rc = 1;
  }
  free(buf);
  free(strip);
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

/* Direct LJPEG v2 API check: a non-zero skipLength must be rejected (it would
   otherwise write past the target buffer), while skipLength=0 decodes normally.
   Uses a self-contained LJPEG stream produced by the encoder. */
static int test_lj92_skipLength(tinydng_context *ctx) {
  (void)ctx;
  const int w = 16, h = 16, comps = 1, bits = 12;
  uint16_t *img = (uint16_t *)malloc((size_t)w * h * sizeof(uint16_t));
  uint8_t *enc = NULL;
  int encLen = 0;
  tdng_lj92 lj = NULL;
  int dw = 0, dh = 0, dbits = 0, dcomps = 0;
  uint16_t *target = NULL;
  int rc = 0;
  size_t need, i;

  if (!img) {
    CHECK(0, "lj92 image alloc");
    return 1;
  }
  for (i = 0; i < (size_t)w * h; i++) {
    img[i] = (uint16_t)((i * 0x2C7u + 0x1Au) & 0xFFFu);
  }
  if (tdng_lj92_encode_ex(img, w, h, bits, comps, 1, w, 0, NULL, 0, &enc,
                          &encLen) != TDNG_LJ92_ERROR_NONE ||
      enc == NULL || encLen <= 0) {
    CHECK(0, "lj92 encode failed");
    free(img);
    return 1;
  }
  if (tdng_lj92_open(&lj, enc, encLen, &dw, &dh, &dbits, &dcomps) !=
          TDNG_LJ92_ERROR_NONE ||
      dw != w || dh != h || dcomps != comps) {
    CHECK(0, "lj92 open failed (dw=%d dh=%d comps=%d)", dw, dh, dcomps);
    rc = 1;
    goto done;
  }
  need = (size_t)dw * (size_t)dcomps * (size_t)dh;
  if (need > SIZE_MAX / 2u) {
    CHECK(0, "lj92 dims overflow");
    rc = 1;
    goto done;
  }
  target = (uint16_t *)malloc(need * 2u);
  if (!target) {
    CHECK(0, "lj92 target alloc");
    rc = 1;
    goto done;
  }

  /* skipLength != 0 must be rejected (no out-of-bounds write). */
  {
    int r = tdng_lj92_decode(lj, target, (int)((size_t)dw * dcomps), 1, NULL, 0);
    CHECK(r == TDNG_LJ92_ERROR_CORRUPT,
          "lj92 rejects skipLength=1 (got %d)", r);
  }

  /* skipLength == 0 must decode successfully and round-trip the samples. */
  {
    int r = tdng_lj92_decode(lj, target, (int)((size_t)dw * dcomps), 0, NULL, 0);
    CHECK(r == TDNG_LJ92_ERROR_NONE, "lj92 decodes skipLength=0 (got %d)", r);
    if (r == TDNG_LJ92_ERROR_NONE) {
      size_t n = (size_t)w * h;
      size_t bad = 0;
      for (i = 0; i < n; i++) {
        if (target[i] != img[i]) {
          bad = 1;
          break;
        }
      }
      CHECK(bad == 0u, "lj92 round-trip mismatch");
    }
  }

  printf("  lj92 skipLength rejection + round-trip OK\n");

done:
  if (lj) tdng_lj92_close(lj);
  free(target);
  free(enc);
  free(img);
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
  test_bigtiff_be(ctx);
  test_bigtiff_tiled(ctx);
  test_bigtiff_long_offsets(ctx);
  test_bigtiff_predictor2(ctx);
  test_bigtiff_subifds(ctx);
  test_predictor2(ctx);
  test_predictor3(ctx);
  test_packed12(ctx);
  test_lj92_skipLength(ctx);
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
