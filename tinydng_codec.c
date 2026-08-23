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
  uint16_t packed;        /* 1 => keep raw sub-byte packed bytes */
  uint16_t invert_1bit;   /* 1 => map stored bit 1 -> 0, 0 -> 255
                             (PSD bitmap mode: bit 1 = black) */
} td_geom;

/* Forward decls (defined below). */
static int td_packed_row_bytes(uint32_t width, uint16_t spp, uint16_t bps,
                               size_t *out);

static tinydng_status td_compute_geom(tinydng_context *ctx,
                                       const tinydng_image_info *img,
                                       td_geom *g, int keep_packed,
                                       tinydng_error *err) {
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

  /* Sub-byte packed output is only defined for chunky (planar config 1)
     sub-byte samples; planar images fall back to the unpacked layout. */
  g->packed = (uint16_t)((keep_packed && g->bps > 8u && g->bps < 16u &&
                          g->planar != 2u) ? 1u : 0u);
  if (g->packed) {
    size_t rowb;
    g->out_bps = g->bps;
    g->out_bytes = (size_t)(g->bps + 7u) / 8u;
    if (g->spp == 0u || g->width == 0u || g->height == 0u) {
      td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_DECODE, 0, 0, 0,
                   "invalid decode geometry");
      return TINYDNG_E_PARSE;
    }
    if (!td_packed_row_bytes(g->width, g->spp, g->bps, &rowb) ||
        !td_safe_mul_size(rowb, (size_t)g->height, &g->image_size)) {
      td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_DECODE, 0, 0, 0,
                   "decoded image size overflow");
      return TINYDNG_E_BOUNDS;
    }
    g->pixel_stride = rowb; /* per-row packed bytes (no per-pixel stride) */
    g->row_stride = rowb;
    return TINYDNG_OK;
  }

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
  if (g->planar != 1u && g->planar != 2u) {
    td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_DECODE, 0, 0, 0,
                 "invalid planar configuration %u", (unsigned)g->planar);
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

/* Reusable growable scratch buffer. Per-worker instances keep multi-segment
   decodes from hammering the (locked) context allocator for every segment.
   Ownership: allocated through the ctx allocator, released with
   td_scratch_free when the worker finishes. */
typedef struct {
  uint8_t* p;
  size_t cap;
} td_scratch;

static void td_scratch_free(tinydng_context* ctx, td_scratch* s) {
  if (s->p) {
    td_ctx_free(ctx, s->p);
    s->p = NULL;
    s->cap = 0;
  }
}

static uint8_t* td_scratch_get(tinydng_context* ctx, td_scratch* s, size_t need,
                               tinydng_error* err) {
  if (s->cap < need) {
    td_ctx_free(ctx, s->p);
    s->p = (uint8_t*)td_ctx_alloc(ctx, need, err);
    if (!s->p) {
      s->cap = 0;
      return NULL;
    }
    s->cap = need;
  }
  return s->p;
}

/* Obtain a contiguous pointer to a segment's compressed/raw bytes. Uses
   zero-copy map when available; otherwise allocates `*owned` and reads.
   When `re` is non-NULL it is used as a reusable growable read buffer (the
   result stays valid only until the next request on the same scratch), which
   lets multi-segment decodes avoid per-segment allocator traffic. */
static const uint8_t* td_segment_bytes(tinydng_context* ctx, tinydng_io* io,
                                       uint64_t io_size, uint64_t off,
                                       size_t len, td_scratch* re,
                                       tinydng_error* err) {
  uint8_t *buf;
  if (len == 0u) {
    return NULL;
  }
  if (io->map) {
    const uint8_t *p = io->map(io, off, len);
    if (p) {
      return p;
    }
  }
  if (re) {
    buf = td_scratch_get(ctx, re, len, err);
  } else {
    buf = (uint8_t*)td_ctx_alloc(ctx, len, err);
  }
  if (!buf) {
    return NULL;
  }
  if (td_io_view(io, io_size, off, len, buf, len) == NULL) {
    if (!re) {
      td_ctx_free(ctx, buf);
    }
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_DECODE, 0, 0, off,
                 "segment bytes out of range");
    return NULL;
  }
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
      /* Pointer-stepped per channel: cur += prev with prev tracking the
         previous sample of the SAME channel. Semantically identical to
         p[x*spp+c] += p[(x-1)*spp+c], without per-iteration multiplications. */
      if (bytes == 1u) {
        uint8_t* p = buf + base + c;
        uint8_t prev = p[0];
        for (x = 1; x < w; x++) {
          p += spp;
          prev = (uint8_t)(*p + prev);
          *p = prev;
        }
      } else if (bytes == 2u) {
        uint8_t* p = buf + (base + c) * 2u;
        const size_t step = (size_t)spp * 2u;
        uint16_t prev;
        memcpy(&prev, p, 2);
        for (x = 1; x < w; x++) {
          uint16_t cur;
          p += step;
          memcpy(&cur, p, 2);
          cur = (uint16_t)(cur + prev);
          memcpy(p, &cur, 2);
          prev = cur;
        }
      } else if (bytes == 4u) {
        uint8_t* p = buf + (base + c) * 4u;
        const size_t step = (size_t)spp * 4u;
        uint32_t prev;
        memcpy(&prev, p, 4);
        for (x = 1; x < w; x++) {
          uint32_t cur;
          p += step;
          memcpy(&cur, p, 4);
          cur = cur + prev;
          memcpy(p, &cur, 4);
          prev = cur;
        }
      }
    }
  }
}

/* Fast row unpackers for the common raw depths. Groups are chosen so each
   consumes a whole number of bytes (12b: 2 samples / 3 bytes, 10b: 4 / 5,
   14b: 4 / 7), removing the per-sample refill branch of the generic loop.
   `n` is the sample count; tails fall back to the byte-aligned generic path. */
static void td_unpack_tail(const uint8_t* rp, uint16_t* dst, size_t n,
                           unsigned bps) {
  uint32_t bitbuf = 0;
  int nbits = 0;
  size_t s;
  for (s = 0; s < n; s++) {
    uint32_t v;
    while (nbits < (int)bps) {
      bitbuf = (bitbuf << 8) | (uint32_t)(*rp++);
      nbits += 8;
    }
    v = (bitbuf >> (nbits - (int)bps)) & ((1u << bps) - 1u);
    nbits -= (int)bps;
    bitbuf &= (1u << nbits) - 1u;
    dst[s] = (uint16_t)v;
  }
}

static void td_unpack_row_12(const uint8_t* rp, uint16_t* dst, size_t n) {
  size_t i = 0;
  for (; i + 2 <= n; i += 2, rp += 3) {
    uint32_t t =
        ((uint32_t)rp[0] << 16) | ((uint32_t)rp[1] << 8) | (uint32_t)rp[2];
    dst[i] = (uint16_t)(t >> 12);
    dst[i + 1] = (uint16_t)(t & 0x0FFFu);
  }
  td_unpack_tail(rp, dst + i, n - i, 12u);
}

static void td_unpack_row_10(const uint8_t* rp, uint16_t* dst, size_t n) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4, rp += 5) {
    uint64_t t = ((uint64_t)rp[0] << 32) | ((uint64_t)rp[1] << 24) |
                 ((uint64_t)rp[2] << 16) | ((uint64_t)rp[3] << 8) |
                 (uint64_t)rp[4];
    dst[i] = (uint16_t)(t >> 30);
    dst[i + 1] = (uint16_t)((t >> 20) & 0x03FFu);
    dst[i + 2] = (uint16_t)((t >> 10) & 0x03FFu);
    dst[i + 3] = (uint16_t)(t & 0x03FFu);
  }
  td_unpack_tail(rp, dst + i, n - i, 10u);
}

static void td_unpack_row_14(const uint8_t* rp, uint16_t* dst, size_t n) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4, rp += 7) {
    uint64_t t = ((uint64_t)rp[0] << 48) | ((uint64_t)rp[1] << 40) |
                 ((uint64_t)rp[2] << 32) | ((uint64_t)rp[3] << 24) |
                 ((uint64_t)rp[4] << 16) | ((uint64_t)rp[5] << 8) |
                 (uint64_t)rp[6];
    dst[i] = (uint16_t)(t >> 42);
    dst[i + 1] = (uint16_t)((t >> 28) & 0x3FFFu);
    dst[i + 2] = (uint16_t)((t >> 14) & 0x3FFFu);
    dst[i + 3] = (uint16_t)(t & 0x3FFFu);
  }
  td_unpack_tail(rp, dst + i, n - i, 14u);
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
/* Byte count of one row of MSB-first packed samples (width*spp*bps bits,
   rounded up to a whole byte). Rows are independent and byte-aligned. */
static int td_packed_row_bytes(uint32_t width, uint16_t spp, uint16_t bps,
                               size_t *out) {
  size_t bits;
  if (!td_safe_mul_size((size_t)width, (size_t)spp, &bits) ||
      !td_safe_mul_size(bits, (size_t)bps, &bits)) {
    return 0;
  }
  *out = bits / 8u + ((bits % 8u) != 0u);
  return 1;
}

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
    in_row = bits / 8u + ((bits % 8u) != 0u);
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

  if (g->packed) {
    /* KEEP_PACKED: emit the raw stored bytes (already MSB-first, row-aligned,
       predictor already applied at the sample level by the encoder). */
    size_t rowb;
    if (!td_packed_row_bytes(bw, g->spp, g->bps, &rowb)) {
      td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_DECODE, 0, 0, 0,
                   "packed row size overflow");
      return TINYDNG_E_BOUNDS;
    }
    for (y = 0; y < bh; y++) {
      memcpy(block + (size_t)y * rowb, src + (size_t)y * rowb, rowb);
    }
    return TINYDNG_OK;
  }

  if (g->bps == 8u || g->bps == 16u || g->bps == 32u) {
    size_t stored_bytes = (size_t)g->bps / 8u;
    size_t samples_per_row, in_row_bytes;
    if (!td_safe_mul_size((size_t)bw, (size_t)g->spp, &samples_per_row) ||
        !td_safe_mul_size(samples_per_row, stored_bytes, &in_row_bytes)) {
      td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_DECODE, 0, 0, 0,
                   "stored row size overflow");
      return TINYDNG_E_BOUNDS;
    }
    int need_swap = (stored_bytes > 1u) && (big_endian != td_host_big());
    for (y = 0; y < bh; y++) {
      memcpy(block + (size_t)y * in_row_bytes, src + (size_t)y * in_row_bytes,
             in_row_bytes);
    }
    if (need_swap) {
      size_t total_samples;
      size_t i;
      if (!td_safe_mul_size(samples_per_row, (size_t)bh, &total_samples)) {
        td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_DECODE, 0, 0, 0,
                     "stored sample count overflow");
        return TINYDNG_E_BOUNDS;
      }
      if (stored_bytes == 2u) {
        uint8_t* q = block;
        for (i = 0; i < total_samples; i++, q += 2) {
          uint8_t t = q[0];
          q[0] = q[1];
          q[1] = t;
        }
      } else {
        uint8_t* q = block;
        for (i = 0; i < total_samples; i++, q += 4) {
          uint8_t a = q[0], b = q[1], c = q[2], d = q[3];
          q[0] = d;
          q[1] = c;
          q[2] = b;
          q[3] = a;
        }
      }
    }
    return TINYDNG_OK;
  }

  /* Packed sub-byte depths (10/12/14): MSB-first, rows byte-aligned. */
  {
    size_t samples_per_row, bits_per_row, in_row_bytes;
    if (!td_safe_mul_size((size_t)bw, (size_t)g->spp, &samples_per_row) ||
        !td_safe_mul_size(samples_per_row, (size_t)g->bps, &bits_per_row)) {
      td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_DECODE, 0, 0, 0,
                   "packed row size overflow");
      return TINYDNG_E_BOUNDS;
    }
    in_row_bytes = bits_per_row / 8u + ((bits_per_row % 8u) != 0u);
    for (y = 0; y < bh; y++) {
      const uint8_t *rp = src + (size_t)y * in_row_bytes;
      size_t row_base = (size_t)y * samples_per_row;
      if (g->out_bytes == 1u) {
        /* bps < 8: 8-bit output elements (incl. PSD bitmap polarity). */
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
          if (g->invert_1bit && g->bps == 1u) {
            /* PSD bitmap polarity: stored bit 1 = black. */
            v = v ? 0u : 255u;
          }
          ((uint8_t *)block)[row_base + s] = (uint8_t)v;
        }
      } else if (g->bps == 12u && samples_per_row >= 2u &&
                 in_row_bytes == ((samples_per_row / 2u) * 3u)) {
        /* Fast paths for the common raw depths; group boundaries are
           byte-aligned so the tail handler resumes cleanly. */
        td_unpack_row_12(rp, (uint16_t*)(void*)block + row_base,
                         samples_per_row);
      } else if (g->bps == 10u && samples_per_row >= 4u &&
                 in_row_bytes == ((samples_per_row / 4u) * 5u)) {
        td_unpack_row_10(rp, (uint16_t*)(void*)block + row_base,
                         samples_per_row);
      } else if (g->bps == 14u && samples_per_row >= 4u &&
                 in_row_bytes == ((samples_per_row / 4u) * 7u)) {
        td_unpack_row_14(rp, (uint16_t*)(void*)block + row_base,
                         samples_per_row);
      } else {
        /* MSB-first accumulator: shift whole bytes in, pull bps bits out of
           the top. nbits stays below bps+8 <= 24, so one 32-bit value
           suffices. */
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
          bitbuf &= (1u << nbits) - 1u;
          {
            uint16_t w16 = (uint16_t)v;
            memcpy((uint8_t*)block + (row_base + s) * 2u, &w16, sizeof(w16));
          }
        }
      }
    }
  }
  return TINYDNG_OK;
}

/* Decode one uncompressed segment into `block`. */
static tinydng_status td_decode_block_uncompressed(
    tinydng_context* ctx, tinydng_io* io, uint64_t io_size, int big_endian,
    const td_geom* g, const tinydng_segment* seg, uint32_t bw, uint32_t bh,
    uint8_t* block, td_scratch* in_re, tinydng_error* err) {
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
  src = td_segment_bytes(ctx, io, io_size, seg->offset, need, in_re, err);
  if (!src) {
    return td_error_status_or(err, TINYDNG_E_BOUNDS);
  }
  st = td_fill_block_from_stored(g, big_endian, src, need, bw, bh, block, err);
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
/* TIFF LZW (early-change, MSB-first). Returns decoded byte count or -1.
 *
 * Like libtiff, decoding stops as soon as the output buffer is satisfied:
 * real-world encoders (Photoshop, NASA PDS) may terminate strips without an
 * explicit EOI and pad the final bits with garbage, so consuming codes past
 * the expected byte count would spuriously fail valid files.
 */
static long td_lzw_decode(const uint8_t *in, size_t in_len, uint8_t *out,
                          size_t out_cap) {
  enum { CLEAR = 256, EOI = 257 };
  uint16_t prefix[4096];
  uint8_t suffix[4096];
  int code_size = 9;
  int next_code = 258;
  size_t out_pos = 0;
  uint64_t bitbuf = 0;
  int bitcnt = 0;
  size_t in_pos = 0;
  int prev = -1;

  for (;;) {
    int code;
    /* Refill up to 5 bytes at once so <=12-bit codes rarely stall; identical
       semantics to a byte-at-a-time MSB-first reader. */
    while (bitcnt < code_size) {
      size_t avail = in_len - in_pos;
      size_t take = avail < 5u ? avail : 5u;
      uint64_t v = 0;
      size_t i;
      if (take == 0) {
        break;
      }
      for (i = 0; i < take; i++) {
        v = (v << 8) | in[in_pos + i];
      }
      in_pos += take;
      bitbuf = (bitbuf << (take * 8u)) | v;
      bitcnt += (int)(take * 8u);
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
    if (prev == -1) {
      if (code >= 256) {
        return -1;
      }
      if (out_pos >= out_cap) {
        return -1;
      }
      out[out_pos++] = (uint8_t)code;
      prev = code;
      if (out_pos >= out_cap) {
        break; /* output satisfied */
      }
      continue;
    }
    {
      int kwk = (code == next_code);
      int head = kwk ? prev : code;
      int c = head;
      size_t len = 0;
      if (!kwk && code > next_code) {
        return -1; /* invalid code */
      }
      /* Measure the string so the capacity is verified with one check, then
         emit it forward directly into the output (no intermediate stack).
         The prefix-chain walk visits characters last-to-first, so bytes are
         written through a descending cursor. */
      while (c >= 256) {
        len++;
        c = prefix[c];
      }
      /* c == leading literal == first char of this string */
      len += kwk ? 2u : 1u;
      if (out_pos > out_cap || len > out_cap - out_pos) {
        return -1;
      }
      {
        size_t w_last = out_pos + len - 1u; /* inclusive last slot     */
        size_t w = kwk ? (w_last - 1u) : w_last;
        if (kwk) {
          /* KwKwK: one extra copy of this string's first char. */
          out[w_last] = (uint8_t)c;
        }
        c = head;
        while (c >= 256) {
          out[w--] = suffix[c];
          c = prefix[c];
        }
        out[out_pos] = (uint8_t)c; /* leading literal / first char */
        out_pos += len;
        /* Add new entry prev + firstchar(this string). */
        if (next_code < 4096) {
          prefix[next_code] = (uint16_t)prev;
          suffix[next_code] = (uint8_t)c;
          next_code++;
          /* TIFF early change: widen one code before the table fills. */
          if (code_size < 12 && next_code == (1 << code_size) - 1) {
            code_size++;
          }
        }
      }
      prev = code;
    }
    if (out_pos >= out_cap) {
      break; /* output satisfied */
    }
  }
  return (long)out_pos;
}
#endif /* TINYDNG_NO_LZW */

#ifndef TINYDNG_NO_PACKBITS
long td_packbits_decode(const uint8_t *in, size_t in_len, uint8_t *out,
                        size_t out_cap) {
  size_t ip = 0;
  size_t op = 0;
  while (ip < in_len) {
    int8_t n = (int8_t)in[ip++];
    if (n >= 0) {
      size_t cnt = (size_t)n + 1u;
      if (cnt > in_len - ip || cnt > out_cap - op) {
        return -1;
      }
      memcpy(out + op, in + ip, cnt);
      op += cnt;
      ip += cnt;
    } else if (n != -128) {
      size_t cnt = (size_t)(1 - n);
      uint8_t v;
      if (ip >= in_len || cnt > out_cap - op) {
        return -1;
      }
      v = in[ip++];
      memset(out + op, v, cnt);
      op += cnt;
    }
  }
  return (long)op;
}
#endif /* TINYDNG_NO_PACKBITS */

/* Decode one LZW/PackBits/ZIP segment into `block`. `in_re`/`st_re` are
   optional reusable per-worker scratches (input bytes / decompressed bytes). */
static tinydng_status td_decode_block_compressed(
    tinydng_context* ctx, tinydng_io* io, uint64_t io_size, int big_endian,
    const td_geom* g, const tinydng_segment* seg, uint16_t compression,
    uint32_t bw, uint32_t bh, uint8_t* block, td_scratch* in_re,
    td_scratch* st_re, tinydng_error* err) {
  const uint8_t *src;
  uint8_t* stored = NULL;
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
                         in_re, err);
  if (!src) {
    return td_error_status_or(err, TINYDNG_E_BOUNDS);
  }

  /* Fast path: when the stored layout equals the output layout (8-bit
     samples, or matching byte order for 16/32), decompress straight into the
     destination block and skip both the scratch and the final copy pass.
     PSD zip-with-prediction is excluded: its unpredict pass must run before
     the block conversion. */
  if (compression != (uint16_t)TD_COMPRESSION_PSD_ZIP_PRED && !g->packed &&
      g->bps == g->out_bps && (g->bps == 8u || big_endian == td_host_big())) {
#ifndef TINYDNG_NO_LZW
    if (compression == TINYDNG_COMPRESSION_LZW) {
      got = td_lzw_decode(src, (size_t)seg->byte_count, block, need);
    } else
#endif
#ifndef TINYDNG_NO_PACKBITS
        if (compression == TINYDNG_COMPRESSION_PACKBITS) {
      got = td_packbits_decode(src, (size_t)seg->byte_count, block, need);
    } else
#endif
#ifndef TINYDNG_NO_ZIP
        if (compression == TINYDNG_COMPRESSION_ZIP) {
      mz_ulong dlen = (mz_ulong)need;
      int mzr = mz_uncompress(block, &dlen, src, (mz_ulong)seg->byte_count);
      got = (mzr == MZ_OK) ? (long)dlen : -1;
    } else
#endif
    {
      /* fall through to the generic path below */
    }
    if (got >= 0) {
      if ((size_t)got != need) {
        td_set_error(err, TINYDNG_E_DECODE, TINYDNG_STAGE_DECODE, 0, 0, 0,
                     "decompressed size mismatch: got=%ld need=%zu", got, need);
        return TINYDNG_E_DECODE;
      }
      return TINYDNG_OK; /* predictor is applied by the caller */
    }
    got = -1; /* fall back to the generic path for uniform error reporting */
  }

  stored = st_re ? td_scratch_get(ctx, st_re, need, err)
                 : (uint8_t*)td_ctx_alloc(ctx, need, err);
  if (!stored) {
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
      if (!st_re) {
        td_ctx_free(ctx, stored);
      }
      td_set_error(err, TINYDNG_E_UNSUPPORTED, TINYDNG_STAGE_DECODE, 0, 0, 0,
                   "compression %u not enabled", (unsigned)compression);
      return TINYDNG_E_UNSUPPORTED;
  }

  if (got < 0) {
    if (!st_re) {
      td_ctx_free(ctx, stored);
    }
    td_set_error(err, TINYDNG_E_DECODE, TINYDNG_STAGE_DECODE, 0, 0, seg->offset,
                 "decompression failed (comp=%u)", (unsigned)compression);
    return TINYDNG_E_DECODE;
  }

  st = td_finish_compressed(ctx, big_endian, g, stored, (size_t)got, need, bw,
                            bh, block, err);
  if (!st_re) {
    td_ctx_free(ctx, stored);
  }
  return st;
}

#ifndef TINYDNG_NO_BASELINE_JPEG
/* Decode one baseline/lossy JPEG segment (8-bit) into `block`. */
static tinydng_status td_decode_block_baseline(
    tinydng_context* ctx, tinydng_io* io, uint64_t io_size, const td_geom* g,
    const tinydng_segment* seg, uint8_t* block, uint32_t bw, uint32_t bh,
    td_scratch* in_re, tinydng_error* err) {
  td_scratch local;
  td_scratch* sc = in_re ? in_re : &local;
  const uint8_t *src;
  int w = 0, h = 0, comp = 0;
  stbi_uc *pixels;

  if (!in_re) {
    local.p = NULL;
    local.cap = 0;
  }
  if (seg->byte_count > (uint64_t)INT32_MAX) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_DECODE, 0, 0, seg->offset,
                 "jpeg segment too large");
    return TINYDNG_E_BOUNDS;
  }
  src = td_segment_bytes(ctx, io, io_size, seg->offset, (size_t)seg->byte_count,
                         sc, err);
  if (!src) {
    return td_error_status_or(err, TINYDNG_E_BOUNDS);
  }
  /* Decompression-bomb guard: stb_image allocates through libc malloc,
   * outside the tracked allocator and its memory cap. Probe the header and
   * reject images whose decoded size cannot fit the remaining budget before
   * letting stb allocate. */
  if (!stbi_info_from_memory(src, (int)seg->byte_count, &w, &h, &comp)) {
    if (sc == &local) td_scratch_free(ctx, sc);
    td_set_error(err, TINYDNG_E_DECODE, TINYDNG_STAGE_DECODE, 0, 0, seg->offset,
                 "jpeg header parse failed: %s", stbi_failure_reason());
    return TINYDNG_E_DECODE;
  }
  {
    uint64_t need_px = (uint64_t)(uint32_t)w * (uint64_t)(uint32_t)h;
    uint64_t need_bytes;
    uint64_t budget;
    /* Read the accounting under the allocator's lock discipline so a
     * multi-threaded decode never races td_ctx_alloc's bookkeeping. */
    td_mutex *L = ctx->mt_active ? ctx->lock : NULL;
    td_mutex_lock(L);
    budget = ctx->memory_cap_bytes ? (ctx->memory_cap_bytes - ctx->memory_used)
                                   : UINT64_MAX;
    td_mutex_unlock(L);
    if (!td_safe_mul_u64(need_px, (uint64_t)((unsigned)g->spp),
                         &need_bytes) ||
        need_bytes > budget) {
      if (sc == &local) td_scratch_free(ctx, sc);
      td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_DECODE, 0, 0,
                   seg->offset,
                   "jpeg decoded size %dx%dx%u exceeds memory budget (%llu "
                   "bytes left)",
                   w, h, (unsigned)g->spp, (unsigned long long)budget);
      return TINYDNG_E_BOUNDS;
    }
  }
  pixels = stbi_load_from_memory(src, (int)seg->byte_count, &w, &h, &comp,
                                 (int)g->spp);
  if (sc == &local) td_scratch_free(ctx, sc);
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
  if (UINT64_MAX - s->base < off) {
    return 0;
  }
  return s->io->read(s->io, s->base + off, dst, len);
}

static uint64_t td_lj92_stream_size(void *user) {
  return ((td_lj92_stream_io *)user)->size;
}

static void* td_lj92_ctx_alloc(void* user, size_t size) {
  return td_ctx_alloc((tinydng_context*)user, size, NULL);
}

static void td_lj92_ctx_free(void* user, void* ptr) {
  td_ctx_free((tinydng_context*)user, ptr);
}

/* Decode a JPEG lossless frame into an interleaved u16 sample array. SOF11
   exposes native component planes; when every component has the same native
   geometry they can alias the interleaved destination without an upsample or
   scratch copy. Unequal native planes are deliberately not resampled here. */
static int td_lj92_decode_u16(tdng_lj92 lj, uint16_t* dst, int w, int h,
                              int comps) {
  tdng_lj92_frame_info info;
  tdng_lj92_plane planes[TDNG_LJ92_MAX_COMPONENTS];
  size_t total;
  int ret = tdng_lj92_get_frame_info(lj, &info);
  if (ret != TDNG_LJ92_ERROR_NONE) return ret;
  if (info.sof_marker != 0xCB) {
    return tdng_lj92_decode(lj, dst, w * comps, 0, NULL, 0);
  }
  if (info.component_count != comps || comps < 1 ||
      comps > TDNG_LJ92_MAX_COMPONENTS ||
      !td_safe_mul_size((size_t)w * (size_t)comps, (size_t)h, &total)) {
    return TDNG_LJ92_ERROR_CORRUPT;
  }
  memset(planes, 0, sizeof(planes));
  for (int c = 0; c < comps; c++) {
    if (info.components[c].width != (uint32_t)w ||
        info.components[c].height != (uint32_t)h) {
      return TDNG_LJ92_ERROR_UNSUPPORTED;
    }
    planes[c].data = dst + c;
    planes[c].capacity_samples = total - (size_t)c;
    planes[c].row_stride_samples = (size_t)w * (size_t)comps;
    planes[c].pixel_stride_samples = (size_t)comps;
    planes[c].width = (uint32_t)w;
    planes[c].height = (uint32_t)h;
  }
  return tdng_lj92_decode_planes(lj, planes, (size_t)comps);
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
  td_scratch owned;
  const uint8_t *src;
  tdng_lj92 lj = NULL;
  int w = 0, h = 0, bits = 0, comps = 0;
  int ret;
  tinydng_status st = TINYDNG_OK;
  /* The streaming adapter must outlive tdng_lj92_decode, which reads it
     through the decoder's stream_user until tdng_lj92_close. */
  td_lj92_stream_io sio;
  tdng_lj92_allocator ljalloc;

  owned.p = NULL;
  owned.cap = 0;
  ljalloc.alloc = td_lj92_ctx_alloc;
  ljalloc.free = td_lj92_ctx_free;
  ljalloc.user = ctx;

  if (seg->byte_count > (uint64_t)INT32_MAX) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_DECODE, 0, 0, seg->offset,
                 "ljpeg segment too large");
    return TINYDNG_E_BOUNDS;
  }

  if (io->map) {
    src = td_segment_bytes(ctx, io, io_size, seg->offset,
                           (size_t)seg->byte_count, &owned, err);
    if (!src) {
      return td_error_status_or(err, TINYDNG_E_BOUNDS);
    }
    ret = tdng_lj92_open_ex(&lj, src, (int)seg->byte_count, &ljalloc, &w, &h,
                            &bits, &comps);
  } else {
    /* Streaming decode through the io backend (no full-segment copy). */
    sio.io = io;
    sio.base = seg->offset;
    sio.size = seg->byte_count;
    ret = tdng_lj92_open_streaming_ex(&lj, &sio, td_lj92_stream_read,
                                      td_lj92_stream_size, &ljalloc, &w, &h,
                                      &bits, &comps);
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
  {
    tdng_lj92_frame_info info;
    ret = tdng_lj92_get_frame_info(lj, &info);
    if (ret != TDNG_LJ92_ERROR_NONE) {
      td_set_error(err, TINYDNG_E_DECODE, TINYDNG_STAGE_DECODE, 0, 0,
                   seg->offset, "invalid lossless JPEG frame metadata");
      st = TINYDNG_E_DECODE;
      goto cleanup;
    }
    if (info.sof_marker == 0xCB && !io->map) {
      /* The advanced native-plane engine is memory-backed. SOF11 is rare in
         DNG, so materialize only this segment; ordinary SOF3 retains the
         zero-materialization streaming fast path. */
      tdng_lj92_close(lj);
      lj = NULL;
      src = td_segment_bytes(ctx, io, io_size, seg->offset,
                             (size_t)seg->byte_count, &owned, err);
      if (!src) {
        st = td_error_status_or(err, TINYDNG_E_BOUNDS);
        goto cleanup;
      }
      ret = tdng_lj92_open_ex(&lj, src, (int)seg->byte_count, &ljalloc, &w, &h,
                              &bits, &comps);
      if (ret != TDNG_LJ92_ERROR_NONE) {
        td_set_error(err, TINYDNG_E_DECODE, TINYDNG_STAGE_DECODE, 0, 0,
                     seg->offset, "SOF11 reopen failed ret=%d", ret);
        st = TINYDNG_E_DECODE;
        goto cleanup;
      }
    }
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

  if (g->packed) {
    /* KEEP_PACKED with sub-byte depth (bps 9..15): the destination holds
       MSB-first packed rows, not u16 samples. Decode to a full-width u16
       scratch, then repack each row down to g->bps bits.
       (Pre-fix this path wrote raw u16 samples into the packed-size block:
       a heap overflow reachable via TINYDNG_DEC_KEEP_PACKED on 10/12/14-bit
       LJPEG files.) */
    uint16_t* scratch = NULL;
    size_t row_samples = (size_t)w * (size_t)comps;
    size_t scratch_bytes, rowb, y, x;
    unsigned bps = g->bps;
    if (!td_safe_mul_size(row_samples, (size_t)h * sizeof(uint16_t),
                          &scratch_bytes)) {
      td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_DECODE, 0, 0,
                   seg->offset, "ljpeg scratch size overflow");
      st = TINYDNG_E_BOUNDS;
      goto cleanup;
    }
    if (!td_packed_row_bytes(bw, g->spp, bps, &rowb)) {
      td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_DECODE, 0, 0,
                   seg->offset, "packed row size overflow");
      st = TINYDNG_E_BOUNDS;
      goto cleanup;
    }
    scratch = (uint16_t*)td_ctx_alloc(ctx, scratch_bytes, err);
    if (!scratch) {
      st = TINYDNG_E_OOM;
      goto cleanup;
    }
    ret = td_lj92_decode_u16(lj, scratch, w, h, comps);
    if (ret != TDNG_LJ92_ERROR_NONE) {
      td_ctx_free(ctx, scratch);
      st = ret == TDNG_LJ92_ERROR_UNSUPPORTED ? TINYDNG_E_UNSUPPORTED
                                              : TINYDNG_E_DECODE;
      td_set_error(err, st, TINYDNG_STAGE_DECODE, 0, 0, seg->offset,
                   "tdng_lj92_decode failed ret=%d", ret);
      goto cleanup;
    }
    {
      const uint16_t mask = (uint16_t)((1u << bps) - 1u);
      for (y = 0; y < bh; y++) {
        uint8_t* dstrow = block + y * rowb;
        const uint16_t* srow = scratch + y * row_samples;
        memset(dstrow, 0, rowb);
        size_t bitpos = 0;
        for (x = 0; x < row_samples; x++) {
          uint16_t v = (uint16_t)(srow[x] & mask);
          int k;
          for (k = (int)bps - 1; k >= 0; k--) {
            dstrow[bitpos >> 3] =
                (uint8_t)(dstrow[bitpos >> 3] |
                          (uint8_t)(((v >> k) & 1u) << (7u - (bitpos & 7u))));
            bitpos++;
          }
        }
      }
    }
    td_ctx_free(ctx, scratch);
    st = TINYDNG_OK;
    goto cleanup;
  }

  /* The dims check above guarantees w*comps == bw*spp and h == bh, i.e. the
     LJPEG sample grid matches the block exactly (w*comps*h samples). Decode
     straight into `block` as native-order u16 values: the codec treats
     LJPEG blocks as sample-value arrays with no endian-swap pass, and the
     scratch/dst allocation is suitably aligned for uint16_t. */
  ret = td_lj92_decode_u16(lj, (uint16_t*)(void*)block, w, h, comps);
  if (ret != TDNG_LJ92_ERROR_NONE) {
    st = ret == TDNG_LJ92_ERROR_UNSUPPORTED ? TINYDNG_E_UNSUPPORTED
                                            : TINYDNG_E_DECODE;
    td_set_error(err, st, TINYDNG_STAGE_DECODE, 0, 0, seg->offset,
                 "tdng_lj92_decode failed ret=%d", ret);
  }

cleanup:
  if (lj) {
    tdng_lj92_close(lj);
  }
  td_scratch_free(ctx, &owned);
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

/* Copy `nsamp` MSB-first packed samples (each `bps` bits) from `src` beginning
   at sample index `src0` into `dst` beginning at sample index `dst0`.
   Used by the KEEP_PACKED blit for horizontal sub-window clipping. */
static void td_packed_copy_samples(const uint8_t *src, uint8_t *dst,
                                   size_t src0, size_t dst0, size_t nsamp,
                                   uint16_t bps) {
  size_t s;
  for (s = 0; s < nsamp; s++) {
    size_t src_bit = (src0 + s) * bps;
    size_t dst_bit = (dst0 + s) * bps;
    uint32_t v = 0;
    int b;
    for (b = 0; b < (int)bps; b++) {
      size_t sb = src_bit + (size_t)b;
      int bit = (src[sb >> 3u] >> (7 - (int)(sb & 7u))) & 1;
      v = (v << 1) | (uint32_t)bit;
    }
    for (b = 0; b < (int)bps; b++) {
      size_t db = dst_bit + (size_t)b;
      int bit = (int)((v >> ((int)bps - 1 - b)) & 1u);
      uint8_t mask = (uint8_t)(1u << (7 - (int)(db & 7u)));
      if (bit) {
        dst[db >> 3u] |= mask;
      } else {
        dst[db >> 3u] &= (uint8_t)~mask;
      }
    }
  }
}

/* Blit a decoded packed block (rows of byte-aligned MSB-first samples) into
   the packed destination raster window. Horizontal clipping bit-copies the
   affected sample range so arbitrary sub-windows remain correct. */
static void td_blit_block_packed(const td_geom *g, const uint8_t *block,
                                 uint32_t bw, uint32_t bh, uint32_t seg_x,
                                 uint32_t seg_y, uint8_t *dst, uint32_t dst_x0,
                                 uint32_t dst_y0, uint32_t dst_w, uint32_t dst_h,
                                 size_t dst_row_stride) {
  uint32_t copy_w, copy_h, ty;
  size_t block_row;
  int64_t rel_x, rel_y;
  uint32_t src_off_x = 0, src_off_y = 0;
  if (!td_packed_row_bytes(bw, g->spp, g->bps, &block_row)) {
    return; /* overflow: nothing safe to do */
  }
  rel_x = (int64_t)seg_x - (int64_t)dst_x0;
  rel_y = (int64_t)seg_y - (int64_t)dst_y0;
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
    const uint8_t *sp = block + (size_t)(src_off_y + ty) * block_row;
    uint8_t *dp = dst + (size_t)((uint32_t)rel_y + ty) * dst_row_stride;
    td_packed_copy_samples(sp, dp, (size_t)src_off_x * g->spp,
                           (size_t)((uint32_t)rel_x) * g->spp,
                           (size_t)copy_w * g->spp, g->bps);
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

/* Per-worker state: owned scratch buffers (grown on demand, reused across
   segments so threaded decodes do not contend on the context allocator for
   every segment) + a private error slot so threads never share the caller's
   error object. */
typedef struct {
  td_decode_par *par;
  uint8_t *block;
  size_t block_cap;
  td_scratch in_sc;     /* raw/compressed input bytes                */
  td_scratch stored_sc; /* decompressed bytes (generic path)         */
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

  /* Skip segments that don't overlap the window.
     Use uint64_t to avoid uint32 overflow in seg->x + seg->w. */
  if ((uint64_t)seg->x >= (uint64_t)par->win_x + par->win_w ||
      (uint64_t)seg->y >= (uint64_t)par->win_y + par->win_h ||
      (uint64_t)seg->x + seg->w <= par->win_x ||
      (uint64_t)seg->y + seg->h <= par->win_y) {
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
    if (gseg->packed) {
      size_t rowb;
      if (!td_packed_row_bytes(bw, gseg->spp, gseg->bps, &rowb) ||
          !td_safe_mul_size(rowb, (size_t)bh, &need)) {
        td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_DECODE, 0, 0, 0,
                     "block size overflow");
        return 0;
      }
    } else if (!td_safe_mul_size((size_t)bw, (size_t)bh, &block_px) ||
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
                                        target, &w->in_sc, err);
      break;
    case TINYDNG_COMPRESSION_OLD_JPEG:
    case TINYDNG_COMPRESSION_NEW_JPEG:
      if (gseg->out_bps <= 8u) {
#ifndef TINYDNG_NO_BASELINE_JPEG
        st = td_decode_block_baseline(ctx, par->io, par->io_size, gseg, seg,
                                      target, bw, bh, &w->in_sc, err);
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
                                    target, bw, bh, &w->in_sc, err);
      break;
#endif
    case TINYDNG_COMPRESSION_LZW:
    case TINYDNG_COMPRESSION_PACKBITS:
    case TINYDNG_COMPRESSION_ZIP:
#if !defined(TINYDNG_NO_PSD) && !defined(TINYDNG_NO_ZIP)
    case TD_COMPRESSION_PSD_ZIP_PRED:
#endif
      st = td_decode_block_compressed(
          ctx, par->io, par->io_size, par->big_endian, gseg, seg,
          par->img->compression, bw, bh, target, &w->in_sc, &w->stored_sc, err);
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
  if (!gseg->packed && !td_apply_predictor(ctx, gseg, target, bw, bh, err)) {
    if (!err || err->status == TINYDNG_OK) {
      td_set_error(err, TINYDNG_E_DECODE, TINYDNG_STAGE_DECODE, 0, 0, 0,
                   "predictor failed");
    }
    return 0;
  }
  if (par->planar) {
    td_blit_block_planar(par->g, target, bw, bh, seg->plane, seg->x, seg->y,
                         par->dst, par->win_x, par->win_y, par->win_w,
                         par->win_h, par->dst_row_stride);
  } else if (gseg->packed) {
    td_blit_block_packed(par->g, target, bw, bh, seg->x, seg->y, par->dst,
                         par->win_x, par->win_y, par->win_w, par->win_h,
                         par->dst_row_stride);
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

  /* Serialize overlapping decodes on the same context: the allocator and
      mt_active are shared, so two concurrent decode calls would corrupt state.
      The mutex is recursive so a PSD smart-object decode (same thread) nests.
      NOTE for future work: a nested decode that engages MT would clear
      mt_active on its exit while the outer decode's workers are still
      running (unlocking their allocator/stdio reads). No current path does
      this -- segment decoders never re-enter tinydng_decode_* -- but any
      feature that decodes inside a worker must keep decodes serial. */
  td_mutex_lock(ctx->decode_guard);

  memset(&par, 0, sizeof(par));
  par.ctx = ctx;
  par.io = (tinydng_io *)&doc->io;
  par.io_size = doc->io_size;
  par.big_endian = doc->big_endian;
  par.img = img;
  par.g = g;
  par.gseg = *g;
  /* PSD bitmap mode stores bit 1 = black (verified against Photoshop files;
   * GIMP's psd plugin maps a set bit to palette index 0 = black too), while
   * TIFF 1-bit conventionally means 1 = max value. Invert+scale only here,
   * and only when not keeping raw packed bytes. */
  if (doc->format == TD_DOC_FORMAT_PSD && g->bps == 1u && !g->packed) {
    par.gseg.invert_1bit = 1u;
  }
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
    st = TINYDNG_E_OOM;
    goto done;
  }
  for (t = 0; t < n; t++) {
    workers[t].par = &par;
    workers[t].block = NULL;
    workers[t].block_cap = 0;
    workers[t].in_sc.p = NULL;
    workers[t].in_sc.cap = 0;
    workers[t].stored_sc.p = NULL;
    workers[t].stored_sc.cap = 0;
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
    td_scratch_free(ctx, &workers[t].in_sc);
    td_scratch_free(ctx, &workers[t].stored_sc);
    if (st == TINYDNG_OK && workers[t].err.status != TINYDNG_OK) {
      if (err) {
        *err = workers[t].err;
      }
      st = workers[t].err.status;
    }
  }
  td_ctx_free(ctx, workers);
done:
  td_mutex_unlock(ctx->decode_guard);
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
  st = td_compute_geom(ctx, img, &g,
                        (opts && (opts->flags & TINYDNG_DEC_KEEP_PACKED)) ? 1 : 0,
                        err);
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
  st = td_compute_geom(ctx, img, &g,
                        (opts && (opts->flags & TINYDNG_DEC_KEEP_PACKED)) ? 1 : 0,
                        err);
  if (st != TINYDNG_OK) {
    return st;
  }
  if (x >= g.width || y >= g.height || w > g.width - x || h > g.height - y) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_DECODE, 0, 0, 0,
                 "region out of bounds");
    return TINYDNG_E_INVALID_ARG;
  }
  if (g.packed) {
    size_t rrow;
    if (!td_packed_row_bytes(w, g.spp, g.bps, &rrow) ||
        !td_safe_mul_size(rrow, (size_t)h, &region_size)) {
      td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_DECODE, 0, 0, 0,
                   "region size overflow");
      return TINYDNG_E_BOUNDS;
    }
    region_row = rrow;
  } else if (!td_safe_mul_size((size_t)w, g.pixel_stride, &region_row) ||
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
