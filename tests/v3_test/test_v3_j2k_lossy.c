/* Lossy JPEG2000 DNG write (j2k_qstep > 0) and 16-bit gray DNG round-trip. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tinydng.h"

static int roundtrip(int w, int h, int spp, int bps, uint16_t photo,
                     float qstep, double max_mse, double *mse_out) {
  tinydng_context *ctx0 = tinydng_context_create(NULL, NULL);
  tinydng_write_io sink;
  uint8_t *px;
  uint8_t *buf = NULL;
  size_t buflen = 0;
  tinydng_error err;
  tinydng_status st;
  int i, bad = 0;
  double mse = 0;
  if (!ctx0) return 1;
  memset(&sink, 0, sizeof(sink));
  if (tinydng_write_io_open_memory(ctx0, &sink, &err) != TINYDNG_OK) return 1;
  px = (uint8_t *)malloc((size_t)w * h * spp * (bps / 8));
  for (i = 0; i < w * h * spp; ++i) {
    if (bps == 16) ((uint16_t *)px)[i] = (uint16_t)((i * 733 + 11) & 0xFFFF);
    else px[i] = (uint8_t)((i * 73 + 11) & 0xFF);
  }

  tinydng_write_options opts;
  memset(&opts, 0, sizeof(opts));
  opts.compression = TINYDNG_COMPRESSION_JPEG2000;
  opts.j2k_qstep = qstep;
  tinydng_write_image meta;
  memset(&meta, 0, sizeof(meta));
  meta.width = (uint32_t)w;
  meta.height = (uint32_t)h;
  meta.samples_per_pixel = (uint16_t)spp;
  meta.bits_per_sample = (uint16_t)bps;
  meta.photometric = photo;

  tinydng_writer *wr = NULL;
  st = tinydng_writer_create(ctx0, sink, &meta, &opts, NULL, &wr, &err);
  if (st != TINYDNG_OK) { fprintf(stderr, "create %d\n", st); return 1; }
  st = tinydng_writer_write_strip(wr, 0, px, &err);
  if (st != TINYDNG_OK) { fprintf(stderr, "write %d\n", st); return 1; }
  st = tinydng_writer_finish(wr, &err);
  if (st != TINYDNG_OK) { fprintf(stderr, "finish %d\n", st); return 1; }
  if (tinydng_write_io_memory_take(ctx0, &sink, &buf, &buflen, &err) != TINYDNG_OK)
    return 1;

  {
    tinydng_context *ctx = tinydng_context_create(NULL, NULL);
    tinydng_document *doc = NULL;
    st = tinydng_open_memory(ctx, buf, buflen, NULL, &doc, &err);
    if (st != TINYDNG_OK) { fprintf(stderr, "open %d\n", st); return 1; }
    {
      tinydng_decode_options dopts;
      memset(&dopts, 0, sizeof(dopts));
      tinydng_pixels out;
      memset(&out, 0, sizeof(out));
      st = tinydng_decode_image(ctx, doc, 0, &dopts, &out, &err);
      if (st != TINYDNG_OK) { fprintf(stderr, "decode %d\n", st); return 1; }
      for (i = 0; i < w * h * spp; ++i) {
        int64_t d;
        int32_t v;
        if (bps == 16) v = ((uint16_t *)out.data)[i];
        else v = out.data[i];
        if (bps == 16) d = (int64_t)v - ((uint16_t *)px)[i];
        else d = (int64_t)v - (int32_t)px[i];
        if (d != 0) bad++;
        mse += (double)d * (double)d;
      }
      mse /= (double)(w * h * spp);
      tinydng_pixels_free(ctx, &out);
    }
    tinydng_document_destroy(ctx, doc);
    tinydng_context_destroy(ctx);
  }
  free(px);
  tinydng_context_destroy(ctx0);
  if (mse_out) *mse_out = mse;
  /* lossless: exact; lossy: within the MSE budget */
  if (max_mse < 0) return bad ? 1 : 0;
  return (mse <= max_mse) ? 0 : 1;
}

int main(void) {
  double mse = 0;
  if (roundtrip(48, 32, 3, 8, 2, 0.0f, -1, NULL)) { fprintf(stderr, "8-bit RGB lossless fail\n"); return 1; }
  if (roundtrip(64, 64, 1, 16, 1, 0.0f, -1, NULL)) { fprintf(stderr, "16-bit gray lossless fail\n"); return 1; }
  if (roundtrip(64, 64, 1, 16, 32803, 0.0f, -1, NULL)) { fprintf(stderr, "16-bit CFA lossless fail\n"); return 1; }
  if (roundtrip(64, 64, 3, 8, 2, 0.02f, 30.0, &mse)) { fprintf(stderr, "lossy qstep=0.02 fail mse=%g\n", mse); return 1; }
  printf("v3 J2K DNG lossy + 16-bit round-trips: OK (lossy mse=%g)\n", mse);
  return 0;
}
