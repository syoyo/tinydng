/*
 * bench.c - streaming encode throughput: tiled lossless-JPEG DNG writing
 * (tinydng_writer_*) vs one-shot tinydng_write_memory, plus the codec-level
 * streaming encoder (tdng_lj92_encode_*) vs one-shot tdng_lj92_encode_ex.
 *
 * usage: bench <lossless_16bit_dng_or_tiff> [iterations]
 *
 * The image is decoded once (largest >8-bit LJPEG image), then re-encoded
 * every iteration. Figures are MPix/s of the source pixels.
 */
#include "tinydng.h"
#include "tiny_dng_ljpeg92_v2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

typedef struct {
  uint32_t w, h;
  uint16_t spp;
  tinydng_pixels px;
} bench_img;

static int load_ljpeg_image(tinydng_context *ctx, const char *path,
                            bench_img *out) {
  tinydng_open_options oopts;
  tinydng_document *doc = NULL;
  tinydng_error err;
  size_t i, n, best = 0;
  uint64_t best_px = 0;
  int found = 0;
  memset(&oopts, 0, sizeof(oopts));
  oopts.flags = TINYDNG_OPEN_PARSE_SUBIFDS;
  if (tinydng_open_file(ctx, path, &oopts, &doc, &err) != TINYDNG_OK) {
    fprintf(stderr, "open failed: %s\n", err.message);
    return 0;
  }
  n = tinydng_image_count(doc);
  for (i = 0; i < n; i++) {
    const tinydng_image_info *im = tinydng_image_get(doc, i);
    if ((im->compression == TINYDNG_COMPRESSION_OLD_JPEG ||
         im->compression == TINYDNG_COMPRESSION_NEW_JPEG) &&
        im->bits_per_sample > 8u) {
      uint64_t px_ = (uint64_t)im->width * im->height;
      if (!found || px_ > best_px) {
        found = 1;
        best = i;
        best_px = px_;
      }
    }
  }
  if (!found) {
    fprintf(stderr, "no lossless 16-bit image found\n");
    tinydng_document_destroy(ctx, doc);
    return 0;
  }
  if (tinydng_decode_image(ctx, doc, best, NULL, &out->px, &err) !=
      TINYDNG_OK) {
    fprintf(stderr, "decode failed: %s\n", err.message);
    tinydng_document_destroy(ctx, doc);
    return 0;
  }
  {
    const tinydng_image_info *im = tinydng_image_get(doc, best);
    out->w = im->width;
    out->h = im->height;
    out->spp = im->samples_per_pixel;
  }
  tinydng_document_destroy(ctx, doc);
  return 1;
}

/* Tiled lossless-JPEG DNG via the streaming writer, memory sink. */
static int bench_stream_write(tinydng_context *ctx, const bench_img *img,
                              uint32_t iterations, double *mpix_out,
                              double *bytes_out) {
  const uint32_t TW = 512, TL = 512;
  const uint32_t across = (img->w + TW - 1u) / TW;
  const uint32_t down = (img->h + TL - 1u) / TL;
  const size_t sb = (size_t)img->px.bits_per_sample / 8u;
  uint8_t *tilebuf =
      (uint8_t *)malloc((size_t)TW * TL * img->spp * sb);
  double t0, t1;
  uint32_t it;
  double total_bytes = 0.0;
  if (!tilebuf) {
    return 0;
  }
  memset(tilebuf, 0, (size_t)TW * TL * img->spp * sb);
  t0 = now_ms();
  for (it = 0; it < iterations; it++) {
    tinydng_write_image meta;
    tinydng_write_options opts;
    tinydng_tiling tiling;
    tinydng_write_io io;
    tinydng_writer *w = NULL;
    tinydng_error err;
    uint8_t *blob = NULL;
    size_t blob_len = 0;
    uint32_t t;
    memset(&meta, 0, sizeof(meta));
    meta.width = img->w;
    meta.height = img->h;
    meta.samples_per_pixel = img->spp;
    meta.bits_per_sample = img->px.bits_per_sample;
    meta.sample_format = TINYDNG_SAMPLEFORMAT_UINT;
    memset(&opts, 0, sizeof(opts));
    opts.as_dng = 1;
    opts.compression = TINYDNG_COMPRESSION_NEW_JPEG;
    memset(&tiling, 0, sizeof(tiling));
    tiling.tile_width = TW;
    tiling.tile_length = TL;
    if (tinydng_write_io_open_memory(ctx, &io, &err) != TINYDNG_OK ||
        tinydng_writer_create(ctx, io, &meta, &opts, &tiling, &w, &err) !=
            TINYDNG_OK) {
      fprintf(stderr, "writer create failed: %s\n", err.message);
      free(tilebuf);
      return 0;
    }
    for (t = 0; t < across * down; t++) {
      if (tinydng_writer_write_tile(w, t, tilebuf, &err) != TINYDNG_OK) {
        fprintf(stderr, "tile write failed: %s\n", err.message);
        tinydng_writer_finish(w, &err);
        io.close(&io);
        free(tilebuf);
        return 0;
      }
    }
    if (tinydng_writer_finish(w, &err) != TINYDNG_OK ||
        tinydng_write_io_memory_take(ctx, &io, &blob, &blob_len, &err) !=
            TINYDNG_OK) {
      fprintf(stderr, "finish/take failed: %s\n", err.message);
      io.close(&io);
      free(tilebuf);
      return 0;
    }
    io.close(&io);
    total_bytes += (double)blob_len;
    tinydng_buffer_free(ctx, blob);
  }
  t1 = now_ms();
  free(tilebuf);
  *mpix_out = (double)img->w * (double)img->h * (double)iterations /
              (t1 - t0) / 1e3;
  *bytes_out = total_bytes / (double)iterations;
  return 1;
}

/* Growable-buffer sink for the codec-level streaming encode. */
typedef struct mbuf {
  uint8_t *data;
  size_t cap;
  size_t len;
} mbuf;

static size_t mbuf_write(void *u, const void *d, size_t n) {
  mbuf *m = (mbuf *)u;
  if (m->len + n > m->cap) {
    size_t cap = m->cap ? m->cap : 4096u;
    while (cap < m->len + n) cap *= 2u;
    {
      uint8_t *nb = (uint8_t *)realloc(m->data, cap);
      if (!nb) return 0;
      m->data = nb;
      m->cap = cap;
    }
  }
  memcpy(m->data + m->len, d, n);
  m->len += n;
  return n;
}

static int stream_encode_one(const uint16_t *tile, int w, int h, int comps,
                             uint8_t **out, size_t *out_len) {
  mbuf m;
  tdng_lj92_enc lj = NULL;
  int ret;
  m.data = NULL;
  m.cap = 0;
  m.len = 0;
  ret = tdng_lj92_encode_open(&lj, w, h, 16, comps, 1, w * comps, 0, &m,
                              mbuf_write);
  if (ret == TDNG_LJ92_ERROR_NONE) ret = tdng_lj92_encode_scan(lj, tile, NULL, 0);
  if (ret == TDNG_LJ92_ERROR_NONE) ret = tdng_lj92_encode_begin(lj);
  if (ret == TDNG_LJ92_ERROR_NONE) ret = tdng_lj92_encode_rows(lj, tile, 0, h);
  if (ret == TDNG_LJ92_ERROR_NONE) ret = tdng_lj92_encode_finish(lj);
  else tdng_lj92_encode_finish(lj);
  if (ret != TDNG_LJ92_ERROR_NONE) {
    free(m.data);
    return -1;
  }
  *out = m.data;
  *out_len = m.len;
  return 0;
}

/* Codec-level: streaming encoder vs one-shot, per tile. */
static int bench_codec(const bench_img *img, uint32_t iterations,
                       double *stream_mpix, double *oneshot_mpix) {
  const uint32_t TW = 512, TL = 512;
  const uint32_t across = (img->w + TW - 1u) / TW;
  const uint32_t down = (img->h + TL - 1u) / TL;
  size_t tile_px = (size_t)TW * TL * img->spp;
  uint16_t *tile = (uint16_t *)malloc(tile_px * sizeof(uint16_t));
  double t0, t1;
  uint32_t it, t;
  double total = (double)img->w * (double)img->h * (double)iterations;
  if (!tile) {
    return 0;
  }
  memset(tile, 0, tile_px * sizeof(uint16_t));

  /* streaming */
  t0 = now_ms();
  for (it = 0; it < iterations; it++) {
    for (t = 0; t < across * down; t++) {
      uint8_t *buf = NULL;
      size_t len = 0;
      if (stream_encode_one(tile, TW, TL, img->spp, &buf, &len) != 0) {
        fprintf(stderr, "stream encode failed\n");
        free(tile);
        return 0;
      }
      free(buf);
    }
  }
  t1 = now_ms();
  *stream_mpix = total / (t1 - t0) / 1e3;

  /* one-shot */
  t0 = now_ms();
  for (it = 0; it < iterations; it++) {
    for (t = 0; t < across * down; t++) {
      uint8_t *buf = NULL;
      int len = 0;
      if (tdng_lj92_encode_ex(tile, TW, TL, 16, img->spp, 1, TW * img->spp, 0,
                              NULL, 0, &buf, &len) != TDNG_LJ92_ERROR_NONE) {
        fprintf(stderr, "one-shot encode failed\n");
        free(tile);
        return 0;
      }
      free(buf);
    }
  }
  t1 = now_ms();
  *oneshot_mpix = total / (t1 - t0) / 1e3;
  free(tile);
  return 1;
}

int main(int argc, char **argv) {
  tinydng_context *ctx;
  bench_img img;
  uint32_t iterations;
  double mpix, bytes, smpix, ompix;

  if (argc < 2) {
    fprintf(stderr, "usage: %s <lossless_16bit_dng> [iterations]\n", argv[0]);
    return 1;
  }
  iterations = (argc > 2) ? (uint32_t)atoi(argv[2]) : 10u;
  ctx = tinydng_context_create(NULL, NULL);
  if (!ctx) {
    return 1;
  }
  memset(&img, 0, sizeof(img));
  if (!load_ljpeg_image(ctx, argv[1], &img)) {
    tinydng_context_destroy(ctx);
    return 1;
  }
  printf("image=%ux%u spp=%u bps=%u iterations=%u\n", img.w, img.h, img.spp,
         img.px.bits_per_sample, iterations);
  printf("pixels=%llu (%.1f MPix)\n",
         (unsigned long long)img.w * img.h,
         (double)img.w * img.h / 1e6);

  if (!bench_stream_write(ctx, &img, iterations, &mpix, &bytes)) {
    tinydng_pixels_free(ctx, &img.px);
    tinydng_context_destroy(ctx);
    return 1;
  }
  printf("stream writer (tiled LJPEG DNG): %7.2f MPix/s, %8.1f KB/frame\n",
         mpix, bytes / 1024.0);

  if (!bench_codec(&img, iterations, &smpix, &ompix)) {
    tinydng_pixels_free(ctx, &img.px);
    tinydng_context_destroy(ctx);
    return 1;
  }
  printf("codec streaming encode:        %7.2f MPix/s\n", smpix);
  printf("codec one-shot encode:         %7.2f MPix/s\n", ompix);

  tinydng_pixels_free(ctx, &img.px);
  tinydng_context_destroy(ctx);
  return 0;
}
