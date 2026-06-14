/* Multi-threaded decode tests. Hand-builds two multi-segment images:
 *   - a multi-strip grayscale TIFF (exercises the worker queue + the "direct"
 *     decode-into-destination path + the stdio read lock), and
 *   - a tiled RGB TIFF (exercises per-tile scratch allocation + de-tile blit).
 * Each is decoded serially (num_threads=1) and with several thread counts; all
 * outputs must be byte-identical to the serial result AND match the known pixel
 * pattern (so the fixture and the serial decode are validated too, not just
 * MT==serial). Region decode and the stdio backend are covered as well.
 * argv[1] (repo source dir) is accepted for harness symmetry; unused here. */
#include "td_test_util.h"

/* Deterministic per-sample pattern. */
static uint8_t pix(uint32_t x, uint32_t y, uint32_t c) {
  return (uint8_t)((x * 7u + y * 13u + c * 53u + 17u) & 0xffu);
}

/* Little-endian classic TIFF, spp=1 bps=8, uncompressed, multiple strips. */
static uint8_t *build_strip_tiff(uint32_t w, uint32_t h, uint32_t rps,
                                 uint32_t *nseg_out, size_t *out_size) {
  const uint32_t spp = 1u;
  uint32_t nstrips = (h + rps - 1u) / rps;
  size_t data_off = 8;
  size_t data_len = (size_t)w * h * spp;
  size_t off_arr = data_off + data_len;
  size_t cnt_arr = off_arr + (size_t)nstrips * 4u;
  size_t ifd_off = cnt_arr + (size_t)nstrips * 4u;
  const int NENT = 10;
  size_t total;
  uint8_t *buf, *e;
  uint32_t x, y, s;
  int n = 0;
  if (ifd_off & 1u) {
    ifd_off++;
  }
  total = ifd_off + 2u + (size_t)NENT * 12u + 4u;
  buf = (uint8_t *)calloc(1, total);
  if (!buf) {
    return NULL;
  }
  buf[0] = 'I';
  buf[1] = 'I';
  tdt_pu16(buf + 2, 42, 0);
  tdt_pu32(buf + 4, (uint32_t)ifd_off, 0);
  for (y = 0; y < h; y++) {
    for (x = 0; x < w; x++) {
      buf[data_off + (size_t)y * w + x] = pix(x, y, 0);
    }
  }
  for (s = 0; s < nstrips; s++) {
    uint32_t r0 = s * rps;
    uint32_t rows = (r0 + rps <= h) ? rps : (h - r0);
    tdt_pu32(buf + off_arr + (size_t)s * 4u,
             (uint32_t)(data_off + (size_t)r0 * w * spp), 0);
    tdt_pu32(buf + cnt_arr + (size_t)s * 4u, (uint32_t)((size_t)rows * w * spp),
             0);
  }
  e = buf + ifd_off;
  tdt_pu16(e, (uint16_t)NENT, 0);
  e += 2;
#define ENT(tag, type, cnt, val)         \
  do {                                   \
    tdt_pu16(e + n * 12 + 0, (tag), 0);  \
    tdt_pu16(e + n * 12 + 2, (type), 0); \
    tdt_pu32(e + n * 12 + 4, (cnt), 0);  \
    tdt_pu32(e + n * 12 + 8, (val), 0);  \
    n++;                                 \
  } while (0)
  ENT(256, 4, 1, w);                       /* ImageWidth */
  ENT(257, 4, 1, h);                       /* ImageLength */
  ENT(258, 3, 1, 8);                       /* BitsPerSample */
  ENT(259, 3, 1, 1);                       /* Compression = none */
  ENT(262, 3, 1, 1);                       /* Photometric = blackiszero */
  ENT(273, 4, nstrips, (uint32_t)off_arr); /* StripOffsets */
  ENT(277, 3, 1, spp);                     /* SamplesPerPixel */
  ENT(278, 4, 1, rps);                     /* RowsPerStrip */
  ENT(279, 4, nstrips, (uint32_t)cnt_arr); /* StripByteCounts */
  ENT(339, 3, 1, 1);                       /* SampleFormat = uint */
#undef ENT
  tdt_pu32(e + NENT * 12, 0, 0);
  *nseg_out = nstrips;
  *out_size = total;
  return buf;
}

/* Little-endian classic TIFF, spp=3 bps=8 RGB, uncompressed, tiled. */
static uint8_t *build_tiled_tiff(uint32_t w, uint32_t h, uint32_t tw,
                                 uint32_t th, uint32_t *nseg_out,
                                 size_t *out_size) {
  const uint32_t spp = 3u;
  uint32_t tx_n = (w + tw - 1u) / tw;
  uint32_t ty_n = (h + th - 1u) / th;
  uint32_t ntiles = tx_n * ty_n;
  size_t tile_bytes = (size_t)tw * th * spp;
  size_t data_off = 8;
  size_t data_len = (size_t)ntiles * tile_bytes;
  size_t bps_arr = data_off + data_len;
  size_t off_arr = bps_arr + (size_t)spp * 2u;
  size_t cnt_arr = off_arr + (size_t)ntiles * 4u;
  size_t ifd_off = cnt_arr + (size_t)ntiles * 4u;
  const int NENT = 11;
  size_t total;
  uint8_t *buf, *e;
  uint32_t txi, tyi, xx, yy, c, i;
  int n = 0;
  if (ifd_off & 1u) {
    ifd_off++;
  }
  total = ifd_off + 2u + (size_t)NENT * 12u + 4u;
  buf = (uint8_t *)calloc(1, total);
  if (!buf) {
    return NULL;
  }
  buf[0] = 'I';
  buf[1] = 'I';
  tdt_pu16(buf + 2, 42, 0);
  tdt_pu32(buf + 4, (uint32_t)ifd_off, 0);
  for (tyi = 0; tyi < ty_n; tyi++) {
    for (txi = 0; txi < tx_n; txi++) {
      uint32_t ti = tyi * tx_n + txi;
      uint8_t *tp = buf + data_off + (size_t)ti * tile_bytes;
      for (yy = 0; yy < th; yy++) {
        for (xx = 0; xx < tw; xx++) {
          uint32_t gx = txi * tw + xx, gy = tyi * th + yy;
          uint8_t *d = tp + ((size_t)yy * tw + xx) * spp;
          for (c = 0; c < spp; c++) {
            d[c] = (gx < w && gy < h) ? pix(gx, gy, c) : 0u;
          }
        }
      }
    }
  }
  for (i = 0; i < spp; i++) {
    tdt_pu16(buf + bps_arr + (size_t)i * 2u, 8, 0);
  }
  for (i = 0; i < ntiles; i++) {
    tdt_pu32(buf + off_arr + (size_t)i * 4u,
             (uint32_t)(data_off + (size_t)i * tile_bytes), 0);
    tdt_pu32(buf + cnt_arr + (size_t)i * 4u, (uint32_t)tile_bytes, 0);
  }
  e = buf + ifd_off;
  tdt_pu16(e, (uint16_t)NENT, 0);
  e += 2;
#define ENT(tag, type, cnt, val)         \
  do {                                   \
    tdt_pu16(e + n * 12 + 0, (tag), 0);  \
    tdt_pu16(e + n * 12 + 2, (type), 0); \
    tdt_pu32(e + n * 12 + 4, (cnt), 0);  \
    tdt_pu32(e + n * 12 + 8, (val), 0);  \
    n++;                                 \
  } while (0)
  ENT(256, 4, 1, w);                        /* ImageWidth */
  ENT(257, 4, 1, h);                        /* ImageLength */
  ENT(258, 3, spp, (uint32_t)bps_arr);      /* BitsPerSample[3] */
  ENT(259, 3, 1, 1);                        /* Compression = none */
  ENT(262, 3, 1, 2);                        /* Photometric = RGB */
  ENT(277, 3, 1, spp);                      /* SamplesPerPixel */
  ENT(322, 4, 1, tw);                       /* TileWidth */
  ENT(323, 4, 1, th);                       /* TileLength */
  ENT(324, 4, ntiles, (uint32_t)off_arr);   /* TileOffsets */
  ENT(325, 4, ntiles, (uint32_t)cnt_arr);   /* TileByteCounts */
  ENT(339, 3, 1, 1);                        /* SampleFormat = uint */
#undef ENT
  tdt_pu32(e + NENT * 12, 0, 0);
  *nseg_out = ntiles;
  *out_size = total;
  return buf;
}

/* Compare a decoded raster against the pattern; returns mismatch count. */
static size_t verify_raster(const tinydng_pixels *px, uint32_t w, uint32_t h,
                            uint32_t spp) {
  size_t mm = 0;
  uint32_t x, y, c;
  if (px->width != w || px->height != h || px->samples_per_pixel != spp ||
      px->bits_per_sample != 8u) {
    return (size_t)-1;
  }
  for (y = 0; y < h; y++) {
    for (x = 0; x < w; x++) {
      for (c = 0; c < spp; c++) {
        if (px->data[((size_t)y * w + x) * spp + c] != pix(x, y, c)) {
          mm++;
        }
      }
    }
  }
  return mm;
}

static tinydng_status decode_mem(tinydng_context *ctx, const uint8_t *buf,
                                 size_t size, uint32_t nt, tinydng_pixels *px,
                                 size_t *segcount) {
  tinydng_document *doc = NULL;
  tinydng_error err;
  tinydng_decode_options o;
  tinydng_status st;
  memset(&o, 0, sizeof(o));
  o.num_threads = nt;
  st = tinydng_open_memory(ctx, buf, size, NULL, &doc, &err);
  if (st != TINYDNG_OK) {
    return st;
  }
  if (segcount) {
    const tinydng_image_info *ii = tinydng_image_get(doc, 0);
    *segcount = ii ? tinydng_image_segment_count(ii) : 0u;
  }
  st = tinydng_decode_image(ctx, doc, 0, &o, px, &err);
  tinydng_document_destroy(ctx, doc);
  return st;
}

static tinydng_status decode_region_mem(tinydng_context *ctx,
                                        const uint8_t *buf, size_t size,
                                        uint32_t nt, uint32_t x, uint32_t y,
                                        uint32_t w, uint32_t h,
                                        tinydng_pixels *px) {
  tinydng_document *doc = NULL;
  tinydng_error err;
  tinydng_decode_options o;
  tinydng_status st;
  memset(&o, 0, sizeof(o));
  o.num_threads = nt;
  st = tinydng_open_memory(ctx, buf, size, NULL, &doc, &err);
  if (st != TINYDNG_OK) {
    return st;
  }
  st = tinydng_decode_region(ctx, doc, 0, x, y, w, h, &o, px, &err);
  tinydng_document_destroy(ctx, doc);
  return st;
}

static int write_file(const char *path, const uint8_t *d, size_t n) {
  FILE *f = fopen(path, "wb");
  size_t wrote;
  if (!f) {
    return 0;
  }
  wrote = fwrite(d, 1, n, f);
  fclose(f);
  return wrote == n;
}

/* Decode through the stdio backend (map()==NULL) to exercise the locked
   fseek+fread read path under MT. */
static tinydng_status decode_stdio(tinydng_context *ctx, const char *path,
                                   uint32_t nt, tinydng_pixels *px) {
  tinydng_io io;
  tinydng_error err;
  tinydng_document *doc = NULL;
  tinydng_decode_options o;
  tinydng_status st;
  memset(&o, 0, sizeof(o));
  o.num_threads = nt;
  st = tinydng_io_open_stdio(ctx, path, &io, &err);
  if (st != TINYDNG_OK) {
    return st;
  }
  st = tinydng_open_io(ctx, io, NULL, &doc, &err);
  if (st != TINYDNG_OK) {
    if (io.close) {
      io.close(&io);
    }
    return st;
  }
  st = tinydng_decode_image(ctx, doc, 0, &o, px, &err);
  tinydng_document_destroy(ctx, doc);
  return st;
}

/* Decode `buf` at several thread counts and require byte-identical output to a
   serial reference, which itself is validated against the pattern. */
static void run_fixture(tinydng_context *ctx, const char *name, uint8_t *buf,
                        size_t size, uint32_t w, uint32_t h, uint32_t spp,
                        uint32_t expect_segs, const char *tmp_path) {
  const uint32_t counts[] = {0u, 2u, 3u, 4u, 8u, 16u};
  tinydng_pixels ref;
  size_t segc = 0;
  size_t i;
  tinydng_status st;

  memset(&ref, 0, sizeof(ref));
  st = decode_mem(ctx, buf, size, 1u, &ref, &segc);
  CHECK(st == TINYDNG_OK, "%s: serial decode failed (%d)", name, (int)st);
  if (st != TINYDNG_OK) {
    return;
  }
  CHECK(segc == expect_segs, "%s: segment_count=%zu expected %u", name, segc,
        expect_segs);
  CHECK(verify_raster(&ref, w, h, spp) == 0,
        "%s: serial raster mismatches pattern", name);

  for (i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
    tinydng_pixels px;
    memset(&px, 0, sizeof(px));
    st = decode_mem(ctx, buf, size, counts[i], &px, NULL);
    CHECK(st == TINYDNG_OK, "%s: decode nt=%u failed (%d)", name, counts[i],
          (int)st);
    if (st == TINYDNG_OK) {
      CHECK(px.size == ref.size &&
                memcmp(px.data, ref.data, ref.size) == 0,
            "%s: nt=%u output differs from serial", name, counts[i]);
      tinydng_pixels_free(ctx, &px);
    }
  }

  /* Region decode: a sub-window, serial vs MT, and vs the pattern. */
  {
    uint32_t rx = 20u, ry = 18u, rw = (w > 140u) ? 120u : (w / 2u),
             rh = (h > 110u) ? 90u : (h / 2u);
    tinydng_pixels r1, r8;
    memset(&r1, 0, sizeof(r1));
    memset(&r8, 0, sizeof(r8));
    st = decode_region_mem(ctx, buf, size, 1u, rx, ry, rw, rh, &r1);
    CHECK(st == TINYDNG_OK, "%s: region serial failed (%d)", name, (int)st);
    if (st == TINYDNG_OK) {
      size_t mm = 0;
      uint32_t x, y, c;
      for (y = 0; y < rh; y++) {
        for (x = 0; x < rw; x++) {
          for (c = 0; c < spp; c++) {
            if (r1.data[((size_t)y * rw + x) * spp + c] !=
                pix(rx + x, ry + y, c)) {
              mm++;
            }
          }
        }
      }
      CHECK(mm == 0, "%s: region raster mismatches pattern", name);
      st = decode_region_mem(ctx, buf, size, 8u, rx, ry, rw, rh, &r8);
      CHECK(st == TINYDNG_OK, "%s: region nt=8 failed (%d)", name, (int)st);
      if (st == TINYDNG_OK) {
        CHECK(r8.size == r1.size && memcmp(r8.data, r1.data, r1.size) == 0,
              "%s: region nt=8 differs from serial", name);
        tinydng_pixels_free(ctx, &r8);
      }
      tinydng_pixels_free(ctx, &r1);
    }
  }

  /* stdio backend (locked read path) decoded with 8 threads. */
  if (write_file(tmp_path, buf, size)) {
    tinydng_pixels ps;
    memset(&ps, 0, sizeof(ps));
    st = decode_stdio(ctx, tmp_path, 8u, &ps);
    CHECK(st == TINYDNG_OK, "%s: stdio nt=8 failed (%d)", name, (int)st);
    if (st == TINYDNG_OK) {
      CHECK(ps.size == ref.size && memcmp(ps.data, ref.data, ref.size) == 0,
            "%s: stdio nt=8 differs from serial", name);
      tinydng_pixels_free(ctx, &ps);
    }
    remove(tmp_path);
  }

  tinydng_pixels_free(ctx, &ref);
}

int main(int argc, char **argv) {
  tinydng_context *ctx = tinydng_context_create(NULL, NULL);
  uint8_t *buf;
  size_t size;
  uint32_t nseg;
  (void)argc;
  (void)argv;
  if (!ctx) {
    printf("MT: context create failed\n");
    return 1;
  }

  buf = build_strip_tiff(200u, 164u, 16u, &nseg, &size);
  CHECK(buf != NULL, "build strip fixture");
  if (buf) {
    run_fixture(ctx, "multi-strip", buf, size, 200u, 164u, 1u, nseg,
                "td_mt_strip.tmp.tiff");
    free(buf);
  }

  buf = build_tiled_tiff(200u, 164u, 64u, 48u, &nseg, &size);
  CHECK(buf != NULL, "build tiled fixture");
  if (buf) {
    run_fixture(ctx, "tiled-rgb", buf, size, 200u, 164u, 3u, nseg,
                "td_mt_tiled.tmp.tiff");
    free(buf);
  }

  tinydng_context_destroy(ctx);
  printf("MT: %s\n", g_fail ? "FAIL" : "ALL PASS");
  return g_fail ? 1 : 0;
}
