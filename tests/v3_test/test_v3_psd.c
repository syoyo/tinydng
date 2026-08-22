/* PSD/PSB tests: writer <-> reader round-trips (composite + layers, RAW/RLE,
   8/16/32-bit, PSD+PSB), zip/zip-pred channels, 1-bit composite, group tree,
   MT determinism, truncation fuzz-lite, and (optional) a real-world corpus
   sweep: `test_v3_psd corpus <dir>` opens and fully decodes every *.psd/psb.
   Self-contained except for corpus mode. */
#include "td_test_util.h"

#include "miniz.h"

/* ------------------------------------------------------------------ */
/* Round-trip: composite + layers                                     */
/* ------------------------------------------------------------------ */

static void fill_pattern(uint8_t *buf, size_t bytes, uint32_t seed) {
  size_t i;
  for (i = 0; i < bytes; i++) {
    buf[i] = (uint8_t)(((i + seed) * 2654435761u) >> ((i & 3u) * 8u));
  }
}

static int roundtrip(tinydng_context *ctx, const char *name, uint16_t depth,
                     uint16_t channels, uint16_t color_mode, uint16_t comp,
                     int as_psb, uint32_t W, uint32_t H) {
  tinydng_error e;
  size_t sb = (size_t)depth / 8u;
  size_t comp_bytes = (size_t)W * H * channels * sb;
  uint8_t *composite = (uint8_t *)malloc(comp_bytes);
  /* Layer 0: full-size; layer 1: offset rect; layers 2+3: group pair. */
  uint32_t lw = W > 2u ? W - 2u : 1u, lh = H > 1u ? H - 1u : 1u;
  size_t lplane = (size_t)lw * lh * sb;
  uint8_t *lay_r = (uint8_t *)malloc(lplane);
  uint8_t *lay_g = (uint8_t *)malloc(lplane);
  tinydng_psd_write_channel lch[2];
  tinydng_psd_write_layer layers[4];
  tinydng_psd_write_doc wd;
  tinydng_psd_write_options wo;
  uint8_t *blob = NULL;
  size_t blob_len = 0;
  tinydng_document *doc = NULL;
  const tinydng_psd_info *psd;
  tinydng_pixels px;
  int rc = 1;

  fill_pattern(composite, comp_bytes, 7u);
  fill_pattern(lay_r, lplane, 11u);
  fill_pattern(lay_g, lplane, 13u);

  memset(lch, 0, sizeof(lch));
  lch[0].id = 0;
  lch[0].data = lay_r;
  lch[0].size = lplane;
  lch[1].id = -1;
  lch[1].data = lay_g;
  lch[1].size = lplane;

  memset(layers, 0, sizeof(layers));
  layers[0].top = 1;
  layers[0].left = 2;
  layers[0].bottom = 1 + (int32_t)lh;
  layers[0].right = 2 + (int32_t)lw;
  layers[0].name = "base \xc3\xa9\xe6\x97\xa5"; /* non-ASCII UTF-8 */
  layers[0].channels = lch;
  layers[0].channel_count = 2;
  layers[1].top = -3;
  layers[1].left = -4;
  layers[1].bottom = -3 + (int32_t)lh;
  layers[1].right = -4 + (int32_t)lw;
  layers[1].name = "offset";
  layers[1].opacity = 128;
  layers[1].blend_mode = TINYDNG_PSD_BLEND_MULTIPLY;
  layers[1].channels = lch;
  layers[1].channel_count = 2;
  /* Group: divider (index 2, bottom) then folder (index 3, top). */
  layers[2].name = "</Layer group>";
  layers[2].section = TINYDNG_PSD_SECTION_DIVIDER;
  layers[3].name = "group";
  layers[3].section = TINYDNG_PSD_SECTION_OPEN_FOLDER;

  memset(&wd, 0, sizeof(wd));
  wd.width = W;
  wd.height = H;
  wd.depth = depth;
  wd.color_mode = color_mode;
  wd.channel_count = channels;
  wd.composite = composite;
  wd.composite_size = comp_bytes;
  wd.layers = layers;
  wd.layer_count = 4;
  memset(&wo, 0, sizeof(wo));
  wo.compression = comp;
  wo.as_psb = (uint8_t)as_psb;

  if (tinydng_psd_write_memory(ctx, &wd, &wo, &blob, &blob_len, &e) !=
      TINYDNG_OK) {
    CHECK(0, "%s: write failed: %s", name, e.message);
    goto done;
  }
  if (tinydng_open_memory(ctx, blob, blob_len, NULL, &doc, &e) != TINYDNG_OK) {
    CHECK(0, "%s: reopen failed: %s", name, e.message);
    goto done;
  }
  psd = tinydng_document_psd(doc);
  CHECK(psd != NULL, "%s: no psd info", name);
  if (!psd) {
    goto done;
  }
  CHECK(psd->width == W && psd->height == H, "%s: header dims", name);
  CHECK(psd->depth == depth, "%s: depth", name);
  CHECK(psd->is_psb == (as_psb ? 1u : 0u), "%s: psb flag", name);
  CHECK(psd->layer_count == 4u, "%s: layer count (got %zu)", name,
        psd->layer_count);
  if (psd->layer_count == 4u) {
    CHECK(strcmp(psd->layers[0].name, "base \xc3\xa9\xe6\x97\xa5") == 0,
          "%s: unicode layer name (got '%s')", name, psd->layers[0].name);
    CHECK(psd->layers[1].opacity == 128u, "%s: opacity", name);
    CHECK(psd->layers[1].blend_mode == TINYDNG_PSD_BLEND_MULTIPLY,
          "%s: blend mode", name);
    CHECK(psd->layers[1].top == -3 && psd->layers[1].left == -4,
          "%s: negative rect", name);
    CHECK(psd->layers[3].section == TINYDNG_PSD_SECTION_OPEN_FOLDER,
          "%s: folder section", name);
    /* Group tree: divider at 2 belongs to folder at 3. */
    CHECK(psd->layers[2].parent == 3u, "%s: group parent (got %u)", name,
          psd->layers[2].parent);
    CHECK(psd->layers[3].parent == UINT32_MAX, "%s: root parent", name);
  }

  /* Composite pixel-exact. */
  if (tinydng_decode_image(ctx, doc, 0, NULL, &px, &e) != TINYDNG_OK) {
    CHECK(0, "%s: composite decode failed: %s", name, e.message);
    goto done;
  }
  CHECK(px.size == comp_bytes, "%s: composite size", name);
  CHECK(memcmp(px.data, composite, comp_bytes) == 0,
        "%s: composite bytes differ", name);
  tinydng_pixels_free(ctx, &px);

  /* Layer pixel-exact (interleaved R + alpha from the two planes). */
  if (tinydng_psd_decode_layer(ctx, doc, 0, NULL, &px, &e) != TINYDNG_OK) {
    CHECK(0, "%s: layer decode failed: %s", name, e.message);
    goto done;
  }
  CHECK(px.width == lw && px.height == lh, "%s: layer dims", name);
  CHECK(px.samples_per_pixel == 2u, "%s: layer spp", name);
  {
    size_t n = (size_t)lw * lh, i, bad = 0;
    for (i = 0; i < n && bad == 0u; i++) {
      if (memcmp(px.data + (2u * i) * sb, lay_r + i * sb, sb) != 0 ||
          memcmp(px.data + (2u * i + 1u) * sb, lay_g + i * sb, sb) != 0) {
        bad = 1;
      }
    }
    CHECK(bad == 0u, "%s: layer bytes differ", name);
  }
  tinydng_pixels_free(ctx, &px);

  /* Single-channel decode. */
  if (tinydng_psd_decode_layer_channel(ctx, doc, 0, 0, NULL, &px, &e) !=
      TINYDNG_OK) {
    CHECK(0, "%s: channel decode failed: %s", name, e.message);
    goto done;
  }
  CHECK(px.samples_per_pixel == 1u && px.size == lplane,
        "%s: channel plane size", name);
  CHECK(memcmp(px.data, lay_r, lplane) == 0, "%s: channel bytes", name);
  tinydng_pixels_free(ctx, &px);
  rc = 0;

done:
  if (doc) {
    tinydng_document_destroy(ctx, doc);
  }
  tinydng_buffer_free(ctx, blob);
  free(composite);
  free(lay_r);
  free(lay_g);
  return rc;
}

/* ------------------------------------------------------------------ */
/* Hand-built fixtures: zip channels, 1-bit composite                 */
/* ------------------------------------------------------------------ */

/* Build a minimal single-layer PSD with one zip(-pred) channel. */
static void test_zip_layer(tinydng_context *ctx, uint16_t depth,
                           int predicted) {
  enum { W = 9, H = 5 };
  tinydng_error e;
  size_t sb = (size_t)depth / 8u;
  size_t plane = (size_t)W * H * sb;
  uint8_t stored[9 * 5 * 4];
  uint8_t expect[9 * 5 * 4];
  uint8_t zbuf[512];
  mz_ulong zlen = sizeof(zbuf);
  uint8_t file[4096];
  size_t at = 0, lm_len_at, li_len_at, extra_at, chlen_at;
  size_t i;
  tinydng_document *doc = NULL;
  tinydng_pixels px;
  char name[64];

  snprintf(name, sizeof(name), "zip%s-%u", predicted ? "pred" : "", depth);

  /* Host-order expected samples + big-endian stored bytes. */
  for (i = 0; i < (size_t)W * H; i++) {
    if (depth == 8u) {
      expect[i] = (uint8_t)(i * 7u);
      stored[i] = expect[i];
    } else if (depth == 16u) {
      uint16_t v = (uint16_t)(i * 517u);
      memcpy(expect + 2u * i, &v, 2u);
      stored[2u * i] = (uint8_t)(v >> 8);
      stored[2u * i + 1u] = (uint8_t)v;
    } else {
      uint32_t v = (uint32_t)(i * 68111u);
      memcpy(expect + 4u * i, &v, 4u);
      stored[4u * i] = (uint8_t)(v >> 24);
      stored[4u * i + 1u] = (uint8_t)(v >> 16);
      stored[4u * i + 2u] = (uint8_t)(v >> 8);
      stored[4u * i + 3u] = (uint8_t)v;
    }
  }
  if (predicted) {
    /* Forward-predict rows (inverse of the reader's un-predict). */
    uint8_t pred[9 * 5 * 4];
    uint32_t y;
    memcpy(pred, stored, plane);
    for (y = 0; y < H; y++) {
      uint8_t *row = pred + (size_t)y * W * sb;
      if (depth == 8u) {
        int x;
        for (x = W - 1; x >= 1; x--) {
          row[x] = (uint8_t)(row[x] - row[x - 1]);
        }
      } else if (depth == 16u) {
        int x;
        for (x = W - 1; x >= 1; x--) {
          uint16_t cur = (uint16_t)(((uint16_t)row[2 * x] << 8) |
                                    row[2 * x + 1]);
          uint16_t prev = (uint16_t)(((uint16_t)row[2 * (x - 1)] << 8) |
                                     row[2 * (x - 1) + 1]);
          uint16_t d = (uint16_t)(cur - prev);
          row[2 * x] = (uint8_t)(d >> 8);
          row[2 * x + 1] = (uint8_t)d;
        }
      } else {
        /* interleave byte planes then delta */
        uint8_t tmp[9 * 4];
        int x;
        size_t b;
        for (x = 0; x < W; x++) {
          for (b = 0; b < 4u; b++) {
            tmp[b * W + x] = row[4u * (size_t)x + b];
          }
        }
        for (x = (int)(W * 4u) - 1; x >= 1; x--) {
          tmp[x] = (uint8_t)(tmp[x] - tmp[x - 1]);
        }
        memcpy(row, tmp, (size_t)W * 4u);
      }
    }
    memcpy(stored, pred, plane);
  }
  CHECK(mz_compress(zbuf, &zlen, stored, (mz_ulong)plane) == MZ_OK,
        "%s: mz_compress", name);

  /* File: header, empty color mode + resources, layer section with one
     layer/one channel, RAW composite (zeros). */
  memset(file, 0, sizeof(file));
  memcpy(file, "8BPS", 4);
  tdt_pu16(file + 4, 1, 1);
  tdt_pu16(file + 12, 1, 1);       /* channels */
  tdt_pu32(file + 14, H, 1);       /* height */
  tdt_pu32(file + 18, W, 1);       /* width */
  tdt_pu16(file + 22, depth, 1);
  tdt_pu16(file + 24, TINYDNG_PSD_GRAYSCALE, 1);
  at = 26;
  tdt_pu32(file + at, 0, 1); at += 4; /* color mode */
  tdt_pu32(file + at, 0, 1); at += 4; /* resources */
  lm_len_at = at; at += 4;            /* layer/mask total (patch) */
  li_len_at = at; at += 4;            /* layer info len (patch) */
  tdt_pu16(file + at, 1, 1); at += 2; /* layer count */
  tdt_pu32(file + at, 0, 1); at += 4; /* top */
  tdt_pu32(file + at, 0, 1); at += 4; /* left */
  tdt_pu32(file + at, H, 1); at += 4; /* bottom */
  tdt_pu32(file + at, W, 1); at += 4; /* right */
  tdt_pu16(file + at, 1, 1); at += 2; /* 1 channel */
  tdt_pu16(file + at, 0, 1); at += 2; /* id 0 */
  chlen_at = at; at += 4;             /* channel length (patch) */
  memcpy(file + at, "8BIM", 4); at += 4;
  memcpy(file + at, "norm", 4); at += 4;
  file[at++] = 255; /* opacity */
  file[at++] = 0;   /* clipping */
  file[at++] = 0;   /* flags */
  file[at++] = 0;   /* filler */
  extra_at = at; at += 4;             /* extra len (patch) */
  tdt_pu32(file + at, 0, 1); at += 4; /* mask */
  tdt_pu32(file + at, 0, 1); at += 4; /* ranges */
  file[at++] = 1; file[at++] = 'z'; file[at++] = 0; file[at++] = 0; /* name */
  tdt_pu32(file + extra_at, (uint32_t)(at - extra_at - 4u), 1);
  /* channel image data */
  tdt_pu16(file + at, predicted ? 3u : 2u, 1); at += 2;
  memcpy(file + at, zbuf, zlen); at += (size_t)zlen;
  if (zlen & 1u) { file[at++] = 0; } /* keep section even */
  tdt_pu32(file + chlen_at, (uint32_t)(2u + zlen), 1);
  tdt_pu32(file + li_len_at, (uint32_t)(at - li_len_at - 4u), 1);
  tdt_pu32(file + lm_len_at, (uint32_t)(at - lm_len_at - 4u), 1);
  /* composite: RAW zeros */
  tdt_pu16(file + at, 0, 1); at += 2;
  at += plane;

  if (tinydng_open_memory(ctx, file, at, NULL, &doc, &e) != TINYDNG_OK) {
    CHECK(0, "%s: open failed: %s", name, e.message);
    return;
  }
  if (tinydng_psd_decode_layer(ctx, doc, 0, NULL, &px, &e) != TINYDNG_OK) {
    CHECK(0, "%s: layer decode failed: %s", name, e.message);
    tinydng_document_destroy(ctx, doc);
    return;
  }
  CHECK(px.size == plane, "%s: plane size", name);
  CHECK(memcmp(px.data, expect, plane) == 0, "%s: decoded bytes", name);
  tinydng_pixels_free(ctx, &px);
  tinydng_document_destroy(ctx, doc);
}

/* 1-bit bitmap composite, RAW. */
static void test_bitmap_composite(tinydng_context *ctx) {
  enum { W = 12, H = 3 };
  tinydng_error e;
  uint8_t file[256];
  size_t row_bytes = (W + 7) / 8; /* 2 */
  size_t at;
  tinydng_document *doc = NULL;
  tinydng_pixels px;
  uint32_t y, x;

  memset(file, 0, sizeof(file));
  memcpy(file, "8BPS", 4);
  tdt_pu16(file + 4, 1, 1);
  tdt_pu16(file + 12, 1, 1);
  tdt_pu32(file + 14, H, 1);
  tdt_pu32(file + 18, W, 1);
  tdt_pu16(file + 22, 1, 1); /* depth 1 */
  tdt_pu16(file + 24, TINYDNG_PSD_BITMAP, 1);
  at = 26;
  tdt_pu32(file + at, 0, 1); at += 4;
  tdt_pu32(file + at, 0, 1); at += 4;
  tdt_pu32(file + at, 0, 1); at += 4; /* no layers */
  tdt_pu16(file + at, 0, 1); at += 2; /* RAW */
  /* rows: 0xAA 0xA0 pattern */
  for (y = 0; y < H; y++) {
    file[at++] = 0xAA;
    file[at++] = 0xA0;
  }
  (void)row_bytes;

  if (tinydng_open_memory(ctx, file, at, NULL, &doc, &e) != TINYDNG_OK) {
    CHECK(0, "bitmap: open failed: %s", e.message);
    return;
  }
  if (tinydng_decode_image(ctx, doc, 0, NULL, &px, &e) != TINYDNG_OK) {
    CHECK(0, "bitmap: decode failed: %s", e.message);
    tinydng_document_destroy(ctx, doc);
    return;
  }
  CHECK(px.bits_per_sample == 8u && px.width == W && px.height == H,
        "bitmap: geometry");
  /* PSD bitmap polarity: a stored set bit is BLACK (Photoshop semantics;
   * matches GIMP's psd plugin), scaled to the 8-bit output range. */
  for (y = 0; y < H; y++) {
    for (x = 0; x < W; x++) {
      uint8_t want = (x & 1u) ? 255u : 0u; /* 0xAA... = 10101010 1010 */
      CHECK(px.data[y * W + x] == want, "bitmap: pixel %u,%u", x, y);
    }
  }
  tinydng_pixels_free(ctx, &px);
  tinydng_document_destroy(ctx, doc);
}

/* ------------------------------------------------------------------ */
/* MT determinism                                                     */
/* ------------------------------------------------------------------ */

static void test_mt_determinism(tinydng_context *ctx) {
  tinydng_error e;
  enum { W = 64, H = 48, C = 4 };
  size_t bytes = (size_t)W * H * C;
  uint8_t *composite = (uint8_t *)malloc(bytes);
  tinydng_psd_write_doc wd;
  tinydng_psd_write_options wo;
  uint8_t *blob = NULL;
  size_t blob_len = 0;
  tinydng_document *doc = NULL;
  tinydng_pixels p1, p8;
  tinydng_decode_options o1, o8;

  fill_pattern(composite, bytes, 3u);
  memset(&wd, 0, sizeof(wd));
  wd.width = W;
  wd.height = H;
  wd.depth = 8;
  wd.color_mode = TINYDNG_PSD_RGB;
  wd.channel_count = C;
  wd.composite = composite;
  wd.composite_size = bytes;
  memset(&wo, 0, sizeof(wo));
  wo.compression = TINYDNG_PSD_COMP_RLE;

  if (tinydng_psd_write_memory(ctx, &wd, &wo, &blob, &blob_len, &e) !=
          TINYDNG_OK ||
      tinydng_open_memory(ctx, blob, blob_len, NULL, &doc, &e) !=
          TINYDNG_OK) {
    CHECK(0, "mt: setup failed: %s", e.message);
    free(composite);
    tinydng_buffer_free(ctx, blob);
    return;
  }
  memset(&o1, 0, sizeof(o1));
  o1.num_threads = 1;
  memset(&o8, 0, sizeof(o8));
  o8.num_threads = 8;
  CHECK(tinydng_decode_image(ctx, doc, 0, &o1, &p1, &e) == TINYDNG_OK,
        "mt: serial decode");
  CHECK(tinydng_decode_image(ctx, doc, 0, &o8, &p8, &e) == TINYDNG_OK,
        "mt: parallel decode");
  CHECK(p1.size == p8.size && memcmp(p1.data, p8.data, p1.size) == 0,
        "mt: MT != ST bytes");
  CHECK(memcmp(p1.data, composite, bytes) == 0, "mt: bytes wrong");
  tinydng_pixels_free(ctx, &p1);
  tinydng_pixels_free(ctx, &p8);
  tinydng_document_destroy(ctx, doc);
  tinydng_buffer_free(ctx, blob);
  free(composite);
}

/* ------------------------------------------------------------------ */
/* Robustness: truncation sweep + hostile fields                      */
/* ------------------------------------------------------------------ */

static void test_truncation(tinydng_context *ctx) {
  tinydng_error e;
  enum { W = 8, H = 6, C = 3 };
  size_t bytes = (size_t)W * H * C;
  uint8_t *composite = (uint8_t *)malloc(bytes);
  tinydng_psd_write_channel ch;
  tinydng_psd_write_layer layer;
  tinydng_psd_write_doc wd;
  uint8_t *blob = NULL;
  size_t blob_len = 0;
  size_t cut;

  fill_pattern(composite, bytes, 1u);
  memset(&ch, 0, sizeof(ch));
  ch.id = 0;
  ch.data = composite;
  ch.size = (size_t)W * H;
  memset(&layer, 0, sizeof(layer));
  layer.right = W;
  layer.bottom = H;
  layer.name = "L";
  layer.channels = &ch;
  layer.channel_count = 1;
  memset(&wd, 0, sizeof(wd));
  wd.width = W;
  wd.height = H;
  wd.depth = 8;
  wd.color_mode = TINYDNG_PSD_RGB;
  wd.channel_count = C;
  wd.composite = composite;
  wd.composite_size = bytes;
  wd.layers = &layer;
  wd.layer_count = 1;

  if (tinydng_psd_write_memory(ctx, &wd, NULL, &blob, &blob_len, &e) !=
      TINYDNG_OK) {
    CHECK(0, "trunc: write failed: %s", e.message);
    free(composite);
    return;
  }
  /* Every truncation point must fail cleanly or open + decode cleanly. */
  for (cut = 0; cut < blob_len; cut++) {
    tinydng_document *doc = NULL;
    if (tinydng_open_memory(ctx, blob, cut, NULL, &doc, &e) == TINYDNG_OK) {
      tinydng_pixels px;
      if (tinydng_decode_image(ctx, doc, 0, NULL, &px, &e) == TINYDNG_OK) {
        tinydng_pixels_free(ctx, &px);
      }
      if (tinydng_document_psd(doc) &&
          tinydng_document_psd(doc)->layer_count > 0u) {
        tinydng_pixels lp;
        if (tinydng_psd_decode_layer(ctx, doc, 0, NULL, &lp, &e) ==
            TINYDNG_OK) {
          tinydng_pixels_free(ctx, &lp);
        }
      }
      tinydng_document_destroy(ctx, doc);
    }
  }
  tinydng_buffer_free(ctx, blob);
  free(composite);
}

static void test_hostile(tinydng_context *ctx) {
  tinydng_error e;
  tinydng_document *doc = NULL;
  uint8_t f[128];

  /* INT16_MIN layer count. */
  memset(f, 0, sizeof(f));
  memcpy(f, "8BPS", 4);
  tdt_pu16(f + 4, 1, 1);
  tdt_pu16(f + 12, 3, 1);
  tdt_pu32(f + 14, 4, 1);
  tdt_pu32(f + 18, 4, 1);
  tdt_pu16(f + 22, 8, 1);
  tdt_pu16(f + 24, TINYDNG_PSD_RGB, 1);
  tdt_pu32(f + 26, 0, 1);
  tdt_pu32(f + 30, 0, 1);
  tdt_pu32(f + 34, 40, 1); /* layer/mask total */
  tdt_pu32(f + 38, 36, 1); /* layer info len */
  tdt_pu16(f + 42, 0x8000u, 1); /* INT16_MIN */
  CHECK(tinydng_open_memory(ctx, f, 100, NULL, &doc, &e) != TINYDNG_OK,
        "hostile: INT16_MIN accepted");
  if (doc) {
    tinydng_document_destroy(ctx, doc);
    doc = NULL;
  }

  /* Channel count out of range. */
  memset(f, 0, sizeof(f));
  memcpy(f, "8BPS", 4);
  tdt_pu16(f + 4, 1, 1);
  tdt_pu16(f + 12, 57, 1); /* > 56 */
  tdt_pu32(f + 14, 4, 1);
  tdt_pu32(f + 18, 4, 1);
  tdt_pu16(f + 22, 8, 1);
  tdt_pu16(f + 24, TINYDNG_PSD_RGB, 1);
  CHECK(tinydng_open_memory(ctx, f, 64, NULL, &doc, &e) != TINYDNG_OK,
        "hostile: 57 channels accepted");
  if (doc) {
    tinydng_document_destroy(ctx, doc);
    doc = NULL;
  }

  /* Depth 4 invalid. */
  memset(f, 0, sizeof(f));
  memcpy(f, "8BPS", 4);
  tdt_pu16(f + 4, 1, 1);
  tdt_pu16(f + 12, 1, 1);
  tdt_pu32(f + 14, 4, 1);
  tdt_pu32(f + 18, 4, 1);
  tdt_pu16(f + 22, 4, 1);
  tdt_pu16(f + 24, TINYDNG_PSD_GRAYSCALE, 1);
  CHECK(tinydng_open_memory(ctx, f, 64, NULL, &doc, &e) != TINYDNG_OK,
        "hostile: depth 4 accepted");
  if (doc) {
    tinydng_document_destroy(ctx, doc);
    doc = NULL;
  }
}

/* ------------------------------------------------------------------ */
/* Smart-object nesting + depth cap                                   */
/* ------------------------------------------------------------------ */

/* Wrap `inner` (any file bytes) in a minimal grayscale PSD carrying one
   embedded ('liFD') smart object in a global 'lnk2' block. */
static size_t wrap_in_psd(const uint8_t *inner, size_t inner_len,
                          uint8_t *out, size_t cap) {
  size_t at = 0, lm_at, blk_len_at, entry_len_at, entry_start;
  enum { W = 2, H = 2 };
  if (cap < inner_len + 256u) {
    return 0;
  }
  memset(out, 0, inner_len + 256u);
  memcpy(out, "8BPS", 4);
  tdt_pu16(out + 4, 1, 1);
  tdt_pu16(out + 12, 1, 1);
  tdt_pu32(out + 14, H, 1);
  tdt_pu32(out + 18, W, 1);
  tdt_pu16(out + 22, 8, 1);
  tdt_pu16(out + 24, TINYDNG_PSD_GRAYSCALE, 1);
  at = 26;
  tdt_pu32(out + at, 0, 1); at += 4; /* color mode */
  tdt_pu32(out + at, 0, 1); at += 4; /* resources */
  lm_at = at; at += 4;               /* layer/mask total (patch) */
  tdt_pu32(out + at, 0, 1); at += 4; /* layer info: empty */
  tdt_pu32(out + at, 0, 1); at += 4; /* global mask: empty */
  memcpy(out + at, "8BIM", 4); at += 4;
  memcpy(out + at, "lnk2", 4); at += 4;
  blk_len_at = at; at += 4;
  entry_len_at = at; at += 8;        /* u64 entry length (patch) */
  entry_start = at;
  memcpy(out + at, "liFD", 4); at += 4;
  tdt_pu32(out + at, 2, 1); at += 4; /* version */
  out[at++] = 3; memcpy(out + at, "uid", 3); at += 3; /* pascal uid */
  tdt_pu32(out + at, 5, 1); at += 4; /* unicode name: 5 units */
  { const char *nm = "inner"; int i;
    for (i = 0; i < 5; i++) { out[at++] = 0; out[at++] = (uint8_t)nm[i]; } }
  memcpy(out + at, "8BPS", 4); at += 4; /* filetype */
  memcpy(out + at, "8BIM", 4); at += 4; /* creator */
  tdt_pu64(out + at, inner_len, 1); at += 8;
  out[at++] = 0; /* no file-open descriptor */
  memcpy(out + at, inner, inner_len); at += inner_len;
  tdt_pu64(out + entry_len_at, at - entry_start, 1);
  while ((at - (blk_len_at + 4u)) & 3u) { out[at++] = 0; } /* entry pad 4 */
  tdt_pu32(out + blk_len_at, (uint32_t)(at - blk_len_at - 4u), 1);
  if (at & 1u) { out[at++] = 0; }
  tdt_pu32(out + lm_at, (uint32_t)(at - lm_at - 4u), 1);
  tdt_pu16(out + at, 0, 1); at += 2; /* composite RAW */
  at += (size_t)W * H;               /* zero pixels */
  return at;
}

static void test_smart_object_nesting(tinydng_context *ctx) {
  tinydng_error e;
  uint8_t *bufs[8];
  size_t lens[8];
  int levels = 6; /* > default max_embed_depth (4) */
  int i;
  tinydng_document *doc = NULL;

  /* Innermost: tiny writer-generated PSD. */
  {
    tinydng_psd_write_doc wd;
    uint8_t px[4] = {1, 2, 3, 4};
    memset(&wd, 0, sizeof(wd));
    wd.width = 2;
    wd.height = 2;
    wd.depth = 8;
    wd.color_mode = TINYDNG_PSD_GRAYSCALE;
    wd.channel_count = 1;
    wd.composite = px;
    wd.composite_size = 4;
    if (tinydng_psd_write_memory(ctx, &wd, NULL, &bufs[0], &lens[0], &e) !=
        TINYDNG_OK) {
      CHECK(0, "so: inner write failed: %s", e.message);
      return;
    }
  }
  for (i = 1; i <= levels; i++) {
    size_t cap = lens[i - 1] + 512u;
    bufs[i] = (uint8_t *)malloc(cap);
    lens[i] = wrap_in_psd(bufs[i - 1], lens[i - 1], bufs[i], cap);
    CHECK(lens[i] > 0u, "so: wrap level %d failed", i);
  }

  if (tinydng_open_memory(ctx, bufs[levels], lens[levels], NULL, &doc, &e) !=
      TINYDNG_OK) {
    CHECK(0, "so: outer open failed: %s", e.message);
  } else {
    /* Walk down: each level must expose exactly one smart object until the
       depth cap trips. */
    int depth = 0;
    tinydng_document *cur = doc;
    for (;;) {
      const tinydng_psd_info *psd = tinydng_document_psd(cur);
      tinydng_document *child = NULL;
      tinydng_status st;
      if (!psd || psd->smart_object_count == 0u) {
        break; /* reached the writer-generated innermost file */
      }
      st = tinydng_psd_smart_object_open(ctx, cur, 0, NULL, &child, &e);
      if (st != TINYDNG_OK) {
        CHECK(st == TINYDNG_E_UNSUPPORTED, "so: unexpected error: %s",
              e.message);
        break;
      }
      depth++;
      if (cur != doc) {
        tinydng_document_destroy(ctx, cur);
      }
      cur = child;
    }
    CHECK(depth == 4, "so: expected depth cap at 4, got %d", depth);
    if (cur != doc) {
      tinydng_document_destroy(ctx, cur);
    }
    tinydng_document_destroy(ctx, doc);
  }
  tinydng_buffer_free(ctx, bufs[0]);
  for (i = 1; i <= levels; i++) {
    free(bufs[i]);
  }
}

/* ------------------------------------------------------------------ */
/* Corpus mode: open + decode everything under a directory            */
/* ------------------------------------------------------------------ */

static int run_corpus(const char *path) {
  /* Delegated to the shell for portability: the test binary takes explicit
     file arguments in corpus mode. */
  (void)path;
  return 0;
}

static void corpus_one(tinydng_context *ctx, const char *path) {
  tinydng_error e;
  tinydng_document *doc = NULL;
  tinydng_status st = tinydng_open_file(ctx, path, NULL, &doc, &e);
  const tinydng_psd_info *psd;
  printf("corpus: %s -> %s", path, tinydng_status_string(st));
  if (st != TINYDNG_OK) {
    printf(" (%s)\n", e.message);
    return;
  }
  psd = tinydng_document_psd(doc);
  if (psd) {
    tinydng_pixels px;
    size_t i;
    printf(" %ux%u depth=%u mode=%u layers=%zu res=%zu", psd->width,
           psd->height, psd->depth, psd->color_mode, psd->layer_count,
           psd->resource_count);
    if (tinydng_decode_image(ctx, doc, 0, NULL, &px, &e) == TINYDNG_OK) {
      printf(" composite=OK");
      tinydng_pixels_free(ctx, &px);
    } else {
      printf(" composite=%s(%s)", tinydng_status_string(e.status), e.message);
    }
    for (i = 0; i < psd->layer_count; i++) {
      if (psd->layers[i].width && psd->layers[i].height &&
          psd->layers[i].channel_count) {
        if (tinydng_psd_decode_layer(ctx, doc, i, NULL, &px, &e) ==
            TINYDNG_OK) {
          tinydng_pixels_free(ctx, &px);
        } else {
          printf(" layer%zu=%s", i, tinydng_status_string(e.status));
        }
      }
    }
  }
  printf("\n");
  tinydng_document_destroy(ctx, doc);
}

/* ------------------------------------------------------------------ */

/* max_psd_segments cap: an RLE composite whose segment table would exceed the
   configured cap must be dropped gracefully (image_count == 0) rather than
   allocating a huge table/buffer. The same PSD opened with the default cap
   parses the composite normally, confirming the cap (not corruption) triggered
   the degradation. */
static int test_psd_segment_cap(tinydng_context *ctx) {
  tinydng_error e;
  uint32_t W = 4, H = 4;
  uint16_t channels = 3, depth = 8;
  size_t sb = (size_t)depth / 8u;
  size_t comp_bytes = (size_t)W * H * channels * sb;
  uint8_t *composite = (uint8_t *)malloc(comp_bytes ? comp_bytes : 1u);
  uint8_t *blob = NULL;
  size_t blob_len = 0;
  tinydng_psd_write_doc wd;
  tinydng_psd_write_options wo;
  tinydng_config ccfg;
  tinydng_context *cctx = NULL;
  tinydng_document *doc = NULL;
  int rc = 0;
  size_t i;

  if (!composite) {
    CHECK(0, "segment-cap: alloc");
    return 1;
  }
  for (i = 0; i < comp_bytes; i++) composite[i] = (uint8_t)(i * 7u);

  memset(&wd, 0, sizeof(wd));
  wd.width = W;
  wd.height = H;
  wd.depth = depth;
  wd.color_mode = (uint16_t)TINYDNG_PSD_RGB;
  wd.channel_count = channels;
  wd.composite = composite;
  wd.composite_size = comp_bytes;
  memset(&wo, 0, sizeof(wo));
  wo.compression = TINYDNG_PSD_COMP_RLE;

  if (tinydng_psd_write_memory(ctx, &wd, &wo, &blob, &blob_len, &e) !=
      TINYDNG_OK) {
    CHECK(0, "segment-cap: write failed: %s", e.message);
    free(composite);
    return 1;
  }

  /* Default cap: composite parses normally. */
  {
    tinydng_document *d0 = NULL;
    if (tinydng_open_memory(ctx, blob, blob_len, NULL, &d0, &e) != TINYDNG_OK) {
      CHECK(0, "segment-cap: default open failed: %s", e.message);
      rc = 1;
    } else {
      CHECK(tinydng_image_count(d0) >= 1u, "segment-cap: default has composite");
      tinydng_document_destroy(ctx, d0);
    }
  }

  /* Tiny cap (1): RLE composite (height*channels = 12 > 1) is dropped. */
  memset(&ccfg, 0, sizeof(ccfg));
  ccfg.max_psd_segments = 1u;
  cctx = tinydng_context_create(&ccfg, NULL);
  if (!cctx) {
    CHECK(0, "segment-cap: capped context create failed");
    rc = 1;
  } else {
    if (tinydng_open_memory(cctx, blob, blob_len, NULL, &doc, &e) !=
        TINYDNG_OK) {
      CHECK(0, "segment-cap: capped open failed: %s", e.message);
      rc = 1;
    } else {
      CHECK(tinydng_image_count(doc) == 0u,
            "segment-cap: composite dropped under tiny cap");
      tinydng_document_destroy(cctx, doc);
    }
    tinydng_context_destroy(cctx);
  }

  tinydng_buffer_free(ctx, blob);
  free(composite);
  return rc;
}

int main(int argc, char **argv) {
  tinydng_error e;
  tinydng_context *ctx = tinydng_context_create(NULL, &e);
  if (!ctx) {
    fprintf(stderr, "context create failed\n");
    return 1;
  }

  if (argc > 2 && strcmp(argv[1], "corpus") == 0) {
    int i;
    for (i = 2; i < argc; i++) {
      corpus_one(ctx, argv[i]);
    }
    tinydng_context_destroy(ctx);
    return g_fail;
  }
  (void)run_corpus;

  /* Round-trip matrix. */
  {
    static const uint16_t depths[] = {8, 16, 32};
    static const uint16_t comps[] = {TINYDNG_PSD_COMP_RAW,
                                     TINYDNG_PSD_COMP_RLE};
    size_t d, c;
    int psb;
    for (d = 0; d < 3; d++) {
      for (c = 0; c < 2; c++) {
        for (psb = 0; psb <= 1; psb++) {
          char name[64];
          snprintf(name, sizeof(name), "rt-d%u-c%u-psb%d", depths[d],
                   comps[c], psb);
          roundtrip(ctx, name, depths[d], 3,
                    (uint16_t)TINYDNG_PSD_RGB, comps[c], psb, 21, 13);
        }
      }
    }
    /* gray, RGBA, CMYK */
    roundtrip(ctx, "rt-gray", 8, 1, (uint16_t)TINYDNG_PSD_GRAYSCALE,
              TINYDNG_PSD_COMP_RLE, 0, 7, 9);
    roundtrip(ctx, "rt-rgba", 8, 4, (uint16_t)TINYDNG_PSD_RGB,
              TINYDNG_PSD_COMP_RLE, 0, 16, 16);
    roundtrip(ctx, "rt-cmyk", 8, 4, (uint16_t)TINYDNG_PSD_CMYK,
              TINYDNG_PSD_COMP_RAW, 0, 5, 5);
    /* degenerate dims */
    roundtrip(ctx, "rt-1x1", 8, 3, (uint16_t)TINYDNG_PSD_RGB,
              TINYDNG_PSD_COMP_RLE, 0, 1, 1);
    roundtrip(ctx, "rt-wide", 8, 1, (uint16_t)TINYDNG_PSD_GRAYSCALE,
              TINYDNG_PSD_COMP_RLE, 0, 3000, 1);
  }

  test_zip_layer(ctx, 8, 0);
  test_zip_layer(ctx, 8, 1);
  test_zip_layer(ctx, 16, 0);
  test_zip_layer(ctx, 16, 1);
  test_zip_layer(ctx, 32, 0);
  test_zip_layer(ctx, 32, 1);
  test_bitmap_composite(ctx);
  test_mt_determinism(ctx);
  test_truncation(ctx);
  test_hostile(ctx);
  test_smart_object_nesting(ctx);
  test_psd_segment_cap(ctx);

  {
    size_t leak = tinydng_context_memory_used(ctx);
    CHECK(leak == 0u, "context leak: %zu bytes still tracked", leak);
  }
  tinydng_context_destroy(ctx);
  if (g_fail) {
    fprintf(stderr, "test_v3_psd: FAILED\n");
    return 1;
  }
  printf("test_v3_psd: all tests passed\n");
  return 0;
}
