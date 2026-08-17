/*
 * test_v2_streaming.c - streaming LJPEG decode (tdng_lj92_open_streaming)
 * vs in-memory decode (tdng_lj92_open): outputs must be byte-identical.
 *
 * Covers:
 *   - encoder-generated streams (predictors 1/2/7 x 1/3 components x
 *     8/10/12/16-bit), known-size and unknown-size streams, skipLength
 *   - real-world streams: every tile_*.lj92 in <dir> (optional argv[1])
 *   - baseline JPEG (SOF0) -> TDNG_LJ92_ERROR_NOT_LOSSLESS
 *   - garbage / truncated streams -> TDNG_LJ92_ERROR_CORRUPT
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

/* ---- in-memory stream backend over a byte array --------------------- */

typedef struct memio {
  const uint8_t* data;
  size_t size;
} memio;

static size_t memio_read(void* user, uint64_t off, void* dst, size_t len) {
  memio* m = (memio*)user;
  if (off > m->size || (uint64_t)len > (m->size - off)) return 0;
  memcpy(dst, m->data + (size_t)off, len);
  return len;
}

static uint64_t memio_size(void* user) {
  return (uint64_t)((memio*)user)->size;
}

typedef struct counted_memio {
  memio base;
  size_t read_calls;
} counted_memio;

static size_t counted_memio_read(void* user, uint64_t off, void* dst,
                                 size_t len) {
  counted_memio* m = (counted_memio*)user;
  m->read_calls++;
  return memio_read(&m->base, off, dst, len);
}

static uint64_t counted_memio_size(void* user) {
  return (uint64_t)((counted_memio*)user)->base.size;
}

/* ---- decode a stream twice (memory vs streaming) and compare --------- */

static int decode_memory(const uint8_t* data, size_t n, int* w, int* h, int* b,
                         int* c, uint16_t* out, int skip_length) {
  tdng_lj92 lj = NULL;
  int ret = tdng_lj92_open(&lj, data, (int)n, w, h, b, c);
  if (ret != TDNG_LJ92_ERROR_NONE || !lj) return ret;
  ret = tdng_lj92_decode(lj, out, *w * *c, skip_length, NULL, 0);
  tdng_lj92_close(lj);
  if (ret != TDNG_LJ92_ERROR_NONE) return ret;
  return TDNG_LJ92_ERROR_NONE;
}

static int decode_streaming(const uint8_t* data, size_t n, int know_size,
                            int* w, int* h, int* b, int* c, uint16_t* out,
                            int skip_length) {
  memio m;
  m.data = data;
  m.size = n;
  tdng_lj92 lj = NULL;
  int ret = tdng_lj92_open_streaming(&lj, &m, memio_read,
                                     know_size ? memio_size : NULL, w, h, b, c);
  if (ret != TDNG_LJ92_ERROR_NONE || !lj) return ret;
  ret = tdng_lj92_decode(lj, out, *w * *c, skip_length, NULL, 0);
  tdng_lj92_close(lj);
  if (ret != TDNG_LJ92_ERROR_NONE) return ret;
  return TDNG_LJ92_ERROR_NONE;
}

/* Compare memory vs streaming decode of one stream, both with and without
   a skip-length, and with known/unknown stream size. Returns 1 on pass. */
static int compare_paths(const uint8_t* data, size_t n, const char* name) {
  int w = 0, h = 0, b = 0, c = 0;
  int ws = 0, hs = 0, bs = 0, cs = 0;
  int skip_values[] = {0, 7};
  int i;
  int ok = 1;
  size_t px, out_samples;
  uint16_t *ref, *got;

  {
    tdng_lj92 lj = NULL;
    int r = tdng_lj92_open(&lj, data, (int)n, &w, &h, &b, &c);
    if (r != TDNG_LJ92_ERROR_NONE || !lj || w <= 0 || h <= 0 || c <= 0) {
      printf("  [FAIL] %s: memory open failed ret=%d\n", name, r);
      if (lj) tdng_lj92_close(lj);
      return 0;
    }
    tdng_lj92_close(lj);
  }
  px = (size_t)w * (size_t)h * (size_t)c;
  out_samples = px + (size_t)7 * (size_t)h; /* room for the skip=7 case */
  ref = (uint16_t*)malloc(out_samples * sizeof(uint16_t));
  got = (uint16_t*)malloc(out_samples * sizeof(uint16_t));
  if (!ref || !got) {
    free(ref);
    free(got);
    printf("  [FAIL] %s: OOM\n", name);
    return 0;
  }

  for (i = 0; i < 2; i++) {
    int skip = skip_values[i];
    int r;
    memset(ref, 0xAA, out_samples * sizeof(uint16_t));
    memset(got, 0xAA, out_samples * sizeof(uint16_t));
    r = decode_memory(data, n, &w, &h, &b, &c, ref, skip);
    if (r != TDNG_LJ92_ERROR_NONE) {
      printf("  [FAIL] %s: memory decode ret=%d (skip=%d)\n", name, r, skip);
      ok = 0;
      break;
    }
    r = decode_streaming(data, n, 1, &ws, &hs, &bs, &cs, got, skip);
    if (r != TDNG_LJ92_ERROR_NONE) {
      printf("  [FAIL] %s: streaming decode ret=%d (skip=%d)\n", name, r, skip);
      ok = 0;
      break;
    }
    if (ws != w || hs != h || bs != b || cs != c) {
      printf("  [FAIL] %s: streaming dims %dx%d b=%d c=%d != memory %dx%d b=%d c=%d\n",
             name, ws, hs, bs, cs, w, h, b, c);
      ok = 0;
      break;
    }
    {
      size_t row_samples = (size_t)w * (size_t)c + (size_t)skip;
      size_t compare_bytes = row_samples * (size_t)h * sizeof(uint16_t);
      if (memcmp(ref, got, compare_bytes) != 0) {
        printf("  [FAIL] %s: streaming output differs from memory (skip=%d)\n",
               name, skip);
        ok = 0;
        break;
      }
    }
    /* unknown stream size (size_fn == NULL) */
    r = decode_streaming(data, n, 0, &ws, &hs, &bs, &cs, got, skip);
    if (r != TDNG_LJ92_ERROR_NONE || ws != w || hs != h) {
      printf("  [FAIL] %s: unknown-size streaming decode differs (skip=%d)\n",
             name, skip);
      ok = 0;
      break;
    }
    {
      size_t row_samples = (size_t)w * (size_t)c + (size_t)skip;
      size_t compare_bytes = row_samples * (size_t)h * sizeof(uint16_t);
      if (memcmp(ref, got, compare_bytes) != 0) {
        printf("  [FAIL] %s: unknown-size streaming output differs (skip=%d)\n",
               name, skip);
        ok = 0;
        break;
      }
    }
  }
  free(ref);
  free(got);
  return ok;
}

/* ---- encoder-generated streams --------------------------------------- */

static void test_encode_generated(void) {
  static const int predictors[] = {1, 2, 7};
  static const int comps_list[] = {1, 3};
  static const int bps_list[] = {8, 10, 12, 16};
  int p, ci, bi;
  printf("Testing encoder-generated streams...\n");

  for (p = 0; p < 3; p++) {
    for (ci = 0; ci < 2; ci++) {
      for (bi = 0; bi < 4; bi++) {
        int W = 61, H = 37; /* odd dims; predictors 1/2/7, all bitdepths */
        int comps = comps_list[ci];
        int bps = bps_list[bi];
        int pred = predictors[p];
        size_t npx = (size_t)W * H * (size_t)comps;
        uint16_t* img = (uint16_t*)malloc(npx * sizeof(uint16_t));
        uint16_t* dec = (uint16_t*)malloc(npx * sizeof(uint16_t));
        uint8_t* enc = NULL;
        int enc_len = 0;
        char name[128];
        int i;
        int ret;

        snprintf(name, sizeof(name), "enc p=%d c=%d b=%d", pred, comps, bps);
        for (i = 0; i < (int)npx; i++) {
          int v = (i * 2654435761u) & ((1u << bps) - 1u);
          img[i] = (uint16_t)v;
        }
        ret = tdng_lj92_encode_ex(img, W, H, bps, comps, pred, W * comps, 0,
                                  NULL, 0, &enc, &enc_len);
        if (ret != TDNG_LJ92_ERROR_NONE || !enc) {
          printf("  [FAIL] %s: encode failed ret=%d\n", name, ret);
          free(enc);
          free(dec);
          free(img);
          fail_count++;
          test_count++;
          continue;
        }
        /* decode back with both paths and compare to the source image */
        {
          tdng_lj92 lj = NULL;
          int w, h, b, c;
          ret = tdng_lj92_open(&lj, enc, enc_len, &w, &h, &b, &c);
          if (ret == TDNG_LJ92_ERROR_NONE && lj) {
            ret = tdng_lj92_decode(lj, dec, w * c, 0, NULL, 0);
            tdng_lj92_close(lj);
          }
          if (ret != TDNG_LJ92_ERROR_NONE || w != W || h != H ||
              memcmp(img, dec, npx * sizeof(uint16_t)) != 0) {
            printf("  [FAIL] %s: memory round-trip mismatch (ret=%d)\n", name,
                   ret);
            check(0, name);
          } else {
            int ok = compare_paths(enc, (size_t)enc_len, name);
            check(ok, name);
          }
        }
        free(enc);
        free(dec);
        free(img);
      }
    }
  }
}

/* A known-size stream should be pulled in cache-sized windows rather than
 * issuing one callback for each marker/entropy byte. */
static void test_known_size_prefetch(void) {
  uint16_t image[32 * 32];
  uint16_t decoded[32 * 32];
  uint8_t* encoded = NULL;
  int encoded_len = 0;
  int width = 0, height = 0, bitdepth = 0, components = 0;
  tdng_lj92 lj = NULL;
  counted_memio m;
  int i;
  int ret;

  for (i = 0; i < (int)(sizeof(image) / sizeof(image[0])); i++) {
    image[i] = (uint16_t)((i * 73u) & 0xFFu);
  }
  ret = tdng_lj92_encode_ex(image, 32, 32, 8, 1, 1, 32, 0, NULL, 0,
                            &encoded, &encoded_len);
  if (ret == TDNG_LJ92_ERROR_NONE && encoded) {
    memset(&m, 0, sizeof(m));
    m.base.data = encoded;
    m.base.size = (size_t)encoded_len;
    ret = tdng_lj92_open_streaming(&lj, &m, counted_memio_read,
                                   counted_memio_size, &width, &height,
                                   &bitdepth, &components);
    if (ret == TDNG_LJ92_ERROR_NONE) {
      ret = tdng_lj92_decode(lj, decoded, width * components, 0, NULL, 0);
    }
    check(ret == TDNG_LJ92_ERROR_NONE && m.read_calls <= 8u,
          "known-size streaming uses chunk prefetch");
    tdng_lj92_close(lj);
  } else {
    check(0, "known-size streaming prefetch fixture encode");
  }
  free(encoded);
}

/* ---- real-world streams from a tile directory ------------------------ */

static int test_tile_dir(const char* dir) {
  char path[512];
  FILE* f;
  int count = 0;
  printf("Testing streams in %s\n", dir);
  for (int k = 0; k < 256; k++) {
    snprintf(path, sizeof(path), "%s/tile_%03d.lj92", dir, k);
    f = fopen(path, "rb");
    if (!f) break;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0 && n < (1 << 28)) {
      uint8_t* data = (uint8_t*)malloc((size_t)n);
      if (data && fread(data, 1, (size_t)n, f) == (size_t)n) {
        int ok = compare_paths(data, (size_t)n, path);
        check(ok, path);
        count++;
      }
      free(data);
    }
    fclose(f);
  }
  return count;
}

/* ---- error paths ----------------------------------------------------- */

static void test_errors(void) {
  uint8_t baseline[40]; /* SOI + SOF0 + SOS headers, no real scan needed */
  int w = 0, h = 0, b = 0, c = 0;
  memio m;
  tdng_lj92 lj = NULL;
  int ret;

  printf("Testing error paths...\n");

  /* Baseline JPEG (SOF0): open must report NOT_LOSSLESS. */
  baseline[0] = 0xFF; baseline[1] = 0xD8;                  /* SOI  */
  baseline[2] = 0xFF; baseline[3] = 0xC0;                  /* SOF0 */
  baseline[4] = 0x00; baseline[5] = 0x0B;                  /* Lf=11 = 8+3*Nf */
  baseline[6] = 8;                                          /* P   */
  baseline[7] = 0x00; baseline[8] = 0x10;                  /* Y=16 */
  baseline[9] = 0x00; baseline[10] = 0x10;                 /* X=16 */
  baseline[11] = 1;                                         /* Nf  */
  baseline[12] = 0x01; baseline[13] = 0x11; baseline[14] = 0x00; /* C1: H/V=1,Tq=0 */
  baseline[15] = 0xFF; baseline[16] = 0xDA;                /* SOS  */
  baseline[17] = 0x00; baseline[18] = 0x08;                /* Ls=8 */
  baseline[19] = 1;                                         /* Ns  */
  baseline[20] = 0x01; baseline[21] = 0x00;                /* C1: Td/Ta=0 */
  baseline[22] = 0x00; baseline[23] = 0x3F; baseline[24] = 0x00; /* Ss Se AhAl */
  m.data = baseline;
  m.size = 25;
  ret = tdng_lj92_open_streaming(&lj, &m, memio_read, memio_size, &w, &h, &b, &c);
  check(ret == TDNG_LJ92_ERROR_NOT_LOSSLESS,
        "baseline JPEG stream -> NOT_LOSSLESS");
  if (lj) tdng_lj92_close(lj);
  lj = NULL;

  /* Garbage: must be CORRUPT. */
  m.data = (const uint8_t*)"this is not a jpeg stream";
  m.size = 26;
  ret = tdng_lj92_open_streaming(&lj, &m, memio_read, memio_size, &w, &h, &b, &c);
  check(ret == TDNG_LJ92_ERROR_CORRUPT, "garbage stream -> CORRUPT");
  if (lj) tdng_lj92_close(lj);
  lj = NULL;

  /* NULL read_fn / handle. */
  ret = tdng_lj92_open_streaming(NULL, &m, memio_read, memio_size, &w, &h, &b,
                                 &c);
  check(ret == TDNG_LJ92_ERROR_BAD_HANDLE, "NULL handle -> BAD_HANDLE");
  ret = tdng_lj92_open_streaming(&lj, &m, NULL, memio_size, &w, &h, &b, &c);
  check(ret == TDNG_LJ92_ERROR_BAD_HANDLE, "NULL read_fn -> BAD_HANDLE");
}

static void test_truncated_stream(const uint8_t* data, size_t n) {
  size_t cut;
  char name[64];
  for (cut = 1; cut < n && cut < 512; cut *= 2) {
    memio m;
    tdng_lj92 lj = NULL;
    int w, h, b, c;
    int ret;
    int failed_ok = 0;
    snprintf(name, sizeof(name), "truncated@%zu", cut);
    m.data = data;
    m.size = cut;
    ret = tdng_lj92_open_streaming(&lj, &m, memio_read, memio_size, &w, &h, &b,
                                   &c);
    if (ret != TDNG_LJ92_ERROR_NONE) {
      failed_ok = 1; /* open refused */
    } else if (lj) {
      size_t px = (size_t)w * (size_t)h * (size_t)c;
      uint16_t* out = (uint16_t*)malloc(px * sizeof(uint16_t));
      ret = tdng_lj92_decode(lj, out, w * c, 0, NULL, 0);
      failed_ok = (ret == TDNG_LJ92_ERROR_CORRUPT);
      free(out);
    }
    tdng_lj92_close(lj);
    check(failed_ok, name);
  }
}

int main(int argc, char** argv) {
  const char* dir = (argc > 1) ? argv[1] : NULL;
  int tile_count = 0;

  test_encode_generated();
  test_known_size_prefetch();
  test_errors();

  if (dir) {
    tile_count = test_tile_dir(dir);
    if (tile_count == 0) {
      printf("  (no tile_*.lj92 found in %s)\n", dir);
    }
  }

  /* Real-world stream truncation probes: reuse one tile if available. */
  if (dir) {
    char path[512];
    snprintf(path, sizeof(path), "%s/tile_000.lj92", dir);
    FILE* f = fopen(path, "rb");
    if (f) {
      fseek(f, 0, SEEK_END);
      long n = ftell(f);
      fseek(f, 0, SEEK_SET);
      if (n > 0 && n < (1 << 28)) {
        uint8_t* data = (uint8_t*)malloc((size_t)n);
        if (data && fread(data, 1, (size_t)n, f) == (size_t)n) {
          test_truncated_stream(data, (size_t)n);
        }
        free(data);
      }
      fclose(f);
    }
  }

  printf("\n%d tests, %d passed, %d failed\n", test_count, pass_count,
         fail_count);
  return fail_count == 0 ? 0 : 1;
}
