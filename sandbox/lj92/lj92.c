/* lj92.c - clean-room pure-C11 Lossless JPEG (ITU-T T.81 Annex H) codec.
 * See lj92.h for the public API and design notes.
 */
#include "lj92.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* `restrict` is C99/C11; allow this TU to also build as C++ (e.g. unity
 * builds) where the keyword is spelled __restrict on the common compilers. */
#if defined(__cplusplus) && !defined(restrict)
#if defined(__GNUC__) || defined(__clang__) || defined(_MSC_VER)
#define restrict __restrict
#else
#define restrict
#endif
#endif

/* ===========================================================================
 * Platform / SIMD configuration
 * ===========================================================================
 * Compile-time gating: LJ92_HAVE_{SSE2,SSE41,AVX2} say which kernel families
 * are compiled in. On x86 with GCC/Clang we can always compile them (the hot
 * functions carry __attribute__((target(...))) so they don't need global -m
 * flags) and pick at runtime via CPUID. Define LJ92_DISABLE_SIMD to force a
 * scalar-only build (e.g. for non-x86 targets or minimal binaries). */
#if (defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || \
     defined(_M_IX86))
#define LJ92_X86 1
#endif

#if defined(LJ92_X86) && (defined(__GNUC__) || defined(__clang__)) && \
    !defined(LJ92_DISABLE_SIMD)
#define LJ92_HAVE_SSE2 1
#define LJ92_HAVE_SSE41 1
#define LJ92_HAVE_AVX2 1
#endif

#if defined(LJ92_HAVE_SSE2)
#include <immintrin.h>  /* pulls in SSE..AVX2 intrinsics under target attrs */
#endif

#if defined(__GNUC__) || defined(__clang__)
#define LJ92_ALWAYS_INLINE inline __attribute__((always_inline))
#define LJ92_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define LJ92_BSWAP64(x) __builtin_bswap64(x)
#else
#define LJ92_ALWAYS_INLINE inline
#define LJ92_UNLIKELY(x) (x)
static uint64_t LJ92_BSWAP64(uint64_t x) {
  return ((x & 0xff00000000000000ULL) >> 56) |
         ((x & 0x00ff000000000000ULL) >> 40) |
         ((x & 0x0000ff0000000000ULL) >> 24) |
         ((x & 0x000000ff00000000ULL) >> 8) |
         ((x & 0x00000000ff000000ULL) << 8) |
         ((x & 0x0000000000ff0000ULL) << 24) |
         ((x & 0x000000000000ff00ULL) << 40) |
         ((x & 0x00000000000000ffULL) << 56);
}
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#define LJ92_MAX_COMPONENTS 16

/* The entropy bit reader refills in 8-byte chunks and may overshoot the true
 * end of the stream on corrupt input. We destuff into a buffer padded with
 * LJ92_PAD zero bytes plus 8 more so the final 8-byte load stays in bounds. */
#define LJ92_PAD 32
#define LJ92_ALLOC_PAD (LJ92_PAD + 8)

/* ===========================================================================
 * Reconstruction kernel dispatch (declared up front, defined far below)
 * ===========================================================================
 * Only predictor 1 (running horizontal sum) benefits from SIMD; predictor 7
 * and friends are a 2D recurrence and stay scalar. */
typedef void (*lj92_psum_fn)(u16* dst, const u16* diff, int count, u16 seed);
static lj92_psum_fn g_psum;       /* contiguous (mono) prefix sum (decode pred 1) */

/* Encoder per-row residual kernel (encode predictor has no recurrence). */
typedef void (*lj92_encdiff_fn)(const u16* cur, const u16* prev, int W, int NC,
                                int pred, int initpx, int row, int16_t* diff);
static lj92_encdiff_fn g_encdiff;

/* Encoder per-row SSSS-category kernel (vectorized bit-length of |residual|).
 * This is the hot part of the frequency-scan/histogram pass and of the body's
 * category lookup; the scalar clz dominated both, so it is dispatched too. */
typedef void (*lj92_ssss_fn)(const int16_t* diff, int n, u8* sbuf);
static lj92_ssss_fn g_ssss_row;
static lj92_simd g_simd_active = LJ92_SIMD_AUTO;
static lj92_simd g_simd_forced = LJ92_SIMD_AUTO;
static void lj92_simd_init(void);

/* ===========================================================================
 * Fast MSB-first bit reader
 * ===========================================================================
 * `acc` holds the next bits left-justified (top = next bit); `cnt` is the
 * number of valid bits. The refill keeps one load per symbol off the critical
 * path: it is the branchless "or-in / advance / |=56" trick, invoked only when
 * cnt < 32 so one refill always covers a whole symbol (<=16 Huffman + <=16
 * residual bits). A windowed reader (reload a 64-bit window each symbol) was
 * tried and is ~30% slower here because it puts two dependent loads (window +
 * LUT) on the per-symbol critical path. */
typedef struct {
  const u8* p;
  const u8* p_end;  /* one past last real destuffed byte */
  u64 acc;
  int cnt;
} brd;

static LJ92_ALWAYS_INLINE void brd_init(brd* b, const u8* p, const u8* p_end) {
  b->p = p;
  b->p_end = p_end;
  b->acc = 0;
  b->cnt = 0;
}

static LJ92_ALWAYS_INLINE void brd_refill(brd* restrict b) {
  if (b->cnt >= 32) return;
  if (LJ92_UNLIKELY(b->p > b->p_end + LJ92_PAD)) {
    b->cnt = 64;  /* poison: stop advancing; end-check flags corruption */
    return;
  }
  u64 next;
  memcpy(&next, b->p, 8);
  next = LJ92_BSWAP64(next);
  b->acc |= next >> b->cnt;
  b->p += (63 - b->cnt) >> 3;
  b->cnt |= 56;
}

static LJ92_ALWAYS_INLINE int brd_overrun(const brd* restrict b) {
  return b->p > b->p_end + LJ92_PAD ? 1 : 0;
}

/* Decode one (Huffman symbol + signed residual) pair. The LUT entry packs
 * (ssss << 8) | total, where total = code_length + ssss. A single shift of
 * `acc` consumes the whole pair; residual sign-extension is branchless and
 * also yields 0 for ssss == 0. Assumes maxbits <= 16 and cnt topped up. */
static LJ92_ALWAYS_INLINE int brd_decode(brd* restrict b, const u16* restrict lut,
                                         int maxbits) {
  brd_refill(b);
  u32 peek = (u32)(b->acc >> (64 - maxbits));
  u32 e = lut[peek];
  u32 total = e & 0xff;
  u32 ssss = e >> 8;
  u32 resid = (u32)(b->acc >> (64 - total)) & ((1u << ssss) - 1u);
  b->acc <<= total;
  b->cnt -= (int)total;
  int m = 1 << ssss;
  int half = m >> 1;
  int sign = ((int)resid - half) >> 31; /* 0 or -1 */
  return (int)resid + (sign & (1 - m));
}

/* ===========================================================================
 * Decoder state and stream parsing
 * =========================================================================== */
struct lj92_decoder {
  const u8* data;
  int datalen;
  int ix;
  int scanstart;

  int x, y;          /* width, height */
  int bits;          /* bit depth */
  int components;    /* Nf */
  int sof_marker;

  /* Per-table Huffman LUTs: entry = (ssss << 8) | total. After parsing, all
   * LUTs are expanded to a common index width (maxbits_uniform) so the entropy
   * loop peeks with a single shift width across components. */
  u16* lut[LJ92_MAX_COMPONENTS];
  int lutbits[LJ92_MAX_COMPONENTS];
  int num_tables;
  int maxbits_uniform;

  /* Decode-time output config. */
  u16* out;
  int skip;
  const u16* lin;
  int linlen;

  /* Working buffers. */
  u16* rowbuf;     /* outrow[0..1] backing storage */
  u16* outrow[2];
  u16* diffrow;    /* per-row decoded diffs (pred-1 SIMD path) */

  /* Destuffed entropy stream. */
  u8* ebuf;
  int ebuf_len;
  int ebuf_cap;
};

#define BE16(p) (((int)((p)[0]) << 8) | (int)((p)[1]))

/* Find the next non-stuffed marker (0xFF xx, xx != 00/FF). */
static int find_marker(lj92_dec s) {
  int ix = s->ix;
  const u8* d = s->data;
  while (ix < s->datalen - 1) {
    if (d[ix] == 0xFF && d[ix + 1] != 0xFF && d[ix + 1] != 0x00) {
      s->ix = ix + 2;
      return d[ix + 1];
    }
    ix++;
  }
  return -1;
}

static int parse_dht(lj92_dec s) {
  if (s->ix + 2 > s->datalen) return LJ92_ERR_CORRUPT;
  const u8* h = &s->data[s->ix];
  int len = BE16(h);
  if (len < 19) return LJ92_ERR_CORRUPT;
  if (s->ix + len > s->datalen) return LJ92_ERR_CORRUPT;
  if (s->num_tables >= LJ92_MAX_COMPONENTS) return LJ92_ERR_CORRUPT;

  u8 counts[17];
  counts[0] = 0;
  int total_codes = 0;
  for (int L = 1; L <= 16; L++) {
    counts[L] = h[2 + L];
    total_codes += counts[L];
  }
  if (len - 19 < total_codes) return LJ92_ERR_CORRUPT;

  const u8* vals = &s->data[s->ix + 19];
  /* Lossless JPEG symbols are SSSS categories 0..16. Reject anything wider so
   * the residual shifts (1 << ssss) stay defined. */
  for (int i = 0; i < total_codes; i++) {
    if (vals[i] > 16) return LJ92_ERR_NOT_LOSSLESS;
  }

  int maxbits = 16;
  while (maxbits > 0 && !counts[maxbits]) maxbits--;
  if (maxbits <= 0) return LJ92_ERR_CORRUPT;
  s->lutbits[s->num_tables] = maxbits;

  size_t entries = (size_t)1 << maxbits;
  u16* lut = (u16*)malloc(entries * sizeof(u16));
  if (!lut) return LJ92_ERR_NO_MEMORY;
  s->lut[s->num_tables] = lut;
  /* Kraft-incomplete tables leave some prefixes unassigned; fill with a
   * forward-progress sentinel (total=1, ssss=0) so corrupt streams can't
   * stall, then get caught by the end-of-scan overrun check. */
  for (size_t i = 0; i < entries; i++) lut[i] = 0x0001;

  int i = 0, hv = 0, rv = 0, vl = 0, codelen = 1;
  while (i < (1 << maxbits)) {
    if (codelen > maxbits) break;
    if (vl >= counts[codelen]) { codelen++; vl = 0; continue; }
    if (rv == (1 << (maxbits - codelen))) { rv = 0; vl++; hv++; continue; }
    int sym = vals[hv];                       /* ssss category, 0..16 */
    lut[i++] = (u16)((sym << 8) | (codelen + sym)); /* (ssss<<8)|total */
    rv++;
  }
  s->num_tables++;
  s->ix += len;
  return LJ92_OK;
}

static int parse_sof(lj92_dec s, int marker) {
  if (s->ix + 8 > s->datalen) return LJ92_ERR_CORRUPT;
  int Lf = BE16(&s->data[s->ix]);
  if (Lf < 8 || s->ix + Lf > s->datalen) return LJ92_ERR_CORRUPT;
  s->bits = s->data[s->ix + 2];
  s->y = BE16(&s->data[s->ix + 3]);
  s->x = BE16(&s->data[s->ix + 5]);
  s->components = s->data[s->ix + 7];
  s->sof_marker = marker;
  s->ix += Lf;
  if (s->bits < 2 || s->bits > 16) return LJ92_ERR_CORRUPT;
  if (s->components < 1 || s->components > LJ92_MAX_COMPONENTS)
    return LJ92_ERR_CORRUPT;
  return LJ92_OK;
}

static int parse_skip(lj92_dec s) {
  if (s->ix + 2 > s->datalen) return LJ92_ERR_CORRUPT;
  int len = BE16(&s->data[s->ix]);
  if (len < 2 || s->ix + len > s->datalen) return LJ92_ERR_CORRUPT;
  s->ix += len;
  return LJ92_OK;
}

/* Destuff the entropy payload at start_off into s->ebuf: collapse FF00 -> FF,
 * stop at the first real marker, append LJ92_ALLOC_PAD zero bytes. */
static int destuff(lj92_dec s, int start_off) {
  if (start_off < 0 || start_off >= s->datalen) return LJ92_ERR_CORRUPT;
  if (s->datalen > INT_MAX - LJ92_ALLOC_PAD) return LJ92_ERR_CORRUPT;
  int remain = s->datalen - start_off;
  int need = remain + LJ92_ALLOC_PAD;
  if (s->ebuf_cap < need) {
    u8* nb = (u8*)realloc(s->ebuf, (size_t)need);
    if (!nb) return LJ92_ERR_NO_MEMORY;
    s->ebuf = nb;
    s->ebuf_cap = need;
  }
  const u8* src = s->data + start_off;
  const u8* end = s->data + s->datalen;
  u8* dst = s->ebuf;
  while (src < end) {
    u8 c = *src++;
    *dst++ = c;
    if (c == 0xFF) {
      if (src >= end) break;
      u8 c2 = *src;
      if (c2 == 0x00) { src++; continue; }
      if (c2 == 0xFF) continue;
      dst--;  /* real marker: drop the trailing FF */
      break;
    }
  }
  s->ebuf_len = (int)(dst - s->ebuf);
  memset(dst, 0, (size_t)LJ92_ALLOC_PAD);
  return LJ92_OK;
}

/* ===========================================================================
 * Fused scan: decode + predict + store in one loop. The predictor work and the
 * store fill the backend idle left by the serial Huffman dependency chain, so
 * fusing beats a separate decode/reconstruct split. maxbits is unified across
 * components (one peek width); PRED/NC/LIN are compile-time constants. */
static LJ92_ALWAYS_INLINE int run_scan(lj92_dec s, brd* restrict b, const int PRED,
                                       const int NC, const int LIN) {
  const int W = s->x, H = s->y;
  const int out_stride = W * NC + s->skip;
  const int init_px = 1 << (s->bits - 1);
  const u16* lin = s->lin;
  const int linlen = s->linlen;
  const int maxbits = s->maxbits_uniform;

  const u16* hl[LJ92_MAX_COMPONENTS];
  if (s->num_tables < 1) return LJ92_ERR_CORRUPT;
  for (int c = 0; c < NC; c++) {
    int idx = (c < s->num_tables) ? c : 0;
    hl[c] = s->lut[idx];
    if (!hl[c]) return LJ92_ERR_CORRUPT;
  }

  u16* out = s->out;
  u16* rowbuf = NULL, *lastraw = NULL;
  if (LIN) {
    rowbuf = s->outrow[0];
    lastraw = s->outrow[1];
    if (!rowbuf || !lastraw) return LJ92_ERR_NO_MEMORY;
  }

  /* Row 0: predictor 1 (left); first sample uses 2^(P-1). */
  {
    for (int c = 0; c < NC; c++) {
      int d = brd_decode(b, hl[c], maxbits);
      int raw = (int)(u16)(init_px + d);
      if (LIN) {
        if ((unsigned)raw > (unsigned)linlen) return LJ92_ERR_CORRUPT;
        rowbuf[c] = (u16)raw;
        out[c] = lin[raw];
      } else {
        out[c] = (u16)raw;
      }
    }
    for (int col = 1; col < W; col++) {
      int cur = col * NC, prv = cur - NC;
      for (int c = 0; c < NC; c++) {
        int Px = LIN ? rowbuf[prv + c] : out[prv + c];
        int d = brd_decode(b, hl[c], maxbits);
        int raw = (int)(u16)(Px + d);
        if (LIN) {
          if ((unsigned)raw > (unsigned)linlen) return LJ92_ERR_CORRUPT;
          rowbuf[cur + c] = (u16)raw;
          out[cur + c] = lin[raw];
        } else {
          out[cur + c] = (u16)raw;
        }
      }
    }
  }

  /* Rows 1..H-1. */
  for (int row = 1; row < H; row++) {
    const u16* prevraw;
    if (LIN) {
      u16* tmp = lastraw; lastraw = rowbuf; rowbuf = tmp;
      prevraw = lastraw;
    } else {
      prevraw = out;
    }
    u16* curout = out + out_stride;
    u16* currow = LIN ? rowbuf : curout;

    /* col 0: predictor = above. */
    for (int c = 0; c < NC; c++) {
      int Px = prevraw[c];
      int d = brd_decode(b, hl[c], maxbits);
      int raw = (int)(u16)(Px + d);
      if (LIN) {
        if ((unsigned)raw > (unsigned)linlen) return LJ92_ERR_CORRUPT;
        currow[c] = (u16)raw;
        curout[c] = lin[raw];
      } else {
        currow[c] = (u16)raw;
      }
    }
    for (int col = 1; col < W; col++) {
      int cur = col * NC, prv = cur - NC;
      for (int c = 0; c < NC; c++) {
        int left = currow[prv + c];
        int above = prevraw[cur + c];
        int abovel = prevraw[prv + c];
        int Px;
        switch (PRED) {
          case 0: Px = 0; break;
          case 1: Px = left; break;
          case 2: Px = above; break;
          case 3: Px = abovel; break;
          case 4: Px = left + above - abovel; break;
          case 5: Px = left + ((above - abovel) >> 1); break;
          case 6: Px = above + ((left - abovel) >> 1); break;
          case 7: default: Px = (left + above) >> 1; break;
        }
        int d = brd_decode(b, hl[c], maxbits);
        int raw = (int)(u16)(Px + d);
        if (LIN) {
          if ((unsigned)raw > (unsigned)linlen) return LJ92_ERR_CORRUPT;
          currow[cur + c] = (u16)raw;
          curout[cur + c] = lin[raw];
        } else {
          currow[cur + c] = (u16)raw;
        }
      }
    }
    out = curout;
  }

  if (brd_overrun(b)) return LJ92_ERR_CORRUPT;
  return LJ92_OK;
}

/* Compile-time specializations for the hot combinations. */
static int scan_p7_n3_nolin(lj92_dec s, brd* b) { return run_scan(s, b, 7, 3, 0); }
static int scan_p7_n3_lin(lj92_dec s, brd* b)   { return run_scan(s, b, 7, 3, 1); }
static int scan_p1_n3_nolin(lj92_dec s, brd* b) { return run_scan(s, b, 1, 3, 0); }
static int scan_p1_n3_lin(lj92_dec s, brd* b)   { return run_scan(s, b, 1, 3, 1); }
static int scan_p7_n1_nolin(lj92_dec s, brd* b) { return run_scan(s, b, 7, 1, 0); }
static int scan_p7_n1_lin(lj92_dec s, brd* b)   { return run_scan(s, b, 7, 1, 1); }
static int scan_p1_n1_nolin(lj92_dec s, brd* b) { return run_scan(s, b, 1, 1, 0); }
static int scan_p1_n1_lin(lj92_dec s, brd* b)   { return run_scan(s, b, 1, 1, 1); }

/* Scalar contiguous prefix sum (also the fallback kernel). */
static void psum_scalar(u16* dst, const u16* diff, int count, u16 seed) {
  u16 acc = seed;
  for (int i = 0; i < count; i++) { acc = (u16)(acc + diff[i]); dst[i] = acc; }
}

/* Predictor-1 mono via two-pass decode + SIMD prefix sum. Measured SLOWER than
 * the fused run_scan path here: lossless-JPEG decode is bound by the serial
 * Huffman dependency, so moving reconstruction out of the entropy loop only
 * adds a memory pass and removes the latency-hiding overlap. Kept behind
 * LJ92_USE_PSUM_PRED1 for A/B comparison; the default routes pred-1 mono
 * through the fused path. */
#ifdef LJ92_USE_PSUM_PRED1
static int scan_p1_n1(lj92_dec s, brd* restrict b) {
  u16* out = s->out;
  u16* thisrow = s->outrow[0];
  u16* lastrow = s->outrow[1];
  if (s->num_tables < 1 || !s->lut[0]) return LJ92_ERR_CORRUPT;
  if (!s->diffrow) return LJ92_ERR_NO_MEMORY;
  const u16* hl = s->lut[0];
  const int hb = s->lutbits[0];
  const int W = s->x;
  const u16* lin = s->lin;
  const int linlen = s->linlen;

  for (int row = 0; row < s->y; row++) {
    int first_px = (row == 0) ? (1 << (s->bits - 1)) : (int)lastrow[0];
    for (int col = 0; col < W; col++)
      s->diffrow[col] = (u16)brd_decode(b, hl, hb);
    g_psum(thisrow, s->diffrow, W, (u16)first_px);
    if (lin) {
      for (int col = 0; col < W; col++) {
        if (thisrow[col] > linlen) return LJ92_ERR_CORRUPT;
        out[col] = lin[thisrow[col]];
      }
    } else {
      memcpy(out, thisrow, (size_t)W * sizeof(u16));
    }
    u16* t = lastrow; lastrow = thisrow; thisrow = t;
    out += W + s->skip;
  }
  if (brd_overrun(b)) return LJ92_ERR_CORRUPT;
  return LJ92_OK;
}
#endif /* LJ92_USE_PSUM_PRED1 */

/* Generic fallback: runtime PRED and NC up to LJ92_MAX_COMPONENTS. */
static int scan_generic(lj92_dec s, brd* restrict b, int pred) {
  const int NC = s->components;
  if (NC < 1 || NC > LJ92_MAX_COMPONENTS) return LJ92_ERR_CORRUPT;
  const int W = s->x, H = s->y;
  const int out_stride = W * NC + s->skip;
  const int init_px = 1 << (s->bits - 1);
  const u16* lin = s->lin;
  const int linlen = s->linlen;
  const int has_lin = (lin != NULL);

  const u16* hl[LJ92_MAX_COMPONENTS];
  int hb[LJ92_MAX_COMPONENTS];
  if (s->num_tables < 1) return LJ92_ERR_CORRUPT;
  for (int c = 0; c < NC; c++) {
    int idx = (c < s->num_tables) ? c : 0;
    hl[c] = s->lut[idx];
    hb[c] = s->lutbits[idx];
    if (!hl[c]) return LJ92_ERR_CORRUPT;
  }

  u16* out = s->out;
  u16* rowbuf = s->outrow[0];
  u16* lastraw = s->outrow[1];

  for (int c = 0; c < NC; c++) {
    int d = brd_decode(b, hl[c], hb[c]);
    int raw = (int)(u16)(init_px + d);
    rowbuf[c] = (u16)raw;
    if (has_lin) {
      if ((unsigned)raw > (unsigned)linlen) return LJ92_ERR_CORRUPT;
      out[c] = lin[raw];
    } else out[c] = (u16)raw;
  }
  for (int col = 1; col < W; col++) {
    int cur = col * NC, prv = cur - NC;
    for (int c = 0; c < NC; c++) {
      int Px = rowbuf[prv + c];
      int d = brd_decode(b, hl[c], hb[c]);
      int raw = (int)(u16)(Px + d);
      rowbuf[cur + c] = (u16)raw;
      if (has_lin) {
        if ((unsigned)raw > (unsigned)linlen) return LJ92_ERR_CORRUPT;
        out[cur + c] = lin[raw];
      } else out[cur + c] = (u16)raw;
    }
  }
  out += out_stride;

  for (int row = 1; row < H; row++) {
    u16* tmp = lastraw; lastraw = rowbuf; rowbuf = tmp;
    for (int c = 0; c < NC; c++) {
      int d = brd_decode(b, hl[c], hb[c]);
      int raw = (int)(u16)(lastraw[c] + d);
      rowbuf[c] = (u16)raw;
      if (has_lin) {
        if ((unsigned)raw > (unsigned)linlen) return LJ92_ERR_CORRUPT;
        out[c] = lin[raw];
      } else out[c] = (u16)raw;
    }
    for (int col = 1; col < W; col++) {
      int cur = col * NC, prv = cur - NC;
      for (int c = 0; c < NC; c++) {
        int left = rowbuf[prv + c];
        int above = lastraw[cur + c];
        int abovel = lastraw[prv + c];
        int Px;
        switch (pred) {
          case 0: Px = 0; break;
          case 1: Px = left; break;
          case 2: Px = above; break;
          case 3: Px = abovel; break;
          case 4: Px = left + above - abovel; break;
          case 5: Px = left + ((above - abovel) >> 1); break;
          case 6: Px = above + ((left - abovel) >> 1); break;
          case 7: default: Px = (left + above) >> 1; break;
        }
        int d = brd_decode(b, hl[c], hb[c]);
        int raw = (int)(u16)(Px + d);
        rowbuf[cur + c] = (u16)raw;
        if (has_lin) {
          if ((unsigned)raw > (unsigned)linlen) return LJ92_ERR_CORRUPT;
          out[cur + c] = lin[raw];
        } else out[cur + c] = (u16)raw;
      }
    }
    out += out_stride;
  }
  if (brd_overrun(b)) return LJ92_ERR_CORRUPT;
  return LJ92_OK;
}

static int parse_scan(lj92_dec s) {
  s->ix = s->scanstart;
  if (s->ix + 3 > s->datalen) return LJ92_ERR_CORRUPT;
  int Ls = BE16(&s->data[s->ix]);
  int ncomp = s->data[s->ix + 2];
  if (Ls < 6 + 2 * ncomp || s->ix + Ls > s->datalen) return LJ92_ERR_CORRUPT;
  if (ncomp < 1 || ncomp > LJ92_MAX_COMPONENTS) return LJ92_ERR_CORRUPT;
  if (s->components > 0 && ncomp != s->components) return LJ92_ERR_CORRUPT;
  int pred = s->data[s->ix + 3 + 2 * ncomp];
  if (pred < 0 || pred > 7) return LJ92_ERR_CORRUPT;
  s->ix += Ls;

  if (s->x <= 0 || s->y <= 0 || s->components <= 0) return LJ92_OK;

  int ret = destuff(s, s->ix);
  if (ret != LJ92_OK) return ret;

  brd b;
  brd_init(&b, s->ebuf, s->ebuf + s->ebuf_len);

  const int NC = s->components;
  const int LIN = (s->lin != NULL) ? 1 : 0;

#ifdef LJ92_USE_PSUM_PRED1
  if (pred == 1 && NC == 1) return scan_p1_n1(s, &b);  /* two-pass + SIMD psum */
#endif
  if (NC == 1) {
    if (pred == 1) return LIN ? scan_p1_n1_lin(s, &b) : scan_p1_n1_nolin(s, &b);
    if (pred == 7) return LIN ? scan_p7_n1_lin(s, &b) : scan_p7_n1_nolin(s, &b);
  } else if (NC == 3) {
    if (pred == 1) return LIN ? scan_p1_n3_lin(s, &b) : scan_p1_n3_nolin(s, &b);
    if (pred == 7) return LIN ? scan_p7_n3_lin(s, &b) : scan_p7_n3_nolin(s, &b);
  }
  return scan_generic(s, &b, pred);
}

static int parse_image(lj92_dec s) {
  for (;;) {
    int m = find_marker(s);
    int ret = LJ92_OK;
    if (m == 0xC4) ret = parse_dht(s);
    else if (m == 0xC3) ret = parse_sof(s, 0xC3);
    else if (m >= 0xC0 && m <= 0xCF && m != 0xC4) ret = parse_sof(s, m);
    else if (m == 0xDB || m == 0xFE) ret = parse_skip(s);
    else if (m == 0xD9) break;        /* EOI */
    else if (m == 0xDA) { s->scanstart = s->ix; break; }  /* SOS */
    else if (m == -1) return LJ92_ERR_CORRUPT;
    else ret = parse_skip(s);
    if (ret != LJ92_OK) return ret;
  }
  return LJ92_OK;
}

static int find_soi(lj92_dec s) {
  if (find_marker(s) != 0xD8) return LJ92_ERR_CORRUPT;
  int ret = parse_image(s);
  if (ret != LJ92_OK) return ret;
  if (s->x <= 0 || s->components <= 0) return LJ92_ERR_CORRUPT;
  if (s->sof_marker != 0xC3) return LJ92_ERR_NOT_LOSSLESS;
  return LJ92_OK;
}

/* Expand every Huffman LUT to the widest table's index width so the entropy
 * loop uses one peek width. A direct LUT of m bits expands to M bits by
 * replicating each entry 2^(M-m) times (the M-bit index's top m bits select
 * the original entry). Done back-to-front so in-place would be safe; here we
 * allocate fresh for clarity. */
static int expand_luts_uniform(lj92_dec s) {
  int M = 0;
  for (int i = 0; i < s->num_tables; i++)
    if (s->lutbits[i] > M) M = s->lutbits[i];
  if (M <= 0) return LJ92_ERR_CORRUPT;
  s->maxbits_uniform = M;
  for (int i = 0; i < s->num_tables; i++) {
    int m = s->lutbits[i];
    if (m == M) continue;
    size_t oldn = (size_t)1 << m;
    int step = 1 << (M - m);
    u16* nl = (u16*)malloc(((size_t)1 << M) * sizeof(u16));
    if (!nl) return LJ92_ERR_NO_MEMORY;
    const u16* ol = s->lut[i];
    for (size_t j = 0; j < oldn; j++)
      for (int k = 0; k < step; k++) nl[j * step + k] = ol[j];
    free(s->lut[i]);
    s->lut[i] = nl;
    s->lutbits[i] = M;
  }
  return LJ92_OK;
}

static void free_decoder(lj92_dec s) {
  for (int i = 0; i < s->num_tables; i++) { free(s->lut[i]); s->lut[i] = NULL; }
  free(s->rowbuf); s->rowbuf = NULL;
  free(s->diffrow); s->diffrow = NULL;
  free(s->ebuf); s->ebuf = NULL;
}

/* ===========================================================================
 * Public decoder API
 * =========================================================================== */
int lj92_decode_open(lj92_dec* dec, const u8* data, int datalen, int* width,
                     int* height, int* bitdepth, int* components) {
  if (!dec) return LJ92_ERR_BAD_HANDLE;
  lj92_simd_init();
  lj92_dec s = (lj92_dec)calloc(1, sizeof(*s));
  if (!s) return LJ92_ERR_NO_MEMORY;
  s->data = data;
  s->datalen = datalen;

  int ret = find_soi(s);
  if (ret == LJ92_OK && s->num_tables >= 1) ret = expand_luts_uniform(s);
  if (ret == LJ92_OK && s->x > 0 && s->components > 0) {
    if (s->x > 0xFFFF || s->components > LJ92_MAX_COMPONENTS) {
      ret = LJ92_ERR_CORRUPT;
    } else {
      size_t row_slots = (size_t)s->x * (size_t)s->components;
      s->rowbuf = (u16*)calloc(row_slots * 2, sizeof(u16));
      s->diffrow = (u16*)calloc(row_slots, sizeof(u16));
      if (!s->rowbuf || !s->diffrow) {
        ret = LJ92_ERR_NO_MEMORY;
      } else {
        s->outrow[0] = s->rowbuf;
        s->outrow[1] = s->rowbuf + row_slots;
      }
    }
  }

  if (ret != LJ92_OK) {
    *dec = NULL;
    free_decoder(s);
    free(s);
    return ret;
  }
  *width = s->x;
  *height = s->y;
  *bitdepth = s->bits;
  *components = s->components;
  *dec = s;
  return LJ92_OK;
}

int lj92_decode_run(lj92_dec dec, u16* target, int write_stride, int skip,
                    const u16* linearize, int linearize_len) {
  (void)write_stride;
  if (!dec) return LJ92_ERR_BAD_HANDLE;
  dec->out = target;
  dec->skip = skip;
  dec->lin = linearize;
  dec->linlen = linearize_len;
  return parse_scan(dec);
}

void lj92_decode_close(lj92_dec dec) {
  if (dec) { free_decoder(dec); free(dec); }
}

/* Run the one-time SIMD dispatch selection up front. Call once before using the
 * codec concurrently so worker threads only ever read the dispatch table. */
void lj92_init(void) { lj92_simd_init(); }

/* ===========================================================================
 * Encoder
 * ===========================================================================
 * Single-scan lossless JPEG with one Huffman table per component (Tdj=j). The
 * predictor reads ORIGINAL (delinearized) neighbor pixels, so unlike the
 * decoder the per-row diff computation has no recurrence and is straightforward
 * to vectorize; the diff/category pass is split out (enc_diff_row, with a SIMD
 * kernel) from the serial bit packing. */
#define LJ92_NSYM 17 /* SSSS categories 0..16 */

typedef struct {
  const u16* image;
  int w, h, bitdepth, comps, pred;
  int read_len, skip;
  const u16* delin;
  int delin_len;

  u8* out;
  size_t cap, len;

  int hist[LJ92_MAX_COMPONENTS][LJ92_NSYM];
  int bits[LJ92_MAX_COMPONENTS][17];      /* bits[c][L]=#codes of length L */
  u8 huffval[LJ92_MAX_COMPONENTS][LJ92_NSYM];
  int nval[LJ92_MAX_COMPONENTS];
  u16 code[LJ92_MAX_COMPONENTS][LJ92_NSYM];
  u8 clen[LJ92_MAX_COMPONENTS][LJ92_NSYM];

  u64 bitbuf;  /* MSB-first, left-justified */
  int nbits;
} lje;

static int enc_clz32(unsigned x) {
#if defined(__GNUC__) || defined(__clang__)
  return x ? __builtin_clz(x) : 32;
#else
  int n = 0; if (!x) return 32; while (!(x & 0x80000000u)) { n++; x <<= 1; } return n;
#endif
}
static LJ92_ALWAYS_INLINE int enc_ssss(int diff) {
  int a = diff < 0 ? -diff : diff;
  return a == 0 ? 0 : 32 - enc_clz32((unsigned)a);
}

/* Scalar default for the dispatched per-row SSSS kernel (also the SIMD tail). */
static void ssss_row_scalar(const int16_t* diff, int n, u8* sbuf) {
  for (int i = 0; i < n; i++) sbuf[i] = (u8)enc_ssss(diff[i]);
}

/* Predictor value at (row,col) from original neighbors, matching the decoder's
 * first-row/first-column special cases. */
static LJ92_ALWAYS_INLINE int enc_px(int pred, int left, int above, int abovel,
                                     int initpx, int row, int col) {
  if (row == 0 && col == 0) return initpx;
  if (row == 0) return left;
  if (col == 0) return above;
  switch (pred) {
    case 1: return left;
    case 2: return above;
    case 3: return abovel;
    case 4: return left + above - abovel;
    case 5: return left + ((above - abovel) >> 1);
    case 6: return above + ((left - abovel) >> 1);
    case 7: default: return (left + above) >> 1;
  }
}

static int enc_reserve(lje* e, size_t extra) {
  size_t need = e->len + extra;
  if (need <= e->cap) return LJ92_OK;
  size_t cap = e->cap ? e->cap : 4096;
  while (cap < need) {
    if (cap > (size_t)INT_MAX / 2) { cap = need; break; }
    cap *= 2;
  }
  if (cap > (size_t)INT_MAX) return LJ92_ERR_TOO_WIDE;
  u8* nb = (u8*)realloc(e->out, cap);
  if (!nb) return LJ92_ERR_NO_MEMORY;
  e->out = nb;
  e->cap = cap;
  return LJ92_OK;
}
static LJ92_ALWAYS_INLINE void enc_u8(lje* e, u8 b) { e->out[e->len++] = b; }
static LJ92_ALWAYS_INLINE void enc_stuffed(lje* e, u8 b) {
  e->out[e->len++] = b;
  if (b == 0xFF) e->out[e->len++] = 0x00;
}
/* Push n (<=32) low bits of v, MSB-first, into a 64-bit accumulator, draining
 * whole bytes (with 0xFF 0x00 stuffing). Caller reserves output space. */
static LJ92_ALWAYS_INLINE void enc_putbits(lje* e, u32 v, int n) {
  e->bitbuf |= ((u64)v & (((u64)1 << n) - 1)) << (64 - e->nbits - n);
  e->nbits += n;
  while (e->nbits >= 8) {
    u8 b = (u8)(e->bitbuf >> 56);
    enc_stuffed(e, b);
    e->bitbuf <<= 8;
    e->nbits -= 8;
  }
}
static void enc_flush(lje* e) {
  if (e->nbits > 0) {
    u8 pad = (u8)((1u << (8 - e->nbits)) - 1u);
    u8 b = (u8)(e->bitbuf >> 56) | pad;
    enc_stuffed(e, b);
    e->bitbuf = 0;
    e->nbits = 0;
  }
}

/* Emit one sample (component c, residual d, category ssss): Huffman code plus
 * residual bits in a single push. Returns 0 if the symbol has no code (cannot
 * happen after the frequency scan; defensive). */
static LJ92_ALWAYS_INLINE int enc_emit(lje* e, int c, int d, int ssss) {
  int cl = e->clen[c][ssss];
  if (cl == 0) return 0;
  if (ssss > 0) {
    u32 resid = (d < 0 ? (u32)(d + (1 << ssss) - 1) : (u32)d) & ((1u << ssss) - 1u);
    enc_putbits(e, ((u32)e->code[c][ssss] << ssss) | resid, cl + ssss);
  } else {
    enc_putbits(e, e->code[c][ssss], cl);
  }
  return 1;
}

/* Limited-length Huffman table generation (ITU-T T.81, Annex K.2/K.3). */
static void enc_build_table(lje* e, int c) {
  float freq[18];
  int codesize[18], others[18];
  for (int i = 0; i < 18; i++) { freq[i] = 0; codesize[i] = 0; others[i] = -1; }
  int total = 0;
  for (int s = 0; s < LJ92_NSYM; s++) total += e->hist[c][s];
  if (total > 0)
    for (int s = 0; s < LJ92_NSYM; s++)
      if (e->hist[c][s]) freq[s] = (float)e->hist[c][s] / (float)total;
  freq[17] = 1e-30f; /* reserved sentinel so the longest real code <=16 bits */

  for (;;) {
    int v1 = -1, v2 = -1; float f1 = 1e30f, f2 = 1e30f;
    for (int i = 0; i < 18; i++) {
      if (freq[i] > 0 && freq[i] <= f1) { f2 = f1; v2 = v1; f1 = freq[i]; v1 = i; }
      else if (freq[i] > 0 && freq[i] <= f2) { f2 = freq[i]; v2 = i; }
    }
    if (v2 < 0) break;
    freq[v1] += freq[v2];
    freq[v2] = 0;
    for (;;) { codesize[v1]++; if (others[v1] < 0) break; v1 = others[v1]; }
    others[v1] = v2;
    for (;;) { codesize[v2]++; if (others[v2] < 0) break; v2 = others[v2]; }
  }

  int bits[33];
  for (int i = 0; i < 33; i++) bits[i] = 0;
  for (int i = 0; i < 18; i++) if (codesize[i]) bits[codesize[i]]++;
  for (int i = 32; i > 16; i--) {
    while (bits[i] > 0) {
      int j = i - 2;
      while (j > 0 && bits[j] == 0) j--;
      if (j == 0) break;
      bits[i] -= 2; bits[i - 1] += 1; bits[j + 1] += 2; bits[j] -= 1;
    }
  }
  for (int i = 16; i > 0; i--) if (bits[i] > 0) { bits[i]--; break; } /* drop sentinel */

  for (int i = 0; i < 17; i++) e->bits[c][i] = bits[i];
  int k = 0;
  for (int L = 1; L <= 16; L++)
    for (int s = 0; s < LJ92_NSYM; s++)
      if (codesize[s] == L) e->huffval[c][k++] = (u8)s;
  e->nval[c] = k;

  for (int i = 0; i < LJ92_NSYM; i++) { e->code[c][i] = 0; e->clen[c][i] = 0; }
  int cd = 0; k = 0;
  for (int L = 1; L <= 16; L++) {
    for (int j = 0; j < bits[L]; j++) {
      u8 sym = e->huffval[c][k++];
      e->code[c][sym] = (u16)cd;
      e->clen[c][sym] = (u8)L;
      cd++;
    }
    cd <<= 1;
  }
}

/* Compute one row of residuals (low 16 bits) for component-interleaved data,
 * reading original delinearized pixels. `cur`/`prev` are this/previous row of
 * delinearized samples (NC-interleaved). Returns the residuals in diff[]. The
 * encoder predictor has no recurrence, so this whole row is data-parallel. */
static void enc_diff_row(const u16* restrict cur, const u16* restrict prev,
                         int W, int NC, int pred, int initpx, int row,
                         int16_t* restrict diff) {
  for (int c = 0; c < NC; c++) {
    int Px = enc_px(pred, 0, prev ? prev[c] : 0, 0, initpx, row, 0);
    diff[c] = (int16_t)(cur[c] - Px);
  }
  for (int col = 1; col < W; col++) {
    int base = col * NC, pv = base - NC;
    for (int c = 0; c < NC; c++) {
      int left = cur[pv + c];
      int above = prev ? prev[base + c] : 0;
      int abovel = prev ? prev[pv + c] : 0;
      int Px = enc_px(pred, left, above, abovel, initpx, row, col);
      diff[base + c] = (int16_t)(cur[base + c] - Px);
    }
  }
}

/* Gather original (delinearized) samples for one output row into `dst`. */
static int enc_fetch_row(lje* e, int row, u16* dst) {
  const int W = e->w, NC = e->comps;
  const u16* src = e->image + (size_t)row * (e->read_len + e->skip);
  for (int i = 0; i < W * NC; i++) {
    u16 p = src[i];
    if (e->delin) {
      if (p >= e->delin_len) return LJ92_ERR_TOO_WIDE;
      p = e->delin[p];
    }
    if (p >= (1 << e->bitdepth)) return LJ92_ERR_TOO_WIDE;
    dst[i] = p;
  }
  return LJ92_OK;
}

static int enc_headers(lje* e) {
  const int NC = e->comps;
  size_t need = 2 + 2 + (size_t)(6 + 3 * NC); /* SOI + SOF3 */
  for (int c = 0; c < NC; c++) need += 2 + 2 + 1 + 16 + (size_t)e->nval[c]; /* DHTs */
  need += 2 + 2 + 1 + (size_t)(2 * NC) + 3; /* SOS */
  int ret = enc_reserve(e, need);
  if (ret != LJ92_OK) return ret;

  enc_u8(e, 0xFF); enc_u8(e, 0xD8);            /* SOI */
  enc_u8(e, 0xFF); enc_u8(e, 0xC3);            /* SOF3 */
  int Lf = 8 + 3 * NC;
  enc_u8(e, (u8)(Lf >> 8)); enc_u8(e, (u8)Lf);
  enc_u8(e, (u8)e->bitdepth);
  enc_u8(e, (u8)(e->h >> 8)); enc_u8(e, (u8)e->h);
  enc_u8(e, (u8)(e->w >> 8)); enc_u8(e, (u8)e->w);
  enc_u8(e, (u8)NC);
  for (int c = 0; c < NC; c++) { enc_u8(e, (u8)c); enc_u8(e, 0x11); enc_u8(e, 0); }

  for (int c = 0; c < NC; c++) {                /* one DHT per component */
    enc_u8(e, 0xFF); enc_u8(e, 0xC4);
    int Lh = 2 + 1 + 16 + e->nval[c];
    enc_u8(e, (u8)(Lh >> 8)); enc_u8(e, (u8)Lh);
    enc_u8(e, (u8)c);                            /* Tc=0, Th=c */
    for (int L = 1; L <= 16; L++) enc_u8(e, (u8)e->bits[c][L]);
    for (int i = 0; i < e->nval[c]; i++) enc_u8(e, e->huffval[c][i]);
  }

  enc_u8(e, 0xFF); enc_u8(e, 0xDA);            /* SOS */
  int Ls = 3 + 2 * NC + 3;
  enc_u8(e, (u8)(Ls >> 8)); enc_u8(e, (u8)Ls);
  enc_u8(e, (u8)NC);
  for (int c = 0; c < NC; c++) { enc_u8(e, (u8)c); enc_u8(e, (u8)(c << 4)); } /* Tdj=c */
  enc_u8(e, (u8)e->pred);
  enc_u8(e, 0);
  enc_u8(e, 0);
  return LJ92_OK;
}

int lj92_encode(const u16* image, int width, int height, int bitdepth,
                int components, int predictor, int read_len, int skip,
                const u16* delinearize, int delinearize_len, u8** encoded,
                int* encoded_len) {
  if (!image || !encoded || !encoded_len) return LJ92_ERR_BAD_HANDLE;
  if (width <= 0 || width > 0xFFFF || height <= 0 || height > 0xFFFF)
    return LJ92_ERR_BAD_HANDLE;
  if (bitdepth < 2 || bitdepth > 16) return LJ92_ERR_BAD_HANDLE;
  if (components < 1 || components > 4) return LJ92_ERR_BAD_HANDLE;
  if (predictor < 1 || predictor > 7) return LJ92_ERR_BAD_HANDLE;
  if (read_len < 0 || skip < 0) return LJ92_ERR_BAD_HANDLE;
  if (delinearize && delinearize_len <= 0) return LJ92_ERR_BAD_HANDLE;
  lj92_simd_init();  /* ensure g_encdiff is selected */

  lje e;
  memset(&e, 0, sizeof(e));
  e.image = image;
  e.w = width; e.h = height; e.bitdepth = bitdepth; e.comps = components;
  e.pred = predictor;
  e.read_len = read_len > 0 ? read_len : width * components;
  e.skip = skip;
  e.delin = delinearize; e.delin_len = delinearize_len;

  const int W = width, H = height, NC = components;
  const int initpx = 1 << (bitdepth - 1);
  /* SIMD ssss writes categories to a row buffer (worth it only when vectorized);
   * the scalar tier stays fused (compute ssss inline, no extra memory pass). */
  const int simd_ssss = (g_simd_active != LJ92_SIMD_SCALAR);
  int ret = LJ92_OK;

  const size_t stride = (size_t)e.read_len + e.skip;
  size_t row_slots = (size_t)W * NC;
  int16_t* diff = (int16_t*)malloc(row_slots * sizeof(int16_t));
  u8* sbuf = (u8*)malloc(row_slots);  /* per-row SSSS categories */
  /* Row buffers are only needed when a delinearize table forces a per-sample
   * transform; otherwise the predictor reads straight out of `image`. */
  u16* row0 = NULL, *row1 = NULL;
  if (delinearize) {
    row0 = (u16*)malloc(row_slots * sizeof(u16));
    row1 = (u16*)malloc(row_slots * sizeof(u16));
    if (!row0 || !row1) { ret = LJ92_ERR_NO_MEMORY; goto done; }
  }
  if (!diff || !sbuf) { ret = LJ92_ERR_NO_MEMORY; goto done; }

  /* ---- Pass 1: frequency scan ---- */
  {
    for (int row = 0; row < H; row++) {
      const u16* cur; const u16* prev;
      if (delinearize) {
        u16* dst = (row & 1) ? row1 : row0;
        ret = enc_fetch_row(&e, row, dst);
        if (ret != LJ92_OK) goto done;
        cur = dst;
        prev = row ? ((row & 1) ? row0 : row1) : NULL;
      } else {
        cur = image + (size_t)row * stride;
        prev = row ? image + (size_t)(row - 1) * stride : NULL;
      }
      g_encdiff(cur, prev, W, NC, predictor, initpx, row, diff);
      if (simd_ssss) {
        g_ssss_row(diff, W * NC, sbuf);
        for (int col = 0; col < W; col++)
          for (int c = 0; c < NC; c++)
            e.hist[c][sbuf[col * NC + c]]++;
      } else {
        for (int col = 0; col < W; col++)
          for (int c = 0; c < NC; c++)
            e.hist[c][enc_ssss(diff[col * NC + c])]++;  /* fused scalar */
      }
    }
  }
  for (int c = 0; c < NC; c++) enc_build_table(&e, c);

  ret = enc_headers(&e);
  if (ret != LJ92_OK) goto done;

  /* ---- Pass 2: entropy-coded body ---- */
  {
    /* Worst case: 16-bit code + 16-bit residual per sample, doubled for byte
     * stuffing, plus slack. */
    size_t worst = (size_t)W * H * NC * 8u + 64u;
    ret = enc_reserve(&e, worst);
    if (ret != LJ92_OK) goto done;
    e.bitbuf = 0; e.nbits = 0;
    for (int row = 0; row < H; row++) {
      const u16* cur; const u16* prev;
      if (delinearize) {
        u16* dst = (row & 1) ? row1 : row0;
        ret = enc_fetch_row(&e, row, dst);
        if (ret != LJ92_OK) goto done;
        cur = dst;
        prev = row ? ((row & 1) ? row0 : row1) : NULL;
      } else {
        cur = image + (size_t)row * stride;
        prev = row ? image + (size_t)(row - 1) * stride : NULL;
      }
      g_encdiff(cur, prev, W, NC, predictor, initpx, row, diff);
      if (simd_ssss) {
        g_ssss_row(diff, W * NC, sbuf);
        for (int col = 0; col < W; col++)
          for (int c = 0; c < NC; c++) {
            int idx = col * NC + c;
            if (!enc_emit(&e, c, diff[idx], sbuf[idx])) { ret = LJ92_ERR_CORRUPT; goto done; }
          }
      } else {
        for (int col = 0; col < W; col++)
          for (int c = 0; c < NC; c++) {
            int d = diff[col * NC + c];               /* fused scalar */
            if (!enc_emit(&e, c, d, enc_ssss(d))) { ret = LJ92_ERR_CORRUPT; goto done; }
          }
      }
    }
    enc_flush(&e);
  }

  ret = enc_reserve(&e, 2);
  if (ret != LJ92_OK) goto done;
  enc_u8(&e, 0xFF); enc_u8(&e, 0xD9); /* EOI */

  {
    u8* shrunk = (u8*)realloc(e.out, e.len);
    if (shrunk) e.out = shrunk;
  }
  *encoded = e.out;
  *encoded_len = (int)e.len;
  e.out = NULL;

done:
  free(row0); free(row1); free(diff); free(sbuf);
  free(e.out);
  return ret;
}

/* ===========================================================================
 * SIMD prefix-sum kernels + runtime dispatch
 * =========================================================================== */
#if defined(LJ92_HAVE_SSE2)
__attribute__((target("sse2"))) static void psum_sse2(u16* dst, const u16* diff,
                                                      int count, u16 seed) {
  u16 carry = seed;
  int i = 0;
  for (; i + 8 <= count; i += 8) {
    __m128i v = _mm_loadu_si128((const __m128i*)(diff + i));
    v = _mm_add_epi16(v, _mm_slli_si128(v, 2));
    v = _mm_add_epi16(v, _mm_slli_si128(v, 4));
    v = _mm_add_epi16(v, _mm_slli_si128(v, 8));
    v = _mm_add_epi16(v, _mm_set1_epi16((short)carry));
    _mm_storeu_si128((__m128i*)(dst + i), v);
    carry = (u16)_mm_extract_epi16(v, 7);
  }
  if (i < count) psum_scalar(dst + i, diff + i, count - i, carry);
}
#endif

#if defined(LJ92_HAVE_AVX2)
__attribute__((target("avx2"))) static void psum_avx2(u16* dst, const u16* diff,
                                                      int count, u16 seed) {
  const __m256i hi_mask = _mm256_setr_epi16(0, 0, 0, 0, 0, 0, 0, 0, -1, -1, -1,
                                            -1, -1, -1, -1, -1);
  u16 carry = seed;
  int i = 0;
  for (; i + 16 <= count; i += 16) {
    __m256i v = _mm256_loadu_si256((const __m256i*)(diff + i));
    v = _mm256_add_epi16(v, _mm256_slli_si256(v, 2));
    v = _mm256_add_epi16(v, _mm256_slli_si256(v, 4));
    v = _mm256_add_epi16(v, _mm256_slli_si256(v, 8));
    /* propagate low-lane total into the high 128-bit lane */
    __m256i low_tot = _mm256_and_si256(
        _mm256_set1_epi16((short)_mm_extract_epi16(_mm256_castsi256_si128(v), 7)),
        hi_mask);
    v = _mm256_add_epi16(v, low_tot);
    v = _mm256_add_epi16(v, _mm256_set1_epi16((short)carry));
    _mm256_storeu_si256((__m256i*)(dst + i), v);
    carry = (u16)_mm_extract_epi16(_mm256_extracti128_si256(v, 1), 7);
  }
  if (i < count) {
#if defined(LJ92_HAVE_SSE2)
    psum_sse2(dst + i, diff + i, count - i, carry);
#else
    psum_scalar(dst + i, diff + i, count - i, carry);
#endif
  }
}
#endif

/* ---- Encoder residual kernels (SIMD) -------------------------------------
 * Column 0 and the scalar tail go through enc_px; the interior (col>=1) is
 * vectorized for the modes whose predictor is exact in wrap-around 16-bit
 * arithmetic: 1/left, 2/above, 3/abovel, 4/(L+A-AL), and 7/avg-floor. Row 0
 * always uses the left predictor (mode 1). Predictors 5 and 6 need a >1-bit
 * arithmetic shift that can differ from the decoder's 32-bit shift at 16-bit
 * depth, so they fall through to the scalar tail. */
#if defined(LJ92_HAVE_SSE2)
__attribute__((target("sse2"))) static void enc_diff_row_sse2(
    const u16* cur, const u16* prev, int W, int NC, int pred, int initpx,
    int row, int16_t* diff) {
  for (int c = 0; c < NC; c++) {
    int Px = enc_px(pred, 0, prev ? prev[c] : 0, 0, initpx, row, 0);
    diff[c] = (int16_t)(cur[c] - Px);
  }
  int n = W * NC, i = NC, mode = (row == 0) ? 1 : pred;
  const __m128i one = _mm_set1_epi16(1);
#define LJ92_SSE_DIFF(PXCODE)                                       \
  for (; i + 8 <= n; i += 8) {                                      \
    __m128i cv = _mm_loadu_si128((const __m128i*)(cur + i));        \
    __m128i px;                                                     \
    PXCODE;                                                         \
    _mm_storeu_si128((__m128i*)(diff + i), _mm_sub_epi16(cv, px));  \
  }
  if (mode == 1) {
    LJ92_SSE_DIFF(px = _mm_loadu_si128((const __m128i*)(cur + i - NC)));
  } else if (mode == 2) {
    LJ92_SSE_DIFF(px = _mm_loadu_si128((const __m128i*)(prev + i)));
  } else if (mode == 3) {
    LJ92_SSE_DIFF(px = _mm_loadu_si128((const __m128i*)(prev + i - NC)));
  } else if (mode == 4) {
    LJ92_SSE_DIFF(
        __m128i l = _mm_loadu_si128((const __m128i*)(cur + i - NC));
        __m128i a = _mm_loadu_si128((const __m128i*)(prev + i));
        __m128i al = _mm_loadu_si128((const __m128i*)(prev + i - NC));
        px = _mm_sub_epi16(_mm_add_epi16(l, a), al));
  } else if (mode == 7) {
    LJ92_SSE_DIFF(
        __m128i l = _mm_loadu_si128((const __m128i*)(cur + i - NC));
        __m128i a = _mm_loadu_si128((const __m128i*)(prev + i));
        px = _mm_sub_epi16(_mm_avg_epu16(l, a),
                           _mm_and_si128(_mm_xor_si128(l, a), one)));
  }
#undef LJ92_SSE_DIFF
  for (; i < n; i++) {
    int col = i / NC, c = i - col * NC;
    int Px = enc_px(pred, cur[i - NC], prev ? prev[i] : 0,
                    prev ? prev[i - NC] : 0, initpx, row, col);
    (void)c;
    diff[i] = (int16_t)(cur[i] - Px);
  }
}
#endif

#if defined(LJ92_HAVE_AVX2)
__attribute__((target("avx2"))) static void enc_diff_row_avx2(
    const u16* cur, const u16* prev, int W, int NC, int pred, int initpx,
    int row, int16_t* diff) {
  for (int c = 0; c < NC; c++) {
    int Px = enc_px(pred, 0, prev ? prev[c] : 0, 0, initpx, row, 0);
    diff[c] = (int16_t)(cur[c] - Px);
  }
  int n = W * NC, i = NC, mode = (row == 0) ? 1 : pred;
  const __m256i one = _mm256_set1_epi16(1);
#define LJ92_AVX_DIFF(PXCODE)                                          \
  for (; i + 16 <= n; i += 16) {                                       \
    __m256i cv = _mm256_loadu_si256((const __m256i*)(cur + i));        \
    __m256i px;                                                        \
    PXCODE;                                                            \
    _mm256_storeu_si256((__m256i*)(diff + i), _mm256_sub_epi16(cv, px)); \
  }
  if (mode == 1) {
    LJ92_AVX_DIFF(px = _mm256_loadu_si256((const __m256i*)(cur + i - NC)));
  } else if (mode == 2) {
    LJ92_AVX_DIFF(px = _mm256_loadu_si256((const __m256i*)(prev + i)));
  } else if (mode == 3) {
    LJ92_AVX_DIFF(px = _mm256_loadu_si256((const __m256i*)(prev + i - NC)));
  } else if (mode == 4) {
    LJ92_AVX_DIFF(
        __m256i l = _mm256_loadu_si256((const __m256i*)(cur + i - NC));
        __m256i a = _mm256_loadu_si256((const __m256i*)(prev + i));
        __m256i al = _mm256_loadu_si256((const __m256i*)(prev + i - NC));
        px = _mm256_sub_epi16(_mm256_add_epi16(l, a), al));
  } else if (mode == 7) {
    LJ92_AVX_DIFF(
        __m256i l = _mm256_loadu_si256((const __m256i*)(cur + i - NC));
        __m256i a = _mm256_loadu_si256((const __m256i*)(prev + i));
        px = _mm256_sub_epi16(_mm256_avg_epu16(l, a),
                              _mm256_and_si256(_mm256_xor_si256(l, a), one)));
  }
#undef LJ92_AVX_DIFF
  for (; i < n; i++) {
    int col = i / NC;
    int Px = enc_px(pred, cur[i - NC], prev ? prev[i] : 0,
                    prev ? prev[i - NC] : 0, initpx, row, col);
    diff[i] = (int16_t)(cur[i] - Px);
  }
}
#endif

/* Vectorized SSSS category (bit-length of |residual|, 0..16) for 8 residuals
 * at a time. |v| -> float32 (exact for <=16-bit) -> exponent field gives
 * floor(log2); ssss = exp-126, clamped to 0 for v==0. SSE2-only ops (abs and
 * the >=0 clamp are emulated), so this one kernel serves the SSE2/SSE4.1/AVX2
 * tiers. This replaces the scalar clz that dominated both encoder passes. */
#if defined(LJ92_HAVE_SSE2)
__attribute__((target("sse2"))) static void ssss_row_sse2(const int16_t* diff,
                                                          int n, u8* sbuf) {
  const __m128i bias = _mm_set1_epi32(126);
  const __m128i z = _mm_setzero_si128();
  int i = 0;
  for (; i + 8 <= n; i += 8) {
    __m128i v = _mm_loadu_si128((const __m128i*)(diff + i));
    __m128i sgn = _mm_srai_epi16(v, 15);
    __m128i a = _mm_sub_epi16(_mm_xor_si128(v, sgn), sgn);   /* |v| */
    __m128i lo = _mm_unpacklo_epi16(a, z);                   /* 4x u32 */
    __m128i hi = _mm_unpackhi_epi16(a, z);
    __m128i elo = _mm_srli_epi32(_mm_castps_si128(_mm_cvtepi32_ps(lo)), 23);
    __m128i ehi = _mm_srli_epi32(_mm_castps_si128(_mm_cvtepi32_ps(hi)), 23);
    elo = _mm_sub_epi32(elo, bias);
    ehi = _mm_sub_epi32(ehi, bias);
    elo = _mm_andnot_si128(_mm_srai_epi32(elo, 31), elo);    /* max(.,0) */
    ehi = _mm_andnot_si128(_mm_srai_epi32(ehi, 31), ehi);
    __m128i p16 = _mm_packs_epi32(elo, ehi);                 /* 8x u16 */
    __m128i p8 = _mm_packus_epi16(p16, p16);                 /* low 8 bytes */
    _mm_storel_epi64((__m128i*)(sbuf + i), p8);
  }
  for (; i < n; i++) sbuf[i] = (u8)enc_ssss(diff[i]);
}
#endif

#if defined(LJ92_HAVE_SSE2)
static int cpu_has_sse2(void) {
#if defined(LJ92_X86) && (defined(__GNUC__) || defined(__clang__))
  __builtin_cpu_init();
  return __builtin_cpu_supports("sse2");
#else
  return 0;
#endif
}
#endif
#if defined(LJ92_HAVE_SSE41)
static int cpu_has_sse41(void) {
#if defined(LJ92_X86) && (defined(__GNUC__) || defined(__clang__))
  __builtin_cpu_init();
  return __builtin_cpu_supports("sse4.1");
#else
  return 0;
#endif
}
#endif
#if defined(LJ92_HAVE_AVX2)
static int cpu_has_avx2(void) {
#if defined(LJ92_X86) && (defined(__GNUC__) || defined(__clang__))
  __builtin_cpu_init();
  return __builtin_cpu_supports("avx2");
#else
  return 0;
#endif
}
#endif

static void apply_simd_selection(lj92_simd want) {
  /* AUTO picks the best the CPU supports; an explicit request is honored only
   * if both the kernel was compiled in and the CPU supports it, else we fall
   * back to scalar (lj92_simd_force then reports the mismatch). The horizontal
   * prefix sum needs no pshufb, so SSE4.1 shares the SSE2 kernel; the SSE4.1
   * tier is still distinguished so dispatch/benchmarking can target it. */
  g_psum = psum_scalar;
  g_encdiff = enc_diff_row;
  g_ssss_row = ssss_row_scalar;
  g_simd_active = LJ92_SIMD_SCALAR;
  if (want == LJ92_SIMD_SCALAR) return;

#if defined(LJ92_HAVE_AVX2)
  if ((want == LJ92_SIMD_AUTO || want == LJ92_SIMD_AVX2) && cpu_has_avx2()) {
    g_psum = psum_avx2;
    g_encdiff = enc_diff_row_avx2;
    g_ssss_row = ssss_row_sse2;  /* 8-wide ssss already ~5x; AVX2 not needed */
    g_simd_active = LJ92_SIMD_AVX2;
    return;
  }
#endif
#if defined(LJ92_HAVE_SSE41)
  if (want == LJ92_SIMD_SSE41 && cpu_has_sse41()) {
    g_psum = psum_sse2;
    g_encdiff = enc_diff_row_sse2;
    g_ssss_row = ssss_row_sse2;
    g_simd_active = LJ92_SIMD_SSE41;
    return;
  }
#endif
#if defined(LJ92_HAVE_SSE2)
  if ((want == LJ92_SIMD_AUTO || want == LJ92_SIMD_SSE2 ||
       want == LJ92_SIMD_SSE41) &&
      cpu_has_sse2()) {
    g_psum = psum_sse2;
    g_encdiff = enc_diff_row_sse2;
    g_ssss_row = ssss_row_sse2;
    g_simd_active = LJ92_SIMD_SSE2;
    return;
  }
#endif
}

static void lj92_simd_init(void) {
  if (g_psum) return;  /* already initialized */
  apply_simd_selection(g_simd_forced ? g_simd_forced : LJ92_SIMD_AUTO);
}

const char* lj92_simd_name(void) {
  lj92_simd_init();
  switch (g_simd_active) {
    case LJ92_SIMD_AVX2: return "avx2";
    case LJ92_SIMD_SSE41: return "sse4.1";
    case LJ92_SIMD_SSE2: return "sse2";
    default: return "scalar";
  }
}

int lj92_simd_force(lj92_simd which) {
  g_psum = NULL;  /* force re-init */
  g_simd_forced = which;
  apply_simd_selection(which);
  /* Verify the request actually took (e.g. CPU lacks the feature). */
  if (which == LJ92_SIMD_AVX2 && g_simd_active != LJ92_SIMD_AVX2) return -1;
  if (which == LJ92_SIMD_SSE41 && g_simd_active != LJ92_SIMD_SSE41) return -1;
  if (which == LJ92_SIMD_SSE2 && g_simd_active != LJ92_SIMD_SSE2) return -1;
  return 0;
}
