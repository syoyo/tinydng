/*
 * fuzz-v3-psd.c - libFuzzer harness for the PSD/PSB parser + decoders.
 * Walks everything the PSD module exposes: composite decode, every layer
 * (interleaved + per channel), thumbnail, smart-object enumeration and a
 * depth-capped recursive open/decode.
 *
 *   clang -std=c11 -g -O1 -fsanitize=address,undefined,fuzzer -fno-sanitize-recover=all -I.. \
 *     fuzz-v3-psd.c ../tinydng_api.c ../tinydng_io.c ../tinydng_tiff.c \
 *     ../tinydng_dng.c ../tinydng_codec.c ../tinydng_write.c ../tinydng_psd.c \
 *     ../tinydng_psd_write.c ../tinydng_miniz.c ../tinydng_stb_image.c \
 *     ../tiny_dng_ljpeg92_v2.c -o fuzz-v3-psd
 *
 * Seeds: gen_v3_psd_corpus + fuzzer/v3-psd.dict.
 */
#include "../tinydng.h"

#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  tinydng_config cfg;
  tinydng_context *ctx;
  tinydng_document *doc = NULL;
  tinydng_error err;

  memset(&cfg, 0, sizeof(cfg));
  cfg.memory_cap_bytes = 256u * 1024u * 1024u;
  cfg.max_image_pixels = 1u << 24; /* keep decode buffers small-ish */
  cfg.max_psd_layers = 256;
  cfg.max_embed_depth = 2;

  ctx = tinydng_context_create(&cfg, &err);
  if (!ctx) {
    return 0;
  }

  if (tinydng_open_memory(ctx, data, size, NULL, &doc, &err) == TINYDNG_OK) {
    const tinydng_psd_info *psd = tinydng_document_psd(doc);
    tinydng_pixels px;
    if (tinydng_decode_image(ctx, doc, 0, NULL, &px, &err) == TINYDNG_OK) {
      tinydng_pixels_free(ctx, &px);
    }
    if (psd) {
      size_t i;
      for (i = 0; i < psd->layer_count; i++) {
        size_t c;
        if (tinydng_psd_decode_layer(ctx, doc, i, NULL, &px, &err) ==
            TINYDNG_OK) {
          tinydng_pixels_free(ctx, &px);
        }
        for (c = 0; c < psd->layers[i].channel_count && c < 4u; c++) {
          if (tinydng_psd_decode_layer_channel(ctx, doc, i, c, NULL, &px,
                                               &err) == TINYDNG_OK) {
            tinydng_pixels_free(ctx, &px);
          }
        }
      }
      if (tinydng_psd_decode_thumbnail(ctx, doc, &px, &err) == TINYDNG_OK) {
        tinydng_pixels_free(ctx, &px);
      }
      for (i = 0; i < psd->smart_object_count; i++) {
        tinydng_document *child = NULL;
        if (tinydng_psd_smart_object_open(ctx, doc, i, NULL, &child, &err) ==
            TINYDNG_OK) {
          if (tinydng_decode_image(ctx, child, 0, NULL, &px, &err) ==
              TINYDNG_OK) {
            tinydng_pixels_free(ctx, &px);
          }
          tinydng_document_destroy(ctx, child);
        }
        if (tinydng_psd_smart_object_decode(ctx, doc, i, &px, &err) ==
            TINYDNG_OK) {
          tinydng_pixels_free(ctx, &px);
        }
      }
    }
    tinydng_document_destroy(ctx, doc);
  }
  tinydng_context_destroy(ctx);
  return 0;
}
