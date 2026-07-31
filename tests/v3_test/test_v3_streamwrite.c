/* Streaming writer tests (tinydng_writer_*): tiled and multi-strip
 * round-trips across compression modes and byte orders, byte-parity of
 * tinydng_write_memory with the streaming writer, and error paths.
 * The read-back decodes must reproduce the source pixels exactly.
 */
#include "td_test_util.h"

static tinydng_context *g_ctx;
static tinydng_error g_err;

/* Deterministic 16-bit sample source. */
static void fill16(uint16_t *p, size_t n, uint32_t seed) {
  size_t i;
  for (i = 0; i < n; i++) {
    p[i] = (uint16_t)((i * 0x9E3779B9u + seed * 0x85EBCA6Bu) & 0xFFFFu);
  }
}

static void fill8(uint8_t *p, size_t n, uint32_t seed) {
  size_t i;
  for (i = 0; i < n; i++) {
    p[i] = (uint8_t)((i * 97u + seed * 13u) & 0xFFu);
  }
}

/* Decode image 0 of a memory blob and compare to `expected` (byte-wise). */
static int roundtrip_ok(const uint8_t *blob, size_t blob_len,
                        const uint8_t *expected, size_t expected_size) {
  tinydng_document *doc = NULL;
  tinydng_pixels px;
  tinydng_status st;
  int ok = 0;
  st = tinydng_open_memory(g_ctx, blob, blob_len, NULL, &doc, &g_err);
  if (st != TINYDNG_OK) {
    fprintf(stderr, "  (open: %s)\n", g_err.message);
    return 0;
  }
  st = tinydng_decode_image(g_ctx, doc, 0, NULL, &px, &g_err);
  if (st != TINYDNG_OK) {
    fprintf(stderr, "  (decode: %s)\n", g_err.message);
  } else if (px.size != expected_size ||
             memcmp(px.data, expected, expected_size) != 0) {
    fprintf(stderr, "  (pixel mismatch: got %zu want %zu)\n", px.size,
            expected_size);
  } else {
    ok = 1;
  }
  if (st == TINYDNG_OK) {
    tinydng_pixels_free(g_ctx, &px);
  }
  tinydng_document_destroy(g_ctx, doc);
  return ok;
}

/* ------------------------------------------------------------------ */
/* Tiled round-trip                                                    */
/* ------------------------------------------------------------------ */

static int test_tiled(uint16_t comp, int be, int as_dng,
                      const tinydng_cfa *cfa, const tinydng_raw_info *raw) {
  const uint32_t W = 33, H = 17;
  const uint32_t TW = 16, TL = 8;
  const uint32_t across = (W + TW - 1u) / TW;
  const uint32_t down = (H + TL - 1u) / TL;
  const uint16_t bps = (comp == TINYDNG_COMPRESSION_NEW_JPEG) ? 16u : 8u;
  const uint16_t spp = (as_dng && cfa) ? 1u : 3u;
  size_t img_bytes = (size_t)W * H * spp * (bps / 8u);
  uint8_t *img = (uint8_t *)malloc(img_bytes);
  uint8_t *tile_px = (uint8_t *)malloc((size_t)TW * TL * spp * (bps / 8u));
  tinydng_write_io io;
  tinydng_writer *w = NULL;
  tinydng_write_image meta;
  tinydng_write_options opts;
  tinydng_tiling tiling;
  uint32_t t;
  int ok = 1;

  if (!img || !tile_px) {
    free(img);
    free(tile_px);
    return 0;
  }
  if (bps == 8u) {
    fill8(img, img_bytes, (uint32_t)comp * 31u + (uint32_t)be * 7u);
  } else {
    fill16((uint16_t *)img, img_bytes / 2u,
           (uint32_t)comp * 131u + (uint32_t)be * 17u);
  }
  memset(&meta, 0, sizeof(meta));
  meta.width = W;
  meta.height = H;
  meta.samples_per_pixel = spp;
  meta.bits_per_sample = bps;
  meta.sample_format = TINYDNG_SAMPLEFORMAT_UINT;
  meta.cfa = cfa;
  meta.raw = raw;
  memset(&opts, 0, sizeof(opts));
  opts.big_endian = (uint8_t)be;
  opts.as_dng = (uint8_t)as_dng;
  opts.compression = comp;
  memset(&tiling, 0, sizeof(tiling));
  tiling.tile_width = TW;
  tiling.tile_length = TL;

  if (tinydng_write_io_open_memory(g_ctx, &io, &g_err) != TINYDNG_OK ||
      tinydng_writer_create(g_ctx, io, &meta, &opts, &tiling, &w, &g_err) !=
          TINYDNG_OK) {
    CHECK(0, "tiled create: %s", g_err.message);
    free(img);
    free(tile_px);
    return 0;
  }
  for (t = 0; t < across * down; t++) {
    uint32_t x = (t % across) * TW;
    uint32_t y = (t / across) * TL;
    uint32_t pw = (x + TW <= W) ? TW : (W - x);
    uint32_t ph = (y + TL <= H) ? TL : (H - y);
    size_t row_bytes = (size_t)W * spp * (bps / 8u);
    size_t trow_bytes = (size_t)pw * spp * (bps / 8u);
    uint32_t r;
    for (r = 0; r < ph; r++) {
      memcpy(tile_px + (size_t)r * trow_bytes,
             img + ((size_t)(y + r) * W + x) * spp * (bps / 8u), trow_bytes);
    }
    if (tinydng_writer_write_tile(w, t, tile_px, &g_err) != TINYDNG_OK) {
      CHECK(0, "tile %u write: %s", t, g_err.message);
      ok = 0;
      break;
    }
  }
  if (ok) {
    if (tinydng_writer_finish(w, &g_err) != TINYDNG_OK) {
      CHECK(0, "tiled finish: %s", g_err.message);
      ok = 0;
    }
  } else {
    tinydng_writer_finish(w, &g_err);
  }
  if (ok) {
    uint8_t *blob;
    size_t blob_len;
    if (tinydng_write_io_memory_take(g_ctx, &io, &blob, &blob_len, &g_err) ==
        TINYDNG_OK) {
      ok = roundtrip_ok(blob, blob_len, img, img_bytes);
      tinydng_buffer_free(g_ctx, blob);
    }
  }
  io.close(&io);
  free(img);
  free(tile_px);
  return ok;
}

/* ------------------------------------------------------------------ */
/* Multi-strip round-trip                                              */
/* ------------------------------------------------------------------ */

static int test_strips(uint16_t comp, int be, uint32_t rps) {
  const uint32_t W = 40, H = 13;
  const uint16_t bps = (comp == TINYDNG_COMPRESSION_NEW_JPEG) ? 16u : 8u;
  const uint16_t spp = 1;
  const uint32_t n_strips = (H + rps - 1u) / rps;
  size_t img_bytes = (size_t)W * H * spp * (bps / 8u);
  uint8_t *img = (uint8_t *)malloc(img_bytes);
  uint8_t *strip_px = (uint8_t *)malloc((size_t)W * rps * spp * (bps / 8u));
  tinydng_write_io io;
  tinydng_writer *w = NULL;
  tinydng_write_image meta;
  tinydng_write_options opts;
  tinydng_tiling tiling;
  uint32_t s;
  int ok = 1;

  if (!img || !strip_px) {
    free(img);
    free(strip_px);
    return 0;
  }
  if (bps == 8u) {
    fill8(img, img_bytes, 99u);
  } else {
    fill16((uint16_t *)img, img_bytes / 2u, 77u);
  }
  memset(&meta, 0, sizeof(meta));
  meta.width = W;
  meta.height = H;
  meta.samples_per_pixel = spp;
  meta.bits_per_sample = bps;
  meta.sample_format = TINYDNG_SAMPLEFORMAT_UINT;
  memset(&opts, 0, sizeof(opts));
  opts.big_endian = (uint8_t)be;
  opts.compression = comp;
  memset(&tiling, 0, sizeof(tiling));
  tiling.rows_per_strip = rps;

  if (tinydng_write_io_open_memory(g_ctx, &io, &g_err) != TINYDNG_OK ||
      tinydng_writer_create(g_ctx, io, &meta, &opts, &tiling, &w, &g_err) !=
          TINYDNG_OK) {
    CHECK(0, "strips create: %s", g_err.message);
    free(img);
    free(strip_px);
    return 0;
  }
  for (s = 0; s < n_strips; s++) {
    uint32_t y = s * rps;
    uint32_t ph = (y + rps <= H) ? rps : (H - y);
    memcpy(strip_px, img + (size_t)y * W * spp * (bps / 8u),
           (size_t)W * ph * spp * (bps / 8u));
    if (tinydng_writer_write_strip(w, s, strip_px, &g_err) != TINYDNG_OK) {
      CHECK(0, "strip %u write: %s", s, g_err.message);
      ok = 0;
      break;
    }
  }
  if (ok) {
    if (tinydng_writer_finish(w, &g_err) != TINYDNG_OK) {
      CHECK(0, "strips finish: %s", g_err.message);
      ok = 0;
    }
  } else {
    tinydng_writer_finish(w, &g_err);
  }
  if (ok) {
    uint8_t *blob;
    size_t blob_len;
    if (tinydng_write_io_memory_take(g_ctx, &io, &blob, &blob_len, &g_err) ==
        TINYDNG_OK) {
      ok = roundtrip_ok(blob, blob_len, img, img_bytes);
      tinydng_buffer_free(g_ctx, blob);
    }
  }
  io.close(&io);
  free(img);
  free(strip_px);
  return ok;
}

/* ------------------------------------------------------------------ */
/* write_memory byte parity                                            */
/* ------------------------------------------------------------------ */

static int test_memory_parity(uint16_t comp, int be, int as_dng) {
  const uint32_t W = 29, H = 11;
  const uint16_t bps = (comp == TINYDNG_COMPRESSION_NEW_JPEG) ? 16u : 8u;
  const uint16_t spp = 1;
  size_t img_bytes = (size_t)W * H * spp * (bps / 8u);
  uint8_t *img = (uint8_t *)malloc(img_bytes);
  tinydng_write_image meta;
  tinydng_write_options opts;
  tinydng_write_io io;
  tinydng_writer *w = NULL;
  uint8_t *ref = NULL, *got = NULL;
  size_t ref_len = 0, got_len = 0;
  int ok = 1;

  if (!img) {
    return 0;
  }
  fill16((uint16_t *)img, img_bytes / 2u, 1234u);
  memset(&meta, 0, sizeof(meta));
  meta.width = W;
  meta.height = H;
  meta.samples_per_pixel = spp;
  meta.bits_per_sample = bps;
  meta.sample_format = TINYDNG_SAMPLEFORMAT_UINT;
  meta.data = img;
  meta.data_size = img_bytes;
  memset(&opts, 0, sizeof(opts));
  opts.big_endian = (uint8_t)be;
  opts.as_dng = (uint8_t)as_dng;
  opts.compression = comp;

  if (tinydng_write_memory(g_ctx, &meta, &opts, &ref, &ref_len, &g_err) !=
      TINYDNG_OK) {
    CHECK(0, "write_memory: %s", g_err.message);
    free(img);
    return 0;
  }
  if (tinydng_write_io_open_memory(g_ctx, &io, &g_err) != TINYDNG_OK ||
      tinydng_writer_create(g_ctx, io, &meta, &opts, NULL, &w, &g_err) !=
          TINYDNG_OK) {
    CHECK(0, "writer create: %s", g_err.message);
    tinydng_buffer_free(g_ctx, ref);
    free(img);
    return 0;
  }
  if (tinydng_writer_write_strip(w, 0, img, &g_err) != TINYDNG_OK ||
      tinydng_writer_finish(w, &g_err) != TINYDNG_OK) {
    CHECK(0, "writer strip/finish: %s", g_err.message);
    tinydng_buffer_free(g_ctx, ref);
    io.close(&io);
    free(img);
    return 0;
  }
  if (tinydng_write_io_memory_take(g_ctx, &io, &got, &got_len, &g_err) ==
      TINYDNG_OK) {
    if (ref_len != got_len || memcmp(ref, got, ref_len) != 0) {
      CHECK(0, "write_memory != writer output (len %zu vs %zu)", ref_len,
            got_len);
      ok = 0;
    } else {
      CHECK(1, "write_memory byte-identical to writer (%zu bytes)", ref_len);
    }
    tinydng_buffer_free(g_ctx, got);
  }
  io.close(&io);
  tinydng_buffer_free(g_ctx, ref);
  free(img);
  return ok;
}

/* ------------------------------------------------------------------ */
/* write_file round-trip                                               */
/* ------------------------------------------------------------------ */

static int test_write_file(uint16_t comp) {
  const uint32_t W = 24, H = 9;
  const uint16_t bps = (comp == TINYDNG_COMPRESSION_NEW_JPEG) ? 16u : 8u;
  const uint16_t spp = 3;
  size_t img_bytes = (size_t)W * H * spp * (bps / 8u);
  uint8_t *img = (uint8_t *)malloc(img_bytes);
  tinydng_write_image meta;
  tinydng_write_options opts;
  const char *path = "test_v3_streamwrite_out.tiff";
  int ok = 1;

  if (!img) {
    return 0;
  }
  fill8(img, img_bytes, 5u);
  memset(&meta, 0, sizeof(meta));
  meta.width = W;
  meta.height = H;
  meta.samples_per_pixel = spp;
  meta.bits_per_sample = bps;
  meta.sample_format = TINYDNG_SAMPLEFORMAT_UINT;
  meta.data = img;
  meta.data_size = img_bytes;
  memset(&opts, 0, sizeof(opts));
  opts.compression = comp;
  if (tinydng_write_file(g_ctx, path, &meta, &opts, &g_err) != TINYDNG_OK) {
    CHECK(0, "write_file: %s", g_err.message);
    free(img);
    return 0;
  }
  {
    unsigned char *blob;
    size_t blob_len;
    blob = tdt_slurp(path, &blob_len);
    remove(path);
    if (!blob) {
      CHECK(0, "re-read %s failed", path);
      free(img);
      return 0;
    }
    ok = roundtrip_ok(blob, blob_len, img, img_bytes);
    free(blob);
  }
  CHECK(ok, "write_file -> open/decode round-trip (comp=%u)", comp);
  free(img);
  return ok;
}

/* ------------------------------------------------------------------ */
/* Error paths                                                         */
/* ------------------------------------------------------------------ */

/* Sink that fails after `limit` total bytes. */
typedef struct fail_sink {
  size_t limit;
  size_t written;
} fail_sink;

static size_t fail_sink_write(tinydng_write_io *io, uint64_t off,
                              const void *data, size_t len) {
  fail_sink *f = (fail_sink *)io->backend;
  (void)off;
  (void)data;
  if (f->written >= f->limit) {
    return 0;
  }
  if (f->written + len > f->limit) {
    f->written = f->limit;
    return 0;
  }
  f->written += len;
  return len;
}

static void test_errors(void) {
  tinydng_write_image meta;
  tinydng_write_options opts;
  tinydng_tiling tiling;
  tinydng_write_io io;
  tinydng_writer *w = NULL;
  uint8_t px[64 * 64 * 3 * 2];
  fail_sink fs;
  tinydng_status st;
  size_t i;

  for (i = 0; i < sizeof(px); i++) {
    px[i] = (uint8_t)i;
  }
  memset(&meta, 0, sizeof(meta));
  meta.width = 64;
  meta.height = 64;
  meta.samples_per_pixel = 3;
  meta.bits_per_sample = 8;
  meta.sample_format = TINYDNG_SAMPLEFORMAT_UINT;
  memset(&opts, 0, sizeof(opts));
  opts.compression = TINYDNG_COMPRESSION_LZW;
  memset(&tiling, 0, sizeof(tiling));
  tiling.tile_width = 16;
  tiling.tile_length = 16;

  /* LJPEG with bps != 16 */
  opts.compression = TINYDNG_COMPRESSION_NEW_JPEG;
  if (tinydng_write_io_open_memory(g_ctx, &io, &g_err) == TINYDNG_OK) {
    st = tinydng_writer_create(g_ctx, io, &meta, &opts, &tiling, &w, &g_err);
    CHECK(st == TINYDNG_E_UNSUPPORTED, "LJPEG requires 16-bit");
    if (st == TINYDNG_OK) tinydng_writer_finish(w, &g_err);
    io.close(&io);
  }
  meta.bits_per_sample = 16;
  opts.compression = TINYDNG_COMPRESSION_LZW;

  if (tinydng_write_io_open_memory(g_ctx, &io, &g_err) != TINYDNG_OK ||
      tinydng_writer_create(g_ctx, io, &meta, &opts, &tiling, &w, &g_err) !=
          TINYDNG_OK) {
    CHECK(0, "create: %s", g_err.message);
    return;
  }
  st = tinydng_writer_write_strip(w, 0, px, &g_err);
  CHECK(st == TINYDNG_E_INVALID_ARG, "write_strip on tiled writer");
  st = tinydng_writer_write_tile(w, 16, px, &g_err); /* 4x4=16 tiles */
  CHECK(st == TINYDNG_E_INVALID_ARG, "tile index out of range");
  st = tinydng_writer_write_tile(w, 0, px, &g_err);
  CHECK(st == TINYDNG_OK, "tile 0 written");
  st = tinydng_writer_write_tile(w, 0, px, &g_err);
  CHECK(st == TINYDNG_E_INVALID_ARG, "tile written twice");
  st = tinydng_writer_finish(w, &g_err);
  /* finish releases the writer even on the validation-error path */
  CHECK(st == TINYDNG_E_INVALID_ARG, "finish with unwritten tiles");
  io.close(&io);

  /* failing sink => E_IO */
  memset(&fs, 0, sizeof(fs));
  fs.limit = 64;
  memset(&io, 0, sizeof(io));
  io.write = fail_sink_write;
  io.size = NULL;
  io.close = NULL;
  io.backend = &fs;
  st = tinydng_writer_create(g_ctx, io, &meta, &opts, NULL, &w, &g_err);
  CHECK(st == TINYDNG_OK, "create with failing sink");
  if (st == TINYDNG_OK) {
    st = tinydng_writer_write_strip(w, 0, px, &g_err);
    CHECK(st == TINYDNG_E_IO, "short sink -> E_IO");
    tinydng_writer_finish(w, &g_err);
  }

  /* use after finish */
  if (tinydng_write_io_open_memory(g_ctx, &io, &g_err) == TINYDNG_OK &&
      tinydng_writer_create(g_ctx, io, &meta, &opts, &tiling, &w, &g_err) ==
          TINYDNG_OK) {
    st = tinydng_writer_finish(w, &g_err);
    CHECK(st == TINYDNG_E_INVALID_ARG, "finish with unwritten tiles (2)");
    io.close(&io);
  }
}

/* ------------------------------------------------------------------ */

/* Real-world round trip: decode a corpus DNG's lossless image, re-encode
 * it as a tiled lossless-JPEG DNG through the streaming writer (feeding
 * tiles one at a time from the decoded pixels, carrying the source CFA/raw
 * metadata), then decode the output and require byte-identical pixels. */
static int test_real_corpus(const char *root) {
  char path[1024];
  tinydng_document *src_doc = NULL;
  tinydng_pixels px;
  const tinydng_image_info *img;
  tinydng_status st;
  int found = -1;
  int ok = 1;
  size_t i, n;

  snprintf(path, sizeof(path), "%s/pixel3.dng", root);
  {
    tinydng_open_options oopts;
    memset(&oopts, 0, sizeof(oopts));
    oopts.flags = TINYDNG_OPEN_PARSE_SUBIFDS;
    st = tinydng_open_file(g_ctx, path, &oopts, &src_doc, &g_err);
  }
  if (st != TINYDNG_OK) {
    CHECK(0, "corpus open %s: %s", path, g_err.message);
    return 0;
  }
  n = tinydng_image_count(src_doc);
  for (i = 0; i < n; i++) {
    const tinydng_image_info *im = tinydng_image_get(src_doc, i);
    if ((im->compression == TINYDNG_COMPRESSION_OLD_JPEG ||
         im->compression == TINYDNG_COMPRESSION_NEW_JPEG) &&
        im->bits_per_sample > 8u &&
        (found < 0 ||
         (uint64_t)im->width * im->height >
             (uint64_t)tinydng_image_get(src_doc, (size_t)found)->width *
                 tinydng_image_get(src_doc, (size_t)found)->height)) {
      found = (int)i;
    }
  }
  if (found < 0) {
    CHECK(0, "corpus: no lossless image in %s", path);
    tinydng_document_destroy(g_ctx, src_doc);
    return 0;
  }
  st = tinydng_decode_image(g_ctx, src_doc, (size_t)found, NULL, &px, &g_err);
  if (st != TINYDNG_OK) {
    CHECK(0, "corpus decode: %s", g_err.message);
    tinydng_document_destroy(g_ctx, src_doc);
    return 0;
  }
  img = tinydng_image_get(src_doc, (size_t)found);

  /* Re-encode as a tiled LJPEG DNG (256x256 tiles). */
  {
    tinydng_write_image meta;
    tinydng_write_options opts;
    tinydng_tiling tiling;
    tinydng_write_io io;
    tinydng_writer *w = NULL;
    const uint32_t TW = 256, TL = 256;
    const uint32_t across = (img->width + TW - 1u) / TW;
    const uint32_t down = (img->height + TL - 1u) / TL;
    const size_t sb = (size_t)px.bits_per_sample / 8u;
    uint8_t *tilebuf = (uint8_t *)malloc((size_t)TW * TL * px.samples_per_pixel * sb);
    uint8_t *blob = NULL;
    size_t blob_len = 0;
    uint32_t t;

    if (!tilebuf) {
      tinydng_pixels_free(g_ctx, &px);
      tinydng_document_destroy(g_ctx, src_doc);
      return 0;
    }
    memset(&meta, 0, sizeof(meta));
    meta.width = img->width;
    meta.height = img->height;
    meta.samples_per_pixel = img->samples_per_pixel;
    meta.bits_per_sample = px.bits_per_sample;
    meta.sample_format = TINYDNG_SAMPLEFORMAT_UINT;
    meta.cfa = &img->cfa;
    meta.raw = &img->raw;
    memset(&opts, 0, sizeof(opts));
    opts.as_dng = 1;
    opts.compression = TINYDNG_COMPRESSION_NEW_JPEG;
    memset(&tiling, 0, sizeof(tiling));
    tiling.tile_width = TW;
    tiling.tile_length = TL;

    if (tinydng_write_io_open_memory(g_ctx, &io, &g_err) != TINYDNG_OK ||
        tinydng_writer_create(g_ctx, io, &meta, &opts, &tiling, &w, &g_err) !=
            TINYDNG_OK) {
      CHECK(0, "corpus writer create: %s", g_err.message);
      free(tilebuf);
      tinydng_pixels_free(g_ctx, &px);
      tinydng_document_destroy(g_ctx, src_doc);
      return 0;
    }
    for (t = 0; t < across * down && ok; t++) {
      uint32_t tx = (t % across) * TW;
      uint32_t ty = (t / across) * TL;
      uint32_t pw = (tx + TW <= img->width) ? TW : (img->width - tx);
      uint32_t ph = (ty + TL <= img->height) ? TL : (img->height - ty);
      size_t row_bytes = (size_t)img->width * px.samples_per_pixel * sb;
      size_t trow_bytes = (size_t)pw * px.samples_per_pixel * sb;
      uint32_t r;
      for (r = 0; r < ph; r++) {
        memcpy(tilebuf + (size_t)r * trow_bytes,
               px.data + ((size_t)(ty + r) * img->width + tx) *
                             px.samples_per_pixel * sb,
               trow_bytes);
      }
      if (tinydng_writer_write_tile(w, t, tilebuf, &g_err) != TINYDNG_OK) {
        CHECK(0, "corpus tile %u: %s", t, g_err.message);
        ok = 0;
      }
    }
    if (ok && tinydng_writer_finish(w, &g_err) != TINYDNG_OK) {
      CHECK(0, "corpus finish: %s", g_err.message);
      ok = 0;
    }
    if (!ok) {
      tinydng_writer_finish(w, &g_err);
    }
    free(tilebuf);
    if (ok && tinydng_write_io_memory_take(g_ctx, &io, &blob, &blob_len,
                                           &g_err) == TINYDNG_OK) {
      ok = roundtrip_ok(blob, blob_len, px.data, px.size);
      tinydng_buffer_free(g_ctx, blob);
    }
    io.close(&io);
    CHECK(ok, "pixel3.dng lossless -> tiled LJPEG DNG -> decode identical");
  }
  tinydng_pixels_free(g_ctx, &px);
  tinydng_document_destroy(g_ctx, src_doc);
  return ok;
}

int main(int argc, char **argv) {
  tinydng_cfa cfa;
  tinydng_raw_info raw;
  const char *root = (argc > 1) ? argv[1] : ".";
  int r = 0;

  g_ctx = tinydng_context_create(NULL, NULL);
  if (!g_ctx) {
    return 1;
  }
  printf("== streamwrite tests ==\n");

  memset(&cfa, 0, sizeof(cfa));
  cfa.present = 1;
  cfa.pattern_dim[0] = 2;
  cfa.pattern_dim[1] = 2;
  cfa.pattern[0] = 0;
  cfa.pattern[1] = 1;
  cfa.pattern[2] = 1;
  cfa.pattern[3] = 0;
  cfa.pattern_size = 4;
  memset(&raw, 0, sizeof(raw));
  raw.has_dng_version = 1;
  raw.dng_version[0] = 1;
  raw.dng_version[1] = 4;
  raw.black_level_present = 1;
  raw.black_level[0] = 512;
  raw.white_level_present = 1;
  raw.white_level[0] = 65535;

  /* tiled round-trips: compression x endianness x (plain | DNG) */
  r = test_tiled(TINYDNG_COMPRESSION_NONE, 0, 0, NULL, NULL);
  CHECK(r, "tiled uncompressed LE");
  r = test_tiled(TINYDNG_COMPRESSION_LZW, 0, 0, NULL, NULL);
  CHECK(r, "tiled LZW LE");
  r = test_tiled(TINYDNG_COMPRESSION_LZW, 1, 0, NULL, NULL);
  CHECK(r, "tiled LZW BE");
  r = test_tiled(TINYDNG_COMPRESSION_NEW_JPEG, 0, 0, NULL, NULL);
  CHECK(r, "tiled LJPEG LE");
  r = test_tiled(TINYDNG_COMPRESSION_NEW_JPEG, 1, 0, NULL, NULL);
  CHECK(r, "tiled LJPEG BE");
  r = test_tiled(TINYDNG_COMPRESSION_NEW_JPEG, 0, 1, &cfa, &raw);
  CHECK(r, "tiled LJPEG DNG (CFA)");
  r = test_tiled(TINYDNG_COMPRESSION_NONE, 1, 1, NULL, NULL);
  CHECK(r, "tiled uncompressed DNG BE");
  r = test_tiled(TINYDNG_COMPRESSION_PACKBITS, 0, 0, NULL, NULL);
  CHECK(r, "tiled PackBits LE");

  /* multi-strip round-trips */
  r = test_strips(TINYDNG_COMPRESSION_NONE, 0, 4);
  CHECK(r, "strips uncompressed (rps=4)");
  r = test_strips(TINYDNG_COMPRESSION_LZW, 1, 3);
  CHECK(r, "strips LZW BE (rps=3)");
  r = test_strips(TINYDNG_COMPRESSION_NEW_JPEG, 0, 4);
  CHECK(r, "strips LJPEG (rps=4)");
  r = test_strips(TINYDNG_COMPRESSION_NEW_JPEG, 0, 1);
  CHECK(r, "strips LJPEG single-row");
  r = test_strips(TINYDNG_COMPRESSION_PACKBITS, 1, 5);
  CHECK(r, "strips PackBits BE (rps=5)");

  /* write_memory byte parity */
  r = test_memory_parity(TINYDNG_COMPRESSION_NONE, 0, 0);
  CHECK(r, "write_memory parity: none");
  r = test_memory_parity(TINYDNG_COMPRESSION_LZW, 1, 0);
  CHECK(r, "write_memory parity: lzw BE");
  r = test_memory_parity(TINYDNG_COMPRESSION_NEW_JPEG, 0, 0);
  CHECK(r, "write_memory parity: ljpeg");
  r = test_memory_parity(TINYDNG_COMPRESSION_NEW_JPEG, 1, 1);
  CHECK(r, "write_memory parity: ljpeg BE DNG");
  r = test_memory_parity(TINYDNG_COMPRESSION_NONE, 0, 1);
  CHECK(r, "write_memory parity: none DNG");
  r = test_memory_parity(TINYDNG_COMPRESSION_PACKBITS, 1, 0);
  CHECK(r, "write_memory parity: packbits BE");

  /* write_file */
  r = test_write_file(TINYDNG_COMPRESSION_LZW);
  CHECK(r, "write_file LZW round-trip");
  r = test_write_file(TINYDNG_COMPRESSION_NEW_JPEG);
  CHECK(r, "write_file LJPEG round-trip");
  r = test_write_file(TINYDNG_COMPRESSION_PACKBITS);
  CHECK(r, "write_file PackBits round-trip");

  r = test_real_corpus(root);
  CHECK(r, "real corpus streaming round-trip");

  test_errors();

  if (tinydng_context_memory_used(g_ctx) != 0u) {
    fprintf(stderr, "  FAIL: leak, used=%zu\n",
            tinydng_context_memory_used(g_ctx));
    g_fail = 1;
  }
  tinydng_context_destroy(g_ctx);
  printf(g_fail ? "STREAMWRITE: FAILURES\n" : "STREAMWRITE: ALL PASS\n");
  return g_fail;
}
