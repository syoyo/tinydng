/*
 * fuzz-v3-psd-write.c - libFuzzer harness for the PSD WRITER + round-trip.
 * Derives a write_doc (dims/depth/mode/compression/PSB/layers) from the fuzz
 * input, writes it, re-opens the result and asserts the decoded composite and
 * layer bytes are identical to what was written -- silent corruption fails,
 * not just crashes/UB/leaks.
 *
 *   clang -std=c11 -g -O1 -fsanitize=address,undefined,fuzzer -fno-sanitize-recover=all -I.. \
 *     fuzz-v3-psd-write.c ../tinydng_api.c ../tinydng_io.c ../tinydng_tiff.c \
 *     ../tinydng_dng.c ../tinydng_codec.c ../tinydng_write.c ../tinydng_psd.c \
 *     ../tinydng_psd_write.c ../tinydng_miniz.c ../tinydng_stb_image.c \
 *     ../tiny_dng_ljpeg92_v2.c -o fuzz-v3-psd-write
 */
#include "../tinydng.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  tinydng_config cfg;
  tinydng_context *ctx;
  tinydng_error err;
  tinydng_psd_write_doc wd;
  tinydng_psd_write_options wo;
  tinydng_psd_write_channel wch;
  tinydng_psd_write_layer layer;
  uint8_t *blob = NULL;
  size_t blob_len = 0;
  uint8_t *composite = NULL;
  uint8_t *plane = NULL;
  static const uint16_t kDepths[] = {8, 16, 32};
  static const uint16_t kModes[] = {TINYDNG_PSD_GRAYSCALE, TINYDNG_PSD_RGB,
                                    TINYDNG_PSD_CMYK,
                                    TINYDNG_PSD_MULTICHANNEL};
  uint32_t W, H;
  uint16_t depth, mode, channels;
  size_t sample_bytes, comp_bytes, plane_bytes, i;
  char name[24];

  if (size <= 8u) {
    return 0; /* need at least one pixel-source byte after the 8-byte header */
  }
  W = 1u + (data[0] % 40u);
  H = 1u + (data[1] % 40u);
  depth = kDepths[data[2] % 3u];
  mode = kModes[data[3] % 4u];
  channels = (uint16_t)(1u + (data[4] % 5u));

  memset(&cfg, 0, sizeof(cfg));
  cfg.memory_cap_bytes = 128u * 1024u * 1024u;
  ctx = tinydng_context_create(&cfg, &err);
  if (!ctx) {
    return 0;
  }

  sample_bytes = (size_t)depth / 8u;
  comp_bytes = (size_t)W * H * channels * sample_bytes;
  plane_bytes = (size_t)W * H * sample_bytes;
  composite = (uint8_t *)malloc(comp_bytes);
  plane = (uint8_t *)malloc(plane_bytes);
  if (!composite || !plane) {
    free(composite);
    free(plane);
    tinydng_context_destroy(ctx);
    return 0;
  }
  /* Pixels from the remaining input, repeated. */
  for (i = 0; i < comp_bytes; i++) {
    composite[i] = data[8u + (i % (size - 8u))];
  }
  for (i = 0; i < plane_bytes; i++) {
    plane[i] = data[8u + ((i * 3u + 1u) % (size - 8u))];
  }
  for (i = 0; i < sizeof(name) - 1u; i++) {
    name[i] = (char)(32u + (data[8u + (i % (size - 8u))] % 95u));
  }
  name[sizeof(name) - 1u] = '\0';

  memset(&wch, 0, sizeof(wch));
  wch.id = 0;
  wch.data = plane;
  wch.size = plane_bytes;
  memset(&layer, 0, sizeof(layer));
  layer.top = (int8_t)data[5];
  layer.left = (int8_t)data[6];
  layer.bottom = layer.top + (int32_t)H;
  layer.right = layer.left + (int32_t)W;
  layer.name = name;
  layer.channels = &wch;
  layer.channel_count = 1;

  memset(&wd, 0, sizeof(wd));
  wd.width = W;
  wd.height = H;
  wd.depth = depth;
  wd.color_mode = mode;
  wd.channel_count = channels;
  wd.composite = composite;
  wd.composite_size = comp_bytes;
  wd.layers = &layer;
  wd.layer_count = (data[7] & 1u) ? 1u : 0u;
  memset(&wo, 0, sizeof(wo));
  wo.compression = (data[7] & 2u) ? TINYDNG_PSD_COMP_RLE
                                  : TINYDNG_PSD_COMP_RAW;
  wo.as_psb = (uint8_t)((data[7] >> 2) & 1u);

  if (tinydng_psd_write_memory(ctx, &wd, &wo, &blob, &blob_len, &err) ==
      TINYDNG_OK) {
    tinydng_document *doc = NULL;
    if (tinydng_open_memory(ctx, blob, blob_len, NULL, &doc, &err) !=
        TINYDNG_OK) {
      assert(0 && "writer output must reopen");
    } else {
      tinydng_pixels px;
      const tinydng_psd_info *psd = tinydng_document_psd(doc);
      assert(psd && psd->width == W && psd->height == H &&
             psd->depth == depth);
      if (tinydng_decode_image(ctx, doc, 0, NULL, &px, &err) != TINYDNG_OK) {
        assert(0 && "composite must decode");
      } else {
        assert(px.size == comp_bytes);
        assert(memcmp(px.data, composite, comp_bytes) == 0);
        tinydng_pixels_free(ctx, &px);
      }
      if (wd.layer_count == 1u) {
        assert(psd->layer_count == 1u);
        if (tinydng_psd_decode_layer(ctx, doc, 0, NULL, &px, &err) !=
            TINYDNG_OK) {
          assert(0 && "layer must decode");
        } else {
          assert(px.size == plane_bytes);
          assert(memcmp(px.data, plane, plane_bytes) == 0);
          tinydng_pixels_free(ctx, &px);
        }
        assert(strcmp(psd->layers[0].name, name) == 0);
      }
      tinydng_document_destroy(ctx, doc);
    }
    tinydng_buffer_free(ctx, blob);
  }
  free(composite);
  free(plane);
  assert(tinydng_context_memory_used(ctx) == 0u);
  tinydng_context_destroy(ctx);
  return 0;
}
