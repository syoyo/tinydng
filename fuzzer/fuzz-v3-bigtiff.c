/*
 * fuzz-v3-bigtiff.c - libFuzzer harness for BigTIFF write+read round-trip and
 *                     direct reader fuzzing with BigTIFF binary data.
 *
 *   clang -std=c11 -g -O1 -fsanitize=address,undefined,fuzzer -I.. \
 *     fuzz-v3-bigtiff.c ../tinydng_api.c ../tinydng_io.c ../tinydng_tiff.c \
 *     ../tinydng_dng.c ../tinydng_codec.c ../tinydng_write.c ../tinydng_miniz.c \
 *     ../tinydng_stb_image.c ../tiny_dng_ljpeg92_v2.c -o fuzz-v3-bigtiff
 */
#include "../tinydng.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  tinydng_config cfg;
  tinydng_context *ctx;
  tinydng_error err;

  memset(&cfg, 0, sizeof(cfg));
  cfg.memory_cap_bytes = 64u * 1024u * 1024u;
  cfg.max_images = 8;
  ctx = tinydng_context_create(&cfg, NULL);
  if (!ctx) {
    return 0;
  }

  /* --- Path A: direct reader fuzzing with raw BigTIFF data --- */
  {
    tinydng_document *doc = NULL;
    tinydng_open_options opts;
    memset(&opts, 0, sizeof(opts));
    if (size > 0 && (data[0] & 1u)) {
      opts.flags |= TINYDNG_OPEN_PARSE_SUBIFDS;
    }
    if (tinydng_open_memory(ctx, data, size, &opts, &doc, &err) == TINYDNG_OK) {
      size_t i, n = tinydng_image_count(doc);
      for (i = 0; i < n; i++) {
        tinydng_pixels px;
        if (tinydng_decode_image(ctx, doc, i, NULL, &px, &err) == TINYDNG_OK) {
          tinydng_pixels_free(ctx, &px);
        }
        /* Also exercise the KEEP_PACKED path (sub-byte packing preserved). */
        {
          tinydng_decode_options pkopts;
          tinydng_pixels pxp;
          memset(&pkopts, 0, sizeof(pkopts));
          pkopts.flags = TINYDNG_DEC_KEEP_PACKED;
          if (tinydng_decode_image(ctx, doc, i, &pkopts, &pxp, &err) ==
              TINYDNG_OK) {
            tinydng_pixels_free(ctx, &pxp);
          }
        }
      }
      tinydng_document_destroy(ctx, doc);
    }
  }

  /* --- Path B: write BigTIFF from fuzz-derived config, read back, verify --- */
  if (size >= 16u) {
    const uint8_t *h = data;
    uint32_t w = 1u + (h[0] & 63u);
    uint32_t ht = 1u + (h[1] & 63u);
    uint16_t spp = (uint16_t)(1u + (h[2] & 3u));
    uint16_t bps = (uint16_t)(h[3] % 3u == 0u ? 8u : (h[3] % 3u == 1u ? 16u : 32u));
    size_t need = (size_t)w * ht * spp * (bps / 8u);
    size_t i, osz = 0;
    uint8_t *pix, *out = NULL;
    tinydng_write_image wi;
    tinydng_write_options wo;

    pix = (uint8_t *)malloc(need ? need : 1u);
    if (!pix) { tinydng_context_destroy(ctx); return 0; }
    for (i = 0; i < need; i++) pix[i] = (uint8_t)((h[4 + i % (size > 4u ? size - 4u : 1u)] ^ (i >> 3)) & 0xffu);

    memset(&wi, 0, sizeof(wi));
    wi.width = w;
    wi.height = ht;
    wi.samples_per_pixel = spp;
    wi.bits_per_sample = bps;
    wi.sample_format = (uint16_t)((h[4] % 3u) + 1u);
    wi.data = pix;
    wi.data_size = need;

    memset(&wo, 0, sizeof(wo));
    wo.bigtiff = 1;
    wo.big_endian = (h[5] & 1u);
    switch (h[6] % 4u) {
      case 1: wo.compression = TINYDNG_COMPRESSION_LZW; break;
      case 2: wo.compression = TINYDNG_COMPRESSION_NEW_JPEG; break;
      default: wo.compression = TINYDNG_COMPRESSION_NONE; break;
    }
    if (wo.compression == TINYDNG_COMPRESSION_NEW_JPEG && bps != 16u) {
      wo.compression = TINYDNG_COMPRESSION_NONE;
    }

    if (tinydng_write_memory(ctx, &wi, &wo, &out, &osz, &err) == TINYDNG_OK) {
      tinydng_document *doc2 = NULL;
      tinydng_error rerr;
      int lossless = (wo.compression == TINYDNG_COMPRESSION_NONE ||
                      wo.compression == TINYDNG_COMPRESSION_LZW ||
                      wo.compression == TINYDNG_COMPRESSION_NEW_JPEG);
      /* Verify we produced a BigTIFF (magic 'II'/'MM' + version 43). */
      if (osz >= 4u) {
        int be = (out[0] == 'M');
        uint16_t ver = be
            ? (uint16_t)(((uint16_t)out[2] << 8) | out[3])
            : (uint16_t)(out[2] | ((uint16_t)out[3] << 8));
        if (out[0] == 'I' || out[0] == 'M') {
          if (ver != 43u) {
            fprintf(stderr, "non-BigTIFF output (v=%u)\n", (unsigned)ver);
            abort();
          }
        }
      }
      if (tinydng_open_memory(ctx, out, osz, NULL, &doc2, &rerr) == TINYDNG_OK) {
        tinydng_pixels px;
        tinydng_error derr;
        if (tinydng_decode_image(ctx, doc2, 0, NULL, &px, &derr) == TINYDNG_OK) {
          if (lossless && px.data && px.size == need && px.width == w &&
              px.height == ht && px.samples_per_pixel == spp &&
              px.bits_per_sample == bps && need > 0u) {
            if (memcmp(px.data, pix, need) != 0) {
              fprintf(stderr,
                      "BigTIFF round-trip MISMATCH: %ux%u spp=%u bps=%u comp=%u be=%u\n",
                      w, ht, spp, bps, (unsigned)wo.compression, wo.big_endian);
              abort();
            }
          }
          tinydng_pixels_free(ctx, &px);
        }
        tinydng_document_destroy(ctx, doc2);
      }
      tinydng_buffer_free(ctx, out);
    }

    free(pix);
  }

  tinydng_context_destroy(ctx);
  return 0;
}
