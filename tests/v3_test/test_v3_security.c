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
 */
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
  printf(g_fail ? "SECURITY: FAILURES\n" : "SECURITY: ALL PASS\n");
  return g_fail;
}
