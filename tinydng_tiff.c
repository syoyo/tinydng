/*
 * tinydng_tiff.c - reader layer + TIFF/BigTIFF container parser.
 * SPDX-License-Identifier: MIT
 */
#include "td_internal.h"

/* ------------------------------------------------------------------ */
/* Reader scalars                                                     */
/* ------------------------------------------------------------------ */

int td_r_u16(const td_reader *r, uint64_t at, uint16_t *out) {
  uint8_t sc[2];
  const uint8_t *p;
  if (!r || !out) {
    return 0;
  }
  p = td_io_view(r->io, r->size, at, 2u, sc, sizeof(sc));
  if (!p) {
    return 0;
  }
  *out = r->big_endian ? (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1])
                       : (uint16_t)(((uint16_t)p[1] << 8) | (uint16_t)p[0]);
  return 1;
}

int td_r_u32(const td_reader *r, uint64_t at, uint32_t *out) {
  uint8_t sc[4];
  const uint8_t *p;
  if (!r || !out) {
    return 0;
  }
  p = td_io_view(r->io, r->size, at, 4u, sc, sizeof(sc));
  if (!p) {
    return 0;
  }
  if (r->big_endian) {
    *out = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
  } else {
    *out = ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[1] << 8) | (uint32_t)p[0];
  }
  return 1;
}

int td_r_u64(const td_reader *r, uint64_t at, uint64_t *out) {
  uint8_t sc[8];
  const uint8_t *p;
  int i;
  uint64_t v = 0;
  if (!r || !out) {
    return 0;
  }
  p = td_io_view(r->io, r->size, at, 8u, sc, sizeof(sc));
  if (!p) {
    return 0;
  }
  if (r->big_endian) {
    for (i = 0; i < 8; i++) {
      v = (v << 8) | (uint64_t)p[i];
    }
  } else {
    for (i = 7; i >= 0; i--) {
      v = (v << 8) | (uint64_t)p[i];
    }
  }
  *out = v;
  return 1;
}

int td_r_i32(const td_reader *r, uint64_t at, int32_t *out) {
  uint32_t u;
  if (!td_r_u32(r, at, &u)) {
    return 0;
  }
  *out = (int32_t)u;
  return 1;
}

/* ------------------------------------------------------------------ */
/* Buffer-based value reads (for bulk-parsed regions)                 */
/* ------------------------------------------------------------------ */

static uint16_t td_u16_from_buf(const uint8_t *p, int big_endian) {
  return big_endian ? (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1])
                    : (uint16_t)(((uint16_t)p[1] << 8) | (uint16_t)p[0]);
}

static uint32_t td_u32_from_buf(const uint8_t *p, int big_endian) {
  if (big_endian) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
  }
  return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[1] << 8) | (uint32_t)p[0];
}

static uint64_t td_u64_from_buf(const uint8_t *p, int big_endian) {
  uint64_t v = 0;
  int i;
  if (big_endian) {
    for (i = 0; i < 8; i++) {
      v = (v << 8) | (uint64_t)p[i];
    }
  } else {
    for (i = 7; i >= 0; i--) {
      v = (v << 8) | (uint64_t)p[i];
    }
  }
  return v;
}

/* Widen a TIFF-typed element at `p` (file byte order) to u64, mirroring
   td_r_val_uint's semantics for the unsigned array types. */
static int td_val_uint_from_buf(const uint8_t *p, uint16_t type,
                                int big_endian, uint64_t *out) {
  switch (type) {
    case TD_TYPE_BYTE:
    case TD_TYPE_ASCII:
    case TD_TYPE_UNDEFINED:
      *out = (uint64_t)p[0];
      return 1;
    case TD_TYPE_SHORT:
      *out = (uint64_t)td_u16_from_buf(p, big_endian);
      return 1;
    case TD_TYPE_LONG:
    case TD_TYPE_IFD:
      *out = (uint64_t)td_u32_from_buf(p, big_endian);
      return 1;
    case TD_TYPE_LONG8:
    case TD_TYPE_IFD8:
      *out = td_u64_from_buf(p, big_endian);
      return 1;
    default:
      return 0;
  }
}

/* ------------------------------------------------------------------ */
/* TIFF type sizes + typed value reads                                */
/* ------------------------------------------------------------------ */

size_t td_tiff_type_size(uint16_t type) {
  switch (type) {
    case TD_TYPE_BYTE:
    case TD_TYPE_ASCII:
    case TD_TYPE_SBYTE:
    case TD_TYPE_UNDEFINED:
      return 1;
    case TD_TYPE_SHORT:
    case TD_TYPE_SSHORT:
      return 2;
    case TD_TYPE_LONG:
    case TD_TYPE_SLONG:
    case TD_TYPE_FLOAT:
    case TD_TYPE_IFD:
      return 4;
    case TD_TYPE_RATIONAL:
    case TD_TYPE_SRATIONAL:
    case TD_TYPE_DOUBLE:
    case TD_TYPE_LONG8:
    case TD_TYPE_SLONG8:
    case TD_TYPE_IFD8:
      return 8;
    default:
      return 0;
  }
}

int td_r_val_uint(const td_reader *r, uint16_t type, uint64_t at,
                  uint64_t *out) {
  switch (type) {
    case TD_TYPE_BYTE:
    case TD_TYPE_ASCII:
    case TD_TYPE_UNDEFINED: {
      uint8_t sc[1];
      const uint8_t *p = td_io_view(r->io, r->size, at, 1u, sc, sizeof(sc));
      if (!p) {
        return 0;
      }
      *out = (uint64_t)p[0];
      return 1;
    }
    case TD_TYPE_SHORT: {
      uint16_t v;
      if (!td_r_u16(r, at, &v)) {
        return 0;
      }
      *out = (uint64_t)v;
      return 1;
    }
    case TD_TYPE_LONG:
    case TD_TYPE_IFD: {
      uint32_t v;
      if (!td_r_u32(r, at, &v)) {
        return 0;
      }
      *out = (uint64_t)v;
      return 1;
    }
    case TD_TYPE_LONG8:
    case TD_TYPE_IFD8:
      return td_r_u64(r, at, out);
    default:
      return 0;
  }
}

int td_r_val_int(const td_reader *r, uint16_t type, uint64_t at, int64_t *out) {
  switch (type) {
    case TD_TYPE_SBYTE: {
      uint8_t sc[1];
      const uint8_t *p = td_io_view(r->io, r->size, at, 1u, sc, sizeof(sc));
      if (!p) {
        return 0;
      }
      *out = (int64_t)(int8_t)p[0];
      return 1;
    }
    case TD_TYPE_SSHORT: {
      uint16_t v;
      if (!td_r_u16(r, at, &v)) {
        return 0;
      }
      *out = (int64_t)(int16_t)v;
      return 1;
    }
    case TD_TYPE_SLONG: {
      int32_t v;
      if (!td_r_i32(r, at, &v)) {
        return 0;
      }
      *out = (int64_t)v;
      return 1;
    }
    case TD_TYPE_SLONG8: {
      uint64_t v;
      if (!td_r_u64(r, at, &v)) {
        return 0;
      }
      *out = (int64_t)v;
      return 1;
    }
    default: {
      /* Fall back to unsigned widening for unsigned types. */
      uint64_t u;
      if (td_r_val_uint(r, type, at, &u)) {
        *out = (int64_t)u;
        return 1;
      }
      return 0;
    }
  }
}

int td_r_val_real(const td_reader *r, uint16_t type, uint64_t at, double *out) {
  switch (type) {
    case TD_TYPE_RATIONAL: {
      uint32_t num, den;
      if (!td_r_u32(r, at, &num) || !td_r_u32(r, at + 4u, &den)) {
        return 0;
      }
      *out = (den == 0u) ? 0.0 : ((double)num / (double)den);
      return 1;
    }
    case TD_TYPE_SRATIONAL: {
      int32_t num, den;
      if (!td_r_i32(r, at, &num) || !td_r_i32(r, at + 4u, &den)) {
        return 0;
      }
      *out = (den == 0) ? 0.0 : ((double)num / (double)den);
      return 1;
    }
    case TD_TYPE_FLOAT: {
      uint32_t u;
      float f;
      if (!td_r_u32(r, at, &u)) {
        return 0;
      }
      memcpy(&f, &u, sizeof(f));
      *out = (double)f;
      return 1;
    }
    case TD_TYPE_DOUBLE: {
      uint64_t u;
      double d;
      if (!td_r_u64(r, at, &u)) {
        return 0;
      }
      memcpy(&d, &u, sizeof(d));
      *out = d;
      return 1;
    }
    default: {
      /* Allow integer types to widen to real (e.g. SHORT matrices). */
      int64_t s;
      if (td_r_val_int(r, type, at, &s)) {
        *out = (double)s;
        return 1;
      }
      return 0;
    }
  }
}

/* ------------------------------------------------------------------ */
/* IFD entry decoding                                                 */
/* ------------------------------------------------------------------ */

typedef struct td_entry {
  uint16_t tag;
  uint16_t type;
  uint64_t count;
  size_t type_size;
  uint64_t data_off; /* absolute offset of element 0 */
} td_entry;

/* Decode one IFD entry row (12 bytes classic / 20 bytes BigTIFF, in file
   byte order) into `out`. `entry_pos` is the absolute offset of the row
   (used for the inline value field). */
static int td_parse_entry_row(const td_reader *r, const uint8_t *row,
                              uint64_t entry_pos, td_entry *out) {
  uint64_t data_bytes;
  uint64_t inline_cap;
  out->tag = td_u16_from_buf(row, r->big_endian);
  out->type = td_u16_from_buf(row + 2u, r->big_endian);
  if (r->bigtiff) {
    out->count = td_u64_from_buf(row + 4u, r->big_endian);
    inline_cap = 8u;
  } else {
    out->count = (uint64_t)td_u32_from_buf(row + 4u, r->big_endian);
    inline_cap = 4u;
  }
  out->type_size = td_tiff_type_size(out->type);
  if (out->type_size == 0u) {
    out->data_off = entry_pos + (r->bigtiff ? 12u : 8u); /* caller skips */
    return 1;
  }
  if (!td_safe_mul_u64((uint64_t)out->type_size, out->count, &data_bytes)) {
    return 0;
  }
  if (data_bytes <= inline_cap) {
    out->data_off = entry_pos + (r->bigtiff ? 12u : 8u);
  } else if (r->bigtiff) {
    out->data_off = td_u64_from_buf(row + 12u, r->big_endian);
  } else {
    out->data_off = (uint64_t)td_u32_from_buf(row + 8u, r->big_endian);
  }
  return 1;
}

/* Read the whole IFD entry table [entries_start, + n*stride + next-pointer
   bytes) in ONE IO call, zero-copy when the backend can map
   it. When it can't, allocates `*owned` (caller must free). Returns NULL on
   overflow / out-of-range, with `err` unset (caller reports). */
static const uint8_t *td_entries_bulk(tinydng_context *ctx, const td_reader *r,
                                      uint64_t entries_start, uint64_t n,
                                      uint64_t stride, uint8_t **owned,
                                      tinydng_error *err) {
  uint64_t table_len;
  uint64_t next_ptr_size = r->bigtiff ? 8u : 4u;
  const uint8_t *p;
  *owned = NULL;
  if (!td_safe_mul_u64(n, stride, &table_len) ||
      !td_safe_add_u64(table_len, next_ptr_size, &table_len) ||
      table_len > (uint64_t)SIZE_MAX) {
    return NULL;
  }
  if (table_len > r->size || entries_start > r->size - table_len) {
    return NULL;
  }
  p = td_io_view(r->io, r->size, entries_start, (size_t)table_len, NULL, 0);
  if (p) {
    return p;
  }
  {
    uint8_t *buf = (uint8_t *)td_ctx_alloc(ctx, (size_t)table_len, err);
    if (!buf) {
      return NULL;
    }
    p = td_io_view(r->io, r->size, entries_start, (size_t)table_len, buf,
                   (size_t)table_len);
    if (!p) {
      td_ctx_free(ctx, buf);
      return NULL;
    }
    *owned = buf;
    return p;
  }
}

/* Read a uint array (offsets/counts/sub-ifds) into a context-owned u64[]. */
static int td_read_u64_array(tinydng_context *ctx, const td_reader *r,
                             const td_entry *e, size_t max_count,
                             uint64_t **out_arr, size_t *out_count,
                             uint32_t ifd_index, tinydng_error *err) {
  uint64_t *arr;
  size_t i, n, bytes;
  n = (size_t)e->count;
  if (e->count > (uint64_t)max_count) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_IFD, ifd_index, e->tag, 0,
                 "array count %llu exceeds cap %zu",
                 (unsigned long long)e->count, max_count);
    return 0;
  }
  if (n == 0u) {
    *out_arr = NULL;
    *out_count = 0;
    return 1;
  }
  /* The array's source range [data_off, data_off + count*type_size) must fit in
   * the file. This ties the up-front u64[] allocation to the input size (a tiny
   * file can't declare a huge count to force a large allocation) and, computed
   * overflow-safe, also rejects a 64-bit data_off/stride that would wrap. */
  {
    uint64_t span, last;
    if (e->type_size == 0u ||
        !td_safe_mul_u64(e->count, (uint64_t)e->type_size, &span) ||
        !td_safe_add_u64(e->data_off, span, &last) || last > r->size) {
      td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_IFD, ifd_index, e->tag, 0,
                   "array range out of file bounds (count %llu)",
                   (unsigned long long)e->count);
      return 0;
    }
  }
  if (!td_safe_mul_size(n, sizeof(uint64_t), &bytes)) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_IFD, ifd_index, e->tag, 0,
                 "array size overflow");
    return 0;
  }
  arr = (uint64_t *)td_ctx_alloc(ctx, bytes, err);
  if (!arr) {
    return 0;
  }
  /* Bulk-read the whole array in one IO call instead of one read per
     element: on the stdio backend each per-element read is a seek+fread. */
  {
    uint8_t *scratch = NULL;
    const uint8_t *src;
    size_t span = (size_t)e->count * e->type_size;
    src = td_io_view(r->io, r->size, e->data_off, span, NULL, 0);
    if (!src) {
      scratch = (uint8_t *)td_ctx_alloc(ctx, span, err);
      if (!scratch) {
        td_ctx_free(ctx, arr);
        return 0;
      }
      src = td_io_view(r->io, r->size, e->data_off, span, scratch, span);
    }
    if (!src) {
      td_ctx_free(ctx, scratch);
      td_ctx_free(ctx, arr);
      td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_IFD, ifd_index, e->tag,
                   e->data_off, "failed reading array range (count %llu)",
                   (unsigned long long)e->count);
      return 0;
    }
    for (i = 0; i < n; i++) {
      uint64_t v;
      if (!td_val_uint_from_buf(src + (size_t)i * e->type_size, e->type,
                                r->big_endian, &v)) {
        td_ctx_free(ctx, scratch);
        td_ctx_free(ctx, arr);
        td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_IFD, ifd_index,
                     e->tag, e->data_off, "failed reading array element %zu",
                     i);
        return 0;
      }
      arr[i] = v;
    }
    td_ctx_free(ctx, scratch);
  }
  *out_arr = arr;
  *out_count = n;
  return 1;
}

/* Double a context-owned array with overflow-safe sizing. Returns the new
 * pointer (and updates *cap) or NULL on overflow/OOM; on failure the old block
 * is left intact for the caller to free. */
static void *td_grow_array(tinydng_context *ctx, void *ptr, size_t *cap,
                           size_t elem, tinydng_error *err) {
  size_t oldc = *cap, newc, ob, nb;
  void *np;
  if (oldc > (SIZE_MAX / 2u)) {
    return NULL;
  }
  newc = oldc ? (oldc * 2u) : 16u;
  if (!td_safe_mul_size(oldc, elem, &ob) || !td_safe_mul_size(newc, elem, &nb)) {
    return NULL;
  }
  np = td_ctx_realloc(ctx, ptr, ob, nb, err);
  if (np) {
    *cap = newc;
  }
  return np;
}

static int td_read_scalar_uint(const td_reader *r, const td_entry *e,
                               uint32_t *out) {
  uint64_t v;
  if (e->count < 1u || e->type_size == 0u) {
    return 0;
  }
  if (!td_r_val_uint(r, e->type, e->data_off, &v)) {
    return 0;
  }
  *out = (uint32_t)v;
  return 1;
}

/* ------------------------------------------------------------------ */
/* IFD parse                                                          */
/* ------------------------------------------------------------------ */

/* Parse a metadata-only sub-IFD (e.g. EXIF IFD 34665): route every entry
   through the DNG/EXIF metadata handler. Best-effort; never recurses. */
static void td_parse_metadata_ifd(tinydng_context *ctx, const td_reader *r,
                                  uint64_t off, tinydng_image_info *img,
                                  uint32_t ifd_index, tinydng_error *err) {
  uint64_t num_entries = 0;
  uint64_t entries_start;
  uint64_t entry_stride = r->bigtiff ? 20u : 12u;
  uint64_t i;

  if (off == 0u || off >= r->size) {
    return;
  }
  if (r->bigtiff) {
    if (!td_r_u64(r, off, &num_entries)) {
      return;
    }
    entries_start = off + 8u;
  } else {
    uint16_t n;
    if (!td_r_u16(r, off, &n)) {
      return;
    }
    num_entries = (uint64_t)n;
    entries_start = off + 2u;
  }
  if (num_entries > (uint64_t)ctx->max_ifd_entries) {
    return;
  }
  {
    uint8_t *owned = NULL;
    const uint8_t *tbl = td_entries_bulk(ctx, r, entries_start, num_entries,
                                         entry_stride, &owned, err);
    if (!tbl) {
      return;
    }
    for (i = 0; i < num_entries; i++) {
      td_entry e;
      uint64_t pos = entries_start + i * entry_stride;
      if (!td_parse_entry_row(r, tbl + i * entry_stride, pos, &e) ||
          e.type_size == 0u) {
        continue;
      }
      /* Don't follow nested IFD pointers from here (avoid recursion). */
      if (e.tag == TD_TAG_EXIF_IFD || e.tag == TD_TAG_SUB_IFDS) {
        continue;
      }
      (void)td_dng_handle_tag(ctx, r, img, ifd_index, e.tag, e.type, e.count,
                              e.data_off, err);
    }
    td_ctx_free(ctx, owned);
  }
}

static void td_free_build(tinydng_context *ctx, td_ifd_build *b) {
  td_ctx_free(ctx, b->strip_offsets);
  td_ctx_free(ctx, b->strip_byte_counts);
  td_ctx_free(ctx, b->tile_offsets);
  td_ctx_free(ctx, b->tile_byte_counts);
  td_ctx_free(ctx, b->sub_ifds);
  memset(b, 0, sizeof(*b));
}

/* Parse one IFD. Fills `img` metadata + geometry into `b`. Returns status. */
static tinydng_status td_parse_ifd(tinydng_context *ctx, const td_reader *r,
                                   uint64_t ifd_off, uint32_t ifd_index,
                                   tinydng_image_info *img, td_ifd_build *b,
                                   uint64_t *next_ifd_out, tinydng_error *err) {
  uint64_t num_entries = 0;
  uint64_t entries_start;
  uint64_t entry_stride = r->bigtiff ? 20u : 12u;
  uint64_t i;

  memset(b, 0, sizeof(*b));
  b->samples_per_pixel = 1;
  b->bits_per_sample = 1;
  b->compression = TINYDNG_COMPRESSION_NONE;
  b->planar_configuration = 1;
  b->predictor = 1;
  b->sample_format = TINYDNG_SAMPLEFORMAT_UINT;
  *next_ifd_out = 0;

  if (r->bigtiff) {
    if (!td_r_u64(r, ifd_off, &num_entries)) {
      td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_IFD, ifd_index, 0,
                   ifd_off, "failed reading IFD entry count");
      return TINYDNG_E_BOUNDS;
    }
    entries_start = ifd_off + 8u;
  } else {
    uint16_t n;
    if (!td_r_u16(r, ifd_off, &n)) {
      td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_IFD, ifd_index, 0,
                   ifd_off, "failed reading IFD entry count");
      return TINYDNG_E_BOUNDS;
    }
    num_entries = (uint64_t)n;
    entries_start = ifd_off + 2u;
  }

  if (num_entries > (uint64_t)ctx->max_ifd_entries) {
    td_set_error(err, TINYDNG_E_UNSUPPORTED, TINYDNG_STAGE_IFD, ifd_index, 0,
                 ifd_off, "too many IFD entries (%llu > %u)",
                 (unsigned long long)num_entries, ctx->max_ifd_entries);
    return TINYDNG_E_UNSUPPORTED;
  }

  /* Read the whole entry table (+ next-IFD pointer) in one IO call: on the
     stdio backend this replaces 3 seeks+freads per entry. */
  {
    uint8_t *owned = NULL;
    const uint8_t *tbl = td_entries_bulk(ctx, r, entries_start, num_entries,
                                         entry_stride, &owned, err);
    if (!tbl) {
      td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_IFD, ifd_index, 0,
                   entries_start, "IFD entry table out of range");
      return TINYDNG_E_BOUNDS;
    }

    for (i = 0; i < num_entries; i++) {
      td_entry e;
      uint64_t pos = entries_start + i * entry_stride;
      uint32_t sv;
      if (!td_parse_entry_row(r, tbl + i * entry_stride, pos, &e)) {
        td_ctx_free(ctx, owned);
        td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_IFD, ifd_index, 0, pos,
                     "failed reading IFD entry %llu", (unsigned long long)i);
        return TINYDNG_E_BOUNDS;
      }
      if (e.type_size == 0u) {
        continue; /* unknown type: skip defensively */
      }
      switch (e.tag) {
      case TD_TAG_IMAGE_WIDTH:
        if (td_read_scalar_uint(r, &e, &b->width)) {
          b->has_width = 1;
        }
        break;
      case TD_TAG_IMAGE_LENGTH:
        if (td_read_scalar_uint(r, &e, &b->height)) {
          b->has_height = 1;
        }
        break;
      case TD_TAG_BITS_PER_SAMPLE:
        if (td_read_scalar_uint(r, &e, &sv)) {
          b->bits_per_sample = (uint16_t)sv;
          b->has_bps = 1;
        }
        break;
      case TD_TAG_SAMPLES_PER_PIXEL:
        if (td_read_scalar_uint(r, &e, &sv)) {
          b->samples_per_pixel = (uint16_t)sv;
          b->has_spp = 1;
        }
        break;
      case TD_TAG_COMPRESSION:
        if (td_read_scalar_uint(r, &e, &sv)) {
          b->compression = (uint16_t)sv;
          b->has_compression = 1;
        }
        break;
      case TD_TAG_PHOTOMETRIC:
        if (td_read_scalar_uint(r, &e, &sv)) {
          b->photometric = (uint16_t)sv;
        }
        break;
      case TD_TAG_PLANAR_CONFIGURATION:
        if (td_read_scalar_uint(r, &e, &sv)) {
          b->planar_configuration = (uint16_t)sv;
        }
        break;
      case TD_TAG_PREDICTOR:
        if (td_read_scalar_uint(r, &e, &sv)) {
          b->predictor = (uint16_t)sv;
        }
        break;
      case TD_TAG_SAMPLE_FORMAT:
        if (td_read_scalar_uint(r, &e, &sv)) {
          b->sample_format = (uint16_t)sv;
        }
        break;
      case TD_TAG_ROWS_PER_STRIP:
        if (td_read_scalar_uint(r, &e, &b->rows_per_strip)) {
          b->has_rows_per_strip = 1;
        }
        break;
      case TD_TAG_TILE_WIDTH:
        if (td_read_scalar_uint(r, &e, &b->tile_width)) {
          b->has_tile_width = 1;
        }
        break;
      case TD_TAG_TILE_LENGTH:
        if (td_read_scalar_uint(r, &e, &b->tile_length)) {
          b->has_tile_length = 1;
        }
        break;
      case TD_TAG_JPEG_IF_OFFSET:
        (void)td_read_scalar_uint(r, &e, &b->jpeg_if_offset);
        break;
      case TD_TAG_JPEG_IF_BYTE_COUNT:
        (void)td_read_scalar_uint(r, &e, &b->jpeg_if_byte_count);
        break;
      case TD_TAG_NEW_SUBFILE_TYPE:
        (void)td_read_scalar_uint(r, &e, &b->new_subfile_type);
        break;
      case TD_TAG_EXIF_IFD: {
        uint32_t exif_off;
        if (td_read_scalar_uint(r, &e, &exif_off)) {
          td_parse_metadata_ifd(ctx, r, (uint64_t)exif_off, img, ifd_index, err);
        }
        break;
      }
      case TD_TAG_STRIP_OFFSETS:
        if (!td_read_u64_array(ctx, r, &e, 1u << 24, &b->strip_offsets,
                               &b->strip_offset_count, ifd_index, err)) {
          td_ctx_free(ctx, owned);
          return err->status;
        }
        break;
      case TD_TAG_STRIP_BYTE_COUNTS:
        if (!td_read_u64_array(ctx, r, &e, 1u << 24, &b->strip_byte_counts,
                               &b->strip_byte_count_count, ifd_index, err)) {
          td_ctx_free(ctx, owned);
          return err->status;
        }
        break;
      case TD_TAG_TILE_OFFSETS:
        if (!td_read_u64_array(ctx, r, &e, 1u << 24, &b->tile_offsets,
                               &b->tile_offset_count, ifd_index, err)) {
          td_ctx_free(ctx, owned);
          return err->status;
        }
        break;
      case TD_TAG_TILE_BYTE_COUNTS:
        if (!td_read_u64_array(ctx, r, &e, 1u << 24, &b->tile_byte_counts,
                               &b->tile_byte_count_count, ifd_index, err)) {
          td_ctx_free(ctx, owned);
          return err->status;
        }
        break;
      case TD_TAG_SUB_IFDS:
        if (!td_read_u64_array(ctx, r, &e, 4096u, &b->sub_ifds,
                               &b->sub_ifd_count, ifd_index, err)) {
          td_ctx_free(ctx, owned);
          return err->status;
        }
        break;
      default:
        if (!td_dng_handle_tag(ctx, r, img, ifd_index, e.tag, e.type, e.count,
                               e.data_off, err)) {
          td_ctx_free(ctx, owned);
          return err->status ? err->status : TINYDNG_E_PARSE;
        }
        break;
      }
    }

    /* Next-IFD pointer follows the entries (part of the bulk read). */
    {
      const uint8_t *np = tbl + num_entries * entry_stride;
      if (r->bigtiff) {
        *next_ifd_out = td_u64_from_buf(np, r->big_endian);
      } else {
        *next_ifd_out = (uint64_t)td_u32_from_buf(np, r->big_endian);
      }
    }
    td_ctx_free(ctx, owned);
  }
  return TINYDNG_OK;
}

/* ------------------------------------------------------------------ */
/* Geometry validation + segment table                                */
/* ------------------------------------------------------------------ */

static uint64_t td_ceil_div_u64(uint64_t a, uint64_t b) {
  return (a + b - 1u) / b;
}

static tinydng_status td_build_segments(tinydng_context *ctx, const td_reader *r,
                                        td_ifd_build *b, uint32_t ifd_index,
                                        tinydng_image_info *img,
                                        tinydng_error *err) {
  tinydng_segment *segs = NULL;
  size_t count = 0;
  size_t i;
  uint64_t pixel_count = 0;
  int tiled;

  /* --- geometry sanity --- */
  if (!b->has_width || !b->has_height || b->width == 0u || b->height == 0u) {
    td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_GEOMETRY, ifd_index,
                 TD_TAG_IMAGE_WIDTH, 0, "missing or zero image dimensions");
    return TINYDNG_E_PARSE;
  }
  if (b->samples_per_pixel == 0u || b->samples_per_pixel > 16u) {
    td_set_error(err, TINYDNG_E_UNSUPPORTED, TINYDNG_STAGE_GEOMETRY, ifd_index,
                 TD_TAG_SAMPLES_PER_PIXEL, 0, "unsupported samples_per_pixel=%u",
                 (unsigned)b->samples_per_pixel);
    return TINYDNG_E_UNSUPPORTED;
  }
  if (!td_safe_mul_u64((uint64_t)b->width, (uint64_t)b->height,
                       &pixel_count) ||
      (ctx->max_image_pixels && pixel_count > ctx->max_image_pixels)) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_GEOMETRY, ifd_index, 0, 0,
                 "image too large: %ux%u (cap %llu px)", b->width, b->height,
                 (unsigned long long)ctx->max_image_pixels);
    return TINYDNG_E_BOUNDS;
  }

  tiled = (b->has_tile_width && b->has_tile_length && b->tile_width > 0u &&
           b->tile_length > 0u && b->tile_offsets != NULL);

  if (tiled) {
    uint64_t across = td_ceil_div_u64(b->width, b->tile_width);
    uint64_t down = td_ceil_div_u64(b->height, b->tile_length);
    uint64_t per_plane = across * down;
    uint64_t expected = per_plane;
    if (b->planar_configuration == 2u) {
      expected = per_plane * (uint64_t)b->samples_per_pixel;
    }
    if (b->tile_offset_count != b->tile_byte_count_count ||
        (uint64_t)b->tile_offset_count != expected) {
      td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_GEOMETRY, ifd_index,
                   TD_TAG_TILE_OFFSETS, 0,
                   "tile count mismatch: offsets=%zu counts=%zu expected=%llu",
                   b->tile_offset_count, b->tile_byte_count_count,
                   (unsigned long long)expected);
      return TINYDNG_E_PARSE;
    }
    count = b->tile_offset_count;
  } else {
    if (!b->has_rows_per_strip || b->rows_per_strip == 0u) {
      b->rows_per_strip = b->height;
    }
    if (b->strip_offset_count == 0u && b->jpeg_if_offset != 0u) {
      count = 1; /* JPEGInterchangeFormat single segment */
    } else {
      uint64_t strips_per_plane =
          td_ceil_div_u64(b->height, b->rows_per_strip);
      uint64_t expected = strips_per_plane;
      if (b->planar_configuration == 2u) {
        expected = strips_per_plane * (uint64_t)b->samples_per_pixel;
      }
      if (b->strip_offset_count != b->strip_byte_count_count ||
          b->strip_offset_count == 0u ||
          (uint64_t)b->strip_offset_count != expected) {
        td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_GEOMETRY, ifd_index,
                     TD_TAG_STRIP_OFFSETS, 0,
                     "strip count mismatch: offsets=%zu counts=%zu expected=%llu",
                     b->strip_offset_count, b->strip_byte_count_count,
                     (unsigned long long)expected);
        return TINYDNG_E_PARSE;
      }
      count = b->strip_offset_count;
    }
  }

  {
    size_t seg_bytes;
    if (!td_safe_mul_size(count, sizeof(tinydng_segment), &seg_bytes)) {
      td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_GEOMETRY, ifd_index, 0,
                   0, "segment table size overflow");
      return TINYDNG_E_BOUNDS;
    }
    segs = (tinydng_segment *)td_ctx_calloc(ctx, seg_bytes, err);
    if (!segs) {
      return TINYDNG_E_OOM;
    }
  }

  if (tiled) {
    uint64_t across = td_ceil_div_u64(b->width, b->tile_width);
    uint64_t down = td_ceil_div_u64(b->height, b->tile_length);
    uint64_t per_plane = across * down;
    for (i = 0; i < count; i++) {
      uint64_t local = (per_plane > 0u) ? ((uint64_t)i % per_plane) : 0u;
      uint64_t col = (across > 0u) ? (local % across) : 0u;
      uint64_t row = (across > 0u) ? (local / across) : 0u;
      uint64_t x = col * b->tile_width;
      uint64_t y = row * b->tile_length;
      uint64_t off = b->tile_offsets[i];
      uint64_t bc = b->tile_byte_counts[i];
      uint64_t w = b->tile_width;
      uint64_t h = b->tile_length;
      if (x < b->width && (b->width - x) < w) {
        w = b->width - x;
      }
      if (y < b->height && (b->height - y) < h) {
        h = b->height - y;
      }
      if (off > r->size || bc > (r->size - off)) {
        td_ctx_free(ctx, segs);
        td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_GEOMETRY, ifd_index,
                     TD_TAG_TILE_OFFSETS, off,
                     "tile[%zu] out of range off=%llu bc=%llu size=%llu", i,
                     (unsigned long long)off, (unsigned long long)bc,
                     (unsigned long long)r->size);
        return TINYDNG_E_BOUNDS;
      }
      segs[i].offset = off;
      segs[i].byte_count = bc;
      segs[i].index = (uint32_t)i;
      segs[i].kind = TINYDNG_SEG_TILE;
      segs[i].x = (uint32_t)x;
      segs[i].y = (uint32_t)y;
      segs[i].w = (uint32_t)w;
      segs[i].h = (uint32_t)h;
      segs[i].plane =
          (per_plane > 0u) ? (uint16_t)((uint64_t)i / per_plane) : 0u;
    }
  } else if (b->strip_offset_count == 0u && b->jpeg_if_offset != 0u) {
    uint64_t off = b->jpeg_if_offset;
    uint64_t bc = b->jpeg_if_byte_count;
    if (off > r->size) {
      td_ctx_free(ctx, segs);
      td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_GEOMETRY, ifd_index,
                   TD_TAG_JPEG_IF_OFFSET, off, "jpeg interchange out of range");
      return TINYDNG_E_BOUNDS;
    }
    if (bc == 0u || bc > (r->size - off)) {
      /* Byte count 0 or extending past EOF: clamp to the end of the file
         (the fuzzer hit a crash decoding a 0-byte JPEG interchange). */
      bc = r->size - off;
    }
    if (bc == 0u) {
      td_ctx_free(ctx, segs);
      td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_GEOMETRY, ifd_index,
                   TD_TAG_JPEG_IF_OFFSET, off, "jpeg interchange out of range");
      return TINYDNG_E_BOUNDS;
    }
    segs[0].offset = off;
    segs[0].byte_count = bc;
    segs[0].index = 0;
    segs[0].kind = TINYDNG_SEG_STRIP;
    segs[0].x = 0;
    segs[0].y = 0;
    segs[0].w = b->width;
    segs[0].h = b->height;
  } else {
    uint64_t strips_per_plane = td_ceil_div_u64(b->height, b->rows_per_strip);
    for (i = 0; i < count; i++) {
      uint64_t local =
          (strips_per_plane > 0u) ? ((uint64_t)i % strips_per_plane) : 0u;
      uint64_t plane =
          (strips_per_plane > 0u) ? ((uint64_t)i / strips_per_plane) : 0u;
      uint64_t y = local * (uint64_t)b->rows_per_strip;
      uint64_t h = b->rows_per_strip;
      uint64_t off = b->strip_offsets[i];
      uint64_t bc = b->strip_byte_counts[i];
      if (y >= b->height) {
        h = 0;
      } else if ((b->height - y) < h) {
        h = b->height - y;
      }
      if (off > r->size || bc > (r->size - off)) {
        td_ctx_free(ctx, segs);
        td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_GEOMETRY, ifd_index,
                     TD_TAG_STRIP_OFFSETS, off,
                     "strip[%zu] out of range off=%llu bc=%llu size=%llu", i,
                     (unsigned long long)off, (unsigned long long)bc,
                     (unsigned long long)r->size);
        return TINYDNG_E_BOUNDS;
      }
      segs[i].offset = off;
      segs[i].byte_count = bc;
      segs[i].index = (uint32_t)i;
      segs[i].kind = TINYDNG_SEG_STRIP;
      segs[i].x = 0;
      segs[i].y = (uint32_t)y;
      segs[i].w = b->width;
      segs[i].h = (uint32_t)h;
      segs[i].plane = (uint16_t)plane;
    }
  }

  /* Publish geometry into the image. */
  img->width = b->width;
  img->height = b->height;
  img->samples_per_pixel = b->samples_per_pixel;
  img->bits_per_sample = b->bits_per_sample;
  img->bits_per_sample_decoded = b->bits_per_sample;
  img->compression = b->compression;
  img->sample_format = b->sample_format;
  img->planar_configuration = b->planar_configuration;
  img->predictor = b->predictor;
  img->rows_per_strip = b->rows_per_strip;
  img->tile_width = b->tile_width;
  img->tile_length = b->tile_length;
  img->jpeg_byte_count = b->jpeg_if_byte_count;
  img->segments = segs;
  img->segment_count = count;
  return TINYDNG_OK;
}

/* ------------------------------------------------------------------ */
/* Document image array growth                                        */
/* ------------------------------------------------------------------ */

static tinydng_image_info *td_doc_new_image(tinydng_context *ctx,
                                            tinydng_document *doc,
                                            size_t *cap, tinydng_error *err) {
  if (doc->image_count == *cap) {
    size_t newcap = (*cap == 0u) ? 8u : (*cap * 2u);
    size_t bytes;
    tinydng_image_info *ni;
    if (newcap > ctx->max_images) {
      newcap = ctx->max_images;
    }
    if (newcap <= doc->image_count) {
      td_set_error(err, TINYDNG_E_UNSUPPORTED, TINYDNG_STAGE_IFD, 0, 0, 0,
                   "too many images (cap %u)", ctx->max_images);
      return NULL;
    }
    if (!td_safe_mul_size(newcap, sizeof(tinydng_image_info), &bytes)) {
      td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_IFD, 0, 0, 0,
                   "image array overflow");
      return NULL;
    }
    ni = (tinydng_image_info *)td_ctx_realloc(
        ctx, doc->images, *cap * sizeof(tinydng_image_info), bytes, err);
    if (!ni) {
      return NULL;
    }
    doc->images = ni;
    *cap = newcap;
  }
  {
    tinydng_image_info *img = &doc->images[doc->image_count];
    memset(img, 0, sizeof(*img));
    doc->image_count++;
    return img;
  }
}

/* ------------------------------------------------------------------ */
/* open_io: header + IFD BFS                                          */
/* ------------------------------------------------------------------ */

typedef struct td_ifd_ref {
  uint64_t off;
  uint32_t depth;
} td_ifd_ref;

tinydng_status tinydng_open_io(tinydng_context *ctx, tinydng_io io,
                               const tinydng_open_options *opts,
                               tinydng_document **out, tinydng_error *err) {
  td_reader r;
  tinydng_document *doc = NULL;
  uint8_t magic[2];
  uint16_t version;
  uint64_t first_ifd = 0;
  uint32_t flags = opts ? opts->flags : 0u;
  td_ifd_ref *queue = NULL;
  uint64_t *visited = NULL;
  size_t qcap = 0, qhead = 0, qtail = 0;
  size_t vcap = 0, vcount = 0;
  size_t img_cap = 0;
  uint32_t ifd_seq = 0;
  tinydng_status st = TINYDNG_OK;

  tinydng_error_clear(err);
  if (!ctx || !out) {
    if (io.close) {
      io.close(&io);
    }
    return TINYDNG_E_INVALID_ARG;
  }
  *out = NULL;

  memset(&r, 0, sizeof(r));
  r.io = &io;
  r.size = io.size ? io.size(&io) : 0u;

  {
    /* td_io_view may return a pointer into mapped memory (not `magic`), so
       read the header bytes through the returned pointer, not the scratch. */
    uint8_t scratch4[4];
    const uint8_t *hp = td_io_view(&io, r.size, 0, 4u, scratch4,
                                   sizeof(scratch4));
    if (r.size < 8u || hp == NULL) {
      td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_HEADER, 0, 0, 0,
                   "buffer too small for TIFF/PSD header");
      if (io.close) {
        io.close(&io);
      }
      return TINYDNG_E_PARSE;
    }
    magic[0] = hp[0];
    magic[1] = hp[1];
    if (hp[0] == '8' && hp[1] == 'B' && hp[2] == 'P' && hp[3] == 'S') {
#ifndef TINYDNG_NO_PSD
      r.big_endian = 1;
      doc = (tinydng_document *)td_ctx_calloc(ctx, sizeof(*doc), err);
      if (!doc) {
        if (io.close) {
          io.close(&io);
        }
        return TINYDNG_E_OOM;
      }
      doc->io = io; /* take ownership */
      doc->has_io = 1;
      doc->io_size = r.size;
      doc->big_endian = 1;
      doc->format = TD_DOC_FORMAT_PSD;
      r.io = &doc->io;
      st = td_psd_open(ctx, &r, doc, flags, err);
      if (st != TINYDNG_OK) {
        tinydng_document_destroy(ctx, doc);
        return st;
      }
      *out = doc;
      return TINYDNG_OK;
#else
      td_set_error(err, TINYDNG_E_UNSUPPORTED, TINYDNG_STAGE_HEADER, 0, 0, 0,
                   "PSD support disabled (TINYDNG_NO_PSD)");
      if (io.close) {
        io.close(&io);
      }
      return TINYDNG_E_UNSUPPORTED;
#endif
    }
  }
  if (magic[0] == 'I' && magic[1] == 'I') {
    r.big_endian = 0;
  } else if (magic[0] == 'M' && magic[1] == 'M') {
    r.big_endian = 1;
  } else {
    td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_HEADER, 0, 0, 0,
                 "not a TIFF or PSD signature");
    if (io.close) {
      io.close(&io);
    }
    return TINYDNG_E_PARSE;
  }

  if (!td_r_u16(&r, 2u, &version)) {
    td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_HEADER, 0, 0, 2,
                 "failed reading TIFF version");
    if (io.close) {
      io.close(&io);
    }
    return TINYDNG_E_PARSE;
  }
  if (version == 42u) {
    uint32_t f;
    r.bigtiff = 0;
    if (!td_r_u32(&r, 4u, &f)) {
      td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_HEADER, 0, 0, 4,
                   "failed reading first IFD offset");
      if (io.close) {
        io.close(&io);
      }
      return TINYDNG_E_PARSE;
    }
    first_ifd = (uint64_t)f;
  } else if (version == 43u) {
    uint16_t off_bytesize = 0, reserved = 0;
    r.bigtiff = 1;
    if (!td_r_u16(&r, 4u, &off_bytesize) || !td_r_u16(&r, 6u, &reserved) ||
        off_bytesize != 8u || reserved != 0u ||
        !td_r_u64(&r, 8u, &first_ifd)) {
      td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_HEADER, 0, 0, 4,
                   "invalid BigTIFF header");
      if (io.close) {
        io.close(&io);
      }
      return TINYDNG_E_PARSE;
    }
  } else {
    td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_HEADER, 0, 0, 2,
                 "unsupported TIFF version %u", (unsigned)version);
    if (io.close) {
      io.close(&io);
    }
    return TINYDNG_E_PARSE;
  }

  doc = (tinydng_document *)td_ctx_calloc(ctx, sizeof(*doc), err);
  if (!doc) {
    if (io.close) {
      io.close(&io);
    }
    return TINYDNG_E_OOM;
  }
  doc->io = io; /* take ownership */
  doc->has_io = 1;
  doc->io_size = r.size;
  doc->big_endian = r.big_endian;
  doc->bigtiff = r.bigtiff;
  r.io = &doc->io;

  /* BFS over IFDs. */
  qcap = 16;
  queue = (td_ifd_ref *)td_ctx_alloc(ctx, qcap * sizeof(td_ifd_ref), err);
  vcap = 16;
  visited = (uint64_t *)td_ctx_alloc(ctx, vcap * sizeof(uint64_t), err);
  if (!queue || !visited) {
    st = TINYDNG_E_OOM;
    goto done;
  }
  if (first_ifd != 0u) {
    queue[qtail].off = first_ifd;
    queue[qtail].depth = 0;
    qtail++;
  }

  while (qhead < qtail) {
    td_ifd_ref ref = queue[qhead++];
    td_ifd_build b;
    tinydng_image_info tmp;
    uint64_t next_ifd = 0;
    size_t k;
    int seen = 0;

    if (ref.off == 0u || ref.off >= r.size) {
      continue;
    }
    for (k = 0; k < vcount; k++) {
      if (visited[k] == ref.off) {
        seen = 1;
        break;
      }
    }
    if (seen) {
      continue;
    }
    if (vcount == vcap) {
      uint64_t *nv =
          (uint64_t *)td_grow_array(ctx, visited, &vcap, sizeof(uint64_t), err);
      if (!nv) {
        st = TINYDNG_E_OOM;
        goto done;
      }
      visited = nv;
    }
    visited[vcount++] = ref.off;

    if (ifd_seq >= ctx->max_images) {
      td_set_error(err, TINYDNG_E_UNSUPPORTED, TINYDNG_STAGE_IFD, ifd_seq, 0,
                   ref.off, "too many IFDs (cap %u)", ctx->max_images);
      st = TINYDNG_E_UNSUPPORTED;
      goto done;
    }

    memset(&tmp, 0, sizeof(tmp));
    st = td_parse_ifd(ctx, &r, ref.off, ifd_seq, &tmp, &b, &next_ifd, err);
    if (st != TINYDNG_OK) {
      td_free_build(ctx, &b);
      goto done;
    }

    /* Enqueue SubIFDs (depth+1) and the next IFD (same depth). */
    if ((flags & TINYDNG_OPEN_PARSE_SUBIFDS) && b.sub_ifds &&
        ref.depth < ctx->max_ifd_depth) {
      for (k = 0; k < b.sub_ifd_count; k++) {
        if (qtail == qcap) {
          td_ifd_ref *nq = (td_ifd_ref *)td_grow_array(
              ctx, queue, &qcap, sizeof(td_ifd_ref), err);
          if (!nq) {
            td_free_build(ctx, &b);
            st = TINYDNG_E_OOM;
            goto done;
          }
          queue = nq;
        }
        queue[qtail].off = b.sub_ifds[k];
        queue[qtail].depth = ref.depth + 1u;
        qtail++;
      }
    }
    if (next_ifd != 0u) {
      if (qtail == qcap) {
        td_ifd_ref *nq = (td_ifd_ref *)td_grow_array(
            ctx, queue, &qcap, sizeof(td_ifd_ref), err);
        if (!nq) {
          td_free_build(ctx, &b);
          st = TINYDNG_E_OOM;
          goto done;
        }
        queue = nq;
      }
      queue[qtail].off = next_ifd;
      queue[qtail].depth = ref.depth;
      qtail++;
    }

    /* If this IFD describes an image, build its segment table and keep it. */
    if (b.has_width && b.has_height) {
      tinydng_image_info *img = td_doc_new_image(ctx, doc, &img_cap, err);
      if (!img) {
        td_free_image_payload(ctx, &tmp); /* tmp not yet moved into the doc */
        td_free_build(ctx, &b);
        st = err->status ? err->status : TINYDNG_E_OOM;
        goto done;
      }
      /* move tmp metadata into the slot, then build segments */
      *img = tmp;
      st = td_build_segments(ctx, &r, &b, ifd_seq, img, err);
      if (st != TINYDNG_OK) {
        td_free_build(ctx, &b);
        goto done;
      }
    } else {
      /* Not an image IFD: discard any metadata payload parsed into tmp. */
      td_free_image_payload(ctx, &tmp);
    }
    td_free_build(ctx, &b);
    ifd_seq++;
  }

  if (doc->image_count == 0u) {
    td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_IFD, 0, 0, 0,
                 "no image IFDs found");
    st = TINYDNG_E_PARSE;
    goto done;
  }

  /* Promote IFD0's document-level EXIF into doc->global_exif and clear the
     per-image copy so tinydng_document_destroy() does not double-free. */
  doc->global_exif = doc->images[0].exif;
  memset(&doc->images[0].exif, 0, sizeof(tinydng_exif));
  doc->has_global_exif = 1;

  st = TINYDNG_OK;

done:
  td_ctx_free(ctx, queue);
  td_ctx_free(ctx, visited);
  if (st != TINYDNG_OK) {
    tinydng_document_destroy(ctx, doc);
    return st;
  }
  *out = doc;
  return TINYDNG_OK;
}
