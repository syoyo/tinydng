/*
 * test_v2_streamencode.c - streaming LJPEG92 encode (tdng_lj92_encode_*)
 * vs the one-shot tdng_lj92_encode_ex: outputs must be byte-identical,
 * including when rows are fed incrementally in chunks. Also covers the DNG
 * tiled layout (readLength/skipLength), decode round-trips and error paths.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tiny_dng_ljpeg92_v2.h"

static int test_count = 0;
static int pass_count = 0;
static int fail_count = 0;

static void check(int condition, const char* msg) {
  test_count++;
  if (condition) {
    pass_count++;
    printf("  [PASS] %s\n", msg);
  } else {
    fail_count++;
    printf("  [FAIL] %s\n", msg);
  }
}

/* ---- growable buffer sink ------------------------------------------- */

typedef struct mbuf {
  uint8_t* data;
  size_t cap;
  size_t len;
} mbuf;

static size_t mbuf_write(void* user, const void* data, size_t len) {
  mbuf* m = (mbuf*)user;
  size_t need = m->len + len;
  if (need < m->len) return 0;
  if (need > m->cap) {
    size_t cap = m->cap ? m->cap : 4096u;
    while (cap < need) cap *= 2;
    uint8_t* nb = (uint8_t*)realloc(m->data, cap);
    if (!nb) return 0;
    m->data = nb;
    m->cap = cap;
  }
  memcpy(m->data + m->len, data, len);
  m->len += len;
  return len;
}

/* ---- failing sink: accepts at most `limit` total bytes -------------- */

typedef struct fbuf {
  size_t limit;
  size_t written;
} fbuf;

static size_t fbuf_write(void* user, const void* data, size_t len) {
  fbuf* f = (fbuf*)user;
  (void)data;
  if (f->written >= f->limit) return 0;
  if (f->written + len > f->limit) {
    f->written = f->limit;
    return 0; /* short write => sink failure */
  }
  f->written += len;
  return len;
}

/* ---- encode a full image via the streaming API ----------------------- */

static int stream_encode(const uint16_t* img, int w, int h, int bps, int comps,
                         int pred, int read_len, int skip_len,
                         const uint16_t* delin, int delin_len,
                         const int* row_chunks, int n_chunks, mbuf* out) {
  tdng_lj92_enc lj = NULL;
  int ret;
  out->data = NULL;
  out->cap = 0;
  out->len = 0;
  ret = tdng_lj92_encode_open(&lj, w, h, bps, comps, pred, read_len, skip_len,
                              out, mbuf_write);
  if (ret != TDNG_LJ92_ERROR_NONE) return ret;
  ret = tdng_lj92_encode_scan(lj, img, delin, delin_len);
  if (ret == TDNG_LJ92_ERROR_NONE) ret = tdng_lj92_encode_begin(lj);
  if (ret == TDNG_LJ92_ERROR_NONE) {
    int row0 = 0;
    for (int k = 0; k < n_chunks && ret == TDNG_LJ92_ERROR_NONE; k++) {
      int n = row_chunks[k];
      if (n > h - row0) n = h - row0;
      if (n <= 0) break;
      ret = tdng_lj92_encode_rows(lj, img, row0, n);
      row0 += n;
    }
  }
  if (ret == TDNG_LJ92_ERROR_NONE) {
    ret = tdng_lj92_encode_finish(lj);
  } else {
    tdng_lj92_encode_finish(lj); /* release encoder state */
  }
  return ret;
}

/* ---- main matrix test ------------------------------------------------ */

static void test_parity_matrix(void) {
  static const int predictors[] = {1, 2, 7};
  static const int comps_list[] = {1, 3};
  static const int bps_list[] = {8, 10, 12, 16};
  static const int chunks_whole[] = {1000};  /* one call covering all rows */
  static const int chunks_odd[] = {1, 3, 7, 5, 2, 9, 4, 6, 11}; /* strided */
  int p, ci, bi;
  printf("Testing streaming == one-shot byte parity...\n");

  for (p = 0; p < 3; p++) {
    for (ci = 0; ci < 2; ci++) {
      for (bi = 0; bi < 4; bi++) {
        int W = 61, H = 37;
        int comps = comps_list[ci];
        int bps = bps_list[bi];
        int pred = predictors[p];
        size_t npx = (size_t)W * H * (size_t)comps;
        uint16_t* img = (uint16_t*)malloc(npx * sizeof(uint16_t));
        uint8_t* ref = NULL;
        int ref_len = 0;
        mbuf whole, odd;
        char name[128];
        int i;
        int ret;

        snprintf(name, sizeof(name), "p=%d c=%d b=%d", pred, comps, bps);
        for (i = 0; i < (int)npx; i++) {
          int v = (i * 2654435761u) & ((1u << bps) - 1u);
          img[i] = (uint16_t)v;
        }
        ret = tdng_lj92_encode_ex(img, W, H, bps, comps, pred, W * comps, 0,
                                  NULL, 0, &ref, &ref_len);
        if (ret != TDNG_LJ92_ERROR_NONE || !ref) {
          printf("  [FAIL] %s: one-shot encode failed ret=%d\n", name, ret);
          free(ref);
          free(img);
          fail_count++;
          test_count++;
          continue;
        }
        ret = stream_encode(img, W, H, bps, comps, pred, W * comps, 0, NULL, 0,
                            chunks_whole, 1, &whole);
        if (ret != TDNG_LJ92_ERROR_NONE || whole.len != (size_t)ref_len ||
            memcmp(whole.data, ref, (size_t)ref_len) != 0) {
          printf("  [FAIL] %s: whole-row stream differs (ret=%d len=%zu vs %d)\n",
                 name, ret, whole.len, ref_len);
          check(0, name);
        } else {
          int ok = 1;
          ret = stream_encode(img, W, H, bps, comps, pred, W * comps, 0, NULL,
                              0, chunks_odd, 9, &odd);
          if (ret != TDNG_LJ92_ERROR_NONE || odd.len != (size_t)ref_len ||
              memcmp(odd.data, ref, (size_t)ref_len) != 0) {
            printf("  [FAIL] %s: chunked rows differ (ret=%d len=%zu)\n", name,
                   ret, odd.len);
            ok = 0;
          }
          check(ok, name);
        }
        free(ref);
        free(whole.data);
        free(odd.data);
        free(img);
      }
    }
  }
}

/* ---- DNG tiled layout (readLength/skipLength) ------------------------ */

static void test_dng_tile_layout(void) {
  const int FW = 40, FH = 10;      /* full frame */
  const int TW = 16, TH = 4;       /* tile at (8, 3) */
  const int comps = 3;
  const int bps = 12;
  const int tx = 8, ty = 3;
  const int read_len = TW * comps;                 /* tile row samples */
  const int skip_len = (FW - TW) * comps;          /* frame gap per row */
  size_t frame_px = (size_t)FW * FH * (size_t)comps;
  size_t tile_px = (size_t)TW * TH * (size_t)comps;
  uint16_t* frame = (uint16_t*)malloc(frame_px * sizeof(uint16_t));
  uint16_t* tile = (uint16_t*)malloc(tile_px * sizeof(uint16_t));
  uint16_t* dec = (uint16_t*)malloc(tile_px * sizeof(uint16_t));
  mbuf out;
  int ret;
  int i;

  printf("Testing DNG tiled layout (readLength/skipLength)...\n");
  for (i = 0; i < (int)frame_px; i++) {
    frame[i] = (uint16_t)((i * 0x9E3779B9u) & 0xFFFu);
  }
  /* tile = sub-rect of the frame, rows contiguous in the frame */
  for (int r = 0; r < TH; r++) {
    memcpy(tile + (size_t)r * TW * (size_t)comps,
           frame + ((size_t)(ty + r) * FW + (size_t)tx) * (size_t)comps,
           (size_t)TW * (size_t)comps * sizeof(uint16_t));
  }
  /* one-shot with the frame layout: image base points at the tile's first
     sample inside the frame; the encoder skips the frame gap per row */
  {
    const uint16_t* tile_in_frame = frame + ((size_t)ty * FW + (size_t)tx) * (size_t)comps;
    uint8_t* ref = NULL;
    int ref_len = 0;
    ret = tdng_lj92_encode_ex((uint16_t*)(uintptr_t)tile_in_frame, TW, TH, bps,
                              comps, 1, read_len, skip_len, NULL, 0, &ref,
                              &ref_len);
    check(ret == TDNG_LJ92_ERROR_NONE && ref != NULL,
          "tile one-shot encode");
    if (ret == TDNG_LJ92_ERROR_NONE && ref) {
      ret = stream_encode(tile_in_frame, TW, TH, bps, comps, 1, read_len,
                          skip_len, NULL, 0, (const int[]){2, 2}, 2, &out);
      check(ret == TDNG_LJ92_ERROR_NONE && out.len == (size_t)ref_len &&
                memcmp(out.data, ref, (size_t)ref_len) == 0,
            "tile streaming == one-shot (skip layout)");
      if (ret == TDNG_LJ92_ERROR_NONE) {
        tdng_lj92 ljd = NULL;
        int w, h, b, c;
        ret = tdng_lj92_open(&ljd, out.data, (int)out.len, &w, &h, &b, &c);
        if (ret == TDNG_LJ92_ERROR_NONE && ljd) {
          ret = tdng_lj92_decode(ljd, dec, w * c, 0, NULL, 0);
          tdng_lj92_close(ljd);
        }
        check(ret == TDNG_LJ92_ERROR_NONE && w == TW && h == TH &&
                  memcmp(dec, tile, tile_px * sizeof(uint16_t)) == 0,
              "tile decode round-trip");
      }
      free(ref);
      free(out.data);
    }
  }
  free(frame);
  free(tile);
  free(dec);
}

/* ---- error paths ----------------------------------------------------- */

static void test_errors(void) {
  uint16_t img[64 * 64 * 3];
  mbuf out;
  fbuf f;
  tdng_lj92_enc lj = NULL;
  int ret;
  int i;

  printf("Testing error paths...\n");
  for (i = 0; i < 64 * 64 * 3; i++) img[i] = (uint16_t)((i * 7u) & 0xFFu);

  /* bad params at open */
  ret = tdng_lj92_encode_open(NULL, 8, 8, 8, 1, 1, 8, 0, &out, mbuf_write);
  check(ret == TDNG_LJ92_ERROR_BAD_HANDLE, "open: NULL handle");
  ret = tdng_lj92_encode_open(&lj, 8, 8, 8, 1, 1, 8, 0, &out, NULL);
  check(ret == TDNG_LJ92_ERROR_BAD_HANDLE, "open: NULL sink");
  ret = tdng_lj92_encode_open(&lj, 0, 8, 8, 1, 1, 8, 0, &out, mbuf_write);
  check(ret == TDNG_LJ92_ERROR_BAD_HANDLE, "open: zero width");
  ret = tdng_lj92_encode_open(&lj, 8, 8, 17, 1, 1, 8, 0, &out, mbuf_write);
  check(ret == TDNG_LJ92_ERROR_BAD_HANDLE, "open: bitdepth 17");
  ret = tdng_lj92_encode_open(&lj, 8, 8, 8, 5, 1, 8, 0, &out, mbuf_write);
  check(ret == TDNG_LJ92_ERROR_BAD_HANDLE, "open: 5 components");
  ret = tdng_lj92_encode_open(&lj, 8, 8, 8, 1, 8, 8, 0, &out, mbuf_write);
  check(ret == TDNG_LJ92_ERROR_BAD_HANDLE, "open: predictor 8");

  ret = tdng_lj92_encode_open(&lj, 8, 8, 8, 1, 1, 8, 0, &out, mbuf_write);
  check(ret == TDNG_LJ92_ERROR_NONE, "open ok");
  if (ret != TDNG_LJ92_ERROR_NONE) return;

  ret = tdng_lj92_encode_begin(lj);
  check(ret == TDNG_LJ92_ERROR_BAD_HANDLE, "begin before scan");
  ret = tdng_lj92_encode_rows(lj, img, 0, 8);
  check(ret == TDNG_LJ92_ERROR_BAD_HANDLE, "rows before scan/begin");
  ret = tdng_lj92_encode_scan(lj, NULL, NULL, 0);
  check(ret == TDNG_LJ92_ERROR_BAD_HANDLE, "scan: NULL image");
  ret = tdng_lj92_encode_scan(lj, img, NULL, 0);
  check(ret == TDNG_LJ92_ERROR_NONE, "scan ok");
  ret = tdng_lj92_encode_scan(lj, img, NULL, 0);
  check(ret == TDNG_LJ92_ERROR_BAD_HANDLE, "scan twice");
  ret = tdng_lj92_encode_begin(lj);
  check(ret == TDNG_LJ92_ERROR_NONE, "begin ok");
  ret = tdng_lj92_encode_rows(lj, img, 1, 4);
  check(ret == TDNG_LJ92_ERROR_BAD_HANDLE, "rows: wrong start row");
  ret = tdng_lj92_encode_rows(lj, img, 0, 9);
  check(ret == TDNG_LJ92_ERROR_BAD_HANDLE, "rows: past height");
  ret = tdng_lj92_encode_rows(lj, img + 64, 0, 4);
  check(ret == TDNG_LJ92_ERROR_BAD_HANDLE, "rows: different base pointer");
  ret = tdng_lj92_encode_rows(lj, img, 0, 4);
  check(ret == TDNG_LJ92_ERROR_NONE, "rows: first chunk");
  ret = tdng_lj92_encode_rows(lj, img, 3, 1);
  check(ret == TDNG_LJ92_ERROR_BAD_HANDLE, "rows: out of order");
  ret = tdng_lj92_encode_rows(lj, img, 4, 4);
  check(ret == TDNG_LJ92_ERROR_NONE, "rows: second chunk");
  ret = tdng_lj92_encode_finish(lj);
  check(ret == TDNG_LJ92_ERROR_NONE && out.len > 0u, "finish ok");

  /* short sink => IO */
  f.limit = 20;
  f.written = 0;
  ret = tdng_lj92_encode_open(&lj, 64, 64, 16, 3, 1, 64 * 3, 0, &f, fbuf_write);
  check(ret == TDNG_LJ92_ERROR_NONE, "open ok (failing sink)");
  if (ret == TDNG_LJ92_ERROR_NONE) {
    ret = tdng_lj92_encode_scan(lj, img, NULL, 0);
    if (ret == TDNG_LJ92_ERROR_NONE) ret = tdng_lj92_encode_begin(lj);
    if (ret == TDNG_LJ92_ERROR_NONE) {
      ret = tdng_lj92_encode_rows(lj, img, 0, 64);
    }
    if (ret == TDNG_LJ92_ERROR_NONE) {
      ret = tdng_lj92_encode_finish(lj);
    } else {
      tdng_lj92_encode_finish(lj);
    }
    check(ret == TDNG_LJ92_ERROR_IO, "short sink -> IO");
  }
}

int main(void) {
  test_parity_matrix();
  test_dng_tile_layout();
  test_errors();
  printf("\n%d tests, %d passed, %d failed\n", test_count, pass_count,
         fail_count);
  return fail_count == 0 ? 0 : 1;
}
