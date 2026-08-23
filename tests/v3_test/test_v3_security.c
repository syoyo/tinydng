/* Focused regression fixtures for the security-review fixes. Each builds a
 * crafted in-memory TIFF/DNG and asserts the hardened behavior. These are
 * designed to FAIL if the corresponding fix is reverted; run under ASan+UBSan.
 *
 * Covered:
 *   A  GainMap dimension-product overflow      (dng.c: td_safe_mul_u64)
 *   B  GainMap payload must fit opcode nbytes   (dng.c)
 *   C  Segment-array count vs file size         (tiff.c: td_read_u64_array)
 *   D  RATIONAL EXIF tag type gating            (dng.c)
 *   E  LinearizationTable element stride        (dng.c)
 *   F  Duplicate string tag frees the old value (dng.c: td_set_ascii)
 *   G  LJPEG huffman symbol ssss > 16 rejected  (lj92: build_huff_lut)
 *   H  JPEGInterchangeFormat byte-count clamp   (tiff.c td_build_segments)
 *   I  Truncated SOF/segment length             (lj92 parseImage)
 *   J  Streaming read_fn over-report clamped    (lj92 srefill)
 *   K  Linearization strict bound (raw < len)   (lj92 decode paths)
 *   L  Writer CFA counts clamp                  (writer td_add CFA tags)
 *   M  Writer NaN/huge rationals saturate       (writer td_add_*rationals)
 *   N  PSD writer NULL layers/channels          (psd_write validate)
 *   O  Zero-length opcode keeps list parsing    (dng.c opcode walk)
 *   P  LZW strip without EOI + trailing junk    (codec td_lzw_decode)
 *   Q  Allocation-failure injection storm       (all td_ctx_alloc sites)
 */
#include <math.h>

#include "td_test_util.h"
#include "tiny_dng_ljpeg92_v2.h"

typedef struct { uint16_t tag, type; uint32_t count, val; } ent;

/* Build a minimal valid little-endian 2x2 8-bit image TIFF, plus `nex` extra
 * IFD entries. An out-of-line `blob` (bloblen bytes) is placed at file offset
 * 12; extra entries that reference it use val = 12 + relative offset. */
static uint8_t *build_img(const ent *ex, int nex, const uint8_t *blob,
                          size_t bloblen, size_t *total_out) {
  const size_t blob_off = 12;
  size_t after = blob_off + bloblen;
  size_t ifd_off = after + (after & 1u);
  int nent = 9 + nex, i = 0, k;
  size_t total = ifd_off + 2u + (size_t)nent * 12u + 4u;
  uint8_t *buf = (uint8_t *)calloc(1, total);
  uint8_t *e;
  if (!buf) return NULL;
  buf[0] = 'I'; buf[1] = 'I';
  tdt_pu16(buf + 2, 42, 0);
  tdt_pu32(buf + 4, (uint32_t)ifd_off, 0);
  buf[8] = 10; buf[9] = 20; buf[10] = 30; buf[11] = 40; /* 2x2x8 pixels */
  if (blob && bloblen) memcpy(buf + blob_off, blob, bloblen);
  tdt_pu16(buf + ifd_off, (uint16_t)nent, 0);
  e = buf + ifd_off + 2u;
#define BE(t, ty, c, v)                            \
  do {                                             \
    tdt_pu16(e + i * 12, (uint16_t)(t), 0);        \
    tdt_pu16(e + i * 12 + 2, (uint16_t)(ty), 0);   \
    tdt_pu32(e + i * 12 + 4, (uint32_t)(c), 0);    \
    tdt_pu32(e + i * 12 + 8, (uint32_t)(v), 0);    \
    i++;                                           \
  } while (0)
  BE(256, 3, 1, 2); BE(257, 3, 1, 2); BE(258, 3, 1, 8); BE(259, 3, 1, 1);
  BE(262, 3, 1, 1); BE(273, 4, 1, 8); BE(277, 3, 1, 1); BE(278, 3, 1, 2);
  BE(279, 4, 1, 4);
  for (k = 0; k < nex; k++) BE(ex[k].tag, ex[k].type, ex[k].count, ex[k].val);
#undef BE
  tdt_pu32(e + (size_t)nent * 12u, 0, 0);
  *total_out = total;
  return buf;
}

/* Append a DNG opcode header (big-endian) to `b` at *p and return the byte
 * offset where the payload begins. */
static size_t op_hdr(uint8_t *b, size_t *p, uint32_t id, uint32_t nbytes) {
  tdt_pu32(b + *p, id, 1); *p += 4;
  tdt_pu32(b + *p, 0x01030000u, 1); *p += 4; /* version */
  tdt_pu32(b + *p, 0, 1); *p += 4;           /* flags */
  tdt_pu32(b + *p, nbytes, 1); *p += 4;
  return *p;
}

/* Open a crafted buffer and return the first image (or NULL). */
static const tinydng_image_info *open_img(tinydng_context *ctx, uint8_t *buf,
                                          size_t total, tinydng_document **doc) {
  tinydng_error err;
  *doc = NULL;
  if (tinydng_open_memory(ctx, buf, total, NULL, doc, &err) != TINYDNG_OK) {
    return NULL;
  }
  return tinydng_image_get(*doc, 0);
}

/* A: GainMap map_points_v * map_points_h * map_planes overflows uint64 and wraps
 * to a SMALL value (4) that slips past the TD_MAX_GAINMAP_ITEMS cap. The factors
 * are chosen so (2^31+2^16+1)*(2^31-2^16+1) = 2^62+1 fits, then *4 = 2^64+4 wraps
 * to 4. WITHOUT the overflow-safe multiply the gainmap is accepted (4 pixels, but
 * huge dimension fields inconsistent with pixel_count); the matching 4-float
 * payload lets the pre-fix path complete. With the fix the second multiply
 * overflows and the opcode is rejected (gainmap_count == 0). */
static void case_gainmap_overflow(void) {
  uint8_t b[256]; size_t p = 0;
  tinydng_context *ctx = tinydng_context_create(NULL, NULL);
  tinydng_document *doc; const tinydng_image_info *im; size_t total; uint8_t *buf;
  ent ex;
  tdt_pu32(b + p, 1, 1); p += 4;            /* num opcodes */
  op_hdr(b, &p, 9u, 92u);                    /* GainMap, payload 76 hdr + 16 */
  /* 10 u32 header: top,left,bottom,right, plane,planes,row,col, mpv, mph */
  tdt_pu32(b+p,0,1);p+=4; tdt_pu32(b+p,0,1);p+=4; tdt_pu32(b+p,0,1);p+=4; tdt_pu32(b+p,0,1);p+=4;
  tdt_pu32(b+p,0,1);p+=4; tdt_pu32(b+p,1,1);p+=4; tdt_pu32(b+p,1,1);p+=4; tdt_pu32(b+p,1,1);p+=4;
  tdt_pu32(b+p,2147549185u,1);p+=4;         /* mpv = 2^31 + 2^16 + 1 */
  tdt_pu32(b+p,2147418113u,1);p+=4;         /* mph = 2^31 - 2^16 + 1 (mpv*mph = 2^62+1) */
  tdt_pf64be(b+p,0.1);p+=8; tdt_pf64be(b+p,0.2);p+=8; tdt_pf64be(b+p,0.3);p+=8; tdt_pf64be(b+p,0.4);p+=8;
  tdt_pu32(b+p,4u,1);p+=4;                   /* map_planes=4 ; (2^62+1)*4 = 2^64+4 -> wraps to 4 */
  tdt_pf32be(b+p,1.f);p+=4; tdt_pf32be(b+p,1.f);p+=4; tdt_pf32be(b+p,1.f);p+=4; tdt_pf32be(b+p,1.f);p+=4;
  ex.tag = 51009; ex.type = 7; ex.count = (uint32_t)p; ex.val = 12;
  buf = build_img(&ex, 1, b, p, &total);
  im = open_img(ctx, buf, total, &doc);
  CHECK(im != NULL, "A: gainmap-overflow file should still open");
  CHECK(im && im->raw.gainmap_count == 0,
        "A: overflowing GainMap rejected (count=%zu)",
        im ? im->raw.gainmap_count : (size_t)999);
  if (doc) tinydng_document_destroy(ctx, doc);
  CHECK(tinydng_context_memory_used(ctx) == 0u, "A: no leak");
  tinydng_context_destroy(ctx);
  free(buf);
}

/* B: valid small GainMap dims (2*2*1=4 items, no overflow) but the opcode
 * declares only the 76-byte header (nbytes=76, room for zero pixel floats). The
 * payload-fits-in-nbytes check must reject it (gainmap_count == 0). */
static void case_gainmap_short_nbytes(void) {
  uint8_t b[160]; size_t p = 0;
  tinydng_context *ctx = tinydng_context_create(NULL, NULL);
  tinydng_document *doc; const tinydng_image_info *im; size_t total; uint8_t *buf;
  ent ex;
  tdt_pu32(b + p, 1, 1); p += 4;
  op_hdr(b, &p, 9u, 76u);                    /* nbytes = header only */
  tdt_pu32(b+p,0,1);p+=4; tdt_pu32(b+p,0,1);p+=4; tdt_pu32(b+p,0,1);p+=4; tdt_pu32(b+p,0,1);p+=4;
  tdt_pu32(b+p,0,1);p+=4; tdt_pu32(b+p,1,1);p+=4; tdt_pu32(b+p,1,1);p+=4; tdt_pu32(b+p,1,1);p+=4;
  tdt_pu32(b+p,2,1);p+=4; tdt_pu32(b+p,2,1);p+=4;            /* mpv=mph=2 */
  tdt_pf64be(b+p,0.1);p+=8; tdt_pf64be(b+p,0.2);p+=8; tdt_pf64be(b+p,0.3);p+=8; tdt_pf64be(b+p,0.4);p+=8;
  tdt_pu32(b+p,1,1);p+=4;                                    /* map_planes=1 -> 4 items */
  ex.tag = 51009; ex.type = 7; ex.count = (uint32_t)p; ex.val = 12;
  buf = build_img(&ex, 1, b, p, &total);
  im = open_img(ctx, buf, total, &doc);
  CHECK(im != NULL, "B: short-nbytes gainmap file should still open");
  CHECK(im && im->raw.gainmap_count == 0,
        "B: GainMap with payload exceeding nbytes rejected (count=%zu)",
        im ? im->raw.gainmap_count : (size_t)999);
  if (doc) tinydng_document_destroy(ctx, doc);
  tinydng_context_destroy(ctx);
  free(buf);
}

/* C: StripOffsets declares 1,000,000 LONG entries in a ~130-byte file. The
 * range check must reject before allocating the (8 MB) u64[] array, so the open
 * fails and peak memory stays small. */
static void case_segment_array_cap(void) {
  int nent = 9, i = 0;
  size_t ifd_off = 12;
  size_t total = ifd_off + 2u + (size_t)nent * 12u + 4u;
  uint8_t *buf = (uint8_t *)calloc(1, total), *e;
  tinydng_context *ctx = tinydng_context_create(NULL, NULL);
  tinydng_document *doc = NULL; tinydng_error err; tinydng_status st;
  buf[0] = 'I'; buf[1] = 'I'; tdt_pu16(buf + 2, 42, 0);
  tdt_pu32(buf + 4, (uint32_t)ifd_off, 0);
  buf[8] = 10; buf[9] = 20; buf[10] = 30; buf[11] = 40;
  tdt_pu16(buf + ifd_off, (uint16_t)nent, 0);
  e = buf + ifd_off + 2u;
#define BE(t, ty, c, v)                            \
  do {                                             \
    tdt_pu16(e + i * 12, (uint16_t)(t), 0);        \
    tdt_pu16(e + i * 12 + 2, (uint16_t)(ty), 0);   \
    tdt_pu32(e + i * 12 + 4, (uint32_t)(c), 0);    \
    tdt_pu32(e + i * 12 + 8, (uint32_t)(v), 0);    \
    i++;                                           \
  } while (0)
  BE(256, 3, 1, 2); BE(257, 3, 1, 2); BE(258, 3, 1, 8); BE(259, 3, 1, 1);
  BE(262, 3, 1, 1);
  BE(273, 4, 1000000u, 8);  /* StripOffsets: absurd count for a tiny file */
  BE(277, 3, 1, 1); BE(278, 3, 1, 2); BE(279, 4, 1, 4);
#undef BE
  tdt_pu32(e + (size_t)nent * 12u, 0, 0);
  st = tinydng_open_memory(ctx, buf, total, NULL, &doc, &err);
  CHECK(st != TINYDNG_OK, "C: oversized strip array should be rejected");
  CHECK(tinydng_context_memory_peak(ctx) < (1u << 20),
        "C: no large up-front allocation (peak=%zu)",
        tinydng_context_memory_peak(ctx));
  if (doc) tinydng_document_destroy(ctx, doc);
  tinydng_context_destroy(ctx);
  free(buf);
}

/* D: ExposureTime carried as DOUBLE (8-byte, but not RATIONAL) must be ignored;
 * carried as RATIONAL must still parse. */
static void case_rational_type(void) {
  tinydng_context *ctx = tinydng_context_create(NULL, NULL);
  tinydng_document *doc; const tinydng_image_info *im; size_t total; uint8_t *buf;
  uint8_t blob[8]; ent ex;
  /* D1: DOUBLE */
  tdt_pf64be(blob, 0.5); /* value bytes (any 8-byte content) */
  ex.tag = 33434; ex.type = 12 /*DOUBLE*/; ex.count = 1; ex.val = 12;
  buf = build_img(&ex, 1, blob, 8, &total);
  im = open_img(ctx, buf, total, &doc);
  CHECK(im && im->exif.has_exposure_time == 0,
        "D1: DOUBLE ExposureTime not misread as rational");
  if (doc) tinydng_document_destroy(ctx, doc);
  free(buf);
  /* D2: RATIONAL num=1 den=100 (little-endian, matching the TIFF) */
  tdt_pu32(blob, 1, 0); tdt_pu32(blob + 4, 100, 0);
  ex.tag = 33434; ex.type = 5 /*RATIONAL*/; ex.count = 1; ex.val = 12;
  buf = build_img(&ex, 1, blob, 8, &total);
  im = open_img(ctx, buf, total, &doc);
  CHECK(im && im->exif.has_exposure_time == 1 && im->exif.exposure_time[0] == 1 &&
            im->exif.exposure_time[1] == 100,
        "D2: RATIONAL ExposureTime parsed (has=%d %d/%d)",
        im ? im->exif.has_exposure_time : -1,
        im ? im->exif.exposure_time[0] : -1, im ? im->exif.exposure_time[1] : -1);
  if (doc) tinydng_document_destroy(ctx, doc);
  free(buf);
  tinydng_context_destroy(ctx);
}

/* E: LinearizationTable as LONG must be read with a 4-byte element stride. With
 * a hard-coded 2-byte stride the values come out wrong. */
static void case_linearization_stride(void) {
  tinydng_context *ctx = tinydng_context_create(NULL, NULL);
  tinydng_document *doc; const tinydng_image_info *im; size_t total; uint8_t *buf;
  uint8_t blob[16]; ent ex;
  tdt_pu32(blob + 0, 1, 0); tdt_pu32(blob + 4, 2, 0);
  tdt_pu32(blob + 8, 3, 0); tdt_pu32(blob + 12, 4, 0); /* 4 LONGs: 1,2,3,4 */
  ex.tag = 50712; ex.type = 4 /*LONG*/; ex.count = 4; ex.val = 12;
  buf = build_img(&ex, 1, blob, 16, &total);
  im = open_img(ctx, buf, total, &doc);
  CHECK(im && im->raw.linearization_table_count == 4, "E: lin count=4");
  if (im && im->raw.linearization_table_count == 4) {
    const uint16_t *t = im->raw.linearization_table;
    CHECK(t[0] == 1 && t[1] == 2 && t[2] == 3 && t[3] == 4,
          "E: LONG stride correct ([%u,%u,%u,%u])", t[0], t[1], t[2], t[3]);
  }
  if (doc) tinydng_document_destroy(ctx, doc);
  tinydng_context_destroy(ctx);
  free(buf);
}

/* F: a duplicate Make tag must free the first (large) value, so the live memory
 * after open is close to a control with a single small Make. */
static size_t open_used(uint8_t *buf, size_t total, char *make_out) {
  tinydng_context *ctx = tinydng_context_create(NULL, NULL);
  tinydng_document *doc; const tinydng_image_info *im; size_t used;
  im = open_img(ctx, buf, total, &doc);
  if (im && im->exif.make) {
    make_out[0] = im->exif.make[0];
    make_out[1] = '\0';
  } else {
    make_out[0] = '\0';
  }
  used = tinydng_context_memory_used(ctx);
  if (doc) tinydng_document_destroy(ctx, doc);
  tinydng_context_destroy(ctx);
  return used;
}

static void case_duplicate_tag(void) {
  uint8_t blob[16384]; ent ctl, dup[2]; size_t total; uint8_t *buf;
  size_t used_ctrl, used_dup; char mk[2];
  /* control: a single small Make = "B" inline */
  ctl.tag = 271; ctl.type = 2; ctl.count = 2; ctl.val = (uint32_t)'B';
  buf = build_img(&ctl, 1, NULL, 0, &total);
  used_ctrl = open_used(buf, total, mk);
  free(buf);
  /* duplicate: first Make = 16000 'A' (out-of-line), then Make = "B" */
  memset(blob, 'A', 16000);
  dup[0].tag = 271; dup[0].type = 2; dup[0].count = 16000; dup[0].val = 12;
  dup[1].tag = 271; dup[1].type = 2; dup[1].count = 2; dup[1].val = (uint32_t)'B';
  buf = build_img(dup, 2, blob, 16000, &total);
  used_dup = open_used(buf, total, mk);
  free(buf);
  CHECK(mk[0] == 'B', "F: duplicate Make resolves to the last value");
  CHECK(used_dup < used_ctrl + 8000u,
        "F: first (16000B) Make freed on duplicate (ctrl=%zu dup=%zu)",
        used_ctrl, used_dup);
}

/* G: a lossless-JPEG stream whose DHT maps a code to SSSS category 31. The v2
 * decoder's residual sign-extend does `1 << ssss` (and `64-ssss`/`bb<<=ssss`
 * shifts), which is undefined for ssss>16/30. The fix rejects huffval>16 at
 * table build, so decode returns an error instead of executing the UB. Built as
 * a 1x1 16-bit compression-7 TIFF strip. (Meaningful under TINYDNG_TEST_SANITIZE
 * / a -fno-sanitize-recover UBSan build, where the pre-fix code aborts here.) */
static void case_ljpeg_ssss(void) {
  static const uint8_t lj[] = {
      0xFF, 0xD8,                                            /* SOI */
      0xFF, 0xC3, 0x00, 0x0B, 0x10, 0x00, 0x01, 0x00, 0x01, /* SOF3 P16 1x1 */
      0x01, 0x00, 0x11, 0x00,                               /* 1 component */
      0xFF, 0xC4, 0x00, 0x14, 0x00,                         /* DHT, Lh=20 */
      0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,    /* one len-1 code */
      0x1F,                                                 /* symbol = 31 */
      0xFF, 0xDA, 0x00, 0x08, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, /* SOS pred1 */
      0, 0, 0, 0, 0, 0, 0, 0,                               /* entropy (bit0=0) */
      0xFF, 0xD9};                                          /* EOI */
  size_t ljlen = sizeof(lj);
  size_t strip_off = 8, after = strip_off + ljlen;
  size_t ifd_off = after + (after & 1u);
  int nent = 9, i = 0;
  size_t total = ifd_off + 2u + (size_t)nent * 12u + 4u;
  uint8_t *buf = (uint8_t *)calloc(1, total), *e;
  tinydng_context *ctx = tinydng_context_create(NULL, NULL);
  tinydng_document *doc = NULL;
  tinydng_error err;
  buf[0] = 'I'; buf[1] = 'I'; tdt_pu16(buf + 2, 42, 0);
  tdt_pu32(buf + 4, (uint32_t)ifd_off, 0);
  memcpy(buf + strip_off, lj, ljlen);
  tdt_pu16(buf + ifd_off, (uint16_t)nent, 0);
  e = buf + ifd_off + 2u;
#define E(t, ty, c, v)                            \
  do {                                            \
    tdt_pu16(e + i * 12, (uint16_t)(t), 0);       \
    tdt_pu16(e + i * 12 + 2, (uint16_t)(ty), 0);  \
    tdt_pu32(e + i * 12 + 4, (uint32_t)(c), 0);   \
    tdt_pu32(e + i * 12 + 8, (uint32_t)(v), 0);   \
    i++;                                          \
  } while (0)
  E(256, 3, 1, 1); E(257, 3, 1, 1); E(258, 3, 1, 16); E(259, 3, 1, 7);
  E(262, 3, 1, 1); E(273, 4, 1, (uint32_t)strip_off); E(277, 3, 1, 1);
  E(278, 3, 1, 1); E(279, 4, 1, (uint32_t)ljlen);
#undef E
  tdt_pu32(e + (size_t)nent * 12u, 0, 0);
  /* Must not crash. With the fix, decode returns an error (or open does); the
   * point is that no UB shift executes. */
  if (tinydng_open_memory(ctx, buf, total, NULL, &doc, &err) == TINYDNG_OK) {
    tinydng_pixels px;
    tinydng_error derr;
    int rc = tinydng_decode_image(ctx, doc, 0, NULL, &px, &derr);
    CHECK(rc != TINYDNG_OK, "G: malicious LJPEG (ssss>16) rejected, no UB shift");
    if (rc == TINYDNG_OK) tinydng_pixels_free(ctx, &px);
    tinydng_document_destroy(ctx, doc);
  }
  tinydng_context_destroy(ctx);
  free(buf);
}

/* I: truncated SOF/segment lengths must fail before any signed offset
 * arithmetic can advance past the supplied memory. */
static void case_ljpeg_truncated_segment(void) {
  static const uint8_t bad[] = {
      0xFF, 0xD8,             /* SOI */
      0xFF, 0xC3, 0xFF, 0xFF  /* SOF3 with an unavailable length field */
  };
  tdng_lj92 lj = NULL;
  int w = 0, h = 0, bits = 0, comps = 0;
  int ret = tdng_lj92_open(&lj, bad, (int)sizeof(bad), &w, &h, &bits,
                           &comps);
  CHECK(ret != TDNG_LJ92_ERROR_NONE && lj == NULL,
        "I: truncated LJPEG segment rejected cleanly (ret=%d)", ret);
  if (lj) {
    tdng_lj92_close(lj);
  }
}

/* J: a streaming read_fn that over-reports (returns more than requested).
 * srefill must clamp it so buf_len never exceeds buf_cap; the decode then
 * behaves exactly like a well-behaved callback. */
typedef struct {
  const uint8_t *data;
  size_t size;
} tdj_src;

static size_t evil_read(void *user, uint64_t off, void *dst, size_t len) {
  tdj_src *s = (tdj_src *)user;
  if (off >= s->size) {
    return 0;
  }
  if (len > s->size - (size_t)off) {
    len = s->size - (size_t)off;
  }
  memcpy(dst, s->data + off, len);
  return len + 16u; /* LIE: more than delivered (clamped by srefill) */
}

static uint64_t plain_size(void *user) {
  return ((tdj_src *)user)->size;
}

static void case_ljpeg_stream_overreporting(void) {
  /* Noise image large enough that the encoded stream exceeds the 64KB
   * stream-chunk capacity: the first refill requests exactly CHUNK bytes,
   * and the +16 lie then exceeds buf_cap (the case the clamp guards). */
#define JW 320
#define JH 320
  static uint16_t pix[JW * JH];
  uint32_t i;
  uint32_t rng = 0x12345678u;
  uint8_t *enc = NULL;
  int enclen = 0;
  int rc;
  for (i = 0; i < (uint32_t)(JW * JH); i++) {
    rng = rng * 1664525u + 1013904223u;
    pix[i] = (uint16_t)((rng >> 11) & 0xFFFFu);
  }
  rc = tdng_lj92_encode_ex(pix, JW, JH, 16, 1, 1, JW, 0, NULL, 0, &enc,
                           &enclen);
  CHECK(rc == TDNG_LJ92_ERROR_NONE && enc != NULL && enclen > 65536 + 16,
        "J: encode fixture (%d bytes)", enclen);
  if (rc == TDNG_LJ92_ERROR_NONE && enc) {
    tdj_src src;
    src.data = enc;
    src.size = (size_t)enclen;
    tdng_lj92 lj = NULL;
    int w = 0, h = 0, bits = 0, comps = 0;
    int ret = tdng_lj92_open_streaming(&lj, &src, evil_read, plain_size, &w, &h,
                                       &bits, &comps);
    CHECK(ret == TDNG_LJ92_ERROR_NONE && w == JW && h == JH && bits == 16 &&
              comps == 1,
          "J: streaming open ok with lying read_fn (ret=%d %dx%d)", ret, w, h);
    if (ret == TDNG_LJ92_ERROR_NONE) {
      static uint16_t out[JW * JH];
      uint32_t k;
      int dret;
      memset(out, 0xAA, sizeof(out));
      dret = tdng_lj92_decode(lj, out, JW, 0, NULL, 0);
      CHECK(dret == TDNG_LJ92_ERROR_NONE, "J: streaming decode ok (%d)", dret);
      for (k = 0; k < (uint32_t)(JW * JH); k++) {
        if (out[k] != pix[k]) break;
      }
      CHECK(k == (uint32_t)(JW * JH), "J: decoded pixels match");
      tdng_lj92_close(lj);
    }
  }
  free(enc);
#undef JW
#undef JH
}

/* K: linearization lookups use strict '< table length' bounds. A decoded
 * sample equal to the table length must fail the decode instead of reading
 * lin[len] (the old '> len' check allowed it). */
static void case_ljpeg_linearize_bound(void) {
  uint16_t pix[4] = {100, 100, 100, 100};
  uint8_t *enc = NULL;
  int enclen = 0;
  int rc = tdng_lj92_encode_ex(pix, 2, 2, 16, 1, 1, 2, 0, NULL, 0, &enc,
                               &enclen);
  CHECK(rc == TDNG_LJ92_ERROR_NONE && enc != NULL, "K: encode fixture");
  if (rc == TDNG_LJ92_ERROR_NONE && enc) {
    tdng_lj92 lj = NULL;
    int w = 0, h = 0, bits = 0, comps = 0;
    CHECK(tdng_lj92_open(&lj, enc, enclen, &w, &h, &bits, &comps) ==
              TDNG_LJ92_ERROR_NONE,
          "K: open");
    if (lj) {
      uint16_t out[4];
      /* Exactly 100 entries: sample 100 == len. Pre-fix ('>' bound) this
       * indexed lin[100] -- one entry past the table; post-fix ('>=') it
       * fails the decode. */
      uint16_t short_tab[100];
      uint16_t wide_tab[200];
      uint32_t i;
      memset(short_tab, 0x11, sizeof(short_tab));
      for (i = 0; i < 200; i++) {
        wide_tab[i] = (uint16_t)(i + 1); /* v -> v+1 so identity is visible */
      }
      memset(out, 0, sizeof(out));
      CHECK(tdng_lj92_decode(lj, out, 2, 0, short_tab, 100) ==
                TDNG_LJ92_ERROR_CORRUPT,
            "K: sample == table length rejected (strict bound)");
      memset(out, 0, sizeof(out));
      CHECK(tdng_lj92_decode(lj, out, 2, 0, wide_tab, 200) ==
                TDNG_LJ92_ERROR_NONE,
            "K: sample 100 accepted with 200-entry table");
      CHECK(out[0] == 101 && out[3] == 101,
            "K: mapped through table ([%u %u])", out[0], out[3]);
      tdng_lj92_close(lj);
    }
  }
  free(enc);
}

/* H: JPEGInterchangeFormat byte-count clamp (tiff.c td_build_segments).
 * A 0 byte count (or one extending past EOF) used to reach the baseline
 * JPEG decoder with an empty input (fuzzer crash); it must now be clamped
 * to the end of the file, and an out-of-range offset must fail cleanly. */
static uint8_t *build_jpeg_if_tiff(uint32_t jpeg_off, uint32_t jpeg_count,
                                   size_t *total_out) {
  /* minimal classic TIFF: no strip tags, Compression=7, JPEG IF tags */
  const int NENT = 8;
  size_t ifd_off = 16;
  size_t total = ifd_off + 2u + (size_t)NENT * 12u + 4u;
  uint8_t *buf = (uint8_t *)calloc(1, total);
  uint8_t *e;
  int i = 0;
  if (!buf) return NULL;
  buf[0] = 'I'; buf[1] = 'I';
  tdt_pu16(buf + 2, 42, 0);
  tdt_pu32(buf + 4, (uint32_t)ifd_off, 0);
  buf[8] = 0x47; buf[9] = 0x50; /* garbage "JPEG" bytes at offset 8 */
  tdt_pu16(buf + ifd_off, (uint16_t)NENT, 0);
  e = buf + ifd_off + 2u;
#define J(t, ty, c, v)                             \
  do {                                             \
    tdt_pu16(e + i * 12, (uint16_t)(t), 0);        \
    tdt_pu16(e + i * 12 + 2, (uint16_t)(ty), 0);   \
    tdt_pu32(e + i * 12 + 4, (uint32_t)(c), 0);    \
    tdt_pu32(e + i * 12 + 8, (uint32_t)(v), 0);    \
    i++;                                           \
  } while (0)
  J(256, 4, 1, 2); J(257, 4, 1, 2); J(258, 3, 1, 8); J(259, 3, 1, 7);
  J(262, 3, 1, 1); J(277, 3, 1, 1); J(513, 4, 1, jpeg_off);
  J(514, 4, 1, jpeg_count);
#undef J
  tdt_pu32(e + (size_t)NENT * 12u, 0, 0);
  *total_out = total;
  return buf;
}

static void case_jpeg_if_clamp(void) {
  tinydng_error err;
  uint8_t *buf;
  size_t total;

  /* byte count 0 at a valid offset: clamped to EOF, decode fails cleanly
     (no empty-input crash in the baseline JPEG path). */
  buf = build_jpeg_if_tiff(8, 0, &total);
  if (buf) {
    tinydng_context *ctx = tinydng_context_create(NULL, NULL);
    tinydng_document *doc = NULL;
    tinydng_status st = tinydng_open_memory(ctx, buf, total, NULL, &doc, &err);
    CHECK(st == TINYDNG_OK, "H: jpeg-if bc=0 opens (clamped)");
    if (st == TINYDNG_OK) {
      tinydng_pixels px;
      tinydng_error derr;
      int rc = tinydng_decode_image(ctx, doc, 0, NULL, &px, &derr);
      CHECK(rc != TINYDNG_OK, "H: jpeg-if bc=0 decode fails cleanly, no crash");
      if (rc == TINYDNG_OK) tinydng_pixels_free(ctx, &px);
      tinydng_document_destroy(ctx, doc);
    }
    tinydng_context_destroy(ctx);
    free(buf);
  }

  /* byte count extending past EOF: clamped to EOF (same clean failure). */
  buf = build_jpeg_if_tiff(8, 0xFFFFFFF0u, &total);
  if (buf) {
    tinydng_context *ctx = tinydng_context_create(NULL, NULL);
    tinydng_document *doc = NULL;
    tinydng_status st = tinydng_open_memory(ctx, buf, total, NULL, &doc, &err);
    CHECK(st == TINYDNG_OK, "H: jpeg-if bc past EOF opens (clamped)");
    if (st == TINYDNG_OK) {
      tinydng_pixels px;
      tinydng_error derr;
      int rc = tinydng_decode_image(ctx, doc, 0, NULL, &px, &derr);
      CHECK(rc != TINYDNG_OK, "H: jpeg-if bc past EOF decode fails cleanly");
      if (rc == TINYDNG_OK) tinydng_pixels_free(ctx, &px);
      tinydng_document_destroy(ctx, doc);
    }
    tinydng_context_destroy(ctx);
    free(buf);
  }

  /* offset past EOF: open must reject with BOUNDS. */
  buf = build_jpeg_if_tiff(0xFFFFFFF0u, 4, &total);
  if (buf) {
    tinydng_context *ctx = tinydng_context_create(NULL, NULL);
    tinydng_document *doc = NULL;
    tinydng_status st = tinydng_open_memory(ctx, buf, total, NULL, &doc, &err);
    CHECK(st == TINYDNG_E_BOUNDS, "H: jpeg-if offset past EOF -> BOUNDS");
    if (st == TINYDNG_OK) tinydng_document_destroy(ctx, doc);
    tinydng_context_destroy(ctx);
    free(buf);
  }
}

/* L: a caller struct with CFA counts larger than the fixed-size arrays must
 * not make the writer read past pattern[16]/plane_color[4] (stack overread
 * under ASan pre-fix); emitted tag lengths are clamped to the arrays. */
static void case_writer_cfa_clamp(void) {
  tinydng_context *ctx = tinydng_context_create(NULL, NULL);
  tinydng_write_image img;
  tinydng_write_options opts;
  tinydng_cfa cfa;
  uint8_t pix[4 * 4 * 1];
  uint8_t *out = NULL;
  size_t out_size = 0;
  tinydng_error err;
  tinydng_status st;
  memset(pix, 0x40, sizeof(pix));
  memset(&img, 0, sizeof(img));
  img.width = 4;
  img.height = 4;
  img.samples_per_pixel = 1;
  img.bits_per_sample = 8;
  img.data = pix;
  img.data_size = sizeof(pix);
  memset(&cfa, 0, sizeof(cfa));
  cfa.present = 1;
  cfa.pattern_dim[0] = 2;
  cfa.pattern_dim[1] = 2;
  cfa.pattern_size = 255;     /* poisoned: > sizeof(pattern)==16 */
  cfa.plane_color_count = 200; /* poisoned: > sizeof(plane_color)==4 */
  img.cfa = &cfa;
  memset(&opts, 0, sizeof(opts));
  opts.as_dng = 1;
  st = tinydng_write_memory(ctx, &img, &opts, &out, &out_size, &err);
  CHECK(st == TINYDNG_OK, "L: write with oversized CFA counts ok (%s)",
        err.message);
  if (st == TINYDNG_OK && out) {
    tinydng_document *doc = NULL;
    const tinydng_image_info *im;
    CHECK(tinydng_open_memory(ctx, out, out_size, NULL, &doc, &err) ==
              TINYDNG_OK,
          "L: output re-opens");
    im = doc ? tinydng_image_get(doc, 0) : NULL;
    CHECK(im && im->cfa.pattern_size <= 16 && im->cfa.plane_color_count <= 4,
          "L: parsed counts clamped (%u/%u)",
          im ? (unsigned)im->cfa.pattern_size : 99u,
          im ? (unsigned)im->cfa.plane_color_count : 99u);
    if (doc) tinydng_document_destroy(ctx, doc);
    tinydng_buffer_free(ctx, out);
  }
  tinydng_context_destroy(ctx);
}

/* M: NaN / +-inf / huge doubles in calibration matrices must saturate
 * instead of executing undefined float->int conversions (UBSan traps on the
 * pre-fix plain casts). */
static void case_writer_nan_rational(void) {
  tinydng_context *ctx = tinydng_context_create(NULL, NULL);
  tinydng_write_image img;
  tinydng_write_options opts;
  tinydng_raw_info raw;
  uint8_t pix[2 * 2 * 1];
  uint8_t *out = NULL;
  size_t out_size = 0;
  tinydng_error err;
  int i;
  memset(pix, 0x20, sizeof(pix));
  memset(&img, 0, sizeof(img));
  img.width = 2;
  img.height = 2;
  img.samples_per_pixel = 1;
  img.bits_per_sample = 8;
  img.data = pix;
  img.data_size = sizeof(pix);
  memset(&raw, 0, sizeof(raw));
  raw.color_matrix_present = 1;
  for (i = 0; i < 9; i++) {
    raw.color_matrix1[i] = (i == 0) ? NAN : ((i == 1) ? 1e300 : -INFINITY);
  }
  img.raw = &raw;
  memset(&opts, 0, sizeof(opts));
  opts.as_dng = 1;
  {
    uint8_t *out2 = NULL;
    size_t sz2 = 0;
    CHECK(tinydng_write_memory(ctx, &img, &opts, &out2, &sz2, &err) ==
              TINYDNG_OK,
          "M: write with NaN/Inf matrix ok (%s)", err.message);
    if (out2) {
      tinydng_document *doc = NULL;
      const tinydng_image_info *im;
      CHECK(tinydng_open_memory(ctx, out2, sz2, NULL, &doc, &err) ==
                TINYDNG_OK,
            "M: output re-opens");
      im = doc ? tinydng_image_get(doc, 0) : NULL;
      if (im) {
        int finite_all = 1;
        for (i = 0; i < 9; i++) {
          if (!(im->raw.color_matrix1[i] == im->raw.color_matrix1[i]) ||
              im->raw.color_matrix1[i] > 2147.483647e3 ||
              im->raw.color_matrix1[i] < -2147.483648e3) {
            finite_all = 0;
          }
        }
        CHECK(finite_all, "M: round-tripped matrix saturated/in range");
      }
      if (doc) tinydng_document_destroy(ctx, doc);
      tinydng_buffer_free(ctx, out2);
    }
  }
  tinydng_context_destroy(ctx);
}

/* N: PSD writer rejects layer_count>0 with layers==NULL (and channel_count>0
 * with channels==NULL) at validate time instead of dereferencing NULL. */
static void case_psdw_null_layers(void) {
  tinydng_context *ctx = tinydng_context_create(NULL, NULL);
  tinydng_psd_write_doc doc;
  tinydng_psd_write_layer layer;
  tinydng_psd_write_channel ch;
  uint8_t comp[2 * 2 * 1];
  uint8_t chan[2 * 2 * 1];
  tinydng_error err;
  uint8_t *out = NULL;
  size_t sz = 0;
  tinydng_status st;
  memset(comp, 0x30, sizeof(comp));
  memset(chan, 0x50, sizeof(chan));
  memset(&doc, 0, sizeof(doc));
  doc.width = 2;
  doc.height = 2;
  doc.depth = 8;
  doc.channel_count = 1;
  doc.composite = comp;
  doc.composite_size = sizeof(comp);
  doc.layer_count = 1;
  doc.layers = NULL; /* poisoned */
  st = tinydng_psd_write_memory(ctx, &doc, NULL, &out, &sz, &err);
  CHECK(st == TINYDNG_E_INVALID_ARG && out == NULL,
        "N: NULL layers rejected (%d %s)", (int)st, err.message);
  /* Now a valid layer shell with channels == NULL. */
  memset(&layer, 0, sizeof(layer));
  layer.left = 0;
  layer.top = 0;
  layer.right = 2;
  layer.bottom = 2;
  layer.channel_count = 1;
  layer.channels = NULL; /* poisoned */
  ch.id = 0;
  ch.data = chan;
  ch.size = sizeof(chan);
  (void)ch;
  doc.layers = &layer;
  st = tinydng_psd_write_memory(ctx, &doc, NULL, &out, &sz, &err);
  CHECK(st == TINYDNG_E_INVALID_ARG && out == NULL,
        "N: NULL channels rejected (%d %s)", (int)st, err.message);
  tinydng_context_destroy(ctx);
}

/* O: an opcode carrying nbytes=0 must not abort the rest of its list: a
 * following valid GainMap is still parsed (pre-fix returned early). */
static void case_opcode_zero_nbytes(void) {
  uint8_t b[256];
  size_t p = 0;
  tinydng_context *ctx = tinydng_context_create(NULL, NULL);
  tinydng_document *doc;
  const tinydng_image_info *im;
  size_t total;
  uint8_t *buf;
  ent ex;
  tdt_pu32(b + p, 2, 1);
  p += 4;                       /* two opcodes */
  op_hdr(b, &p, 99u, 0u);       /* unknown id, zero-length payload */
  op_hdr(b, &p, 9u, 92u);       /* GainMap: 76-byte header + 16 bytes */
  tdt_pu32(b + p, 0, 1); p += 4; tdt_pu32(b + p, 0, 1); p += 4;
  tdt_pu32(b + p, 0, 1); p += 4; tdt_pu32(b + p, 0, 1); p += 4;
  tdt_pu32(b + p, 0, 1); p += 4; tdt_pu32(b + p, 1, 1); p += 4;
  tdt_pu32(b + p, 1, 1); p += 4; tdt_pu32(b + p, 1, 1); p += 4;
  tdt_pu32(b + p, 1, 1); p += 4; tdt_pu32(b + p, 1, 1); p += 4; /* mpv=mph=1 */
  tdt_pf64be(b + p, 0.5); p += 8; tdt_pf64be(b + p, 0.5); p += 8;
  tdt_pf64be(b + p, 0.0); p += 8; tdt_pf64be(b + p, 0.0); p += 8;
  tdt_pu32(b + p, 4u, 1); p += 4;               /* map_planes=4 -> 4 items */
  tdt_pf32be(b + p, 1.f); p += 4; tdt_pf32be(b + p, 1.f); p += 4;
  tdt_pf32be(b + p, 1.f); p += 4; tdt_pf32be(b + p, 1.f); p += 4;
  ex.tag = 51009;
  ex.type = 7;
  ex.count = (uint32_t)p;
  ex.val = 12;
  buf = build_img(&ex, 1, b, p, &total);
  im = open_img(ctx, buf, total, &doc);
  CHECK(im != NULL, "O: file opens");
  CHECK(im && im->raw.gainmap_count == 1,
        "O: GainMap after zero-nbytes opcode parsed (count=%zu)",
        im ? im->raw.gainmap_count : (size_t)999);
  if (doc) tinydng_document_destroy(ctx, doc);
  CHECK(tinydng_context_memory_used(ctx) == 0u, "O: no leak");
  tinydng_context_destroy(ctx);
  free(buf);
}

/* P: TIFF LZW strips may end without an explicit EOI code (Photoshop / NASA
 * PDS writers pad the final bits with garbage). The decoder must stop as soon
 * as the expected output size is reached instead of consuming trailing junk.
 * Pre-fix, a junk code AFTER the output was satisfied returned -1 and failed
 * otherwise-valid files. The stream below is CLEAR,'A','A','A','A' followed by
 * eleven 1-bits (junk decodes as code 511 >= next_code 262 -> pre-fix error).
 */
static void case_lzw_trailing_garbage(void) {
  uint16_t codes[5];
  uint8_t lzw[7];
  uint64_t acc = 0;
  int nb = 0, i;
  size_t ob = 0, total;
  uint8_t *buf, *e;
  tinydng_context* ctx;
  tinydng_document* doc = NULL;
  const tinydng_image_info* im;
  const size_t ifd_off = 20; /* past 8-byte header + 4 pad + 7-byte strip */

  codes[0] = 256; /* Clear */
  codes[1] = codes[2] = codes[3] = codes[4] = 65;
  for (i = 0; i < 5; i++) {
    acc = (acc << 9) | codes[i];
    nb += 9;
    while (nb >= 8) {
      lzw[ob++] = (uint8_t)(acc >> (nb - 8));
      nb -= 8;
    }
  }
  acc = (acc << 11) | 0x7FFu; /* trailing garbage, no EOI */
  nb += 11;
  while (nb >= 8) {
    lzw[ob++] = (uint8_t)(acc >> (nb - 8));
    nb -= 8;
  }
  CHECK(ob == sizeof(lzw), "P: fixture packing (%zu)", ob);

  buf = (uint8_t*)calloc(1, ifd_off + 2u + 9u * 12u + 4u);
  CHECK(buf != NULL, "P: alloc");
  if (!buf) {
    return;
  }
  memcpy(buf + 12, lzw, sizeof(lzw)); /* strip data at offset 12 */
  buf[0] = 'I';
  buf[1] = 'I';
  tdt_pu16(buf + 2, 42, 0);
  tdt_pu32(buf + 4, (uint32_t)ifd_off, 0);
  tdt_pu16(buf + ifd_off, 9, 0);
  e = buf + ifd_off + 2u;
#define BE(t, ty, c, v)                 \
  do {                                  \
    tdt_pu16(e, (uint16_t)(t), 0);      \
    tdt_pu16(e + 2, (uint16_t)(ty), 0); \
    tdt_pu32(e + 4, (uint32_t)(c), 0);  \
    tdt_pu32(e + 8, (uint32_t)(v), 0);  \
    e += 12;                            \
  } while (0)
  BE(256, 3, 1, 2);  /* width */
  BE(257, 3, 1, 2);  /* height */
  BE(258, 3, 1, 8);  /* bps */
  BE(259, 3, 1, 5);  /* compression: LZW */
  BE(262, 3, 1, 1);  /* photometric min-is-black */
  BE(273, 4, 1, 12); /* StripOffsets -> strip blob */
  BE(277, 3, 1, 1);  /* spp */
  BE(278, 3, 1, 2);  /* rows per strip */
  BE(279, 4, 1, 7);  /* StripByteCounts */
#undef BE

  ctx = tinydng_context_create(NULL, NULL);
  im = open_img(ctx, buf, ifd_off + 2u + 9u * 12u + 4u, &doc);
  CHECK(im != NULL, "P: file opens");
  if (im) {
    tinydng_pixels px;
    tinydng_error derr;
    tinydng_status st = tinydng_decode_image(ctx, doc, 0, NULL, &px, &derr);
    CHECK(st == TINYDNG_OK, "P: no-EOI LZW strip decodes (%d %s)", (int)st,
          derr.message);
    if (st == TINYDNG_OK) {
      CHECK(px.size == 4 && px.data[0] == 0x41 && px.data[1] == 0x41 &&
                px.data[2] == 0x41 && px.data[3] == 0x41,
            "P: decoded payload intact");
      tinydng_pixels_free(ctx, &px);
    }
  }
  if (doc) tinydng_document_destroy(ctx, doc);
  tinydng_context_destroy(ctx);
  free(buf);
}

/* Q: allocation-failure injection. An allocator that fails after `budget`
 * allocations is used across open + decode (and write) so that every
 * td_ctx_alloc call site is exercised under OOM: the API must return a clean
 * error status and the context must report zero live memory afterwards.
 */
typedef struct {
  tinydng_allocator base;
  size_t countdown; /* allocations remaining before failure */
} fa_state;

static void* fa_alloc(void* ud, size_t n) {
  fa_state* f = (fa_state*)ud;
  if (f->countdown == 0u) {
    return NULL;
  }
  f->countdown--;
  return malloc(n);
}

static void fa_free(void* ud, void* p) {
  (void)ud;
  free(p);
}

/* Run open+decode of `data` under an allocator that fails at every possible
 * allocation ordinal; asserts clean status + zero leak for each. */
static void fa_storm_read(const char* label, const uint8_t* data, size_t size,
                          size_t max_ord) {
  size_t k;
  for (k = 0; k <= max_ord; k++) {
    fa_state fa;
    tinydng_config cfg;
    tinydng_context* ctx;
    tinydng_document* doc = NULL;
    tinydng_error err;
    tinydng_pixels px;
    size_t used_after;
    memset(&fa, 0, sizeof(fa));
    fa.base.alloc = fa_alloc;
    fa.base.free = fa_free;
    fa.base.user_data = &fa;
    fa.countdown = k;
    memset(&cfg, 0, sizeof(cfg));
    cfg.allocator = fa.base;
    ctx = tinydng_context_create(&cfg, &err);
    if (!ctx) {
      continue; /* failing context create itself is acceptable */
    }
    if (tinydng_open_memory(ctx, data, size, NULL, &doc, &err) == TINYDNG_OK) {
      if (tinydng_decode_image(ctx, doc, 0, NULL, &px, &err) == TINYDNG_OK) {
        tinydng_pixels_free(ctx, &px);
      }
      tinydng_document_destroy(ctx, doc);
    }
    used_after = tinydng_context_memory_used(ctx);
    CHECK(used_after == 0u, "Q(%s): leak %zu bytes at alloc-ordinal %zu", label,
          used_after, k);
    tinydng_context_destroy(ctx);
  }
}

static void case_alloc_failure_injection(void) {
  /* Reference inputs built with the default allocator: one uncompressed and
   * one LZW DNG (16x16 gradient, 8-bit). The LZW variant also exercises the
   * per-segment scratch buffers under OOM. */
  const uint32_t W = 16, H = 16;
  uint8_t pixels[16 * 16];
  tinydng_write_image img;
  tinydng_write_options wopts;
  tinydng_context* ref_ctx = tinydng_context_create(NULL, NULL);
  uint8_t *uncomp_dng = NULL, *lzw_dng = NULL;
  size_t uncomp_sz = 0, lzw_sz = 0;
  uint32_t i;
  tinydng_error err;

  CHECK(ref_ctx != NULL, "Q: reference context");
  if (!ref_ctx) {
    return;
  }
  for (i = 0; i < W * H; i++) {
    pixels[i] = (uint8_t)(i * 17u + (i >> 4));
  }
  memset(&img, 0, sizeof(img));
  img.width = W;
  img.height = H;
  img.samples_per_pixel = 1;
  img.bits_per_sample = 8;
  img.sample_format = TINYDNG_SAMPLEFORMAT_UINT;
  img.data = pixels;
  img.data_size = sizeof(pixels);
  memset(&wopts, 0, sizeof(wopts));

  wopts.compression = 1; /* uncompressed */
  if (tinydng_write_memory(ref_ctx, &img, &wopts, &uncomp_dng, &uncomp_sz,
                           &err) != TINYDNG_OK) {
    CHECK(0, "Q: building uncompressed fixture failed: %s", err.message);
  }
  wopts.compression = 5; /* LZW */
  if (tinydng_write_memory(ref_ctx, &img, &wopts, &lzw_dng, &lzw_sz, &err) !=
      TINYDNG_OK) {
    CHECK(0, "Q: building LZW fixture failed: %s", err.message);
  }

  if (uncomp_dng) {
    fa_storm_read("uncomp", uncomp_dng, uncomp_sz, 160);
    tinydng_buffer_free(ref_ctx, uncomp_dng);
  }
  if (lzw_dng) {
    fa_storm_read("lzw", lzw_dng, lzw_sz, 160);
    tinydng_buffer_free(ref_ctx, lzw_dng);
  }

  /* Write-path storm: fail each allocation ordinal inside write_memory. */
  {
    size_t k;
    for (k = 0; k <= 160; k++) {
      fa_state fa;
      tinydng_config cfg;
      tinydng_context* ctx;
      uint8_t* out = NULL;
      size_t out_sz = 0;
      size_t used_after;
      memset(&fa, 0, sizeof(fa));
      fa.base.alloc = fa_alloc;
      fa.base.free = fa_free;
      fa.base.user_data = &fa;
      fa.countdown = k;
      memset(&cfg, 0, sizeof(cfg));
      cfg.allocator = fa.base;
      ctx = tinydng_context_create(&cfg, &err);
      if (!ctx) {
        continue;
      }
      if (tinydng_write_memory(ctx, &img, &wopts, &out, &out_sz, &err) ==
          TINYDNG_OK) {
        tinydng_buffer_free(ctx, out);
      }
      used_after = tinydng_context_memory_used(ctx);
      CHECK(used_after == 0u, "Q(write): leak %zu bytes at ordinal %zu",
            used_after, k);
      tinydng_context_destroy(ctx);
    }
  }
  tinydng_context_destroy(ref_ctx);
}

int main(void) {
  (void)tdt_slurp; /* shared helper unused by this all-in-memory test */
  printf("== v3 security regression fixtures ==\n");
  case_gainmap_overflow();
  case_gainmap_short_nbytes();
  case_segment_array_cap();
  case_rational_type();
  case_linearization_stride();
  case_duplicate_tag();
  case_ljpeg_ssss();
  case_ljpeg_truncated_segment();
  case_jpeg_if_clamp();
  case_ljpeg_stream_overreporting();
  case_ljpeg_linearize_bound();
  case_writer_cfa_clamp();
  case_writer_nan_rational();
  case_psdw_null_layers();
  case_opcode_zero_nbytes();
  case_lzw_trailing_garbage();
  case_alloc_failure_injection();
  printf(g_fail ? "SECURITY: FAILURES\n" : "SECURITY: ALL PASS\n");
  return g_fail;
}
