#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tiny_dng_ljpeg92_v2.h"

static int failures;
#define CHECK(c, ...)                                    \
  do {                                                   \
    if (!(c)) {                                          \
      fprintf(stderr, "FAIL: ");                         \
      fprintf(stderr, __VA_ARGS__);                      \
      fprintf(stderr, " (%s:%d)\n", __FILE__, __LINE__); \
      failures++;                                        \
    }                                                    \
  } while (0)

static size_t marker_at(const uint8_t* p, size_t n, uint8_t marker) {
  size_t i;
  for (i = 0; i + 1u < n; i++) {
    if (p[i] == 0xffu && p[i + 1u] == marker) return i;
  }
  return SIZE_MAX;
}

typedef struct fail_alloc {
  size_t ordinal;
  size_t fail_at;
  size_t live;
} fail_alloc;

static void* fa_alloc(void* user, size_t size) {
  fail_alloc* a = (fail_alloc*)user;
  void* p;
  a->ordinal++;
  if (a->fail_at && a->ordinal == a->fail_at) return NULL;
  p = malloc(size);
  if (p) a->live++;
  return p;
}

static void fa_free(void* user, void* ptr) {
  fail_alloc* a = (fail_alloc*)user;
  if (!ptr) return;
  CHECK(a->live > 0u, "allocator live underflow");
  if (a->live) a->live--;
  free(ptr);
}

typedef struct short_sink {
  size_t accepted;
  size_t limit;
} short_sink;

typedef struct fixed_sink {
  uint8_t data[1024];
  size_t size;
} fixed_sink;

static size_t fixed_write(void* user, const void* data, size_t size) {
  fixed_sink* s = (fixed_sink*)user;
  if (size > sizeof(s->data) - s->size) return 0;
  memcpy(s->data + s->size, data, size);
  s->size += size;
  return size;
}

static size_t short_write(void* user, const void* data, size_t size) {
  short_sink* s = (short_sink*)user;
  size_t room = s->accepted < s->limit ? s->limit - s->accepted : 0u;
  size_t n = size < room ? size : room;
  (void)data;
  s->accepted += n;
  return n;
}

static void put8(uint8_t* b, size_t* n, uint8_t v) { b[(*n)++] = v; }
static void put16(uint8_t* b, size_t* n, unsigned v) {
  put8(b, n, (uint8_t)(v >> 8));
  put8(b, n, (uint8_t)v);
}
static void marker(uint8_t* b, size_t* n, uint8_t m) {
  put8(b, n, 0xffu);
  put8(b, n, m);
}

/* Constant-valued SOF3 fixture. Its sole Huffman code is category 0 => bit 0,
   so scan payloads are easy to construct exactly and independently. */
static size_t constant_fixture(uint8_t* b, int multiscan, int restart) {
  size_t n = 0;
  marker(b, &n, 0xd8);
  marker(b, &n, 0xc3);
  put16(b, &n, 14); /* 8 + 3*2 */
  put8(b, &n, 8);
  put16(b, &n, 2);
  put16(b, &n, 4);
  put8(b, &n, 2);
  put8(b, &n, 1);
  put8(b, &n, 0x21);
  put8(b, &n, 0);
  put8(b, &n, 2);
  put8(b, &n, 0x11);
  put8(b, &n, 0);
  marker(b, &n, 0xc4);
  put16(b, &n, 20);
  put8(b, &n, 0);
  put8(b, &n, 1);
  for (int i = 1; i < 16; i++) put8(b, &n, 0);
  put8(b, &n, 0); /* category zero */
  if (restart) {
    marker(b, &n, 0xdd);
    put16(b, &n, 4);
    put16(b, &n, 2);
  }
  if (!multiscan) {
    marker(b, &n, 0xda);
    put16(b, &n, 10);
    put8(b, &n, 2);
    put8(b, &n, 1);
    put8(b, &n, 0);
    put8(b, &n, 2);
    put8(b, &n, 0);
    put8(b, &n, 1);
    put8(b, &n, 0);
    put8(b, &n, 2); /* predictor1, Pt=2 */
    if (restart) {
      put8(b, &n, 0x03);
      marker(b, &n, 0xd0);
      put8(b, &n, 0x03);
    } else {
      put8(b, &n, 0x00);
      put8(b, &n, 0x0f); /* 12 zero codes + pad */
    }
  } else {
    marker(b, &n, 0xda);
    put16(b, &n, 8);
    put8(b, &n, 1);
    put8(b, &n, 1);
    put8(b, &n, 0);
    put8(b, &n, 1);
    put8(b, &n, 0);
    put8(b, &n, 2); /* eight zero codes */
    put8(b, &n, 0x00);
    marker(b, &n, 0xda);
    put16(b, &n, 8);
    put8(b, &n, 1);
    put8(b, &n, 2);
    put8(b, &n, 0);
    put8(b, &n, 1);
    put8(b, &n, 0);
    put8(b, &n, 2); /* four zero codes + pad */
    put8(b, &n, 0x0f);
  }
  marker(b, &n, 0xd9);
  return n;
}

/* SOF11 predictor-4 conformance fixture. The 8x4 sample result was
   independently decoded with Thomas Richter's complete T.81 codec. */
static const uint8_t sof11_interop[] = {
    0xff, 0xd8, 0xff, 0xcb, 0x00, 0x0b, 0x08, 0x00, 0x04, 0x00, 0x08, 0x01,
    0x01, 0x11, 0x00, 0xff, 0xcc, 0x00, 0x04, 0x00, 0x10, 0xff, 0xdd, 0x00,
    0x04, 0x00, 0x00, 0xff, 0xda, 0x00, 0x08, 0x01, 0x01, 0x00, 0x04, 0x00,
    0x00, 0xff, 0x00, 0xcd, 0x7d, 0x2f, 0x6b, 0xc7, 0x91, 0x1d, 0xec, 0x90,
    0x0d, 0xbd, 0x73, 0x7c, 0x6c, 0x2f, 0x82, 0xce, 0xe9, 0x36, 0xff, 0xd9};
static const uint16_t sof11_interop_pixels[32] = {
    10, 30, 55, 80, 110, 140, 175, 210, 14, 33, 58, 84, 114, 146, 180, 218,
    21, 38, 64, 91, 121, 153, 188, 225, 27, 44, 70, 98, 128, 160, 196, 233};

int main(void) {
  enum { W = 37, H = 19, C = 3 };
  uint16_t image[W * H * C];
  uint16_t decoded[W * H * C];
  uint8_t* encoded = NULL;
  int encoded_len = 0;
  int i, rc;
  tdng_lj92 dec = NULL;
  int w = 0, h = 0, bits = 0, comps = 0;

  {
    tdng_lj92_plane plane;
    uint16_t out[32] = {0};
    memset(&plane, 0, sizeof(plane));
    rc = tdng_lj92_open(&dec, sof11_interop, (int)sizeof(sof11_interop), &w, &h,
                        &bits, &comps);
    CHECK(rc == 0 && w == 8 && h == 4 && bits == 8 && comps == 1,
          "SOF11 interop fixture opens rc=%d", rc);
    if (rc == 0) {
      plane.data = out;
      plane.capacity_samples = 32;
      plane.row_stride_samples = 8;
      plane.pixel_stride_samples = 1;
      plane.width = 8;
      plane.height = 4;
      rc = tdng_lj92_decode_planes(dec, &plane, 1);
      CHECK(rc == 0 && memcmp(out, sof11_interop_pixels, sizeof(out)) == 0,
            "SOF11 interop fixture decodes rc=%d", rc);
    }
    tdng_lj92_close(dec);
    dec = NULL;
  }

  for (i = 0; i < W * H * C; i++) {
    image[i] = (uint16_t)((i * 73 + (i / C) * 19) & 0x0fffu);
  }
  rc = tdng_lj92_encode_ex(image, W, H, 12, C, 7, W * C, 0, NULL, 0, &encoded,
                           &encoded_len);
  CHECK(rc == TDNG_LJ92_ERROR_NONE && encoded && encoded_len > 8,
        "fixture encode rc=%d", rc);

  CHECK(tdng_lj92_open(NULL, encoded, encoded_len, &w, &h, &bits, &comps) ==
            TDNG_LJ92_ERROR_INVALID_ARGUMENT,
        "open validates result handle");
  rc = tdng_lj92_open(&dec, encoded, encoded_len, &w, &h, &bits, &comps);
  CHECK(rc == 0 && w == W && h == H && bits == 12 && comps == C,
        "open valid stream rc=%d dims=%dx%dx%d/%d", rc, w, h, comps, bits);
  CHECK(tdng_lj92_decode(dec, NULL, W * C, 0, NULL, 0) ==
            TDNG_LJ92_ERROR_INVALID_ARGUMENT,
        "decode rejects NULL output");
  rc = tdng_lj92_decode(dec, decoded, W * C, 0, NULL, 0);
  CHECK(rc == 0 && memcmp(image, decoded, sizeof(image)) == 0,
        "round-trip rc=%d", rc);
  tdng_lj92_close(dec);
  dec = NULL;

  /* The header can still open, but removal of EOI must fail exact entropy
     accounting rather than decoding from synthetic zero padding. */
  rc = tdng_lj92_open(&dec, encoded, encoded_len - 2, &w, &h, &bits, &comps);
  CHECK(rc == 0, "truncated payload header opens rc=%d", rc);
  if (rc == 0) {
    rc = tdng_lj92_decode(dec, decoded, W * C, 0, NULL, 0);
    CHECK(rc == TDNG_LJ92_ERROR_CORRUPT, "missing EOI rejected rc=%d", rc);
  }
  tdng_lj92_close(dec);
  dec = NULL;

  {
    uint8_t* copy = (uint8_t*)malloc((size_t)encoded_len);
    size_t dht, sos;
    memcpy(copy, encoded, (size_t)encoded_len);
    dht = marker_at(copy, (size_t)encoded_len, 0xc4u);
    sos = marker_at(copy, (size_t)encoded_len, 0xdau);
    CHECK(dht != SIZE_MAX && sos != SIZE_MAX, "fixture markers present");
    if (dht != SIZE_MAX && sos != SIZE_MAX) {
      copy[dht + 4u] = 2u; /* Th=2 */
      for (i = 0; i < C; i++) copy[sos + 6u + (size_t)i * 2u] = 0x20u;
      rc = tdng_lj92_open(&dec, copy, encoded_len, &w, &h, &bits, &comps);
      CHECK(rc == 0, "nonzero Huffman selector opens rc=%d", rc);
      if (rc == 0) {
        rc = tdng_lj92_decode(dec, decoded, W * C, 0, NULL, 0);
        CHECK(rc == 0 && memcmp(image, decoded, sizeof(image)) == 0,
              "nonzero table selector decodes rc=%d", rc);
      }
      tdng_lj92_close(dec);
      dec = NULL;

      memcpy(copy, encoded, (size_t)encoded_len);
      copy[dht + 5u] = 3u; /* three one-bit codes: oversubscribed */
      rc = tdng_lj92_open(&dec, copy, encoded_len, &w, &h, &bits, &comps);
      CHECK(rc == TDNG_LJ92_ERROR_CORRUPT,
            "oversubscribed Huffman table rejected rc=%d", rc);
      tdng_lj92_close(dec);
      dec = NULL;
    }
    free(copy);
  }

  /* Native-plane decode covers legal component sampling, point transform,
     non-interleaved multiple scans, and restart-boundary prediction reset. */
  for (int mode = 0; mode < 3; mode++) {
    uint8_t fixture[256];
    size_t fixture_size = constant_fixture(fixture, mode == 1, mode == 2);
    tdng_lj92_frame_info fi;
    uint16_t p0[8] = {0}, p1[4] = {0};
    tdng_lj92_plane planes[2];
    rc =
        tdng_lj92_open(&dec, fixture, (int)fixture_size, &w, &h, &bits, &comps);
    CHECK(rc == 0, "native fixture mode %d opens rc=%d", mode, rc);
    if (rc != 0) continue;
    rc = tdng_lj92_get_frame_info(dec, &fi);
    CHECK(rc == 0 && fi.component_count == 2 && fi.components[0].width == 4 &&
              fi.components[0].height == 2 && fi.components[1].width == 2 &&
              fi.components[1].height == 2,
          "native fixture mode %d geometry rc=%d", mode, rc);
    memset(planes, 0, sizeof(planes));
    planes[0].data = p0;
    planes[0].capacity_samples = 8;
    planes[0].row_stride_samples = 4;
    planes[0].pixel_stride_samples = 1;
    planes[0].width = 4;
    planes[0].height = 2;
    planes[1].data = p1;
    planes[1].capacity_samples = 4;
    planes[1].row_stride_samples = 2;
    planes[1].pixel_stride_samples = 1;
    planes[1].width = 2;
    planes[1].height = 2;
    rc = tdng_lj92_decode_planes(dec, planes, 2);
    CHECK(rc == 0, "native fixture mode %d decodes rc=%d", mode, rc);
    for (i = 0; i < 8; i++)
      CHECK(p0[i] == 128, "mode %d p0[%d]=%u", mode, i, (unsigned)p0[i]);
    for (i = 0; i < 4; i++)
      CHECK(p1[i] == 128, "mode %d p1[%d]=%u", mode, i, (unsigned)p1[i]);
    tdng_lj92_close(dec);
    dec = NULL;
  }

  /* An incomplete Huffman tree is legal, but entropy selecting an unused
     prefix is corrupt. The invalid LUT entry must remain shift-safe and its
     error flag must stay sticky even if valid symbols follow it. */
  {
    uint8_t fixture[256];
    size_t fixture_size = constant_fixture(fixture, 0, 0);
    size_t sos = marker_at(fixture, fixture_size, 0xdau);
    uint16_t p0[8] = {0}, p1[4] = {0};
    tdng_lj92_plane planes[2];
    CHECK(sos != SIZE_MAX && sos + 12u < fixture_size,
          "incomplete-tree fixture SOS present");
    if (sos != SIZE_MAX && sos + 12u < fixture_size) {
      fixture[sos + 12u] |= 0x80u;
      rc = tdng_lj92_open(&dec, fixture, (int)fixture_size, &w, &h, &bits,
                          &comps);
      CHECK(rc == 0, "incomplete-tree fixture opens rc=%d", rc);
      if (rc == 0) {
        memset(planes, 0, sizeof(planes));
        planes[0].data = p0;
        planes[0].capacity_samples = 8;
        planes[0].row_stride_samples = 4;
        planes[0].pixel_stride_samples = 1;
        planes[0].width = 4;
        planes[0].height = 2;
        planes[1].data = p1;
        planes[1].capacity_samples = 4;
        planes[1].row_stride_samples = 2;
        planes[1].pixel_stride_samples = 1;
        planes[1].width = 2;
        planes[1].height = 2;
        rc = tdng_lj92_decode_planes(dec, planes, 2);
        CHECK(rc == TDNG_LJ92_ERROR_CORRUPT,
              "unused Huffman prefix rejected rc=%d", rc);
      }
      tdng_lj92_close(dec);
      dec = NULL;
    }
  }

  for (int mode = 0; mode < 6; mode++) {
    int topology = mode % 3;
    int arithmetic = mode >= 3;
    tdng_lj92_frame_info frame;
    tdng_lj92_const_plane src[2];
    tdng_lj92_plane dst[2];
    tdng_lj92_scan_plan scan[2];
    fixed_sink sink;
    uint16_t in0[8], in1[4], out0[8] = {0}, out1[4] = {0};
    memset(&frame, 0, sizeof(frame));
    memset(src, 0, sizeof(src));
    memset(dst, 0, sizeof(dst));
    memset(scan, 0, sizeof(scan));
    memset(&sink, 0, sizeof(sink));
    for (i = 0; i < 8; i++) in0[i] = (uint16_t)(96 + ((i * 17) & 31) * 4);
    for (i = 0; i < 4; i++) in1[i] = (uint16_t)(112 + ((i * 11) & 15) * 4);
    frame.width = 4;
    frame.height = 2;
    frame.precision = 8;
    frame.component_count = 2;
    frame.sof_marker = (uint8_t)(arithmetic ? 0xcb : 0xc3);
    frame.components[0].id = 1;
    frame.components[0].h_sampling = 2;
    frame.components[0].v_sampling = 1;
    frame.components[0].width = 4;
    frame.components[0].height = 2;
    frame.components[1].id = 2;
    frame.components[1].h_sampling = 1;
    frame.components[1].v_sampling = 1;
    frame.components[1].width = 2;
    frame.components[1].height = 2;
    src[0].data = in0;
    src[0].capacity_samples = 8;
    src[0].row_stride_samples = 4;
    src[0].pixel_stride_samples = 1;
    src[0].width = 4;
    src[0].height = 2;
    src[1].data = in1;
    src[1].capacity_samples = 4;
    src[1].row_stride_samples = 2;
    src[1].pixel_stride_samples = 1;
    src[1].width = 2;
    src[1].height = 2;
    if (topology == 1) {
      scan[0].component_count = 1;
      scan[0].component_index[0] = 0;
      scan[1].component_count = 1;
      scan[1].component_index[0] = 1;
    } else {
      scan[0].component_count = 2;
      scan[0].component_index[0] = 0;
      scan[0].component_index[1] = 1;
      if (topology == 2) scan[0].restart_interval_mcus = 2;
    }
    scan[0].predictor = 1;
    scan[0].point_transform = 2;
    scan[1].predictor = 1;
    scan[1].point_transform = 2;
    rc = tdng_lj92_encode_frame(&frame, src, 2, scan, topology == 1 ? 2u : 1u,
                                &sink, fixed_write, NULL);
    CHECK(rc == 0, "advanced encoder mode %d rc=%d", mode, rc);
    if (rc != 0) continue;
    rc = tdng_lj92_open(&dec, sink.data, (int)sink.size, &w, &h, &bits, &comps);
    CHECK(rc == 0, "advanced output mode %d opens rc=%d", mode, rc);
    if (rc != 0) continue;
    dst[0].data = out0;
    dst[0].capacity_samples = 8;
    dst[0].row_stride_samples = 4;
    dst[0].pixel_stride_samples = 1;
    dst[0].width = 4;
    dst[0].height = 2;
    dst[1].data = out1;
    dst[1].capacity_samples = 4;
    dst[1].row_stride_samples = 2;
    dst[1].pixel_stride_samples = 1;
    dst[1].width = 2;
    dst[1].height = 2;
    rc = tdng_lj92_decode_planes(dec, dst, 2);
    CHECK(rc == 0 && memcmp(in0, out0, sizeof(in0)) == 0 &&
              memcmp(in1, out1, sizeof(in1)) == 0,
          "advanced output mode %d round-trips rc=%d", mode, rc);
    tdng_lj92_close(dec);
    dec = NULL;
  }

  /* SOF11's entropy/history allocations obey the caller allocator and unwind
     cleanly at every failure ordinal in both directions. */
  for (i = 1; i < 16; i++) {
    fail_alloc a = {0};
    tdng_lj92_allocator al = {fa_alloc, fa_free, &a};
    tdng_lj92_plane plane;
    uint16_t out[32];
    a.fail_at = (size_t)i;
    memset(&plane, 0, sizeof(plane));
    rc = tdng_lj92_open_ex(&dec, sof11_interop, (int)sizeof(sof11_interop), &al,
                           &w, &h, &bits, &comps);
    if (rc == 0) {
      plane.data = out;
      plane.capacity_samples = 32;
      plane.row_stride_samples = 8;
      plane.pixel_stride_samples = 1;
      plane.width = 8;
      plane.height = 4;
      rc = tdng_lj92_decode_planes(dec, &plane, 1);
    }
    tdng_lj92_close(dec);
    dec = NULL;
    CHECK(a.live == 0u, "SOF11 decode failure ordinal %d leaked %zu", i,
          a.live);
    if (rc == 0) break;
    CHECK(rc == TDNG_LJ92_ERROR_NO_MEMORY,
          "SOF11 decode failure ordinal %d rc=%d", i, rc);
  }
  CHECK(i < 16, "SOF11 decode allocator sweep eventually succeeds");

  for (i = 1; i < 8; i++) {
    fail_alloc a = {0};
    tdng_lj92_allocator al = {fa_alloc, fa_free, &a};
    tdng_lj92_frame_info frame;
    tdng_lj92_const_plane plane;
    tdng_lj92_scan_plan scan;
    fixed_sink sink;
    memset(&frame, 0, sizeof(frame));
    memset(&plane, 0, sizeof(plane));
    memset(&scan, 0, sizeof(scan));
    memset(&sink, 0, sizeof(sink));
    a.fail_at = (size_t)i;
    frame.width = 8;
    frame.height = 4;
    frame.precision = 8;
    frame.component_count = 1;
    frame.sof_marker = 0xcb;
    frame.components[0].id = 1;
    frame.components[0].h_sampling = 1;
    frame.components[0].v_sampling = 1;
    frame.components[0].width = 8;
    frame.components[0].height = 4;
    plane.data = sof11_interop_pixels;
    plane.capacity_samples = 32;
    plane.row_stride_samples = 8;
    plane.pixel_stride_samples = 1;
    plane.width = 8;
    plane.height = 4;
    scan.component_count = 1;
    scan.predictor = 4;
    rc = tdng_lj92_encode_frame(&frame, &plane, 1, &scan, 1, &sink, fixed_write,
                                &al);
    CHECK(a.live == 0u, "SOF11 encode failure ordinal %d leaked %zu", i,
          a.live);
    if (rc == 0) break;
    CHECK(rc == TDNG_LJ92_ERROR_NO_MEMORY,
          "SOF11 encode failure ordinal %d rc=%d", i, rc);
  }
  CHECK(i < 8, "SOF11 encode allocator sweep eventually succeeds");

  /* Every decoder-owned allocation is routed through the supplied allocator,
     and every failure ordinal unwinds to zero live allocations. */
  for (i = 1; i < 32; i++) {
    fail_alloc a;
    tdng_lj92_allocator al;
    memset(&a, 0, sizeof(a));
    a.fail_at = (size_t)i;
    al.alloc = fa_alloc;
    al.free = fa_free;
    al.user = &a;
    rc = tdng_lj92_open_ex(&dec, encoded, encoded_len, &al, &w, &h, &bits,
                           &comps);
    if (rc == 0) {
      rc = tdng_lj92_decode(dec, decoded, W * C, 0, NULL, 0);
    }
    tdng_lj92_close(dec);
    dec = NULL;
    CHECK(a.live == 0u, "allocator failure ordinal %d leaked %zu", i, a.live);
    if (rc == 0) break;
    CHECK(rc == TDNG_LJ92_ERROR_NO_MEMORY, "allocator failure ordinal %d rc=%d",
          i, rc);
  }
  CHECK(i < 32, "allocator sweep eventually succeeds");

  {
    tdng_lj92_enc enc = NULL;
    short_sink sink = {0u, 1u};
    rc = tdng_lj92_encode_open(&enc, W, H, 12, C, 7, W * C, 0, &sink,
                               short_write);
    CHECK(rc == 0, "short-sink encoder opens rc=%d", rc);
    if (rc == 0) rc = tdng_lj92_encode_scan(enc, image, NULL, 0);
    if (rc == 0) rc = tdng_lj92_encode_begin(enc);
    CHECK(rc == TDNG_LJ92_ERROR_IO, "short sink is terminal IO rc=%d", rc);
    CHECK(tdng_lj92_encode_rows(enc, image, 0, H) == TDNG_LJ92_ERROR_STATE,
          "terminal encoder cannot retry rows");
    CHECK(tdng_lj92_encode_finish(enc) == TDNG_LJ92_ERROR_STATE,
          "finish reports terminal state and frees handle");
  }

  free(encoded);
  if (failures) {
    fprintf(stderr, "LJ92: %d failure(s)\n", failures);
    return 1;
  }
  puts("LJ92: OK");
  return 0;
}
