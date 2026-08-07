/*
 * tiny_dng_j2k_enc.c - clean-room C11 JPEG 2000 codestream encoder for
 * HTJ2K (JPEG 2000 Part 15 / ITU-T T.814) streams.
 *
 * Produces a single-tile HTJ2K codestream (RPCL progression, one layer,
 * whole-image precincts, codeblock 2^cb_log_w x 2^cb_log_h) on top of the
 * clean-room HT block encoder (tiny_dng_htj2k.c). The output layout mirrors
 * the OpenJPH encoder so streams decode with both this library's decoder
 * and OpenJPH.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#include "tiny_dng_htj2k.h"
#include "tiny_dng_j2k.h"
#include "tiny_dng_j2k_enc.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int tdj_div_ceil(int a, int b) { return a <= 0 ? 0 : (a + b - 1) / b; }

static int tdj_log2ceil(uint32_t v) {
  int n = 0;
  v--;
  while (v) {
    n++;
    v >>= 1;
  }
  return n;
}

static int tdj_max_int(int a, int b) { return a > b ? a : b; }

static uint32_t tdj_clz32(uint32_t v) {
  uint32_t n = 0;
  if (v == 0) return 32;
  while (!(v & 0x80000000u)) {
    v <<= 1;
    n++;
  }
  return n;
}

/* ------------------------------------------------------------------ */
/* Growable byte buffer (big-endian writer)                            */
/* ------------------------------------------------------------------ */

typedef struct {
  uint8_t *b;
  size_t cap, len;
} ebuf;

static int ebuf_res(ebuf *e, size_t add) {
  if (e->len + add <= e->cap) return 1;
  size_t nc = e->cap ? e->cap * 2 : 4096;
  while (nc < e->len + add) nc *= 2;
  uint8_t *nb = (uint8_t *)realloc(e->b, nc);
  if (!nb) return 0;
  e->b = nb;
  e->cap = nc;
  return 1;
}

static void ebuf_u8(ebuf *e, uint8_t v) {
  if (ebuf_res(e, 1)) e->b[e->len++] = v;
}

static void ebuf_u16(ebuf *e, uint16_t v) {
  if (ebuf_res(e, 2)) {
    e->b[e->len++] = (uint8_t)(v >> 8);
    e->b[e->len++] = (uint8_t)v;
  }
}

static void ebuf_u32(ebuf *e, uint32_t v) {
  if (ebuf_res(e, 4)) {
    e->b[e->len++] = (uint8_t)(v >> 24);
    e->b[e->len++] = (uint8_t)(v >> 16);
    e->b[e->len++] = (uint8_t)(v >> 8);
    e->b[e->len++] = (uint8_t)v;
  }
}

static void ebuf_bytes(ebuf *e, const void *p, size_t n) {
  if (ebuf_res(e, n)) {
    memcpy(e->b + e->len, p, n);
    e->len += n;
  }
}

/* ------------------------------------------------------------------ */
/* Packet-header bit writer with 0xFF byte stuffing (MSB first)        */
/* ------------------------------------------------------------------ */

typedef struct {
  uint8_t *b;
  size_t cap, len;
  uint32_t tmp;
  int avail; /* bits remaining in tmp (8 = empty) */
} ebw;

static int ebw_alloc(ebw *w, size_t cap) {
  w->b = (uint8_t *)malloc(cap ? cap : 64);
  if (!w->b) return 0;
  w->cap = cap ? cap : 64;
  w->len = 0;
  w->tmp = 0;
  w->avail = 8;
  return 1;
}

static void ebw_byte(ebw *w, uint8_t v) {
  if (w->len + 1 > w->cap) {
    size_t nc = w->cap * 2;
    uint8_t *nb = (uint8_t *)realloc(w->b, nc);
    if (!nb) return;
    w->b = nb;
    w->cap = nc;
  }
  w->b[w->len++] = v;
}

/* MSB-first bit packing with the 0xFF handling used by the HTJ2K packet
   header: when a completed byte is 0xFF, the next byte's MSB is left as 0
   (only 7 bits available), which is what the decoder's unstuffing reads. */
static void ebw_bit(ebw *w, uint32_t bit) {
  --w->avail;
  w->tmp |= (bit & 1u) << w->avail;
  if (w->avail <= 0) {
    w->avail = 8 - (w->tmp == 0xFF ? 1 : 0);
    ebw_byte(w, (uint8_t)(w->tmp & 0xFF));
    w->tmp = 0;
  }
}

static void ebw_bits(ebw *w, uint32_t data, int n) {
  int i;
  for (i = n - 1; i >= 0; --i) ebw_bit(w, (data >> i) & 1u);
}

static void ebw_zeros(ebw *w, int n) {
  int i;
  for (i = 0; i < n; ++i) ebw_bit(w, 0);
}

static void ebw_term(ebw *w) {
  if (w->avail < 8) { /* partial byte pending */
    w->b[w->len++] = (uint8_t)(w->tmp & 0xFF);
    w->tmp = 0;
    w->avail = 8;
  }
}

/* ------------------------------------------------------------------ */
/* QCD exponent computation (reversible 5/3)                           */
/* ------------------------------------------------------------------ */

/* BIBO gains for the reversible 5/3 transform. */
static const float tdj_gain_5x3_l[34] = {
    1.0000e+00f, 1.5000e+00f, 1.6250e+00f, 1.6875e+00f, 1.6963e+00f,
    1.7067e+00f, 1.7116e+00f, 1.7129e+00f, 1.7141e+00f, 1.7145e+00f,
    1.7151e+00f, 1.7152e+00f, 1.7155e+00f, 1.7155e+00f, 1.7156e+00f,
    1.7156e+00f, 1.7156e+00f, 1.7156e+00f, 1.7156e+00f, 1.7156e+00f,
    1.7156e+00f, 1.7156e+00f, 1.7156e+00f, 1.7156e+00f, 1.7156e+00f,
    1.7156e+00f, 1.7156e+00f, 1.7156e+00f, 1.7156e+00f, 1.7156e+00f,
    1.7156e+00f, 1.7156e+00f, 1.7156e+00f, 1.7156e+00f};

static const float tdj_gain_5x3_h[34] = {
    2.0000e+00f, 2.5000e+00f, 2.7500e+00f, 2.8047e+00f, 2.8198e+00f,
    2.8410e+00f, 2.8558e+00f, 2.8601e+00f, 2.8628e+00f, 2.8656e+00f,
    2.8662e+00f, 2.8667e+00f, 2.8673e+00f, 2.8675e+00f, 2.8676e+00f,
    2.8677e+00f, 2.8678e+00f, 2.8678e+00f, 2.8679e+00f, 2.8679e+00f,
    2.8679e+00f, 2.8679e+00f, 2.8679e+00f, 2.8679e+00f, 2.8679e+00f,
    2.8679e+00f, 2.8679e+00f, 2.8679e+00f, 2.8679e+00f, 2.8679e+00f,
    2.8679e+00f, 2.8679e+00f, 2.8679e+00f, 2.8679e+00f};

/* BIBO gains for the irreversible 9/7 transform. */
static const float tdj_gain_9x7_l[34] = {
    1.0000e+00f, 1.3803e+00f, 1.3328e+00f, 1.3067e+00f, 1.3028e+00f,
    1.3001e+00f, 1.2993e+00f, 1.2992e+00f, 1.2992e+00f, 1.2992e+00f,
    1.2992e+00f, 1.2992e+00f, 1.2992e+00f, 1.2992e+00f, 1.2992e+00f,
    1.2992e+00f, 1.2992e+00f, 1.2992e+00f, 1.2992e+00f, 1.2992e+00f,
    1.2992e+00f, 1.2992e+00f, 1.2992e+00f, 1.2992e+00f, 1.2992e+00f,
    1.2992e+00f, 1.2992e+00f, 1.2992e+00f, 1.2992e+00f, 1.2992e+00f,
    1.2992e+00f, 1.2992e+00f, 1.2992e+00f, 1.2992e+00f};

static const float tdj_gain_9x7_h[34] = {
    1.2976e+00f, 1.3126e+00f, 1.2757e+00f, 1.2352e+00f, 1.2312e+00f,
    1.2285e+00f, 1.2280e+00f, 1.2278e+00f, 1.2278e+00f, 1.2278e+00f,
    1.2278e+00f, 1.2278e+00f, 1.2278e+00f, 1.2278e+00f, 1.2278e+00f,
    1.2278e+00f, 1.2278e+00f, 1.2278e+00f, 1.2278e+00f, 1.2278e+00f,
    1.2278e+00f, 1.2278e+00f, 1.2278e+00f, 1.2278e+00f, 1.2278e+00f,
    1.2278e+00f, 1.2278e+00f, 1.2278e+00f, 1.2278e+00f, 1.2278e+00f,
    1.2278e+00f, 1.2278e+00f, 1.2278e+00f, 1.2278e+00f};

/* sqrt-energy gains for the irreversible 9/7 transform. */
static const float tdj_sqrt_g_9x7_l[34] = {
    1.0000e+00f, 1.4021e+00f, 2.0304e+00f, 2.9012e+00f, 4.1153e+00f,
    5.8245e+00f, 8.2388e+00f, 1.1652e+01f, 1.6479e+01f, 2.3304e+01f,
    3.2957e+01f, 4.6609e+01f, 6.5915e+01f, 9.3217e+01f, 1.3183e+02f,
    1.8643e+02f, 2.6366e+02f, 3.7287e+02f, 5.2732e+02f, 7.4574e+02f,
    1.0546e+03f, 1.4915e+03f, 2.1093e+03f, 2.9830e+03f, 4.2185e+03f,
    5.9659e+03f, 8.4371e+03f, 1.1932e+04f, 1.6874e+04f, 2.3864e+04f,
    3.3748e+04f, 4.7727e+04f, 6.7496e+04f, 9.5454e+04f};
static const float tdj_sqrt_g_9x7_h[34] = {
    1.4425e+00f, 1.9669e+00f, 2.8839e+00f, 4.1475e+00f, 5.8946e+00f,
    8.3472e+00f, 1.1809e+01f, 1.6701e+01f, 2.3620e+01f, 3.3403e+01f,
    4.7240e+01f, 6.6807e+01f, 9.4479e+01f, 1.3361e+02f, 1.8896e+02f,
    2.6723e+02f, 3.7792e+02f, 5.3446e+02f, 7.5583e+02f, 1.0689e+03f,
    1.5117e+03f, 2.1378e+03f, 3.0233e+03f, 4.2756e+03f, 6.0467e+03f,
    8.5513e+03f, 1.2093e+04f, 1.7103e+04f, 2.4187e+04f, 3.4205e+04f,
    4.8373e+04f, 6.8410e+04f, 9.6747e+04f, 1.3682e+05f};

/* ------------------------------------------------------------------ */
/* Subband storage                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
  int32_t *s;
  int w, h;
} tdj_sb;

static int tdj_sb_alloc(tdj_sb *b, int w, int h) {
  b->s = (int32_t *)calloc((size_t)(w > 0 ? w : 1) * (h > 0 ? h : 1),
                           sizeof(int32_t));
  b->w = w;
  b->h = h;
  return b->s != NULL;
}

static void tdj_sb_free(tdj_sb *b) {
  free(b->s);
  b->s = NULL;
}

/* ------------------------------------------------------------------ */
/* Forward wavelet (even phase)                                        */
/* ------------------------------------------------------------------ */

/* Reversible 5/3 analysis on a contiguous array of length `w`. The input
   is copied into `work`; on return the low-pass coefficients are in
   `out[0..lw)` and high-pass in `out[lw..w)` (lw = ceil(w/2)). */
static void tdj_fwd_53_1d_even(int32_t *out, const int32_t *src, int w,
                               int32_t *work) {
  int lw = (w + 1) >> 1;
  int hw = w >> 1;
  int i;
  if (w == 1) { /* trivial: no filtering for a single sample */
    out[0] = src[0];
    return;
  }
  memcpy(work, src, (size_t)w * sizeof(int32_t));
  /* predict: high[i] -= (low[i] + low[i+1]) >> 1 */
  for (i = 0; i < hw; ++i) {
    int32_t lo = work[2 * i];
    int32_t lo_next = (2 * i + 2 < w) ? work[2 * i + 2] : work[2 * i];
    work[2 * i + 1] -= (lo + lo_next) >> 1;
  }
  /* update: low[i] += (high[i-1] + high[i] + 2) >> 2 */
  for (i = 0; i < lw; ++i) {
    int32_t hi_prev = (2 * i - 1 >= 0) ? work[2 * i - 1] : work[1];
    int32_t hi = (2 * i + 1 < w) ? work[2 * i + 1] : work[w - 2];
    work[2 * i] += (hi_prev + hi + 2) >> 2;
  }
  for (i = 0; i < lw; ++i) out[i] = work[2 * i];
  for (i = 0; i < hw; ++i) out[lw + i] = work[2 * i + 1];
}

/* Irreversible 9/7 analysis on a contiguous int32 array. `scratch` is float
   scratch of size 3*w: [0..w) holds the float source, [w..2w) the low-pass
   and [2w..3w) the high-pass during lifting. Results go to out_l (lw) and
   out_h (hw) as floats. */
static void tdj_fwd_97_1d_even(float *out_l, float *out_h, const int32_t *src,
                               int w, float *scratch) {
  static const float a4 = (float)-1.586134342059924; /* alpha (predict)  */
  static const float a3 = (float)-0.052980118572961; /* beta  (update)   */
  static const float a2 = (float)0.882911075530934;  /* gamma (predict)  */
  static const float a1 = (float)0.443506852043971;  /* delta (update)   */
  static const float K = (float)1.230174104914001;
  int lw = (w + 1) >> 1;
  int hw = w >> 1;
  float *lp, *hp;
  int i;
  for (i = 0; i < w; ++i) scratch[i] = (float)src[i];
  lp = scratch + (size_t)w;
  hp = scratch + 2 * (size_t)w;
  for (i = 0; i < lw; ++i) lp[i] = scratch[2 * i];
  for (i = 0; i < hw; ++i) hp[i] = scratch[2 * i + 1];
  /* step alpha (even): hp[i] += a4*(lp[i] + lp[i+1]) */
  {
    float ex = lp[lw - 1];
    for (i = 0; i < hw; ++i) {
      float s0 = lp[i], s1 = (i + 1 < lw) ? lp[i + 1] : ex;
      hp[i] += a4 * (s0 + s1);
    }
  }
  /* step beta (odd): lp[i] += a3*(hp[i-1] + hp[i]) */
  for (i = 0; i < lw; ++i) {
    float h0 = (i > 0) ? hp[i - 1] : hp[0];
    float h1 = (i < hw) ? hp[i] : hp[hw - 1];
    lp[i] += a3 * (h0 + h1);
  }
  /* step gamma (even): hp[i] += a2*(lp[i] + lp[i+1]) */
  {
    float ex = lp[lw - 1];
    for (i = 0; i < hw; ++i) {
      float s0 = lp[i], s1 = (i + 1 < lw) ? lp[i + 1] : ex;
      hp[i] += a2 * (s0 + s1);
    }
  }
  /* step delta (odd): lp[i] += a1*(hp[i-1] + hp[i]) */
  for (i = 0; i < lw; ++i) {
    float h0 = (i > 0) ? hp[i - 1] : hp[0];
    float h1 = (i < hw) ? hp[i] : hp[hw - 1];
    lp[i] += a1 * (h0 + h1);
  }
  for (i = 0; i < lw; ++i) out_l[i] = lp[i] * (1.0f / K);
  for (i = 0; i < hw; ++i) out_h[i] = hp[i] * K;
}

/* Same analysis on a float source array. `scratch` is float scratch of
   size 2*w (low/high split). */
static void tdj_fwd_97_1d_even_f(float *out_l, float *out_h, const float *src,
                                 int w, float *scratch) {
  static const float a4 = (float)-1.586134342059924;
  static const float a3 = (float)-0.052980118572961;
  static const float a2 = (float)0.882911075530934;
  static const float a1 = (float)0.443506852043971;
  static const float K = (float)1.230174104914001;
  int lw = (w + 1) >> 1;
  int hw = w >> 1;
  float *lp, *hp;
  int i;
  lp = scratch;
  hp = scratch + (size_t)w;
  for (i = 0; i < lw; ++i) lp[i] = src[2 * i];
  for (i = 0; i < hw; ++i) hp[i] = src[2 * i + 1];
  {
    float ex = lp[lw - 1];
    for (i = 0; i < hw; ++i) {
      float s0 = lp[i], s1 = (i + 1 < lw) ? lp[i + 1] : ex;
      hp[i] += a4 * (s0 + s1);
    }
  }
  for (i = 0; i < lw; ++i) {
    float h0 = (i > 0) ? hp[i - 1] : hp[0];
    float h1 = (i < hw) ? hp[i] : hp[hw - 1];
    lp[i] += a3 * (h0 + h1);
  }
  {
    float ex = lp[lw - 1];
    for (i = 0; i < hw; ++i) {
      float s0 = lp[i], s1 = (i + 1 < lw) ? lp[i + 1] : ex;
      hp[i] += a2 * (s0 + s1);
    }
  }
  for (i = 0; i < lw; ++i) {
    float h0 = (i > 0) ? hp[i - 1] : hp[0];
    float h1 = (i < hw) ? hp[i] : hp[hw - 1];
    lp[i] += a1 * (h0 + h1);
  }
  for (i = 0; i < lw; ++i) out_l[i] = lp[i] * (1.0f / K);
  for (i = 0; i < hw; ++i) out_h[i] = hp[i] * K;
}

/* One 2D analysis level on `src` (w x h). Outputs (row filter first, then
   column filter, matching OpenJPH's rounding):
   ll: low rows, low cols (ceil(w/2) x ceil(h/2))
   hl: low rows, high cols (floor(w/2) x ceil(h/2))   [packet band 1]
   lh: high rows, low cols (ceil(w/2) x floor(h/2))   [packet band 2]
   hh: high rows, high cols (floor(w/2) x floor(h/2)) [packet band 3]
   `work` is int32 scratch >= 3*max(w,h), `fwork` float scratch
   >= 5*max(w,h). */
static int tdj_fwd_2d(tdj_sb *ll, tdj_sb *hl, tdj_sb *lh, tdj_sb *hh,
                      const int32_t *src, int w, int h, int reversible,
                      int32_t *work, float *fwork) {
  int lw = (w + 1) >> 1;
  int hw = w >> 1;
  int lh_ = (h + 1) >> 1;
  int hh_ = h >> 1;
  int32_t *tmp;
  int x, y;
  if (!tdj_sb_alloc(ll, lw, lh_) || !tdj_sb_alloc(hl, hw, lh_) ||
      !tdj_sb_alloc(lh, lw, hh_) || !tdj_sb_alloc(hh, hw, hh_))
    return 0;
  tmp = (int32_t *)malloc((size_t)w * h * sizeof(int32_t));
  if (!tmp) return 0;
  /* row filter first: split each column into low rows (top) and high rows
     (bottom). tmp holds [low rows (lh_ x w) | high rows (hh_ x w)]. */
  if (reversible) {
    for (x = 0; x < w; ++x) {
      int32_t *col = work;
      int32_t *res = work + (size_t)h;
      for (y = 0; y < h; ++y) col[y] = src[(size_t)y * w + x];
      tdj_fwd_53_1d_even(res, col, h, work + 2 * (size_t)h);
      for (y = 0; y < lh_; ++y) tmp[(size_t)y * w + x] = res[y];
      for (y = 0; y < hh_; ++y) tmp[(size_t)(lh_ + y) * w + x] = res[lh_ + y];
    }
  } else {
    float *col = fwork;
    int32_t *icol = work;
    for (x = 0; x < w; ++x) {
      float *fl = col;
      float *fh = col + (size_t)h;
      for (y = 0; y < h; ++y) icol[y] = src[(size_t)y * w + x];
      tdj_fwd_97_1d_even(fl, fh, icol, h, col + 2 * (size_t)h);
      for (y = 0; y < lh_; ++y) tmp[(size_t)y * w + x] = (int32_t)lrintf(fl[y]);
      for (y = 0; y < hh_; ++y)
        tmp[(size_t)(lh_ + y) * w + x] = (int32_t)lrintf(fh[y]);
    }
  }
  /* column filter: split each low-pass row into ll (low cols) + hl (high
     cols), each high-pass row into lh (low cols) + hh (high cols). */
  if (reversible) {
    for (y = 0; y < lh_; ++y) {
      tdj_fwd_53_1d_even(work, tmp + (size_t)y * w, w, work + (size_t)w);
      for (x = 0; x < lw; ++x) ll->s[(size_t)y * lw + x] = work[x];
      for (x = 0; x < hw; ++x) hl->s[(size_t)y * hw + x] = work[lw + x];
    }
    for (y = 0; y < hh_; ++y) {
      tdj_fwd_53_1d_even(work, tmp + (size_t)(lh_ + y) * w, w,
                         work + (size_t)w);
      for (x = 0; x < lw; ++x) lh->s[(size_t)y * lw + x] = work[x];
      for (x = 0; x < hw; ++x) hh->s[(size_t)y * hw + x] = work[lw + x];
    }
  } else {
    /* the lossy path is handled by tdj_fwd_2d_irv; this branch should not
       be reached (reversible is 1 whenever this function is called). */
    return 0;
  }
  free(tmp);
  return 1;
}

/* Irreversible 9/7 2D analysis, whole-image tile. `src` is float (w x h).
   The low-low output is written to `ll` (float, becomes the next level's
   input); the high subbands are quantized to int32 with the per-band
   delta_inv `qdelta` (indexed 0=LL,1=HL,2=LH,3=HH). If `quant_ll` is set,
   the low-low is also quantized (final LL(0)); otherwise left as float in
   `ll`. scratch: `fwork` needs 5*max(w,h) floats, `iwork` needs 3*max(w,h)
   ints. */
static int tdj_fwd_2d_irv(tdj_sb *hl, tdj_sb *lh, tdj_sb *hh, float *ll,
                          const float *src, int w, int h, int quant_ll,
                          const float *qdelta, int32_t *iwork, float *fwork) {
  int lw = (w + 1) >> 1;
  int hw = w >> 1;
  int lh_ = (h + 1) >> 1;
  int hh_ = h >> 1;
  float *tmp;
  int x, y;
  if (!tdj_sb_alloc(hl, hw, lh_) || !tdj_sb_alloc(lh, lw, hh_) ||
      !tdj_sb_alloc(hh, hw, hh_))
    return 0;
  tmp = (float *)malloc((size_t)w * h * sizeof(float));
  if (!tmp) return 0;
  /* row filter on float columns: low rows to the top half, high rows to the
     bottom half. The low rows will become LL (low cols) + HL (high cols),
     the high rows LH (low cols) + HH (high cols). Quantization is deferred
     to the column-filter stage so it applies per final subband. */
  {
    float *col = fwork;
    float *res = fwork + (size_t)h;
    for (x = 0; x < w; ++x) {
      float *fl = col;
      float *fh = res;
      for (y = 0; y < h; ++y) col[y] = src[(size_t)y * w + x];
      tdj_fwd_97_1d_even_f(fl, fh, col, h, col + 2 * (size_t)h);
      for (y = 0; y < lh_; ++y) tmp[(size_t)y * w + x] = fl[y];
      for (y = 0; y < hh_; ++y) tmp[(size_t)(lh_ + y) * w + x] = fh[y];
    }
  }
  /* column filter on float rows, quantizing each subband */
  {
    float *fl = fwork;
    float *fh = fwork + (size_t)w;
    float *row = fwork + 2 * (size_t)w;
    float *llr = fwork + 3 * (size_t)w;
    for (y = 0; y < lh_; ++y) {
      tdj_fwd_97_1d_even_f(fl, fh, tmp + (size_t)y * w, w, row);
      for (x = 0; x < lw; ++x) llr[x] = fl[x];
      for (x = 0; x < hw; ++x) hl->s[(size_t)y * hw + x] =
          (int32_t)(fh[x] * qdelta[1]);
      if (quant_ll)
        for (x = 0; x < lw; ++x) ll[(size_t)y * lw + x] =
            (float)(int32_t)(fl[x] * qdelta[0]);
      else
        for (x = 0; x < lw; ++x) ll[(size_t)y * lw + x] = llr[x];
    }
    for (y = 0; y < hh_; ++y) {
      tdj_fwd_97_1d_even_f(fl, fh, tmp + (size_t)(lh_ + y) * w, w, row);
      for (x = 0; x < lw; ++x) lh->s[(size_t)y * lw + x] =
          (int32_t)(fl[x] * qdelta[2]);
      for (x = 0; x < hw; ++x) hh->s[(size_t)y * hw + x] =
          (int32_t)(fh[x] * qdelta[3]);
    }
  }
  free(tmp);
  return 1;
}

/* ------------------------------------------------------------------ */
/* Per-component subband collection                                    */
/* ------------------------------------------------------------------ */

/* Holds every subband for one component:
   sb[0]            = LL(0)
   sb[(r-1)*3+b]    = subband b (1=HL,2=LH,3=HH) at resolution r (1..nd). */
typedef struct {
  int num_decomps;
  tdj_sb *sb;
} tdj_comp_sb;

static void tdj_comp_sb_free(tdj_comp_sb *csb) {
  int n = 1 + 3 * csb->num_decomps;
  int i;
  if (!csb->sb) return;
  for (i = 0; i < n; ++i) tdj_sb_free(&csb->sb[i]);
  free(csb->sb);
  csb->sb = NULL;
}

/* ------------------------------------------------------------------ */
/* Tag trees                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
  int w, h;
  int nlevels;
  int *val;
  uint8_t *flg;
} tt;

static int tt_off(const tt *t, int level, int x, int y) {
  int off = 0, l;
  if (level >= t->nlevels) { /* virtual root */
    for (l = 0; l < t->nlevels; ++l)
      off += tdj_div_ceil(t->w, 1 << l) * tdj_div_ceil(t->h, 1 << l);
    return off;
  }
  for (l = 0; l < level; ++l)
    off += tdj_div_ceil(t->w, 1 << l) * tdj_div_ceil(t->h, 1 << l);
  return off + y * tdj_div_ceil(t->w, 1 << level) + x;
}

static int tt_size(const tt *t) {
  int total = 0, l;
  for (l = 0; l <= t->nlevels; ++l)
    total += tdj_div_ceil(t->w, 1 << l) * tdj_div_ceil(t->h, 1 << l);
  return total;
}

static void tt_build(tt *t) {
  int lev;
  for (lev = 1; lev < t->nlevels; ++lev) {
    int ch_h = tdj_div_ceil(t->h, 1 << (lev - 1));
    int ch_w = tdj_div_ceil(t->w, 1 << (lev - 1));
    int h = tdj_div_ceil(t->h, 1 << lev);
    int w = tdj_div_ceil(t->w, 1 << lev);
    int y, x;
    for (y = 0; y < h; ++y)
      for (x = 0; x < w; ++x) {
        int m = 0x7FFFFFFF;
        int dy, dx;
        for (dy = 0; dy < 2; ++dy)
          for (dx = 0; dx < 2; ++dx) {
            int cx = 2 * x + dx, cy = 2 * y + dy;
            int v;
            if (cx >= ch_w || cy >= ch_h) continue;
            v = t->val[tt_off(t, lev - 1, cx, cy)];
            if (v < m) m = v;
          }
        t->val[tt_off(t, lev, x, y)] = m;
      }
  }
}

/* ------------------------------------------------------------------ */
/* Codeblock encode + packet writing                                   */
/* ------------------------------------------------------------------ */

typedef struct {
  uint8_t *data;
  uint32_t len;
  int empty; /* 1 if no encoded data */
} tdj_ecb;

/* Encode all codeblocks of a subband into `blocks` (nc_x x nc_y). Returns 1.
   kmax is the subband K_max (missing_msbs = kmax-1). For lossy (reversible=0)
   the samples are already quantized integers in |v| (no MSB shift). */
static int tdj_encode_subband(tdj_ecb *blocks, int nc_x, int nc_y,
                              const tdj_sb *sb, int cb_w, int cb_h, int kmax,
                              int reversible) {
  int bx, by;
  uint32_t missing = (uint32_t)(kmax - 1);
  uint32_t shift = 31u - (uint32_t)kmax;
  for (by = 0; by < nc_y; ++by) {
    for (bx = 0; bx < nc_x; ++bx) {
      int x0 = bx * cb_w, y0 = by * cb_h;
      int bw = (x0 + cb_w <= sb->w) ? cb_w : sb->w - x0;
      int bh = (y0 + cb_h <= sb->h) ? cb_h : sb->h - y0;
      uint32_t *coef;
      uint32_t *dp;
      int x, y;
      uint32_t max_mag = 0;
      tdj_ecb *blk = &blocks[by * nc_x + bx];
      blk->data = NULL;
      blk->len = 0;
      blk->empty = 1;
      /* the codeblock size is the actual subband region (bw x bh), not the
         padded codeblock grid (OpenJPH uses the true size for edge blocks) */
      coef = (uint32_t *)calloc((size_t)bw * bh, sizeof(uint32_t));
      if (!coef) return 0;
      dp = coef;
      for (y = 0; y < bh; ++y)
        for (x = 0; x < bw; ++x, ++dp) {
          int32_t v = sb->s[(size_t)(y0 + y) * sb->w + (x0 + x)];
          uint32_t sign = v < 0 ? 0x80000000u : 0u;
          uint32_t mag = (uint32_t)(v < 0 ? -v : v);
          *dp = sign | (reversible ? (mag << shift) : mag);
          max_mag |= mag;
        }
      if (max_mag > 0) {
        uint32_t lengths[3] = {0, 0, 0};
        uint8_t *enc = NULL;
        size_t enc_cap = 0;
        int r = tdng_htj2k_encode_codeblock32(coef, missing, (uint32_t)bw,
                                              (uint32_t)bh, (uint32_t)bw,
                                              lengths, &enc, &enc_cap);
        if (r == TDNG_HTJ2K_OK && lengths[0] > 0) {
          blk->data = enc;
          blk->len = lengths[0];
          blk->empty = 0;
        } else {
          free(enc);
        }
      }
      free(coef);
    }
  }
  return 1;
}

/* Write the packet header for one subband into `w`. `blocks` holds
   nc_x x nc_y encoded blocks. `*coded` is 0 before any non-empty subband
   in the packet; `*num_skipped` counts empty subbands seen so far. */
static void tdj_packet_subband(ebw *w, const tdj_ecb *blocks, int nc_x,
                               int nc_y, uint32_t mmsbs_value, int *coded,
                               int *num_skipped) {
  tt inc, msb;
  int x, y, lev;
  inc.w = nc_x;
  inc.h = nc_y;
  inc.nlevels = 1 + tdj_max_int(tdj_log2ceil((uint32_t)tdj_max_int(nc_x, 1)),
                                tdj_log2ceil((uint32_t)tdj_max_int(nc_y, 1)));
  msb = inc;
  inc.val = (int *)calloc((size_t)tt_size(&inc) + 1, sizeof(int));
  inc.flg = (uint8_t *)calloc((size_t)tt_size(&inc) + 1, 1);
  msb.val = (int *)calloc((size_t)tt_size(&msb) + 1, sizeof(int));
  msb.flg = (uint8_t *)calloc((size_t)tt_size(&msb) + 1, 1);
  if (!inc.val || !inc.flg || !msb.val || !msb.flg) goto done;
  for (y = 0; y < nc_y; ++y)
    for (x = 0; x < nc_x; ++x) {
      const tdj_ecb *b = &blocks[y * nc_x + x];
      inc.val[tt_off(&inc, 0, x, y)] = b->empty ? 1 : 0;
      msb.val[tt_off(&msb, 0, x, y)] = (int)mmsbs_value;
    }
  tt_build(&inc);
  tt_build(&msb);
  inc.val[tt_off(&inc, inc.nlevels, 0, 0)] = 0;
  msb.val[tt_off(&msb, msb.nlevels, 0, 0)] = 0;

  if (inc.val[tt_off(&inc, inc.nlevels - 1, 0, 0)] != 0) {
    /* whole subband empty */
    if (*coded)
      ebw_bit(w, 0);
    else
      ++*num_skipped;
    goto done;
  }
  if (*coded == 0) {
    ebw_bit(w, 1);
    ebw_zeros(w, *num_skipped);
    *num_skipped = 0;
    *coded = 1;
  }
  for (y = 0; y < nc_y; ++y)
    for (x = 0; x < nc_x; ++x) {
      const tdj_ecb *b = &blocks[y * nc_x + x];
      for (lev = inc.nlevels; lev > 0; --lev) {
        int idx_lo = tt_off(&inc, lev - 1, x >> (lev - 1), y >> (lev - 1));
        int idx_hi = tt_off(&inc, lev, x >> lev, y >> lev);
        if (!inc.flg[idx_lo]) {
          int skipped = inc.val[idx_lo] - inc.val[idx_hi];
          ebw_bits(w, (uint32_t)(1 - skipped), 1);
          inc.flg[idx_lo] = 1;
        }
        if (inc.val[idx_lo] > 0) break;
      }
      if (b->empty) continue;
      for (lev = msb.nlevels; lev > 0; --lev) {
        int idx_lo = tt_off(&msb, lev - 1, x >> (lev - 1), y >> (lev - 1));
        int idx_hi = tt_off(&msb, lev, x >> lev, y >> lev);
        if (!msb.flg[idx_lo]) {
          int num_zeros = msb.val[idx_lo] - msb.val[idx_hi];
          ebw_zeros(w, num_zeros);
          ebw_bit(w, 1);
          msb.flg[idx_lo] = 1;
        }
      }
      ebw_bit(w, 0); /* num passes: 1 (cleanup only) */
      {
        int bits1 = 32 - (int)tdj_clz32(b->len);
        int bits = bits1 - 3;
        if (bits < 0) bits = 0;
        ebw_bits(w, 0xFFFFFFFEu, bits + 1);
        ebw_bits(w, b->len, bits + 3);
      }
    }
done:
  free(inc.val);
  free(inc.flg);
  free(msb.val);
  free(msb.flg);
}

/* ------------------------------------------------------------------ */
/* Main encoder                                                        */
/* ------------------------------------------------------------------ */

int tdng_j2k_encode(const int32_t *pixels, int width, int height,
                    int num_comps, const int bits[8], const int is_signed[8],
                    int num_decomps, int reversible, int mct, int cb_log_w,
                    int cb_log_h, float qstep, uint8_t **out, size_t *out_cap,
                    size_t *out_len) {
  ebuf e;
  int cb_w, cb_h;
  int c;
  int ret = TDNG_J2K_OK;
  tdj_comp_sb *comp_sbs = NULL;
  int *kmax_arr = NULL;
  uint8_t qcd_bytes[98];
  uint16_t qcd_u16[98];
  int qcd_count, qcd_guard;
  int qcd_irrev = 0;
  float qdelta_inv[98]; /* 1/(reconstructed delta / 2^(31-K_max)) per subband */
  size_t sot_pos = 0;
  int32_t **planes = NULL;

  memset(&e, 0, sizeof(e));
  if (!pixels || width <= 0 || height <= 0 || num_comps <= 0 || num_comps > 8)
    return TDNG_J2K_ERROR_PARAM;
  if (num_decomps < 0 || num_decomps > 32) return TDNG_J2K_ERROR_PARAM;
  if (cb_log_w < 2 || cb_log_h < 2 || cb_log_w > 8 || cb_log_h > 8 ||
      cb_log_w + cb_log_h > 12)
    return TDNG_J2K_ERROR_PARAM;
  for (c = 0; c < num_comps; ++c)
    if (bits[c] < 1 || bits[c] > 31) return TDNG_J2K_ERROR_PARAM;
  cb_w = 1 << cb_log_w;
  cb_h = 1 << cb_log_h;
  if (mct && num_comps < 3) mct = 0;

  /* ---- QCD ---- */
  {
    int nd = num_decomps;
    int s, d;
    qcd_count = 1 + 3 * nd;
    if (qcd_count > 97) return TDNG_J2K_ERROR_PARAM;
    if (reversible) {
      int B = bits[0];
      int max_B_plus_X = 0;
      const float *gl = tdj_gain_5x3_l;
      const float *gh = tdj_gain_5x3_h;
      for (c = 0; c < num_comps; ++c)
        if (bits[c] > B) B = bits[c];
      if (mct) B += 1;
      s = 0;
      {
        double bibo_l = gl[nd];
        double X = ceil(log(bibo_l * bibo_l) / M_LN2);
        qcd_bytes[s] = (uint8_t)(B + (int)X);
        max_B_plus_X = (int)(B + X);
      }
      for (d = nd; d > 0; --d) {
        double bibo_l = gl[d];
        double bibo_h = gh[d - 1];
        double X = ceil(log(bibo_h * bibo_l) / M_LN2);
        qcd_bytes[++s] = (uint8_t)(B + (int)X);
        if (B + (int)X > max_B_plus_X) max_B_plus_X = (int)(B + X);
        X = ceil(log(bibo_h * bibo_h) / M_LN2);
        qcd_bytes[++s] = (uint8_t)(B + (int)X);
        if (B + (int)X > max_B_plus_X) max_B_plus_X = (int)(B + X);
        X = ceil(log(bibo_h * bibo_l) / M_LN2);
        qcd_bytes[++s] = (uint8_t)(B + (int)X);
        if (B + (int)X > max_B_plus_X) max_B_plus_X = (int)(B + X);
      }
      qcd_guard = tdj_max_int(1, max_B_plus_X - 31);
      if (max_B_plus_X > 38) return TDNG_J2K_ERROR_PARAM;
      for (s = 0; s < qcd_count; ++s)
        qcd_bytes[s] = (uint8_t)((qcd_bytes[s] - qcd_guard) << 3);
    } else {
      /* irreversible: scalar expounded, one guard bit */
      float delta_ref = qstep > 0.0f ? qstep : (1.0f / (float)(1 << 8));
      float arr[4] = {1.0f, 2.0f, 2.0f, 4.0f};
      qcd_irrev = 1;
      qcd_guard = 1;
      s = 0;
      /* LL */
      {
        float gain_l = tdj_sqrt_g_9x7_l[nd];
        float delta = delta_ref / (gain_l * gain_l);
        int exp = 0, mantissa;
        while (delta < 1.0f) { exp++; delta *= 2.0f; }
        mantissa = (int)roundf(delta * 2048.0f) - 2048;
        if (mantissa >= 2048) mantissa = 0x7FF;
        qcd_u16[s] = (uint16_t)((exp << 11) | (mantissa & 0x7FF));
      }
      for (d = nd; d > 0; --d) {
        float gain_l = tdj_sqrt_g_9x7_l[d];
        float gain_h = tdj_sqrt_g_9x7_h[d - 1];
        float delta;
        int exp, mantissa;
        delta = delta_ref / (gain_h * gain_l);
        exp = 0;
        while (delta < 1.0f) { exp++; delta *= 2.0f; }
        mantissa = (int)roundf(delta * 2048.0f) - 2048;
        if (mantissa >= 2048) mantissa = 0x7FF;
        qcd_u16[++s] = (uint16_t)((exp << 11) | (mantissa & 0x7FF));
        delta = delta_ref / (gain_l * gain_h);
        exp = 0;
        while (delta < 1.0f) { exp++; delta *= 2.0f; }
        mantissa = (int)roundf(delta * 2048.0f) - 2048;
        if (mantissa >= 2048) mantissa = 0x7FF;
        qcd_u16[++s] = (uint16_t)((exp << 11) | (mantissa & 0x7FF));
        delta = delta_ref / (gain_h * gain_h);
        exp = 0;
        while (delta < 1.0f) { exp++; delta *= 2.0f; }
        mantissa = (int)roundf(delta * 2048.0f) - 2048;
        if (mantissa >= 2048) mantissa = 0x7FF;
        qcd_u16[++s] = (uint16_t)((exp << 11) | (mantissa & 0x7FF));
      }
      (void)arr;
    }
  }

  kmax_arr = (int *)calloc((size_t)(1 + 3 * num_decomps), sizeof(int));
  if (!kmax_arr) return TDNG_J2K_ERROR_MEMORY;
  for (c = 0; c < 1 + 3 * num_decomps; ++c) {
    if (qcd_irrev) {
      int exp = qcd_u16[c] >> 11;
      kmax_arr[c] = (exp - 1) + qcd_guard; /* = exp */
      /* reconstructed delta and delta_inv = 2^(31-K_max) / delta_recon */
      {
        float mantissa = (float)((qcd_u16[c] & 0x7FF) | 0x800);
        float arr[4] = {1.0f, 2.0f, 2.0f, 4.0f};
        int band = c == 0 ? 0 : ((c - 1) % 3) + 1;
        float delta_recon = mantissa * arr[band] / 2048.0f /
                            (float)(1u << exp);
        int kmax = kmax_arr[c];
        qdelta_inv[c] = (float)(1u << (31 - kmax)) / delta_recon;
      }
    } else {
      int byte = qcd_bytes[c];
      int nb = byte >> 3;
      nb = nb == 0 ? 0 : nb - 1;
      kmax_arr[c] = nb + qcd_guard;
      qdelta_inv[c] = 0.0f;
    }
  }

  comp_sbs = (tdj_comp_sb *)calloc((size_t)num_comps, sizeof(tdj_comp_sb));
  if (!comp_sbs) {
    ret = TDNG_J2K_ERROR_MEMORY;
    goto fail;
  }
  for (c = 0; c < num_comps; ++c) comp_sbs[c].num_decomps = num_decomps;

  /* ---- per-component spatial planes (DC shifted) + forward transform ---- */
  {
    int max_side = tdj_max_int(width, height);
    int32_t *work = (int32_t *)malloc((size_t)3 * max_side * sizeof(int32_t));
    float *fwork = (float *)malloc((size_t)5 * max_side * sizeof(float));
    int x, y;
    planes = (int32_t **)calloc((size_t)num_comps, sizeof(int32_t *));
    if (!planes || !work || !fwork) {
      ret = TDNG_J2K_ERROR_MEMORY;
      free(work);
      free(fwork);
      goto fail;
    }
    for (c = 0; c < num_comps; ++c) {
      planes[c] = (int32_t *)malloc((size_t)width * height * sizeof(int32_t));
      if (!planes[c]) {
        ret = TDNG_J2K_ERROR_MEMORY;
        free(work);
        free(fwork);
        goto fail;
      }
    }
    /* DC level shift */
    for (c = 0; c < num_comps; ++c) {
      int32_t dcshift = is_signed[c] ? 0 : (1 << (bits[c] - 1));
      const int32_t *src = pixels + c;
      int32_t *dst = planes[c];
      for (y = 0; y < height; ++y) {
        const int32_t *row = src + (size_t)y * width * num_comps;
        for (x = 0; x < width; ++x)
          dst[(size_t)y * width + x] = row[x * num_comps] - dcshift;
      }
    }
    /* forward colour transform: reversible RCT (int) or irreversible ICT
       (float) on the first three components' spatial planes */
    if (reversible) {
      if (mct && num_comps >= 3) {
        int32_t *r = planes[0], *g = planes[1], *b = planes[2];
        for (y = 0; y < height; ++y)
          for (x = 0; x < width; ++x) {
            int32_t rr = r[(size_t)y * width + x];
            int32_t gg = g[(size_t)y * width + x];
            int32_t bb = b[(size_t)y * width + x];
            r[(size_t)y * width + x] = (int32_t)(((int64_t)rr +
                                                   (int64_t)gg * 2 + bb) >> 2);
            g[(size_t)y * width + x] = bb - gg;
            b[(size_t)y * width + x] = rr - gg;
          }
      }
      /* forward transform per component (reversible 5/3) */
      for (c = 0; c < num_comps; ++c) {
        if (num_decomps == 0) {
          tdj_sb *ll = (tdj_sb *)calloc(1, sizeof(tdj_sb));
          if (!ll || !tdj_sb_alloc(ll, width, height)) {
            ret = TDNG_J2K_ERROR_MEMORY;
            if (ll) { tdj_sb_free(ll); free(ll); }
            free(work);
            free(fwork);
            goto fail;
          }
          memcpy(ll->s, planes[c], (size_t)width * height * sizeof(int32_t));
          comp_sbs[c].sb = ll;
          continue;
        }
        comp_sbs[c].sb = (tdj_sb *)calloc((size_t)(1 + 3 * num_decomps),
                                         sizeof(tdj_sb));
        if (!comp_sbs[c].sb) {
          ret = TDNG_J2K_ERROR_MEMORY;
          free(work);
          free(fwork);
          goto fail;
        }
        {
          int32_t *cur = planes[c];
          int cur_w = width, cur_h = height;
          int d;
          for (d = num_decomps; d >= 1; --d) {
            tdj_sb ll, hl, lh, hh;
            memset(&ll, 0, sizeof(ll));
            memset(&hl, 0, sizeof(hl));
            memset(&lh, 0, sizeof(lh));
            memset(&hh, 0, sizeof(hh));
            if (!tdj_fwd_2d(&ll, &hl, &lh, &hh, cur, cur_w, cur_h, 1, work,
                            fwork)) {
              ret = TDNG_J2K_ERROR_MEMORY;
              tdj_sb_free(&ll); tdj_sb_free(&hl);
              tdj_sb_free(&lh); tdj_sb_free(&hh);
              free(work);
              free(fwork);
              goto fail;
            }
            comp_sbs[c].sb[(d - 1) * 3 + 1] = hl;
            comp_sbs[c].sb[(d - 1) * 3 + 2] = lh;
            comp_sbs[c].sb[(d - 1) * 3 + 3] = hh;
            if (d == 1) {
              comp_sbs[c].sb[0] = ll;
            } else {
              memcpy(planes[c], ll.s, (size_t)ll.w * ll.h * sizeof(int32_t));
              cur = planes[c];
              cur_w = ll.w;
              cur_h = ll.h;
              tdj_sb_free(&ll);
            }
          }
        }
      }
    } else {
      /* irreversible 9/7: work in float */
      float **fplanes = NULL;
      float *fll = NULL;
      fplanes = (float **)calloc((size_t)num_comps, sizeof(float *));
      fll = (float *)malloc((size_t)width * height * sizeof(float));
      if (!fplanes || !fll) {
        ret = TDNG_J2K_ERROR_MEMORY;
        if (fplanes) free(fplanes);
        free(fll);
        free(work);
        free(fwork);
        goto fail;
      }
      for (c = 0; c < num_comps; ++c) {
        fplanes[c] = (float *)malloc((size_t)width * height * sizeof(float));
        if (!fplanes[c]) {
          ret = TDNG_J2K_ERROR_MEMORY;
          for (c = 0; c < num_comps; ++c) free(fplanes[c]);
          free(fplanes);
          free(fll);
          free(work);
          free(fwork);
          goto fail;
        }
      }
      for (c = 0; c < num_comps; ++c) {
        float mul = (float)(1.0 / (double)(1ULL << bits[c]));
        for (y = 0; y < height; ++y)
          for (x = 0; x < width; ++x)
            fplanes[c][(size_t)y * width + x] =
                (float)planes[c][(size_t)y * width + x] * mul;
      }
      if (mct && num_comps >= 3) {
        static const float ARF = 0.299f;
        static const float AGF = 0.587f;
        static const float ABF = 0.114f;
        static const float BCB =
            (float)(0.5 / (1.0 - (double)ABF));
        static const float BCR =
            (float)(0.5 / (1.0 - (double)ARF));
        float *r = fplanes[0], *g = fplanes[1], *b = fplanes[2];
        for (y = 0; y < height; ++y)
          for (x = 0; x < width; ++x) {
            float rr = r[(size_t)y * width + x];
            float gg = g[(size_t)y * width + x];
            float bb = b[(size_t)y * width + x];
            float yy = ARF * rr + AGF * gg + ABF * bb;
            r[(size_t)y * width + x] = yy;
            g[(size_t)y * width + x] = BCB * (bb - yy);
            b[(size_t)y * width + x] = BCR * (rr - yy);
          }
      }
      for (c = 0; c < num_comps; ++c) {
        comp_sbs[c].sb = (tdj_sb *)calloc((size_t)(1 + 3 * num_decomps),
                                         sizeof(tdj_sb));
        if (!comp_sbs[c].sb) {
          ret = TDNG_J2K_ERROR_MEMORY;
          for (c = 0; c < num_comps; ++c) free(fplanes[c]);
          free(fplanes);
          free(fll);
          free(work);
          free(fwork);
          goto fail;
        }
      }
      for (c = 0; c < num_comps; ++c) {
        float *cur = fplanes[c];
        int cur_w = width, cur_h = height;
        int d;
        for (d = num_decomps; d >= 1; --d) {
          tdj_sb hl, lh, hh;
          float qd[4];
          memset(&hl, 0, sizeof(hl));
          memset(&lh, 0, sizeof(lh));
          memset(&hh, 0, sizeof(hh));
          qd[0] = qdelta_inv[0];
          qd[1] = qdelta_inv[(d - 1) * 3 + 1];
          qd[2] = qdelta_inv[(d - 1) * 3 + 2];
          qd[3] = qdelta_inv[(d - 1) * 3 + 3];
          if (!tdj_fwd_2d_irv(&hl, &lh, &hh, fll, cur, cur_w, cur_h,
                              d == 1, qd, work, fwork)) {
            ret = TDNG_J2K_ERROR_MEMORY;
            tdj_sb_free(&hl); tdj_sb_free(&lh); tdj_sb_free(&hh);
            for (c = 0; c < num_comps; ++c) free(fplanes[c]);
            free(fplanes);
            free(fll);
            free(work);
            free(fwork);
            goto fail;
          }
          comp_sbs[c].sb[(d - 1) * 3 + 1] = hl;
          comp_sbs[c].sb[(d - 1) * 3 + 2] = lh;
          comp_sbs[c].sb[(d - 1) * 3 + 3] = hh;
          if (d == 1) {
            int32_t *lls = (int32_t *)calloc((size_t)(cur_w + 1) / 2 *
                                             (cur_h + 1) / 2,
                                             sizeof(int32_t));
            tdj_sb *llsb = (tdj_sb *)calloc(1, sizeof(tdj_sb));
            if (!lls || !llsb) {
              ret = TDNG_J2K_ERROR_MEMORY;
              free(lls); free(llsb);
              for (c = 0; c < num_comps; ++c) free(fplanes[c]);
              free(fplanes);
              free(fll);
              free(work);
              free(fwork);
              goto fail;
            }
            llsb->s = lls;
            llsb->w = (cur_w + 1) >> 1;
            llsb->h = (cur_h + 1) >> 1;
            for (y = 0; y < llsb->h; ++y)
              for (x = 0; x < llsb->w; ++x)
                llsb->s[(size_t)y * llsb->w + x] =
                    (int32_t)fll[(size_t)y * llsb->w + x];
            comp_sbs[c].sb[0] = *llsb;
            free(llsb);
          } else {
            memcpy(fplanes[c], fll, (size_t)cur_w * cur_h * sizeof(float));
            cur = fplanes[c];
            cur_w = (cur_w + 1) >> 1;
            cur_h = (cur_h + 1) >> 1;
          }
        }
      }
      for (c = 0; c < num_comps; ++c) free(fplanes[c]);
      free(fplanes);
      free(fll);
    }
    free(work);
    free(fwork);
  }

  /* ---- markers ---- */
  {
    /* SOC */
    ebuf_u16(&e, 0xFF4F);
    /* SIZ */
    ebuf_u16(&e, 0xFF51);
    ebuf_u16(&e, (uint16_t)(38 + 3 * num_comps));
    ebuf_u16(&e, 0x4000);
    ebuf_u32(&e, (uint32_t)width);
    ebuf_u32(&e, (uint32_t)height);
    ebuf_u32(&e, 0);
    ebuf_u32(&e, 0);
    ebuf_u32(&e, (uint32_t)width);
    ebuf_u32(&e, (uint32_t)height);
    ebuf_u32(&e, 0);
    ebuf_u32(&e, 0);
    ebuf_u16(&e, (uint16_t)num_comps);
    for (c = 0; c < num_comps; ++c) {
      ebuf_u8(&e, (uint8_t)((is_signed[c] ? 0x80 : 0) | (bits[c] - 1)));
      ebuf_u8(&e, 1);
      ebuf_u8(&e, 1);
    }
    /* CAP (extended capability: HT block coding, Pcap bit 17) */
    {
      int magb = 0;
      int bp;
      uint16_t ccap = 0;
      for (c = 0; c < qcd_count; ++c) {
        int m = qcd_irrev ? ((qcd_u16[c] >> 11) - 1) : kmax_arr[c];
        if (m > magb) magb = m;
      }
      if (magb <= 8) bp = 0;
      else if (magb < 28) bp = magb - 8;
      else bp = 13 + (magb >> 2);
      if (!reversible) ccap = 0x0020; /* irreversible kernel */
      ebuf_u16(&e, 0xFF50);
      ebuf_u16(&e, 8);
      ebuf_u32(&e, 0x00020000); /* Pcap: HT block coding */
      ebuf_u16(&e, (uint16_t)(ccap | bp));
    }
    /* COD */
    ebuf_u16(&e, 0xFF52);
    ebuf_u16(&e, 12);
    ebuf_u8(&e, 0);
    ebuf_u8(&e, 2); /* RPCL */
    ebuf_u16(&e, 1);
    ebuf_u8(&e, (uint8_t)(mct ? 1 : 0));
    ebuf_u8(&e, (uint8_t)num_decomps);
    ebuf_u8(&e, (uint8_t)(cb_log_w - 2));
    ebuf_u8(&e, (uint8_t)(cb_log_h - 2));
    ebuf_u8(&e, 0x40);
    ebuf_u8(&e, (uint8_t)(reversible ? 1 : 0));
    /* QCD */
    ebuf_u16(&e, 0xFF5C);
    if (qcd_irrev) {
      int i;
      ebuf_u16(&e, (uint16_t)(3 + 2 * qcd_count));
      ebuf_u8(&e, (uint8_t)((qcd_guard << 5) | 2)); /* scalar expounded */
      for (i = 0; i < qcd_count; ++i) ebuf_u16(&e, qcd_u16[i]);
    } else {
      ebuf_u16(&e, (uint16_t)(3 + qcd_count));
      ebuf_u8(&e, (uint8_t)(qcd_guard << 5)); /* reversible */
      ebuf_bytes(&e, qcd_bytes, (size_t)qcd_count);
    }
    /* COM */
    {
      static const char comment[] = "tinydng HTJ2K encoder";
      size_t clen = sizeof(comment) - 1;
      ebuf_u16(&e, 0xFF64);
      ebuf_u16(&e, (uint16_t)(4 + clen));
      ebuf_u16(&e, 1);
      ebuf_bytes(&e, comment, clen);
    }
  }

  /* ---- tile: SOT + SOD + packets ---- */
  sot_pos = e.len;
  ebuf_u16(&e, 0xFF90);
  ebuf_u16(&e, 10);
  ebuf_u16(&e, 0);     /* Isot */
  ebuf_u32(&e, 0);     /* Psot (patched below) */
  ebuf_u8(&e, 0);      /* TPsot */
  ebuf_u8(&e, 1);      /* TNsot */
  ebuf_u16(&e, 0xFF93);

  {
    int r;
    for (r = 0; r <= num_decomps; ++r) {
      for (c = 0; c < num_comps; ++c) {
        ebw w;
        int coded = 0;
        int num_skipped = 0;
        int s;
        int nb = (r == 0) ? 1 : 4;
        tdj_comp_sb *csb = &comp_sbs[c];
        if (!ebw_alloc(&w, 4096)) {
          ret = TDNG_J2K_ERROR_MEMORY;
          goto fail;
        }
        {
          /* packet data buffer (codeblock payloads, appended after the
             whole header is flushed) */
          ebuf pdata;
          memset(&pdata, 0, sizeof(pdata));
          for (s = 0; s < nb; ++s) {
            tdj_sb *sb;
            int nc_x, nc_y;
            tdj_ecb *blocks;
            int bx, by;
            int kmax;
            if (r == 0) {
              if (s != 0) continue;
              sb = &csb->sb[0];
            } else {
              if (s == 0) continue;
              /* storage index (d-1)*3+b already matches the packet slot
                 order (1=top-right, 2=bottom-left, 3=bottom-right) */
              sb = &csb->sb[(r - 1) * 3 + s];
            }
            if (sb->w <= 0 || sb->h <= 0) continue;
            nc_x = tdj_div_ceil(sb->w, cb_w);
            nc_y = tdj_div_ceil(sb->h, cb_h);
            blocks = (tdj_ecb *)calloc((size_t)nc_x * nc_y, sizeof(tdj_ecb));
            if (!blocks) {
              ret = TDNG_J2K_ERROR_MEMORY;
              free(w.b);
              free(pdata.b);
              goto fail;
            }
            kmax = kmax_arr[(r == 0) ? 0 : (r - 1) * 3 + s];
            if (!tdj_encode_subband(blocks, nc_x, nc_y, sb, cb_w, cb_h,
                                    kmax, reversible)) {
              free(w.b);
              free(pdata.b);
              goto fail;
            }
            tdj_packet_subband(&w, blocks, nc_x, nc_y, (uint32_t)(kmax - 1),
                               &coded, &num_skipped);
            for (by = 0; by < nc_y; ++by)
              for (bx = 0; bx < nc_x; ++bx) {
                tdj_ecb *blk = &blocks[by * nc_x + bx];
                if (!blk->empty && blk->data && blk->len)
                  ebuf_bytes(&pdata, blk->data, blk->len);
              }
            for (by = 0; by < nc_y; ++by)
              for (bx = 0; bx < nc_x; ++bx) free(blocks[by * nc_x + bx].data);
            free(blocks);
          }
          if (!coded) ebw_bit(&w, 0); /* empty packet */
          ebw_term(&w);
          if (w.len) ebuf_bytes(&e, w.b, w.len); /* header first */
          if (pdata.len) ebuf_bytes(&e, pdata.b, pdata.len); /* then data */
          free(w.b);
          free(pdata.b);
        }
      }
    }
  }

  ebuf_u16(&e, 0xFFD9); /* EOC */

  if (e.len >= sot_pos + 14 && e.len - sot_pos - 6 <= 0xFFFFFFFFu) {
    uint32_t psot = (uint32_t)(e.len - sot_pos - 2); /* tile-part after SOT marker */
    /* Psot is the 4 bytes at sot_pos+6 (after marker, Lsot, Isot) */
    e.b[sot_pos + 6] = (uint8_t)(psot >> 24);
    e.b[sot_pos + 7] = (uint8_t)(psot >> 16);
    e.b[sot_pos + 8] = (uint8_t)(psot >> 8);
    e.b[sot_pos + 9] = (uint8_t)psot;
  }

  *out = e.b;
  *out_cap = e.cap;
  *out_len = e.len;
  goto done;

fail:
  free(e.b);
  *out = NULL;
  *out_cap = 0;
  *out_len = 0;

done:
  free(kmax_arr);
  if (planes) {
    for (c = 0; c < num_comps; ++c) free(planes[c]);
    free(planes);
  }
  if (comp_sbs) {
    for (c = 0; c < num_comps; ++c) tdj_comp_sb_free(&comp_sbs[c]);
    free(comp_sbs);
  }
  return ret;
}
