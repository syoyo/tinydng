/* Round-trip a tiled J2K DNG with edge tiles (image dims not a multiple of
   the tile dims). Covers the 0xFF-padding packet-header handling in the
   J2K decoder. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tinydng.h"

int main(void) {
  tinydng_context *ctx0 = tinydng_context_create(NULL, NULL);
  tinydng_write_io sink;
  int W = 300, H = 200, spp = 3, bps = 8;
  int tw = 128, th = 128;
  int across = (W + tw - 1) / tw;
  int down = (H + th - 1) / th;
  uint8_t *px;
  uint8_t *buf = NULL;
  size_t buflen = 0;
  tinydng_error err;
  tinydng_status st;
  int ty, tx, i, bad = 0;

  if (!ctx0) return 1;
  memset(&sink, 0, sizeof(sink));
  if (tinydng_write_io_open_memory(ctx0, &sink, &err) != TINYDNG_OK) return 1;
  px = (uint8_t *)malloc((size_t)W * H * spp);
  for (i = 0; i < W * H * spp; ++i) px[i] = (uint8_t)((i * 73 + 11) & 0xFF);

  tinydng_write_options opts;
  memset(&opts, 0, sizeof(opts));
  opts.compression = TINYDNG_COMPRESSION_JPEG2000;
  tinydng_write_image meta;
  memset(&meta, 0, sizeof(meta));
  meta.width = (uint32_t)W;
  meta.height = (uint32_t)H;
  meta.samples_per_pixel = spp;
  meta.bits_per_sample = bps;
  meta.photometric = 2; /* RGB */
  tinydng_tiling tiling;
  memset(&tiling, 0, sizeof(tiling));
  tiling.tile_width = (uint32_t)tw;
  tiling.tile_length = (uint32_t)th;

  tinydng_writer *wr = NULL;
  st = tinydng_writer_create(ctx0, sink, &meta, &opts, &tiling, &wr, &err);
  if (st != TINYDNG_OK) { fprintf(stderr, "create %d\n", st); return 1; }
  for (ty = 0; ty < down; ++ty) {
    for (tx = 0; tx < across; ++tx) {
      uint32_t idx = (uint32_t)(ty * across + tx);
      int pw = tw, ph = th;
      uint8_t *tb;
      int yy;
      if (tx == across - 1) pw = W - tx * tw;
      if (ty == down - 1) ph = H - ty * th;
      tb = (uint8_t *)malloc((size_t)pw * ph * spp);
      for (yy = 0; yy < ph; ++yy)
        memcpy(tb + (size_t)yy * pw * spp,
               px + (size_t)(ty * th + yy) * W * spp + (size_t)tx * tw * spp,
               (size_t)pw * spp);
      st = tinydng_writer_write_tile(wr, idx, tb, &err);
      free(tb);
      if (st != TINYDNG_OK) { fprintf(stderr, "tile %u write %d\n", idx, st); return 1; }
    }
  }
  st = tinydng_writer_finish(wr, &err);
  if (st != TINYDNG_OK) { fprintf(stderr, "finish %d\n", st); return 1; }
  if (tinydng_write_io_memory_take(ctx0, &sink, &buf, &buflen, &err) != TINYDNG_OK)
    return 1;
  if (!buf || buflen == 0) return 1;

  {
    tinydng_context *ctx = tinydng_context_create(NULL, NULL);
    tinydng_document *doc = NULL;
    st = tinydng_open_memory(ctx, buf, buflen, NULL, &doc, &err);
    if (st != TINYDNG_OK) { fprintf(stderr, "open %d\n", st); return 1; }
    {
      tinydng_decode_options dopts;
      memset(&dopts, 0, sizeof(dopts));
      dopts.num_threads = 4;
      tinydng_pixels out;
      memset(&out, 0, sizeof(out));
      st = tinydng_decode_image(ctx, doc, 0, &dopts, &out, &err);
      if (st != TINYDNG_OK) { fprintf(stderr, "decode %d\n", st); return 1; }
      for (i = 0; i < W * H * spp; ++i)
        if ((uint8_t)out.data[i] != px[i]) bad++;
      tinydng_pixels_free(ctx, &out);
    }
    tinydng_document_destroy(ctx, doc);
    tinydng_context_destroy(ctx);
  }
  free(px);
  tinydng_context_destroy(ctx0);
  if (bad) { fprintf(stderr, "%d mismatches\n", bad); return 1; }
  printf("v3 tiled J2K DNG (edge tiles) write round-trip: OK\n");
  return 0;
}
