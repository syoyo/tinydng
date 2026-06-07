/*
 * tinydng_write.c - uncompressed TIFF/DNG writer (single image).
 * SPDX-License-Identifier: MIT
 */
#include "td_internal.h"
#include "tiny_dng_ljpeg92_v2.h"

#include <stdio.h>
#include <stdlib.h>

/* TIFF photometric */
#define TD_PHOTO_MINISBLACK 1u
#define TD_PHOTO_RGB 2u
#define TD_PHOTO_CFA 32803u

#define TD_WMAX_ENTRIES 48
#define TD_WMAX_EXTRAS (256u * 1024u)

typedef struct td_wentry {
  uint16_t tag;
  uint16_t type;
  uint32_t count;
  uint8_t value[4];   /* inline value (little-endian-agnostic; see below) */
  size_t ext_len;     /* >0 => value is out-of-line, serialized in `ext`  */
  const uint8_t *ext; /* points into the extras buffer                    */
} td_wentry;

typedef struct td_writer {
  int big_endian;
  td_wentry entries[TD_WMAX_ENTRIES];
  size_t entry_count;
  uint8_t *extras; /* serialized out-of-line values */
  size_t extras_len;
  size_t extras_cap;
  tinydng_context *ctx;
  int failed;
} td_writer;

static void td_put16(uint8_t *p, uint16_t v, int be) {
  if (be) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
  } else {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
  }
}

static void td_put32(uint8_t *p, uint32_t v, int be) {
  if (be) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
  } else {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
  }
}

/* Append serialized bytes to the extras buffer; returns pointer or NULL. */
static const uint8_t *td_extras_put(td_writer *w, const uint8_t *bytes,
                                    size_t len) {
  size_t padded = (len + 1u) & ~(size_t)1u; /* word-align */
  const uint8_t *ret;
  if (w->extras_len + padded > w->extras_cap) {
    w->failed = 1;
    return NULL;
  }
  ret = w->extras + w->extras_len;
  memcpy(w->extras + w->extras_len, bytes, len);
  if (padded > len) {
    w->extras[w->extras_len + len] = 0;
  }
  w->extras_len += padded;
  return ret;
}

/* Add a tag whose value bytes are already serialized in target endianness. */
static void td_add(td_writer *w, uint16_t tag, uint16_t type, uint32_t count,
                   const uint8_t *vbytes, size_t vlen) {
  td_wentry *e;
  if (w->failed || w->entry_count >= TD_WMAX_ENTRIES) {
    w->failed = 1;
    return;
  }
  e = &w->entries[w->entry_count++];
  e->tag = tag;
  e->type = type;
  e->count = count;
  e->ext_len = 0;
  e->ext = NULL;
  memset(e->value, 0, 4);
  if (vlen <= 4u) {
    memcpy(e->value, vbytes, vlen);
  } else {
    e->ext = td_extras_put(w, vbytes, vlen);
    e->ext_len = vlen;
  }
}

static void td_add_short(td_writer *w, uint16_t tag, uint16_t v) {
  uint8_t b[2];
  td_put16(b, v, w->big_endian);
  td_add(w, tag, TD_TYPE_SHORT, 1, b, 2);
}

static void td_add_long(td_writer *w, uint16_t tag, uint32_t v) {
  uint8_t b[4];
  td_put32(b, v, w->big_endian);
  td_add(w, tag, TD_TYPE_LONG, 1, b, 4);
}

static void td_add_shorts(td_writer *w, uint16_t tag, const uint16_t *v,
                          uint32_t n) {
  uint8_t tmp[64];
  uint32_t i;
  if (n * 2u > sizeof(tmp)) {
    w->failed = 1;
    return;
  }
  for (i = 0; i < n; i++) {
    td_put16(tmp + i * 2u, v[i], w->big_endian);
  }
  td_add(w, tag, TD_TYPE_SHORT, n, tmp, (size_t)n * 2u);
}

static void td_add_longs(td_writer *w, uint16_t tag, const uint32_t *v,
                         uint32_t n) {
  uint8_t tmp[128];
  uint32_t i;
  if (n * 4u > sizeof(tmp)) {
    w->failed = 1;
    return;
  }
  for (i = 0; i < n; i++) {
    td_put32(tmp + i * 4u, v[i], w->big_endian);
  }
  td_add(w, tag, TD_TYPE_LONG, n, tmp, (size_t)n * 4u);
}

static void td_add_bytes(td_writer *w, uint16_t tag, const uint8_t *v,
                         uint32_t n) {
  td_add(w, tag, TD_TYPE_BYTE, n, v, n);
}

/* Doubles -> SRATIONAL (num/den) with fixed denominator. */
static void td_add_srationals(td_writer *w, uint16_t tag, const double *v,
                              uint32_t n) {
  uint8_t tmp[128];
  uint32_t i;
  const int32_t den = 1000000;
  if (n * 8u > sizeof(tmp)) {
    w->failed = 1;
    return;
  }
  for (i = 0; i < n; i++) {
    int32_t num = (int32_t)(v[i] * (double)den);
    td_put32(tmp + i * 8u, (uint32_t)num, w->big_endian);
    td_put32(tmp + i * 8u + 4u, (uint32_t)den, w->big_endian);
  }
  td_add(w, tag, TD_TYPE_SRATIONAL, n, tmp, (size_t)n * 8u);
}

static void td_add_rationals(td_writer *w, uint16_t tag, const double *v,
                             uint32_t n) {
  uint8_t tmp[128];
  uint32_t i;
  const uint32_t den = 1000000u;
  if (n * 8u > sizeof(tmp)) {
    w->failed = 1;
    return;
  }
  for (i = 0; i < n; i++) {
    uint32_t num = (uint32_t)(v[i] * (double)den);
    td_put32(tmp + i * 8u, num, w->big_endian);
    td_put32(tmp + i * 8u + 4u, den, w->big_endian);
  }
  td_add(w, tag, TD_TYPE_RATIONAL, n, tmp, (size_t)n * 8u);
}

static int td_entry_cmp(const void *a, const void *b) {
  const td_wentry *ea = (const td_wentry *)a;
  const td_wentry *eb = (const td_wentry *)b;
  return (int)ea->tag - (int)eb->tag;
}

/* ------------------------------------------------------------------ */
/* Compression: endian helpers + TIFF LZW encoder                     */
/* ------------------------------------------------------------------ */

static int tdw_host_big(void) {
  uint16_t x = 1u;
  uint8_t b[2];
  memcpy(b, &x, 2);
  return b[0] == 0u;
}

static void tdw_swap_copy(uint8_t *dst, const uint8_t *src, size_t n_samples,
                          size_t sb) {
  size_t i;
  if (sb == 2u) {
    for (i = 0; i < n_samples; i++) {
      dst[i * 2u] = src[i * 2u + 1u];
      dst[i * 2u + 1u] = src[i * 2u];
    }
  } else { /* sb == 4 */
    for (i = 0; i < n_samples; i++) {
      dst[i * 4u] = src[i * 4u + 3u];
      dst[i * 4u + 1u] = src[i * 4u + 2u];
      dst[i * 4u + 2u] = src[i * 4u + 1u];
      dst[i * 4u + 3u] = src[i * 4u];
    }
  }
}

static int tdw_emit(uint8_t *out, size_t out_cap, size_t *op, uint32_t *bitbuf,
                    int *bitcnt, int width, int code) {
  *bitbuf = (*bitbuf << width) | (uint32_t)code;
  *bitcnt += width;
  while (*bitcnt >= 8) {
    if (*op >= out_cap) {
      return -1;
    }
    out[(*op)++] = (uint8_t)(*bitbuf >> (*bitcnt - 8));
    *bitcnt -= 8;
  }
  *bitbuf &= (*bitcnt > 0) ? ((1u << *bitcnt) - 1u) : 0u;
  return 0;
}

/* TIFF LZW encode (MSB-first, early change). `tbl` must hold 4096*256 int16.
   Returns encoded length or -1 on overflow. */
static long td_lzw_encode(const uint8_t *in, size_t in_len, uint8_t *out,
                          size_t out_cap, int16_t *tbl) {
  const int CLEAR = 256, EOI = 257;
  size_t op = 0;
  uint32_t bitbuf = 0;
  int bitcnt = 0;
  int width = 9, next = 258;
  size_t i;
  int prev;

  if (tdw_emit(out, out_cap, &op, &bitbuf, &bitcnt, width, CLEAR)) {
    return -1;
  }
  if (in_len == 0u) {
    if (tdw_emit(out, out_cap, &op, &bitbuf, &bitcnt, width, EOI)) {
      return -1;
    }
    if (bitcnt > 0) {
      if (op >= out_cap) {
        return -1;
      }
      out[op++] = (uint8_t)(bitbuf << (8 - bitcnt));
    }
    return (long)op;
  }

  memset(tbl, 0xFF, (size_t)4096u * 256u * sizeof(int16_t));
  prev = in[0];
  for (i = 1; i < in_len; i++) {
    int c = in[i];
    int16_t e = tbl[(size_t)prev * 256u + (size_t)c];
    if (e != -1) {
      prev = e;
      continue;
    }
    if (tdw_emit(out, out_cap, &op, &bitbuf, &bitcnt, width, prev)) {
      return -1;
    }
    tbl[(size_t)prev * 256u + (size_t)c] = (int16_t)next;
    next++;
    /* Mirror libtiff exactly: reset at free_ent==CODE_MAX-1 (4094), else widen
       when free_ent > maxcode, i.e. next == (1<<width). The encoder widens one
       entry later than the decoder (TIFF early change). */
    if (next == 4094) {
      if (tdw_emit(out, out_cap, &op, &bitbuf, &bitcnt, width, CLEAR)) {
        return -1;
      }
      memset(tbl, 0xFF, (size_t)4096u * 256u * sizeof(int16_t));
      width = 9;
      next = 258;
    } else if (width < 12 && next == (1 << width)) {
      width++;
    }
    prev = c;
  }
  if (tdw_emit(out, out_cap, &op, &bitbuf, &bitcnt, width, prev)) {
    return -1;
  }
  if (tdw_emit(out, out_cap, &op, &bitbuf, &bitcnt, width, EOI)) {
    return -1;
  }
  if (bitcnt > 0) {
    if (op >= out_cap) {
      return -1;
    }
    out[op++] = (uint8_t)(bitbuf << (8 - bitcnt));
  }
  return (long)op;
}

/* Produce the on-disk strip bytes for `img` honoring compression + endianness.
   On success *out_strip is a ctx-owned buffer (free with td_ctx_free). */
static tinydng_status td_prepare_strip(tinydng_context *ctx,
                                       const tinydng_write_image *img, int be,
                                       uint16_t spp, uint16_t bps, uint16_t comp,
                                       size_t data_size, uint8_t **out_strip,
                                       size_t *out_len, uint16_t *out_comp,
                                       tinydng_error *err) {
  size_t sb = (size_t)bps / 8u;
  int need_swap = (sb > 1u) && (be != tdw_host_big());
  size_t n_samples = (sb > 0u) ? (data_size / sb) : data_size;

  *out_strip = NULL;
  *out_len = 0;
  *out_comp = comp ? comp : (uint16_t)TINYDNG_COMPRESSION_NONE;

  if (comp == 0u || comp == TINYDNG_COMPRESSION_NONE) {
    uint8_t *s = (uint8_t *)td_ctx_alloc(ctx, data_size, err);
    if (!s) {
      return TINYDNG_E_OOM;
    }
    if (need_swap) {
      tdw_swap_copy(s, img->data, n_samples, sb);
    } else {
      memcpy(s, img->data, data_size);
    }
    *out_strip = s;
    *out_len = data_size;
    *out_comp = TINYDNG_COMPRESSION_NONE;
    return TINYDNG_OK;
  }

  if (comp == TINYDNG_COMPRESSION_LZW) {
    const uint8_t *src;
    uint8_t *tmp = NULL;
    uint8_t *enc;
    int16_t *tbl;
    size_t cap;
    long n;
    if (need_swap) {
      tmp = (uint8_t *)td_ctx_alloc(ctx, data_size, err);
      if (!tmp) {
        return TINYDNG_E_OOM;
      }
      tdw_swap_copy(tmp, img->data, n_samples, sb);
      src = tmp;
    } else {
      src = img->data;
    }
    cap = data_size + data_size / 2u + 1024u;
    enc = (uint8_t *)td_ctx_alloc(ctx, cap, err);
    tbl = (int16_t *)td_ctx_alloc(ctx, (size_t)4096u * 256u * sizeof(int16_t),
                                  err);
    if (!enc || !tbl) {
      td_ctx_free(ctx, tmp);
      td_ctx_free(ctx, enc);
      td_ctx_free(ctx, tbl);
      return TINYDNG_E_OOM;
    }
    n = td_lzw_encode(src, data_size, enc, cap, tbl);
    td_ctx_free(ctx, tbl);
    td_ctx_free(ctx, tmp);
    if (n < 0) {
      td_ctx_free(ctx, enc);
      td_set_error(err, TINYDNG_E_INTERNAL, TINYDNG_STAGE_WRITE, 0, 0, 0,
                   "LZW encode overflow");
      return TINYDNG_E_INTERNAL;
    }
    *out_strip = enc;
    *out_len = (size_t)n;
    *out_comp = TINYDNG_COMPRESSION_LZW;
    return TINYDNG_OK;
  }

  if (comp == TINYDNG_COMPRESSION_NEW_JPEG ||
      comp == TINYDNG_COMPRESSION_OLD_JPEG) {
    uint16_t *img16;
    uint8_t *encoded = NULL;
    int enclen = 0;
    int rc;
    uint8_t *out;
    if (bps != 16u) {
      td_set_error(err, TINYDNG_E_UNSUPPORTED, TINYDNG_STAGE_WRITE, 0, 0, 0,
                   "lossless JPEG writer requires 16-bit samples");
      return TINYDNG_E_UNSUPPORTED;
    }
    img16 = (uint16_t *)(void *)(uintptr_t)img->data; /* host-order uint16 */
    rc = tdng_lj92_encode_ex(img16, (int)img->width, (int)img->height, 16,
                             (int)spp, 1 /*predictor=left*/,
                             (int)((size_t)img->width * spp), 0, NULL, 0,
                             &encoded, &enclen);
    if (rc != 0 || !encoded || enclen <= 0) {
      if (encoded) {
        free(encoded);
      }
      td_set_error(err, TINYDNG_E_INTERNAL, TINYDNG_STAGE_WRITE, 0, 0, 0,
                   "tdng_lj92_encode_ex failed rc=%d", rc);
      return TINYDNG_E_INTERNAL;
    }
    out = (uint8_t *)td_ctx_alloc(ctx, (size_t)enclen, err);
    if (!out) {
      free(encoded);
      return TINYDNG_E_OOM;
    }
    memcpy(out, encoded, (size_t)enclen);
    free(encoded);
    *out_strip = out;
    *out_len = (size_t)enclen;
    *out_comp = TINYDNG_COMPRESSION_NEW_JPEG;
    return TINYDNG_OK;
  }

  td_set_error(err, TINYDNG_E_UNSUPPORTED, TINYDNG_STAGE_WRITE, 0, 0, 0,
               "unsupported writer compression %u", (unsigned)comp);
  return TINYDNG_E_UNSUPPORTED;
}

tinydng_status tinydng_write_memory(tinydng_context *ctx,
                                    const tinydng_write_image *img,
                                    const tinydng_write_options *opts,
                                    uint8_t **out_data, size_t *out_size,
                                    tinydng_error *err) {
  td_writer w;
  int be = opts ? (opts->big_endian != 0) : 0;
  uint16_t spp, bps, sfmt, photo;
  size_t data_size, strip_pad, strip_off, extras_base, ifd_off, total;
  size_t expected;
  size_t i, idx;
  uint8_t *buf;
  size_t ext_running;
  uint8_t *strip_data = NULL;
  size_t strip_len = 0;
  uint16_t comp_value = TINYDNG_COMPRESSION_NONE;

  tinydng_error_clear(err);
  if (!ctx || !img || !out_data || !out_size || !img->data) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "null argument to tinydng_write_memory");
    return TINYDNG_E_INVALID_ARG;
  }
  *out_data = NULL;
  *out_size = 0;

  spp = img->samples_per_pixel ? img->samples_per_pixel : 1u;
  bps = img->bits_per_sample ? img->bits_per_sample : 8u;
  sfmt = img->sample_format ? img->sample_format : TINYDNG_SAMPLEFORMAT_UINT;
  if (bps != 8u && bps != 16u && bps != 32u) {
    td_set_error(err, TINYDNG_E_UNSUPPORTED, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "writer supports 8/16/32-bit only (got %u)", (unsigned)bps);
    return TINYDNG_E_UNSUPPORTED;
  }
  if (img->width == 0u || img->height == 0u || spp == 0u) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "invalid image dimensions");
    return TINYDNG_E_INVALID_ARG;
  }
  if (!td_safe_mul_size((size_t)img->width * img->height, (size_t)spp,
                        &expected) ||
      !td_safe_mul_size(expected, (size_t)bps / 8u, &expected)) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "image size overflow");
    return TINYDNG_E_BOUNDS;
  }
  if (img->data_size < expected) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "data_size %zu < required %zu", img->data_size, expected);
    return TINYDNG_E_INVALID_ARG;
  }
  data_size = expected;

  photo = img->photometric;
  if (photo == 0u) {
    if (opts && opts->as_dng && img->cfa && img->cfa->present) {
      photo = TD_PHOTO_CFA;
    } else if (spp >= 3u) {
      photo = TD_PHOTO_RGB;
    } else {
      photo = TD_PHOTO_MINISBLACK;
    }
  }

  memset(&w, 0, sizeof(w));
  w.ctx = ctx;
  w.big_endian = be;
  w.extras_cap = TD_WMAX_EXTRAS;
  w.extras = (uint8_t *)td_ctx_alloc(ctx, w.extras_cap, err);
  if (!w.extras) {
    return TINYDNG_E_OOM;
  }

  {
    uint16_t comp = opts ? opts->compression : 0u;
    tinydng_status pst = td_prepare_strip(ctx, img, be, spp, bps, comp,
                                          data_size, &strip_data, &strip_len,
                                          &comp_value, err);
    if (pst != TINYDNG_OK) {
      td_ctx_free(ctx, w.extras);
      return pst;
    }
  }

  strip_off = 8u;
  strip_pad = strip_len & 1u; /* pad strip to even */

  /* --- core TIFF tags (ascending order enforced by sort below) --- */
  td_add_long(&w, TD_TAG_IMAGE_WIDTH, img->width);
  td_add_long(&w, TD_TAG_IMAGE_LENGTH, img->height);
  {
    uint16_t bpsv[16];
    uint16_t k;
    for (k = 0; k < spp && k < 16u; k++) {
      bpsv[k] = bps;
    }
    if (spp == 1u) {
      td_add_short(&w, TD_TAG_BITS_PER_SAMPLE, bps);
    } else {
      td_add_shorts(&w, TD_TAG_BITS_PER_SAMPLE, bpsv, spp);
    }
  }
  td_add_short(&w, TD_TAG_COMPRESSION, comp_value);
  td_add_short(&w, TD_TAG_PHOTOMETRIC, photo);
  td_add_long(&w, TD_TAG_STRIP_OFFSETS, (uint32_t)strip_off);
  td_add_short(&w, TD_TAG_SAMPLES_PER_PIXEL, spp);
  td_add_long(&w, TD_TAG_ROWS_PER_STRIP, img->height);
  td_add_long(&w, TD_TAG_STRIP_BYTE_COUNTS, (uint32_t)strip_len);
  {
    uint16_t sfv[16];
    uint16_t k;
    for (k = 0; k < spp && k < 16u; k++) {
      sfv[k] = sfmt;
    }
    if (spp == 1u) {
      td_add_short(&w, TD_TAG_SAMPLE_FORMAT, sfmt);
    } else {
      td_add_shorts(&w, TD_TAG_SAMPLE_FORMAT, sfv, spp);
    }
  }

  /* --- optional DNG metadata --- */
  if (opts && opts->as_dng) {
    const tinydng_raw_info *raw = img->raw;
    const tinydng_cfa *cfa = img->cfa;
    td_add_short(&w, TD_TAG_NEW_SUBFILE_TYPE, 0);
    if (cfa && cfa->present) {
      uint16_t dim[2];
      dim[0] = cfa->pattern_dim[0] ? cfa->pattern_dim[0] : 2u;
      dim[1] = cfa->pattern_dim[1] ? cfa->pattern_dim[1] : 2u;
      td_add_shorts(&w, TD_TAG_CFA_REPEAT_PATTERN_DIM, dim, 2);
      if (cfa->pattern_size) {
        td_add_bytes(&w, TD_TAG_CFA_PATTERN, cfa->pattern, cfa->pattern_size);
      }
      if (cfa->plane_color_count) {
        td_add_bytes(&w, TD_TAG_CFA_PLANE_COLOR, cfa->plane_color,
                     cfa->plane_color_count);
      }
    }
    if (raw) {
      if (raw->has_dng_version) {
        td_add_bytes(&w, TD_TAG_DNG_VERSION, raw->dng_version, 4);
      } else {
        uint8_t ver[4] = {1u, 4u, 0u, 0u};
        td_add_bytes(&w, TD_TAG_DNG_VERSION, ver, 4);
      }
      if (raw->black_level_present) {
        uint32_t bl[4];
        uint32_t k, n = (cfa && cfa->pattern_size) ? 1u : spp;
        if (n > 4u) {
          n = 4u;
        }
        for (k = 0; k < n; k++) {
          bl[k] = (uint32_t)(raw->black_level[k] < 0 ? 0 : raw->black_level[k]);
        }
        td_add_longs(&w, TD_TAG_BLACK_LEVEL, bl, n);
      }
      if (raw->white_level_present) {
        uint32_t wl = (uint32_t)(raw->white_level[0] < 0 ? 0
                                                         : raw->white_level[0]);
        td_add_long(&w, TD_TAG_WHITE_LEVEL, wl);
      }
      if (raw->color_matrix_present) {
        td_add_srationals(&w, TD_TAG_COLOR_MATRIX1, raw->color_matrix1, 9);
      }
      if (raw->has_as_shot_neutral) {
        td_add_rationals(&w, TD_TAG_AS_SHOT_NEUTRAL, raw->as_shot_neutral, 3);
      }
      if (raw->calibration_illuminant1) {
        td_add_short(&w, TD_TAG_CALIBRATION_ILLUMINANT1,
                     raw->calibration_illuminant1);
      }
    }
  }

  if (w.failed) {
    td_ctx_free(ctx, w.extras);
    td_ctx_free(ctx, strip_data);
    td_set_error(err, TINYDNG_E_INTERNAL, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "writer ran out of tag/extras capacity");
    return TINYDNG_E_INTERNAL;
  }

  /* Sort entries by tag (TIFF requires ascending). */
  qsort(w.entries, w.entry_count, sizeof(td_wentry), td_entry_cmp);

  extras_base = strip_off + strip_len + strip_pad;
  ifd_off = extras_base + w.extras_len;
  total = ifd_off + 2u + w.entry_count * 12u + 4u;

  buf = (uint8_t *)td_ctx_alloc(ctx, total, err);
  if (!buf) {
    td_ctx_free(ctx, w.extras);
    td_ctx_free(ctx, strip_data);
    return TINYDNG_E_OOM;
  }
  memset(buf, 0, total);

  /* Header */
  if (be) {
    buf[0] = 'M';
    buf[1] = 'M';
  } else {
    buf[0] = 'I';
    buf[1] = 'I';
  }
  td_put16(buf + 2, 42u, be);
  td_put32(buf + 4, (uint32_t)ifd_off, be);

  /* Strip data (already compressed / endian-adjusted) */
  memcpy(buf + strip_off, strip_data, strip_len);

  /* Extras (out-of-line tag values), re-based to absolute offsets. */
  if (w.extras_len) {
    memcpy(buf + extras_base, w.extras, w.extras_len);
  }

  /* IFD */
  td_put16(buf + ifd_off, (uint16_t)w.entry_count, be);
  ext_running = 0;
  for (i = 0; i < w.entry_count; i++) {
    td_wentry *e = &w.entries[i];
    uint8_t *ep = buf + ifd_off + 2u + i * 12u;
    td_put16(ep, e->tag, be);
    td_put16(ep + 2, e->type, be);
    td_put32(ep + 4, e->count, be);
    if (e->ext_len > 0u) {
      size_t padded = (e->ext_len + 1u) & ~(size_t)1u;
      td_put32(ep + 8, (uint32_t)(extras_base + ext_running), be);
      ext_running += padded;
    } else {
      memcpy(ep + 8, e->value, 4);
    }
  }
  idx = ifd_off + 2u + w.entry_count * 12u;
  td_put32(buf + idx, 0u, be); /* next IFD = 0 */

  td_ctx_free(ctx, w.extras);
  td_ctx_free(ctx, strip_data);
  *out_data = buf;
  *out_size = total;
  return TINYDNG_OK;
}

void tinydng_buffer_free(tinydng_context *ctx, uint8_t *buf) {
  td_ctx_free(ctx, buf);
}

tinydng_status tinydng_write_file(tinydng_context *ctx, const char *path,
                                  const tinydng_write_image *img,
                                  const tinydng_write_options *opts,
                                  tinydng_error *err) {
  uint8_t *buf = NULL;
  size_t size = 0;
  tinydng_status st;
  FILE *fp;

  if (!ctx || !path || !img) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "null argument to tinydng_write_file");
    return TINYDNG_E_INVALID_ARG;
  }
  st = tinydng_write_memory(ctx, img, opts, &buf, &size, err);
  if (st != TINYDNG_OK) {
    return st;
  }
  fp = fopen(path, "wb");
  if (!fp) {
    td_ctx_free(ctx, buf);
    td_set_error(err, TINYDNG_E_IO, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "fopen('%s') for write failed", path);
    return TINYDNG_E_IO;
  }
  if (fwrite(buf, 1, size, fp) != size) {
    fclose(fp);
    td_ctx_free(ctx, buf);
    td_set_error(err, TINYDNG_E_IO, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "short write to '%s'", path);
    return TINYDNG_E_IO;
  }
  fclose(fp);
  td_ctx_free(ctx, buf);
  return TINYDNG_OK;
}
