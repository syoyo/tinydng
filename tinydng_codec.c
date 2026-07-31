/*
 * tinydng_codec.c - lazy decode: geometry, predictors, uncompressed + LJPEG,
 * tile/strip -> image assembly.
 * SPDX-License-Identifier: MIT
 */
#include "td_internal.h"
#include "tiny_dng_ljpeg92_v2.h"

#ifndef TINYDNG_NO_ZIP
#include "miniz.h"
#endif
#ifndef TINYDNG_NO_BASELINE_JPEG
#define STBI_NO_STDIO
#include "stb_image.h"
#endif

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static int td_host_big(void) {
  uint16_t x = 1u;
  uint8_t b[2];
  memcpy(b, &x, 2);
  return b[0] == 0u;
}

typedef struct td_geom {
  uint32_t width, height;
  uint16_t spp;
  uint16_t bps;           /* stored bits per sample          */
  uint16_t out_bps;       /* decoded bits per sample (8/16/32) */
  uint16_t sample_format;
  uint16_t planar;
  uint16_t predictor;
  size_t out_bytes;       /* out_bps / 8                     */
  size_t pixel_stride;    /* spp * out_bytes (chunky)        */
  size_t row_stride;      /* width * pixel_stride            */
  size_t image_size;      /* row_stride * height             */
} td_geom;

static tinydng_status td_compute_geom(tinydng_context *ctx,
                                      const tinydng_image_info *img,
                                      td_geom *g, tinydng_error *err) {
  uint16_t out_bps;
  (void)ctx;
  memset(g, 0, sizeof(*g));
  g->width = img->width;
  g->height = img->height;
  g->spp = img->samples_per_pixel;
  g->bps = img->bits_per_sample;
  g->sample_format = img->sample_format;
  g->planar = img->planar_configuration;
  g->predictor = img->predictor;

  if (img->compression == TINYDNG_COMPRESSION_LOSSY_JPEG) {
    out_bps = 8; /* baseline/lossy JPEG decodes to 8-bit */
  } else if (img->compression == TINYDNG_COMPRESSION_OLD_JPEG ||
             img->compression == TINYDNG_COMPRESSION_NEW_JPEG) {
    /* bps<=8 => baseline JPEG preview (8-bit); else lossless JPEG (16-bit). */
    out_bps = (g->bps <= 8) ? 8 : 16;
  } else if (g->bps <= 8) {
    out_bps = 8;
  } else if (g->bps <= 16) {
    out_bps = 16;
  } else {
    out_bps = 32;
  }
  g->out_bps = out_bps;
  g->out_bytes = (size_t)out_bps / 8u;

  if (g->spp == 0u || g->width == 0u || g->height == 0u) {
    td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_DECODE, 0, 0, 0,
                 "invalid decode geometry");
    return TINYDNG_E_PARSE;
  }
  if (!td_safe_mul_size((size_t)g->spp, g->out_bytes, &g->pixel_stride) ||
      !td_safe_mul_size((size_t)g->width, g->pixel_stride, &g->row_stride) ||
      !td_safe_mul_size((size_t)g->height, g->row_stride, &g->image_size)) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_DECODE, 0, 0, 0,
                 "decoded image size overflow");
    return TINYDNG_E_BOUNDS;
  }
  return TINYDNG_OK;
}

/* Obtain a contiguous pointer to a segment's compressed/raw bytes. Uses
   zero-copy map when available; otherwise allocates `*owned` and reads. */
static const uint8_t *td_segment_bytes(tinydng_context *ctx, tinydng_io *io,
                                       uint64_t io_size, uint64_t off,
                                       size_t len, uint8_t **owned,
                                       tinydng_error *err) {
  uint8_t *buf;
  *owned = NULL;
  if (len == 0u) {
    return NULL;
  }
  if (io->map) {
    const uint8_t *p = io->map(io, off, len);
    if (p) {
      return p;
    }
  }
  buf = (uint8_t *)td_ctx_alloc(ctx, len, err);
  if (!buf) {
    return NULL;
  }
  if (td_io_view(io, io_size, off, len, buf, len) == NULL) {
    td_ctx_free(ctx, buf);
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_DECODE, 0, 0, off,
                 "segment bytes out of range");
    return NULL;
  }
  *owned = buf;
  return buf;
}

/* ------------------------------------------------------------------ */
/* Predictors (operate on one decoded block, chunky)                  */
/* ------------------------------------------------------------------ */

static void td_unpredict_h(uint8_t *buf, uint32_t w, uint32_t h, uint16_t spp,
                           size_t bytes) {
  uint32_t y, x;
  uint16_t c;
  size_t row_elems = (size_t)w * spp;
  if (w < 2u) {
    return;
  }
  for (y = 0; y < h; y++) {
    size_t base = (size_t)y * row_elems;
    for (c = 0; c < spp; c++) {
      if (bytes == 1u) {
        uint8_t *p = buf + base;
        for (x = 1; x < w; x++) {
          p[(size_t)x * spp + c] =
              (uint8_t)(p[(size_t)x * spp + c] + p[((size_t)x - 1) * spp + c]);
        }
      } else if (bytes == 2u) {
        uint16_t *p = (uint16_t *)buf + base;
        for (x = 1; x < w; x++) {
          p[(size_t)x * spp + c] =
              (uint16_t)(p[(size_t)x * spp + c] + p[((size_t)x - 1) * spp + c]);
        }
      } else if (bytes == 4u) {
        uint32_t *p = (uint32_t *)buf + base;
        for (x = 1; x < w; x++) {
          p[(size_t)x * spp + c] =
              p[(size_t)x * spp + c] + p[((size_t)x - 1) * spp + c];
        }
      }
    }
  }
}

/* Floating-point predictor (TIFF predictor 3), libtiff fpAcc algorithm. */
static int td_unpredict_fp(tinydng_context *ctx, uint8_t *buf, uint32_t w,
                           uint32_t h, uint16_t spp, size_t bytes,
                           tinydng_error *err) {
  uint32_t y;
  size_t wc = (size_t)w * spp;     /* samples per row */
  size_t cc = wc * bytes;          /* bytes per row   */
  int host_big = td_host_big();
  uint8_t *tmp;
  if (bytes != 2u && bytes != 4u) {
    return 1; /* nothing to do */
  }
  tmp = (uint8_t *)td_ctx_alloc(ctx, cc, err);
  if (!tmp) {
    return 0;
  }
  for (y = 0; y < h; y++) {
    uint8_t *row = buf + (size_t)y * cc;
    size_t i, n, b;
    for (i = (size_t)spp; i < cc; i++) {
      row[i] = (uint8_t)(row[i] + row[i - (size_t)spp]);
    }
    memcpy(tmp, row, cc);
    for (n = 0; n < wc; n++) {
      for (b = 0; b < bytes; b++) {
        size_t src = host_big ? (b * wc + n) : ((bytes - 1u - b) * wc + n);
        row[bytes * n + b] = tmp[src];
      }
    }
  }
  td_ctx_free(ctx, tmp);
  return 1;
}

static int td_apply_predictor(tinydng_context *ctx, const td_geom *g,
                              uint8_t *block, uint32_t bw, uint32_t bh,
                              tinydng_error *err) {
  if (g->predictor == 2u) {
    if (g->sample_format == TINYDNG_SAMPLEFORMAT_IEEEFP) {
      td_set_error(err, TINYDNG_E_UNSUPPORTED, TINYDNG_STAGE_DECODE, 0, 0, 0,
                   "predictor 2 invalid for float samples");
      return 0;
    }
    td_unpredict_h(block, bw, bh, g->spp, g->out_bytes);
    return 1;
  }
  if (g->predictor == 3u) {
    if (g->sample_format != TINYDNG_SAMPLEFORMAT_IEEEFP) {
      td_set_error(err, TINYDNG_E_UNSUPPORTED, TINYDNG_STAGE_DECODE, 0, 0, 0,
                   "predictor 3 requires float samples");
      return 0;
    }
    return td_unpredict_fp(ctx, block, bw, bh, g->spp, g->out_bytes, err);
  }
  return 1; /* predictor 1 / none */
}

/* ------------------------------------------------------------------ */
/* Block decoders                                                     */
/* ------------------------------------------------------------------ */

/* Bytes of stored (pre-decode) sample data for one bw*bh block. */
static int td_stored_block_size(const td_geom *g, uint32_t bw, uint32_t bh,
                                size_t *out) {
  size_t spr, in_row, total;
  /* samples per row = bw * spp; overflow-safe so 32-bit builds fail closed. */
  if (!td_safe_mul_size((size_t)bw, (size_t)g->spp, &spr)) {
    return 0;
  }
  if (g->bps == 8u || g->bps == 16u || g->bps == 32u) {
    if (!td_safe_mul_size(spr, (size_t)g->bps / 8u, &in_row)) {
      return 0;
    }
  } else if (g->bps >= 1u && g->bps <= 16u) {
    size_t bits;
    if (!td_safe_mul_size(spr, (size_t)g->bps, &bits)) {
      return 0;
    }
    in_row = (bits + 7u) / 8u;
  } else {
    return 0;
  }
  if (!td_safe_mul_size(in_row, bh, &total)) {
    return 0;
  }
  *out = total;
  return 1;
}

/* Convert a contiguous stored-byte block (file byte order) into the decoded
   `block` (host order, out_bps samples). Handles 8/16/32 + 10/12/14 packed. */
static tinydng_status td_fill_block_from_stored(const td_geom *g, int big_endian,
                                                const uint8_t *src,
                                                size_t src_len, uint32_t bw,
                                                uint32_t bh, uint8_t *block,
                                                tinydng_error *err) {
  uint32_t y;
  size_t need;
  if (!td_stored_block_size(g, bw, bh, &need)) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_DECODE, 0, 0, 0,
                 "stored block size overflow");
    return TINYDNG_E_BOUNDS;
  }
  if (src_len < need) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_DECODE, 0, 0, 0,
                 "stored data too small: need=%zu have=%zu", need, src_len);
    return TINYDNG_E_BOUNDS;
  }

  if (g->bps == 8u || g->bps == 16u || g->bps == 32u) {
    size_t stored_bytes = (size_t)g->bps / 8u;
    size_t in_row_bytes = (size_t)bw * g->spp * stored_bytes;
    int need_swap = (stored_bytes > 1u) && (big_endian != td_host_big());
    for (y = 0; y < bh; y++) {
      memcpy(block + (size_t)y * in_row_bytes, src + (size_t)y * in_row_bytes,
             in_row_bytes);
    }
    if (need_swap) {
      size_t total_samples = (size_t)bw * bh * g->spp;
      size_t i;
      if (stored_bytes == 2u) {
        uint16_t *p = (uint16_t *)block;
        for (i = 0; i < total_samples; i++) {
          p[i] = (uint16_t)((p[i] >> 8) | (p[i] << 8));
        }
      } else {
        uint32_t *p = (uint32_t *)block;
        for (i = 0; i < total_samples; i++) {
          uint32_t v = p[i];
          p[i] = ((v >> 24) & 0xFFu) | ((v >> 8) & 0xFF00u) |
                 ((v << 8) & 0xFF0000u) | ((v << 24) & 0xFF000000u);
        }
      }
    }
    return TINYDNG_OK;
  }

  /* Packed sub-byte depths (10/12/14): MSB-first, rows byte-aligned. */
  {
    size_t samples_per_row = (size_t)bw * g->spp;
    size_t in_row_bytes = (samples_per_row * g->bps + 7u) / 8u;
    for (y = 0; y < bh; y++) {
      const uint8_t *rp = src + (size_t)y * in_row_bytes;
      size_t row_base = (size_t)y * samples_per_row;
      /* MSB-first accumulator: shift whole bytes in, pull bps bits out of the
         top. nbits stays below bps+8 <= 24, so one 32-bit value suffices. */
      uint32_t bitbuf = 0;
      int nbits = 0;
      size_t s;
      for (s = 0; s < samples_per_row; s++) {
        uint32_t v;
        while (nbits < g->bps) {
          bitbuf = (bitbuf << 8) | (uint32_t)(*rp++);
          nbits += 8;
        }
        v = bitbuf >> (nbits - g->bps);
        nbits -= g->bps;
        bitbuf &= (1u << nbits) - 1u; /* keep only the unconsumed low bits */
        /* Write into the destination element width: bps<8 -> 8-bit output,
           bps 9..15 -> 16-bit output (see out_bps in td_compute_geom). */
        if (g->out_bytes == 1u) {
          ((uint8_t *)block)[row_base + s] = (uint8_t)v;
        } else {
          ((uint16_t *)block)[row_base + s] = (uint16_t)v;
        }
      }
    }
  }
  return TINYDNG_OK;
}

/* Decode one uncompressed segment into `block`. */
static tinydng_status td_decode_block_uncompressed(
    tinydng_context *ctx, tinydng_io *io, uint64_t io_size, int big_endian,
    const td_geom *g, const tinydng_segment *seg, uint32_t bw, uint32_t bh,
    uint8_t *block, tinydng_error *err) {
  uint8_t *owned = NULL;
  const uint8_t *src;
  size_t need;
  tinydng_status st;
  if (!td_stored_block_size(g, bw, bh, &need)) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_DECODE, 0, 0, 0,
                 "uncompressed input size overflow");
    return TINYDNG_E_BOUNDS;
  }
  if ((uint64_t)need > seg->byte_count) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_DECODE, 0, 0, seg->offset,
                 "strip/tile too small: need=%zu have=%llu", need,
                 (unsigned long long)seg->byte_count);
    return TINYDNG_E_BOUNDS;
  }
  src = td_segment_bytes(ctx, io, io_size, seg->offset, need, &owned, err);
  if (!src) {
    return err->status ? err->status : TINYDNG_E_BOUNDS;
  }
  st = td_fill_block_from_stored(g, big_endian, src, need, bw, bh, block, err);
  if (owned) {
    td_ctx_free(ctx, owned);
  }
  return st;
}

#if !defined(TINYDNG_NO_LZW) || !defined(TINYDNG_NO_PACKBITS) || \
    !defined(TINYDNG_NO_ZIP)
/* Shared helper: decode a compressed segment into a stored-byte scratch of
   exactly `need` bytes, then convert to the output block. */
static tinydng_status td_finish_compressed(tinydng_context *ctx, int big_endian,
                                           const td_geom *g, uint8_t *stored,
                                           size_t got, size_t need, uint32_t bw,
                                           uint32_t bh, uint8_t *block,
                                           tinydng_error *err) {
  (void)ctx;
  if (got != need) {
    td_set_error(err, TINYDNG_E_DECODE, TINYDNG_STAGE_DECODE, 0, 0, 0,
                 "decompressed size mismatch: got=%zu need=%zu", got, need);
    return TINYDNG_E_DECODE;
  }
  return td_fill_block_from_stored(g, big_endian, stored, need, bw, bh, block,
                                   err);
}
#endif

#ifndef TINYDNG_NO_LZW
/* TIFF LZW (early-change, MSB-first). Returns decoded byte count or -1. */
static long td_lzw_decode(const uint8_t *in, size_t in_len, uint8_t *out,
                          size_t out_cap) {
  enum { CLEAR = 256, EOI = 257 };
  uint16_t prefix[4096];
  uint8_t suffix[4096];
  uint8_t stack[4096];
  int code_size = 9;
  int next_code = 258;
  long out_pos = 0;
  uint64_t bitbuf = 0;
  int bitcnt = 0;
  size_t in_pos = 0;
  int prev = -1;

  for (;;) {
    int code;
    while (bitcnt < code_size && in_pos < in_len) {
      bitbuf = (bitbuf << 8) | in[in_pos++];
      bitcnt += 8;
    }
    if (bitcnt < code_size) {
      break; /* ran out of input */
    }
    code = (int)((bitbuf >> (bitcnt - code_size)) & ((1u << code_size) - 1u));
    bitcnt -= code_size;

    if (code == EOI) {
      break;
    }
    if (code == CLEAR) {
      code_size = 9;
      next_code = 258;
      prev = -1;
      continue;
    }
    {
      int sp = 0;
      int cur = code;
      if (prev == -1) {
        if (code >= 256) {
          return -1;
        }
        if ((size_t)out_pos >= out_cap) {
          return -1;
        }
        out[out_pos++] = (uint8_t)code;
        prev = code;
        continue;
      }
      if (code < next_code) {
        cur = code;
      } else if (code == next_code) {
        /* KwKwK case: emit prev string + its first char */
        stack[sp++] = (uint8_t)0; /* placeholder, replaced below */
        cur = prev;
      } else {
        return -1; /* invalid code */
      }
      /* Walk the chain for `cur` onto the stack. */
      while (cur >= 256) {
        if (sp >= 4096 || cur >= 4096) {
          return -1;
        }
        stack[sp++] = suffix[cur];
        cur = prefix[cur];
      }
      if (sp >= 4096) {
        return -1;
      }
      stack[sp++] = (uint8_t)cur;
      if (code == next_code) {
        /* fix the KwKwK placeholder: first char is `cur` (= firstchar(prev)) */
        stack[0] = (uint8_t)cur;
      }
      /* Emit reversed stack. */
      while (sp > 0) {
        if ((size_t)out_pos >= out_cap) {
          return -1;
        }
        out[out_pos++] = stack[--sp];
      }
      /* Add new entry prev + firstchar. */
      if (next_code < 4096) {
        prefix[next_code] = (uint16_t)prev;
        suffix[next_code] = (uint8_t)cur;
        next_code++;
        /* TIFF early change: widen one code before the table fills. */
        if (code_size < 12 && next_code == (1 << code_size) - 1) {
          code_size++;
        }
      }
      prev = code;
    }
  }
  return out_pos;
}
#endif /* TINYDNG_NO_LZW */

#ifndef TINYDNG_NO_PACKBITS
long td_packbits_decode(const uint8_t *in, size_t in_len, uint8_t *out,
                        size_t out_cap) {
  size_t ip = 0;
  long op = 0;
  while (ip < in_len) {
    int8_t n = (int8_t)in[ip++];
    if (n >= 0) {
      int cnt = n + 1;
      int k;
      if (ip + (size_t)cnt > in_len) {
        return -1;
      }
      for (k = 0; k < cnt; k++) {
        if ((size_t)op >= out_cap) {
          return -1;
        }
        out[op++] = in[ip++];
      }
    } else if (n != -128) {
      int cnt = 1 - n;
      int k;
      uint8_t v;
      if (ip >= in_len) {
        return -1;
      }
      v = in[ip++];
      for (k = 0; k < cnt; k++) {
        if ((size_t)op >= out_cap) {
          return -1;
        }
        out[op++] = v;
      }
    }
  }
  return op;
}
#endif /* TINYDNG_NO_PACKBITS */

/* Decode one LZW/PackBits/ZIP segment into `block`. */
static tinydng_status td_decode_block_compressed(
    tinydng_context *ctx, tinydng_io *io, uint64_t io_size, int big_endian,
    const td_geom *g, const tinydng_segment *seg, uint16_t compression,
    uint32_t bw, uint32_t bh, uint8_t *block, tinydng_error *err) {
  uint8_t *owned = NULL;
  uint8_t *stored = NULL;
  const uint8_t *src;
  size_t need;
  long got = -1;
  tinydng_status st;

  if (!td_stored_block_size(g, bw, bh, &need)) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_DECODE, 0, 0, 0,
                 "stored block size overflow");
    return TINYDNG_E_BOUNDS;
  }
  if (seg->byte_count > (uint64_t)INT32_MAX) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_DECODE, 0, 0, seg->offset,
                 "compressed segment too large");
    return TINYDNG_E_BOUNDS;
  }
  src = td_segment_bytes(ctx, io, io_size, seg->offset, (size_t)seg->byte_count,
                         &owned, err);
  if (!src) {
    return err->status ? err->status : TINYDNG_E_BOUNDS;
  }
  stored = (uint8_t *)td_ctx_alloc(ctx, need, err);
  if (!stored) {
    if (owned) {
      td_ctx_free(ctx, owned);
    }
    return TINYDNG_E_OOM;
  }

  switch (compression) {
#ifndef TINYDNG_NO_LZW
    case TINYDNG_COMPRESSION_LZW:
      got = td_lzw_decode(src, (size_t)seg->byte_count, stored, need);
      break;
#endif
#ifndef TINYDNG_NO_PACKBITS
    case TINYDNG_COMPRESSION_PACKBITS:
      got = td_packbits_decode(src, (size_t)seg->byte_count, stored, need);
      break;
#endif
#ifndef TINYDNG_NO_ZIP
    case TINYDNG_COMPRESSION_ZIP: {
      mz_ulong dlen = (mz_ulong)need;
      int mzr = mz_uncompress(stored, &dlen, src, (mz_ulong)seg->byte_count);
      got = (mzr == MZ_OK) ? (long)dlen : -1;
      break;
    }
#ifndef TINYDNG_NO_PSD
    case TD_COMPRESSION_PSD_ZIP_PRED: {
      /* PSD zip-with-prediction: one whole channel plane per segment. */
      mz_ulong dlen = (mz_ulong)need;
      int mzr = mz_uncompress(stored, &dlen, src, (mz_ulong)seg->byte_count);
      got = (mzr == MZ_OK) ? (long)dlen : -1;
      if (got == (long)need &&
          !td_psd_unpredict_plane(ctx, stored, bw, bh, g->bps, err)) {
        got = -1;
      }
      break;
    }
#endif
#endif
    default:
      td_ctx_free(ctx, stored);
      if (owned) {
        td_ctx_free(ctx, owned);
      }
      td_set_error(err, TINYDNG_E_UNSUPPORTED, TINYDNG_STAGE_DECODE, 0, 0, 0,
                   "compression %u not enabled", (unsigned)compression);
      return TINYDNG_E_UNSUPPORTED;
  }

  if (got < 0) {
    td_ctx_free(ctx, stored);
    if (owned) {
      td_ctx_free(ctx, owned);
    }
    td_set_error(err, TINYDNG_E_DECODE, TINYDNG_STAGE_DECODE, 0, 0, seg->offset,
                 "decompression failed (comp=%u)", (unsigned)compression);
    return TINYDNG_E_DECODE;
  }

  st = td_finish_compressed(ctx, big_endian, g, stored, (size_t)got, need, bw,
                            bh, block, err);
  td_ctx_free(ctx, stored);
  if (owned) {
    td_ctx_free(ctx, owned);
  }
  return st;
}

#ifndef TINYDNG_NO_BASELINE_JPEG
/* Decode one baseline/lossy JPEG segment (8-bit) into `block`. */
static tinydng_status td_decode_block_baseline(tinydng_context *ctx,
                                               tinydng_io *io, uint64_t io_size,
                                               const td_geom *g,
                                               const tinydng_segment *seg,
                                               uint8_t *block, uint32_t bw,
                                               uint32_t bh, tinydng_error *err) {
  uint8_t *owned = NULL;
  const uint8_t *src;
  int w = 0, h = 0, comp = 0;
  stbi_uc *pixels;

  if (seg->byte_count > (uint64_t)INT32_MAX) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_DECODE, 0, 0, seg->offset,
                 "jpeg segment too large");
    return TINYDNG_E_BOUNDS;
  }
  src = td_segment_bytes(ctx, io, io_size, seg->offset, (size_t)seg->byte_count,
                         &owned, err);
  if (!src) {
    return err->status ? err->status : TINYDNG_E_BOUNDS;
  }
  pixels = stbi_load_from_memory(src, (int)seg->byte_count, &w, &h, &comp,
                                 (int)g->spp);
  if (owned) {
    td_ctx_free(ctx, owned);
  }
  if (!pixels) {
    td_set_error(err, TINYDNG_E_DECODE, TINYDNG_STAGE_DECODE, 0, 0, seg->offset,
                 "stb baseline JPEG decode failed: %s", stbi_failure_reason());
    return TINYDNG_E_DECODE;
  }
  if ((uint32_t)w != bw || (uint32_t)h != bh) {
    stbi_image_free(pixels);
    td_set_error(err, TINYDNG_E_DECODE, TINYDNG_STAGE_DECODE, 0, 0, seg->offset,
                 "baseline JPEG dims %dx%d != block %ux%u", w, h, bw, bh);
    return TINYDNG_E_DECODE;
  }
  memcpy(block, pixels, (size_t)bw * bh * g->spp); /* 8-bit, spp channels */
  stbi_image_free(pixels);
  return TINYDNG_OK;
}
#endif /* TINYDNG_NO_BASELINE_JPEG */

/* Streaming adapter: exposes a byte range of a tinydng_io to the LJPEG
   streaming decoder, so stdio (map == NULL) segments are never materialized
   as a whole; the entropy payload is destuffed straight from the backend. */
typedef struct td_lj92_stream_io {
  tinydng_io *io;
  uint64_t base; /* absolute file offset of the segment */
  uint64_t size; /* segment size */
} td_lj92_stream_io;

static size_t td_lj92_stream_read(void *user, uint64_t off, void *dst,
                                  size_t len) {
  td_lj92_stream_io *s = (td_lj92_stream_io *)user;
  if (off > s->size || (uint64_t)len > (s->size - off)) {
    return 0;
  }
  return s->io->read(s->io, s->base + off, dst, len);
}

static uint64_t td_lj92_stream_size(void *user) {
  return ((td_lj92_stream_io *)user)->size;
}

/* Decode one lossless-JPEG segment into `block` (bw*bh*spp*2). Returns the
   actual decoded dims via out_bw/out_bh. When the backend can map the
   segment, decode zero-copy from the mapping; otherwise stream it through
   the io in chunks (never materializing the whole segment). */
static tinydng_status td_decode_block_ljpeg(tinydng_context *ctx,
                                            tinydng_io *io, uint64_t io_size,
                                            const td_geom *g,
                                            const tinydng_segment *seg,
                                            uint8_t *block, uint32_t bw,
                                            uint32_t bh, tinydng_error *err) {
  uint8_t *owned = NULL;
  const uint8_t *src;
  tdng_lj92 lj = NULL;
  int w = 0, h = 0, bits = 0, comps = 0;
  int ret;
  tinydng_status st = TINYDNG_OK;

  if (seg->byte_count > (uint64_t)INT32_MAX) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_DECODE, 0, 0, seg->offset,
                 "ljpeg segment too large");
    return TINYDNG_E_BOUNDS;
  }

  if (io->map) {
    src = td_segment_bytes(ctx, io, io_size, seg->offset,
                           (size_t)seg->byte_count, &owned, err);
    if (!src) {
      return err->status ? err->status : TINYDNG_E_BOUNDS;
    }
    ret = tdng_lj92_open(&lj, src, (int)seg->byte_count, &w, &h, &bits, &comps);
  } else {
    /* Streaming decode through the io backend (no full-segment copy). */
    td_lj92_stream_io sio;
    sio.io = io;
    sio.base = seg->offset;
    sio.size = seg->byte_count;
    ret = tdng_lj92_open_streaming(&lj, &sio, td_lj92_stream_read,
                                   td_lj92_stream_size, &w, &h, &bits, &comps);
  }
  if (ret == TDNG_LJ92_ERROR_NOT_LOSSLESS) {
    td_set_error(err, TINYDNG_E_UNSUPPORTED, TINYDNG_STAGE_DECODE, 0, 0,
                 seg->offset, "baseline JPEG decode not implemented (P4)");
    st = TINYDNG_E_UNSUPPORTED;
    goto cleanup;
  }
  if (ret != TDNG_LJ92_ERROR_NONE || w <= 0 || h <= 0) {
    td_set_error(err, TINYDNG_E_DECODE, TINYDNG_STAGE_DECODE, 0, 0, seg->offset,
                 "tdng_lj92_open failed ret=%d", ret);
    st = TINYDNG_E_DECODE;
    goto cleanup;
  }
  /* The LJPEG may store the block as (w x h) with `comps` interleaved
     channels; total samples/row = w*comps must match the block (bw*spp).
     This covers the DNG CFA trick (e.g. 256-wide tile stored as 128x2). */
  if ((uint64_t)w * (uint64_t)comps != (uint64_t)bw * (uint64_t)g->spp ||
      (uint32_t)h != bh) {
    td_set_error(err, TINYDNG_E_DECODE, TINYDNG_STAGE_DECODE, 0, 0, seg->offset,
                 "ljpeg %dx%d comps=%d != block %ux%u spp=%u", w, h, comps, bw,
                 bh, (unsigned)g->spp);
    st = TINYDNG_E_DECODE;
    goto cleanup;
  }

  ret = tdng_lj92_decode(lj, (uint16_t *)block,
                         (int)((size_t)w * (size_t)comps), 0, NULL, 0);
  if (ret != TDNG_LJ92_ERROR_NONE) {
    td_set_error(err, TINYDNG_E_DECODE, TINYDNG_STAGE_DECODE, 0, 0, seg->offset,
                 "tdng_lj92_decode failed ret=%d", ret);
    st = TINYDNG_E_DECODE;
    goto cleanup;
  }

cleanup:
  if (lj) {
    tdng_lj92_close(lj);
  }
  if (owned) {
    td_ctx_free(ctx, owned);
  }
  return st;
}

/* ------------------------------------------------------------------ */
/* Block dimension policy                                             */
/* ------------------------------------------------------------------ */

static void td_block_dims(const tinydng_image_info *img,
                          const tinydng_segment *seg, uint32_t *bw,
                          uint32_t *bh) {
  if (seg->kind == TINYDNG_SEG_TILE) {
    *bw = img->tile_width;
    *bh = img->tile_length;
  } else {
    *bw = img->width;
    *bh = seg->h;
  }
}

/* Blit a decoded block (chunky, bw*bh) into the destination raster window. */
static void td_blit_block(const td_geom *g, const uint8_t *block, uint32_t bw,
                          uint32_t bh, uint32_t seg_x, uint32_t seg_y,
                          uint8_t *dst, uint32_t dst_x0, uint32_t dst_y0,
                          uint32_t dst_w, uint32_t dst_h, size_t dst_row_stride) {
  uint32_t copy_w, copy_h, ty;
  size_t src_row = (size_t)bw * g->pixel_stride;
  /* Position of this block inside the destination window. */
  int64_t rel_x = (int64_t)seg_x - (int64_t)dst_x0;
  int64_t rel_y = (int64_t)seg_y - (int64_t)dst_y0;
  uint32_t src_off_x = 0, src_off_y = 0;
  if (rel_x < 0) {
    src_off_x = (uint32_t)(-rel_x);
    rel_x = 0;
  }
  if (rel_y < 0) {
    src_off_y = (uint32_t)(-rel_y);
    rel_y = 0;
  }
  if ((uint32_t)rel_x >= dst_w || (uint32_t)rel_y >= dst_h) {
    return;
  }
  if (src_off_x >= bw || src_off_y >= bh) {
    return;
  }
  copy_w = bw - src_off_x;
  if (copy_w > dst_w - (uint32_t)rel_x) {
    copy_w = dst_w - (uint32_t)rel_x;
  }
  copy_h = bh - src_off_y;
  if (copy_h > dst_h - (uint32_t)rel_y) {
    copy_h = dst_h - (uint32_t)rel_y;
  }
  for (ty = 0; ty < copy_h; ty++) {
    const uint8_t *sp = block + (size_t)(src_off_y + ty) * src_row +
                        (size_t)src_off_x * g->pixel_stride;
    uint8_t *dp = dst + (size_t)((uint32_t)rel_y + ty) * dst_row_stride +
                  (size_t)((uint32_t)rel_x) * g->pixel_stride;
    memcpy(dp, sp, (size_t)copy_w * g->pixel_stride);
  }
}

/* Scatter a single-component decoded block into the interleaved destination
   plane slot (PlanarConfiguration=2). */
static void td_blit_block_planar(const td_geom *g, const uint8_t *block,
                                 uint32_t bw, uint32_t bh, uint16_t plane,
                                 uint32_t seg_x, uint32_t seg_y, uint8_t *dst,
                                 uint32_t dst_x0, uint32_t dst_y0,
                                 uint32_t dst_w, uint32_t dst_h,
                                 size_t dst_row_stride) {
  uint32_t copy_w, copy_h, ty, tx;
  size_t eb = g->out_bytes;
  size_t src_row = (size_t)bw * eb;
  size_t plane_off = (size_t)plane * eb;
  int64_t rel_x = (int64_t)seg_x - (int64_t)dst_x0;
  int64_t rel_y = (int64_t)seg_y - (int64_t)dst_y0;
  uint32_t src_off_x = 0, src_off_y = 0;
  if (plane >= g->spp) {
    return;
  }
  if (rel_x < 0) {
    src_off_x = (uint32_t)(-rel_x);
    rel_x = 0;
  }
  if (rel_y < 0) {
    src_off_y = (uint32_t)(-rel_y);
    rel_y = 0;
  }
  if ((uint32_t)rel_x >= dst_w || (uint32_t)rel_y >= dst_h) {
    return;
  }
  if (src_off_x >= bw || src_off_y >= bh) {
    return;
  }
  copy_w = bw - src_off_x;
  if (copy_w > dst_w - (uint32_t)rel_x) {
    copy_w = dst_w - (uint32_t)rel_x;
  }
  copy_h = bh - src_off_y;
  if (copy_h > dst_h - (uint32_t)rel_y) {
    copy_h = dst_h - (uint32_t)rel_y;
  }
  for (ty = 0; ty < copy_h; ty++) {
    const uint8_t *sp = block + (size_t)(src_off_y + ty) * src_row +
                        (size_t)src_off_x * eb;
    uint8_t *dp = dst + (size_t)((uint32_t)rel_y + ty) * dst_row_stride +
                  (size_t)((uint32_t)rel_x) * g->pixel_stride + plane_off;
    for (tx = 0; tx < copy_w; tx++) {
      memcpy(dp, sp, eb);
      sp += eb;
      dp += g->pixel_stride;
    }
  }
}

/* ------------------------------------------------------------------ */
/* Core: decode segments overlapping a destination window             */
/* ------------------------------------------------------------------ */

/* Shared, read-only-during-run state for one decode_window invocation. The
   only mutable fields (`next`, `failed`) are guarded by `lock`; segments are
   independent and blit to disjoint destination regions. */
typedef struct {
  tinydng_context *ctx;
  tinydng_io *io;
  uint64_t io_size;
  int big_endian;
  const tinydng_image_info *img;
  const td_geom *g;
  td_geom gseg;
  int planar;
  uint32_t win_x, win_y, win_w, win_h;
  uint8_t *dst;
  size_t dst_row_stride;
  td_mutex *lock; /* guards next/failed; NULL when running serially */
  size_t next;    /* next segment index to claim */
  int failed;     /* a worker reported an error */
} td_decode_par;

/* Per-worker state: an owned scratch block (grown on demand) + a private
   error slot so threads never share the caller's error object. */
typedef struct {
  td_decode_par *par;
  uint8_t *block;
  size_t block_cap;
  tinydng_error err;
} td_decode_worker_arg;

/* Decode a single segment into the window. Returns 1 on success (including
   no-op skips for non-overlapping / empty segments), 0 on error (with
   w->err set). The worker's scratch block is reused/grown, never freed here. */
static int td_decode_one_segment(td_decode_par *par, const tinydng_segment *seg,
                                 td_decode_worker_arg *w) {
  tinydng_context *ctx = par->ctx;
  const td_geom *gseg = &par->gseg;
  tinydng_error *err = &w->err;
  uint32_t bw, bh;
  size_t need, block_samples;
  tinydng_status st;
  uint8_t *target;
  int direct;

  /* Skip segments that don't overlap the window. */
  if (seg->x >= par->win_x + par->win_w || seg->y >= par->win_y + par->win_h ||
      seg->x + seg->w <= par->win_x || seg->y + seg->h <= par->win_y) {
    return 1;
  }
  td_block_dims(par->img, seg, &bw, &bh);
  if (bw == 0u || bh == 0u) {
    return 1;
  }

  /* Direct decode into the destination when the block fills full window rows
     (strips, and single-tile-as-whole-image): avoids a second full-size
     buffer + blit. Requires matching row stride (bw == win_w) and chunky
     layout (planar must scatter into plane slots). */
  direct = (!par->planar && bw == par->win_w && seg->x == par->win_x &&
            seg->y >= par->win_y &&
            (uint64_t)(seg->y - par->win_y) + bh <= par->win_h);

  if (direct) {
    target = par->dst + (size_t)(seg->y - par->win_y) * par->dst_row_stride;
  } else {
    size_t block_px;
    if (!td_safe_mul_size((size_t)bw, (size_t)bh, &block_px) ||
        !td_safe_mul_size(block_px, (size_t)gseg->spp, &block_samples) ||
        !td_safe_mul_size(block_samples, gseg->out_bytes, &need)) {
      td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_DECODE, 0, 0, 0,
                   "block size overflow");
      return 0;
    }
    if (need > w->block_cap) {
      td_ctx_free(ctx, w->block);
      w->block = (uint8_t *)td_ctx_alloc(ctx, need, err);
      if (!w->block) {
        w->block_cap = 0;
        return 0;
      }
      w->block_cap = need;
    }
    target = w->block;
  }

  switch (par->img->compression) {
    case TINYDNG_COMPRESSION_NONE:
      st = td_decode_block_uncompressed(ctx, par->io, par->io_size,
                                        par->big_endian, gseg, seg, bw, bh,
                                        target, err);
      break;
    case TINYDNG_COMPRESSION_OLD_JPEG:
    case TINYDNG_COMPRESSION_NEW_JPEG:
      if (gseg->out_bps <= 8u) {
#ifndef TINYDNG_NO_BASELINE_JPEG
        st = td_decode_block_baseline(ctx, par->io, par->io_size, gseg, seg,
                                      target, bw, bh, err);
#else
        td_set_error(err, TINYDNG_E_UNSUPPORTED, TINYDNG_STAGE_DECODE, 0, 0, 0,
                     "baseline JPEG disabled");
        return 0;
#endif
      } else {
        st = td_decode_block_ljpeg(ctx, par->io, par->io_size, gseg, seg, target,
                                   bw, bh, err);
      }
      break;
#ifndef TINYDNG_NO_BASELINE_JPEG
    case TINYDNG_COMPRESSION_LOSSY_JPEG:
      st = td_decode_block_baseline(ctx, par->io, par->io_size, gseg, seg,
                                    target, bw, bh, err);
      break;
#endif
    case TINYDNG_COMPRESSION_LZW:
    case TINYDNG_COMPRESSION_PACKBITS:
    case TINYDNG_COMPRESSION_ZIP:
#if !defined(TINYDNG_NO_PSD) && !defined(TINYDNG_NO_ZIP)
    case TD_COMPRESSION_PSD_ZIP_PRED:
#endif
      st = td_decode_block_compressed(ctx, par->io, par->io_size,
                                      par->big_endian, gseg, seg,
                                      par->img->compression, bw, bh, target,
                                      err);
      break;
    default:
      td_set_error(err, TINYDNG_E_UNSUPPORTED, TINYDNG_STAGE_DECODE, 0, 0, 0,
                   "compression %u not implemented yet",
                   (unsigned)par->img->compression);
      return 0;
  }
  if (st != TINYDNG_OK) {
    return 0;
  }
  if (!td_apply_predictor(ctx, gseg, target, bw, bh, err)) {
    if (err->status == TINYDNG_OK) {
      td_set_error(err, TINYDNG_E_DECODE, TINYDNG_STAGE_DECODE, 0, 0, 0,
                   "predictor failed");
    }
    return 0;
  }
  if (par->planar) {
    td_blit_block_planar(par->g, target, bw, bh, seg->plane, seg->x, seg->y,
                         par->dst, par->win_x, par->win_y, par->win_w,
                         par->win_h, par->dst_row_stride);
  } else if (!direct) {
    td_blit_block(par->g, target, bw, bh, seg->x, seg->y, par->dst, par->win_x,
                  par->win_y, par->win_w, par->win_h, par->dst_row_stride);
  }
  return 1;
}

/* Worker: claim segments from the shared queue until drained or another worker
   fails. One worker running alone is the serial path. */
static void *td_decode_worker(void *p) {
  td_decode_worker_arg *w = (td_decode_worker_arg *)p;
  td_decode_par *par = w->par;
  for (;;) {
    size_t i;
    td_mutex_lock(par->lock);
    if (par->failed || par->next >= par->img->segment_count) {
      td_mutex_unlock(par->lock);
      break;
    }
    i = par->next++;
    td_mutex_unlock(par->lock);

    if (!td_decode_one_segment(par, &par->img->segments[i], w)) {
      td_mutex_lock(par->lock);
      par->failed = 1;
      td_mutex_unlock(par->lock);
      break;
    }
  }
  return NULL;
}

static tinydng_status td_decode_window(tinydng_context *ctx,
                                       const tinydng_document *doc,
                                       const tinydng_image_info *img,
                                       const td_geom *g, uint32_t win_x,
                                       uint32_t win_y, uint32_t win_w,
                                       uint32_t win_h, uint8_t *dst,
                                       size_t dst_row_stride, unsigned nthreads,
                                       tinydng_error *err) {
  td_decode_par par;
  td_decode_worker_arg *workers;
  unsigned n, t;
  int parallel;
  tinydng_status st = TINYDNG_OK;

  memset(&par, 0, sizeof(par));
  par.ctx = ctx;
  par.io = (tinydng_io *)&doc->io;
  par.io_size = doc->io_size;
  par.big_endian = doc->big_endian;
  par.img = img;
  par.g = g;
  par.gseg = *g;
  par.planar = (img->planar_configuration == 2u && g->spp > 1u);
  if (par.planar) {
    par.gseg.spp = 1u; /* in planar mode each segment carries one component */
  }
  par.win_x = win_x;
  par.win_y = win_y;
  par.win_w = win_w;
  par.win_h = win_h;
  par.dst = dst;
  par.dst_row_stride = dst_row_stride;
  par.lock = NULL;
  par.next = 0;
  par.failed = 0;

  n = nthreads ? nthreads : 1u;
  if ((size_t)n > img->segment_count) {
    n = (unsigned)img->segment_count;
  }
  if (n > TD_MAX_DECODE_THREADS) {
    n = TD_MAX_DECODE_THREADS;
  }
  if (n == 0u) {
    n = 1u; /* segment_count == 0: one worker that decodes nothing */
  }
  parallel = (n > 1u && ctx->lock != NULL && td_threads_available());

  workers = (td_decode_worker_arg *)td_ctx_alloc(
      ctx, (size_t)n * sizeof(*workers), err);
  if (!workers) {
    return TINYDNG_E_OOM;
  }
  for (t = 0; t < n; t++) {
    workers[t].par = &par;
    workers[t].block = NULL;
    workers[t].block_cap = 0;
    tinydng_error_clear(&workers[t].err);
  }

  if (parallel) {
    par.lock = ctx->lock;
    ctx->mt_active = 1;
    td_threads_run(td_decode_worker, workers, sizeof(*workers), n);
    ctx->mt_active = 0;
  } else {
    /* One worker drains the whole queue: identical to the old serial loop. */
    td_decode_worker(&workers[0]);
  }

  for (t = 0; t < n; t++) {
    td_ctx_free(ctx, workers[t].block);
    if (st == TINYDNG_OK && workers[t].err.status != TINYDNG_OK) {
      *err = workers[t].err;
      st = workers[t].err.status;
    }
  }
  td_ctx_free(ctx, workers);
  return st;
}

/* Effective worker count for a decode: opts->num_threads (0 = auto = CPU
   count), clamped to the segment count and TD_MAX_DECODE_THREADS. */
static unsigned td_effective_threads(const tinydng_decode_options *opts,
                                     size_t segment_count) {
  unsigned n;
  if (segment_count <= 1u) {
    return 1u;
  }
  n = (opts && opts->num_threads) ? opts->num_threads : td_cpu_count();
  if ((size_t)n > segment_count) {
    n = (unsigned)segment_count;
  }
  if (n > TD_MAX_DECODE_THREADS) {
    n = TD_MAX_DECODE_THREADS;
  }
  if (n == 0u) {
    n = 1u;
  }
  return n;
}

/* ------------------------------------------------------------------ */
/* Public decode entry points                                         */
/* ------------------------------------------------------------------ */

static tinydng_status td_alloc_or_use_dst(tinydng_context *ctx,
                                          const tinydng_decode_options *opts,
                                          size_t need, uint8_t **dst_out,
                                          uint8_t *owns_out,
                                          tinydng_error *err) {
  if (opts && opts->dst) {
    if (opts->dst_capacity < need) {
      td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_DECODE, 0, 0, 0,
                   "dst capacity %zu < required %zu", opts->dst_capacity, need);
      return TINYDNG_E_INVALID_ARG;
    }
    *dst_out = (uint8_t *)opts->dst;
    *owns_out = 0;
    return TINYDNG_OK;
  }
  *dst_out = (uint8_t *)td_ctx_alloc(ctx, need, err);
  if (!*dst_out) {
    return TINYDNG_E_OOM;
  }
  *owns_out = 1;
  return TINYDNG_OK;
}

tinydng_status tinydng_decode_image(tinydng_context *ctx,
                                    const tinydng_document *doc,
                                    size_t image_idx,
                                    const tinydng_decode_options *opts,
                                    tinydng_pixels *out, tinydng_error *err) {
  const tinydng_image_info *img;
  td_geom g;
  tinydng_status st;
  uint8_t *dst;
  uint8_t owns = 0;

  tinydng_error_clear(err);
  if (!ctx || !doc || !out) {
    return TINYDNG_E_INVALID_ARG;
  }
  memset(out, 0, sizeof(*out));
  img = tinydng_image_get(doc, image_idx);
  if (!img) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_DECODE, 0, 0, 0,
                 "image index out of range");
    return TINYDNG_E_INVALID_ARG;
  }
  st = td_compute_geom(ctx, img, &g, err);
  if (st != TINYDNG_OK) {
    return st;
  }
  st = td_alloc_or_use_dst(ctx, opts, g.image_size, &dst, &owns, err);
  if (st != TINYDNG_OK) {
    return st;
  }
  st = td_decode_window(ctx, doc, img, &g, 0, 0, g.width, g.height, dst,
                        g.row_stride,
                        td_effective_threads(opts, img->segment_count), err);
  if (st != TINYDNG_OK) {
    if (owns) {
      td_ctx_free(ctx, dst);
    }
    return st;
  }
  out->data = dst;
  out->size = g.image_size;
  out->width = g.width;
  out->height = g.height;
  out->samples_per_pixel = g.spp;
  out->bits_per_sample = g.out_bps;
  out->sample_format = g.sample_format;
  out->owns_memory = owns;
  return TINYDNG_OK;
}

tinydng_status tinydng_decode_region(tinydng_context *ctx,
                                     const tinydng_document *doc,
                                     size_t image_idx, uint32_t x, uint32_t y,
                                     uint32_t w, uint32_t h,
                                     const tinydng_decode_options *opts,
                                     tinydng_pixels *out, tinydng_error *err) {
  const tinydng_image_info *img;
  td_geom g;
  tinydng_status st;
  uint8_t *dst;
  uint8_t owns = 0;
  size_t region_row, region_size;

  tinydng_error_clear(err);
  if (!ctx || !doc || !out || w == 0u || h == 0u) {
    return TINYDNG_E_INVALID_ARG;
  }
  memset(out, 0, sizeof(*out));
  img = tinydng_image_get(doc, image_idx);
  if (!img) {
    return TINYDNG_E_INVALID_ARG;
  }
  st = td_compute_geom(ctx, img, &g, err);
  if (st != TINYDNG_OK) {
    return st;
  }
  if (x >= g.width || y >= g.height || w > g.width - x || h > g.height - y) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_DECODE, 0, 0, 0,
                 "region out of bounds");
    return TINYDNG_E_INVALID_ARG;
  }
  if (!td_safe_mul_size((size_t)w, g.pixel_stride, &region_row) ||
      !td_safe_mul_size(region_row, (size_t)h, &region_size)) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_DECODE, 0, 0, 0,
                 "region size overflow");
    return TINYDNG_E_BOUNDS;
  }
  st = td_alloc_or_use_dst(ctx, opts, region_size, &dst, &owns, err);
  if (st != TINYDNG_OK) {
    return st;
  }
  st = td_decode_window(ctx, doc, img, &g, x, y, w, h, dst, region_row,
                        td_effective_threads(opts, img->segment_count), err);
  if (st != TINYDNG_OK) {
    if (owns) {
      td_ctx_free(ctx, dst);
    }
    return st;
  }
  out->data = dst;
  out->size = region_size;
  out->width = w;
  out->height = h;
  out->samples_per_pixel = g.spp;
  out->bits_per_sample = g.out_bps;
  out->sample_format = g.sample_format;
  out->owns_memory = owns;
  return TINYDNG_OK;
}

tinydng_status tinydng_decode_segment(tinydng_context *ctx,
                                      const tinydng_document *doc,
                                      size_t image_idx, size_t seg_idx,
                                      const tinydng_decode_options *opts,
                                      tinydng_pixels *out, tinydng_error *err) {
  const tinydng_image_info *img;
  tinydng_segment seg;
  tinydng_error_clear(err);
  if (!ctx || !doc || !out) {
    return TINYDNG_E_INVALID_ARG;
  }
  img = tinydng_image_get(doc, image_idx);
  if (!img || seg_idx >= img->segment_count) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_DECODE, 0, 0, 0,
                 "segment index out of range");
    return TINYDNG_E_INVALID_ARG;
  }
  seg = img->segments[seg_idx];
  return tinydng_decode_region(ctx, doc, image_idx, seg.x, seg.y, seg.w, seg.h,
                               opts, out, err);
}
