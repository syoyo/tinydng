/*
 * tinydng_write.c - uncompressed TIFF/DNG writer (single image) + the
 * streaming/tiled writer (tinydng_writer_*).
 * SPDX-License-Identifier: MIT
 */
/* Feature-test macros must precede all system includes so that POSIX
   fseeko/ftello are visible even under a strict -std=c11 compile. */
#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#endif

#include "td_internal.h"
#include "tiny_dng_ljpeg92_v2.h"

#include <limits.h>
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
  uint64_t count;
  uint8_t value[8];   /* inline value (LE-agnostic; see below)        */
  size_t ext_len;     /* >0 => value is out-of-line, serialized in `ext`  */
  const uint8_t *ext; /* points into the extras buffer                    */
} td_wentry;

typedef struct td_writer {
  int big_endian;
  int bigtiff;
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

static void td_put64(uint8_t *p, uint64_t v, int be) {
  if (be) {
    p[0] = (uint8_t)(v >> 56);
    p[1] = (uint8_t)(v >> 48);
    p[2] = (uint8_t)(v >> 40);
    p[3] = (uint8_t)(v >> 32);
    p[4] = (uint8_t)(v >> 24);
    p[5] = (uint8_t)(v >> 16);
    p[6] = (uint8_t)(v >> 8);
    p[7] = (uint8_t)v;
  } else {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
    p[4] = (uint8_t)(v >> 32);
    p[5] = (uint8_t)(v >> 40);
    p[6] = (uint8_t)(v >> 48);
    p[7] = (uint8_t)(v >> 56);
  }
}

/* Append serialized bytes to the extras buffer; returns pointer or NULL. */
static const uint8_t *td_extras_put(td_writer *w, const uint8_t *bytes,
                                    size_t len) {
  size_t padded;
  if (!td_safe_add_size(len, 1u, &padded)) {
    w->failed = 1;
    return NULL;
  }
  padded &= ~(size_t)1u; /* word-align */
  const uint8_t *ret;
  if (padded > w->extras_cap || w->extras_len > w->extras_cap - padded) {
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
static void td_add(td_writer *w, uint16_t tag, uint16_t type, uint64_t count,
                   const uint8_t *vbytes, size_t vlen) {
  td_wentry *e;
  size_t inline_cap = w->bigtiff ? 8u : 4u;
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
  memset(e->value, 0, 8);
  if (vlen <= inline_cap) {
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

static void td_add_long8(td_writer *w, uint16_t tag, uint64_t v) {
  uint8_t b[8];
  td_put64(b, v, w->big_endian);
  td_add(w, tag, TD_TYPE_LONG8, 1, b, 8);
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

/* Doubles -> SRATIONAL (num/den) with fixed denominator. The double->int32
 * conversion is saturated: a NaN/Inf or out-of-range caller value would be
 * undefined behavior (C11 6.3.1.4) as a plain cast. */
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
    int32_t num;
    double d = v[i] * (double)den;
    if (!(d == d)) { /* NaN */
      num = 0;
    } else if (d >= 2147483647.0) {
      num = INT32_MAX;
    } else if (d <= -2147483648.0) {
      num = (-2147483647 - 1);
    } else {
      num = (int32_t)d;
    }
    td_put32(tmp + i * 8u, (uint32_t)num, w->big_endian);
    td_put32(tmp + i * 8u + 4u, (uint32_t)den, w->big_endian);
  }
  td_add(w, tag, TD_TYPE_SRATIONAL, n, tmp, (size_t)n * 8u);
}

/* Doubles -> RATIONAL; negative / NaN values clamp to 0 (RATIONAL is
 * unsigned), huge values saturate at UINT32_MAX. */
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
    uint32_t num;
    double d = v[i] * (double)den;
    if (!(d == d) || d <= 0.0) { /* NaN or negative */
      num = 0;
    } else if (d >= 4294967295.0) {
      num = UINT32_MAX;
    } else {
      num = (uint32_t)d;
    }
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

/* TIFF LZW encode (MSB-first, early change). The dictionary has at most 3836
   entries between CLEAR codes. Keep it in a sparse hash table instead of the
   4096*256 direct-address table; this is both much smaller and friendlier to
   the cache for tiled writes. */
#define TD_LZW_HASH_BITS 13u
#define TD_LZW_HASH_SIZE (1u << TD_LZW_HASH_BITS)
#define TD_LZW_HASH_MASK (TD_LZW_HASH_SIZE - 1u)
#define TD_LZW_EMPTY_KEY UINT32_MAX

typedef struct td_lzw_table {
  uint32_t *keys;
  uint16_t *codes;
} td_lzw_table;

static inline size_t td_lzw_hash(uint32_t key) {
  return (size_t)((key * 2654435761u) >> (32u - TD_LZW_HASH_BITS));
}

static void td_lzw_clear(td_lzw_table *tbl) {
  size_t i;
  for (i = 0; i < TD_LZW_HASH_SIZE; i++) {
    tbl->keys[i] = TD_LZW_EMPTY_KEY;
  }
}

static inline size_t td_lzw_find_slot(const td_lzw_table *tbl, uint32_t key) {
  size_t slot = td_lzw_hash(key);
  while (tbl->keys[slot] != TD_LZW_EMPTY_KEY && tbl->keys[slot] != key) {
    slot = (slot + 1u) & TD_LZW_HASH_MASK;
  }
  return slot;
}

static long td_lzw_encode(const uint8_t *in, size_t in_len, uint8_t *out,
                          size_t out_cap, td_lzw_table *tbl) {
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

  td_lzw_clear(tbl);
  prev = in[0];
  for (i = 1; i < in_len; i++) {
    int c = in[i];
    uint32_t key = ((uint32_t)prev << 8) | (uint32_t)c;
    size_t slot = td_lzw_find_slot(tbl, key);
    if (tbl->keys[slot] == key) {
      prev = tbl->codes[slot];
      continue;
    }
    if (tdw_emit(out, out_cap, &op, &bitbuf, &bitcnt, width, prev)) {
      return -1;
    }
    tbl->keys[slot] = key;
    tbl->codes[slot] = (uint16_t)next;
    next++;
    /* Mirror libtiff exactly: reset at free_ent==CODE_MAX-1 (4094), else widen
       when free_ent > maxcode, i.e. next == (1<<width). The encoder widens one
       entry later than the decoder (TIFF early change). */
    if (next == 4094) {
      if (tdw_emit(out, out_cap, &op, &bitbuf, &bitcnt, width, CLEAR)) {
        return -1;
      }
      td_lzw_clear(tbl);
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

/* ------------------------------------------------------------------ */
/* Write-io backends (file / memory)                                  */
/* ------------------------------------------------------------------ */

static int td_wseek(FILE *fp, uint64_t off) {
#if defined(_WIN32)
  return _fseeki64(fp, (__int64)off, SEEK_SET);
#elif defined(__unix__) || defined(__APPLE__) || defined(__linux__)
  return fseeko(fp, (off_t)off, SEEK_SET);
#else
  if (off > (uint64_t)LONG_MAX) {
    return -1;
  }
  return fseek(fp, (long)off, SEEK_SET);
#endif
}

typedef struct td_wio_file {
  tinydng_context *ctx;
  FILE *fp;
} td_wio_file;

static size_t td_wio_file_write(tinydng_write_io *io, uint64_t off,
                                const void *data, size_t len) {
  td_wio_file *f = (td_wio_file *)io->backend;
  if (td_wseek(f->fp, off) != 0) {
    return 0;
  }
  return fwrite(data, 1, len, f->fp);
}

static uint64_t td_wio_file_size(tinydng_write_io *io) {
  td_wio_file *f = (td_wio_file *)io->backend;
  long cur, end;
  cur = ftell(f->fp);
  if (td_wseek(f->fp, 0) != 0) {
    return 0;
  }
  if (td_wseek(f->fp, (uint64_t)-1) != 0 && fseek(f->fp, 0, SEEK_END) != 0) {
    return 0;
  }
  end = ftell(f->fp);
  if (cur >= 0) {
    td_wseek(f->fp, (uint64_t)cur);
  }
  return (end < 0) ? 0u : (uint64_t)end;
}

static void td_wio_file_close(tinydng_write_io *io) {
  td_wio_file *f = (td_wio_file *)io->backend;
  if (f) {
    if (f->fp) {
      fclose(f->fp);
    }
    td_ctx_free(f->ctx, f);
  }
  io->backend = NULL;
}

tinydng_status tinydng_write_io_open_file(tinydng_context *ctx,
                                          const char *path,
                                          tinydng_write_io *out,
                                          tinydng_error *err) {
  td_wio_file *f;
  if (!ctx || !path || !out) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "null argument to tinydng_write_io_open_file");
    return TINYDNG_E_INVALID_ARG;
  }
  f = (td_wio_file *)td_ctx_calloc(ctx, sizeof(*f), err);
  if (!f) {
    return TINYDNG_E_OOM;
  }
  f->ctx = ctx;
  f->fp = fopen(path, "w+b");
  if (!f->fp) {
    td_ctx_free(ctx, f);
    td_set_error(err, TINYDNG_E_IO, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "fopen('%s') for write failed", path);
    return TINYDNG_E_IO;
  }
  memset(out, 0, sizeof(*out));
  out->write = td_wio_file_write;
  out->size = td_wio_file_size;
  out->close = td_wio_file_close;
  out->backend = f;
  return TINYDNG_OK;
}

typedef struct td_wio_mem {
  tinydng_context *ctx;
  uint8_t *data;
  size_t cap;
  size_t size; /* high-water mark */
  int detached;
} td_wio_mem;

static size_t td_wio_mem_write(tinydng_write_io *io, uint64_t off,
                               const void *data, size_t len) {
  td_wio_mem *m = (td_wio_mem *)io->backend;
  uint64_t end = off + len;
  if (end < off) {
    return 0; /* u64 wrap */
  }
  if (end > (uint64_t)m->cap) {
    size_t cap = m->cap ? m->cap : 4096u;
    while (cap < end) {
      if (cap > (size_t)-1 / 2u) {
        cap = (size_t)end;
        break;
      }
      cap *= 2u;
    }
    if (cap < end) {
      return 0; /* offset does not fit size_t */
    }
    {
      tinydng_error rerr;
      uint8_t *nb = (uint8_t *)td_ctx_realloc(m->ctx, m->data, m->cap, cap,
                                              &rerr);
      if (!nb) {
        return 0;
      }
      m->data = nb;
      m->cap = cap;
    }
  }
  if (off > (uint64_t)m->size) {
    memset(m->data + m->size, 0, (size_t)(off - (uint64_t)m->size));
  }
  memcpy(m->data + (size_t)off, data, len);
  if (end > (uint64_t)m->size) {
    m->size = (size_t)end;
  }
  return len;
}

static uint64_t td_wio_mem_size(tinydng_write_io *io) {
  return (uint64_t)((td_wio_mem *)io->backend)->size;
}

static void td_wio_mem_close(tinydng_write_io *io) {
  td_wio_mem *m = (td_wio_mem *)io->backend;
  if (m) {
    if (!m->detached) {
      td_ctx_free(m->ctx, m->data);
    }
    td_ctx_free(m->ctx, m);
  }
  io->backend = NULL;
}

tinydng_status tinydng_write_io_open_memory(tinydng_context *ctx,
                                            tinydng_write_io *out,
                                            tinydng_error *err) {
  td_wio_mem *m;
  if (!ctx || !out) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "null argument to tinydng_write_io_open_memory");
    return TINYDNG_E_INVALID_ARG;
  }
  m = (td_wio_mem *)td_ctx_calloc(ctx, sizeof(*m), err);
  if (!m) {
    return TINYDNG_E_OOM;
  }
  m->ctx = ctx;
  memset(out, 0, sizeof(*out));
  out->write = td_wio_mem_write;
  out->size = td_wio_mem_size;
  out->close = td_wio_mem_close;
  out->backend = m;
  return TINYDNG_OK;
}

tinydng_status tinydng_write_io_memory_take(tinydng_context *ctx,
                                            tinydng_write_io *io,
                                            uint8_t **out_data,
                                            size_t *out_size,
                                            tinydng_error *err) {
  td_wio_mem *m;
  if (!ctx || !io || !out_data || !out_size) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "null argument to tinydng_write_io_memory_take");
    return TINYDNG_E_INVALID_ARG;
  }
  m = (td_wio_mem *)io->backend;
  if (!m || io->write != td_wio_mem_write || m->detached) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "io is not a memory backend (or already taken)");
    return TINYDNG_E_INVALID_ARG;
  }
  *out_data = m->data;
  *out_size = m->size;
  m->data = NULL;
  m->cap = 0;
  m->size = 0;
  m->detached = 1;
  return TINYDNG_OK;
}

/* ------------------------------------------------------------------ */
/* Streaming / tiled writer                                           */
/* ------------------------------------------------------------------ */

/* Reserve `len` zeroed bytes in the extras buffer (even-aligned like
   td_extras_put); returns a writable pointer or NULL (w->failed set). */
static uint8_t *td_reserve(td_writer *w, size_t len) {
  size_t padded;
  if (!td_safe_add_size(len, 1u, &padded)) {
    w->failed = 1;
    return NULL;
  }
  padded &= ~(size_t)1u;
  uint8_t *ret;
  if (w->failed || padded > w->extras_cap ||
      w->extras_len > w->extras_cap - padded) {
    w->failed = 1;
    return NULL;
  }
  ret = w->extras + w->extras_len;
  memset(ret, 0, padded);
  w->extras_len += padded;
  return ret;
}

/* Add a tag whose value already lives in a reserved extras region. */
static void td_add_reserved(td_writer *w, uint16_t tag, uint16_t type,
                            uint64_t count, uint8_t *ext, size_t ext_len) {
  td_wentry *e;
  if (w->failed || w->entry_count >= TD_WMAX_ENTRIES) {
    w->failed = 1;
    return;
  }
  e = &w->entries[w->entry_count++];
  e->tag = tag;
  e->type = type;
  e->count = count;
  e->ext_len = ext_len;
  e->ext = ext;
  memset(e->value, 0, 8);
}

typedef struct td_writer_seg {
  uint64_t offset;   /* absolute payload offset in the stream */
  uint64_t byte_count;
} td_writer_seg;

struct tinydng_writer {
  tinydng_context *ctx;
  tinydng_write_io sink;
  int big_endian;
  int bigtiff;
  uint32_t width, height;
  uint16_t spp, bps, sfmt, photo;
  uint16_t compression; /* effective: NONE / LZW / NEW_JPEG */
  int tiled;
  uint32_t tile_width, tile_length;
  uint32_t rows_per_strip;
  uint32_t seg_count;
  td_writer_seg *segs;
  td_writer w;
  uint64_t data_pos;
  int finished;
  /* Cached LZW encoder hash table (reused across tiles). */
  td_lzw_table lzw_tbl;
};

static tinydng_status td_wio_err(tinydng_write_io *sink, uint64_t at,
                                 tinydng_error *err) {
  (void)sink;
  td_set_error(err, TINYDNG_E_IO, TINYDNG_STAGE_WRITE, 0, 0, at,
               "sink short write");
  return TINYDNG_E_IO;
}

/* Adapter: route the LJPEG92 streaming encoder's output directly into the
   write-io sink at a fixed absolute position. */
typedef struct td_lj92_sink_ctx {
  tinydng_write_io *sink;
  uint64_t pos;     /* absolute write position */
  uint64_t written; /* bytes written for this stream */
  int failed;
} td_lj92_sink_ctx;

static size_t td_lj92_sink_write(void *user, const void *data, size_t len) {
  td_lj92_sink_ctx *s = (td_lj92_sink_ctx *)user;
  if (s->failed) {
    return 0;
  }
  if (UINT64_MAX - s->pos < s->written ||
      UINT64_MAX - (s->pos + s->written) < (uint64_t)len ||
      s->sink->write(s->sink, s->pos + s->written, data, len) != len) {
    s->failed = 1;
    return 0;
  }
  s->written += len;
  return len;
}

/* TIFF PackBits encode. Emits literal runs (n in [0,127] => n+1 literal
   bytes) and repeat runs (n in [-127,-1] => 1-n copies of one byte); the
   loader's td_packbits_decode handles both. Returns encoded length or -1
   on overflow. */
static long td_packbits_encode(const uint8_t *in, size_t in_len, uint8_t *out,
                               size_t out_cap) {
  size_t ip = 0, op = 0;
  while (ip < in_len) {
    size_t run = 1;
    while (ip + run < in_len && in[ip + run] == in[ip] && run < 128u) {
      run++;
    }
    if (run >= 3u) {
      if (op + 2u > out_cap) {
        return -1;
      }
      out[op++] = (uint8_t)(1 - (int)run);
      out[op++] = in[ip];
      ip += run;
    } else {
      size_t lit = 0;
      while (ip < in_len && lit < 128u) {
        size_t r = 1;
        while (ip + r < in_len && in[ip + r] == in[ip] && r < 128u) {
          r++;
        }
        if (r >= 3u) {
          break;
        }
        ip++;
        lit++;
      }
      if (op + 1u + lit > out_cap) {
        return -1;
      }
      out[op++] = (uint8_t)(lit - 1u);
      memcpy(out + op, in + ip - lit, lit);
      op += lit;
    }
  }
  return (long)op;
}

/* Encode one tile/strip payload and write it at absolute `at`. Returns the
   encoded length via *out_len. `pw`/`ph` are the (edge-cropped) payload
   dims. */
static tinydng_status td_writer_put_payload(tinydng_writer *w,
                                            const uint8_t *pixels, uint32_t pw,
                                            uint32_t ph, uint64_t at,
                                            size_t *out_len,
                                            tinydng_error *err) {
  size_t sb = (size_t)w->bps / 8u;
  size_t data_size;
  size_t n_samples;
  int need_swap = (sb > 1u) && (w->big_endian != tdw_host_big());

  if (!td_safe_mul_size((size_t)pw, (size_t)ph, &data_size) ||
      !td_safe_mul_size(data_size, (size_t)w->spp, &data_size) ||
      !td_safe_mul_size(data_size, sb, &data_size)) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_WRITE, 0, 0, at,
                 "payload size overflow");
    return TINYDNG_E_BOUNDS;
  }
  n_samples = (sb > 0u) ? (data_size / sb) : data_size;

  *out_len = 0;
  switch (w->compression) {
    case TINYDNG_COMPRESSION_NONE:
      if (need_swap) {
        uint8_t *tmp = (uint8_t *)td_ctx_alloc(w->ctx, data_size, err);
        if (!tmp) {
          return TINYDNG_E_OOM;
        }
        tdw_swap_copy(tmp, pixels, n_samples, sb);
        if (w->sink.write(&w->sink, at, tmp, data_size) != data_size) {
          td_ctx_free(w->ctx, tmp);
          return td_wio_err(&w->sink, at, err);
        }
        td_ctx_free(w->ctx, tmp);
      } else if (w->sink.write(&w->sink, at, pixels, data_size) != data_size) {
        return td_wio_err(&w->sink, at, err);
      }
      *out_len = data_size;
      return TINYDNG_OK;

    case TINYDNG_COMPRESSION_LZW: {
      const uint8_t *src = pixels;
      uint8_t *tmp = NULL;
      uint8_t *enc;
      size_t cap;
      long n;
      if (need_swap) {
        tmp = (uint8_t *)td_ctx_alloc(w->ctx, data_size, err);
        if (!tmp) {
          return TINYDNG_E_OOM;
        }
        tdw_swap_copy(tmp, pixels, n_samples, sb);
        src = tmp;
      }
      if (!td_safe_add_size(data_size, data_size / 2u, &cap) ||
          !td_safe_add_size(cap, 1024u, &cap)) {
        td_ctx_free(w->ctx, tmp);
        td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_WRITE, 0, 0, at,
                     "LZW buffer size overflow");
        return TINYDNG_E_BOUNDS;
      }
      enc = (uint8_t *)td_ctx_alloc(w->ctx, cap, err);
      if (!enc) {
        td_ctx_free(w->ctx, tmp);
        return TINYDNG_E_OOM;
      }
      /* Use the pre-allocated hash table cached in the writer, falling back
         to per-call allocation for writers that don't cache it. */
      if (!w->lzw_tbl.keys) {
        w->lzw_tbl.keys = (uint32_t *)td_ctx_alloc(
            w->ctx, (size_t)TD_LZW_HASH_SIZE * sizeof(*w->lzw_tbl.keys), err);
        w->lzw_tbl.codes = (uint16_t *)td_ctx_alloc(
            w->ctx, (size_t)TD_LZW_HASH_SIZE * sizeof(*w->lzw_tbl.codes), err);
        if (!w->lzw_tbl.keys || !w->lzw_tbl.codes) {
          td_ctx_free(w->ctx, tmp);
          td_ctx_free(w->ctx, enc);
          td_ctx_free(w->ctx, w->lzw_tbl.keys);
          td_ctx_free(w->ctx, w->lzw_tbl.codes);
          w->lzw_tbl.keys = NULL;
          w->lzw_tbl.codes = NULL;
          return TINYDNG_E_OOM;
        }
      }
      n = td_lzw_encode(src, data_size, enc, cap, &w->lzw_tbl);
      td_ctx_free(w->ctx, tmp);
      if (n < 0) {
        td_ctx_free(w->ctx, enc);
        td_set_error(err, TINYDNG_E_INTERNAL, TINYDNG_STAGE_WRITE, 0, 0, at,
                     "LZW encode overflow");
        return TINYDNG_E_INTERNAL;
      }
      if (w->sink.write(&w->sink, at, enc, (size_t)n) != (size_t)n) {
        td_ctx_free(w->ctx, enc);
        return td_wio_err(&w->sink, at, err);
      }
      td_ctx_free(w->ctx, enc);
      *out_len = (size_t)n;
      return TINYDNG_OK;
    }

    case TINYDNG_COMPRESSION_PACKBITS: {
      const uint8_t *src = pixels;
      uint8_t *tmp = NULL;
      uint8_t *enc;
      size_t cap;
      long n;
      if (need_swap) {
        tmp = (uint8_t *)td_ctx_alloc(w->ctx, data_size, err);
        if (!tmp) {
          return TINYDNG_E_OOM;
        }
        tdw_swap_copy(tmp, pixels, n_samples, sb);
        src = tmp;
      }
      if (!td_safe_add_size(data_size, data_size / 2u, &cap) ||
          !td_safe_add_size(cap, 1024u, &cap)) {
        td_ctx_free(w->ctx, tmp);
        td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_WRITE, 0, 0, at,
                     "PackBits buffer size overflow");
        return TINYDNG_E_BOUNDS;
      }
      enc = (uint8_t *)td_ctx_alloc(w->ctx, cap, err);
      if (!enc) {
        td_ctx_free(w->ctx, tmp);
        return TINYDNG_E_OOM;
      }
      n = td_packbits_encode(src, data_size, enc, cap);
      td_ctx_free(w->ctx, tmp);
      if (n < 0) {
        td_ctx_free(w->ctx, enc);
        td_set_error(err, TINYDNG_E_INTERNAL, TINYDNG_STAGE_WRITE, 0, 0, at,
                     "PackBits encode overflow");
        return TINYDNG_E_INTERNAL;
      }
      if (w->sink.write(&w->sink, at, enc, (size_t)n) != (size_t)n) {
        td_ctx_free(w->ctx, enc);
        return td_wio_err(&w->sink, at, err);
      }
      td_ctx_free(w->ctx, enc);
      *out_len = (size_t)n;
      return TINYDNG_OK;
    }

    case TINYDNG_COMPRESSION_NEW_JPEG: {
      /* Lossless JPEG (predictor 1, 16-bit, host-order samples) streamed
         straight to the sink — no intermediate encode buffer. */
      td_lj92_sink_ctx sc;
      tdng_lj92_enc lj = NULL;
      const uint16_t *img16 = (const uint16_t *)(const void *)pixels;
      int ret;
      sc.sink = &w->sink;
      sc.pos = at;
      sc.written = 0;
      sc.failed = 0;
      ret = tdng_lj92_encode_open(&lj, (int)pw, (int)ph, (int)w->bps,
                                  (int)w->spp, 1 /*predictor=left*/,
                                  (int)((size_t)pw * w->spp), 0, &sc,
                                  td_lj92_sink_write);
      if (ret == TDNG_LJ92_ERROR_NONE) {
        ret = tdng_lj92_encode_scan(lj, img16, NULL, 0);
      }
      if (ret == TDNG_LJ92_ERROR_NONE) {
        ret = tdng_lj92_encode_begin(lj);
      }
      if (ret == TDNG_LJ92_ERROR_NONE) {
        ret = tdng_lj92_encode_rows(lj, img16, 0, (int)ph);
      }
      if (ret == TDNG_LJ92_ERROR_NONE) {
        ret = tdng_lj92_encode_finish(lj);
      } else {
        tdng_lj92_encode_finish(lj);
      }
      if (ret != TDNG_LJ92_ERROR_NONE || sc.failed) {
        td_set_error(err, TINYDNG_E_INTERNAL, TINYDNG_STAGE_WRITE, 0, 0, at,
                     "lossless JPEG payload encode failed rc=%d", ret);
        return TINYDNG_E_INTERNAL;
      }
      *out_len = (size_t)sc.written;
      return TINYDNG_OK;
    }

    default:
      td_set_error(err, TINYDNG_E_UNSUPPORTED, TINYDNG_STAGE_WRITE, 0, 0, 0,
                   "unsupported writer compression %u",
                   (unsigned)w->compression);
      return TINYDNG_E_UNSUPPORTED;
  }
}

/* Patch the tile/strip offset+count arrays (or inline values) with the
   real positions once every segment has been written. */
static void td_writer_patch_segs(tinydng_writer *w) {
  uint16_t off_tag = w->tiled ? TD_TAG_TILE_OFFSETS : TD_TAG_STRIP_OFFSETS;
  uint16_t cnt_tag = w->tiled ? TD_TAG_TILE_BYTE_COUNTS : TD_TAG_STRIP_BYTE_COUNTS;
  size_t i;
  int bigtiff = w->bigtiff;
  size_t elem_sz = bigtiff ? 8u : 4u;
  for (i = 0; i < w->w.entry_count; i++) {
    td_wentry *e = &w->w.entries[i];
    if (e->tag == off_tag || e->tag == cnt_tag) {
      uint32_t k;
      for (k = 0; k < w->seg_count; k++) {
        uint8_t b[8];
        uint64_t v = (e->tag == off_tag) ? w->segs[k].offset
                                         : w->segs[k].byte_count;
        if (bigtiff) {
          td_put64(b, v, w->big_endian);
        } else {
          td_put32(b, (uint32_t)v, w->big_endian);
        }
        if (e->ext_len > 0u) {
          memcpy((uint8_t *)e->ext + (size_t)k * elem_sz, b, elem_sz);
        } else if (k == 0u) {
          memcpy(e->value, b, elem_sz);
        }
      }
    }
  }
}

/* Serialize the IFD (entries sorted by tag) at `ifd_off`, with out-of-line
   values re-based to `extras_base`. */
static tinydng_status td_writer_emit_ifd(const td_writer *w,
                                         tinydng_write_io *sink,
                                         uint64_t ifd_off,
                                         uint64_t extras_base,
                                         tinydng_error *err) {
  size_t ext_running = 0;
  size_t i;

  if (w->bigtiff) {
    /* BigTIFF: 20-byte entries, 8-byte count, 8-byte inline value, 8-byte
       next-IFD pointer, 8-byte entry-count field. */
    uint8_t hdr[8u + TD_WMAX_ENTRIES * 20u + 8u];
    size_t n = 8u + w->entry_count * 20u + 8u;
    td_put64(hdr, w->entry_count, w->big_endian);
    for (i = 0; i < w->entry_count; i++) {
      const td_wentry *e = &w->entries[i];
      uint8_t *ep = hdr + 8u + i * 20u;
      td_put16(ep, e->tag, w->big_endian);
      td_put16(ep + 2, e->type, w->big_endian);
      td_put64(ep + 4, e->count, w->big_endian);
      if (e->ext_len > 0u) {
        size_t padded;
        uint64_t ext_off;
        if (!td_safe_add_size(e->ext_len, 1u, &padded) ||
            !td_safe_add_u64(extras_base, (uint64_t)ext_running, &ext_off)) {
          td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_WRITE, 0, e->tag,
                       extras_base, "IFD extra offset overflow");
          return TINYDNG_E_BOUNDS;
        }
        padded &= ~(size_t)1u;
        td_put64(ep + 12, ext_off, w->big_endian);
        if (padded > SIZE_MAX - ext_running) {
          td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_WRITE, 0, e->tag,
                       extras_base, "IFD extra size overflow");
          return TINYDNG_E_BOUNDS;
        }
        ext_running += padded;
      } else {
        memcpy(ep + 12, e->value, 8);
      }
    }
    td_put64(hdr + 8u + w->entry_count * 20u, 0u, w->big_endian);
    if (sink->write(sink, ifd_off, hdr, n) != n) {
      return td_wio_err(sink, ifd_off, err);
    }
  } else {
    /* Classic: 12-byte entries, 4-byte count, 4-byte inline value, 4-byte
       next-IFD pointer, 2-byte entry-count field. */
    uint8_t hdr[2u + TD_WMAX_ENTRIES * 12u + 4u];
    size_t n = 2u + w->entry_count * 12u + 4u;
    td_put16(hdr, (uint16_t)w->entry_count, w->big_endian);
    for (i = 0; i < w->entry_count; i++) {
      const td_wentry *e = &w->entries[i];
      uint8_t *ep = hdr + 2u + i * 12u;
      td_put16(ep, e->tag, w->big_endian);
      td_put16(ep + 2, e->type, w->big_endian);
      td_put32(ep + 4, (uint32_t)e->count, w->big_endian);
      if (e->ext_len > 0u) {
        size_t padded;
        uint64_t ext_off;
        if (!td_safe_add_size(e->ext_len, 1u, &padded) ||
            !td_safe_add_u64(extras_base, (uint64_t)ext_running, &ext_off) ||
            ext_off > UINT32_MAX) {
          td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_WRITE, 0, e->tag,
                       extras_base, "classic TIFF extra offset overflow");
          return TINYDNG_E_BOUNDS;
        }
        padded &= ~(size_t)1u;
        td_put32(ep + 8, (uint32_t)ext_off, w->big_endian);
        if (padded > SIZE_MAX - ext_running) {
          td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_WRITE, 0, e->tag,
                       extras_base, "IFD extra size overflow");
          return TINYDNG_E_BOUNDS;
        }
        ext_running += padded;
      } else {
        memcpy(ep + 8, e->value, 4);
      }
    }
    td_put32(hdr + 2u + w->entry_count * 12u, 0u, w->big_endian);
    if (sink->write(sink, ifd_off, hdr, n) != n) {
      return td_wio_err(sink, ifd_off, err);
    }
  }
  return TINYDNG_OK;
}

static tinydng_status td_writer_put_header(tinydng_write_io *sink, int be,
                                           int bigtiff, uint64_t ifd_off,
                                           tinydng_error *err) {
  if (bigtiff) {
    uint8_t hdr[16];
    hdr[0] = be ? 'M' : 'I';
    hdr[1] = be ? 'M' : 'I';
    td_put16(hdr + 2, 43u, be);          /* version 43              */
    td_put16(hdr + 4, 8u, be);           /* offset byte size = 8   */
    td_put16(hdr + 6, 0u, be);           /* reserved = 0            */
    td_put64(hdr + 8, ifd_off, be);
    if (sink->write(sink, 0, hdr, 16) != 16u) {
      return td_wio_err(sink, 0, err);
    }
  } else {
    uint8_t hdr[8];
    hdr[0] = be ? 'M' : 'I';
    hdr[1] = be ? 'M' : 'I';
    td_put16(hdr + 2, 42u, be);
    td_put32(hdr + 4, (uint32_t)ifd_off, be);
    if (sink->write(sink, 0, hdr, 8) != 8u) {
      return td_wio_err(sink, 0, err);
    }
  }
  return TINYDNG_OK;
}

static uint32_t td_wceil(uint32_t a, uint32_t b) {
  return (b == 0u) ? 0u : (a / b) + ((a % b) != 0u);
}

tinydng_status tinydng_writer_create(tinydng_context *ctx,
                                     tinydng_write_io sink,
                                     const tinydng_write_image *meta,
                                     const tinydng_write_options *opts,
                                     const tinydng_tiling *tiling,
                                     tinydng_writer **out,
                                     tinydng_error *err) {
  tinydng_writer *w;
  uint16_t spp, bps, sfmt, photo, comp;
  uint32_t across = 1u, down = 1u;
  uint32_t seg_count;
  size_t seg_bytes;
  int be;
  tinydng_status st;

  tinydng_error_clear(err);
  if (!ctx || !meta || !out || !sink.write) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "null argument to tinydng_writer_create");
    return TINYDNG_E_INVALID_ARG;
  }
  *out = NULL;
  spp = meta->samples_per_pixel ? meta->samples_per_pixel : 1u;
  bps = meta->bits_per_sample ? meta->bits_per_sample : 8u;
  sfmt = meta->sample_format ? meta->sample_format : TINYDNG_SAMPLEFORMAT_UINT;
  if (bps != 8u && bps != 16u && bps != 32u) {
    td_set_error(err, TINYDNG_E_UNSUPPORTED, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "writer supports 8/16/32-bit only (got %u)", (unsigned)bps);
    return TINYDNG_E_UNSUPPORTED;
  }
  if (meta->width == 0u || meta->height == 0u || spp == 0u) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "invalid image dimensions");
    return TINYDNG_E_INVALID_ARG;
  }
  if (spp > 16u) {
    td_set_error(err, TINYDNG_E_UNSUPPORTED, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "writer supports at most 16 samples per pixel (got %u)",
                 (unsigned)spp);
    return TINYDNG_E_UNSUPPORTED;
  }
  comp = opts ? opts->compression : 0u;
  if (comp == 0u || comp == TINYDNG_COMPRESSION_NONE) {
    comp = TINYDNG_COMPRESSION_NONE;
  } else if (comp == TINYDNG_COMPRESSION_LZW) {
    comp = TINYDNG_COMPRESSION_LZW;
  } else if (comp == TINYDNG_COMPRESSION_PACKBITS) {
    comp = TINYDNG_COMPRESSION_PACKBITS;
  } else if (comp == TINYDNG_COMPRESSION_NEW_JPEG ||
             comp == TINYDNG_COMPRESSION_OLD_JPEG) {
    if (bps != 16u) {
      td_set_error(err, TINYDNG_E_UNSUPPORTED, TINYDNG_STAGE_WRITE, 0, 0, 0,
                   "lossless JPEG writer requires 16-bit samples");
      return TINYDNG_E_UNSUPPORTED;
    }
    comp = TINYDNG_COMPRESSION_NEW_JPEG;
  } else {
    td_set_error(err, TINYDNG_E_UNSUPPORTED, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "unsupported writer compression %u", (unsigned)comp);
    return TINYDNG_E_UNSUPPORTED;
  }

  photo = meta->photometric;
  if (photo == 0u) {
    if (opts && opts->as_dng && meta->cfa && meta->cfa->present) {
      photo = TD_PHOTO_CFA;
    } else if (spp >= 3u) {
      photo = TD_PHOTO_RGB;
    } else {
      photo = TD_PHOTO_MINISBLACK;
    }
  }

  w = (tinydng_writer *)td_ctx_calloc(ctx, sizeof(*w), err);
  if (!w) {
    return TINYDNG_E_OOM;
  }
  w->ctx = ctx;
  w->sink = sink;
  be = opts ? (opts->big_endian != 0) : 0;
  w->big_endian = be;
  w->bigtiff = opts ? (opts->bigtiff != 0) : 0;
  w->width = meta->width;
  w->height = meta->height;
  w->spp = spp;
  w->bps = bps;
  w->sfmt = sfmt;
  w->photo = photo;
  w->compression = comp;

  if (tiling && tiling->tile_width > 0u && tiling->tile_length > 0u) {
    if (tiling->tile_width > 0xFFFFu || tiling->tile_length > 0xFFFFu) {
      td_set_error(err, TINYDNG_E_UNSUPPORTED, TINYDNG_STAGE_WRITE, 0, 0, 0,
                   "tile dims must be <= 65535");
      td_ctx_free(ctx, w);
      return TINYDNG_E_UNSUPPORTED;
    }
    w->tiled = 1;
    w->tile_width = tiling->tile_width;
    w->tile_length = tiling->tile_length;
    across = td_wceil(meta->width, tiling->tile_width);
    down = td_wceil(meta->height, tiling->tile_length);
  } else {
    uint32_t rps = (tiling && tiling->rows_per_strip > 0u)
                       ? tiling->rows_per_strip
                       : meta->height;
    w->rows_per_strip = rps;
    down = td_wceil(meta->height, rps);
  }
  if (comp == TINYDNG_COMPRESSION_NEW_JPEG &&
      ((uint64_t)meta->width > (uint64_t)INT_MAX ||
       (uint64_t)meta->height > (uint64_t)INT_MAX ||
       (uint64_t)meta->width * (uint64_t)spp > (uint64_t)INT_MAX)) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "lossless JPEG dimensions exceed encoder limits");
    td_ctx_free(ctx, w);
    return TINYDNG_E_BOUNDS;
  }
  if ((uint64_t)across * (uint64_t)down > 0xFFFFFFu) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "segment count too large (%u x %u)", across, down);
    td_ctx_free(ctx, w);
    return TINYDNG_E_BOUNDS;
  }
  seg_count = across * down;
  w->seg_count = seg_count;
  if (!td_safe_mul_size((size_t)seg_count, sizeof(td_writer_seg), &seg_bytes)) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "segment table size overflow");
    td_ctx_free(ctx, w);
    return TINYDNG_E_BOUNDS;
  }
  w->segs = (td_writer_seg *)td_ctx_calloc(ctx, seg_bytes, err);
  if (!w->segs) {
    td_ctx_free(ctx, w);
    return TINYDNG_E_OOM;
  }

  w->w.ctx = ctx;
  w->w.big_endian = be;
  w->w.bigtiff = w->bigtiff;
  w->w.extras_cap = w->bigtiff ? (4u * 1024u * 1024u) : TD_WMAX_EXTRAS;
  w->w.extras = (uint8_t *)td_ctx_alloc(ctx, w->w.extras_cap, err);
  if (!w->w.extras) {
    td_ctx_free(ctx, w->segs);
    td_ctx_free(ctx, w);
    return TINYDNG_E_OOM;
  }

  /* --- core TIFF tags --- */
  td_add_long(&w->w, TD_TAG_IMAGE_WIDTH, meta->width);
  td_add_long(&w->w, TD_TAG_IMAGE_LENGTH, meta->height);
  {
    uint16_t bpsv[16];
    uint16_t k;
    for (k = 0; k < spp && k < 16u; k++) {
      bpsv[k] = bps;
    }
    if (spp == 1u) {
      td_add_short(&w->w, TD_TAG_BITS_PER_SAMPLE, bps);
    } else {
      td_add_shorts(&w->w, TD_TAG_BITS_PER_SAMPLE, bpsv, spp);
    }
  }
  td_add_short(&w->w, TD_TAG_COMPRESSION, comp);
  if (comp == TINYDNG_COMPRESSION_NEW_JPEG) {
    /* The lossless-JPEG payload is always encoded with the horizontal
       (left) predictor; advertise it so external readers decode correctly. */
    td_add_short(&w->w, TD_TAG_PREDICTOR, 1u);
  }
  td_add_short(&w->w, TD_TAG_PHOTOMETRIC, photo);
  td_add_short(&w->w, TD_TAG_SAMPLES_PER_PIXEL, spp);
  {
    uint16_t sfv[16];
    uint16_t k;
    for (k = 0; k < spp && k < 16u; k++) {
      sfv[k] = sfmt;
    }
    if (spp == 1u) {
      td_add_short(&w->w, TD_TAG_SAMPLE_FORMAT, sfmt);
    } else {
      td_add_shorts(&w->w, TD_TAG_SAMPLE_FORMAT, sfv, spp);
    }
  }

  /* --- segment layout tags --- */
  if (w->tiled) {
    uint16_t off_type = w->bigtiff ? TD_TYPE_LONG8 : TD_TYPE_LONG;
    size_t elem_sz = w->bigtiff ? 8u : 4u;
    td_add_short(&w->w, TD_TAG_TILE_WIDTH, (uint16_t)tiling->tile_width);
    td_add_short(&w->w, TD_TAG_TILE_LENGTH, (uint16_t)tiling->tile_length);
    if (seg_count == 1u) {
      if (w->bigtiff) {
        td_add_long8(&w->w, TD_TAG_TILE_OFFSETS, 0);
        td_add_long8(&w->w, TD_TAG_TILE_BYTE_COUNTS, 0);
      } else {
        td_add_long(&w->w, TD_TAG_TILE_OFFSETS, 0);
        td_add_long(&w->w, TD_TAG_TILE_BYTE_COUNTS, 0);
      }
    } else {
      uint8_t *offs = td_reserve(&w->w, (size_t)seg_count * elem_sz);
      uint8_t *cnts = td_reserve(&w->w, (size_t)seg_count * elem_sz);
      td_add_reserved(&w->w, TD_TAG_TILE_OFFSETS, off_type, seg_count,
                      offs, (size_t)seg_count * elem_sz);
      td_add_reserved(&w->w, TD_TAG_TILE_BYTE_COUNTS, off_type, seg_count,
                      cnts, (size_t)seg_count * elem_sz);
    }
  } else {
    uint16_t off_type = w->bigtiff ? TD_TYPE_LONG8 : TD_TYPE_LONG;
    size_t elem_sz = w->bigtiff ? 8u : 4u;
    if (seg_count == 1u) {
      if (w->bigtiff) {
        td_add_long8(&w->w, TD_TAG_STRIP_OFFSETS, 0);
        td_add_long8(&w->w, TD_TAG_STRIP_BYTE_COUNTS, 0);
      } else {
        td_add_long(&w->w, TD_TAG_STRIP_OFFSETS, 0);
        td_add_long(&w->w, TD_TAG_STRIP_BYTE_COUNTS, 0);
      }
      td_add_long(&w->w, TD_TAG_ROWS_PER_STRIP, meta->height);
    } else {
      uint8_t *offs = td_reserve(&w->w, (size_t)seg_count * elem_sz);
      uint8_t *cnts = td_reserve(&w->w, (size_t)seg_count * elem_sz);
      td_add_reserved(&w->w, TD_TAG_STRIP_OFFSETS, off_type, seg_count,
                      offs, (size_t)seg_count * elem_sz);
      td_add_long(&w->w, TD_TAG_ROWS_PER_STRIP, w->rows_per_strip);
      td_add_reserved(&w->w, TD_TAG_STRIP_BYTE_COUNTS, off_type, seg_count,
                      cnts, (size_t)seg_count * elem_sz);
    }
  }

  /* --- optional DNG metadata --- */
  if (opts && opts->as_dng) {
    const tinydng_raw_info *raw = meta->raw;
    const tinydng_cfa *cfa = meta->cfa;
    td_add_short(&w->w, TD_TAG_NEW_SUBFILE_TYPE, 0);
    if (cfa && cfa->present) {
      uint16_t dim[2];
      dim[0] = cfa->pattern_dim[0] ? cfa->pattern_dim[0] : 2u;
      dim[1] = cfa->pattern_dim[1] ? cfa->pattern_dim[1] : 2u;
      td_add_shorts(&w->w, TD_TAG_CFA_REPEAT_PATTERN_DIM, dim, 2);
      /* Clamp counts to the fixed-size arrays in tinydng_cfa; a
       * inconsistently-initialized caller struct must not make the writer
       * read past the end of pattern[16] / plane_color[4]. */
      if (cfa->pattern_size) {
        uint32_t pn = cfa->pattern_size;
        if (pn > (uint32_t)sizeof(cfa->pattern)) {
          pn = (uint32_t)sizeof(cfa->pattern);
        }
        td_add_bytes(&w->w, TD_TAG_CFA_PATTERN, cfa->pattern, pn);
      }
      if (cfa->plane_color_count) {
        uint32_t cn = cfa->plane_color_count;
        if (cn > (uint32_t)sizeof(cfa->plane_color)) {
          cn = (uint32_t)sizeof(cfa->plane_color);
        }
        td_add_bytes(&w->w, TD_TAG_CFA_PLANE_COLOR, cfa->plane_color, cn);
      }
    }
    if (raw) {
      if (raw->has_dng_version) {
        td_add_bytes(&w->w, TD_TAG_DNG_VERSION, raw->dng_version, 4);
      } else {
        uint8_t ver[4] = {1u, 4u, 0u, 0u};
        td_add_bytes(&w->w, TD_TAG_DNG_VERSION, ver, 4);
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
        td_add_longs(&w->w, TD_TAG_BLACK_LEVEL, bl, n);
      }
      if (raw->white_level_present) {
        uint32_t wl = (uint32_t)(raw->white_level[0] < 0 ? 0
                                                         : raw->white_level[0]);
        td_add_long(&w->w, TD_TAG_WHITE_LEVEL, wl);
      }
      if (raw->color_matrix_present) {
        td_add_srationals(&w->w, TD_TAG_COLOR_MATRIX1, raw->color_matrix1, 9);
      }
      if (raw->has_as_shot_neutral) {
        td_add_rationals(&w->w, TD_TAG_AS_SHOT_NEUTRAL, raw->as_shot_neutral, 3);
      }
      if (raw->calibration_illuminant1) {
        td_add_short(&w->w, TD_TAG_CALIBRATION_ILLUMINANT1,
                     raw->calibration_illuminant1);
      }
    }
  }

  if (w->w.failed) {
    td_set_error(err, TINYDNG_E_INTERNAL, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "writer ran out of tag/extras capacity");
    td_ctx_free(ctx, w->w.extras);
    td_ctx_free(ctx, w->segs);
    td_ctx_free(ctx, w);
    return TINYDNG_E_INTERNAL;
  }

  /* Sort entries by tag (TIFF requires ascending). */
  qsort(w->w.entries, w->w.entry_count, sizeof(td_wentry), td_entry_cmp);

  /* Header placeholder; the IFD offset is patched at finish. */
  st = td_writer_put_header(&w->sink, be, w->bigtiff, 0, err);
  if (st != TINYDNG_OK) {
    td_ctx_free(ctx, w->w.extras);
    td_ctx_free(ctx, w->segs);
    td_ctx_free(ctx, w);
    return st;
  }
  /* Pre-allocate the LZW hash table if LZW compression is selected so it can
     be reused across strips/tiles without repeated alloc/free. */
  if (comp == TINYDNG_COMPRESSION_LZW) {
    w->lzw_tbl.keys = (uint32_t *)td_ctx_alloc(
        ctx, (size_t)TD_LZW_HASH_SIZE * sizeof(uint32_t), err);
    w->lzw_tbl.codes = (uint16_t *)td_ctx_alloc(
        ctx, (size_t)TD_LZW_HASH_SIZE * sizeof(uint16_t), err);
    if (!w->lzw_tbl.keys || !w->lzw_tbl.codes) {
      td_ctx_free(ctx, w->lzw_tbl.keys);
      td_ctx_free(ctx, w->lzw_tbl.codes);
      td_ctx_free(ctx, w->w.extras);
      td_ctx_free(ctx, w->segs);
      td_ctx_free(ctx, w);
      return TINYDNG_E_OOM;
    }
  }
  w->data_pos = w->bigtiff ? 16u : 8u;
  *out = w;
  return TINYDNG_OK;
}

static tinydng_status td_writer_put_segment(tinydng_writer *w, uint32_t index,
                                            const void *pixels, uint32_t pw,
                                            uint32_t ph, tinydng_error *err) {
  size_t len = 0;
  tinydng_status st;
  uint64_t at;
  if (!w || !pixels) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "null argument to writer segment write");
    return TINYDNG_E_INVALID_ARG;
  }
  if (w->finished) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "writer already finished");
    return TINYDNG_E_INVALID_ARG;
  }
  at = w->data_pos;
  if (index >= w->seg_count) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "segment index %u out of range (count %u)", index,
                 w->seg_count);
    return TINYDNG_E_INVALID_ARG;
  }
  if (w->segs[index].offset != 0u) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "segment %u written twice", index);
    return TINYDNG_E_INVALID_ARG;
  }
  st = td_writer_put_payload(w, (const uint8_t *)pixels, pw, ph, at, &len, err);
  if (st != TINYDNG_OK) {
    return st;
  }
  if (!w->bigtiff && (at > (uint64_t)UINT32_MAX || len > (size_t)UINT32_MAX)) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_WRITE, 0, 0, at,
                 "payload exceeds classic TIFF offset limits");
    return TINYDNG_E_BOUNDS;
  }
  w->segs[index].offset = at;
  w->segs[index].byte_count = (uint64_t)len;
  if (!td_safe_add_u64(at, (uint64_t)len, &w->data_pos)) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_WRITE, 0, 0, at,
                 "payload position overflow");
    return TINYDNG_E_BOUNDS;
  }
  if (w->data_pos & 1u) {
    uint8_t zero = 0;
    if (w->sink.write(&w->sink, w->data_pos, &zero, 1) != 1u) {
      return td_wio_err(&w->sink, w->data_pos, err);
    }
    w->data_pos++;
  }
  return TINYDNG_OK;
}

tinydng_status tinydng_writer_write_tile(tinydng_writer *w, uint32_t tile_index,
                                         const void *pixels,
                                         tinydng_error *err) {
  uint32_t across, pw, ph;
  uint64_t x, y;
  uint8_t *padded = NULL;
  const uint8_t *src = (const uint8_t *)pixels;
  tinydng_status st;
  if (!w) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "null writer");
    return TINYDNG_E_INVALID_ARG;
  }
  if (!w->tiled) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "writer is not tiled (use tinydng_writer_write_strip)");
    return TINYDNG_E_INVALID_ARG;
  }
  if (!pixels) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "null tile pixels");
    return TINYDNG_E_INVALID_ARG;
  }
  if (tile_index >= w->seg_count) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "tile index %u out of range", tile_index);
    return TINYDNG_E_INVALID_ARG;
  }
  across = td_wceil(w->width, w->tile_width);
  x = (uint64_t)(tile_index % across) * w->tile_width;
  y = (uint64_t)(tile_index / across) * w->tile_length;
  pw = w->tile_width;
  ph = w->tile_length;
  if (x + pw > (uint64_t)w->width) {
    pw = (uint32_t)((uint64_t)w->width - x);
  }
  if (y + ph > (uint64_t)w->height) {
    ph = (uint32_t)((uint64_t)w->height - y);
  }
  /* DNG requires every tile stream to carry the FULL tile dims (the loader
     decodes tile_width x tile_length and blits only the valid region), so
     edge tiles are padded to the full tile size before encoding. */
  if (pw != w->tile_width || ph != w->tile_length) {
    size_t sb = (size_t)w->bps / 8u;
    size_t full_row;
    size_t src_row;
    size_t pad_bytes;
    uint32_t r;
    if (!td_safe_mul_size((size_t)w->tile_width, (size_t)w->spp,
                          &full_row) ||
        !td_safe_mul_size(full_row, sb, &full_row) ||
        !td_safe_mul_size((size_t)pw, (size_t)w->spp, &src_row) ||
        !td_safe_mul_size(src_row, sb, &src_row) ||
        !td_safe_mul_size((size_t)w->tile_width, (size_t)w->tile_length,
                          &pad_bytes) ||
        !td_safe_mul_size(pad_bytes, (size_t)w->spp, &pad_bytes) ||
        !td_safe_mul_size(pad_bytes, sb, &pad_bytes)) {
      td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_WRITE, 0, 0, 0,
                   "padded tile size overflow");
      return TINYDNG_E_BOUNDS;
    }
    padded = (uint8_t *)td_ctx_calloc(w->ctx, pad_bytes, err);
    if (!padded) {
      return TINYDNG_E_OOM;
    }
    for (r = 0; r < ph; r++) {
      memcpy(padded + (size_t)r * full_row, src + (size_t)r * src_row,
             src_row);
    }
    src = padded;
  }
  st = td_writer_put_segment(w, tile_index, src, w->tile_width,
                             w->tile_length, err);
  if (padded) {
    td_ctx_free(w->ctx, padded);
  }
  return st;
}

tinydng_status tinydng_writer_write_strip(tinydng_writer *w,
                                          uint32_t strip_index,
                                          const void *pixels,
                                          tinydng_error *err) {
  uint64_t y;
  uint32_t ph;
  if (!w) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "null writer");
    return TINYDNG_E_INVALID_ARG;
  }
  if (w->tiled) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "writer is tiled (use tinydng_writer_write_tile)");
    return TINYDNG_E_INVALID_ARG;
  }
  if (strip_index >= w->seg_count) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "strip index %u out of range", strip_index);
    return TINYDNG_E_INVALID_ARG;
  }
  y = (uint64_t)strip_index * w->rows_per_strip;
  ph = w->rows_per_strip;
  if (y + ph > (uint64_t)w->height) {
    ph = (uint32_t)((uint64_t)w->height - y);
  }
  return td_writer_put_segment(w, strip_index, pixels, w->width, ph, err);
}

/* The extras buffer is laid out in entry-ADD order, but the IFD's running
   offsets walk the SORTED entries. After patching, rebuild the buffer in
   sorted-entry order (and fix the entry pointers) so the two agree. */
static tinydng_status td_writer_relayout_extras(tinydng_writer *w,
                                                tinydng_error *err) {
  size_t running = 0;
  size_t i;
  uint8_t *nb = (uint8_t *)td_ctx_alloc(w->ctx, w->w.extras_cap, err);
  if (!nb) {
    return TINYDNG_E_OOM;
  }
  for (i = 0; i < w->w.entry_count; i++) {
    td_wentry *e = &w->w.entries[i];
    if (e->ext_len > 0u) {
      size_t padded;
      if (!td_safe_add_size(e->ext_len, 1u, &padded) ||
          padded > w->w.extras_cap || running > w->w.extras_cap - padded) {
        td_ctx_free(w->ctx, nb);
        td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_WRITE, 0, e->tag, 0,
                     "IFD extras layout overflow");
        return TINYDNG_E_BOUNDS;
      }
      padded &= ~(size_t)1u;
      memcpy(nb + running, e->ext, e->ext_len);
      if (padded > e->ext_len) {
        nb[running + e->ext_len] = 0;
      }
      e->ext = nb + running;
      running += padded;
    }
  }
  td_ctx_free(w->ctx, w->w.extras);
  w->w.extras = nb;
  w->w.extras_len = running;
  return TINYDNG_OK;
}

tinydng_status tinydng_writer_finish(tinydng_writer *w, tinydng_error *err) {
  uint64_t extras_base, ifd_off;
  tinydng_status st = TINYDNG_OK;
  uint32_t i;

  tinydng_error_clear(err);
  if (!w) {
    return TINYDNG_E_INVALID_ARG;
  }
  if (w->finished) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "writer already finished");
    return TINYDNG_E_INVALID_ARG;
  }
  for (i = 0; i < w->seg_count; i++) {
    if (w->segs[i].offset == 0u) {
      td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_WRITE, 0, 0, 0,
                   "segment %u was never written", i);
      st = TINYDNG_E_INVALID_ARG;
      goto cleanup;
    }
  }
  td_writer_patch_segs(w);
  if (w->w.failed) {
    td_set_error(err, TINYDNG_E_INTERNAL, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "writer ran out of extras capacity");
    st = TINYDNG_E_INTERNAL;
    goto cleanup;
  }
  st = td_writer_relayout_extras(w, err);
  if (st != TINYDNG_OK) {
    goto cleanup;
  }

  extras_base = w->data_pos; /* already even-aligned */
  if (w->w.extras_len > 0u) {
    if (w->sink.write(&w->sink, extras_base, w->w.extras, w->w.extras_len) !=
        w->w.extras_len) {
      st = td_wio_err(&w->sink, extras_base, err);
      goto cleanup;
    }
  }
  if (!td_safe_add_u64(extras_base, (uint64_t)w->w.extras_len, &ifd_off)) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_WRITE, 0, 0,
                 extras_base, "writer IFD offset overflow");
    st = TINYDNG_E_BOUNDS;
    goto cleanup;
  }
  if (!w->bigtiff && ifd_off > (uint64_t)UINT32_MAX) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_WRITE, 0, 0, ifd_off,
                 "classic TIFF offset limit exceeded");
    st = TINYDNG_E_BOUNDS;
    goto cleanup;
  }
  st = td_writer_emit_ifd(&w->w, &w->sink, ifd_off, extras_base, err);
  if (st == TINYDNG_OK) {
    st = td_writer_put_header(&w->sink, w->big_endian, w->bigtiff, ifd_off, err);
  }

cleanup:
  td_ctx_free(w->ctx, w->w.extras);
  td_ctx_free(w->ctx, w->segs);
  td_ctx_free(w->ctx, w->lzw_tbl.keys);
  td_ctx_free(w->ctx, w->lzw_tbl.codes);
  w->finished = 1;
  td_ctx_free(w->ctx, w);
  return st;
}

/* Validate img->data_size against the geometry (parity with the
   pre-writer behavior). */
static tinydng_status td_write_check_data(const tinydng_write_image *img,
                                          tinydng_error *err) {
  uint16_t spp = img->samples_per_pixel ? img->samples_per_pixel : 1u;
  uint16_t bps = img->bits_per_sample ? img->bits_per_sample : 8u;
  size_t expected;
  if (!td_safe_mul_size((size_t)img->width, (size_t)img->height, &expected) ||
      !td_safe_mul_size(expected, (size_t)spp, &expected) ||
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
  return TINYDNG_OK;
}

tinydng_status tinydng_write_memory(tinydng_context *ctx,
                                    const tinydng_write_image *img,
                                    const tinydng_write_options *opts,
                                    uint8_t **out_data, size_t *out_size,
                                    tinydng_error *err) {
  tinydng_writer *w = NULL;
  tinydng_write_io io;
  tinydng_status st;

  tinydng_error_clear(err);
  if (!ctx || !img || !out_data || !out_size || !img->data) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "null argument to tinydng_write_memory");
    return TINYDNG_E_INVALID_ARG;
  }
  *out_data = NULL;
  *out_size = 0;
  st = td_write_check_data(img, err);
  if (st != TINYDNG_OK) {
    return st;
  }
  st = tinydng_write_io_open_memory(ctx, &io, err);
  if (st != TINYDNG_OK) {
    return st;
  }
  st = tinydng_writer_create(ctx, io, img, opts, NULL, &w, err);
  if (st != TINYDNG_OK) {
    io.close(&io);
    return st;
  }
  st = tinydng_writer_write_strip(w, 0, img->data, err);
  if (st != TINYDNG_OK) {
    tinydng_writer_finish(w, err); /* releases writer state */
    io.close(&io);
    return st;
  }
  st = tinydng_writer_finish(w, err);
  if (st != TINYDNG_OK) {
    io.close(&io);
    return st;
  }
  st = tinydng_write_io_memory_take(ctx, &io, out_data, out_size, err);
  io.close(&io);
  return st;
}

void tinydng_buffer_free(tinydng_context *ctx, uint8_t *buf) {
  td_ctx_free(ctx, buf);
}

tinydng_status tinydng_write_file(tinydng_context *ctx, const char *path,
                                  const tinydng_write_image *img,
                                  const tinydng_write_options *opts,
                                  tinydng_error *err) {
  tinydng_writer *w = NULL;
  tinydng_write_io io;
  tinydng_status st;

  tinydng_error_clear(err);
  if (!ctx || !path || !img || !img->data) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_WRITE, 0, 0, 0,
                 "null argument to tinydng_write_file");
    return TINYDNG_E_INVALID_ARG;
  }
  st = td_write_check_data(img, err);
  if (st != TINYDNG_OK) {
    return st;
  }
  st = tinydng_write_io_open_file(ctx, path, &io, err);
  if (st != TINYDNG_OK) {
    return st;
  }
  st = tinydng_writer_create(ctx, io, img, opts, NULL, &w, err);
  if (st != TINYDNG_OK) {
    io.close(&io);
    return st;
  }
  st = tinydng_writer_write_strip(w, 0, img->data, err);
  if (st != TINYDNG_OK) {
    tinydng_writer_finish(w, err); /* releases writer state */
    io.close(&io);
    return st;
  }
  st = tinydng_writer_finish(w, err);
  io.close(&io);
  return st;
}
