/*
 * tinydng_psd.c - Adobe PSD/PSB parser + layer/thumbnail/smart-object decode.
 *
 * Clean-room implementation from the "Adobe Photoshop File Formats
 * Specification". All multi-byte values are big-endian. Every section walk
 * is bounds-checked against a precomputed section end and must strictly
 * advance; all allocation goes through the tracked context allocator.
 *
 * SPDX-License-Identifier: MIT
 */
#include "td_internal.h"

#ifndef TINYDNG_NO_PSD

#ifndef TINYDNG_NO_ZIP
#include "miniz.h"
#endif
#ifndef TINYDNG_NO_BASELINE_JPEG
#define STBI_NO_STDIO
#include "stb_image.h"
#endif

#define TD_PSD_FOURCC(a, b, c, d) TINYDNG_PSD_FOURCC(a, b, c, d)

#define TD_PSD_SIG_8BPS TD_PSD_FOURCC('8', 'B', 'P', 'S')
#define TD_PSD_SIG_8BIM TD_PSD_FOURCC('8', 'B', 'I', 'M')
#define TD_PSD_SIG_8B64 TD_PSD_FOURCC('8', 'B', '6', '4')

#define TD_PSD_MAX_CHANNELS 56u
#define TD_PSD_MAX_LAYER_CHANNELS 58u /* 56 image + user mask + real mask */
#define TD_PSD_MAX_LAYER_BLOCKS 512u
#define TD_PSD_MAX_GLOBAL_BLOCKS 2048u
#define TD_PSD_MAX_SMART_OBJECTS 256u
#define TD_PSD_MAX_UNICODE_BYTES (16u * 1024u * 1024u)
#define TD_PSD_SCAN_CHUNK 4096u

/* ------------------------------------------------------------------ */
/* Small read helpers                                                 */
/* ------------------------------------------------------------------ */

static int td_psd_r_len(const td_reader *r, uint64_t at, int is_psb,
                        uint64_t *out) {
  if (is_psb) {
    return td_r_u64(r, at, out);
  } else {
    uint32_t v;
    if (!td_r_u32(r, at, &v)) {
      return 0;
    }
    *out = (uint64_t)v;
    return 1;
  }
}

static int td_psd_r_i16(const td_reader *r, uint64_t at, int16_t *out) {
  uint16_t v;
  if (!td_r_u16(r, at, &v)) {
    return 0;
  }
  *out = (int16_t)v;
  return 1;
}

/* Copy [off, off+len) into a ctx-owned buffer (zero-copy map is used as the
   source when available). Returns NULL with err set on OOB/OOM. */
static uint8_t *td_psd_read_copy(tinydng_context *ctx, const td_reader *r,
                                 uint64_t off, size_t len,
                                 tinydng_error *err) {
  uint8_t *buf;
  const uint8_t *view;
  if (len == 0u) {
    /* Valid zero-length request: return a 1-byte allocation so callers can
       treat NULL uniformly as failure. */
    return (uint8_t *)td_ctx_calloc(ctx, 1u, err);
  }
  buf = (uint8_t *)td_ctx_alloc(ctx, len, err);
  if (!buf) {
    return NULL;
  }
  view = td_io_view(r->io, r->size, off, len, buf, len);
  if (!view) {
    td_ctx_free(ctx, buf);
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_METADATA, 0, 0, off,
                 "PSD: byte range [%llu,+%zu) out of file",
                 (unsigned long long)off, len);
    return NULL;
  }
  if (view != buf) {
    memcpy(buf, view, len);
  }
  return buf;
}

/* ------------------------------------------------------------------ */
/* Strings                                                            */
/* ------------------------------------------------------------------ */

/* Copy `n` raw bytes as a NUL-terminated string (Pascal names are typically
   ASCII; bytes are passed through unmodified). */
static char *td_psd_strdup_bytes(tinydng_context *ctx, const uint8_t *bytes,
                                 size_t n, tinydng_error *err) {
  char *s = (char *)td_ctx_alloc(ctx, n + 1u, err);
  if (!s) {
    return NULL;
  }
  if (n > 0u) {
    memcpy(s, bytes, n);
  }
  s[n] = '\0';
  return s;
}

/* Read a Pascal string at `at`; the whole field (1 length byte + bytes) is
   padded up to a multiple of `pad`. Returns 1 on success and sets *advance
   to the padded field size. *out may be NULL if the caller passed NULL. */
static int td_psd_read_pascal(tinydng_context *ctx, const td_reader *r,
                              uint64_t at, uint64_t end, uint32_t pad,
                              char **out, uint64_t *advance,
                              tinydng_error *err) {
  uint8_t len8;
  uint64_t field;
  uint8_t tmp[256];
  const uint8_t *p;
  if (at >= end || !td_io_view(r->io, r->size, at, 1u, tmp, sizeof(tmp))) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_METADATA, 0, 0, at,
                 "PSD: Pascal string out of range");
    return 0;
  }
  p = td_io_view(r->io, r->size, at, 1u, tmp, sizeof(tmp));
  len8 = p[0];
  field = 1u + (uint64_t)len8;
  field = (field + (pad - 1u)) & ~(uint64_t)(pad - 1u);
  if (field > end - at) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_METADATA, 0, 0, at,
                 "PSD: Pascal string overruns section");
    return 0;
  }
  if (out) {
    const uint8_t *sp =
        (len8 > 0u) ? td_io_view(r->io, r->size, at + 1u, len8, tmp,
                                 sizeof(tmp))
                    : tmp;
    if (!sp) {
      td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_METADATA, 0, 0, at,
                   "PSD: Pascal string bytes out of range");
      return 0;
    }
    *out = td_psd_strdup_bytes(ctx, sp, len8, err);
    if (!*out) {
      return 0;
    }
  }
  *advance = field;
  return 1;
}

/* Convert UTF-16BE (n_units code units) to a ctx-owned UTF-8 string.
   Surrogate-pair aware; lone surrogates become U+FFFD. */
static char *td_psd_utf16be_to_utf8(tinydng_context *ctx, const uint8_t *in,
                                    size_t n_units, tinydng_error *err) {
  size_t cap, len = 0, i = 0;
  char *out;
  if (!td_safe_mul_size(n_units, 4u, &cap) ||
      !td_safe_add_size(cap, 1u, &cap)) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_METADATA, 0, 0, 0,
                 "PSD: unicode string too long");
    return NULL;
  }
  out = (char *)td_ctx_alloc(ctx, cap, err);
  if (!out) {
    return NULL;
  }
  while (i < n_units) {
    uint32_t cp = ((uint32_t)in[2u * i] << 8) | in[2u * i + 1u];
    i++;
    if (cp >= 0xD800u && cp <= 0xDBFFu) {
      if (i < n_units) {
        uint32_t lo = ((uint32_t)in[2u * i] << 8) | in[2u * i + 1u];
        if (lo >= 0xDC00u && lo <= 0xDFFFu) {
          cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
          i++;
        } else {
          cp = 0xFFFDu;
        }
      } else {
        cp = 0xFFFDu;
      }
    } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
      cp = 0xFFFDu;
    }
    if (cp == 0u) {
      break; /* embedded NUL terminates (Photoshop pads names with NUL) */
    }
    if (cp < 0x80u) {
      out[len++] = (char)cp;
    } else if (cp < 0x800u) {
      out[len++] = (char)(0xC0u | (cp >> 6));
      out[len++] = (char)(0x80u | (cp & 0x3Fu));
    } else if (cp < 0x10000u) {
      out[len++] = (char)(0xE0u | (cp >> 12));
      out[len++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
      out[len++] = (char)(0x80u | (cp & 0x3Fu));
    } else {
      out[len++] = (char)(0xF0u | (cp >> 18));
      out[len++] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
      out[len++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
      out[len++] = (char)(0x80u | (cp & 0x3Fu));
    }
  }
  out[len] = '\0';
  return out;
}

/* Read a UnicodeString (u32 code-unit count + UTF-16BE) at `at`. Sets
   *advance to the wire size. */
static char *td_psd_read_unicode(tinydng_context *ctx, const td_reader *r,
                                 uint64_t at, uint64_t end, uint64_t *advance,
                                 tinydng_error *err) {
  uint32_t units;
  uint64_t bytes, total;
  uint8_t *raw;
  char *out;
  if (at > end || end - at < 4u || !td_r_u32(r, at, &units)) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_METADATA, 0, 0, at,
                 "PSD: unicode string header out of range");
    return NULL;
  }
  bytes = (uint64_t)units * 2u;
  if (bytes > (uint64_t)SIZE_MAX || bytes > TD_PSD_MAX_UNICODE_BYTES ||
      !td_safe_add_u64(4u, bytes, &total) || total > end - at) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_METADATA, 0, 0, at,
                 "PSD: unicode string overruns section");
    return NULL;
  }
  if (units == 0u) {
    *advance = 4u;
    return td_psd_strdup_bytes(ctx, NULL, 0u, err);
  }
  raw = td_psd_read_copy(ctx, r, at + 4u, (size_t)bytes, err);
  if (!raw) {
    return NULL;
  }
  out = td_psd_utf16be_to_utf8(ctx, raw, (size_t)units, err);
  td_ctx_free(ctx, raw);
  if (!out) {
    return NULL;
  }
  *advance = total;
  return out;
}

/* ------------------------------------------------------------------ */
/* Free                                                               */
/* ------------------------------------------------------------------ */

void td_psd_free_info(tinydng_context *ctx, struct tinydng_psd_info *psd) {
  size_t i;
  if (!psd) {
    return;
  }
  td_ctx_free(ctx, psd->color_mode_data);
  if (psd->layers) {
    for (i = 0; i < psd->layer_count; i++) {
      td_ctx_free(ctx, psd->layers[i].name);
      td_ctx_free(ctx, psd->layers[i].channels);
      td_ctx_free(ctx, psd->layers[i].blocks);
    }
    td_ctx_free(ctx, psd->layers);
  }
  if (psd->resources) {
    for (i = 0; i < psd->resource_count; i++) {
      td_ctx_free(ctx, psd->resources[i].name);
    }
    td_ctx_free(ctx, psd->resources);
  }
  td_ctx_free(ctx, psd->global_blocks);
  if (psd->smart_objects) {
    for (i = 0; i < psd->smart_object_count; i++) {
      td_ctx_free(ctx, psd->smart_objects[i].uid);
      td_ctx_free(ctx, psd->smart_objects[i].filename);
    }
    td_ctx_free(ctx, psd->smart_objects);
  }
  td_ctx_free(ctx, psd);
}

/* ------------------------------------------------------------------ */
/* ZIP-with-prediction un-predict                                     */
/* ------------------------------------------------------------------ */

/* Undo PSD zip prediction in place on stored big-endian bytes, one plane of
   w*h samples, rows independent.
   - depth 8:  horizontal byte delta.
   - depth 16: delta on big-endian 16-bit values.
   - depth 32: byte delta across the doubled row then de-interleave the four
     byte planes (same transform as TIFF predictor 3 with spp=1). */
int td_psd_unpredict_plane(tinydng_context *ctx, uint8_t *plane, uint32_t w,
                           uint32_t h, uint16_t depth, tinydng_error *err) {
  uint32_t y;
  if (depth == 8u) {
    size_t row = (size_t)w;
    for (y = 0; y < h; y++) {
      uint8_t *p = plane + (size_t)y * row;
      size_t x;
      for (x = 1; x < row; x++) {
        p[x] = (uint8_t)(p[x] + p[x - 1u]);
      }
    }
    return 1;
  }
  if (depth == 16u) {
    size_t row;
    if (!td_safe_mul_size((size_t)w, 2u, &row)) {
      td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_DECODE, 0, 0, 0,
                   "PSD: 16-bit row size overflow");
      return 0;
    }
    for (y = 0; y < h; y++) {
      uint8_t *p = plane + (size_t)y * row;
      uint32_t x;
      uint16_t prev = ((uint16_t)p[0] << 8) | p[1];
      for (x = 1; x < w; x++) {
        uint16_t v = (uint16_t)(((uint16_t)p[2u * x] << 8) | p[2u * x + 1u]);
        v = (uint16_t)(v + prev);
        p[2u * x] = (uint8_t)(v >> 8);
        p[2u * x + 1u] = (uint8_t)(v & 0xFFu);
        prev = v;
      }
    }
    return 1;
  }
  if (depth == 32u) {
    size_t row;
    if (!td_safe_mul_size((size_t)w, 4u, &row)) {
      td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_DECODE, 0, 0, 0,
                   "PSD: 32-bit row size overflow");
      return 0;
    }
    uint8_t *tmp = (uint8_t *)td_ctx_alloc(ctx, row, err);
    if (!tmp) {
      return 0;
    }
    for (y = 0; y < h; y++) {
      uint8_t *p = plane + (size_t)y * row;
      size_t i;
      uint32_t n;
      for (i = 1; i < row; i++) {
        p[i] = (uint8_t)(p[i] + p[i - 1u]);
      }
      memcpy(tmp, p, row);
      for (n = 0; n < w; n++) {
        p[4u * n + 0u] = tmp[(size_t)0u * w + n];
        p[4u * n + 1u] = tmp[(size_t)1u * w + n];
        p[4u * n + 2u] = tmp[(size_t)2u * w + n];
        p[4u * n + 3u] = tmp[(size_t)3u * w + n];
      }
    }
    td_ctx_free(ctx, tmp);
    return 1;
  }
  td_set_error(err, TINYDNG_E_UNSUPPORTED, TINYDNG_STAGE_DECODE, 0, 0, 0,
               "PSD: zip prediction unsupported for depth %u",
               (unsigned)depth);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Tagged blocks                                                      */
/* ------------------------------------------------------------------ */

static int td_psd_key_is_u64(uint32_t key) {
  switch (key) {
    case TD_PSD_FOURCC('L', 'M', 's', 'k'):
    case TD_PSD_FOURCC('L', 'r', '1', '6'):
    case TD_PSD_FOURCC('L', 'r', '3', '2'):
    case TD_PSD_FOURCC('L', 'a', 'y', 'r'):
    case TD_PSD_FOURCC('M', 't', '1', '6'):
    case TD_PSD_FOURCC('M', 't', '3', '2'):
    case TD_PSD_FOURCC('M', 't', 'r', 'n'):
    case TD_PSD_FOURCC('A', 'l', 'p', 'h'):
    case TD_PSD_FOURCC('F', 'M', 's', 'k'):
    case TD_PSD_FOURCC('l', 'n', 'k', '2'):
    case TD_PSD_FOURCC('F', 'E', 'i', 'd'):
    case TD_PSD_FOURCC('F', 'X', 'i', 'd'):
    case TD_PSD_FOURCC('P', 'x', 'S', 'D'):
      return 1;
    default:
      return 0;
  }
}

/* Read one tagged block at *at. Returns 1 on success (block filled, *at
   advanced past padded data), 0 when no valid block starts here (graceful
   stop), -1 on a fatal bounds error (err set). Tolerates up to 3 bytes of
   padding slack before the signature (some writers pad blocks to 4). */
static int td_psd_read_tagged_block(const td_reader *r, int is_psb,
                                    uint64_t *at, uint64_t end,
                                    tinydng_psd_block *out,
                                    tinydng_error *err) {
  uint64_t p = *at;
  uint32_t sig = 0, key;
  uint64_t len, data_off, padded;
  uint32_t shift;
  (void)err;
  for (shift = 0; shift <= 3u; shift++) {
    if (end - p < 12u || end < p) {
      return 0;
    }
    if (!td_r_u32(r, p, &sig)) {
      return 0;
    }
    if (sig == TD_PSD_SIG_8BIM || sig == TD_PSD_SIG_8B64) {
      break;
    }
    p++;
  }
  if (sig != TD_PSD_SIG_8BIM && sig != TD_PSD_SIG_8B64) {
    return 0;
  }
  if (!td_r_u32(r, p + 4u, &key)) {
    return 0;
  }
  if (is_psb && td_psd_key_is_u64(key)) {
    if (end - p < 16u || !td_r_u64(r, p + 8u, &len)) {
      return 0;
    }
    data_off = p + 16u;
  } else {
    uint32_t l32;
    if (!td_r_u32(r, p + 8u, &l32)) {
      return 0;
    }
    len = (uint64_t)l32;
    data_off = p + 12u;
  }
  if (data_off > end || len > end - data_off) {
    return 0; /* declared length overruns the section: stop gracefully */
  }
  out->key = key;
  out->offset = data_off;
  out->length = len;
  padded = len + (len & 1u); /* pad to 2 */
  if (padded > end - data_off) {
    *at = end; /* padding byte would overrun; consume the rest */
  } else {
    *at = data_off + padded;
  }
  return 1;
}

/* ------------------------------------------------------------------ */
/* Section: header + color mode data                                  */
/* ------------------------------------------------------------------ */

static tinydng_status td_psd_parse_header(tinydng_context *ctx, td_reader *r,
                                          tinydng_psd_info *psd,
                                          tinydng_error *err) {
  uint32_t sig;
  uint16_t version, channels, depth, mode;
  uint32_t height, width;
  uint64_t pixels;
  uint32_t dim_cap;

  if (!td_r_u32(r, 0u, &sig) || sig != TD_PSD_SIG_8BPS) {
    td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_HEADER, 0, 0, 0,
                 "PSD: bad signature");
    return TINYDNG_E_PARSE;
  }
  if (!td_r_u16(r, 4u, &version) || (version != 1u && version != 2u)) {
    td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_HEADER, 0, 0, 4,
                 "PSD: unsupported version");
    return TINYDNG_E_PARSE;
  }
  /* 6 reserved bytes at [6,12): must be zero per spec; tolerated if not. */
  if (!td_r_u16(r, 12u, &channels) || !td_r_u32(r, 14u, &height) ||
      !td_r_u32(r, 18u, &width) || !td_r_u16(r, 22u, &depth) ||
      !td_r_u16(r, 24u, &mode)) {
    td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_HEADER, 0, 0, 12,
                 "PSD: truncated header");
    return TINYDNG_E_PARSE;
  }
  psd->is_psb = (version == 2u);
  dim_cap = psd->is_psb ? 300000u : 30000u;
  if (channels < 1u || channels > TD_PSD_MAX_CHANNELS) {
    td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_HEADER, 0, 0, 12,
                 "PSD: channel count %u out of range", (unsigned)channels);
    return TINYDNG_E_PARSE;
  }
  if (width < 1u || width > dim_cap || height < 1u || height > dim_cap) {
    td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_HEADER, 0, 0, 14,
                 "PSD: dimensions %ux%u out of range", width, height);
    return TINYDNG_E_PARSE;
  }
  if (depth != 1u && depth != 8u && depth != 16u && depth != 32u) {
    td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_HEADER, 0, 0, 22,
                 "PSD: depth %u not in {1,8,16,32}", (unsigned)depth);
    return TINYDNG_E_PARSE;
  }
  if (mode > 9u || mode == 5u || mode == 6u) {
    td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_HEADER, 0, 0, 24,
                 "PSD: invalid color mode %u", (unsigned)mode);
    return TINYDNG_E_PARSE;
  }
  if (!td_safe_mul_u64(width, height, &pixels) ||
      pixels > ctx->max_image_pixels) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_HEADER, 0, 0, 14,
                 "PSD: image exceeds max_image_pixels");
    return TINYDNG_E_BOUNDS;
  }
  psd->channel_count = channels;
  psd->depth = depth;
  psd->color_mode = mode;
  psd->width = width;
  psd->height = height;
  return TINYDNG_OK;
}

static tinydng_status td_psd_parse_color_mode(tinydng_context *ctx,
                                              td_reader *r,
                                              tinydng_psd_info *psd,
                                              uint64_t at, uint64_t *next,
                                              tinydng_error *err) {
  uint32_t len;
  if (!td_r_u32(r, at, &len)) {
    td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_METADATA, 0, 0, at,
                 "PSD: truncated color mode data length");
    return TINYDNG_E_PARSE;
  }
  if ((uint64_t)len > r->size - (at + 4u)) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_METADATA, 0, 0, at,
                 "PSD: color mode data overruns file");
    return TINYDNG_E_BOUNDS;
  }
  if (psd->color_mode == TINYDNG_PSD_INDEXED && len != 768u) {
    td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_METADATA, 0, 0, at,
                 "PSD: indexed palette must be 768 bytes (got %u)", len);
    return TINYDNG_E_PARSE;
  }
  if (len > 0u) {
    psd->color_mode_data = td_psd_read_copy(ctx, r, at + 4u, len, err);
    if (!psd->color_mode_data) {
      return td_error_status_or(err, TINYDNG_E_OOM);
    }
    psd->color_mode_data_size = len;
  }
  *next = at + 4u + len;
  return TINYDNG_OK;
}

/* ------------------------------------------------------------------ */
/* Section: image resources (8BIM)                                    */
/* ------------------------------------------------------------------ */

static tinydng_status td_psd_parse_resources(tinydng_context *ctx,
                                             td_reader *r,
                                             tinydng_psd_info *psd,
                                             uint64_t at, uint64_t *next,
                                             tinydng_error *err) {
  uint32_t section_len;
  uint64_t cur, end;
  size_t cap = 0;

  if (!td_r_u32(r, at, &section_len)) {
    td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_METADATA, 0, 0, at,
                 "PSD: truncated image resources length");
    return TINYDNG_E_PARSE;
  }
  if ((uint64_t)section_len > r->size - (at + 4u)) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_METADATA, 0, 0, at,
                 "PSD: image resources overrun file");
    return TINYDNG_E_BOUNDS;
  }
  cur = at + 4u;
  end = cur + section_len;
  *next = end;

  while (end - cur >= 12u) {
    uint32_t sig;
    uint16_t id;
    uint64_t name_adv = 0;
    uint32_t size;
    uint64_t data_off, padded;
    char *name = NULL;

    if (!td_r_u32(r, cur, &sig)) {
      break;
    }
    if (sig != TD_PSD_SIG_8BIM &&
        sig != TD_PSD_FOURCC('M', 'e', 'S', 'a') &&
        sig != TD_PSD_FOURCC('P', 'H', 'U', 'T') &&
        sig != TD_PSD_FOURCC('D', 'C', 'S', 'R') &&
        sig != TD_PSD_FOURCC('A', 'g', 'H', 'g')) {
      break; /* garbage: stop parsing resources gracefully */
    }
    if (!td_r_u16(r, cur + 4u, &id)) {
      break;
    }
    if (!td_psd_read_pascal(ctx, r, cur + 6u, end, 2u, &name, &name_adv,
                            err)) {
      tinydng_error_clear(err);
      break;
    }
    if (end - (cur + 6u + name_adv) < 4u ||
        !td_r_u32(r, cur + 6u + name_adv, &size)) {
      td_ctx_free(ctx, name);
      break;
    }
    data_off = cur + 6u + name_adv + 4u;
    if ((uint64_t)size > end - data_off) {
      td_ctx_free(ctx, name);
      break;
    }
    if (psd->resource_count >= ctx->max_psd_resources) {
      td_ctx_free(ctx, name);
      td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_METADATA, 0, 0, cur,
                   "PSD: too many image resources (cap %u)",
                   ctx->max_psd_resources);
      return TINYDNG_E_BOUNDS;
    }
    if (psd->resource_count == cap) {
      size_t new_cap = cap ? cap * 2u : 16u;
      size_t old_bytes = cap * sizeof(tinydng_psd_resource);
      size_t new_bytes;
      tinydng_psd_resource *grown;
      if (!td_safe_mul_size(new_cap, sizeof(tinydng_psd_resource),
                            &new_bytes)) {
        td_ctx_free(ctx, name);
        return TINYDNG_E_BOUNDS;
      }
      grown = (tinydng_psd_resource *)td_ctx_realloc(ctx, psd->resources,
                                                     old_bytes, new_bytes,
                                                     err);
      if (!grown) {
        td_ctx_free(ctx, name);
        return TINYDNG_E_OOM;
      }
      psd->resources = grown;
      cap = new_cap;
    }
    psd->resources[psd->resource_count].id = id;
    psd->resources[psd->resource_count].name = name;
    psd->resources[psd->resource_count].offset = data_off;
    psd->resources[psd->resource_count].length = size;
    psd->resource_count++;

    padded = (uint64_t)size + (size & 1u);
    if (padded > end - data_off) {
      cur = end;
    } else {
      cur = data_off + padded;
    }
  }
  return TINYDNG_OK;
}

/* ------------------------------------------------------------------ */
/* Section: layer records                                             */
/* ------------------------------------------------------------------ */

/* Validate a layer/mask rect; returns 1 and stores width/height. */
static int td_psd_rect_ok(const tinydng_context *ctx, int32_t top,
                          int32_t left, int32_t bottom, int32_t right,
                          uint32_t *w, uint32_t *h) {
  int64_t ww = (int64_t)right - (int64_t)left;
  int64_t hh = (int64_t)bottom - (int64_t)top;
  if (ww < 0 || hh < 0 || ww > 0x7FFFFFFF || hh > 0x7FFFFFFF) {
    return 0;
  }
  if ((uint64_t)ww * (uint64_t)hh > ctx->max_image_pixels) {
    return 0;
  }
  *w = (uint32_t)ww;
  *h = (uint32_t)hh;
  return 1;
}

/* Parse layer records + channel image data. `at` points at the i16 layer
   count; `end` bounds the containing layer-info payload. Fills psd->layers.
   `depth` is the document bit depth (needed only for validation). */
static tinydng_status td_psd_parse_layer_records(tinydng_context *ctx,
                                                 td_reader *r,
                                                 tinydng_psd_info *psd,
                                                 uint64_t at, uint64_t end,
                                                 tinydng_error *err) {
  int16_t count_raw;
  uint32_t count;
  uint64_t cur;
  size_t li;
  tinydng_status st = TINYDNG_OK;

  if (end - at < 2u || !td_psd_r_i16(r, at, &count_raw)) {
    td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_METADATA, 0, 0, at,
                 "PSD: truncated layer count");
    return TINYDNG_E_PARSE;
  }
  if (count_raw < 0) {
    if (count_raw == INT16_MIN) {
      td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_METADATA, 0, 0, at,
                   "PSD: invalid layer count");
      return TINYDNG_E_PARSE;
    }
    psd->has_transparency = 1;
    count = (uint32_t)(-count_raw);
  } else {
    count = (uint32_t)count_raw;
  }
  if (count == 0u) {
    return TINYDNG_OK;
  }
  if (count > ctx->max_psd_layers) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_METADATA, 0, 0, at,
                 "PSD: %u layers exceeds cap %u", count, ctx->max_psd_layers);
    return TINYDNG_E_BOUNDS;
  }
  /* Each layer record is at least 34 bytes on the wire. */
  if ((uint64_t)count * 34u > end - at) {
    td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_METADATA, 0, 0, at,
                 "PSD: layer count %u larger than section", count);
    return TINYDNG_E_PARSE;
  }
  {
    size_t bytes;
    if (!td_safe_mul_size((size_t)count, sizeof(tinydng_psd_layer), &bytes)) {
      return TINYDNG_E_BOUNDS;
    }
    psd->layers = (tinydng_psd_layer *)td_ctx_calloc(ctx, bytes, err);
    if (!psd->layers) {
      return TINYDNG_E_OOM;
    }
  }
  psd->layer_count = count;
  cur = at + 2u;

  for (li = 0; li < count; li++) {
    tinydng_psd_layer *L = &psd->layers[li];
    int32_t rect[4];
    uint16_t nch;
    uint32_t sig, blend;
    uint32_t extra_len;
    uint64_t extra_end;
    size_t c;
    int k;

    L->parent = UINT32_MAX;

    if (end - cur < 18u) {
      td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_METADATA, 0, 0, cur,
                   "PSD: truncated layer record %zu", li);
      return TINYDNG_E_PARSE;
    }
    for (k = 0; k < 4; k++) {
      if (!td_r_i32(r, cur + (uint64_t)k * 4u, &rect[k])) {
        return TINYDNG_E_PARSE;
      }
    }
    L->top = rect[0];
    L->left = rect[1];
    L->bottom = rect[2];
    L->right = rect[3];
    if (!td_psd_rect_ok(ctx, L->top, L->left, L->bottom, L->right, &L->width,
                        &L->height)) {
      td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_METADATA, 0, 0, cur,
                   "PSD: layer %zu rect invalid", li);
      return TINYDNG_E_PARSE;
    }
    if (!td_r_u16(r, cur + 16u, &nch)) {
      return TINYDNG_E_PARSE;
    }
    if (nch > TD_PSD_MAX_LAYER_CHANNELS) {
      td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_METADATA, 0, 0, cur,
                   "PSD: layer %zu channel count %u out of range", li,
                   (unsigned)nch);
      return TINYDNG_E_PARSE;
    }
    cur += 18u;
    if (nch > 0u) {
      size_t bytes;
      if (!td_safe_mul_size((size_t)nch, sizeof(tinydng_psd_channel),
                            &bytes)) {
        return TINYDNG_E_BOUNDS;
      }
      L->channels = (tinydng_psd_channel *)td_ctx_calloc(ctx, bytes, err);
      if (!L->channels) {
        return TINYDNG_E_OOM;
      }
    }
    L->channel_count = nch;
    {
      uint64_t ch_rec = psd->is_psb ? 10u : 6u;
      for (c = 0; c < nch; c++) {
        int16_t id;
        uint64_t clen;
        if (end - cur < ch_rec) {
          td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_METADATA, 0, 0,
                       cur, "PSD: truncated channel info (layer %zu)", li);
          return TINYDNG_E_PARSE;
        }
        if (!td_psd_r_i16(r, cur, &id) ||
            !td_psd_r_len(r, cur + 2u, psd->is_psb, &clen)) {
          return TINYDNG_E_PARSE;
        }
        L->channels[c].id = id;
        L->channels[c].data_length = clen; /* incl. 2-byte tag; fixed later */
        cur += ch_rec;
      }
    }
    if (end - cur < 16u) {
      td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_METADATA, 0, 0, cur,
                   "PSD: truncated blend info (layer %zu)", li);
      return TINYDNG_E_PARSE;
    }
    if (!td_r_u32(r, cur, &sig) || sig != TD_PSD_SIG_8BIM) {
      td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_METADATA, 0, 0, cur,
                   "PSD: layer %zu blend signature missing", li);
      return TINYDNG_E_PARSE;
    }
    if (!td_r_u32(r, cur + 4u, &blend)) {
      return TINYDNG_E_PARSE;
    }
    L->blend_mode = blend;
    {
      uint8_t b4[4];
      const uint8_t *p =
          td_io_view(r->io, r->size, cur + 8u, 4u, b4, sizeof(b4));
      if (!p) {
        return TINYDNG_E_PARSE;
      }
      L->opacity = p[0];
      L->clipping = p[1];
      L->flags = p[2];
      /* p[3] filler */
    }
    if (!td_r_u32(r, cur + 12u, &extra_len)) {
      return TINYDNG_E_PARSE;
    }
    cur += 16u;
    if ((uint64_t)extra_len > end - cur) {
      td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_METADATA, 0, 0, cur,
                   "PSD: layer %zu extra data overruns section", li);
      return TINYDNG_E_PARSE;
    }
    extra_end = cur + extra_len;

    /* --- extra data: mask, blending ranges, name, tagged blocks --- */
    {
      uint64_t p = cur;
      uint32_t mask_size;
      uint32_t ranges_size;
      uint64_t name_adv = 0;

      if (extra_end - p < 4u || !td_r_u32(r, p, &mask_size)) {
        td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_METADATA, 0, 0, p,
                     "PSD: layer %zu mask header truncated", li);
        return TINYDNG_E_PARSE;
      }
      p += 4u;
      if ((uint64_t)mask_size > extra_end - p) {
        td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_METADATA, 0, 0, p,
                     "PSD: layer %zu mask data overruns extra", li);
        return TINYDNG_E_PARSE;
      }
      if (mask_size >= 18u) {
        int32_t mrect[4];
        uint8_t mb[2];
        const uint8_t *mp;
        for (k = 0; k < 4; k++) {
          if (!td_r_i32(r, p + (uint64_t)k * 4u, &mrect[k])) {
            return TINYDNG_E_PARSE;
          }
        }
        mp = td_io_view(r->io, r->size, p + 16u, 2u, mb, sizeof(mb));
        if (!mp) {
          return TINYDNG_E_PARSE;
        }
        L->mask.top = mrect[0];
        L->mask.left = mrect[1];
        L->mask.bottom = mrect[2];
        L->mask.right = mrect[3];
        L->mask.default_color = mp[0];
        L->mask.flags = mp[1];
        L->mask.present = 1;
      }
      p += mask_size;

      if (extra_end - p < 4u || !td_r_u32(r, p, &ranges_size)) {
        td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_METADATA, 0, 0, p,
                     "PSD: layer %zu blending ranges truncated", li);
        return TINYDNG_E_PARSE;
      }
      p += 4u;
      if ((uint64_t)ranges_size > extra_end - p) {
        td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_METADATA, 0, 0, p,
                     "PSD: layer %zu blending ranges overrun extra", li);
        return TINYDNG_E_PARSE;
      }
      p += ranges_size;

      if (!td_psd_read_pascal(ctx, r, p, extra_end, 4u, &L->name, &name_adv,
                              err)) {
        return td_error_status_or(err, TINYDNG_E_PARSE);
      }
      p += name_adv;

      /* Tagged blocks until extra_end. */
      {
        size_t bcap = 0;
        for (;;) {
          tinydng_psd_block blk;
          int rc = td_psd_read_tagged_block(r, psd->is_psb, &p, extra_end,
                                            &blk, err);
          if (rc <= 0) {
            break;
          }
          if (L->block_count >= TD_PSD_MAX_LAYER_BLOCKS) {
            break;
          }
          if (L->block_count == bcap) {
            size_t new_cap = bcap ? bcap * 2u : 8u;
            tinydng_psd_block *grown = (tinydng_psd_block *)td_ctx_realloc(
                ctx, L->blocks, bcap * sizeof(tinydng_psd_block),
                new_cap * sizeof(tinydng_psd_block), err);
            if (!grown) {
              return TINYDNG_E_OOM;
            }
            L->blocks = grown;
            bcap = new_cap;
          }
          L->blocks[L->block_count++] = blk;

          /* Interpret well-known blocks eagerly. */
          if (blk.key == TD_PSD_FOURCC('l', 'u', 'n', 'i')) {
            uint64_t adv = 0;
            char *uni = td_psd_read_unicode(ctx, r, blk.offset,
                                            blk.offset + blk.length, &adv,
                                            err);
            if (uni) {
              td_ctx_free(ctx, L->name);
              L->name = uni;
            } else {
              tinydng_error_clear(err);
            }
          } else if (blk.key == TD_PSD_FOURCC('l', 's', 'c', 't') &&
                     blk.length >= 4u) {
            uint32_t sect;
            if (td_r_u32(r, blk.offset, &sect) && sect <= 3u) {
              L->section = (uint8_t)sect;
            }
          }
        }
      }
    }
    cur = extra_end;
  }

  /* --- channel image data: u16 compression + payload per channel --- */
  for (li = 0; li < count; li++) {
    tinydng_psd_layer *L = &psd->layers[li];
    size_t c;
    for (c = 0; c < L->channel_count; c++) {
      tinydng_psd_channel *ch = &L->channels[c];
      uint64_t clen = ch->data_length;
      uint16_t comp;
      if (clen < 2u) {
        /* Length 0 channels appear in some writers: treat as empty RAW. */
        if (clen != 0u) {
          td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_METADATA, 0, 0,
                       cur, "PSD: channel length %llu invalid",
                       (unsigned long long)clen);
          return TINYDNG_E_PARSE;
        }
        ch->compression = TINYDNG_PSD_COMP_RAW;
        ch->data_offset = cur;
        ch->data_length = 0u;
        continue;
      }
      if (clen > end - cur) {
        td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_METADATA, 0, 0, cur,
                     "PSD: channel data overruns layer section");
        return TINYDNG_E_PARSE;
      }
      if (!td_r_u16(r, cur, &comp)) {
        return TINYDNG_E_PARSE;
      }
      if (comp > TINYDNG_PSD_COMP_ZIP_PRED) {
        td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_METADATA, 0, 0, cur,
                     "PSD: unknown channel compression %u", (unsigned)comp);
        return TINYDNG_E_PARSE;
      }
      ch->compression = comp;
      ch->data_offset = cur + 2u;
      ch->data_length = clen - 2u;
      cur += clen;
    }
  }

  /* --- group tree: walk top-down (reverse file order) with a stack --- */
  {
    uint32_t *stack;
    size_t sp = 0;
    size_t i;
    stack = (uint32_t *)td_ctx_alloc(
        ctx, (size_t)count * sizeof(uint32_t), err);
    if (!stack) {
      return TINYDNG_E_OOM;
    }
    for (i = count; i-- > 0u;) {
      tinydng_psd_layer *L = &psd->layers[i];
      L->parent = sp ? stack[sp - 1u] : UINT32_MAX;
      if (L->section == TINYDNG_PSD_SECTION_OPEN_FOLDER ||
          L->section == TINYDNG_PSD_SECTION_CLOSED_FOLDER) {
        stack[sp++] = (uint32_t)i;
      } else if (L->section == TINYDNG_PSD_SECTION_DIVIDER && sp > 0u) {
        sp--;
      }
    }
    td_ctx_free(ctx, stack);
  }
  return st;
}

/* Parse a full "layer info" payload (its own length field first). Used for
   the main sub-section and for the Layr/Lr16/Lr32 tagged blocks. */
static tinydng_status td_psd_parse_layer_info(tinydng_context *ctx,
                                              td_reader *r,
                                              tinydng_psd_info *psd,
                                              uint64_t at, uint64_t end,
                                              tinydng_error *err) {
  uint64_t len;
  uint64_t len_field = psd->is_psb ? 8u : 4u;
  uint64_t body, body_end;
  if (end - at < len_field) {
    return TINYDNG_OK; /* empty */
  }
  if (!td_psd_r_len(r, at, psd->is_psb, &len)) {
    td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_METADATA, 0, 0, at,
                 "PSD: truncated layer info length");
    return TINYDNG_E_PARSE;
  }
  body = at + len_field;
  if (len > end - body) {
    td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_METADATA, 0, 0, at,
                 "PSD: layer info overruns section");
    return TINYDNG_E_PARSE;
  }
  if (len == 0u) {
    return TINYDNG_OK;
  }
  body_end = body + len;
  return td_psd_parse_layer_records(ctx, r, psd, body, body_end, err);
}

/* ------------------------------------------------------------------ */
/* Smart-object link blocks                                           */
/* ------------------------------------------------------------------ */

static const uint32_t td_psd_link_keys[] = {
    TD_PSD_FOURCC('l', 'n', 'k', '2'), TD_PSD_FOURCC('l', 'n', 'k', '3'),
    TD_PSD_FOURCC('l', 'n', 'k', 'D'), TD_PSD_FOURCC('l', 'n', 'k', 'E')};

static int td_psd_is_link_key(uint32_t key) {
  size_t i;
  for (i = 0; i < sizeof(td_psd_link_keys) / sizeof(td_psd_link_keys[0]);
       i++) {
    if (key == td_psd_link_keys[i]) {
      return 1;
    }
  }
  return 0;
}

/* Scan forward in [from, limit) for a known embedded-file magic; returns the
   absolute offset or 0. Bounded by the entry, so at most a few KB when the
   descriptor heuristic is needed. */
static uint64_t td_psd_scan_magic(const td_reader *r, uint64_t from,
                                  uint64_t limit) {
  uint64_t p = from;
  uint8_t scratch[TD_PSD_SCAN_CHUNK];
  while (p < limit && limit - p >= 4u) {
    uint64_t remain = limit - p;
    size_t want = (remain > sizeof(scratch)) ? sizeof(scratch) :
                  (size_t)remain;
    const uint8_t *v = td_io_view(r->io, r->size, p, want, scratch,
                                 sizeof(scratch));
    size_t i;
    if (!v) {
      return 0;
    }
    for (i = 0; i + 4u <= want; i++) {
      const uint8_t *q = v + i;
      if ((q[0] == '8' && q[1] == 'B' && q[2] == 'P' && q[3] == 'S') ||
          (q[0] == 0xFFu && q[1] == 0xD8u && q[2] == 0xFFu) ||
          (q[0] == 0x89u && q[1] == 'P' && q[2] == 'N' && q[3] == 'G') ||
          (q[0] == 'I' && q[1] == 'I' && q[2] == 0x2Au && q[3] == 0x00u) ||
          (q[0] == 'M' && q[1] == 'M' && q[2] == 0x00u &&
           (q[3] == 0x2Au || q[3] == 0x2Bu))) {
        return p + (uint64_t)i;
      }
    }
    if (want <= 3u) {
      break;
    }
    p += (uint64_t)(want - 3u);
  }
  return 0;
}

static tinydng_status td_psd_parse_link_block(tinydng_context *ctx,
                                              td_reader *r,
                                              tinydng_psd_info *psd,
                                              const tinydng_psd_block *blk,
                                              tinydng_error *err) {
  uint64_t cur = blk->offset;
  uint64_t end;
  if (!td_safe_add_u64(blk->offset, blk->length, &end)) {
    return TINYDNG_E_BOUNDS;
  }
  size_t cap = psd->smart_object_count;

  while (end - cur >= 8u && cur < end) {
    uint64_t entry_len, entry_end, p;
    uint32_t kind, version;
    char *uid = NULL, *fname = NULL;
    uint32_t filetype = 0;
    uint64_t data_len = 0, data_off = 0;
    uint64_t adv = 0;
    uint8_t has_desc = 0;

    if (!td_r_u64(r, cur, &entry_len)) {
      break;
    }
    if (entry_len < 8u || cur > end || end - cur < 8u ||
        entry_len > end - cur - 8u) {
      break;
    }
    p = cur + 8u;
    if (!td_safe_add_u64(p, entry_len, &entry_end)) {
      break;
    }

    if (!td_r_u32(r, p, &kind)) {
      break;
    }
    if (kind != TD_PSD_FOURCC('l', 'i', 'F', 'D') &&
        kind != TD_PSD_FOURCC('l', 'i', 'F', 'E') &&
        kind != TD_PSD_FOURCC('l', 'i', 'F', 'A')) {
      break;
    }
    if (!td_r_u32(r, p + 4u, &version)) {
      break;
    }
    p += 8u;

    if (!td_psd_read_pascal(ctx, r, p, entry_end, 1u, &uid, &adv, err)) {
      tinydng_error_clear(err);
      break;
    }
    p += adv;
    fname = td_psd_read_unicode(ctx, r, p, entry_end, &adv, err);
    if (!fname) {
      tinydng_error_clear(err);
      td_ctx_free(ctx, uid);
      break;
    }
    p += adv;
    if (entry_end - p < 17u || !td_r_u32(r, p, &filetype) ||
        !td_r_u64(r, p + 8u, &data_len)) {
      td_ctx_free(ctx, uid);
      td_ctx_free(ctx, fname);
      break;
    }
    /* p+4: creator fourcc (ignored) */
    {
      uint8_t b1[1];
      const uint8_t *v =
          td_io_view(r->io, r->size, p + 16u, 1u, b1, sizeof(b1));
      if (!v) {
        td_ctx_free(ctx, uid);
        td_ctx_free(ctx, fname);
        break;
      }
      has_desc = v[0];
    }
    p += 17u;

    if (kind == TD_PSD_FOURCC('l', 'i', 'F', 'D') && data_len > 0u &&
        data_len <= entry_end - p) {
      if (!has_desc) {
        data_off = p;
      } else {
        /* A file-open descriptor precedes the payload; we do not parse
           descriptors, so locate the payload by its magic (bounded scan). */
        uint64_t hit = td_psd_scan_magic(r, p, entry_end);
        if (hit != 0u && data_len <= entry_end - hit) {
          data_off = hit;
        }
      }
    }

    if (psd->smart_object_count >= TD_PSD_MAX_SMART_OBJECTS) {
      td_ctx_free(ctx, uid);
      td_ctx_free(ctx, fname);
      break;
    }
    if (psd->smart_object_count == cap) {
      size_t new_cap = cap ? cap * 2u : 4u;
      tinydng_psd_smart_object *grown =
          (tinydng_psd_smart_object *)td_ctx_realloc(
              ctx, psd->smart_objects,
              cap * sizeof(tinydng_psd_smart_object),
              new_cap * sizeof(tinydng_psd_smart_object), err);
      if (!grown) {
        td_ctx_free(ctx, uid);
        td_ctx_free(ctx, fname);
        return TINYDNG_E_OOM;
      }
      psd->smart_objects = grown;
      cap = new_cap;
    }
    {
      tinydng_psd_smart_object *so =
          &psd->smart_objects[psd->smart_object_count++];
      so->kind = kind;
      so->uid = uid;
      so->filename = fname;
      so->filetype = filetype;
      so->data_offset = data_off;
      so->data_length = (data_off != 0u) ? data_len : 0u;
    }

    /* Entries are aligned to 4 bytes. */
    {
      uint64_t next = entry_end;
      next += (4u - (next & 3u)) & 3u;
      if (next <= cur || next > end) {
        break;
      }
      cur = next;
    }
  }
  return TINYDNG_OK;
}

/* ------------------------------------------------------------------ */
/* Section: layer and mask information                                */
/* ------------------------------------------------------------------ */

static tinydng_status td_psd_parse_layer_mask_info(tinydng_context *ctx,
                                                   td_reader *r,
                                                   tinydng_psd_info *psd,
                                                   uint64_t at,
                                                   uint64_t *next,
                                                   tinydng_error *err) {
  uint64_t total;
  uint64_t len_field = psd->is_psb ? 8u : 4u;
  uint64_t body, end, cur;
  tinydng_status st;
  const tinydng_psd_block *lr_block = NULL;

  if (r->size - at < len_field) {
    td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_METADATA, 0, 0, at,
                 "PSD: truncated layer/mask section length");
    return TINYDNG_E_PARSE;
  }
  if (!td_psd_r_len(r, at, psd->is_psb, &total)) {
    return TINYDNG_E_PARSE;
  }
  body = at + len_field;
  if (total > r->size - body) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_METADATA, 0, 0, at,
                 "PSD: layer/mask section overruns file");
    return TINYDNG_E_BOUNDS;
  }
  end = body + total;
  *next = end;
  if (total == 0u) {
    return TINYDNG_OK;
  }

  /* Layer info sub-section. */
  st = td_psd_parse_layer_info(ctx, r, psd, body, end, err);
  if (st != TINYDNG_OK) {
    return st;
  }
  {
    uint64_t li_len;
    if (!td_psd_r_len(r, body, psd->is_psb, &li_len)) {
      return TINYDNG_OK; /* already validated as empty */
    }
    li_len += li_len & 1u; /* even alignment */
    if (li_len > end - (body + len_field)) {
      li_len = end - (body + len_field);
    }
    cur = body + len_field + li_len;
  }

  /* Global layer mask info. */
  if (end - cur >= 4u) {
    uint32_t glm_len;
    if (td_r_u32(r, cur, &glm_len) && (uint64_t)glm_len <= end - (cur + 4u)) {
      cur += 4u + glm_len;
    } else {
      cur = end;
    }
  }

  /* Global tagged blocks. */
  {
    size_t cap = 0;
    for (;;) {
      tinydng_psd_block blk;
      int rc = td_psd_read_tagged_block(r, psd->is_psb, &cur, end, &blk, err);
      if (rc <= 0) {
        break;
      }
      if (psd->global_block_count >= TD_PSD_MAX_GLOBAL_BLOCKS) {
        break;
      }
      if (psd->global_block_count == cap) {
        size_t new_cap = cap ? cap * 2u : 8u;
        tinydng_psd_block *grown = (tinydng_psd_block *)td_ctx_realloc(
            ctx, psd->global_blocks, cap * sizeof(tinydng_psd_block),
            new_cap * sizeof(tinydng_psd_block), err);
        if (!grown) {
          return TINYDNG_E_OOM;
        }
        psd->global_blocks = grown;
        cap = new_cap;
      }
      psd->global_blocks[psd->global_block_count++] = blk;
    }
  }

  /* 16/32-bit documents store the real layers in Lr16/Lr32 blocks. */
  if (psd->layer_count == 0u) {
    size_t i;
    for (i = 0; i < psd->global_block_count; i++) {
      uint32_t key = psd->global_blocks[i].key;
      if (key == TD_PSD_FOURCC('L', 'r', '1', '6') ||
          key == TD_PSD_FOURCC('L', 'r', '3', '2') ||
          key == TD_PSD_FOURCC('L', 'a', 'y', 'r')) {
        lr_block = &psd->global_blocks[i];
        break;
      }
    }
    if (lr_block && lr_block->length >= 2u) {
      /* Block data starts directly at the i16 layer count (unlike the main
         sub-section there is no inner length field). */
      st = td_psd_parse_layer_records(ctx, r, psd, lr_block->offset,
                                      lr_block->offset + lr_block->length,
                                      err);
      if (st != TINYDNG_OK) {
        return st;
      }
    }
  }

  /* Smart-object link blocks. */
  {
    size_t i;
    for (i = 0; i < psd->global_block_count; i++) {
      if (td_psd_is_link_key(psd->global_blocks[i].key)) {
        st = td_psd_parse_link_block(ctx, r, psd, &psd->global_blocks[i],
                                     err);
        if (st != TINYDNG_OK) {
          return st;
        }
      }
    }
  }
  return TINYDNG_OK;
}

/* ------------------------------------------------------------------ */
/* Section: composite image data -> segment table                     */
/* ------------------------------------------------------------------ */

static tinydng_status td_psd_build_composite(tinydng_context *ctx,
                                             td_reader *r,
                                             tinydng_document *doc,
                                             tinydng_psd_info *psd,
                                             uint64_t at, tinydng_error *err) {
  uint16_t comp;
  tinydng_image_info *img;
  uint64_t row_bytes, plane_bytes;
  uint32_t channels = psd->channel_count;

  if (r->size < at || r->size - at < 2u || !td_r_u16(r, at, &comp) ||
      comp > TINYDNG_PSD_COMP_ZIP_PRED) {
    /* Composite missing/garbled: document opens, metadata-only. */
    psd->has_composite = 0;
    return TINYDNG_OK;
  }
  psd->has_composite = 1;
  psd->composite_compression = comp;

  doc->images =
      (tinydng_image_info *)td_ctx_calloc(ctx, sizeof(*doc->images), err);
  if (!doc->images) {
    return TINYDNG_E_OOM;
  }
  doc->image_count = 1;
  img = &doc->images[0];
  img->width = psd->width;
  img->height = psd->height;
  img->samples_per_pixel = (uint16_t)channels;
  img->bits_per_sample = psd->depth;
  img->bits_per_sample_decoded =
      (psd->depth <= 8u) ? 8u : psd->depth;
  img->sample_format = (psd->depth == 32u) ? TINYDNG_SAMPLEFORMAT_IEEEFP
                                           : TINYDNG_SAMPLEFORMAT_UINT;
  img->planar_configuration = (channels > 1u) ? 2u : 1u;
  img->predictor = 1u;

  {
    uint64_t bits;
    if (!td_safe_mul_u64((uint64_t)psd->width, psd->depth, &bits)) {
      return TINYDNG_E_BOUNDS;
    }
    row_bytes = bits / 8u + ((bits % 8u) != 0u);
    if (!td_safe_mul_u64(row_bytes, (uint64_t)psd->height, &plane_bytes)) {
      return TINYDNG_E_BOUNDS;
    }
  }

  if (comp == TINYDNG_PSD_COMP_RAW) {
    tinydng_segment *segs;
    uint32_t c;
    size_t bytes;
    img->compression = TINYDNG_COMPRESSION_NONE;
    img->rows_per_strip = psd->height;
    if (at > r->size - 2u ||
        (channels != 0u && plane_bytes >
         (r->size - (at + 2u)) / (uint64_t)channels)) {
      td_ctx_free(ctx, doc->images);
      doc->images = NULL;
      doc->image_count = 0;
      psd->has_composite = 0;
      return TINYDNG_OK;
    }
    if (!td_safe_mul_size((size_t)channels, sizeof(tinydng_segment),
                          &bytes)) {
      return TINYDNG_E_BOUNDS;
    }
    segs = (tinydng_segment *)td_ctx_calloc(ctx, bytes, err);
    if (!segs) {
      return TINYDNG_E_OOM;
    }
    for (c = 0; c < channels; c++) {
      segs[c].offset = at + 2u + (uint64_t)c * plane_bytes;
      segs[c].byte_count = plane_bytes;
      segs[c].index = c;
      segs[c].x = 0;
      segs[c].y = 0;
      segs[c].w = psd->width;
      segs[c].h = psd->height;
      segs[c].plane = (uint16_t)c;
      segs[c].kind = TINYDNG_SEG_STRIP;
    }
    img->segments = segs;
    img->segment_count = channels;
    return TINYDNG_OK;
  }

  if (comp == TINYDNG_PSD_COMP_RLE) {
    uint64_t entries64, table_bytes;
    size_t entries, i, ebytes;
    uint32_t esz = psd->is_psb ? 4u : 2u;
    uint8_t *table;
    tinydng_segment *segs;
    uint64_t data_off, running;
    size_t seg_bytes;

    img->compression = TINYDNG_COMPRESSION_PACKBITS;
    img->rows_per_strip = 1u;

    if (!td_safe_mul_u64((uint64_t)psd->height, channels, &entries64) ||
        !td_safe_mul_u64(entries64, esz, &table_bytes) ||
        table_bytes > r->size - (at + 2u)) {
      td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_METADATA, 0, 0, at,
                   "PSD: RLE row table overruns file");
      /* Treat as missing composite rather than failing the whole open. */
      tinydng_error_clear(err);
      td_ctx_free(ctx, doc->images);
      doc->images = NULL;
      doc->image_count = 0;
      psd->has_composite = 0;
      return TINYDNG_OK;
    }
    entries = (size_t)entries64;
    if (!td_safe_mul_size(entries, (size_t)esz, &ebytes) ||
        !td_safe_mul_size(entries, sizeof(tinydng_segment), &seg_bytes)) {
      return TINYDNG_E_BOUNDS;
    }
    table = td_psd_read_copy(ctx, r, at + 2u, ebytes, err);
    if (!table) {
      return td_error_status_or(err, TINYDNG_E_OOM);
    }
    segs = (tinydng_segment *)td_ctx_calloc(ctx, seg_bytes, err);
    if (!segs) {
      td_ctx_free(ctx, table);
      return TINYDNG_E_OOM;
    }
    data_off = at + 2u + table_bytes;
    running = data_off;
    for (i = 0; i < entries; i++) {
      uint64_t n;
      uint32_t c = (uint32_t)(i / psd->height);
      uint32_t row = (uint32_t)(i % psd->height);
      if (esz == 2u) {
        n = ((uint64_t)table[2u * i] << 8) | table[2u * i + 1u];
      } else {
        n = ((uint64_t)table[4u * i] << 24) |
            ((uint64_t)table[4u * i + 1u] << 16) |
            ((uint64_t)table[4u * i + 2u] << 8) | table[4u * i + 3u];
      }
      segs[i].offset = running;
      segs[i].byte_count = n;
      segs[i].index = (uint32_t)i;
      segs[i].x = 0;
      segs[i].y = row;
      segs[i].w = psd->width;
      segs[i].h = 1u;
      segs[i].plane = (uint16_t)c;
      segs[i].kind = TINYDNG_SEG_STRIP;
      if (!td_safe_add_u64(running, n, &running)) {
        td_ctx_free(ctx, table);
        td_ctx_free(ctx, segs);
        td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_METADATA, 0, 0, at,
                     "PSD: RLE offsets overflow");
        return TINYDNG_E_BOUNDS;
      }
    }
    td_ctx_free(ctx, table);
    img->segments = segs;
    img->segment_count = entries;
    return TINYDNG_OK;
  }

  /* ZIP / ZIP-with-prediction composite: a single zlib stream holds all
     planes, which cannot be split into per-plane segments without
     decompressing. Photoshop itself never writes this; supported only for
     single-channel documents (then the stream is exactly one plane). */
  if (channels == 1u) {
    tinydng_segment *segs =
        (tinydng_segment *)td_ctx_calloc(ctx, sizeof(*segs), err);
    if (!segs) {
      return TINYDNG_E_OOM;
    }
    img->compression = (comp == TINYDNG_PSD_COMP_ZIP)
                           ? TINYDNG_COMPRESSION_ZIP
                           : (uint16_t)TD_COMPRESSION_PSD_ZIP_PRED;
    img->rows_per_strip = psd->height;
    segs[0].offset = at + 2u;
    segs[0].byte_count = r->size - (at + 2u);
    segs[0].index = 0;
    segs[0].x = 0;
    segs[0].y = 0;
    segs[0].w = psd->width;
    segs[0].h = psd->height;
    segs[0].plane = 0;
    segs[0].kind = TINYDNG_SEG_STRIP;
    img->segments = segs;
    img->segment_count = 1;
  } else {
    img->compression = (comp == TINYDNG_PSD_COMP_ZIP)
                           ? TINYDNG_COMPRESSION_ZIP
                           : (uint16_t)TD_COMPRESSION_PSD_ZIP_PRED;
    img->segment_count = 0; /* decode unavailable; metadata intact */
  }
  return TINYDNG_OK;
}

/* ------------------------------------------------------------------ */
/* td_psd_open                                                        */
/* ------------------------------------------------------------------ */

tinydng_status td_psd_open(tinydng_context *ctx, td_reader *r,
                           tinydng_document *doc, uint32_t open_flags,
                           tinydng_error *err) {
  tinydng_psd_info *psd;
  tinydng_status st;
  uint64_t at, next;
  (void)open_flags;

  if (r->size < 26u) {
    td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_HEADER, 0, 0, 0,
                 "PSD: file smaller than header");
    return TINYDNG_E_PARSE;
  }
  psd = (tinydng_psd_info *)td_ctx_calloc(ctx, sizeof(*psd), err);
  if (!psd) {
    return TINYDNG_E_OOM;
  }
  doc->psd = psd; /* owned by the document from here on */

  st = td_psd_parse_header(ctx, r, psd, err);
  if (st != TINYDNG_OK) {
    return st;
  }
  at = 26u;
  st = td_psd_parse_color_mode(ctx, r, psd, at, &next, err);
  if (st != TINYDNG_OK) {
    return st;
  }
  at = next;
  st = td_psd_parse_resources(ctx, r, psd, at, &next, err);
  if (st != TINYDNG_OK) {
    return st;
  }
  at = next;
  st = td_psd_parse_layer_mask_info(ctx, r, psd, at, &next, err);
  if (st != TINYDNG_OK) {
    return st;
  }
  at = next;
  return td_psd_build_composite(ctx, r, doc, psd, at, err);
}

/* ------------------------------------------------------------------ */
/* Raw block access                                                   */
/* ------------------------------------------------------------------ */

tinydng_status tinydng_psd_read_block(tinydng_context *ctx,
                                      const tinydng_document *doc,
                                      uint64_t offset, uint64_t length,
                                      uint8_t **out_data, size_t *out_size,
                                      tinydng_error *err) {
  uint8_t *buf;
  tinydng_error_clear(err);
  if (!ctx || !doc || !out_data || !out_size) {
    return TINYDNG_E_INVALID_ARG;
  }
  *out_data = NULL;
  *out_size = 0;
  if (length > (uint64_t)SIZE_MAX || offset > doc->io_size ||
      length > doc->io_size - offset) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_IO, 0, 0, offset,
                 "PSD: block range out of file");
    return TINYDNG_E_BOUNDS;
  }
  {
    td_reader r;
    memset(&r, 0, sizeof(r));
    r.io = (tinydng_io *)&doc->io;
    r.size = doc->io_size;
    r.big_endian = 1;
    buf = td_psd_read_copy(ctx, &r, offset, (size_t)length, err);
  }
  if (!buf) {
    return td_error_status_or(err, TINYDNG_E_OOM);
  }
  *out_data = buf;
  *out_size = (size_t)length;
  return TINYDNG_OK;
}

/* ------------------------------------------------------------------ */
/* Layer / channel decode                                             */
/* ------------------------------------------------------------------ */

typedef struct td_psd_chan_task {
  tinydng_context *ctx;
  const tinydng_document *doc;
  const tinydng_psd_channel *ch;
  uint32_t w, h;        /* channel rect */
  uint16_t depth;       /* document depth */
  int is_psb;
  uint8_t *dst;         /* first sample of this channel's slot */
  size_t pixel_stride;  /* bytes between consecutive pixels in dst */
  size_t out_bytes;     /* bytes per output sample (1/2/4) */
  int invert1;          /* 1-bit: emit bit?0:255 */
  tinydng_error err;
  int ok;
} td_psd_chan_task;

/* Decompress one channel into a stored-bytes plane (row_bytes * h,
   big-endian, packed for depth 1). Returns the plane or NULL. */
static uint8_t *td_psd_load_plane(tinydng_context *ctx,
                                  const tinydng_document *doc,
                                  const tinydng_psd_channel *ch, uint32_t w,
                                  uint32_t h, uint16_t depth, int is_psb,
                                  size_t *out_plane_bytes,
                                  tinydng_error *err) {
  td_reader r;
  size_t row_bytes, plane_bytes;
  uint8_t *plane;

  memset(&r, 0, sizeof(r));
  r.io = (tinydng_io *)&doc->io;
  r.size = doc->io_size;
  r.big_endian = 1;
  (void)is_psb; /* only consulted on the RLE (PackBits) path */

  {
    size_t bits;
    if (!td_safe_mul_size((size_t)w, (size_t)depth, &bits)) {
      td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_DECODE, 0, 0, 0,
                   "PSD: channel row size overflow");
      return NULL;
    }
    row_bytes = bits / 8u + ((bits % 8u) != 0u);
  }
  if (!td_safe_mul_size(row_bytes, (size_t)h, &plane_bytes) ||
      plane_bytes == 0u) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_DECODE, 0, 0, 0,
                 "PSD: channel plane size invalid");
    return NULL;
  }
  plane = (uint8_t *)td_ctx_alloc(ctx, plane_bytes, err);
  if (!plane) {
    return NULL;
  }

  switch (ch->compression) {
    case TINYDNG_PSD_COMP_RAW: {
      if (ch->data_length < plane_bytes) {
        td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_DECODE, 0, 0,
                     ch->data_offset, "PSD: raw channel too small");
        goto fail;
      }
      if (!td_io_view(r.io, r.size, ch->data_offset, plane_bytes, plane,
                      plane_bytes)) {
        td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_DECODE, 0, 0,
                     ch->data_offset, "PSD: raw channel out of file");
        goto fail;
      }
      {
        const uint8_t *v = td_io_view(r.io, r.size, ch->data_offset,
                                      plane_bytes, plane, plane_bytes);
        if (v != plane) {
          memcpy(plane, v, plane_bytes);
        }
      }
      break;
    }
#ifndef TINYDNG_NO_PACKBITS
    case TINYDNG_PSD_COMP_RLE: {
      uint32_t esz = is_psb ? 4u : 2u;
      size_t table_bytes;
      uint8_t *table;
      uint8_t *rowbuf = NULL;
      size_t rowbuf_cap = 0;
      uint64_t running;
      uint32_t y;
      if (!td_safe_mul_size((size_t)h, (size_t)esz, &table_bytes) ||
          (uint64_t)table_bytes > ch->data_length) {
        td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_DECODE, 0, 0,
                     ch->data_offset, "PSD: RLE channel table too small");
        goto fail;
      }
      table = td_psd_read_copy(ctx, &r, ch->data_offset, table_bytes, err);
      if (!table) {
        goto fail;
      }
      running = ch->data_offset + table_bytes;
      for (y = 0; y < h; y++) {
        uint64_t n;
        const uint8_t *src;
        long got;
        if (esz == 2u) {
          n = ((uint64_t)table[2u * y] << 8) | table[2u * y + 1u];
        } else {
          n = ((uint64_t)table[4u * y] << 24) |
              ((uint64_t)table[4u * y + 1u] << 16) |
              ((uint64_t)table[4u * y + 2u] << 8) | table[4u * y + 3u];
        }
        if (n == 0u ||
            n > ch->data_length - (running - ch->data_offset)) {
          td_ctx_free(ctx, table);
          td_ctx_free(ctx, rowbuf);
          td_set_error(err, TINYDNG_E_DECODE, TINYDNG_STAGE_DECODE, 0, 0,
                       running, "PSD: RLE row length invalid");
          goto fail;
        }
        if ((size_t)n > rowbuf_cap) {
          td_ctx_free(ctx, rowbuf);
          rowbuf = (uint8_t *)td_ctx_alloc(ctx, (size_t)n, err);
          if (!rowbuf) {
            td_ctx_free(ctx, table);
            goto fail;
          }
          rowbuf_cap = (size_t)n;
        }
        src = td_io_view(r.io, r.size, running, (size_t)n, rowbuf,
                         rowbuf_cap);
        if (!src) {
          td_ctx_free(ctx, table);
          td_ctx_free(ctx, rowbuf);
          td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_DECODE, 0, 0,
                       running, "PSD: RLE row out of file");
          goto fail;
        }
        got = td_packbits_decode(src, (size_t)n, plane + (size_t)y * row_bytes,
                                 row_bytes);
        if (got != (long)row_bytes) {
          td_ctx_free(ctx, table);
          td_ctx_free(ctx, rowbuf);
          td_set_error(err, TINYDNG_E_DECODE, TINYDNG_STAGE_DECODE, 0, 0,
                       running, "PSD: RLE row decode mismatch");
          goto fail;
        }
        if (!td_safe_add_u64(running, n, &running)) {
          td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_DECODE, 0, 0,
                       running, "PSD: RLE data offset overflow");
          goto fail;
        }
      }
      td_ctx_free(ctx, table);
      td_ctx_free(ctx, rowbuf);
      break;
    }
#endif /* TINYDNG_NO_PACKBITS */
#ifndef TINYDNG_NO_ZIP
    case TINYDNG_PSD_COMP_ZIP:
    case TINYDNG_PSD_COMP_ZIP_PRED: {
      uint8_t *comp_buf;
      mz_ulong dlen = (mz_ulong)plane_bytes;
      int mzr;
      if (ch->data_length > (uint64_t)INT32_MAX ||
          ch->data_length == 0u) {
        td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_DECODE, 0, 0,
                     ch->data_offset, "PSD: zip channel size invalid");
        goto fail;
      }
      comp_buf = td_psd_read_copy(ctx, &r, ch->data_offset,
                                  (size_t)ch->data_length, err);
      if (!comp_buf) {
        goto fail;
      }
      mzr = mz_uncompress(plane, &dlen, comp_buf,
                          (mz_ulong)ch->data_length);
      td_ctx_free(ctx, comp_buf);
      if (mzr != MZ_OK || dlen != (mz_ulong)plane_bytes) {
        td_set_error(err, TINYDNG_E_DECODE, TINYDNG_STAGE_DECODE, 0, 0,
                     ch->data_offset, "PSD: zip channel inflate failed");
        goto fail;
      }
      if (ch->compression == TINYDNG_PSD_COMP_ZIP_PRED &&
          !td_psd_unpredict_plane(ctx, plane, w, h, depth, err)) {
        goto fail;
      }
      break;
    }
#endif /* TINYDNG_NO_ZIP */
    default:
      td_set_error(err, TINYDNG_E_UNSUPPORTED, TINYDNG_STAGE_DECODE, 0, 0,
                   ch->data_offset, "PSD: channel compression %u disabled",
                   (unsigned)ch->compression);
      goto fail;
  }
  *out_plane_bytes = plane_bytes;
  return plane;

fail:
  td_ctx_free(ctx, plane);
  return NULL;
}

/* Convert one stored plane to host samples scattered into dst. */
static void td_psd_scatter_plane(const uint8_t *plane, uint32_t w, uint32_t h,
                                 uint16_t depth, uint8_t *dst,
                                 size_t pixel_stride, int invert1) {
  size_t n_pixels;
  if (!td_safe_mul_size((size_t)w, (size_t)h, &n_pixels)) {
    return;
  }
  size_t i;
  if (depth == 8u) {
    for (i = 0; i < n_pixels; i++) {
      dst[i * pixel_stride] = plane[i];
    }
  } else if (depth == 16u) {
    for (i = 0; i < n_pixels; i++) {
      uint16_t v = (uint16_t)(((uint16_t)plane[2u * i] << 8) |
                              plane[2u * i + 1u]);
      memcpy(dst + i * pixel_stride, &v, 2u);
    }
  } else if (depth == 32u) {
    for (i = 0; i < n_pixels; i++) {
      uint32_t v = ((uint32_t)plane[4u * i] << 24) |
                   ((uint32_t)plane[4u * i + 1u] << 16) |
                   ((uint32_t)plane[4u * i + 2u] << 8) | plane[4u * i + 3u];
      memcpy(dst + i * pixel_stride, &v, 4u);
    }
  } else { /* depth 1: packed MSB-first, rows byte-aligned */
    size_t row_bytes = (size_t)w / 8u + (((size_t)w % 8u) != 0u);
    uint32_t y, x;
    for (y = 0; y < h; y++) {
      const uint8_t *rp = plane + (size_t)y * row_bytes;
      uint8_t *dp = dst + (size_t)y * w * pixel_stride;
      for (x = 0; x < w; x++) {
        uint8_t bit = (uint8_t)((rp[x >> 3] >> (7u - (x & 7u))) & 1u);
        uint8_t v = invert1 ? (uint8_t)(bit ? 0u : 255u)
                            : (uint8_t)(bit ? 255u : 0u);
        dp[(size_t)x * pixel_stride] = v;
      }
    }
  }
}

static void *td_psd_chan_worker(void *p) {
  td_psd_chan_task *t = (td_psd_chan_task *)p;
  size_t plane_bytes = 0;
  uint8_t *plane =
      td_psd_load_plane(t->ctx, t->doc, t->ch, t->w, t->h, t->depth,
                        t->is_psb, &plane_bytes, &t->err);
  if (!plane) {
    t->ok = 0;
    return NULL;
  }
  td_psd_scatter_plane(plane, t->w, t->h, t->depth, t->dst, t->pixel_stride,
                       t->invert1);
  td_ctx_free(t->ctx, plane);
  t->ok = 1;
  return NULL;
}

/* Run channel tasks, in parallel when requested and possible. */
static tinydng_status td_psd_run_tasks(tinydng_context *ctx,
                                       td_psd_chan_task *tasks, size_t n,
                                       const tinydng_decode_options *opts,
                                       tinydng_error *err) {
  unsigned want = (opts && opts->num_threads) ? opts->num_threads
                                              : td_cpu_count();
  size_t i;
  if (n > 1u && want > 1u && ctx->lock != NULL && td_threads_available()) {
    unsigned run = (n > (size_t)TD_MAX_DECODE_THREADS)
                       ? TD_MAX_DECODE_THREADS
                       : (unsigned)n;
    (void)run;
    ctx->mt_active = 1;
    td_threads_run(td_psd_chan_worker, tasks, sizeof(*tasks), (unsigned)n);
    ctx->mt_active = 0;
  } else {
    for (i = 0; i < n; i++) {
      td_psd_chan_worker(&tasks[i]);
    }
  }
  for (i = 0; i < n; i++) {
    if (!tasks[i].ok) {
      if (err) {
        *err = tasks[i].err;
      }
      return tasks[i].err.status != TINYDNG_OK ? tasks[i].err.status
                                               : TINYDNG_E_DECODE;
    }
  }
  return TINYDNG_OK;
}

tinydng_status tinydng_psd_decode_layer(tinydng_context *ctx,
                                        const tinydng_document *doc,
                                        size_t layer_idx,
                                        const tinydng_decode_options *opts,
                                        tinydng_pixels *out,
                                        tinydng_error *err) {
  const tinydng_psd_info *psd;
  const tinydng_psd_layer *L;
  uint16_t out_bps;
  size_t out_bytes, spp = 0, alpha_slot = 0, n_img = 0;
  size_t pixel_stride, total, i;
  uint8_t *dst = NULL;
  uint8_t owns = 0;
  td_psd_chan_task tasks[TD_PSD_MAX_LAYER_CHANNELS];
  size_t n_tasks = 0;
  tinydng_status st;

  tinydng_error_clear(err);
  if (!ctx || !doc || !out) {
    return TINYDNG_E_INVALID_ARG;
  }
  memset(out, 0, sizeof(*out));
  psd = tinydng_document_psd(doc);
  if (!psd || layer_idx >= psd->layer_count) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_DECODE, 0, 0, 0,
                 "PSD: layer index out of range");
    return TINYDNG_E_INVALID_ARG;
  }
  L = &psd->layers[layer_idx];
  if (L->width == 0u || L->height == 0u) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_DECODE, 0, 0, 0,
                 "PSD: layer %zu has an empty rect", layer_idx);
    return TINYDNG_E_INVALID_ARG;
  }

  /* Output channels: image channels ordered by id, then transparency. */
  {
    int has_alpha = 0;
    for (i = 0; i < L->channel_count; i++) {
      if (L->channels[i].id >= 0) {
        n_img++;
      } else if (L->channels[i].id == -1) {
        has_alpha = 1;
      }
    }
    spp = n_img + (has_alpha ? 1u : 0u);
    alpha_slot = n_img;
  }
  if (spp == 0u || spp > TD_PSD_MAX_LAYER_CHANNELS) {
    td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_DECODE, 0, 0, 0,
                 "PSD: layer %zu has no image channels", layer_idx);
    return TINYDNG_E_PARSE;
  }

  out_bps = (psd->depth <= 8u) ? 8u : psd->depth;
  out_bytes = (size_t)out_bps / 8u;
  pixel_stride = spp * out_bytes;
  if (!td_safe_mul_size((size_t)L->width, (size_t)L->height, &total) ||
      !td_safe_mul_size(total, pixel_stride, &total)) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_DECODE, 0, 0, 0,
                 "PSD: layer size overflow");
    return TINYDNG_E_BOUNDS;
  }

  if (opts && opts->dst) {
    if (opts->dst_capacity < total) {
      td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_DECODE, 0, 0, 0,
                   "PSD: dst capacity %zu < required %zu", opts->dst_capacity,
                   total);
      return TINYDNG_E_INVALID_ARG;
    }
    dst = (uint8_t *)opts->dst;
  } else {
    dst = (uint8_t *)td_ctx_calloc(ctx, total, err);
    if (!dst) {
      return TINYDNG_E_OOM;
    }
    owns = 1;
  }

  /* Build one task per decodable channel. Slot for id>=0 is its rank among
     the non-negative ids (ids are almost always 0..n-1). */
  for (i = 0; i < L->channel_count; i++) {
    const tinydng_psd_channel *ch = &L->channels[i];
    size_t slot, j;
    if (ch->id < -1) {
      continue; /* masks are not part of the interleaved output */
    }
    if (ch->id == -1) {
      slot = alpha_slot;
    } else {
      slot = 0;
      for (j = 0; j < L->channel_count; j++) {
        if (L->channels[j].id >= 0 && L->channels[j].id < ch->id) {
          slot++;
        }
      }
    }
    if (slot >= spp) {
      continue;
    }
    tasks[n_tasks].ctx = ctx;
    tasks[n_tasks].doc = doc;
    tasks[n_tasks].ch = ch;
    tasks[n_tasks].w = L->width;
    tasks[n_tasks].h = L->height;
    tasks[n_tasks].depth = psd->depth;
    tasks[n_tasks].is_psb = psd->is_psb;
    tasks[n_tasks].dst = dst + slot * out_bytes;
    tasks[n_tasks].pixel_stride = pixel_stride;
    tasks[n_tasks].out_bytes = out_bytes;
    tasks[n_tasks].invert1 = 1;
    tinydng_error_clear(&tasks[n_tasks].err);
    tasks[n_tasks].ok = 0;
    n_tasks++;
  }
  if (n_tasks == 0u) {
    if (owns) {
      td_ctx_free(ctx, dst);
    }
    td_set_error(err, TINYDNG_E_DECODE, TINYDNG_STAGE_DECODE, 0, 0, 0,
                 "PSD: layer %zu has no channel data", layer_idx);
    return TINYDNG_E_DECODE;
  }

  st = td_psd_run_tasks(ctx, tasks, n_tasks, opts, err);
  if (st != TINYDNG_OK) {
    if (owns) {
      td_ctx_free(ctx, dst);
    }
    return st;
  }

  out->data = dst;
  out->size = total;
  out->width = L->width;
  out->height = L->height;
  out->samples_per_pixel = (uint16_t)spp;
  out->bits_per_sample = out_bps;
  out->sample_format = (psd->depth == 32u) ? TINYDNG_SAMPLEFORMAT_IEEEFP
                                           : TINYDNG_SAMPLEFORMAT_UINT;
  out->owns_memory = owns;
  return TINYDNG_OK;
}

tinydng_status tinydng_psd_decode_layer_channel(
    tinydng_context *ctx, const tinydng_document *doc, size_t layer_idx,
    size_t channel_idx, const tinydng_decode_options *opts,
    tinydng_pixels *out, tinydng_error *err) {
  const tinydng_psd_info *psd;
  const tinydng_psd_layer *L;
  const tinydng_psd_channel *ch;
  uint32_t w, h;
  uint16_t out_bps;
  size_t out_bytes, total;
  uint8_t *dst;
  uint8_t owns = 0;
  td_psd_chan_task task;

  tinydng_error_clear(err);
  if (!ctx || !doc || !out) {
    return TINYDNG_E_INVALID_ARG;
  }
  memset(out, 0, sizeof(*out));
  psd = tinydng_document_psd(doc);
  if (!psd || layer_idx >= psd->layer_count) {
    return TINYDNG_E_INVALID_ARG;
  }
  L = &psd->layers[layer_idx];
  if (channel_idx >= L->channel_count) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_DECODE, 0, 0, 0,
                 "PSD: channel index out of range");
    return TINYDNG_E_INVALID_ARG;
  }
  ch = &L->channels[channel_idx];
  if (ch->id <= -2) {
    if (!L->mask.present ||
        !td_psd_rect_ok(ctx, L->mask.top, L->mask.left, L->mask.bottom,
                        L->mask.right, &w, &h)) {
      td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_DECODE, 0, 0, 0,
                   "PSD: mask channel without a valid mask rect");
      return TINYDNG_E_PARSE;
    }
  } else {
    w = L->width;
    h = L->height;
  }
  if (w == 0u || h == 0u) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_DECODE, 0, 0, 0,
                 "PSD: channel rect is empty");
    return TINYDNG_E_INVALID_ARG;
  }
  out_bps = (psd->depth <= 8u) ? 8u : psd->depth;
  out_bytes = (size_t)out_bps / 8u;
  if (!td_safe_mul_size((size_t)w, (size_t)h, &total) ||
      !td_safe_mul_size(total, out_bytes, &total)) {
    return TINYDNG_E_BOUNDS;
  }
  if (opts && opts->dst) {
    if (opts->dst_capacity < total) {
      return TINYDNG_E_INVALID_ARG;
    }
    dst = (uint8_t *)opts->dst;
  } else {
    dst = (uint8_t *)td_ctx_alloc(ctx, total, err);
    if (!dst) {
      return TINYDNG_E_OOM;
    }
    owns = 1;
  }
  task.ctx = ctx;
  task.doc = doc;
  task.ch = ch;
  task.w = w;
  task.h = h;
  task.depth = psd->depth;
  task.is_psb = psd->is_psb;
  task.dst = dst;
  task.pixel_stride = out_bytes;
  task.out_bytes = out_bytes;
  task.invert1 = 1;
  tinydng_error_clear(&task.err);
  task.ok = 0;
  td_psd_chan_worker(&task);
  if (!task.ok) {
    if (owns) {
      td_ctx_free(ctx, dst);
    }
    if (err) {
      *err = task.err;
    }
    return task.err.status != TINYDNG_OK ? task.err.status : TINYDNG_E_DECODE;
  }
  out->data = dst;
  out->size = total;
  out->width = w;
  out->height = h;
  out->samples_per_pixel = 1u;
  out->bits_per_sample = out_bps;
  out->sample_format = (psd->depth == 32u) ? TINYDNG_SAMPLEFORMAT_IEEEFP
                                           : TINYDNG_SAMPLEFORMAT_UINT;
  out->owns_memory = owns;
  return TINYDNG_OK;
}

/* ------------------------------------------------------------------ */
/* Thumbnail                                                          */
/* ------------------------------------------------------------------ */

tinydng_status tinydng_psd_decode_thumbnail(tinydng_context *ctx,
                                            const tinydng_document *doc,
                                            tinydng_pixels *out,
                                            tinydng_error *err) {
  const tinydng_psd_info *psd;
  const tinydng_psd_resource *res = NULL;
  int is_1033 = 0;
  uint32_t format, tw, th;
  size_t i;
  td_reader r;

  tinydng_error_clear(err);
  if (!ctx || !doc || !out) {
    return TINYDNG_E_INVALID_ARG;
  }
  memset(out, 0, sizeof(*out));
  psd = tinydng_document_psd(doc);
  if (!psd) {
    return TINYDNG_E_INVALID_ARG;
  }
  for (i = 0; i < psd->resource_count; i++) {
    if (psd->resources[i].id == 1036u) {
      res = &psd->resources[i];
      is_1033 = 0;
      break;
    }
    if (psd->resources[i].id == 1033u && !res) {
      res = &psd->resources[i];
      is_1033 = 1;
    }
  }
  if (!res || res->length < 28u) {
    td_set_error(err, TINYDNG_E_UNSUPPORTED, TINYDNG_STAGE_DECODE, 0, 0, 0,
                 "PSD: no thumbnail resource");
    return TINYDNG_E_UNSUPPORTED;
  }
  memset(&r, 0, sizeof(r));
  r.io = (tinydng_io *)&doc->io;
  r.size = doc->io_size;
  r.big_endian = 1;
  if (!td_r_u32(&r, res->offset, &format) ||
      !td_r_u32(&r, res->offset + 4u, &tw) ||
      !td_r_u32(&r, res->offset + 8u, &th)) {
    return TINYDNG_E_PARSE;
  }
  if (tw == 0u || th == 0u || tw > 10000u || th > 10000u) {
    td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_DECODE, 0, 0,
                 res->offset, "PSD: thumbnail dims invalid");
    return TINYDNG_E_PARSE;
  }
  (void)is_1033; /* only consulted on the baseline-JPEG path below */
  if (format == 1u) {
#ifndef TINYDNG_NO_BASELINE_JPEG
    size_t jlen = (size_t)(res->length - 28u);
    uint8_t *jbuf;
    int w = 0, h = 0, comp = 0;
    stbi_uc *pixels;
    if (jlen == 0u || jlen > (size_t)INT32_MAX) {
      return TINYDNG_E_PARSE;
    }
    jbuf = td_psd_read_copy(ctx, &r, res->offset + 28u, jlen, err);
    if (!jbuf) {
      return td_error_status_or(err, TINYDNG_E_OOM);
    }
    pixels = stbi_load_from_memory(jbuf, (int)jlen, &w, &h, &comp, 3);
    td_ctx_free(ctx, jbuf);
    if (!pixels) {
      td_set_error(err, TINYDNG_E_DECODE, TINYDNG_STAGE_DECODE, 0, 0,
                   res->offset, "PSD: thumbnail JPEG decode failed: %s",
                   stbi_failure_reason());
      return TINYDNG_E_DECODE;
    }
    {
      size_t n, pixel_count;
      if (!td_safe_mul_size((size_t)w, (size_t)h, &pixel_count) ||
          !td_safe_mul_size(pixel_count, 3u, &n)) {
        stbi_image_free(pixels);
        td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_DECODE, 0, 0,
                     res->offset, "PSD: thumbnail output size overflow");
        return TINYDNG_E_BOUNDS;
      }
      uint8_t *buf = (uint8_t *)td_ctx_alloc(ctx, n, err);
      if (!buf) {
        stbi_image_free(pixels);
        return TINYDNG_E_OOM;
      }
      memcpy(buf, pixels, n);
      stbi_image_free(pixels);
      if (is_1033) { /* legacy thumbnails are BGR */
        size_t k;
        for (k = 0; k + 2u < n; k += 3u) {
          uint8_t t = buf[k];
          buf[k] = buf[k + 2u];
          buf[k + 2u] = t;
        }
      }
      out->data = buf;
      out->size = n;
      out->width = (uint32_t)w;
      out->height = (uint32_t)h;
      out->samples_per_pixel = 3u;
      out->bits_per_sample = 8u;
      out->sample_format = TINYDNG_SAMPLEFORMAT_UINT;
      out->owns_memory = 1;
    }
    return TINYDNG_OK;
#else
    td_set_error(err, TINYDNG_E_UNSUPPORTED, TINYDNG_STAGE_DECODE, 0, 0, 0,
                 "PSD: JPEG thumbnail decode disabled");
    return TINYDNG_E_UNSUPPORTED;
#endif
  }
  td_set_error(err, TINYDNG_E_UNSUPPORTED, TINYDNG_STAGE_DECODE, 0, 0,
               res->offset, "PSD: thumbnail format %u unsupported", format);
  return TINYDNG_E_UNSUPPORTED;
}

/* ------------------------------------------------------------------ */
/* Smart objects                                                      */
/* ------------------------------------------------------------------ */

/* Sub-range io backend: a [base, base+size) window over the parent
   document's io, so embedded smart-object payloads are parsed and decoded
   without copying the bytes. The recursively opened document ALIASES the
   parent io: it must be destroyed before the parent document, and reads on
   the two documents must not interleave on a seek-based (stdio) parent
   backend. */
typedef struct td_psd_subio {
  tinydng_context *ctx;
  tinydng_io *parent; /* aliased, never closed */
  uint64_t base;
  uint64_t size;
} td_psd_subio;

static size_t td_psd_subio_read(tinydng_io *io, uint64_t off, void *dst,
                                size_t len) {
  td_psd_subio *s = (td_psd_subio *)io->backend;
  if (off > s->size || (uint64_t)len > (s->size - off)) {
    return 0;
  }
  return s->parent->read(s->parent, s->base + off, dst, len);
}

static uint64_t td_psd_subio_size(tinydng_io *io) {
  td_psd_subio *s = (td_psd_subio *)io->backend;
  return s->size;
}

static const uint8_t *td_psd_subio_map(tinydng_io *io, uint64_t off,
                                       size_t len) {
  td_psd_subio *s = (td_psd_subio *)io->backend;
  if (!s->parent->map) {
    return NULL;
  }
  if (off > s->size || (uint64_t)len > (s->size - off)) {
    return NULL;
  }
  return s->parent->map(s->parent, s->base + off, len);
}

static void td_psd_subio_close(tinydng_io *io) {
  td_psd_subio *s = (td_psd_subio *)io->backend;
  if (s) {
    td_ctx_free(s->ctx, s);
  }
  io->backend = NULL;
}

/* Resolve a sub-range chain down to the root io: nested smart objects view
   the root document's io with a cumulative absolute offset, so destroying
   intermediate documents (the walk-down pattern) never dangles. */
static void td_psd_resolve_subio(const tinydng_document *doc, uint64_t offset,
                                 tinydng_io **root_io, uint64_t *root_off) {
  tinydng_io *io = (tinydng_io *)&doc->io;
  uint64_t off = offset;
  while (io->map == td_psd_subio_map) {
    td_psd_subio *s = (td_psd_subio *)io->backend;
    off = s->base + off;
    io = s->parent;
  }
  *root_io = io;
  *root_off = off;
}

static const tinydng_psd_smart_object *td_psd_get_so(
    const tinydng_document *doc, size_t so_idx, tinydng_error *err) {
  const tinydng_psd_info *psd = tinydng_document_psd(doc);
  if (!psd || so_idx >= psd->smart_object_count) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_METADATA, 0, 0, 0,
                 "PSD: smart object index out of range");
    return NULL;
  }
  return &psd->smart_objects[so_idx];
}

tinydng_status tinydng_psd_smart_object_open(tinydng_context *ctx,
                                             const tinydng_document *doc,
                                             size_t so_idx,
                                             const tinydng_open_options *opts,
                                             tinydng_document **out,
                                             tinydng_error *err) {
  const tinydng_psd_smart_object *so;
  td_psd_subio *mem;
  tinydng_io io;
  tinydng_status st;

  tinydng_error_clear(err);
  if (!ctx || !doc || !out) {
    return TINYDNG_E_INVALID_ARG;
  }
  *out = NULL;
  so = td_psd_get_so(doc, so_idx, err);
  if (!so) {
    return TINYDNG_E_INVALID_ARG;
  }
  if (so->data_offset == 0u || so->data_length == 0u) {
    td_set_error(err, TINYDNG_E_UNSUPPORTED, TINYDNG_STAGE_METADATA, 0, 0, 0,
                 "PSD: smart object has no embedded payload");
    return TINYDNG_E_UNSUPPORTED;
  }
  if (so->data_offset > doc->io_size ||
      so->data_length > (doc->io_size - so->data_offset)) {
    td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_METADATA, 0, 0,
                 so->data_offset, "PSD: smart object payload out of range");
    return TINYDNG_E_BOUNDS;
  }
  if (doc->embed_depth + 1u > ctx->max_embed_depth) {
    td_set_error(err, TINYDNG_E_UNSUPPORTED, TINYDNG_STAGE_METADATA, 0, 0, 0,
                 "PSD: smart object nesting exceeds max_embed_depth (%u)",
                 ctx->max_embed_depth);
    return TINYDNG_E_UNSUPPORTED;
  }
  /* Window over the root io: no payload copy. The child document must be
     destroyed before the ROOT document (`doc` or its outermost ancestor),
     and must not be read concurrently with it on a seek-based parent
     backend (see td_psd_subio). */
  mem = (td_psd_subio *)td_ctx_calloc(ctx, sizeof(*mem), err);
  if (!mem) {
    return TINYDNG_E_OOM;
  }
  mem->ctx = ctx;
  td_psd_resolve_subio(doc, so->data_offset, &mem->parent, &mem->base);
  mem->size = so->data_length;
  memset(&io, 0, sizeof(io));
  io.read = td_psd_subio_read;
  io.size = td_psd_subio_size;
  io.map = td_psd_subio_map;
  io.close = td_psd_subio_close;
  io.backend = mem;

  st = tinydng_open_io(ctx, io, opts, out, err);
  if (st == TINYDNG_OK && *out) {
    (*out)->embed_depth = doc->embed_depth + 1u;
  }
  return st;
}

tinydng_status tinydng_psd_smart_object_decode(tinydng_context *ctx,
                                               const tinydng_document *doc,
                                               size_t so_idx,
                                               tinydng_pixels *out,
                                               tinydng_error *err) {
  const tinydng_psd_smart_object *so;
  uint8_t magic[4] = {0, 0, 0, 0};
  td_reader r;

  tinydng_error_clear(err);
  if (!ctx || !doc || !out) {
    return TINYDNG_E_INVALID_ARG;
  }
  memset(out, 0, sizeof(*out));
  so = td_psd_get_so(doc, so_idx, err);
  if (!so) {
    return TINYDNG_E_INVALID_ARG;
  }
  if (so->data_offset == 0u || so->data_length < 4u) {
    td_set_error(err, TINYDNG_E_UNSUPPORTED, TINYDNG_STAGE_DECODE, 0, 0, 0,
                 "PSD: smart object has no embedded payload");
    return TINYDNG_E_UNSUPPORTED;
  }
  memset(&r, 0, sizeof(r));
  r.io = (tinydng_io *)&doc->io;
  r.size = doc->io_size;
  r.big_endian = 1;
  {
    uint8_t scratch[4];
    const uint8_t *v =
        td_io_view(r.io, r.size, so->data_offset, 4u, scratch,
                   sizeof(scratch));
    if (!v) {
      return TINYDNG_E_BOUNDS;
    }
    memcpy(magic, v, 4u);
  }

  if ((magic[0] == 0xFFu && magic[1] == 0xD8u && magic[2] == 0xFFu) ||
      (magic[0] == 0x89u && magic[1] == 'P' && magic[2] == 'N' &&
       magic[3] == 'G')) {
#ifndef TINYDNG_NO_BASELINE_JPEG
    uint8_t *payload = NULL;
    size_t payload_size = 0;
    int w = 0, h = 0, comp = 0;
    stbi_uc *pixels;
    tinydng_status st;
    if (so->data_length > (uint64_t)INT32_MAX) {
      return TINYDNG_E_BOUNDS;
    }
    st = tinydng_psd_read_block(ctx, doc, so->data_offset, so->data_length,
                                &payload, &payload_size, err);
    if (st != TINYDNG_OK) {
      return st;
    }
    pixels = stbi_load_from_memory(payload, (int)payload_size, &w, &h, &comp,
                                   0);
    td_ctx_free(ctx, payload);
    if (!pixels || w <= 0 || h <= 0 || comp <= 0) {
      if (pixels) {
        stbi_image_free(pixels);
      }
      td_set_error(err, TINYDNG_E_DECODE, TINYDNG_STAGE_DECODE, 0, 0,
                   so->data_offset, "PSD: smart object decode failed: %s",
                   stbi_failure_reason());
      return TINYDNG_E_DECODE;
    }
    {
      size_t n, pixels_count;
      if (!td_safe_mul_size((size_t)w, (size_t)h, &pixels_count) ||
          !td_safe_mul_size(pixels_count, (size_t)comp, &n)) {
        stbi_image_free(pixels);
        td_set_error(err, TINYDNG_E_BOUNDS, TINYDNG_STAGE_DECODE, 0, 0,
                     so->data_offset, "PSD: smart object output size overflow");
        return TINYDNG_E_BOUNDS;
      }
      uint8_t *buf = (uint8_t *)td_ctx_alloc(ctx, n, err);
      if (!buf) {
        stbi_image_free(pixels);
        return TINYDNG_E_OOM;
      }
      memcpy(buf, pixels, n);
      stbi_image_free(pixels);
      out->data = buf;
      out->size = n;
      out->width = (uint32_t)w;
      out->height = (uint32_t)h;
      out->samples_per_pixel = (uint16_t)comp;
      out->bits_per_sample = 8u;
      out->sample_format = TINYDNG_SAMPLEFORMAT_UINT;
      out->owns_memory = 1;
    }
    return TINYDNG_OK;
#else
    td_set_error(err, TINYDNG_E_UNSUPPORTED, TINYDNG_STAGE_DECODE, 0, 0, 0,
                 "PSD: stb decode disabled (TINYDNG_NO_BASELINE_JPEG)");
    return TINYDNG_E_UNSUPPORTED;
#endif
  }

  /* PSD / TIFF / DNG payload: recursive open + decode image 0. */
  {
    tinydng_document *child = NULL;
    tinydng_status st = tinydng_psd_smart_object_open(ctx, doc, so_idx, NULL,
                                                      &child, err);
    if (st != TINYDNG_OK) {
      return st;
    }
    st = tinydng_decode_image(ctx, child, 0, NULL, out, err);
    tinydng_document_destroy(ctx, child);
    return st;
  }
}

#endif /* TINYDNG_NO_PSD */
