/* Shared helpers for the tinydng v3 test suite. */
#ifndef TD_TEST_UTIL_H
#define TD_TEST_UTIL_H

#include "tinydng.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;

#define CHECK(cond, ...)                            \
  do {                                              \
    if (!(cond)) {                                  \
      fprintf(stderr, "  FAIL: ");                  \
      fprintf(stderr, __VA_ARGS__);                 \
      fprintf(stderr, " (%s:%d)\n", __FILE__, __LINE__); \
      g_fail = 1;                                   \
    }                                               \
  } while (0)

static void tdt_pu16(uint8_t *p, uint16_t v, int be) {
  if (be) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
  } else {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
  }
}

static void tdt_pu32(uint8_t *p, uint32_t v, int be) {
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

static void tdt_pu64(uint8_t *p, uint64_t v, int be) {
  int i;
  for (i = 0; i < 8; i++) {
    int sh = be ? (56 - 8 * i) : (8 * i);
    p[i] = (uint8_t)(v >> sh);
  }
}

static void tdt_pf64be(uint8_t *p, double d) {
  uint64_t u;
  memcpy(&u, &d, 8);
  tdt_pu64(p, u, 1);
}

static void tdt_pf32be(uint8_t *p, float f) {
  uint32_t u;
  memcpy(&u, &f, 4);
  tdt_pu32(p, u, 1);
}

static unsigned char *tdt_slurp(const char *path, size_t *n) {
  FILE *f = fopen(path, "rb");
  long s;
  unsigned char *b;
  if (!f) {
    return NULL;
  }
  fseek(f, 0, SEEK_END);
  s = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (s < 0) {
    fclose(f);
    return NULL;
  }
  b = (unsigned char *)malloc((size_t)s + 1u);
  if (b && fread(b, 1, (size_t)s, f) != (size_t)s) {
    free(b);
    b = NULL;
  }
  fclose(f);
  if (b) {
    *n = (size_t)s;
  }
  return b;
}

#endif /* TD_TEST_UTIL_H */
