/* gen_v3_psd_corpus.c - generate a PSD/PSB seed corpus for fuzz-v3-psd.
 *
 * Emits small, valid PSD/PSB files across the depth x color-mode x
 * compression x flavor matrix (via the writer), plus a layered file and a
 * PSD-embedding-PSD smart object seed. Good seeds let the fuzzer explore
 * around each parser/decoder path.
 *
 *   cc -std=c11 -I.. fuzzer/gen_v3_psd_corpus.c tinydng_api.c tinydng_io.c \
 *      tinydng_tiff.c tinydng_dng.c tinydng_codec.c tinydng_write.c \
 *      tinydng_psd.c tinydng_psd_write.c tinydng_miniz.c tinydng_stb_image.c \
 *      tiny_dng_ljpeg92_v2.c -o gen_psd
 *   ./gen_psd <outdir>
 */
#include "../tinydng.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_n = 0;
static void dump(const char *dir, const char *name, const uint8_t *buf,
                 size_t n) {
  char path[1024];
  FILE *f;
  snprintf(path, sizeof(path), "%s/psd_%02d_%s", dir, g_n++, name);
  f = fopen(path, "wb");
  if (!f) {
    fprintf(stderr, "cannot write %s\n", path);
    return;
  }
  fwrite(buf, 1, n, f);
  fclose(f);
  printf("  %s (%zu bytes)\n", path, n);
}

static void gen_one(tinydng_context *ctx, const char *dir, const char *name,
                    uint16_t depth, uint16_t channels, uint16_t mode,
                    uint16_t comp, int psb, int with_layer) {
  tinydng_error e;
  uint32_t W = 6, H = 5;
  size_t sb = (size_t)depth / 8u;
  size_t cbytes = (size_t)W * H * channels * sb;
  size_t lbytes = (size_t)W * H * sb;
  uint8_t *composite = (uint8_t *)malloc(cbytes);
  uint8_t *lp = (uint8_t *)malloc(lbytes);
  tinydng_psd_write_channel wch[2];
  tinydng_psd_write_layer layer;
  tinydng_psd_write_doc wd;
  tinydng_psd_write_options wo;
  uint8_t *blob = NULL;
  size_t blob_len = 0;
  size_t i;

  for (i = 0; i < cbytes; i++) {
    composite[i] = (uint8_t)(i * 37u + 5u);
  }
  for (i = 0; i < lbytes; i++) {
    lp[i] = (uint8_t)(i * 91u + 3u);
  }

  memset(wch, 0, sizeof(wch));
  wch[0].id = 0;
  wch[0].data = lp;
  wch[0].size = lbytes;
  wch[1].id = -1;
  wch[1].data = lp;
  wch[1].size = lbytes;
  memset(&layer, 0, sizeof(layer));
  layer.top = 0;
  layer.left = 0;
  layer.bottom = (int32_t)H;
  layer.right = (int32_t)W;
  layer.name = "seed";
  layer.channels = wch;
  layer.channel_count = 2;

  memset(&wd, 0, sizeof(wd));
  wd.width = W;
  wd.height = H;
  wd.depth = depth;
  wd.color_mode = mode;
  wd.channel_count = channels;
  wd.composite = composite;
  wd.composite_size = cbytes;
  if (with_layer) {
    wd.layers = &layer;
    wd.layer_count = 1;
  }
  memset(&wo, 0, sizeof(wo));
  wo.compression = comp;
  wo.as_psb = (uint8_t)psb;

  if (tinydng_psd_write_memory(ctx, &wd, &wo, &blob, &blob_len, &e) ==
      TINYDNG_OK) {
    dump(dir, name, blob, blob_len);
    tinydng_buffer_free(ctx, blob);
  } else {
    fprintf(stderr, "gen %s failed: %s\n", name, e.message);
  }
  free(composite);
  free(lp);
}

int main(int argc, char **argv) {
  const char *dir = (argc > 1) ? argv[1] : ".";
  tinydng_error e;
  tinydng_context *ctx = tinydng_context_create(NULL, &e);
  if (!ctx) {
    return 1;
  }
  printf("generating PSD seeds in %s\n", dir);

  gen_one(ctx, dir, "rgb8_raw.psd", 8, 3, TINYDNG_PSD_RGB,
          TINYDNG_PSD_COMP_RAW, 0, 0);
  gen_one(ctx, dir, "rgb8_rle.psd", 8, 3, TINYDNG_PSD_RGB,
          TINYDNG_PSD_COMP_RLE, 0, 1);
  gen_one(ctx, dir, "rgba8_rle.psd", 8, 4, TINYDNG_PSD_RGB,
          TINYDNG_PSD_COMP_RLE, 0, 1);
  gen_one(ctx, dir, "gray8_rle.psd", 8, 1, TINYDNG_PSD_GRAYSCALE,
          TINYDNG_PSD_COMP_RLE, 0, 1);
  gen_one(ctx, dir, "cmyk8_raw.psd", 8, 4, TINYDNG_PSD_CMYK,
          TINYDNG_PSD_COMP_RAW, 0, 1);
  gen_one(ctx, dir, "rgb16_rle.psd", 16, 3, TINYDNG_PSD_RGB,
          TINYDNG_PSD_COMP_RLE, 0, 1);
  gen_one(ctx, dir, "gray16_raw.psd", 16, 1, TINYDNG_PSD_GRAYSCALE,
          TINYDNG_PSD_COMP_RAW, 0, 1);
  gen_one(ctx, dir, "rgb32_raw.psd", 32, 3, TINYDNG_PSD_RGB,
          TINYDNG_PSD_COMP_RAW, 0, 1);
  gen_one(ctx, dir, "gray32_rle.psd", 32, 1, TINYDNG_PSD_GRAYSCALE,
          TINYDNG_PSD_COMP_RLE, 0, 1);
  gen_one(ctx, dir, "rgb8_rle.psb", 8, 3, TINYDNG_PSD_RGB,
          TINYDNG_PSD_COMP_RLE, 1, 1);
  gen_one(ctx, dir, "rgb16_raw.psb", 16, 3, TINYDNG_PSD_RGB,
          TINYDNG_PSD_COMP_RAW, 1, 1);
  gen_one(ctx, dir, "multichan8.psd", 8, 5, TINYDNG_PSD_MULTICHANNEL,
          TINYDNG_PSD_COMP_RLE, 0, 0);

  tinydng_context_destroy(ctx);
  printf("done: %d seeds\n", g_n);
  return 0;
}
