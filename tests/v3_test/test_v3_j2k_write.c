/* Round-trip a DNG written with Compression=JPEG2000 (34712). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tinydng.h"

int main(void) {
  tinydng_context *ctx0 = tinydng_context_create(NULL, NULL);
  tinydng_write_io sink;
  int w = 48, h = 32, spp = 3, bps = 8;
  uint8_t *px;
  uint8_t *buf = NULL;
  size_t buflen = 0;
  tinydng_error err;
  tinydng_status st;
  int i, bad = 0;

  if (!ctx0) return 1;
  memset(&sink, 0, sizeof(sink));
  if (tinydng_write_io_open_memory(ctx0, &sink, &err) != TINYDNG_OK) return 1;
  px = (uint8_t *)malloc((size_t)w * h * spp);
  for (i = 0; i < w * h * spp; ++i) px[i] = (uint8_t)((i * 73 + 11) & 0xFF);

  tinydng_write_options opts;
  memset(&opts, 0, sizeof(opts));
  opts.compression = TINYDNG_COMPRESSION_JPEG2000;
  tinydng_write_image meta;
  memset(&meta, 0, sizeof(meta));
  meta.width = (uint32_t)w;
  meta.height = (uint32_t)h;
  meta.samples_per_pixel = spp;
  meta.bits_per_sample = bps;
  meta.photometric = 2; /* RGB */

  tinydng_writer *wr = NULL;
  st = tinydng_writer_create(ctx0, sink, &meta, &opts, NULL, &wr, &err);
  if (st != TINYDNG_OK) { fprintf(stderr, "create %d\n", st); return 1; }
  st = tinydng_writer_write_strip(wr, 0, px, &err);
  if (st != TINYDNG_OK) { fprintf(stderr, "write %d\n", st); return 1; }
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
    tinydng_decode_options dopts;
    memset(&dopts, 0, sizeof(dopts));
    tinydng_pixels out;
    memset(&out, 0, sizeof(out));
    st = tinydng_decode_image(ctx, doc, 0, &dopts, &out, &err);
    if (st != TINYDNG_OK) { fprintf(stderr, "decode %d\n", st); return 1; }
    for (i = 0; i < w * h * spp; ++i)
      if ((uint8_t)out.data[i] != px[i]) bad++;
    tinydng_pixels_free(ctx, &out);
    tinydng_document_destroy(ctx, doc);
    tinydng_context_destroy(ctx);
  }
  free(px);
  tinydng_context_destroy(ctx0);
  if (bad) { fprintf(stderr, "%d mismatches\n", bad); return 1; }
  printf("v3 J2K DNG write round-trip: OK\n");
  return 0;
}
