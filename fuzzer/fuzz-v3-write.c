/*
 * fuzz-v3-write.c - libFuzzer harness for the tinydng WRITER + a write/read
 * round-trip. The fuzz-v3 harness only exercises the reader; this one drives
 * tinydng_write_memory (LZW encoder, lossless-JPEG encode, extras buffer, IFD
 * assembly, DNG metadata) with configs/pixels derived from the input, then
 * reads the produced buffer back and decodes it. For the lossless compressions
 * it asserts the decoded pixels are byte-identical to what was written, so a
 * silent corruption fails too -- not only crashes/UB/leaks.
 *
 *   clang -std=c11 -g -O1 -fsanitize=address,undefined,fuzzer -fno-sanitize-recover=all -I.. \
 *     fuzz-v3-write.c ../tinydng_api.c ../tinydng_io.c ../tinydng_tiff.c \
 *     ../tinydng_dng.c ../tinydng_codec.c ../tinydng_write.c ../tinydng_miniz.c \
 *     ../tinydng_stb_image.c ../tiny_dng_ljpeg92_v2.c -o fuzz-v3-write
 */
#include "../tinydng.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  tinydng_config cfg;
  tinydng_context *ctx;
  tinydng_write_image wi;
  tinydng_write_options wo;
  tinydng_cfa cfa;
  tinydng_raw_info raw;
  uint8_t *pix = NULL, *out = NULL;
  size_t need, osz = 0, i;
  uint32_t w, h;
  uint16_t spp, bps;
  uint8_t hdr[8];
  tinydng_error err;

  if (size < (size_t)sizeof(hdr)) {
    return 0;
  }
  memcpy(hdr, data, sizeof(hdr));
  data += sizeof(hdr);
  size -= sizeof(hdr);

  /* Derive a small, bounded image config from the header bytes. */
  w = 1u + (hdr[0] & 63u);            /* 1..64 */
  h = 1u + (hdr[1] & 63u);            /* 1..64 */
  spp = (uint16_t)(1u + (hdr[2] & 3u)); /* 1..4 */
  bps = (uint16_t)(hdr[3] % 3u == 0u ? 8u : (hdr[3] % 3u == 1u ? 16u : 32u));

  memset(&cfg, 0, sizeof(cfg));
  cfg.memory_cap_bytes = 64u * 1024u * 1024u;
  cfg.max_images = 8;
  ctx = tinydng_context_create(&cfg, NULL);
  if (!ctx) {
    return 0;
  }

  need = (size_t)w * h * spp * (bps / 8u);
  pix = (uint8_t *)malloc(need ? need : 1u);
  if (!pix) { tinydng_context_destroy(ctx); return 0; }
  for (i = 0; i < need; i++) pix[i] = size ? data[i % size] : (uint8_t)i;

  memset(&wi, 0, sizeof(wi));
  wi.width = w; wi.height = h; wi.samples_per_pixel = spp;
  wi.bits_per_sample = bps;
  wi.sample_format = (uint16_t)((hdr[4] % 3u) + 1u); /* UINT/INT/IEEEFP */
  wi.data = pix; wi.data_size = need;

  memset(&wo, 0, sizeof(wo));
  wo.big_endian = (hdr[5] & 1u);
  wo.as_dng = (hdr[5] & 2u) ? 1u : 0u;
  switch (hdr[6] % 4u) {
    case 1: wo.compression = TINYDNG_COMPRESSION_LZW; break;
    case 2: wo.compression = TINYDNG_COMPRESSION_NEW_JPEG; break;
    default: wo.compression = TINYDNG_COMPRESSION_NONE; break;
  }
  /* lossless-JPEG writer requires 16-bit samples. */
  if (wo.compression == TINYDNG_COMPRESSION_NEW_JPEG && bps != 16u) {
    wo.compression = TINYDNG_COMPRESSION_NONE;
  }

  /* Optionally exercise the DNG metadata / extras-serialization path. */
  if (wo.as_dng) {
    memset(&raw, 0, sizeof(raw));
    raw.has_dng_version = 1; raw.dng_version[0] = 1; raw.dng_version[1] = 4;
    if (hdr[7] & 1u) { raw.black_level_present = 1; raw.black_level[0] = 8; }
    if (hdr[7] & 2u) { raw.white_level_present = 1; raw.white_level[0] = 1023; }
    if (hdr[7] & 4u) { raw.color_matrix_present = 1; for (i = 0; i < 9; i++) raw.color_matrix1[i] = 0.1 * (double)i; }
    if (hdr[7] & 8u) { raw.has_as_shot_neutral = 1; raw.as_shot_neutral[0] = 0.5; raw.as_shot_neutral[1] = 1.0; raw.as_shot_neutral[2] = 0.5; }
    wi.raw = &raw;
    if (spp == 1u) {
      memset(&cfa, 0, sizeof(cfa));
      cfa.present = 1; cfa.pattern_dim[0] = 2; cfa.pattern_dim[1] = 2;
      cfa.pattern_size = 4; cfa.pattern[1] = 1; cfa.pattern[2] = 1; cfa.pattern[3] = 2;
      wi.cfa = &cfa;
    }
  }

  if (tinydng_write_memory(ctx, &wi, &wo, &out, &osz, &err) == TINYDNG_OK) {
    tinydng_document *doc = NULL;
    tinydng_error rerr;
    int lossless = (wo.compression == TINYDNG_COMPRESSION_NONE ||
                    wo.compression == TINYDNG_COMPRESSION_LZW ||
                    wo.compression == TINYDNG_COMPRESSION_NEW_JPEG);
    if (tinydng_open_memory(ctx, out, osz, NULL, &doc, &rerr) == TINYDNG_OK) {
      tinydng_pixels px;
      tinydng_error derr;
      if (tinydng_decode_image(ctx, doc, 0, NULL, &px, &derr) == TINYDNG_OK) {
        /* Round-trip invariant: a lossless write then read must reproduce the
         * exact bytes (writer handles endianness; the reader returns host
         * order, tightly packed chunky). */
        if (lossless && px.data && px.size == need && px.width == w &&
            px.height == h && px.samples_per_pixel == spp &&
            px.bits_per_sample == bps && need > 0u) {
          if (memcmp(px.data, pix, need) != 0) {
            fprintf(stderr,
                    "round-trip MISMATCH: %ux%u spp=%u bps=%u comp=%u be=%u\n",
                    w, h, spp, bps, (unsigned)wo.compression, wo.big_endian);
            abort();
          }
        }
        tinydng_pixels_free(ctx, &px);
      }
      tinydng_document_destroy(ctx, doc);
    }
    tinydng_buffer_free(ctx, out);
  }

  free(pix);
  tinydng_context_destroy(ctx);
  return 0;
}
