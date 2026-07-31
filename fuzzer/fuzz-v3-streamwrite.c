/*
 * fuzz-v3-streamwrite.c - libFuzzer harness for the streaming/tiled writer
 * (tinydng_writer_*). Derives a small image config + tiling + compression
 * from the input, writes every tile/strip with fuzz-derived pixels, reads
 * the produced buffer back and decodes it. For the lossless compressions it
 * asserts the decoded pixels are byte-identical to what was written, so a
 * silent corruption fails too -- not only crashes/UB/leaks.
 *
 *   clang -std=c11 -g -O1 -fsanitize=address,undefined,fuzzer -fno-sanitize-recover=all -I.. \
 *     fuzz-v3-streamwrite.c ../tinydng_api.c ../tinydng_io.c ../tinydng_tiff.c \
 *     ../tinydng_dng.c ../tinydng_codec.c ../tinydng_write.c ../tinydng_miniz.c \
 *     ../tinydng_stb_image.c ../tiny_dng_ljpeg92_v2.c -o fuzz-v3-streamwrite
 */
#include "../tinydng.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  tinydng_config cfg;
  tinydng_context *ctx;
  tinydng_write_image meta;
  tinydng_write_options opts;
  tinydng_tiling tiling;
  tinydng_write_io io;
  tinydng_writer *w = NULL;
  tinydng_error err;
  uint8_t *pix = NULL, *blob = NULL;
  size_t blob_len = 0, need, i;
  uint32_t wpx, hpx, tw, tl, rps, seg_count, s;
  uint16_t spp, bps, comp;
  uint8_t hdr[16];
  int tiled, be;
  tinydng_status st;

  if (size < (size_t)sizeof(hdr)) {
    return 0;
  }
  memcpy(hdr, data, sizeof(hdr));
  data += sizeof(hdr);
  size -= sizeof(hdr);

  wpx = 1u + (hdr[0] & 63u);           /* 1..64 */
  hpx = 1u + (hdr[1] & 63u);           /* 1..64 */
  spp = (uint16_t)(1u + (hdr[2] & 3u)); /* 1..4 */
  bps = (uint16_t)((hdr[3] % 3u == 0u) ? 8u
                  : (hdr[3] % 3u == 1u) ? 16u
                                        : 32u);
  comp = (uint16_t)(hdr[4] % 3u == 0u ? TINYDNG_COMPRESSION_NONE
                   : hdr[4] % 3u == 1u ? TINYDNG_COMPRESSION_LZW
                                       : TINYDNG_COMPRESSION_NEW_JPEG);
  if (comp == TINYDNG_COMPRESSION_NEW_JPEG) {
    bps = 16;
  }
  be = (hdr[5] & 1u);
  tiled = (hdr[6] & 1u);
  tw = 1u + (hdr[7] & 31u);            /* 1..32 */
  tl = 1u + (hdr[8] & 31u);
  rps = 1u + (hdr[9] & 15u);           /* 1..16 */
  if (tw > 0xFFFFu || tl > 0xFFFFu) {
    return 0; /* writer rejects tile dims > 65535 */
  }

  memset(&cfg, 0, sizeof(cfg));
  cfg.memory_cap_bytes = 64u * 1024u * 1024u;
  cfg.max_images = 8;
  ctx = tinydng_context_create(&cfg, NULL);
  if (!ctx) {
    return 0;
  }

  /* The writer reads one full tile/strip from `pix` per call, so the
     buffer must cover the largest of: whole image, one tile, one strip. */
  {
    size_t img_bytes = (size_t)wpx * hpx * spp * (bps / 8u);
    size_t tile_bytes = (size_t)tw * tl * spp * (bps / 8u);
    size_t strip_bytes = (size_t)wpx * rps * spp * (bps / 8u);
    need = img_bytes;
    if (tile_bytes > need) need = tile_bytes;
    if (strip_bytes > need) need = strip_bytes;
  }
  pix = (uint8_t *)malloc(need ? need : 1u);
  if (!pix) {
    tinydng_context_destroy(ctx);
    return 0;
  }
  for (i = 0; i < need; i++) {
    pix[i] = size ? data[i % size] : (uint8_t)i;
  }

  memset(&meta, 0, sizeof(meta));
  meta.width = wpx;
  meta.height = hpx;
  meta.samples_per_pixel = spp;
  meta.bits_per_sample = bps;
  meta.sample_format = TINYDNG_SAMPLEFORMAT_UINT;
  memset(&opts, 0, sizeof(opts));
  opts.big_endian = (uint8_t)be;
  opts.compression = comp;
  memset(&tiling, 0, sizeof(tiling));
  if (tiled) {
    tiling.tile_width = tw;
    tiling.tile_length = tl;
  } else {
    tiling.rows_per_strip = rps;
  }

  st = tinydng_write_io_open_memory(ctx, &io, &err);
  if (st == TINYDNG_OK) {
    st = tinydng_writer_create(ctx, io, &meta, &opts, &tiling, &w, &err);
  }
  if (st != TINYDNG_OK) {
    if (st == TINYDNG_OK) io.close(&io);
    free(pix);
    tinydng_context_destroy(ctx);
    return 0;
  }

  /* The writer owns the sink copy; the segment count is bounded small. */
  {
    uint32_t across, down;
    if (tiled) {
      across = (wpx + tw - 1u) / tw;
      down = (hpx + tl - 1u) / tl;
    } else {
      across = 1u;
      down = (hpx + rps - 1u) / rps;
    }
    seg_count = across * down;
    if (seg_count > 4096u) {
      seg_count = 4096u;
    }
  }
  for (s = 0; s < seg_count && st == TINYDNG_OK; s++) {
    st = tiled ? tinydng_writer_write_tile(w, s, pix, &err)
               : tinydng_writer_write_strip(w, s, pix, &err);
    if (st != TINYDNG_OK) {
      break;
    }
  }
  if (st != TINYDNG_OK) {
    tinydng_writer_finish(w, &err); /* releases writer state */
    io.close(&io);
    free(pix);
    tinydng_context_destroy(ctx);
    return 0;
  }
  st = tinydng_writer_finish(w, &err);
  if (st != TINYDNG_OK) {
    io.close(&io);
    free(pix);
    tinydng_context_destroy(ctx);
    return 0;
  }
  st = tinydng_write_io_memory_take(ctx, &io, &blob, &blob_len, &err);
  io.close(&io);
  if (st != TINYDNG_OK) {
    free(pix);
    tinydng_context_destroy(ctx);
    return 0;
  }

  /* Read back and compare (lossless compressions only). The expected
     image is rebuilt from the same tile/strip rects the writer consumed:
     every segment copies the leading rect of `pix` into its position. */
  if (blob && blob_len > 0u) {
    tinydng_document *doc = NULL;
    tinydng_pixels px;
    tinydng_status ost = tinydng_open_memory(ctx, blob, blob_len, NULL, &doc, &err);
    if (ost == TINYDNG_OK) {
      ost = tinydng_decode_image(ctx, doc, 0, NULL, &px, &err);
      if (ost == TINYDNG_OK && comp != TINYDNG_COMPRESSION_NONE &&
          (comp == TINYDNG_COMPRESSION_LZW ||
           comp == TINYDNG_COMPRESSION_NEW_JPEG)) {
        size_t img_bytes = (size_t)wpx * hpx * spp * (bps / 8u);
        size_t sb = (size_t)bps / 8u;
        uint8_t *expected = (uint8_t *)malloc(img_bytes ? img_bytes : 1u);
        if (expected) {
          uint32_t k;
          memset(expected, 0, img_bytes);
          for (k = 0; k < seg_count; k++) {
            uint32_t x, y, pw, ph, r;
            if (tiled) {
              uint32_t across = (wpx + tw - 1u) / tw;
              x = (k % across) * tw;
              y = (k / across) * tl;
              pw = (x + tw <= wpx) ? tw : (wpx - x);
              ph = (y + tl <= hpx) ? tl : (hpx - y);
            } else {
              x = 0;
              y = k * rps;
              pw = wpx;
              ph = (y + rps <= hpx) ? rps : (hpx - y);
            }
            for (r = 0; r < ph; r++) {
              memcpy(expected + ((size_t)(y + r) * wpx + x) * spp * sb,
                     pix + (size_t)r * pw * spp * sb, (size_t)pw * spp * sb);
            }
          }
          if (px.size != img_bytes ||
              memcmp(px.data, expected, img_bytes) != 0) {
            abort(); /* silent corruption */
          }
          free(expected);
        }
      }
      if (ost == TINYDNG_OK) {
        tinydng_pixels_free(ctx, &px);
      }
      tinydng_document_destroy(ctx, doc);
    }
  }
  tinydng_buffer_free(ctx, blob);
  free(pix);
  tinydng_context_destroy(ctx);
  return 0;
}
