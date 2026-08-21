/*
 * tinydng_psd_write.c - PSD/PSB writer (composite + layers, RAW/RLE).
 *
 * Emission strategy: channel payloads are compressed into scratch buffers
 * first so every length field is known when its record is emitted; only
 * whole-section lengths are back-patched. All output is big-endian.
 *
 * SPDX-License-Identifier: MIT
 */
#include "td_internal.h"

#include <stdio.h>

#ifndef TINYDNG_NO_PSD

#define TD_PSDW_SIG_8BPS TINYDNG_PSD_FOURCC('8', 'B', 'P', 'S')
#define TD_PSDW_SIG_8BIM TINYDNG_PSD_FOURCC('8', 'B', 'I', 'M')

/* ------------------------------------------------------------------ */
/* Growable big-endian output buffer                                  */
/* ------------------------------------------------------------------ */

typedef struct td_psdw {
  tinydng_context *ctx;
  uint8_t *p;
  size_t len, cap;
  int failed;
  tinydng_error *err;
} td_psdw;

static int td_psdw_reserve(td_psdw *w, size_t extra) {
  size_t need;
  if (w->failed) {
    return 0;
  }
  if (!td_safe_add_size(w->len, extra, &need)) {
    w->failed = 1;
    return 0;
  }
  if (need > w->cap) {
    size_t new_cap = w->cap ? w->cap : 1024u;
    uint8_t *grown;
    while (new_cap < need) {
      if (!td_safe_mul_size(new_cap, 2u, &new_cap)) {
        w->failed = 1;
        return 0;
      }
    }
    grown = (uint8_t *)td_ctx_realloc(w->ctx, w->p, w->cap, new_cap, w->err);
    if (!grown) {
      w->failed = 1;
      return 0;
    }
    w->p = grown;
    w->cap = new_cap;
  }
  return 1;
}

static void td_psdw_bytes(td_psdw *w, const void *src, size_t n) {
  if (!td_psdw_reserve(w, n)) {
    return;
  }
  if (n > 0u) {
    memcpy(w->p + w->len, src, n);
    w->len += n;
  }
}

static void td_psdw_zeros(td_psdw *w, size_t n) {
  if (!td_psdw_reserve(w, n)) {
    return;
  }
  memset(w->p + w->len, 0, n);
  w->len += n;
}

static void td_psdw_u8(td_psdw *w, uint8_t v) { td_psdw_bytes(w, &v, 1u); }

static void td_psdw_u16(td_psdw *w, uint16_t v) {
  uint8_t b[2];
  b[0] = (uint8_t)(v >> 8);
  b[1] = (uint8_t)(v & 0xFFu);
  td_psdw_bytes(w, b, 2u);
}

static void td_psdw_u32(td_psdw *w, uint32_t v) {
  uint8_t b[4];
  b[0] = (uint8_t)(v >> 24);
  b[1] = (uint8_t)(v >> 16);
  b[2] = (uint8_t)(v >> 8);
  b[3] = (uint8_t)(v & 0xFFu);
  td_psdw_bytes(w, b, 4u);
}

static void td_psdw_u64(td_psdw *w, uint64_t v) {
  td_psdw_u32(w, (uint32_t)(v >> 32));
  td_psdw_u32(w, (uint32_t)(v & 0xFFFFFFFFu));
}

static void td_psdw_i32(td_psdw *w, int32_t v) {
  td_psdw_u32(w, (uint32_t)v);
}

/* Length field sized by document flavor. */
static void td_psdw_len(td_psdw *w, int is_psb, uint64_t v) {
  if (is_psb) {
    td_psdw_u64(w, v);
  } else {
    td_psdw_u32(w, (uint32_t)v);
  }
}

static size_t td_psdw_mark(td_psdw *w, int is_psb) {
  size_t at = w->len;
  td_psdw_len(w, is_psb, 0u);
  return at;
}

static void td_psdw_patch_len(td_psdw *w, int is_psb, size_t at,
                              uint64_t v) {
  if (w->failed) {
    return;
  }
  if (is_psb) {
    w->p[at + 0u] = (uint8_t)(v >> 56);
    w->p[at + 1u] = (uint8_t)(v >> 48);
    w->p[at + 2u] = (uint8_t)(v >> 40);
    w->p[at + 3u] = (uint8_t)(v >> 32);
    w->p[at + 4u] = (uint8_t)(v >> 24);
    w->p[at + 5u] = (uint8_t)(v >> 16);
    w->p[at + 6u] = (uint8_t)(v >> 8);
    w->p[at + 7u] = (uint8_t)(v & 0xFFu);
  } else {
    w->p[at + 0u] = (uint8_t)(v >> 24);
    w->p[at + 1u] = (uint8_t)(v >> 16);
    w->p[at + 2u] = (uint8_t)(v >> 8);
    w->p[at + 3u] = (uint8_t)(v & 0xFFu);
  }
}

static int td_psdw_patch_u32_len(td_psdw *w, size_t at, uint64_t v,
                                 tinydng_error *err) {
  if (v > UINT32_MAX) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_WRITE, 0, 0, at,
                 "PSD writer: 32-bit section length overflow");
    return 0;
  }
  td_psdw_patch_len(w, 0, at, v);
  return 1;
}

/* Pascal string: 1 length byte + bytes, field padded to `pad`. */
static void td_psdw_pascal(td_psdw *w, const char *s, uint32_t pad) {
  size_t n = s ? strlen(s) : 0u;
  size_t field;
  if (n > 255u) {
    n = 255u;
  }
  td_psdw_u8(w, (uint8_t)n);
  td_psdw_bytes(w, s, n);
  field = 1u + n;
  while (field % pad != 0u) {
    td_psdw_u8(w, 0u);
    field++;
  }
}

/* UTF-8 -> UnicodeString (u32 code-unit count + UTF-16BE). */
static void td_psdw_unicode(td_psdw *w, const char *s) {
  size_t i = 0, n = s ? strlen(s) : 0u;
  size_t count_at = w->len;
  uint32_t units = 0;
  td_psdw_u32(w, 0u); /* patched below */
  while (i < n) {
    uint32_t cp = (uint8_t)s[i];
    size_t extra = 0;
    if (cp >= 0xF0u) {
      extra = 3;
      cp &= 0x07u;
    } else if (cp >= 0xE0u) {
      extra = 2;
      cp &= 0x0Fu;
    } else if (cp >= 0xC0u) {
      extra = 1;
      cp &= 0x1Fu;
    }
    if (i + extra >= n + 1u && extra > 0u && i + extra > n - 1u + 1u) {
      break;
    }
    if (i + extra >= n && extra > 0u) {
      break; /* truncated sequence */
    }
    i++;
    while (extra-- > 0u) {
      cp = (cp << 6) | ((uint8_t)s[i++] & 0x3Fu);
    }
    if (cp >= 0x10000u && cp <= 0x10FFFFu) {
      uint32_t v = cp - 0x10000u;
      td_psdw_u16(w, (uint16_t)(0xD800u + (v >> 10)));
      td_psdw_u16(w, (uint16_t)(0xDC00u + (v & 0x3FFu)));
      units += 2u;
    } else {
      if (cp > 0xFFFFu) {
        cp = 0xFFFDu;
      }
      td_psdw_u16(w, (uint16_t)cp);
      units += 1u;
    }
  }
  if (!w->failed) {
    w->p[count_at + 0u] = (uint8_t)(units >> 24);
    w->p[count_at + 1u] = (uint8_t)(units >> 16);
    w->p[count_at + 2u] = (uint8_t)(units >> 8);
    w->p[count_at + 3u] = (uint8_t)(units & 0xFFu);
  }
}

/* ------------------------------------------------------------------ */
/* PackBits encoder                                                   */
/* ------------------------------------------------------------------ */

/* Greedy PackBits: runs of >= 3 identical bytes become replicates, the rest
   accumulates into literals of up to 128 bytes. Never emits -128. Returns
   encoded size or -1 if `cap` is too small. Worst case n + (n+127)/128. */
static long td_packbits_encode(const uint8_t *in, size_t n, uint8_t *out,
                               size_t cap) {
  size_t ip = 0;
  size_t op = 0;
  while (ip < n) {
    size_t run = 1;
    while (ip + run < n && run < 128u && in[ip + run] == in[ip]) {
      run++;
    }
    if (run >= 3u) {
      if (op + 2u > cap) {
        return -1;
      }
      out[op++] = (uint8_t)(int8_t)(1 - (int)run);
      out[op++] = in[ip];
      ip += run;
    } else {
      size_t lit_start = ip;
      size_t lit = 0;
      while (ip < n && lit < 128u) {
        size_t r = 1;
        while (ip + r < n && r < 128u && in[ip + r] == in[ip]) {
          r++;
        }
        if (r >= 3u) {
          break;
        }
        ip += r;
        lit += r;
        if (lit > 128u) {
          ip -= lit - 128u;
          lit = 128u;
        }
      }
      if (op + 1u + lit > cap) {
        return -1;
      }
      out[op++] = (uint8_t)(lit - 1u);
      memcpy(out + op, in + lit_start, lit);
      op += lit;
    }
  }
  return (long)op;
}

/* ------------------------------------------------------------------ */
/* Channel payload compression                                        */
/* ------------------------------------------------------------------ */

/* One compressed channel payload (compression tag NOT included). */
typedef struct td_psdw_payload {
  uint8_t *data;   /* ctx-owned */
  size_t size;
  uint16_t compression; /* actually used (RLE may fall back to RAW) */
} td_psdw_payload;

/* Serialize one host-order plane (w*h samples of depth bits) to stored
   big-endian bytes. */
static uint8_t *td_psdw_plane_be(tinydng_context *ctx, const uint8_t *host,
                                 uint32_t w, uint32_t h, uint16_t depth,
                                 size_t *out_size, tinydng_error *err) {
  size_t n_samples;
  size_t bytes = (size_t)depth / 8u;
  size_t total;
  uint8_t *out;
  size_t i;
  if (!td_safe_mul_size((size_t)w, (size_t)h, &n_samples) ||
      !td_safe_mul_size(n_samples, bytes, &total)) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "PSD writer: plane size overflow");
    return NULL;
  }
  out = (uint8_t *)td_ctx_alloc(ctx, total, err);
  if (!out) {
    return NULL;
  }
  if (bytes == 1u) {
    memcpy(out, host, total);
  } else if (bytes == 2u) {
    for (i = 0; i < n_samples; i++) {
      uint16_t v;
      memcpy(&v, host + i * 2u, 2u);
      out[2u * i] = (uint8_t)(v >> 8);
      out[2u * i + 1u] = (uint8_t)(v & 0xFFu);
    }
  } else {
    for (i = 0; i < n_samples; i++) {
      uint32_t v;
      memcpy(&v, host + i * 4u, 4u);
      out[4u * i] = (uint8_t)(v >> 24);
      out[4u * i + 1u] = (uint8_t)(v >> 16);
      out[4u * i + 2u] = (uint8_t)(v >> 8);
      out[4u * i + 3u] = (uint8_t)(v & 0xFFu);
    }
  }
  *out_size = total;
  return out;
}

/* Compress a stored-BE plane into a payload per the requested compression.
   RLE payload = row-length table (u16/u32 per row) + packed rows; falls back
   to RAW when a row does not fit the table entry width. */
static tinydng_status td_psdw_compress_plane(tinydng_context *ctx,
                                             const uint8_t *stored,
                                             size_t stored_size, uint32_t w,
                                             uint32_t h, uint16_t depth,
                                             uint16_t comp, int is_psb,
                                             td_psdw_payload *out,
                                             tinydng_error *err) {
  size_t bits, row_bytes;
  if (!td_safe_mul_size((size_t)w, (size_t)depth, &bits)) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "PSD writer: row size overflow");
    return TINYDNG_E_BOUNDS;
  }
  row_bytes = bits / 8u + ((bits % 8u) != 0u);
  memset(out, 0, sizeof(*out));

  if (comp == TINYDNG_PSD_COMP_RLE && h > 0u && row_bytes > 0u) {
    size_t esz = is_psb ? 4u : 2u;
    size_t table_bytes, worst_row, overhead;
    size_t cap, y, op;
    uint8_t *buf;
    uint64_t limit = is_psb ? 0xFFFFFFFFu : 0xFFFFu;
    int fallback = 0;
    if (!td_safe_mul_size((size_t)h, esz, &table_bytes) ||
        !td_safe_add_size(row_bytes, 127u, &overhead) ||
        !td_safe_add_size(row_bytes, overhead / 128u, &worst_row) ||
        !td_safe_mul_size(worst_row, (size_t)h, &cap) ||
        !td_safe_add_size(cap, table_bytes, &cap)) {
      return TINYDNG_E_BOUNDS;
    }
    buf = (uint8_t *)td_ctx_alloc(ctx, cap, err);
    if (!buf) {
      return TINYDNG_E_OOM;
    }
    op = table_bytes;
    for (y = 0; y < h; y++) {
      long got = td_packbits_encode(stored + y * row_bytes, row_bytes,
                                    buf + op, cap - op);
      if (got < 0 || (uint64_t)got > limit) {
        fallback = 1;
        break;
      }
      if (esz == 2u) {
        buf[2u * y] = (uint8_t)((uint64_t)got >> 8);
        buf[2u * y + 1u] = (uint8_t)((uint64_t)got & 0xFFu);
      } else {
        buf[4u * y] = (uint8_t)((uint64_t)got >> 24);
        buf[4u * y + 1u] = (uint8_t)((uint64_t)got >> 16);
        buf[4u * y + 2u] = (uint8_t)((uint64_t)got >> 8);
        buf[4u * y + 3u] = (uint8_t)((uint64_t)got & 0xFFu);
      }
      op += (size_t)got;
    }
    if (!fallback) {
      out->data = buf;
      out->size = op;
      out->compression = TINYDNG_PSD_COMP_RLE;
      return TINYDNG_OK;
    }
    td_ctx_free(ctx, buf);
    /* fall through to RAW */
  } else if (comp != TINYDNG_PSD_COMP_RAW &&
             comp != TINYDNG_PSD_COMP_RLE) {
    td_set_error(err, TINYDNG_E_UNSUPPORTED, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "PSD writer: compression %u not supported (use RAW or RLE)",
                 (unsigned)comp);
    return TINYDNG_E_UNSUPPORTED;
  }

  out->data = (uint8_t *)td_ctx_alloc(ctx, stored_size ? stored_size : 1u,
                                      err);
  if (!out->data) {
    return TINYDNG_E_OOM;
  }
  memcpy(out->data, stored, stored_size);
  out->size = stored_size;
  out->compression = TINYDNG_PSD_COMP_RAW;
  return TINYDNG_OK;
}

/* ------------------------------------------------------------------ */
/* Validation                                                         */
/* ------------------------------------------------------------------ */

static tinydng_status td_psdw_validate(tinydng_context *ctx,
                                       const tinydng_psd_write_doc *doc,
                                       const tinydng_psd_write_options *opts,
                                       tinydng_error *err) {
  uint32_t dim_cap = (opts && opts->as_psb) ? 300000u : 30000u;
  uint64_t pixels;
  size_t sample_bytes;
  size_t i;

  if (doc->width < 1u || doc->width > dim_cap || doc->height < 1u ||
      doc->height > dim_cap) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "PSD writer: dimensions %ux%u out of range", doc->width,
                 doc->height);
    return TINYDNG_E_INVALID_ARG;
  }
  if (doc->depth != 8u && doc->depth != 16u && doc->depth != 32u) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "PSD writer: depth %u not in {8,16,32}",
                 (unsigned)doc->depth);
    return TINYDNG_E_INVALID_ARG;
  }
  if (doc->channel_count < 1u || doc->channel_count > 56u) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "PSD writer: channel count %u out of range",
                 (unsigned)doc->channel_count);
    return TINYDNG_E_INVALID_ARG;
  }
  if (!td_safe_mul_u64(doc->width, doc->height, &pixels) ||
      pixels > ctx->max_image_pixels) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "PSD writer: image exceeds max_image_pixels");
    return TINYDNG_E_BOUNDS;
  }
  sample_bytes = (size_t)doc->depth / 8u;
  if (doc->composite) {
    size_t need;
    if (!td_safe_mul_size((size_t)pixels, sample_bytes, &need) ||
        !td_safe_mul_size(need, (size_t)doc->channel_count, &need) ||
        doc->composite_size != need) {
      td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_WRITE, 0, 0, 0,
                   "PSD writer: composite_size mismatch");
      return TINYDNG_E_INVALID_ARG;
    }
  }
  if (doc->color_mode == TINYDNG_PSD_INDEXED && !doc->palette) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "PSD writer: indexed mode requires a 768-byte palette");
    return TINYDNG_E_INVALID_ARG;
  }
  if (doc->icc == NULL && doc->icc_size > 0u) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "PSD writer: icc_size > 0 with NULL icc pointer");
    return TINYDNG_E_INVALID_ARG;
  }
  if (doc->layer_count > 0u && doc->layers == NULL) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "PSD writer: layer_count > 0 with NULL layers");
    return TINYDNG_E_INVALID_ARG;
  }
  if (doc->layer_count > ctx->max_psd_layers ||
      doc->layer_count > 32767u) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "PSD writer: too many layers");
    return TINYDNG_E_INVALID_ARG;
  }
  for (i = 0; i < doc->layer_count; i++) {
    const tinydng_psd_write_layer *L = &doc->layers[i];
    int64_t lw = (int64_t)L->right - L->left;
    int64_t lh = (int64_t)L->bottom - L->top;
    size_t need;
    uint16_t c;
    if (lw < 0 || lh < 0 || (uint64_t)lw * (uint64_t)lh >
                                ctx->max_image_pixels) {
      td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_WRITE, 0, 0, 0,
                   "PSD writer: layer %zu rect invalid", i);
      return TINYDNG_E_INVALID_ARG;
    }
    if (L->channel_count > 56u) {
      td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_WRITE, 0, 0, 0,
                   "PSD writer: layer %zu channel count out of range", i);
      return TINYDNG_E_INVALID_ARG;
    }
    if (L->channel_count > 0u && L->channels == NULL) {
      td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_WRITE, 0, 0, 0,
                   "PSD writer: layer %zu channel_count > 0 with NULL channels",
                   i);
      return TINYDNG_E_INVALID_ARG;
    }
    {
      uint64_t layer_pixels;
      if (!td_safe_mul_u64((uint64_t)lw, (uint64_t)lh, &layer_pixels) ||
          layer_pixels > (uint64_t)SIZE_MAX ||
          !td_safe_mul_size((size_t)layer_pixels, sample_bytes, &need)) {
        td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_WRITE, 0, 0, 0,
                     "PSD writer: layer channel size overflow");
        return TINYDNG_E_BOUNDS;
      }
    }
    for (c = 0; c < L->channel_count; c++) {
      const tinydng_psd_write_channel *ch = &L->channels[c];
      if (ch->id < -1) {
        td_set_error(err, TINYDNG_E_UNSUPPORTED, TINYDNG_STAGE_WRITE, 0, 0,
                     0, "PSD writer: mask channels not supported");
        return TINYDNG_E_UNSUPPORTED;
      }
      if ((need > 0u && !ch->data) || ch->size != need) {
        td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_WRITE, 0, 0,
                     0, "PSD writer: layer %zu channel %u size mismatch", i,
                     (unsigned)c);
        return TINYDNG_E_INVALID_ARG;
      }
    }
  }
  return TINYDNG_OK;
}

/* ------------------------------------------------------------------ */
/* Emission                                                           */
/* ------------------------------------------------------------------ */

static void td_psdw_resources(td_psdw *w, const tinydng_psd_write_doc *doc) {
  size_t section_at = w->len;
  td_psdw_u32(w, 0u); /* patched */

  /* 1005: resolution info (72 dpi, inches). */
  td_psdw_u32(w, TD_PSDW_SIG_8BIM);
  td_psdw_u16(w, 1005u);
  td_psdw_u16(w, 0u); /* empty Pascal name (len 0 + pad) */
  td_psdw_u32(w, 16u);
  td_psdw_u32(w, 72u << 16); /* hRes 72.0 fixed */
  td_psdw_u16(w, 1u);        /* display unit: ppi */
  td_psdw_u16(w, 1u);        /* width unit: inch */
  td_psdw_u32(w, 72u << 16); /* vRes */
  td_psdw_u16(w, 1u);
  td_psdw_u16(w, 1u);

  if (doc->icc && doc->icc_size > 0u) {
    if (doc->icc_size > UINT32_MAX) {
      td_set_error(w->err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_WRITE, 0, 0,
                   section_at, "PSD writer: ICC resource length overflow");
      w->failed = 1;
      return;
    }
    td_psdw_u32(w, TD_PSDW_SIG_8BIM);
    td_psdw_u16(w, 1039u);
    td_psdw_u16(w, 0u);
    td_psdw_u32(w, (uint32_t)doc->icc_size);
    td_psdw_bytes(w, doc->icc, doc->icc_size);
    if (doc->icc_size & 1u) {
      td_psdw_u8(w, 0u);
    }
  }
  if (!w->failed) {
    uint64_t sz = (uint64_t)(w->len - section_at - 4u);
    if (sz > UINT32_MAX) {
      td_set_error(w->err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_WRITE, 0, 0,
                   section_at, "PSD writer: resource section length overflow");
      w->failed = 1;
      return;
    }
    w->p[section_at + 0u] = (uint8_t)(sz >> 24);
    w->p[section_at + 1u] = (uint8_t)(sz >> 16);
    w->p[section_at + 2u] = (uint8_t)(sz >> 8);
    w->p[section_at + 3u] = (uint8_t)(sz & 0xFFu);
  }
}

/* Emit the layer info payload (count + records + channel data), starting at
   its own length field. */
static tinydng_status td_psdw_layer_info(td_psdw *w,
                                         const tinydng_psd_write_doc *doc,
                                         uint16_t comp, int is_psb,
                                         tinydng_error *err) {
  tinydng_context *ctx = w->ctx;
  size_t li_at;
  size_t i;
  uint16_t c;
  td_psdw_payload *payloads = NULL;
  size_t n_payloads = 0, pi;
  tinydng_status st = TINYDNG_OK;

  li_at = td_psdw_mark(w, is_psb);
  td_psdw_u16(w, (uint16_t)(int16_t)(int32_t)doc->layer_count);

  /* Pre-compress every channel of every layer. */
  for (i = 0; i < doc->layer_count; i++) {
    n_payloads += doc->layers[i].channel_count;
  }
  if (n_payloads > 0u) {
    size_t bytes;
    if (!td_safe_mul_size(n_payloads, sizeof(td_psdw_payload), &bytes)) {
      return TINYDNG_E_BOUNDS;
    }
    payloads = (td_psdw_payload *)td_ctx_calloc(ctx, bytes, err);
    if (!payloads) {
      return TINYDNG_E_OOM;
    }
  }
  pi = 0;
  for (i = 0; i < doc->layer_count && st == TINYDNG_OK; i++) {
    const tinydng_psd_write_layer *L = &doc->layers[i];
    uint32_t lw = (uint32_t)(L->right - L->left);
    uint32_t lh = (uint32_t)(L->bottom - L->top);
    for (c = 0; c < L->channel_count && st == TINYDNG_OK; c++) {
      const tinydng_psd_write_channel *ch = &L->channels[c];
      uint8_t *stored = NULL;
      size_t stored_size = 0;
      if (ch->size == 0u) {
        payloads[pi].data = NULL;
        payloads[pi].size = 0;
        payloads[pi].compression = TINYDNG_PSD_COMP_RAW;
        pi++;
        continue;
      }
      stored = td_psdw_plane_be(ctx, ch->data, lw, lh, doc->depth,
                                &stored_size, err);
      if (!stored) {
        st = td_error_status_or(err, TINYDNG_E_OOM);
        break;
      }
      st = td_psdw_compress_plane(ctx, stored, stored_size, lw, lh,
                                  doc->depth, comp, is_psb, &payloads[pi],
                                  err);
      td_ctx_free(ctx, stored);
      pi++;
    }
  }

  /* Layer records. */
  pi = 0;
  for (i = 0; i < doc->layer_count && st == TINYDNG_OK; i++) {
    const tinydng_psd_write_layer *L = &doc->layers[i];
    size_t extra_at;
    td_psdw_i32(w, L->top);
    td_psdw_i32(w, L->left);
    td_psdw_i32(w, L->bottom);
    td_psdw_i32(w, L->right);
    td_psdw_u16(w, L->channel_count);
    for (c = 0; c < L->channel_count; c++) {
      uint64_t channel_len;
      if ((uint64_t)payloads[pi + c].size > UINT64_MAX - 2u) {
        td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_WRITE, 0, 0,
                     w->len, "PSD writer: channel length overflow");
        st = TINYDNG_E_BOUNDS;
        break;
      }
      channel_len = (uint64_t)payloads[pi + c].size + 2u;
      if (!is_psb && channel_len > UINT32_MAX) {
        td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_WRITE, 0, 0,
                     w->len, "PSD writer: channel length overflow");
        st = TINYDNG_E_BOUNDS;
        break;
      }
      td_psdw_u16(w, (uint16_t)L->channels[c].id);
      td_psdw_len(w, is_psb, channel_len);
    }
    if (st != TINYDNG_OK) {
      break;
    }
    pi += L->channel_count;
    td_psdw_u32(w, TD_PSDW_SIG_8BIM);
    td_psdw_u32(w, L->blend_mode ? L->blend_mode
                                 : (uint32_t)TINYDNG_PSD_BLEND_NORMAL);
    td_psdw_u8(w, L->opacity ? L->opacity : 255u);
    td_psdw_u8(w, L->clipping);
    td_psdw_u8(w, L->flags);
    td_psdw_u8(w, 0u); /* filler */

    extra_at = w->len;
    td_psdw_u32(w, 0u); /* extra length, patched */
    td_psdw_u32(w, 0u); /* no mask */
    td_psdw_u32(w, 0u); /* no blending ranges */
    td_psdw_pascal(w, L->name, 4u);
    if (L->section != 0u) {
      td_psdw_u32(w, TD_PSDW_SIG_8BIM);
      td_psdw_u32(w, TINYDNG_PSD_FOURCC('l', 's', 'c', 't'));
      td_psdw_u32(w, 4u);
      td_psdw_u32(w, L->section);
    }
    {
      /* 'luni' unicode name. */
      size_t luni_len_at;
      td_psdw_u32(w, TD_PSDW_SIG_8BIM);
      td_psdw_u32(w, TINYDNG_PSD_FOURCC('l', 'u', 'n', 'i'));
      luni_len_at = w->len;
      td_psdw_u32(w, 0u);
      {
        size_t start = w->len;
        td_psdw_unicode(w, L->name);
        if (!w->failed) {
          uint64_t sz = (uint64_t)(w->len - start);
          if (!td_psdw_patch_u32_len(w, luni_len_at, sz, err)) {
            st = TINYDNG_E_BOUNDS;
          }
          if (st != TINYDNG_OK) {
            continue;
          }
          if (sz & 1u) {
            if (sz == UINT32_MAX) {
              td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_WRITE, 0, 0,
                           luni_len_at,
                           "PSD writer: unicode name length overflow");
              st = TINYDNG_E_BOUNDS;
              continue;
            }
            td_psdw_u8(w, 0u);
            sz++;
          }
          if (!td_psdw_patch_u32_len(w, luni_len_at, sz, err)) {
            st = TINYDNG_E_BOUNDS;
          }
        }
      }
    }
    if (!w->failed && st == TINYDNG_OK) {
      uint64_t sz = (uint64_t)(w->len - extra_at - 4u);
      if (!td_psdw_patch_u32_len(w, extra_at, sz, err)) {
        st = TINYDNG_E_BOUNDS;
      }
    }
  }

  /* Channel image data. */
  pi = 0;
  for (i = 0; i < doc->layer_count && st == TINYDNG_OK; i++) {
    const tinydng_psd_write_layer *L = &doc->layers[i];
    for (c = 0; c < L->channel_count; c++) {
      td_psdw_u16(w, payloads[pi].compression);
      td_psdw_bytes(w, payloads[pi].data, payloads[pi].size);
      pi++;
    }
  }

  for (pi = 0; pi < n_payloads; pi++) {
    td_ctx_free(ctx, payloads[pi].data);
  }
  td_ctx_free(ctx, payloads);
  if (st != TINYDNG_OK) {
    return st;
  }
  if (w->failed) {
    return td_error_status_or(err, TINYDNG_E_OOM);
  }

  /* Patch layer-info length (payload after the length field, even). */
  {
    size_t len_field = is_psb ? 8u : 4u;
    uint64_t payload = (uint64_t)(w->len - li_at - len_field);
    if (payload & 1u) {
      if (!is_psb && payload == UINT32_MAX) {
        td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_WRITE, 0, 0,
                     li_at, "PSD writer: layer section length overflow");
        return TINYDNG_E_BOUNDS;
      }
      td_psdw_u8(w, 0u);
      payload++;
    }
    if (is_psb) {
      td_psdw_patch_len(w, 1, li_at, payload);
    } else if (!td_psdw_patch_u32_len(w, li_at, payload, err)) {
      return TINYDNG_E_BOUNDS;
    }
  }
  return TINYDNG_OK;
}

/* ------------------------------------------------------------------ */
/* Public entry points                                                */
/* ------------------------------------------------------------------ */

tinydng_status tinydng_psd_write_memory(tinydng_context *ctx,
                                        const tinydng_psd_write_doc *doc,
                                        const tinydng_psd_write_options *opts,
                                        uint8_t **out_data, size_t *out_size,
                                        tinydng_error *err) {
  td_psdw w;
  int is_psb = (opts && opts->as_psb) ? 1 : 0;
  uint16_t comp = opts ? opts->compression : (uint16_t)TINYDNG_PSD_COMP_RLE;
  tinydng_status st;
  size_t lm_at;

  tinydng_error_clear(err);
  if (!ctx || !doc || !out_data || !out_size) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "null argument to tinydng_psd_write_memory");
    return TINYDNG_E_INVALID_ARG;
  }
  *out_data = NULL;
  *out_size = 0;
  if (!opts) {
    comp = TINYDNG_PSD_COMP_RLE;
  }
  st = td_psdw_validate(ctx, doc, opts, err);
  if (st != TINYDNG_OK) {
    return st;
  }

  memset(&w, 0, sizeof(w));
  w.ctx = ctx;
  w.err = err;

  /* --- header --- */
  td_psdw_u32(&w, TD_PSDW_SIG_8BPS);
  td_psdw_u16(&w, is_psb ? 2u : 1u);
  td_psdw_zeros(&w, 6u);
  td_psdw_u16(&w, doc->channel_count);
  td_psdw_u32(&w, doc->height);
  td_psdw_u32(&w, doc->width);
  td_psdw_u16(&w, doc->depth);
  td_psdw_u16(&w, doc->color_mode);

  /* --- color mode data --- */
  if (doc->color_mode == TINYDNG_PSD_INDEXED) {
    td_psdw_u32(&w, 768u);
    td_psdw_bytes(&w, doc->palette, 768u);
  } else {
    td_psdw_u32(&w, 0u);
  }

  /* --- image resources --- */
  td_psdw_resources(&w, doc);
  if (w.failed) {
    st = td_error_status_or(err, TINYDNG_E_BOUNDS);
    td_ctx_free(ctx, w.p);
    return st;
  }

  /* --- layer and mask info --- */
  lm_at = td_psdw_mark(&w, is_psb);
  if (doc->layer_count > 0u) {
    st = td_psdw_layer_info(&w, doc, comp, is_psb, err);
    if (st != TINYDNG_OK) {
      td_ctx_free(ctx, w.p);
      return st;
    }
    if (w.failed) {
      st = td_error_status_or(err, TINYDNG_E_OOM);
      td_ctx_free(ctx, w.p);
      return st;
    }
    td_psdw_u32(&w, 0u); /* global layer mask info: empty */
  }
  {
    size_t len_field = is_psb ? 8u : 4u;
    uint64_t total = (uint64_t)(w.len - lm_at - len_field);
    if (total & 1u) {
      if (!is_psb && total == UINT32_MAX) {
        td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_WRITE, 0, 0,
                     lm_at, "PSD writer: layer-mask section length overflow");
        td_ctx_free(ctx, w.p);
        return TINYDNG_E_BOUNDS;
      }
      td_psdw_u8(&w, 0u);
      total++;
    }
    if (is_psb) {
      td_psdw_patch_len(&w, 1, lm_at, total);
    } else if (!td_psdw_patch_u32_len(&w, lm_at, total, err)) {
      td_ctx_free(ctx, w.p);
      return TINYDNG_E_BOUNDS;
    }
  }

  /* --- composite image data --- */
  {
    uint32_t channels = doc->channel_count;
    size_t sample_bytes = (size_t)doc->depth / 8u;
    size_t plane_samples, plane_size;
    if (!td_safe_mul_size((size_t)doc->width, (size_t)doc->height,
                          &plane_samples) ||
        !td_safe_mul_size(plane_samples, sample_bytes, &plane_size)) {
      td_ctx_free(ctx, w.p);
      td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_WRITE, 0, 0, 0,
                   "PSD writer: composite plane size overflow");
      return TINYDNG_E_BOUNDS;
    }
    uint8_t *plane = (uint8_t *)td_ctx_alloc(ctx, plane_size ? plane_size
                                                             : 1u,
                                             err);
    uint32_t cch;
    uint16_t used = comp;
    td_psdw_payload *pls = NULL;

    if (!plane) {
      td_ctx_free(ctx, w.p);
      return TINYDNG_E_OOM;
    }
    pls = (td_psdw_payload *)td_ctx_calloc(
        ctx, (size_t)channels * sizeof(td_psdw_payload), err);
    if (!pls) {
      td_ctx_free(ctx, plane);
      td_ctx_free(ctx, w.p);
      return TINYDNG_E_OOM;
    }
    /* Compress all planes first: RLE must fall back to RAW for ALL planes
       together (the composite has a single compression tag). */
    for (cch = 0; cch < channels; cch++) {
      size_t s;
      if (doc->composite) {
        const uint8_t *in = doc->composite;
        if (sample_bytes == 1u) {
          for (s = 0; s < plane_samples; s++) {
            plane[s] = in[s * channels + cch];
          }
        } else if (sample_bytes == 2u) {
          for (s = 0; s < plane_samples; s++) {
            uint16_t v;
            memcpy(&v, in + (s * channels + cch) * 2u, 2u);
            plane[2u * s] = (uint8_t)(v >> 8);
            plane[2u * s + 1u] = (uint8_t)(v & 0xFFu);
          }
        } else {
          for (s = 0; s < plane_samples; s++) {
            uint32_t v;
            memcpy(&v, in + (s * channels + cch) * 4u, 4u);
            plane[4u * s] = (uint8_t)(v >> 24);
            plane[4u * s + 1u] = (uint8_t)(v >> 16);
            plane[4u * s + 2u] = (uint8_t)(v >> 8);
            plane[4u * s + 3u] = (uint8_t)(v & 0xFFu);
          }
        }
      } else {
        memset(plane, 0, plane_size);
      }
      st = td_psdw_compress_plane(ctx, plane, plane_size, doc->width,
                                  doc->height, doc->depth, used, is_psb,
                                  &pls[cch], err);
      if (st != TINYDNG_OK) {
        break;
      }
      if (pls[cch].compression != used) {
        /* A plane fell back to RAW: restart all planes as RAW. */
        uint32_t k;
        for (k = 0; k <= cch; k++) {
          td_ctx_free(ctx, pls[k].data);
          memset(&pls[k], 0, sizeof(pls[k]));
        }
        used = TINYDNG_PSD_COMP_RAW;
        cch = (uint32_t)-1; /* restart loop */
      }
    }
    td_ctx_free(ctx, plane);
    if (st != TINYDNG_OK) {
      for (cch = 0; cch < channels; cch++) {
        td_ctx_free(ctx, pls[cch].data);
      }
      td_ctx_free(ctx, pls);
      td_ctx_free(ctx, w.p);
      return st;
    }

    td_psdw_u16(&w, used);
    if (used == TINYDNG_PSD_COMP_RLE) {
      /* All row tables first (channel-major), then all row data. */
      size_t esz = is_psb ? 4u : 2u;
      size_t table_bytes = (size_t)doc->height * esz;
      for (cch = 0; cch < channels; cch++) {
        td_psdw_bytes(&w, pls[cch].data, table_bytes);
      }
      for (cch = 0; cch < channels; cch++) {
        td_psdw_bytes(&w, pls[cch].data + table_bytes,
                      pls[cch].size - table_bytes);
      }
    } else {
      for (cch = 0; cch < channels; cch++) {
        td_psdw_bytes(&w, pls[cch].data, pls[cch].size);
      }
    }
    for (cch = 0; cch < channels; cch++) {
      td_ctx_free(ctx, pls[cch].data);
    }
    td_ctx_free(ctx, pls);
  }

  if (w.failed) {
    td_ctx_free(ctx, w.p);
    if (!err || err->status == TINYDNG_OK) {
      td_set_error(err, TINYDNG_E_OOM, TINYDNG_STAGE_WRITE, 0, 0, 0,
                   "PSD writer: buffer growth failed");
    }
    return td_error_status_or(err, TINYDNG_E_OOM);
  }
  *out_data = w.p;
  *out_size = w.len;
  return TINYDNG_OK;
}

tinydng_status tinydng_psd_write_file(tinydng_context *ctx, const char *path,
                                      const tinydng_psd_write_doc *doc,
                                      const tinydng_psd_write_options *opts,
                                      tinydng_error *err) {
  uint8_t *buf = NULL;
  size_t size = 0;
  tinydng_status st;
  FILE *fp;

  if (!ctx || !path || !doc) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "null argument to tinydng_psd_write_file");
    return TINYDNG_E_INVALID_ARG;
  }
  st = tinydng_psd_write_memory(ctx, doc, opts, &buf, &size, err);
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

#endif /* TINYDNG_NO_PSD */
