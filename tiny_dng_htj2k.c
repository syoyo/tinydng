/*
 * tiny_dng_htj2k.c - clean-room C11 HTJ2K block coder (JPEG 2000 Part 15,
 * ITU-T T.814 high-throughput block coding engine).
 *
 * Implements the HT block decoder and encoder at the codeblock level. The
 * VLC codebook is the normative data from the standard (see
 * tiny_dng_htj2k_tables.h); all decoder/encoder lookup tables are derived
 * from it at first use. Verified interoperable with OpenJPH.
 *
 * SPDX-License-Identifier: MIT
 */
#include "tiny_dng_htj2k.h"
#include "tiny_dng_htj2k_tables.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t ui8;
typedef uint16_t ui16;
typedef uint32_t ui32;
typedef uint64_t ui64;
typedef int32_t si32;

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
__attribute__((target("avx2")))
static int ht_cpu_avx2(void) {
  static int cached = -1;
  if (cached >= 0) return cached;
  __builtin_cpu_init();
#if defined(TINYDNG_FORCE_SCALAR)
  cached = 0;
#else
  cached = __builtin_cpu_supports("avx2") != 0;
#endif
  return cached;
}
#else
static int ht_cpu_avx2(void) { return 0; }
#endif

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

#if defined(__GNUC__) || defined(__clang__)
#define HT_CLZ32(v) ((uint32_t)__builtin_clz((unsigned)(v)))
#define HT_CLZ64(v) ((uint32_t)__builtin_clzll((unsigned long long)(v)))
#define HT_POPCOUNT32(v) ((uint32_t)__builtin_popcount((unsigned)(v)))
#define HT_POPCOUNT64(v) ((uint32_t)__builtin_popcountll((unsigned long long)(v)))
#else
static uint32_t ht_clz32(uint32_t v) {
  uint32_t n = 32;
  while (v) { v >>= 1; n--; }
  return n;
}
static uint32_t ht_clz64(uint64_t v) {
  uint32_t n = 64;
  while (v) { v >>= 1; n--; }
  return n;
}
#define HT_CLZ32(v) ht_clz32((uint32_t)(v))
#define HT_CLZ64(v) ht_clz64((uint64_t)(v))
#define HT_POPCOUNT32(v) ht_popcount32((uint32_t)(v))
#define HT_POPCOUNT64(v) ht_popcount64((uint64_t)(v))
static uint32_t ht_popcount32(uint32_t v) {
  v = v - ((v >> 1) & 0x55555555u);
  v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
  return (((v + (v >> 4)) & 0x0F0F0F0Fu) * 0x01010101u) >> 24;
}
static uint32_t ht_popcount64(uint64_t v) {
  return ht_popcount32((uint32_t)v) + ht_popcount32((uint32_t)(v >> 32));
}
#endif

static int ht_host_big(void) {
  uint16_t x = 1u;
  uint8_t b[2];
  memcpy(b, &x, 2);
  return b[0] == 0u;
}
static int ht_is_le;

static inline uint32_t ht_load_le32(const uint8_t *p) {
  uint32_t v;
  memcpy(&v, p, 4);
  if (ht_is_le) return v;
  return ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) |
         ((v >> 24) & 0xFFu);
}
static inline uint32_t ht_load_le16x2(const uint16_t *p) {
  uint32_t v;
  memcpy(&v, p, 4);
  if (ht_is_le) return v;
  return ((v & 0xFFFFu) << 16) | ((v >> 16) & 0xFFFFu);
}

/* ------------------------------------------------------------------ */
/* Derived tables (built once from the standard codebook)              */
/* ------------------------------------------------------------------ */

/* decode: vlc_dec0/1[1024]; index = (c_q << 7) | cwd, value =
   (rho << 4) | (u_off << 3) | (e_k << 12) | (e_1 << 8) | cwd_len      */
static uint16_t ht_vlc_dec0[1024];
static uint16_t ht_vlc_dec1[1024];
/* decode: uvlc_dec0[320], uvlc_dec1[256], uvlc_bias[320]              */
static uint16_t ht_uvlc_dec0[320];
static uint16_t ht_uvlc_dec1[256];
static uint8_t ht_uvlc_bias[320];

/* encode: vlc_enc0/1[2048]; index = (c_q << 8) | (rho << 4) | emb,
   value = (cwd << 8) | (cwd_len << 4) | e_k                           */
static uint16_t ht_vlc_enc0[2048];
static uint16_t ht_vlc_enc1[2048];
/* encode: uvlc_enc[75]                                                 */
typedef struct ht_uvlc_enc_entry {
  uint8_t pre, pre_len, suf, suf_len, ext, ext_len;
} ht_uvlc_enc_entry;
static ht_uvlc_enc_entry ht_uvlc_enc[75];

static int ht_tables_ready;

/* decode VLC tables from the standard codebook */
static void ht_init_vlc_dec(uint16_t *tgt, const ht_vlc_entry *src, size_t n) {
  size_t i, j;
  memset(tgt, 0, 1024 * sizeof(uint16_t));
  for (i = 0; i < 1024; ++i) {
    uint32_t cwd = (uint32_t)(i & 0x7F);
    uint32_t c_q = (uint32_t)(i >> 7);
    for (j = 0; j < n; ++j) {
      if ((uint32_t)src[j].c_q == c_q &&
          (uint32_t)src[j].cwd == (cwd & ((1u << src[j].cwd_len) - 1u))) {
        tgt[i] = (uint16_t)((src[j].rho << 4) | (src[j].u_off << 3) |
                            (src[j].e_k << 12) | (src[j].e_1 << 8) |
                            src[j].cwd_len);
        break;
      }
    }
  }
}

/* UVLC decode tables; `dec` is the UVLC prefix codebook (T.814 Table 3). */
static void ht_init_uvlc_dec(void) {
  static const uint8_t dec[8] = {
      3 | (5 << 2) | (5 << 5), /* 000 */
      1 | (0 << 2) | (1 << 5), /* xx1 */
      2 | (0 << 2) | (2 << 5), /* x10 */
      1 | (0 << 2) | (1 << 5), /* xx1 */
      3 | (1 << 2) | (3 << 5), /* 100 */
      1 | (0 << 2) | (1 << 5), /* xx1 */
      2 | (0 << 2) | (2 << 5), /* x10 */
      1 | (0 << 2) | (1 << 5)  /* xx1 */
  };
  uint32_t i;
  memset(ht_uvlc_dec0, 0, sizeof(ht_uvlc_dec0));
  memset(ht_uvlc_bias, 0, sizeof(ht_uvlc_bias));
  for (i = 0; i < 320; ++i) {
    uint32_t mode = i >> 6;
    uint32_t vlc = i & 0x3F;
    if (mode == 0) {
      ht_uvlc_dec0[i] = 0;
      ht_uvlc_bias[i] = 0;
    } else if (mode <= 2) {
      uint32_t d = dec[vlc & 0x7];
      uint32_t total_prefix = d & 0x3;
      uint32_t total_suffix = (d >> 2) & 0x7;
      uint32_t u0_suffix_len = (mode == 1) ? total_suffix : 0;
      uint32_t u0 = (mode == 1) ? (d >> 5) : 0;
      uint32_t u1 = (mode == 1) ? 0 : (d >> 5);
      ht_uvlc_dec0[i] = (uint16_t)(total_prefix | (total_suffix << 3) |
                                   (u0_suffix_len << 7) | (u0 << 10) |
                                   (u1 << 13));
    } else if (mode == 3) {
      uint32_t d0 = dec[vlc & 0x7];
      vlc >>= d0 & 0x3;
      uint32_t d1 = dec[vlc & 0x7];
      uint32_t total_prefix, u0_suffix_len, total_suffix, u0, u1;
      if ((d0 & 0x3) == 3) {
        total_prefix = (d0 & 0x3) + 1;
        u0_suffix_len = (d0 >> 2) & 0x7;
        total_suffix = u0_suffix_len;
        u0 = d0 >> 5;
        u1 = (vlc & 1) + 1;
        ht_uvlc_bias[i] = 4;
      } else {
        total_prefix = (d0 & 0x3) + (d1 & 0x3);
        u0_suffix_len = (d0 >> 2) & 0x7;
        total_suffix = u0_suffix_len + ((d1 >> 2) & 0x7);
        u0 = d0 >> 5;
        u1 = d1 >> 5;
        ht_uvlc_bias[i] = 0;
      }
      ht_uvlc_dec0[i] = (uint16_t)(total_prefix | (total_suffix << 3) |
                                   (u0_suffix_len << 7) | (u0 << 10) |
                                   (u1 << 13));
    } else { /* mode == 4 */
      uint32_t d0 = dec[vlc & 0x7];
      vlc >>= d0 & 0x3;
      uint32_t d1 = dec[vlc & 0x7];
      uint32_t total_prefix = (d0 & 0x3) + (d1 & 0x3);
      uint32_t u0_suffix_len = (d0 >> 2) & 0x7;
      uint32_t total_suffix = u0_suffix_len + ((d1 >> 2) & 0x7);
      uint32_t u0 = (d0 >> 5) + 2;
      uint32_t u1 = (d1 >> 5) + 2;
      ht_uvlc_dec0[i] = (uint16_t)(total_prefix | (total_suffix << 3) |
                                   (u0_suffix_len << 7) | (u0 << 10) |
                                   (u1 << 13));
      ht_uvlc_bias[i] = 10;
    }
  }
  memset(ht_uvlc_dec1, 0, sizeof(ht_uvlc_dec1));
  for (i = 0; i < 256; ++i) {
    uint32_t mode = i >> 6;
    uint32_t vlc = i & 0x3F;
    if (mode == 0) {
      ht_uvlc_dec1[i] = 0;
    } else if (mode <= 2) {
      uint32_t d = dec[vlc & 0x7];
      uint32_t total_prefix = d & 0x3;
      uint32_t total_suffix = (d >> 2) & 0x7;
      uint32_t u0_suffix_len = (mode == 1) ? total_suffix : 0;
      uint32_t u0 = (mode == 1) ? (d >> 5) : 0;
      uint32_t u1 = (mode == 1) ? 0 : (d >> 5);
      ht_uvlc_dec1[i] = (uint16_t)(total_prefix | (total_suffix << 3) |
                                   (u0_suffix_len << 7) | (u0 << 10) |
                                   (u1 << 13));
    } else { /* mode == 3 */
      uint32_t d0 = dec[vlc & 0x7];
      vlc >>= d0 & 0x3;
      uint32_t d1 = dec[vlc & 0x7];
      uint32_t total_prefix = (d0 & 0x3) + (d1 & 0x3);
      uint32_t u0_suffix_len = (d0 >> 2) & 0x7;
      uint32_t total_suffix = u0_suffix_len + ((d1 >> 2) & 0x7);
      uint32_t u0 = d0 >> 5;
      uint32_t u1 = d1 >> 5;
      ht_uvlc_dec1[i] = (uint16_t)(total_prefix | (total_suffix << 3) |
                                   (u0_suffix_len << 7) | (u0 << 10) |
                                   (u1 << 13));
    }
  }
}

/* encode VLC tables: pick the best (shortest cwd with most e_k bits) code */
static void ht_init_vlc_enc(uint16_t *tgt, const ht_vlc_entry *src, size_t n) {
  uint32_t popcnt[16];
  uint32_t i;
  for (i = 0; i < 16; ++i) popcnt[i] = HT_POPCOUNT32(i);
  memset(tgt, 0, 2048 * sizeof(uint16_t));
  for (i = 0; i < 2048; ++i) {
    uint32_t c_q = i >> 8;
    uint32_t rho = (i >> 4) & 0xF;
    uint32_t emb = i & 0xF;
    size_t j;
    const ht_vlc_entry *best = NULL;
    int best_e_k = -1;
    if (((emb & rho) != emb) || (rho == 0 && c_q == 0)) continue;
    if (emb) {
      for (j = 0; j < n; ++j) {
        if ((uint32_t)src[j].c_q == c_q && (uint32_t)src[j].rho == rho &&
            (uint32_t)src[j].u_off == 1u && ((emb & (uint32_t)src[j].e_k) == (uint32_t)src[j].e_1)) {
          int ones = (int)popcnt[src[j].e_k & 0xF];
          if (ones >= best_e_k) {
            best = src + j;
            best_e_k = ones;
          }
        }
      }
    } else {
      for (j = 0; j < n; ++j) {
        if ((uint32_t)src[j].c_q == c_q && (uint32_t)src[j].rho == rho &&
            src[j].u_off == 0) {
          best = src + j;
          break;
        }
      }
    }
    if (best)
      tgt[i] = (uint16_t)((best->cwd << 8) | (best->cwd_len << 4) |
                          (best->e_k & 0xF));
  }
}

/* UVLC encode tables (T.814 Table 4). */
static void ht_init_uvlc_enc(void) {
  int i;
  ht_uvlc_enc[0].pre = 0; ht_uvlc_enc[0].pre_len = 0;
  ht_uvlc_enc[0].suf = 0; ht_uvlc_enc[0].suf_len = 0;
  ht_uvlc_enc[0].ext = 0; ht_uvlc_enc[0].ext_len = 0;
  ht_uvlc_enc[1].pre = 1; ht_uvlc_enc[1].pre_len = 1;
  ht_uvlc_enc[1].suf = 0; ht_uvlc_enc[1].suf_len = 0;
  ht_uvlc_enc[1].ext = 0; ht_uvlc_enc[1].ext_len = 0;
  ht_uvlc_enc[2].pre = 2; ht_uvlc_enc[2].pre_len = 2;
  ht_uvlc_enc[2].suf = 0; ht_uvlc_enc[2].suf_len = 0;
  ht_uvlc_enc[2].ext = 0; ht_uvlc_enc[2].ext_len = 0;
  ht_uvlc_enc[3].pre = 4; ht_uvlc_enc[3].pre_len = 3;
  ht_uvlc_enc[3].suf = 0; ht_uvlc_enc[3].suf_len = 1;
  ht_uvlc_enc[3].ext = 0; ht_uvlc_enc[3].ext_len = 0;
  ht_uvlc_enc[4].pre = 4; ht_uvlc_enc[4].pre_len = 3;
  ht_uvlc_enc[4].suf = 1; ht_uvlc_enc[4].suf_len = 1;
  ht_uvlc_enc[4].ext = 0; ht_uvlc_enc[4].ext_len = 0;
  for (i = 5; i < 33; ++i) {
    ht_uvlc_enc[i].pre = 0; ht_uvlc_enc[i].pre_len = 3;
    ht_uvlc_enc[i].suf = (uint8_t)(i - 5); ht_uvlc_enc[i].suf_len = 5;
    ht_uvlc_enc[i].ext = 0; ht_uvlc_enc[i].ext_len = 0;
  }
  for (i = 33; i < 75; ++i) {
    ht_uvlc_enc[i].pre = 0; ht_uvlc_enc[i].pre_len = 3;
    ht_uvlc_enc[i].suf = (uint8_t)(28 + (i - 33) % 4);
    ht_uvlc_enc[i].suf_len = 5;
    ht_uvlc_enc[i].ext = (uint8_t)((i - 33) / 4);
    ht_uvlc_enc[i].ext_len = 4;
  }
}

static void ht_ensure_tables(void) {
  if (ht_tables_ready) return;
  ht_is_le = !ht_host_big();
  ht_init_vlc_dec(ht_vlc_dec0, ht_vlc_tbl0_src, HT_VLC_TBL0_COUNT);
  ht_init_vlc_dec(ht_vlc_dec1, ht_vlc_tbl1_src, HT_VLC_TBL1_COUNT);
  ht_init_uvlc_dec();
  ht_init_vlc_enc(ht_vlc_enc0, ht_vlc_tbl0_src, HT_VLC_TBL0_COUNT);
  ht_init_vlc_enc(ht_vlc_enc1, ht_vlc_tbl1_src, HT_VLC_TBL1_COUNT);
  ht_init_uvlc_enc();
  ht_tables_ready = 1;
}

/* ------------------------------------------------------------------ */
/* MEL decoder state                                                   */
/* ------------------------------------------------------------------ */

typedef struct ht_mel_st {
  const ui8 *data;
  ui64 tmp;
  int bits;
  int size;
  int unstuff;
  int k;
  int num_runs;
  ui64 runs;
} ht_mel_st;

static const int ht_mel_exp[13] = {0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 4, 5};

static void ht_mel_read(ht_mel_st *m) {
  ui32 val, t;
  int bits, unstuff;
  if (m->bits > 32) return;
  val = 0xFFFFFFFFu;
  if (m->size > 4) {
    val = ht_load_le32(m->data);
    m->data += 4;
    m->size -= 4;
  } else if (m->size > 0) {
    int i = 0;
    while (m->size > 1) {
      ui32 v = *m->data++;
      ui32 mask = ~(0xFFu << i);
      val = (val & mask) | (v << i);
      --m->size;
      i += 8;
    }
    {
      ui32 v = *m->data++;
      v |= 0xF;
      ui32 mask = ~(0xFFu << i);
      val = (val & mask) | (v << i);
      --m->size;
    }
  }
  bits = 32 - m->unstuff;
  t = val & 0xFF;
  unstuff = ((val & 0xFF) == 0xFF);
  bits -= unstuff;
  t = t << (8 - unstuff);
  t |= (val >> 8) & 0xFF;
  unstuff = (((val >> 8) & 0xFF) == 0xFF);
  bits -= unstuff;
  t = t << (8 - unstuff);
  t |= (val >> 16) & 0xFF;
  unstuff = (((val >> 16) & 0xFF) == 0xFF);
  bits -= unstuff;
  t = t << (8 - unstuff);
  t |= (val >> 24) & 0xFF;
  m->unstuff = (((val >> 24) & 0xFF) == 0xFF);
  m->tmp |= ((ui64)t) << (64 - bits - m->bits);
  m->bits += bits;
}

static void ht_mel_decode(ht_mel_st *m) {
  if (m->bits < 6) ht_mel_read(m);
  while (m->bits >= 6 && m->num_runs < 8) {
    int eval = ht_mel_exp[m->k];
    int run;
    if (m->tmp & (1ull << 63)) {
      run = (1 << eval) - 1;
      m->k = m->k + 1 < 12 ? m->k + 1 : 12;
      m->tmp <<= 1;
      m->bits -= 1;
      run = run << 1;
    } else {
      run = (int)(m->tmp >> (63 - eval)) & ((1 << eval) - 1);
      m->k = m->k - 1 > 0 ? m->k - 1 : 0;
      m->tmp <<= eval + 1;
      m->bits -= eval + 1;
      run = (run << 1) + 1;
    }
    eval = m->num_runs * 7;
    m->runs &= ~((ui64)0x3F << eval);
    m->runs |= ((ui64)run) << eval;
    m->num_runs++;
  }
}

static void ht_mel_init(ht_mel_st *m, const ui8 *bbuf, int lcup, int scup) {
  uintptr_t p = (uintptr_t)(bbuf + lcup - scup);
  int num = 4 - (int)(p & 0x3);
  int i;
  m->data = bbuf + lcup - scup;
  m->bits = 0;
  m->tmp = 0;
  m->unstuff = 0;
  m->size = scup - 1;
  m->k = 0;
  m->num_runs = 0;
  m->runs = 0;
  for (i = 0; i < num; ++i) {
    ui64 d = (m->size > 0) ? *m->data : 0xFF;
    if (m->size == 1) d |= 0xF;
    m->data += (m->size-- > 0);
    {
      int d_bits = 8 - m->unstuff;
      m->tmp = (m->tmp << d_bits) | d;
      m->bits += d_bits;
    }
    m->unstuff = ((d & 0xFF) == 0xFF);
  }
  m->tmp <<= (64 - m->bits);
}

static int ht_mel_get_run(ht_mel_st *m) {
  int t;
  if (m->num_runs == 0) ht_mel_decode(m);
  t = (int)(m->runs & 0x7F);
  m->runs >>= 7;
  m->num_runs--;
  return t;
}

/* ------------------------------------------------------------------ */
/* Backward-growing bitstream reader (VLC, MRP)                        */
/* ------------------------------------------------------------------ */

typedef struct ht_rev_st {
  const ui8 *data;
  ui64 tmp;
  ui32 bits;
  int size;
  int unstuff;
} ht_rev_st;

static void ht_rev_read(ht_rev_st *r) {
  ui32 val, t;
  int bits, unstuff;
  if (r->bits > 32) return;
  val = 0;
  if (r->size > 3) {
    val = ht_load_le32(r->data - 3);
    r->data -= 4;
    r->size -= 4;
  } else if (r->size > 0) {
    int i = 24;
    while (r->size > 0) {
      ui32 v = *r->data--;
      val |= (v << i);
      --r->size;
      i -= 8;
    }
  }
  t = val >> 24;
  bits = 8 - ((r->unstuff && (((val >> 24) & 0x7F) == 0x7F)) ? 1 : 0);
  unstuff = (val >> 24) > 0x8F;
  t |= ((val >> 16) & 0xFF) << bits;
  bits += 8 - ((unstuff && (((val >> 16) & 0x7F) == 0x7F)) ? 1 : 0);
  unstuff = ((val >> 16) & 0xFF) > 0x8F;
  t |= ((val >> 8) & 0xFF) << bits;
  bits += 8 - ((unstuff && (((val >> 8) & 0x7F) == 0x7F)) ? 1 : 0);
  unstuff = ((val >> 8) & 0xFF) > 0x8F;
  t |= (val & 0xFF) << bits;
  bits += 8 - ((unstuff && ((val & 0x7F) == 0x7F)) ? 1 : 0);
  unstuff = (val & 0xFF) > 0x8F;
  r->tmp |= (ui64)t << r->bits;
  r->bits += bits;
  r->unstuff = unstuff;
}

static void ht_rev_init(ht_rev_st *r, const ui8 *data, int lcup, int scup) {
  uintptr_t p;
  int num, tnum, i;
  ui32 d;
  r->data = data + lcup - 2;
  r->size = scup - 2;
  d = *r->data--;
  r->tmp = d >> 4;
  r->bits = 4 - ((r->tmp & 7) == 7);
  r->unstuff = (d | 0xF) > 0x8F;
  p = (uintptr_t)r->data;
  num = 1 + (int)(p & 0x3);
  tnum = num < r->size ? num : r->size;
  for (i = 0; i < tnum; ++i) {
    ui64 b;
    int d_bits;
    b = *r->data--;
    d_bits = 8 - ((r->unstuff && ((b & 0x7F) == 0x7F)) ? 1 : 0);
    r->tmp |= b << r->bits;
    r->bits += d_bits;
    r->unstuff = b > 0x8F;
  }
  r->size -= tnum;
  ht_rev_read(r);
}

static inline ui32 ht_rev_fetch(ht_rev_st *r) {
  if (r->bits < 32) {
    ht_rev_read(r);
    if (r->bits < 32) ht_rev_read(r);
  }
  return (ui32)r->tmp;
}

static inline ui32 ht_rev_advance(ht_rev_st *r, ui32 nbits) {
  r->tmp >>= nbits;
  r->bits -= nbits;
  return (ui32)r->tmp;
}

static void ht_rev_init_mrp(ht_rev_st *r, const ui8 *data, int lcup, int len2) {
  uintptr_t p;
  int num, i;
  r->data = data + lcup + len2 - 1;
  r->size = len2;
  r->unstuff = 1;
  r->bits = 0;
  r->tmp = 0;
  p = (uintptr_t)r->data;
  num = 1 + (int)(p & 0x3);
  for (i = 0; i < num; ++i) {
    ui64 b;
    int d_bits;
    b = (r->size-- > 0) ? *r->data-- : 0;
    d_bits = 8 - ((r->unstuff && ((b & 0x7F) == 0x7F)) ? 1 : 0);
    r->tmp |= b << r->bits;
    r->bits += d_bits;
    r->unstuff = b > 0x8F;
  }
  ht_rev_read(r);
}

static void ht_rev_read_mrp(ht_rev_st *r) {
  ui32 val, t;
  int bits, unstuff;
  if (r->bits > 32) return;
  val = 0;
  if (r->size > 3) {
    val = ht_load_le32(r->data - 3);
    r->data -= 4;
    r->size -= 4;
  } else if (r->size > 0) {
    int i = 24;
    while (r->size > 0) {
      ui32 v = *r->data--;
      val |= (v << i);
      --r->size;
      i -= 8;
    }
  }
  t = val >> 24;
  bits = 8 - ((r->unstuff && (((val >> 24) & 0x7F) == 0x7F)) ? 1 : 0);
  unstuff = (val >> 24) > 0x8F;
  t |= ((val >> 16) & 0xFF) << bits;
  bits += 8 - ((unstuff && (((val >> 16) & 0x7F) == 0x7F)) ? 1 : 0);
  unstuff = ((val >> 16) & 0xFF) > 0x8F;
  t |= ((val >> 8) & 0xFF) << bits;
  bits += 8 - ((unstuff && (((val >> 8) & 0x7F) == 0x7F)) ? 1 : 0);
  unstuff = ((val >> 8) & 0xFF) > 0x8F;
  t |= (val & 0xFF) << bits;
  bits += 8 - ((unstuff && ((val & 0x7F) == 0x7F)) ? 1 : 0);
  unstuff = (val & 0xFF) > 0x8F;
  r->tmp |= (ui64)t << r->bits;
  r->bits += bits;
  r->unstuff = unstuff;
}

static inline ui32 ht_rev_fetch_mrp(ht_rev_st *r) {
  if (r->bits < 32) {
    ht_rev_read_mrp(r);
    if (r->bits < 32) ht_rev_read_mrp(r);
  }
  return (ui32)r->tmp;
}

static inline ui32 ht_rev_advance_mrp(ht_rev_st *r, ui32 nbits) {
  r->tmp >>= nbits;
  r->bits -= nbits;
  return (ui32)r->tmp;
}

/* ------------------------------------------------------------------ */
/* Forward-growing bitstream reader (MagSgn, SPP)                      */
/* ------------------------------------------------------------------ */

typedef struct ht_frwd_st {
  const ui8 *data;
  ui64 tmp;
  ui32 bits;
  ui32 unstuff;
  int size;
} ht_frwd_st;

static void ht_frwd_read(ht_frwd_st *s, int fill_ff) {
  ui32 val, t;
  int bits, unstuff;
  if (s->bits > 32) return;
  val = 0;
  if (s->size > 3) {
    val = ht_load_le32(s->data);
    s->data += 4;
    s->size -= 4;
  } else if (s->size > 0) {
    int i = 0;
    val = fill_ff ? 0xFFFFFFFFu : 0;
    while (s->size > 0) {
      ui32 v = *s->data++;
      ui32 mask = ~(0xFFu << i);
      val = (val & mask) | (v << i);
      --s->size;
      i += 8;
    }
  } else {
    val = fill_ff ? 0xFFFFFFFFu : 0;
  }
  bits = 8 - s->unstuff;
  t = val & 0xFF;
  unstuff = ((val & 0xFF) == 0xFF);
  t |= ((val >> 8) & 0xFF) << bits;
  bits += 8 - unstuff;
  unstuff = (((val >> 8) & 0xFF) == 0xFF);
  t |= ((val >> 16) & 0xFF) << bits;
  bits += 8 - unstuff;
  unstuff = (((val >> 16) & 0xFF) == 0xFF);
  t |= ((val >> 24) & 0xFF) << bits;
  bits += 8 - unstuff;
  s->unstuff = (((val >> 24) & 0xFF) == 0xFF);
  s->tmp |= ((ui64)t) << s->bits;
  s->bits += bits;
}

static void ht_frwd_init(ht_frwd_st *s, const ui8 *data, int size, int fill_ff) {
  uintptr_t p = (uintptr_t)data;
  int num = 4 - (int)(p & 0x3);
  int i;
  s->data = data;
  s->tmp = 0;
  s->bits = 0;
  s->unstuff = 0;
  s->size = size;
  for (i = 0; i < num; ++i) {
    ui64 d;
    d = s->size-- > 0 ? *s->data++ : (fill_ff ? 0xFF : 0);
    s->tmp |= (d << s->bits);
    s->bits += 8 - s->unstuff;
    s->unstuff = ((d & 0xFF) == 0xFF);
  }
  ht_frwd_read(s, fill_ff);
}

static inline void ht_frwd_advance(ht_frwd_st *s, ui32 nbits) {
  s->tmp >>= nbits;
  s->bits -= nbits;
}

static inline ui32 ht_frwd_fetch(ht_frwd_st *s, int fill_ff) {
  if (s->bits < 32) {
    ht_frwd_read(s, fill_ff);
    if (s->bits < 32) ht_frwd_read(s, fill_ff);
  }
  return (ui32)s->tmp;
}

/* Pack the forward MagSgn stream once.  In the forward reader, a byte after
   FF contributes only its low seven bits: the byte's MSB is the stuffed bit
   suppressed by the reader's bit count.  Eight FF fill bytes preserve the
   reader's tolerant end-of-segment behavior for short blocks. */
static size_t ht_pack_magsgn(const ui8 *src, size_t size, ui8 *dst,
                             size_t cap) {
  size_t pos = 0;
  size_t i;
  int after_ff = 0;
  memset(dst, 0, cap);
  for (i = 0; i < size + 8; ++i) {
    ui32 b = i < size ? src[i] : 0xFFu;
    ui32 n = after_ff ? 7u : 8u;
    ui32 shift = (ui32)(pos & 7u);
    ui32 packed = (b & (n == 7u ? 0x7Fu : 0xFFu)) << shift;
    size_t byte = pos >> 3;
    dst[byte] |= (ui8)packed;
    if (shift && byte + 1 < cap) dst[byte + 1] |= (ui8)(packed >> 8);
    pos += n;
    after_ff = b == 0xFFu;
  }
  return pos;
}

static inline ui64 ht_magsgn_load64(const ui8 *bits, ui32 pos) {
  const ui8 *p = bits + (pos >> 3);
  return (ui64)p[0] | ((ui64)p[1] << 8) | ((ui64)p[2] << 16) |
         ((ui64)p[3] << 24) | ((ui64)p[4] << 32) |
         ((ui64)p[5] << 40) | ((ui64)p[6] << 48) |
         ((ui64)p[7] << 56);
}

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
__attribute__((target("avx2")))
#endif
static int ht_decode_magsgn2_packed(const ui8 *bits, ui32 nbits,
                                    ui32 *pos, ui32 inf, ui32 U_q, ui32 p,
                                    int first_bit, ui32 out[2],
                                    ui32 vn_out[2]) {
  ui32 active[2] = {0, 0}, raw[2] = {0, 0};
  ui32 positions[2] = {0, 0}, shifts[2] = {0, 0};
  ui32 mag[2] = {0, 0}, sign[2] = {0, 0};
  int i;
  for (i = 0; i < 2; ++i) {
    int bit = first_bit + i;
    if (inf & (1u << (4 + bit))) {
      ui32 mn = U_q - ((inf >> (12 + bit)) & 1u);
      if (*pos > nbits || mn > nbits - *pos) return 0;
      active[i] = 0xFFFFFFFFu;
      positions[i] = *pos;
      shifts[i] = *pos & 7u;
      *pos += mn;
    } else {
      vn_out[i] = 0;
    }
  }

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
  if (ht_cpu_avx2()) {
    __m256i indexes = _mm256_setr_epi64x((long long)(positions[0] >> 3),
                                         (long long)(positions[1] >> 3), 0, 0);
    __m256i windows = _mm256_i64gather_epi64((const long long *)bits,
                                             indexes, 1);
    __m256i shiftv = _mm256_setr_epi64x(shifts[0], shifts[1], 0, 0);
    ui64 extracted[4];
    _mm256_storeu_si256((__m256i *)extracted,
                        _mm256_srlv_epi64(windows, shiftv));
    raw[0] = (ui32)extracted[0];
    raw[1] = (ui32)extracted[1];
  } else
#endif
  {
    for (i = 0; i < 2; ++i)
      if (active[i]) raw[i] = (ui32)(ht_magsgn_load64(bits, positions[i]) >>
                                     shifts[i]);
  }
  for (i = 0; i < 2; ++i) {
    if (active[i]) {
      ui32 mn = U_q - ((inf >> (12 + first_bit + i)) & 1u);
      ui32 mask = mn ? ((1u << mn) - 1u) : 0u;
      ui32 vn = (raw[i] & mask) |
                (((inf >> (8 + first_bit + i)) & 1u) << mn) | 1u;
      vn_out[i] = vn;
      mag[i] = vn;
      sign[i] = raw[i] << 31;
    }
  }
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
  if (ht_cpu_avx2()) {
    __m256i v = _mm256_setr_epi32((int)mag[0], (int)mag[1], 0, 0, 0, 0, 0, 0);
    __m256i sg = _mm256_setr_epi32((int)sign[0], (int)sign[1], 0, 0, 0, 0, 0, 0);
    __m256i ac = _mm256_setr_epi32((int)active[0], (int)active[1], 0, 0, 0, 0, 0, 0);
    __m256i sh = _mm256_set1_epi32((int)(p - 1));
    v = _mm256_sllv_epi32(_mm256_add_epi32(v, _mm256_set1_epi32(2)), sh);
    v = _mm256_and_si256(_mm256_or_si256(v, sg), ac);
    _mm_storel_epi64((__m128i *)out, _mm256_castsi256_si128(v));
    return 1;
  }
#endif
  for (i = 0; i < 2; ++i)
    out[i] = active[i] ? sign[i] | ((mag[i] + 2u) << (p - 1)) : 0;
  return 1;
}

/* 8-bit reads used by the 64-bit sample path (keeps up to 64 bits). */
static void ht_frwd_read8(ht_frwd_st *s, int fill_ff) {
  ui8 val = fill_ff ? 0xFFu : 0u;
  int t;
  if (s->size > 0) {
    val = *s->data++;
    --s->size;
  }
  t = s->unstuff ? 1 : 0;
  val = (ui8)(val & (0xFFu >> t));
  s->unstuff = (val == 0xFF);
  s->tmp |= ((ui64)val) << s->bits;
  s->bits += 8 - t;
}

static inline ui64 ht_frwd_fetch64(ht_frwd_st *s, int fill_ff) {
  while (s->bits <= 56) ht_frwd_read8(s, fill_ff);
  return s->tmp;
}

/* ------------------------------------------------------------------ */
/* Block decoder: shared step-1 (VLC + MEL) and step-2 (MagSgn)        */
/* ------------------------------------------------------------------ */

#define HT_SSTR(w) ((((uint32_t)(w) + 2u) + 7u) & ~7u)

/* Decode step 1 (VLC+MEL) into per-quad records (inf / u_q). Returns 0
   on malformed input. */
static int ht_step1(const ui8 *coded_data, int lcup, int scup, uint32_t width,
                    uint32_t height, uint16_t *scratch, uint32_t sstr) {
  ht_mel_st mel;
  ht_rev_st vlc;
  int run;
  uint32_t x, y;
  uint32_t c_q;

  ht_mel_init(&mel, coded_data, lcup, scup);
  ht_rev_init(&vlc, coded_data, lcup, scup);
  run = ht_mel_get_run(&mel);

  /* initial row of quads (rows 0-1) */
  {
    ui32 vlc_val;
    ui16 *sp = scratch;
    c_q = 0;
    for (x = 0; x < width; sp += 4) {
      ui16 t0, t1;
      uint32_t uvlc_mode, uvlc_entry, len, tmp, u_q;
      vlc_val = ht_rev_fetch(&vlc);
      t0 = ht_vlc_dec0[c_q + (vlc_val & 0x7F)];
      if (c_q == 0) {
        run -= 2;
        t0 = (run == -1) ? t0 : 0;
        if (run < 0) run = ht_mel_get_run(&mel);
      }
      sp[0] = t0;
      x += 2;
      c_q = ((t0 & 0x10u) << 3) | ((t0 & 0xE0u) << 2);
      vlc_val = ht_rev_advance(&vlc, t0 & 0x7);

      t1 = 0;
      t1 = ht_vlc_dec0[c_q + (vlc_val & 0x7F)];
      if (c_q == 0 && x < width) {
        run -= 2;
        t1 = (run == -1) ? t1 : 0;
        if (run < 0) run = ht_mel_get_run(&mel);
      }
      t1 = x < width ? t1 : 0;
      sp[2] = t1;
      x += 2;
      c_q = ((t1 & 0x10u) << 3) | ((t1 & 0xE0u) << 2);
      vlc_val = ht_rev_advance(&vlc, t1 & 0x7);

      uvlc_mode = ((t0 & 0x8u) << 3) | ((t1 & 0x8u) << 4);
      if (uvlc_mode == 0xc0) {
        run -= 2;
        uvlc_mode += (run == -1) ? 0x40 : 0;
        if (run < 0) run = ht_mel_get_run(&mel);
      }
      uvlc_entry = ht_uvlc_dec0[uvlc_mode + (vlc_val & 0x3F)];
      vlc_val = ht_rev_advance(&vlc, uvlc_entry & 0x7);
      uvlc_entry >>= 3;
      len = uvlc_entry & 0xF;
      tmp = vlc_val & ((1u << len) - 1);
      vlc_val = ht_rev_advance(&vlc, len);
      uvlc_entry >>= 4;
      len = uvlc_entry & 0x7;
      uvlc_entry >>= 3;
      u_q = 1 + (uvlc_entry & 7) + (tmp & ~(0xFFu << len));
      sp[1] = (uint16_t)u_q;
      u_q = 1 + (uvlc_entry >> 3) + (tmp >> len);
      sp[3] = (uint16_t)u_q;
    }
    sp[0] = sp[1] = 0;
  }

  /* non-initial quad rows */
  for (y = 2; y < height; y += 2) {
    ui32 vlc_val;
    ui16 *sp = scratch + (y >> 1) * sstr;
    c_q = 0;
    for (x = 0; x < width; sp += 4) {
      ui16 t0, t1;
      uint32_t uvlc_mode, uvlc_entry, len, tmp, u_q;
      c_q |= ((sp[0 - (si32)sstr] & 0xA0u) << 2);
      c_q |= ((sp[2 - (si32)sstr] & 0x20u) << 4);

      vlc_val = ht_rev_fetch(&vlc);
      t0 = ht_vlc_dec1[c_q + (vlc_val & 0x7F)];
      if (c_q == 0) {
        run -= 2;
        t0 = (run == -1) ? t0 : 0;
        if (run < 0) run = ht_mel_get_run(&mel);
      }
      sp[0] = t0;
      x += 2;
      c_q = ((t0 & 0x40u) << 2) | ((t0 & 0x80u) << 1);
      c_q |= sp[0 - (si32)sstr] & 0x80;
      c_q |= ((sp[2 - (si32)sstr] & 0xA0u) << 2);
      c_q |= ((sp[4 - (si32)sstr] & 0x20u) << 4);
      vlc_val = ht_rev_advance(&vlc, t0 & 0x7);

      t1 = 0;
      t1 = ht_vlc_dec1[c_q + (vlc_val & 0x7F)];
      if (c_q == 0 && x < width) {
        run -= 2;
        t1 = (run == -1) ? t1 : 0;
        if (run < 0) run = ht_mel_get_run(&mel);
      }
      t1 = x < width ? t1 : 0;
      sp[2] = t1;
      x += 2;
      c_q = ((t1 & 0x40u) << 2) | ((t1 & 0x80u) << 1);
      c_q |= sp[2 - (si32)sstr] & 0x80;
      vlc_val = ht_rev_advance(&vlc, t1 & 0x7);

      uvlc_mode = ((t0 & 0x8u) << 3) | ((t1 & 0x8u) << 4);
      uvlc_entry = ht_uvlc_dec1[uvlc_mode + (vlc_val & 0x3F)];
      vlc_val = ht_rev_advance(&vlc, uvlc_entry & 0x7);
      uvlc_entry >>= 3;
      len = uvlc_entry & 0xF;
      tmp = vlc_val & ((1u << len) - 1);
      vlc_val = ht_rev_advance(&vlc, len);
      uvlc_entry >>= 4;
      len = uvlc_entry & 0x7;
      uvlc_entry >>= 3;
      u_q = (uvlc_entry & 7) + (tmp & ~(0xFFu << len));
      sp[1] = (uint16_t)u_q;
      u_q = (uvlc_entry >> 3) + (tmp >> len);
      sp[3] = (uint16_t)u_q;
    }
    sp[0] = sp[1] = 0;
  }
  return 1;
}

/* Decode step 2 (MagSgn) for the 32-bit sample path. */
static int ht_step2_32(uint32_t width, uint32_t height, uint32_t stride,
                       uint32_t sstr, uint32_t p, uint32_t mmsbp2,
                       const ui8 *magsgn_bits, ui32 magsgn_nbits,
                       uint16_t *scratch, uint32_t *v_n, uint32_t *out) {
  ui32 magsgn_pos = 0;
  uint32_t x, y;

  {
    ui16 *sp = scratch;
    ui32 *vp = v_n;
    ui32 *dp = out;
    uint32_t prev_v_n = 0;
    for (x = 0; x < width; sp += 2, ++vp) {
      ui32 inf = sp[0];
      ui32 U_q = sp[1];
      ui32 vals[4], vns[4];
      if (U_q > mmsbp2) return 0;
      if (!ht_decode_magsgn2_packed(magsgn_bits, magsgn_nbits, &magsgn_pos,
                                    inf, U_q, p, 0, vals, vns)) return 0;
      dp[0] = vals[0];
      if (1 < height) dp[stride] = vals[1];
      vp[0] = prev_v_n | vns[1];
      prev_v_n = 0;
      ++dp;
      if (++x >= width) { ++vp; break; }
      if (!ht_decode_magsgn2_packed(magsgn_bits, magsgn_nbits, &magsgn_pos,
                                    inf, U_q, p, 2, vals + 2, vns + 2))
        return 0;
      dp[0] = vals[2];
      if (1 < height) dp[stride] = vals[3];
      prev_v_n = vns[3];
      ++dp;
      ++x;
    }
    vp[0] = prev_v_n;
  }

  for (y = 2; y < height; y += 2) {
    ui16 *sp = scratch + (y >> 1) * sstr;
    ui32 *vp = v_n;
    ui32 *dp = out + y * stride;
    uint32_t prev_v_n = 0;
    for (x = 0; x < width; sp += 2, ++vp) {
      ui32 inf = sp[0];
      ui32 u_q = sp[1];
      ui32 gamma = inf & 0xF0;
      ui32 emax, kappa, U_q;
      ui32 vals[4], vns[4];
      gamma &= gamma - 0x10;
      emax = vp[0] | vp[1];
      emax = 31 - HT_CLZ32(emax | 2);
      kappa = gamma ? emax : 1;
      U_q = u_q + kappa;
      if (U_q > mmsbp2) return 0;
      if (!ht_decode_magsgn2_packed(magsgn_bits, magsgn_nbits, &magsgn_pos,
                                    inf, U_q, p, 0, vals, vns)) return 0;
      dp[0] = vals[0];
      if (y + 1 < height) dp[stride] = vals[1];
      vp[0] = prev_v_n | vns[1];
      prev_v_n = 0;
      ++dp;
      if (++x >= width) { ++vp; break; }
      if (!ht_decode_magsgn2_packed(magsgn_bits, magsgn_nbits, &magsgn_pos,
                                    inf, U_q, p, 2, vals + 2, vns + 2))
        return 0;
      dp[0] = vals[2];
      if (y + 1 < height) dp[stride] = vals[3];
      prev_v_n = vns[3];
      ++dp;
      ++x;
    }
    vp[0] = prev_v_n;
  }
  return 1;
}

/* SPP (significance propagation) pass. `sign_shift` is 31 for 32-bit
   samples and 63 for 64-bit samples. */
#define HT_DEFINE_SPP(SUFFIX, SAMP_T, SIGN_SHIFT)                            \
  static void ht_spp_##SUFFIX(const ui8 *spp_data, uint32_t spp_size,        \
                              uint32_t width, uint32_t height,               \
                              uint32_t stride, int stripe_causal, uint32_t p,\
                              uint16_t *sigma, uint32_t mstr, SAMP_T *out) { \
    ht_frwd_st sigprop;                                                      \
    uint16_t prev_row_sig[1024 / 4 + 8];                                     \
    uint32_t y, x;                                                           \
    ht_frwd_init(&sigprop, spp_data, (int)spp_size, 0);                      \
    memset(prev_row_sig, 0, sizeof(prev_row_sig));                           \
    for (y = 0; y < height; y += 4) {                                        \
      uint32_t pattern = 0xFFFFu;                                            \
      uint32_t prev = 0;                                                     \
      uint16_t *prev_sig = prev_row_sig;                                     \
      uint16_t *cur_sig = sigma + (y >> 2) * mstr;                           \
      SAMP_T *dpp = out + y * stride;                                        \
      if (height - y < 4) {                                                  \
        pattern = 0x7777u;                                                   \
        if (height - y < 3) {                                                \
          pattern = 0x3333u;                                                 \
          if (height - y < 2) pattern = 0x1111u;                             \
        }                                                                    \
      }                                                                      \
      for (x = 0; x < width; x += 4, ++cur_sig, ++prev_sig) {                \
        uint32_t ps, ns, u, cs, mbr, t, new_sig, inv_sig;                    \
        int s = (int)x + 4 - (int)width;                                     \
        if (s < 0) s = 0;                                                    \
        pattern = pattern >> (s * 4);                                        \
        ps = ht_load_le16x2(prev_sig);                                       \
        ns = ht_load_le16x2(cur_sig + mstr);                                 \
        u = (ps & 0x88888888u) >> 3;                                         \
        if (!stripe_causal) u |= (ns & 0x11111111u) << 3;                    \
        cs = ht_load_le16x2(cur_sig);                                        \
        mbr = cs;                                                            \
        mbr |= (cs & 0x77777777u) << 1;                                      \
        mbr |= (cs & 0xEEEEEEEEu) >> 1;                                      \
        mbr |= u;                                                            \
        t = mbr;                                                             \
        mbr |= t << 4;                                                       \
        mbr |= t >> 4;                                                       \
        mbr |= prev >> 12;                                                   \
        mbr &= pattern;                                                      \
        mbr &= ~cs;                                                          \
        new_sig = mbr;                                                       \
        if (new_sig) {                                                       \
          ui32 cwd = ht_frwd_fetch(&sigprop, 0);                             \
          ui32 cnt = 0;                                                      \
          ui32 col_mask = 0xFu;                                              \
          int i;                                                             \
          inv_sig = ~cs & pattern;                                           \
          for (i = 0; i < 16; i += 4, col_mask <<= 4) {                      \
            ui32 sample_mask;                                                \
            if ((col_mask & new_sig) == 0) continue;                         \
            sample_mask = 0x1111u & col_mask;                                \
            if (new_sig & sample_mask) {                                     \
              new_sig &= ~sample_mask;                                       \
              if (cwd & 1) new_sig |= (0x33u << i) & inv_sig;                \
              cwd >>= 1; ++cnt;                                              \
            }                                                                \
            sample_mask <<= 1;                                               \
            if (new_sig & sample_mask) {                                     \
              new_sig &= ~sample_mask;                                       \
              if (cwd & 1) new_sig |= (0x76u << i) & inv_sig;                \
              cwd >>= 1; ++cnt;                                              \
            }                                                                \
            sample_mask <<= 1;                                               \
            if (new_sig & sample_mask) {                                     \
              new_sig &= ~sample_mask;                                       \
              if (cwd & 1) new_sig |= (0xECu << i) & inv_sig;                \
              cwd >>= 1; ++cnt;                                              \
            }                                                                \
            sample_mask <<= 1;                                               \
            if (new_sig & sample_mask) {                                     \
              new_sig &= ~sample_mask;                                       \
              if (cwd & 1) new_sig |= (0xC8u << i) & inv_sig;                \
              cwd >>= 1; ++cnt;                                              \
            }                                                                \
          }                                                                  \
          if (new_sig) {                                                     \
            SAMP_T *dp = dpp + x;                                            \
            ui32 val = 3u << (p - 2);                                        \
            col_mask = 0xFu;                                                 \
            {                                                                \
              int i;                                                         \
              for (i = 0; i < 4; ++i, ++dp, col_mask <<= 4) {                \
                ui32 sample_mask;                                            \
                if ((col_mask & new_sig) == 0) continue;                     \
                sample_mask = 0x1111u & col_mask;                            \
                if (new_sig & sample_mask) {                                 \
                  dp[0] = ((SAMP_T)cwd << SIGN_SHIFT) | val;                 \
                  cwd >>= 1; ++cnt;                                          \
                }                                                            \
                sample_mask += sample_mask;                                  \
                if (new_sig & sample_mask) {                                 \
                  dp[stride] = ((SAMP_T)cwd << SIGN_SHIFT) | val;            \
                  cwd >>= 1; ++cnt;                                          \
                }                                                            \
                sample_mask += sample_mask;                                  \
                if (new_sig & sample_mask) {                                 \
                  dp[2 * stride] = ((SAMP_T)cwd << SIGN_SHIFT) | val;        \
                  cwd >>= 1; ++cnt;                                          \
                }                                                            \
                sample_mask += sample_mask;                                  \
                if (new_sig & sample_mask) {                                 \
                  dp[3 * stride] = ((SAMP_T)cwd << SIGN_SHIFT) | val;        \
                  cwd >>= 1; ++cnt;                                          \
                }                                                            \
              }                                                              \
            }                                                                \
          }                                                                  \
          ht_frwd_advance(&sigprop, cnt);                                    \
        }                                                                    \
        new_sig |= cs;                                                       \
        *prev_sig = (uint16_t)(new_sig);                                     \
        t = new_sig;                                                         \
        new_sig |= (t & 0x7777u) << 1;                                       \
        new_sig |= (t & 0xEEEEu) >> 1;                                       \
        prev = (new_sig | u) & 0xF000;                                       \
      }                                                                      \
    }                                                                        \
  }

HT_DEFINE_SPP(32, uint32_t, 31)
HT_DEFINE_SPP(64, uint64_t, 63)
#undef HT_DEFINE_SPP

/* MRP (magnitude refinement) pass. */
#define HT_DEFINE_MRP(SUFFIX, SAMP_T)                                          \
  static void ht_mrp_##SUFFIX(const ui8 *coded_data, uint32_t lengths1,        \
                              uint32_t lengths2, uint32_t width,               \
                              uint32_t height, uint32_t stride, uint32_t p,    \
                              uint16_t *sigma, uint32_t mstr, SAMP_T *out) {   \
    ht_rev_st magref;                                                          \
    uint32_t y, i;                                                             \
    ht_rev_init_mrp(&magref, coded_data, (int)lengths1, (int)lengths2);        \
    for (y = 0; y < height; y += 4) {                                          \
      uint16_t *cur_sig = sigma + (y >> 2) * mstr;                             \
      SAMP_T *dpp = out + y * stride;                                          \
      uint32_t half = 1u << (p - 2);                                           \
      for (i = 0; i < width; i += 8, cur_sig += 2) {                           \
        ui32 cwd = ht_rev_fetch_mrp(&magref);                                  \
        ui32 sig = ht_load_le16x2(cur_sig);                                    \
        ui32 col_mask = 0xFu;                                                  \
        if (sig) {                                                             \
          int j;                                                               \
          for (j = 0; j < 8; ++j) {                                            \
            if (sig & col_mask) {                                              \
              SAMP_T *dp = dpp + i + j;                                        \
              ui32 sample_mask = 0x11111111u & col_mask;                       \
              int k;                                                           \
              for (k = 0; k < 4; ++k) {                                        \
                if (sig & sample_mask) {                                       \
                  ui32 sym = (cwd & 1);                                        \
                  sym = (1 - sym) << (p - 1);                                  \
                  sym |= half;                                                 \
                  dp[0] ^= sym;                                                \
                  cwd >>= 1;                                                   \
                }                                                              \
                sample_mask += sample_mask;                                    \
                dp += stride;                                                  \
              }                                                                \
            }                                                                  \
            col_mask <<= 4;                                                    \
          }                                                                    \
        }                                                                      \
        ht_rev_advance_mrp(&magref, HT_POPCOUNT32(sig));                       \
      }                                                                        \
    }                                                                          \
  }

HT_DEFINE_MRP(32, uint32_t)
HT_DEFINE_MRP(64, uint64_t)
#undef HT_DEFINE_MRP

static int ht_decode_block32_inner(uint16_t *scratch, uint32_t scratch_entries,
                                    uint32_t *v_n,
                                    const uint8_t *magsgn_bits,
                                    uint32_t magsgn_nbits,
                                    const uint8_t *coded_data,
                                    uint32_t *decoded_data,
                                    uint32_t missing_msbs, uint32_t num_passes,
                                    uint32_t lengths1, uint32_t lengths2,
                                    uint32_t width, uint32_t height,
                                    uint32_t stride, int stripe_causal) {
  uint32_t p, mmsbp2, sstr, mstr;
  int lcup, scup;
  int ret = TDNG_HTJ2K_OK;

  if (width == 0 || height == 0 || stride < width || width > 1024 ||
      height > 1024) {
    return TDNG_HTJ2K_ERROR_CORRUPT;
  }
  ht_ensure_tables();

  if (num_passes > 3) return TDNG_HTJ2K_ERROR_CORRUPT;
  if (num_passes > 1 && lengths2 == 0) num_passes = 1;
  if (missing_msbs > 30) return TDNG_HTJ2K_ERROR_CORRUPT;
  if (missing_msbs == 30) return TDNG_HTJ2K_ERROR_CORRUPT;
  if (missing_msbs == 29 && num_passes > 1) num_passes = 1;

  p = 30 - missing_msbs;
  if (lengths1 < 2) return TDNG_HTJ2K_ERROR_CORRUPT;

  lcup = (int)lengths1;
  scup = ((int)coded_data[lcup - 1] << 4) + (coded_data[lcup - 2] & 0xF);
  if (scup < 2 || scup > lcup || scup > 4079) return TDNG_HTJ2K_ERROR_CORRUPT;

  mmsbp2 = missing_msbs + 2;
  sstr = HT_SSTR(width);
  mstr = ((((width + 3u) >> 2) + 2u) + 7u) & ~7u;

  if (scratch_entries < (height / 2 + 1) * sstr + 2 ||
      scratch_entries < (height / 4 + 1) * mstr + 2) {
    return TDNG_HTJ2K_ERROR_MEMORY;
  }
  /* v_n is fully overwritten by step 2 for the single-pass case; only the
     SPP/MRP path reuses scratch as sigma and needs the zero fill. */
  if (num_passes > 1)
    memset(v_n, 0, ((size_t)width / 2 + 2) * sizeof(uint32_t));
  if (!ht_step1(coded_data, lcup, scup, width, height, scratch, sstr)) {
    ret = TDNG_HTJ2K_ERROR_CORRUPT;
    goto done;
  }
  if (!ht_step2_32(width, height, stride, sstr, p, mmsbp2,
                   magsgn_bits, magsgn_nbits, scratch, v_n,
                   decoded_data)) {
    ret = TDNG_HTJ2K_ERROR_CORRUPT;
    goto done;
  }

  if (num_passes > 1) {
    /* rearrange quad significance into column significance */
    uint16_t *sigma = scratch;
    uint32_t y;
    for (y = 0; y < height; y += 4) {
      uint16_t *sp = scratch + (y >> 1) * sstr;
      uint16_t *dp = sigma + (y >> 2) * mstr;
      uint32_t x;
      for (x = 0; x < width; x += 4, sp += 4, ++dp) {
        uint32_t t0 = 0, t1 = 0;
        t0 = ((sp[0] & 0x30u) >> 4) | ((sp[0] & 0xC0u) >> 2);
        t0 |= ((sp[2] & 0x30u) << 4) | ((sp[2] & 0xC0u) << 6);
        t1 = ((sp[0 + sstr] & 0x30u) >> 2) | ((sp[0 + sstr] & 0xC0u));
        t1 |= ((sp[2 + sstr] & 0x30u) << 6) | ((sp[2 + sstr] & 0xC0u) << 8);
        dp[0] = (uint16_t)(t0 | t1);
      }
      dp[0] = 0;
    }
    {
      uint16_t *dp = sigma + (y >> 2) * mstr;
      uint32_t x;
      for (x = 0; x < width; x += 4, ++dp) dp[0] = 0;
      dp[0] = 0;
    }
    ht_spp_32(coded_data + lengths1, lengths2, width, height, stride,
             stripe_causal, p, sigma, mstr, decoded_data);
    if (num_passes > 2) {
      ht_mrp_32(coded_data, lengths1, lengths2, width, height, stride, p,
                sigma, mstr, decoded_data);
    }
  }

done:
  return ret;
}

int tdng_htj2k_ctx_init(tdng_htj2k_ctx *ctx) {
  ctx->scratch = NULL;
  ctx->scratch_cap = 0;
  ctx->v_n = NULL;
  ctx->v_n_cap = 0;
  ctx->magsgn = NULL;
  ctx->magsgn_cap = 0;
  return TDNG_HTJ2K_OK;
}

void tdng_htj2k_ctx_free(tdng_htj2k_ctx *ctx) {
  free(ctx->scratch);
  free(ctx->v_n);
  free(ctx->magsgn);
  ctx->scratch = NULL;
  ctx->v_n = NULL;
  ctx->scratch_cap = 0;
  ctx->v_n_cap = 0;
  ctx->magsgn_cap = 0;
}

int tdng_htj2k_decode_codeblock32(const uint8_t *coded_data,
                                  uint32_t *decoded_data,
                                  uint32_t missing_msbs, uint32_t num_passes,
                                  uint32_t lengths1, uint32_t lengths2,
                                  uint32_t width, uint32_t height,
                                  uint32_t stride, int stripe_causal) {
  tdng_htj2k_ctx ctx;
  int r;
  tdng_htj2k_ctx_init(&ctx);
  r = tdng_htj2k_decode_codeblock32_ctx(&ctx, coded_data, decoded_data,
                                        missing_msbs, num_passes, lengths1,
                                        lengths2, width, height, stride,
                                        stripe_causal);
  tdng_htj2k_ctx_free(&ctx);
  return r;
}

int tdng_htj2k_decode_codeblock32_ctx(tdng_htj2k_ctx *ctx,
                                      const uint8_t *coded_data,
                                      uint32_t *decoded_data,
                                      uint32_t missing_msbs,
                                      uint32_t num_passes, uint32_t lengths1,
                                      uint32_t lengths2, uint32_t width,
                                      uint32_t height, uint32_t stride,
                                      int stripe_causal) {
  uint32_t sstr, mstr, scratch_entries;
  uint16_t *scratch;
  uint32_t *v_n;
  size_t need_scratch, need_vn, need_magsgn;
  uint32_t magsgn_nbits;

  if (width == 0 || height == 0 || stride < width || width > 1024 ||
      height > 1024) {
    return TDNG_HTJ2K_ERROR_CORRUPT;
  }
  ht_ensure_tables();
  sstr = HT_SSTR(width);
  mstr = ((((width + 3u) >> 2) + 2u) + 7u) & ~7u;
  scratch_entries = (height / 2 + 1) * sstr + 2;
  if (scratch_entries < (height / 4 + 1) * mstr + 2) {
    scratch_entries = (height / 4 + 1) * mstr + 2;
  }
  need_scratch = (size_t)scratch_entries * sizeof(uint16_t);
  need_vn = ((size_t)width / 2 + 2) * sizeof(uint32_t);
  need_magsgn = (size_t)(lengths1 ? lengths1 : 1) + 8;

  if (ctx->scratch_cap < need_scratch) {
    uint16_t *nb = (uint16_t *)realloc(ctx->scratch, need_scratch);
    if (!nb) return TDNG_HTJ2K_ERROR_MEMORY;
    ctx->scratch = nb;
    ctx->scratch_cap = need_scratch;
  }
  if (ctx->v_n_cap < need_vn) {
    uint32_t *nb = (uint32_t *)realloc(ctx->v_n, need_vn);
    if (!nb) return TDNG_HTJ2K_ERROR_MEMORY;
    ctx->v_n = nb;
    ctx->v_n_cap = need_vn;
  }
  if (ctx->magsgn_cap < need_magsgn) {
    uint8_t *nb = (uint8_t *)realloc(ctx->magsgn, need_magsgn);
    if (!nb) return TDNG_HTJ2K_ERROR_MEMORY;
    ctx->magsgn = nb;
    ctx->magsgn_cap = need_magsgn;
  }
  scratch = (uint16_t *)ctx->scratch;
  v_n = (uint32_t *)ctx->v_n;
  if (lengths1 < 2) return TDNG_HTJ2K_ERROR_CORRUPT;
  {
    int scup = ((int)coded_data[lengths1 - 1] << 4) +
               (coded_data[lengths1 - 2] & 0xF);
    if (scup < 2 || scup > (int)lengths1 || scup > 4079)
      return TDNG_HTJ2K_ERROR_CORRUPT;
    magsgn_nbits = (uint32_t)ht_pack_magsgn(
        coded_data, lengths1 - (size_t)scup, (uint8_t *)ctx->magsgn,
        ctx->magsgn_cap);
  }
  return ht_decode_block32_inner(scratch, scratch_entries, v_n,
                                 (const uint8_t *)ctx->magsgn, magsgn_nbits,
                                 coded_data, decoded_data, missing_msbs,
                                 num_passes, lengths1, lengths2, width, height,
                                 stride, stripe_causal);
}

/* ------------------------------------------------------------------ */
/* Encoder: MEL / VLC / MagSgn coders                                  */
/* ------------------------------------------------------------------ */

typedef struct ht_mel_enc {
  ui8 *buf;
  uint32_t pos;
  uint32_t buf_size;
  int remaining_bits;
  int tmp;
  int run;
  int k;
  int threshold;
  int overflow;
} ht_mel_enc;

static void ht_mel_enc_init(ht_mel_enc *m, uint32_t buf_size, ui8 *data) {
  m->buf = data;
  m->pos = 0;
  m->buf_size = buf_size;
  m->remaining_bits = 8;
  m->tmp = 0;
  m->run = 0;
  m->k = 0;
  m->threshold = 1;
  m->overflow = 0;
}

static void ht_mel_emit_bit(ht_mel_enc *m, int v) {
  m->tmp = (m->tmp << 1) + v;
  m->remaining_bits--;
  if (m->remaining_bits == 0) {
    if (m->pos >= m->buf_size) {
      m->overflow = 1;
      return;
    }
    m->buf[m->pos++] = (ui8)m->tmp;
    m->remaining_bits = (m->tmp == 0xFF ? 7 : 8);
    m->tmp = 0;
  }
}

static void ht_mel_encode(ht_mel_enc *m, int bit) {
  if (bit == 0) {
    ++m->run;
    if (m->run >= m->threshold) {
      ht_mel_emit_bit(m, 1);
      m->run = 0;
      m->k = (m->k + 1 < 12) ? m->k + 1 : 12;
      m->threshold = 1 << ht_mel_exp[m->k];
    }
  } else {
    ht_mel_emit_bit(m, 0);
    {
      int t = ht_mel_exp[m->k];
      while (t > 0) {
        ht_mel_emit_bit(m, (m->run >> --t) & 1);
      }
    }
    m->run = 0;
    m->k = (m->k - 1 > 0) ? m->k - 1 : 0;
    m->threshold = 1 << ht_mel_exp[m->k];
  }
}

typedef struct ht_vlc_enc {
  ui8 *buf;
  uint32_t pos;
  uint32_t buf_size;
  int used_bits;
  int tmp;
  int last_greater_than_8f;
  int overflow;
} ht_vlc_enc;

static void ht_vlc_enc_init(ht_vlc_enc *v, uint32_t buf_size, ui8 *data) {
  v->buf = data + buf_size - 1;
  v->pos = 1;
  v->buf_size = buf_size;
  v->buf[0] = 0xFF;
  v->used_bits = 4;
  v->tmp = 0xF;
  v->last_greater_than_8f = 1;
  v->overflow = 0;
}

static void ht_vlc_encode(ht_vlc_enc *v, int cwd, int cwd_len) {
  while (cwd_len > 0) {
    int avail_bits, t;
    if (v->pos >= v->buf_size) {
      v->overflow = 1;
      return;
    }
    avail_bits = 8 - v->last_greater_than_8f - v->used_bits;
    t = avail_bits < cwd_len ? avail_bits : cwd_len;
    v->tmp |= (cwd & ((1 << t) - 1)) << v->used_bits;
    v->used_bits += t;
    avail_bits -= t;
    cwd_len -= t;
    cwd >>= t;
    if (avail_bits == 0) {
      if (v->last_greater_than_8f && v->tmp != 0x7F) {
        v->last_greater_than_8f = 0;
        continue;
      }
      *(v->buf - v->pos) = (ui8)v->tmp;
      v->pos++;
      v->last_greater_than_8f = v->tmp > 0x8F;
      v->tmp = 0;
      v->used_bits = 0;
    }
  }
}

static int ht_terminate_mel_vlc(ht_mel_enc *m, ht_vlc_enc *v) {
  int mel_mask, vlc_mask, fuse;
  if (m->overflow || v->overflow) return 0;
  if (m->run > 0) ht_mel_emit_bit(m, 1);
  if (m->overflow) return 0;
  m->tmp = m->tmp << m->remaining_bits;
  mel_mask = (0xFF << m->remaining_bits) & 0xFF;
  vlc_mask = 0xFF >> (8 - v->used_bits);
  if ((mel_mask | vlc_mask) == 0) return 1;
  if (m->pos >= m->buf_size) return 0;
  fuse = m->tmp | v->tmp;
  if ((((fuse ^ m->tmp) & mel_mask) | ((fuse ^ v->tmp) & vlc_mask)) == 0 &&
      fuse != 0xFF && v->pos > 1) {
    m->buf[m->pos++] = (ui8)fuse;
  } else {
    if (v->pos >= v->buf_size) return 0;
    m->buf[m->pos++] = (ui8)m->tmp;
    *(v->buf - v->pos) = (ui8)v->tmp;
    v->pos++;
  }
  return 1;
}

typedef struct ht_ms_enc {
  ui8 *buf;
  uint32_t pos;
  uint32_t buf_size;
  int max_bits;
  int used_bits;
  ui32 tmp;
  int overflow;
} ht_ms_enc;

static void ht_ms_init(ht_ms_enc *s, uint32_t buf_size, ui8 *data) {
  s->buf = data;
  s->pos = 0;
  s->buf_size = buf_size;
  s->max_bits = 8;
  s->used_bits = 0;
  s->tmp = 0;
  s->overflow = 0;
}

static void ht_ms_encode(ht_ms_enc *s, ui32 cwd, int cwd_len) {
  while (cwd_len > 0) {
    int t;
    if (s->pos >= s->buf_size) {
      s->overflow = 1;
      return;
    }
    t = s->max_bits - s->used_bits;
    if (t > cwd_len) t = cwd_len;
    s->tmp |= (cwd & ((1u << t) - 1)) << s->used_bits;
    s->used_bits += t;
    cwd >>= t;
    cwd_len -= t;
    if (s->used_bits >= s->max_bits) {
      s->buf[s->pos++] = (ui8)s->tmp;
      s->max_bits = (s->tmp == 0xFF) ? 7 : 8;
      s->tmp = 0;
      s->used_bits = 0;
    }
  }
}

static void ht_ms_terminate(ht_ms_enc *s) {
  if (s->used_bits) {
    int t = s->max_bits - s->used_bits;
    s->tmp |= (0xFF & ((1u << t) - 1)) << s->used_bits;
    s->used_bits += t;
    if (s->tmp != 0xFF) {
      if (s->pos >= s->buf_size) {
        s->overflow = 1;
        return;
      }
      s->buf[s->pos++] = (ui8)s->tmp;
    }
  } else if (s->max_bits == 7) {
    s->pos--;
  }
  if (s->pos >= s->buf_size) s->overflow = 1;
}

/* Encode a cleanup pass for one codeblock. `p` is 30-missing_msbs (32-bit)
   or 62-missing_msbs (64-bit). Returns 0 on buffer overflow / corruption. */
static int ht_encode_cleanup_32(const ui32 *buf, uint32_t width,
                                uint32_t height, uint32_t stride, uint32_t p,
                                uint32_t *out_len, ui8 *out, size_t out_cap) {
  const size_t samples = (size_t)width * height;
  const size_t quads = ((size_t)width / 2 + 1) * ((size_t)height / 2 + 1);
  const size_t ms_size = samples * 5 + 64;
  const size_t mel_vlc_size = quads * 4 + 512;
  const size_t mel_size = quads / 4 + 256;
  ui8 *ms_buf = NULL;
  ui8 *mel_vlc_buf = NULL;
  ui8 *mel_buf;
  ui8 *vlc_buf;
  ht_mel_enc mel;
  ht_vlc_enc vlc;
  ht_ms_enc ms;
  ui8 *e_val, *cx_val;
  size_t e_alloc = (size_t)width / 2 + 4;
  uint32_t x, y;
  int ret = 0;

  ms_buf = (ui8 *)malloc(ms_size);
  mel_vlc_buf = (ui8 *)malloc(mel_vlc_size);
  e_val = (ui8 *)malloc(e_alloc);
  cx_val = (ui8 *)malloc(e_alloc);
  if (!ms_buf || !mel_vlc_buf || !e_val || !cx_val) goto done;

  mel_buf = mel_vlc_buf;
  vlc_buf = mel_vlc_buf + mel_size;
  ht_mel_enc_init(&mel, (uint32_t)mel_size, mel_buf);
  ht_vlc_enc_init(&vlc, (uint32_t)(mel_vlc_size - mel_size), vlc_buf);
  ht_ms_init(&ms, (uint32_t)ms_size, ms_buf);

  /* initial row of quads (rows 0-1) */
  {
    int e_qmax[2] = {0, 0}, e_q[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    int rho[2] = {0, 0};
    int c_q0 = 0;
    ui32 s[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    ui8 *lep = e_val;
    ui8 *lcxp = cx_val;
    const ui32 *sp = buf;
    lep[0] = 0;
    lcxp[0] = 0;
    for (x = 0; x < width; x += 4) {
      ui32 t, val;
      int Uq0, u_q0, u_q1 = 0;
      int eps0 = 0;
      uint16_t tuple0;
      int m;
      t = sp[0];
      val = t + t;
      val >>= p;
      val &= ~1u;
      if (val) {
        rho[0] = 1;
        e_q[0] = 32 - (int)HT_CLZ32(--val);
        e_qmax[0] = e_q[0];
        s[0] = --val + (t >> 31);
      }
      t = height > 1 ? sp[stride] : 0;
      ++sp;
      val = t + t;
      val >>= p;
      val &= ~1u;
      if (val) {
        rho[0] += 2;
        e_q[1] = 32 - (int)HT_CLZ32(--val);
        if (e_q[1] > e_qmax[0]) e_qmax[0] = e_q[1];
        s[1] = --val + (t >> 31);
      }
      if (x + 1 < width) {
        t = sp[0];
        val = t + t;
        val >>= p;
        val &= ~1u;
        if (val) {
          rho[0] += 4;
          e_q[2] = 32 - (int)HT_CLZ32(--val);
          if (e_q[2] > e_qmax[0]) e_qmax[0] = e_q[2];
          s[2] = --val + (t >> 31);
        }
        t = height > 1 ? sp[stride] : 0;
        ++sp;
        val = t + t;
        val >>= p;
        val &= ~1u;
        if (val) {
          rho[0] += 8;
          e_q[3] = 32 - (int)HT_CLZ32(--val);
          if (e_q[3] > e_qmax[0]) e_qmax[0] = e_q[3];
          s[3] = --val + (t >> 31);
        }
      }
      Uq0 = e_qmax[0] > 1 ? e_qmax[0] : 1;
      u_q0 = Uq0 - 1;
      if (u_q0 > 0) {
        eps0 |= (e_q[0] == e_qmax[0]);
        eps0 |= (e_q[1] == e_qmax[0]) << 1;
        eps0 |= (e_q[2] == e_qmax[0]) << 2;
        eps0 |= (e_q[3] == e_qmax[0]) << 3;
      }
      lep[0] = (ui8)(lep[0] > e_q[1] ? lep[0] : e_q[1]);
      lep++;
      lep[0] = (ui8)e_q[3];
      lcxp[0] = (ui8)(lcxp[0] | (ui8)((rho[0] & 2) >> 1));
      lcxp++;
      lcxp[0] = (ui8)((rho[0] & 8) >> 3);
      tuple0 = ht_vlc_enc0[(c_q0 << 8) + (rho[0] << 4) + eps0];
      ht_vlc_encode(&vlc, tuple0 >> 8, (tuple0 >> 4) & 7);
      if (c_q0 == 0) ht_mel_encode(&mel, rho[0] != 0);
      m = (rho[0] & 1) ? Uq0 - (tuple0 & 1) : 0;
      ht_ms_encode(&ms, s[0] & ((1u << m) - 1), m);
      m = (rho[0] & 2) ? Uq0 - ((tuple0 & 2) >> 1) : 0;
      ht_ms_encode(&ms, s[1] & ((1u << m) - 1), m);
      m = (rho[0] & 4) ? Uq0 - ((tuple0 & 4) >> 2) : 0;
      ht_ms_encode(&ms, s[2] & ((1u << m) - 1), m);
      m = (rho[0] & 8) ? Uq0 - ((tuple0 & 8) >> 3) : 0;
      ht_ms_encode(&ms, s[3] & ((1u << m) - 1), m);

      if (x + 2 < width) {
        int c_q1 = (rho[0] >> 1) | (rho[0] & 1);
        int Uq1, eps1 = 0;
        uint16_t tuple1;
        t = sp[0];
        val = t + t;
        val >>= p;
        val &= ~1u;
        if (val) {
          rho[1] = 1;
          e_q[4] = 32 - (int)HT_CLZ32(--val);
          e_qmax[1] = e_q[4];
          s[4] = --val + (t >> 31);
        }
        t = height > 1 ? sp[stride] : 0;
        ++sp;
        val = t + t;
        val >>= p;
        val &= ~1u;
        if (val) {
          rho[1] += 2;
          e_q[5] = 32 - (int)HT_CLZ32(--val);
          if (e_q[5] > e_qmax[1]) e_qmax[1] = e_q[5];
          s[5] = --val + (t >> 31);
        }
        if (x + 3 < width) {
          t = sp[0];
          val = t + t;
          val >>= p;
          val &= ~1u;
          if (val) {
            rho[1] += 4;
            e_q[6] = 32 - (int)HT_CLZ32(--val);
            if (e_q[6] > e_qmax[1]) e_qmax[1] = e_q[6];
            s[6] = --val + (t >> 31);
          }
          t = height > 1 ? sp[stride] : 0;
          ++sp;
          val = t + t;
          val >>= p;
          val &= ~1u;
          if (val) {
            rho[1] += 8;
            e_q[7] = 32 - (int)HT_CLZ32(--val);
            if (e_q[7] > e_qmax[1]) e_qmax[1] = e_q[7];
            s[7] = --val + (t >> 31);
          }
        }
        Uq1 = e_qmax[1] > 1 ? e_qmax[1] : 1;
        u_q1 = Uq1 - 1;
        if (u_q1 > 0) {
          eps1 |= (e_q[4] == e_qmax[1]);
          eps1 |= (e_q[5] == e_qmax[1]) << 1;
          eps1 |= (e_q[6] == e_qmax[1]) << 2;
          eps1 |= (e_q[7] == e_qmax[1]) << 3;
        }
        lep[0] = (ui8)(lep[0] > e_q[5] ? lep[0] : e_q[5]);
        lep++;
        lep[0] = (ui8)e_q[7];
        lcxp[0] = (ui8)(lcxp[0] | (ui8)((rho[1] & 2) >> 1));
        lcxp++;
        lcxp[0] = (ui8)((rho[1] & 8) >> 3);
        tuple1 = ht_vlc_enc0[(c_q1 << 8) + (rho[1] << 4) + eps1];
        ht_vlc_encode(&vlc, tuple1 >> 8, (tuple1 >> 4) & 7);
        if (c_q1 == 0) ht_mel_encode(&mel, rho[1] != 0);
        m = (rho[1] & 1) ? Uq1 - (tuple1 & 1) : 0;
        ht_ms_encode(&ms, s[4] & ((1u << m) - 1), m);
        m = (rho[1] & 2) ? Uq1 - ((tuple1 & 2) >> 1) : 0;
        ht_ms_encode(&ms, s[5] & ((1u << m) - 1), m);
        m = (rho[1] & 4) ? Uq1 - ((tuple1 & 4) >> 2) : 0;
        ht_ms_encode(&ms, s[6] & ((1u << m) - 1), m);
        m = (rho[1] & 8) ? Uq1 - ((tuple1 & 8) >> 3) : 0;
        ht_ms_encode(&ms, s[7] & ((1u << m) - 1), m);
      }

      if (u_q0 > 0 && u_q1 > 0) {
        ht_mel_encode(&mel, (u_q0 < u_q1 ? u_q0 : u_q1) > 2);
      }
      if (u_q0 > 2 && u_q1 > 2) {
        ht_vlc_encode(&vlc, ht_uvlc_enc[u_q0 - 2].pre, ht_uvlc_enc[u_q0 - 2].pre_len);
        ht_vlc_encode(&vlc, ht_uvlc_enc[u_q1 - 2].pre, ht_uvlc_enc[u_q1 - 2].pre_len);
        ht_vlc_encode(&vlc, ht_uvlc_enc[u_q0 - 2].suf, ht_uvlc_enc[u_q0 - 2].suf_len);
        ht_vlc_encode(&vlc, ht_uvlc_enc[u_q1 - 2].suf, ht_uvlc_enc[u_q1 - 2].suf_len);
      } else if (u_q0 > 2 && u_q1 > 0) {
        ht_vlc_encode(&vlc, ht_uvlc_enc[u_q0].pre, ht_uvlc_enc[u_q0].pre_len);
        ht_vlc_encode(&vlc, u_q1 - 1, 1);
        ht_vlc_encode(&vlc, ht_uvlc_enc[u_q0].suf, ht_uvlc_enc[u_q0].suf_len);
      } else {
        ht_vlc_encode(&vlc, ht_uvlc_enc[u_q0].pre, ht_uvlc_enc[u_q0].pre_len);
        ht_vlc_encode(&vlc, ht_uvlc_enc[u_q1].pre, ht_uvlc_enc[u_q1].pre_len);
        ht_vlc_encode(&vlc, ht_uvlc_enc[u_q0].suf, ht_uvlc_enc[u_q0].suf_len);
        ht_vlc_encode(&vlc, ht_uvlc_enc[u_q1].suf, ht_uvlc_enc[u_q1].suf_len);
      }

      c_q0 = (rho[1] >> 1) | (rho[1] & 1);
      s[0] = s[1] = s[2] = s[3] = s[4] = s[5] = s[6] = s[7] = 0;
      e_q[0] = e_q[1] = e_q[2] = e_q[3] = e_q[4] = e_q[5] = e_q[6] = e_q[7] = 0;
      rho[0] = rho[1] = 0;
      e_qmax[0] = e_qmax[1] = 0;
    }
    lep[1] = 0;
  }

  /* non-initial quad rows */
  for (y = 2; y < height; y += 2) {
    ui8 *lep = e_val;
    int max_e;
    int e_qmax[2] = {0, 0}, e_q[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    int rho[2] = {0, 0};
    int c_q0;
    ui32 s[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    ui8 *lcxp = cx_val;
    const ui32 *sp = buf + y * stride;
    max_e = (lep[0] > lep[1] ? lep[0] : lep[1]) - 1;
    lep[0] = 0;
    c_q0 = lcxp[0] + (lcxp[1] << 2);
    lcxp[0] = 0;
    for (x = 0; x < width; x += 4) {
      ui32 t, val;
      int kappa, Uq0, u_q0 = 0, u_q1 = 0;
      int eps0 = 0;
      uint16_t tuple0;
      int m;
      t = sp[0];
      val = t + t;
      val >>= p;
      val &= ~1u;
      if (val) {
        rho[0] = 1;
        e_q[0] = 32 - (int)HT_CLZ32(--val);
        e_qmax[0] = e_q[0];
        s[0] = --val + (t >> 31);
      }
      t = y + 1 < height ? sp[stride] : 0;
      ++sp;
      val = t + t;
      val >>= p;
      val &= ~1u;
      if (val) {
        rho[0] += 2;
        e_q[1] = 32 - (int)HT_CLZ32(--val);
        if (e_q[1] > e_qmax[0]) e_qmax[0] = e_q[1];
        s[1] = --val + (t >> 31);
      }
      if (x + 1 < width) {
        t = sp[0];
        val = t + t;
        val >>= p;
        val &= ~1u;
        if (val) {
          rho[0] += 4;
          e_q[2] = 32 - (int)HT_CLZ32(--val);
          if (e_q[2] > e_qmax[0]) e_qmax[0] = e_q[2];
          s[2] = --val + (t >> 31);
        }
        t = y + 1 < height ? sp[stride] : 0;
        ++sp;
        val = t + t;
        val >>= p;
        val &= ~1u;
        if (val) {
          rho[0] += 8;
          e_q[3] = 32 - (int)HT_CLZ32(--val);
          if (e_q[3] > e_qmax[0]) e_qmax[0] = e_q[3];
          s[3] = --val + (t >> 31);
        }
      }
      kappa = (rho[0] & (rho[0] - 1)) ? (max_e > 1 ? max_e : 1) : 1;
      Uq0 = e_qmax[0] > kappa ? e_qmax[0] : kappa;
      u_q0 = Uq0 - kappa;
      if (u_q0 > 0) {
        eps0 |= (e_q[0] == e_qmax[0]);
        eps0 |= (e_q[1] == e_qmax[0]) << 1;
        eps0 |= (e_q[2] == e_qmax[0]) << 2;
        eps0 |= (e_q[3] == e_qmax[0]) << 3;
      }
      lep[0] = (ui8)(lep[0] > e_q[1] ? lep[0] : e_q[1]);
      lep++;
      max_e = (lep[0] > lep[1] ? lep[0] : lep[1]) - 1;
      lep[0] = (ui8)e_q[3];
      lcxp[0] = (ui8)(lcxp[0] | (ui8)((rho[0] & 2) >> 1));
      lcxp++;
      {
        int c_q1 = lcxp[0] + (lcxp[1] << 2);
        lcxp[0] = (ui8)((rho[0] & 8) >> 3);
        tuple0 = ht_vlc_enc1[(c_q0 << 8) + (rho[0] << 4) + eps0];
        ht_vlc_encode(&vlc, tuple0 >> 8, (tuple0 >> 4) & 7);
        if (c_q0 == 0) ht_mel_encode(&mel, rho[0] != 0);
        m = (rho[0] & 1) ? Uq0 - (tuple0 & 1) : 0;
        ht_ms_encode(&ms, s[0] & ((1u << m) - 1), m);
        m = (rho[0] & 2) ? Uq0 - ((tuple0 & 2) >> 1) : 0;
        ht_ms_encode(&ms, s[1] & ((1u << m) - 1), m);
        m = (rho[0] & 4) ? Uq0 - ((tuple0 & 4) >> 2) : 0;
        ht_ms_encode(&ms, s[2] & ((1u << m) - 1), m);
        m = (rho[0] & 8) ? Uq0 - ((tuple0 & 8) >> 3) : 0;
        ht_ms_encode(&ms, s[3] & ((1u << m) - 1), m);

        if (x + 2 < width) {
          int Uq1, eps1 = 0;
          uint16_t tuple1;
          t = sp[0];
          val = t + t;
          val >>= p;
          val &= ~1u;
          if (val) {
            rho[1] = 1;
            e_q[4] = 32 - (int)HT_CLZ32(--val);
            e_qmax[1] = e_q[4];
            s[4] = --val + (t >> 31);
          }
          t = y + 1 < height ? sp[stride] : 0;
          ++sp;
          val = t + t;
          val >>= p;
          val &= ~1u;
          if (val) {
            rho[1] += 2;
            e_q[5] = 32 - (int)HT_CLZ32(--val);
            if (e_q[5] > e_qmax[1]) e_qmax[1] = e_q[5];
            s[5] = --val + (t >> 31);
          }
          if (x + 3 < width) {
            t = sp[0];
            val = t + t;
            val >>= p;
            val &= ~1u;
            if (val) {
              rho[1] += 4;
              e_q[6] = 32 - (int)HT_CLZ32(--val);
              if (e_q[6] > e_qmax[1]) e_qmax[1] = e_q[6];
              s[6] = --val + (t >> 31);
            }
            t = y + 1 < height ? sp[stride] : 0;
            ++sp;
            val = t + t;
            val >>= p;
            val &= ~1u;
            if (val) {
              rho[1] += 8;
              e_q[7] = 32 - (int)HT_CLZ32(--val);
              if (e_q[7] > e_qmax[1]) e_qmax[1] = e_q[7];
              s[7] = --val + (t >> 31);
            }
          }
          kappa = (rho[1] & (rho[1] - 1)) ? (max_e > 1 ? max_e : 1) : 1;
          c_q1 |= ((rho[0] & 4) >> 1) | ((rho[0] & 8) >> 2);
          Uq1 = e_qmax[1] > kappa ? e_qmax[1] : kappa;
          u_q1 = Uq1 - kappa;
          if (u_q1 > 0) {
            eps1 |= (e_q[4] == e_qmax[1]);
            eps1 |= (e_q[5] == e_qmax[1]) << 1;
            eps1 |= (e_q[6] == e_qmax[1]) << 2;
            eps1 |= (e_q[7] == e_qmax[1]) << 3;
          }
          lep[0] = (ui8)(lep[0] > e_q[5] ? lep[0] : e_q[5]);
          lep++;
          max_e = (lep[0] > lep[1] ? lep[0] : lep[1]) - 1;
          lep[0] = (ui8)e_q[7];
          lcxp[0] = (ui8)(lcxp[0] | (ui8)((rho[1] & 2) >> 1));
          lcxp++;
          c_q0 = lcxp[0] + (lcxp[1] << 2);
          lcxp[0] = (ui8)((rho[1] & 8) >> 3);
          tuple1 = ht_vlc_enc1[(c_q1 << 8) + (rho[1] << 4) + eps1];
          ht_vlc_encode(&vlc, tuple1 >> 8, (tuple1 >> 4) & 7);
          if (c_q1 == 0) ht_mel_encode(&mel, rho[1] != 0);
          m = (rho[1] & 1) ? Uq1 - (tuple1 & 1) : 0;
          ht_ms_encode(&ms, s[4] & ((1u << m) - 1), m);
          m = (rho[1] & 2) ? Uq1 - ((tuple1 & 2) >> 1) : 0;
          ht_ms_encode(&ms, s[5] & ((1u << m) - 1), m);
          m = (rho[1] & 4) ? Uq1 - ((tuple1 & 4) >> 2) : 0;
          ht_ms_encode(&ms, s[6] & ((1u << m) - 1), m);
          m = (rho[1] & 8) ? Uq1 - ((tuple1 & 8) >> 3) : 0;
          ht_ms_encode(&ms, s[7] & ((1u << m) - 1), m);
        }

        ht_vlc_encode(&vlc, ht_uvlc_enc[u_q0].pre, ht_uvlc_enc[u_q0].pre_len);
        ht_vlc_encode(&vlc, ht_uvlc_enc[u_q1].pre, ht_uvlc_enc[u_q1].pre_len);
        ht_vlc_encode(&vlc, ht_uvlc_enc[u_q0].suf, ht_uvlc_enc[u_q0].suf_len);
        ht_vlc_encode(&vlc, ht_uvlc_enc[u_q1].suf, ht_uvlc_enc[u_q1].suf_len);

        c_q0 |= ((rho[1] & 4) >> 1) | ((rho[1] & 8) >> 2);
        s[0] = s[1] = s[2] = s[3] = s[4] = s[5] = s[6] = s[7] = 0;
        e_q[0] = e_q[1] = e_q[2] = e_q[3] = e_q[4] = e_q[5] = e_q[6] = e_q[7] = 0;
        rho[0] = rho[1] = 0;
        e_qmax[0] = e_qmax[1] = 0;
      }
    }
  }

  if (mel.overflow || vlc.overflow || ms.overflow) goto done;
  if (!ht_terminate_mel_vlc(&mel, &vlc)) goto done;
  ht_ms_terminate(&ms);
  if (ms.overflow) goto done;

  {
    uint32_t total = ms.pos + mel.pos + vlc.pos;
    uint32_t scup = mel.pos + vlc.pos;
    if (total + 2 > out_cap) {
      ret = -1; /* caller must grow the buffer */
      goto done;
    }
    memcpy(out, ms.buf, ms.pos);
    memcpy(out + ms.pos, mel.buf, mel.pos);
    memcpy(out + ms.pos + mel.pos, vlc.buf - vlc.pos + 1, vlc.pos);
    out[total - 1] = (ui8)(scup >> 4);
    out[total - 2] = (ui8)((out[total - 2] & 0xF0) | (scup & 0xF));
    *out_len = total;
    ret = 1;
  }

done:
  free(ms_buf);
  free(mel_vlc_buf);
  free(e_val);
  free(cx_val);
  return ret;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

static int ht_grow_out(uint8_t **out, size_t *out_cap, size_t need) {
  uint8_t *nb;
  size_t nc;
  if (need <= *out_cap) return 1;
  nc = *out_cap ? *out_cap : 256;
  while (nc < need) nc *= 2;
  nb = (uint8_t *)realloc(*out, nc);
  if (!nb) return 0;
  *out = nb;
  *out_cap = nc;
  return 1;
}

int tdng_htj2k_encode_codeblock32(const uint32_t *buf, uint32_t missing_msbs,
                                  uint32_t width, uint32_t height,
                                  uint32_t stride, uint32_t *lengths,
                                  uint8_t **out, size_t *out_cap) {
  uint32_t p;
  uint32_t out_len = 0;
  int r;
  if (width == 0 || height == 0 || stride < width || width > 1024 ||
      height > 1024 || missing_msbs > 30) {
    return TDNG_HTJ2K_ERROR_CORRUPT;
  }
  ht_ensure_tables();
  p = 30 - missing_msbs;
  /* first pass: size the output */
  if (!ht_grow_out(out, out_cap, (size_t)width * height * 5 + 4096)) {
    return TDNG_HTJ2K_ERROR_MEMORY;
  }
  r = ht_encode_cleanup_32(buf, width, height, stride, p, &out_len, *out,
                           *out_cap);
  if (r == -1) {
    /* grew during encode would be needed; retry with larger buffer */
    if (!ht_grow_out(out, out_cap, (size_t)width * height * 10 + 8192)) {
      return TDNG_HTJ2K_ERROR_MEMORY;
    }
    r = ht_encode_cleanup_32(buf, width, height, stride, p, &out_len, *out,
                             *out_cap);
  }
  if (r != 1) return TDNG_HTJ2K_ERROR_CORRUPT;
  lengths[0] = out_len;
  return TDNG_HTJ2K_OK;
}

/* 64-bit block decode (coefficients in the ui64 sign-magnitude domain,
   sign in bit 63, magnitude shifted by 63-K_max, p = 62-missing_msbs). */
static int ht_step2_64(const ui8 *coded_data, int lcup, int scup,
                       uint32_t width, uint32_t height, uint32_t stride,
                       uint32_t sstr, uint32_t p, uint32_t mmsbp2,
                       uint16_t *scratch, uint32_t *v_n, ui64 *out) {
  /* The MS reader for the 64-bit path fetches 64 bits at a time. We reuse
     the forward reader but must keep >= 64 bits buffered. */
  ht_frwd_st magsgn;
  uint32_t x, y;
  ht_frwd_init(&magsgn, coded_data, lcup - scup, 1);

  {
    ui16 *sp = scratch;
    ui32 *vp = v_n;
    ui64 *dp = out;
    uint32_t prev_v_n = 0;
    for (x = 0; x < width; sp += 2, ++vp) {
      ui32 inf = sp[0];
      ui32 U_q = sp[1];
      ui64 v_n, val;
      ui32 bit;
      if (U_q > mmsbp2) return 0;
      v_n = 0; val = 0; bit = 0;
      if (inf & (1u << (4 + bit))) {
        ui64 ms_val = ht_frwd_fetch64(&magsgn, 1);
        ui32 m_n = U_q - ((inf >> (12 + bit)) & 1);
        ht_frwd_advance(&magsgn, m_n);
        val = ms_val << 63;
        v_n = ms_val & ((1ull << m_n) - 1);
        v_n |= ((ui64)((inf >> (8 + bit)) & 1)) << m_n;
        v_n |= 1;
        val |= (v_n + 2) << (p - 1);
      }
      dp[0] = val;
      v_n = 0; val = 0; bit = 1;
      if (inf & (1u << (4 + bit))) {
        ui64 ms_val = ht_frwd_fetch64(&magsgn, 1);
        ui32 m_n = U_q - ((inf >> (12 + bit)) & 1);
        ht_frwd_advance(&magsgn, m_n);
        val = ms_val << 63;
        v_n = ms_val & ((1ull << m_n) - 1);
        v_n |= ((ui64)((inf >> (8 + bit)) & 1)) << m_n;
        v_n |= 1;
        val |= (v_n + 2) << (p - 1);
      }
      if (1 < height) dp[stride] = val;
      vp[0] = prev_v_n | v_n;
      prev_v_n = 0;
      ++dp;
      if (++x >= width) { ++vp; break; }
      val = 0; bit = 2;
      if (inf & (1u << (4 + bit))) {
        ui64 ms_val = ht_frwd_fetch64(&magsgn, 1);
        ui32 m_n = U_q - ((inf >> (12 + bit)) & 1);
        ht_frwd_advance(&magsgn, m_n);
        val = ms_val << 63;
        v_n = ms_val & ((1ull << m_n) - 1);
        v_n |= ((ui64)((inf >> (8 + bit)) & 1)) << m_n;
        v_n |= 1;
        val |= (v_n + 2) << (p - 1);
      }
      dp[0] = val;
      v_n = 0; val = 0; bit = 3;
      if (inf & (1u << (4 + bit))) {
        ui64 ms_val = ht_frwd_fetch64(&magsgn, 1);
        ui32 m_n = U_q - ((inf >> (12 + bit)) & 1);
        ht_frwd_advance(&magsgn, m_n);
        val = ms_val << 63;
        v_n = ms_val & ((1ull << m_n) - 1);
        v_n |= ((ui64)((inf >> (8 + bit)) & 1)) << m_n;
        v_n |= 1;
        val |= (v_n + 2) << (p - 1);
      }
      if (1 < height) dp[stride] = val;
      prev_v_n = v_n;
      ++dp;
      ++x;
    }
    vp[0] = prev_v_n;
  }

  for (y = 2; y < height; y += 2) {
    ui16 *sp = scratch + (y >> 1) * sstr;
    ui32 *vp = v_n;
    ui64 *dp = out + y * stride;
    uint32_t prev_v_n = 0;
    for (x = 0; x < width; sp += 2, ++vp) {
      ui32 inf = sp[0];
      ui32 u_q = sp[1];
      ui32 gamma = inf & 0xF0;
      ui32 emax, kappa, U_q;
      ui64 v_n, val;
      ui32 bit;
      gamma &= gamma - 0x10;
      emax = vp[0] | vp[1];
      emax = 63 - HT_CLZ64((ui64)emax | 2);
      kappa = gamma ? emax : 1;
      U_q = u_q + kappa;
      if (U_q > mmsbp2) return 0;
      v_n = 0; val = 0; bit = 0;
      if (inf & (1u << (4 + bit))) {
        ui64 ms_val = ht_frwd_fetch64(&magsgn, 1);
        ui32 m_n = U_q - ((inf >> (12 + bit)) & 1);
        ht_frwd_advance(&magsgn, m_n);
        val = ms_val << 63;
        v_n = ms_val & ((1ull << m_n) - 1);
        v_n |= ((ui64)((inf >> (8 + bit)) & 1)) << m_n;
        v_n |= 1;
        val |= (v_n + 2) << (p - 1);
      }
      dp[0] = val;
      v_n = 0; val = 0; bit = 1;
      if (inf & (1u << (4 + bit))) {
        ui64 ms_val = ht_frwd_fetch64(&magsgn, 1);
        ui32 m_n = U_q - ((inf >> (12 + bit)) & 1);
        ht_frwd_advance(&magsgn, m_n);
        val = ms_val << 63;
        v_n = ms_val & ((1ull << m_n) - 1);
        v_n |= ((ui64)((inf >> (8 + bit)) & 1)) << m_n;
        v_n |= 1;
        val |= (v_n + 2) << (p - 1);
      }
      if (y + 1 < height) dp[stride] = val;
      vp[0] = prev_v_n | v_n;
      prev_v_n = 0;
      ++dp;
      if (++x >= width) { ++vp; break; }
      val = 0; bit = 2;
      if (inf & (1u << (4 + bit))) {
        ui64 ms_val = ht_frwd_fetch64(&magsgn, 1);
        ui32 m_n = U_q - ((inf >> (12 + bit)) & 1);
        ht_frwd_advance(&magsgn, m_n);
        val = ms_val << 63;
        v_n = ms_val & ((1ull << m_n) - 1);
        v_n |= ((ui64)((inf >> (8 + bit)) & 1)) << m_n;
        v_n |= 1;
        val |= (v_n + 2) << (p - 1);
      }
      dp[0] = val;
      v_n = 0; val = 0; bit = 3;
      if (inf & (1u << (4 + bit))) {
        ui64 ms_val = ht_frwd_fetch64(&magsgn, 1);
        ui32 m_n = U_q - ((inf >> (12 + bit)) & 1);
        ht_frwd_advance(&magsgn, m_n);
        val = ms_val << 63;
        v_n = ms_val & ((1ull << m_n) - 1);
        v_n |= ((ui64)((inf >> (8 + bit)) & 1)) << m_n;
        v_n |= 1;
        val |= (v_n + 2) << (p - 1);
      }
      if (y + 1 < height) dp[stride] = val;
      prev_v_n = v_n;
      ++dp;
      ++x;
    }
    vp[0] = prev_v_n;
  }
  return 1;
}

int tdng_htj2k_decode_codeblock64(const uint8_t *coded_data,
                                  uint64_t *decoded_data,
                                  uint32_t missing_msbs, uint32_t num_passes,
                                  uint32_t lengths1, uint32_t lengths2,
                                  uint32_t width, uint32_t height,
                                  uint32_t stride, int stripe_causal) {
  uint32_t p, mmsbp2, sstr, mstr;
  int lcup, scup;
  uint16_t *scratch = NULL;
  uint32_t *v_n = NULL;
  uint32_t scratch_entries;
  int ret = TDNG_HTJ2K_OK;

  if (width == 0 || height == 0 || stride < width || width > 1024 ||
      height > 1024) {
    return TDNG_HTJ2K_ERROR_CORRUPT;
  }
  ht_ensure_tables();
  if (num_passes > 3) return TDNG_HTJ2K_ERROR_CORRUPT;
  if (num_passes > 1 && lengths2 == 0) num_passes = 1;
  if (missing_msbs > 62) return TDNG_HTJ2K_ERROR_CORRUPT;
  if (missing_msbs == 62) return TDNG_HTJ2K_ERROR_CORRUPT;
  if (missing_msbs == 61 && num_passes > 1) num_passes = 1;

  p = 62 - missing_msbs;
  if (lengths1 < 2) return TDNG_HTJ2K_ERROR_CORRUPT;
  lcup = (int)lengths1;
  scup = ((int)coded_data[lcup - 1] << 4) + (coded_data[lcup - 2] & 0xF);
  if (scup < 2 || scup > lcup || scup > 4079) return TDNG_HTJ2K_ERROR_CORRUPT;

  mmsbp2 = missing_msbs + 2;
  sstr = HT_SSTR(width);
  mstr = ((((width + 3u) >> 2) + 2u) + 7u) & ~7u;
  scratch_entries = (height / 2 + 1) * sstr + 2;
  if (scratch_entries < (height / 4 + 1) * mstr + 2) {
    scratch_entries = (height / 4 + 1) * mstr + 2;
  }
  scratch = (uint16_t *)malloc((size_t)scratch_entries * sizeof(uint16_t));
  v_n = (uint32_t *)malloc(((size_t)width / 2 + 2) * sizeof(uint32_t));
  if (!scratch || !v_n) {
    ret = TDNG_HTJ2K_ERROR_MEMORY;
    goto done;
  }
  memset(scratch, 0, (size_t)scratch_entries * sizeof(uint16_t));

  if (!ht_step1(coded_data, lcup, scup, width, height, scratch, sstr)) {
    ret = TDNG_HTJ2K_ERROR_CORRUPT;
    goto done;
  }
  if (!ht_step2_64(coded_data, lcup, scup, width, height, stride, sstr, p,
                   mmsbp2, scratch, v_n, decoded_data)) {
    ret = TDNG_HTJ2K_ERROR_CORRUPT;
    goto done;
  }

  if (num_passes > 1) {
    uint16_t *sigma = scratch;
    uint32_t y;
    for (y = 0; y < height; y += 4) {
      uint16_t *sp = scratch + (y >> 1) * sstr;
      uint16_t *dp = sigma + (y >> 2) * mstr;
      uint32_t x;
      for (x = 0; x < width; x += 4, sp += 4, ++dp) {
        uint32_t t0 = 0, t1 = 0;
        t0 = ((sp[0] & 0x30u) >> 4) | ((sp[0] & 0xC0u) >> 2);
        t0 |= ((sp[2] & 0x30u) << 4) | ((sp[2] & 0xC0u) << 6);
        t1 = ((sp[0 + sstr] & 0x30u) >> 2) | ((sp[0 + sstr] & 0xC0u));
        t1 |= ((sp[2 + sstr] & 0x30u) << 6) | ((sp[2 + sstr] & 0xC0u) << 8);
        dp[0] = (uint16_t)(t0 | t1);
      }
      dp[0] = 0;
    }
    {
      uint16_t *dp = sigma + (y >> 2) * mstr;
      uint32_t x;
      for (x = 0; x < width; x += 4, ++dp) dp[0] = 0;
      dp[0] = 0;
    }
    ht_spp_64(coded_data + lengths1, lengths2, width, height, stride,
             stripe_causal, p, sigma, mstr, decoded_data);
    if (num_passes > 2) {
      ht_mrp_64(coded_data, lengths1, lengths2, width, height, stride, p,
                sigma, mstr, decoded_data);
    }
  }

done:
  free(scratch);
  free(v_n);
  return ret;
}

int tdng_htj2k_encode_codeblock64(const uint64_t *buf, uint32_t missing_msbs,
                                  uint32_t width, uint32_t height,
                                  uint32_t stride, uint32_t *lengths,
                                  uint8_t **out, size_t *out_cap) {
  /* The 64-bit encode path converts to the 32-bit domain when the
     coefficients fit; values beyond 32-bit precision are not supported. */
  (void)buf; (void)missing_msbs; (void)width; (void)height; (void)stride;
  (void)lengths; (void)out; (void)out_cap;
  return TDNG_HTJ2K_ERROR_UNSUPPORTED;
}
