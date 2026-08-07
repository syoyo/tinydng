/*
 * tiny_dng_j2k.c - clean-room C11 JPEG 2000 codestream decoder for HTJ2K
 * codestreams. See tiny_dng_j2k.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "tiny_dng_j2k.h"
#include "tiny_dng_htj2k.h"

#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t ui8;
typedef uint16_t ui16;
typedef uint32_t ui32;
typedef int32_t si32;
typedef int64_t si64;

/* ------------------------------------------------------------------ */
/* Endian / byte reader                                               */
/* ------------------------------------------------------------------ */

static int tdj_host_big(void) {
  uint16_t x = 1u;
  ui8 b[2];
  memcpy(b, &x, 2);
  return b[0] == 0;
}
static int tdj_is_le = 1;

static inline ui16 tdj_rd16(const ui8 *p) {
  return (ui16)((p[0] << 8) | p[1]);
}
static inline ui32 tdj_rd32(const ui8 *p) {
  return ((ui32)p[0] << 24) | ((ui32)p[1] << 16) | ((ui32)p[2] << 8) |
         (ui32)p[3];
}

/* byte cursor with bounds checks */
typedef struct tdj_rd {
  const ui8 *p;
  size_t left;
} tdj_rd;

static int tdj_rd_u8(tdj_rd *r, ui32 *v) {
  if (r->left < 1) return 0;
  *v = *r->p++;
  r->left--;
  return 1;
}
static int tdj_rd_u16(tdj_rd *r, ui32 *v) {
  if (r->left < 2) return 0;
  *v = tdj_rd16(r->p);
  r->p += 2;
  r->left -= 2;
  return 1;
}
static int tdj_rd_u32(tdj_rd *r, ui32 *v) {
  if (r->left < 4) return 0;
  *v = tdj_rd32(r->p);
  r->p += 4;
  r->left -= 4;
  return 1;
}
static int tdj_skip(tdj_rd *r, size_t n) {
  if (r->left < n) return 0;
  r->p += n;
  r->left -= n;
  return 1;
}

/* ------------------------------------------------------------------ */
/* Marker constants                                                   */
/* ------------------------------------------------------------------ */

enum {
  TJ_SOC = 0xFF4F, TJ_CAP = 0xFF50, TJ_SIZ = 0xFF51, TJ_COD = 0xFF52,
  TJ_COC = 0xFF53, TJ_TLM = 0xFF55, TJ_PLM = 0xFF57, TJ_PLT = 0xFF58,
  TJ_PPM = 0xFF60, TJ_PPT = 0xFF61, TJ_CRG = 0xFF63, TJ_COM = 0xFF64,
  TJ_QCD = 0xFF5C, TJ_QCC = 0xFF5D, TJ_RGN = 0xFF5E, TJ_POC = 0xFF5F,
  TJ_CPF = 0xFF54, TJ_PRF = 0xFF56, TJ_DFS = 0xFF62, TJ_ATK = 0xFF65,
  TJ_NLT = 0xFF77, TJ_SOT = 0xFF90, TJ_SOP = 0xFF91, TJ_EPH = 0xFF92,
  TJ_SOD = 0xFF93, TJ_EOC = 0xFFD9
};

/* ------------------------------------------------------------------ */
/* Codestream state                                                    */
/* ------------------------------------------------------------------ */

typedef struct tdj_cb {
  int x, y;          /* codeblock index in the subband grid */
  int w, h;          /* actual codeblock size */
  int stride;        /* subband row stride (in samples) */
  int32_t *samples;  /* decoded coefficients (block domain, sign-magnitude) */
  ui32 missing_msbs, num_passes;
  ui32 pass_len[2];
  const ui8 *data;   /* pointer to coded bytes in the codestream buffer */
} tdj_cb;

typedef struct tdj_band {
  int x0, y0, w, h;    /* subband rect in the resolution coordinate space */
  int cb_w, cb_h;      /* codeblock size */
  int nc_x, nc_y;      /* number of codeblocks */
  int stride;          /* subband row stride */
  tdj_cb *cbs;         /* nc_x * nc_y codeblocks */
  int32_t *samples;    /* full subband buffer (stride * h), lossless */
  float *samples_f;    /* full subband buffer (stride * h), lossy */
} tdj_band;

typedef struct tdj_res {
  int x0, y0, w, h;    /* resolution rect (in the component's space) */
  tdj_band bands[4];   /* 0=LL (res 0 only), 1=HL, 2=LH, 3=HH */
} tdj_res;

typedef struct tdj_comp {
  int width, height;   /* full component size */
  int dx, dy;          /* downsampling */
  int bitdepth;
  int num_decomps;
  tdj_res *res;        /* resolutions 0..num_decomps */
  int32_t *recon;      /* full component reconstruction buffer (lossless) */
  int32_t *recon_alt;  /* ping-pong destination for multilevel synthesis */
  float *recon_f;      /* full component reconstruction buffer (lossy) */
  float *irv_tmp;
  float *irv_line_a;
  float *irv_line_b;
  float *irv_vcol;
  size_t irv_tmp_cap, irv_line_cap, irv_vcol_cap;
  /* per-component quantization (from QCC, or the main QCD) */
  ui8 qcd_bytes[97];
  ui16 qcd_u16[97];
  ui32 qcd_count;
  ui32 qcd_guard;
  int qcd_expounded;
} tdj_comp;

typedef struct tdj_cs {
  const ui8 *data;
  size_t size;

  ui32 xsiz, ysiz, xosiz, yosiz, xtsiz, ytsiz, xtosiz, ytosiz;
  ui32 num_comps;
  int *comp_dx, *comp_dy;   /* per-component downsampling */
  int *comp_bits;           /* per-component bit depths */
  int *comp_signed;

  ui32 prog_order;
  ui32 num_decomps;
  int reversible;           /* 1 = 5/3, 0 = 9/7 */
  int mct;                  /* colour transform flag (Scod bit 1) */
  int cb_log_w, cb_log_h;   /* codeblock log2 size minus 2 */
  int precinct_defined;     /* Scod bit 0 */
  ui8 precinct_size[34];    /* if precinct_defined */
  int stripe_causal;        /* VERT_CAUSAL_MODE in COD block style */
  ui8 qcd_bytes[97];        /* QCD per-subband quantization bytes */
  ui16 qcd_u16[97];         /* QCD per-subband 16-bit entries (expounded) */
  ui32 qcd_count;           /* number of QCD subband entries */
  ui32 qcd_guard;           /* QCD guard bits */
  int qcd_expounded;        /* 1 if QCD uses 16-bit scalar expounded */
  ui8 qcc_bytes[16][97];    /* per-component QCC quantization */
  ui16 qcc_u16[16][97];
  ui32 qcc_count[16];
  ui32 qcc_guard[16];
  int qcc_expounded[16];

  ui32 num_tiles_x, num_tiles_y;
  ui32 tile_w, tile_h;

  tdj_comp *comps;
  int initialized;          /* per-tile structures built */
  int lossy;
  void *synth_tmp;          /* reusable synthesis scratch */
  size_t synth_tmp_cap;
  int32_t *scratch32;       /* reusable row scratch */
  size_t scratch32_cap;
  float *synth_tmp_f;
  size_t synth_tmp_f_cap;
  float *scratch_f;
  size_t scratch_f_cap;
} tdj_cs;

static int tdj_subband_kmax(tdj_comp *cp, int res_num, int band);

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
static int tdj_cpu_avx2(void);
static void tdj_sm_to_int_avx2(int32_t *, const int32_t *, int, uint32_t);
static void tdj_sm_to_float_avx2(float *, const int32_t *, int, float);
static void tdj_pack3_avx2(int32_t *, const int32_t *, const int32_t *,
                           const int32_t *, int, int32_t, int32_t, int32_t);
#endif

static int tdj_div_ceil(int a, int b) {
  return (a + b - 1) / b;
}
static int tdj_max(int a, int b) { return a > b ? a : b; }
static int tdj_min(int a, int b) { return a < b ? a : b; }
/* QCD-derived quant step (delta) for a subband of an irreversible stream. */
static float tdj_subband_delta(tdj_comp *cp, int res_num, int band) {
  static const float arr[4] = {1.0f, 2.0f, 2.0f, 4.0f};
  int idx;
  ui32 enc;
  int eps;
  float mantissa;
  int kmax;
  if (cp->qcd_count == 0) return 1.0f;
  idx = (res_num == 0) ? 0 : (res_num - 1) * 3 + band;
  if (idx >= (int)cp->qcd_count) idx = (int)cp->qcd_count - 1;
  if (cp->qcd_expounded) {
    ui32 u = cp->qcd_u16[idx];
    eps = (int)(u >> 11);
    mantissa = (float)((u & 0x7FF) | 0x800) * arr[band & 3];
    mantissa /= (float)(1 << 11);
    mantissa /= (float)(1u << eps);
  } else {
    enc = cp->qcd_bytes[idx];
    eps = (int)(enc >> 3);
    mantissa = (float)(((enc & 0x7) << 8) | 0x100) * arr[band & 3];
    mantissa /= (float)(1 << 8);
    mantissa /= (float)(1u << eps);
  }
  kmax = tdj_subband_kmax(cp, res_num, band);
  return mantissa / (float)(1u << (31 - kmax));
}

static ui32 tdj_clz32(ui32 v) {
  ui32 n = 32;
  while (v) { v >>= 1; n--; }
  return n;
}
static ui32 tdj_log2ceil(ui32 v) {
  ui32 l = 0;
  while ((1u << l) < v) l++;
  return l;
}

/* QCD-derived K_max for a resolution's subband (0=LL, 1=HL, 2=LH, 3=HH). */
static void tdj_qcd_apply_main(tdj_cs *cs, tdj_comp *cp) {
  memcpy(cp->qcd_bytes, cs->qcd_bytes, sizeof(cs->qcd_bytes));
  memcpy(cp->qcd_u16, cs->qcd_u16, sizeof(cs->qcd_u16));
  cp->qcd_count = cs->qcd_count;
  cp->qcd_guard = cs->qcd_guard;
  cp->qcd_expounded = cs->qcd_expounded;
}

static int tdj_subband_kmax(tdj_comp *cp, int res_num, int band) {
  int idx;
  ui32 byte;
  int num_bits;
  if (cp->qcd_count == 0) return 0;
  idx = (res_num == 0) ? 0 : (res_num - 1) * 3 + band;
  if (idx >= (int)cp->qcd_count) idx = (int)cp->qcd_count - 1;
  byte = cp->qcd_bytes[idx];
  num_bits = (int)(byte >> 3);
  num_bits = num_bits == 0 ? 0 : num_bits - 1;
  return num_bits + (int)cp->qcd_guard;
}

/* ------------------------------------------------------------------ */
/* Main header parsing                                                 */
/* ------------------------------------------------------------------ */

static int tdj_parse_siz(tdj_cs *cs, tdj_rd *r) {
  ui32 l, rsiz;
  ui32 i;
  ui32 csiz;
  if (!tdj_rd_u16(r, &l) || !tdj_rd_u16(r, &rsiz)) return 0;
  if (l < 38) return 0;
  if ((l - 38) % 3 != 0) return 0;
  cs->num_comps = (l - 38) / 3;
  if (cs->num_comps == 0 || cs->num_comps > 16384) return 0;
  if (!tdj_rd_u32(r, &cs->xsiz) || !tdj_rd_u32(r, &cs->ysiz) ||
      !tdj_rd_u32(r, &cs->xosiz) || !tdj_rd_u32(r, &cs->yosiz) ||
      !tdj_rd_u32(r, &cs->xtsiz) || !tdj_rd_u32(r, &cs->ytsiz) ||
      !tdj_rd_u32(r, &cs->xtosiz) || !tdj_rd_u32(r, &cs->ytosiz) ||
      !tdj_rd_u16(r, &csiz))
    return 0;
  if (csiz != cs->num_comps) return 0;
  if (cs->xtsiz == 0 || cs->ytsiz == 0 || cs->xsiz <= cs->xosiz ||
      cs->ysiz <= cs->yosiz || cs->xtosiz > cs->xsiz ||
      cs->ytosiz > cs->ysiz)
    return 0;
  {
    /* sanity-bound the image so corrupt SIZ fields cannot drive the
       allocation/geometry math (int arithmetic + size_t calloc) out of
       range. 2^24 per side / 2^29 total samples is far beyond any real
       DNG and keeps num_tiles_x/y within int range. */
    uint64_t w = (uint64_t)cs->xsiz - cs->xosiz;
    uint64_t h = (uint64_t)cs->ysiz - cs->yosiz;
    if (w > 0xFFFFFFu || h > 0xFFFFFFu || w * h > 0x1FFFFFFFu)
      return 0;
  }
  /* bound the tile geometry too: the caller sizes its output buffer from
     the reported tile dims, so keep the worst-case tile sample count
     (tile_w * tile_h * num_comps) within range. */
  if (cs->xtsiz > 0xFFFFFFu || cs->ytsiz > 0xFFFFFFu) return 0;
  if ((uint64_t)cs->xtsiz * cs->ytsiz * cs->num_comps > 0x7FFFFFFFu)
    return 0;
  cs->comp_dx = (int *)calloc(cs->num_comps, sizeof(int));
  cs->comp_dy = (int *)calloc(cs->num_comps, sizeof(int));
  cs->comp_bits = (int *)calloc(cs->num_comps, sizeof(int));
  cs->comp_signed = (int *)calloc(cs->num_comps, sizeof(int));
  if (!cs->comp_dx || !cs->comp_dy || !cs->comp_bits || !cs->comp_signed)
    return 0;
  for (i = 0; i < cs->num_comps; ++i) {
    ui32 ssiz, xr, yr;
    if (!tdj_rd_u8(r, &ssiz) || !tdj_rd_u8(r, &xr) || !tdj_rd_u8(r, &yr))
      return 0;
    cs->comp_bits[i] = (int)((ssiz & 0x7F) + 1);
    cs->comp_signed[i] = (int)((ssiz >> 7) & 1);
    cs->comp_dx[i] = (int)xr ? (int)xr : 1;
    cs->comp_dy[i] = (int)yr ? (int)yr : 1;
  }
  cs->tile_w = cs->xtsiz;
  cs->tile_h = cs->ytsiz;
  cs->num_tiles_x = tdj_div_ceil((int)cs->xsiz - (int)cs->xtosiz,
                                 (int)cs->xtsiz);
  cs->num_tiles_y = tdj_div_ceil((int)cs->ysiz - (int)cs->ytosiz,
                                 (int)cs->ytsiz);
  return 1;
}

static int tdj_parse_cod(tdj_cs *cs, tdj_rd *r) {
  ui32 l, scod, prog, layers, mct, decomp, bw, bh, bstyle, trans;
  ui32 i;
  if (!tdj_rd_u16(r, &l) || !tdj_rd_u8(r, &scod)) return 0;
  if (l < 12) return 0;
  if (!tdj_rd_u8(r, &prog) || !tdj_rd_u16(r, &layers) || !tdj_rd_u8(r, &mct) ||
      !tdj_rd_u8(r, &decomp) || !tdj_rd_u8(r, &bw) || !tdj_rd_u8(r, &bh) ||
      !tdj_rd_u8(r, &bstyle) || !tdj_rd_u8(r, &trans))
    return 0;
  if (prog > 4) return 0;
  if (decomp > 32 || bw > 8 || bh > 8 || bw + bh > 8) return 0;
  cs->prog_order = prog;
  cs->num_decomps = decomp;
  cs->reversible = (trans == 1);
  cs->mct = (mct & 1) ? 1 : 0;
  cs->precinct_defined = (scod & 1) ? 1 : 0;
  cs->stripe_causal = (int)((bstyle & 0x8) ? 1 : 0);
  cs->cb_log_w = (int)bw;
  cs->cb_log_h = (int)bh;
  if (cs->precinct_defined) {
    for (i = 0; i <= decomp; ++i) {
      if (r->left == 0) return 0;
      cs->precinct_size[i] = *r->p++;
      r->left--;
    }
  }
  (void)layers;
  return 1;
}

/* find a marker; returns 0 on failure */
/* ------------------------------------------------------------------ */
/* Geometry                                                            */
/* ------------------------------------------------------------------ */

/* Reference-grid tile rect for tile (tx, ty). */
static void tdj_tile_rect(tdj_cs *cs, int tx, int ty, int *x0, int *y0,
                          int *x1, int *y1) {
  *x0 = (int)cs->xtosiz + tx * (int)cs->xtsiz;
  *y0 = (int)cs->ytosiz + ty * (int)cs->ytsiz;
  *x1 = tdj_min(*x0 + (int)cs->xtsiz, (int)cs->xsiz);
  *y1 = tdj_min(*y0 + (int)cs->ytsiz, (int)cs->ysiz);
  if (*x0 < (int)cs->xosiz) *x0 = (int)cs->xosiz;
  if (*y0 < (int)cs->yosiz) *y0 = (int)cs->yosiz;
}

/* Build the resolution / subband / codeblock structure for one component
   over the tile region given by (trx0,trx1,try0,try1) on the reference grid. */
static int tdj_build_comp(tdj_cs *cs, tdj_comp *cp, int c,
                          int trx0, int trx1, int try0, int try1) {
  int i, r, nres = cp->num_decomps + 1;
  int cb_w = 1 << (cs->cb_log_w + 2);
  int cb_h = 1 << (cs->cb_log_h + 2);

  /* component region in component coordinates */
  {
    int c_x0 = tdj_div_ceil(trx0, cp->dx);
    int c_y0 = tdj_div_ceil(try0, cp->dy);
    int c_x1 = tdj_div_ceil(trx1, cp->dx);
    int c_y1 = tdj_div_ceil(try1, cp->dy);
    cp->width = c_x1 - c_x0;
    cp->height = c_y1 - c_y0;
    (void)c;
  }

  cp->res = (tdj_res *)calloc(nres, sizeof(tdj_res));
  if (!cp->res) return 0;

  /* resolution rects: halve the component rect num_decomps times */
  {
    int rx0 = 0, ry0 = 0, rx1 = cp->width, ry1 = cp->height;
    /* res r has rect after (num_decomps - r) halvings */
    int rr0[40], rr1[40], ry0_[40], ry1_[40];
    for (r = 0; r <= (int)cs->num_decomps; ++r) {
      int k = (int)cs->num_decomps - r; /* number of halvings */
      int a0 = rx0, b0 = ry0, a1 = rx1, b1 = ry1;
      for (i = 0; i < k; ++i) {
        int na0 = (a0 + 1) >> 1, nb0 = (b0 + 1) >> 1;
        int na1 = (a1 + 1) >> 1, nb1 = (b1 + 1) >> 1;
        a0 = na0; a1 = na1; b0 = nb0; b1 = nb1;
      }
      rr0[r] = a0; rr1[r] = a1; ry0_[r] = b0; ry1_[r] = b1;
      cp->res[r].x0 = a0;
      cp->res[r].y0 = b0;
      cp->res[r].w = a1 - a0;
      cp->res[r].h = b1 - b0;
    }

    /* subbands for each resolution */
    for (r = 0; r < nres; ++r) {
      tdj_res *res = &cp->res[r];
      if (r == 0) {
        /* coarsest: only LL (band 0) */
        tdj_band *b = &res->bands[0];
        b->x0 = rr0[0]; b->y0 = ry0_[0];
        b->w = rr1[0] - rr0[0];
        b->h = ry1_[0] - ry0_[0];
      } else {
        int trx0_ = rr0[r], trx1_ = rr1[r];
        int try0_ = ry0_[r], try1_ = ry1_[r];
        for (i = 1; i < 4; ++i) {
          tdj_band *b = &res->bands[i];
          int tbx0 = (trx0_ - (i & 1) + 1) >> 1;
          int tbx1 = (trx1_ - (i & 1) + 1) >> 1;
          int tby0 = (try0_ - (i >> 1) + 1) >> 1;
          int tby1 = (try1_ - (i >> 1) + 1) >> 1;
          b->x0 = tbx0;
          b->y0 = tby0;
          b->w = tbx1 - tbx0;
          b->h = tby1 - tby0;
        }
      }
    }
  }

  /* allocate subband sample buffers + codeblock bookkeeping */
  for (r = 0; r < nres; ++r) {
    tdj_res *res = &cp->res[r];
    int nb = (r == 0) ? 1 : 4;
    for (i = 0; i < nb; ++i) {
      tdj_band *b = &res->bands[i];
      size_t n;
      int j;
      if (b->w == 0 || b->h == 0) {
        b->nc_x = b->nc_y = 0;
        b->cbs = NULL;
        b->samples = NULL;
        b->stride = 0;
        continue;
      }
      b->cb_w = cb_w;
      b->cb_h = cb_h;
      b->nc_x = tdj_div_ceil(b->w, cb_w);
      b->nc_y = tdj_div_ceil(b->h, cb_h);
      b->stride = b->w;
      n = (size_t)b->w * (size_t)b->h;
      if (b->w && n / (size_t)b->w != (size_t)b->h) return 0;
      if (n > (size_t)0x1FFFFFFFu / 2u) return 0;
      if (cs->reversible) {
        b->samples = (int32_t *)calloc(n, sizeof(int32_t));
        if (!b->samples) return 0;
      } else {
        b->samples_f = (float *)calloc(n, sizeof(float));
        if (!b->samples_f) return 0;
      }
      if ((size_t)b->nc_x && (size_t)b->nc_y > (size_t)0xFFFFFFFFu / (size_t)b->nc_x)
        return 0;
      b->cbs = (tdj_cb *)calloc((size_t)b->nc_x * b->nc_y, sizeof(tdj_cb));
      if (!b->cbs) return 0;
      for (j = 0; j < b->nc_x * b->nc_y; ++j) {
        tdj_cb *cb = &b->cbs[j];
        cb->x = j % b->nc_x;
        cb->y = j / b->nc_x;
        cb->w = tdj_min(cb_w, b->w - cb->x * cb_w);
        cb->h = tdj_min(cb_h, b->h - cb->y * cb_h);
        cb->stride = b->stride;
      }
    }
  }
  cp->recon = (int32_t *)calloc((size_t)cp->width * cp->height,
                                sizeof(int32_t));
  if (!cp->recon) return 0;
  cp->recon_alt = (int32_t *)calloc((size_t)cp->width * cp->height,
                                    sizeof(int32_t));
  if (!cp->recon_alt) return 0;
  if (!cs->reversible) {
    cp->recon_f = (float *)calloc((size_t)cp->width * cp->height,
                                  sizeof(float));
    if (!cp->recon_f) return 0;
  }
  return 1;
}

/* ------------------------------------------------------------------ */
/* Packet header bit reader                                            */
/* ------------------------------------------------------------------ */

typedef struct tdj_bitrd {
  const ui8 *p;
  size_t left;
  ui32 cur;
  int nbits; /* bits remaining in cur (from the current byte) */
  int unstuff; /* 1 if the next byte's MSB must be ignored (prev byte 0xFF) */
  size_t total_bits; /* bits consumed overall */
} tdj_bitrd;

static void tdj_bitrd_init(tdj_bitrd *b, const ui8 *p, size_t len) {
  b->p = p;
  b->left = len;
  b->cur = 0;
  b->nbits = 0;
  b->unstuff = 0;
  b->total_bits = 0;
}

/* read one bit (MSB first; a byte following a 0xFF byte contributes 7 bits) */
static int tdj_bitrd_bit(tdj_bitrd *b, ui32 *bit) {
  if (b->nbits == 0) {
    if (b->left == 0) return 0;
    b->cur = *b->p++;
    b->left--;
    b->nbits = 8 - b->unstuff;
    b->unstuff = ((b->cur & 0xFF) == 0xFF);
    /* the bit after unstuffing is the second MSB */
    b->cur <<= (b->nbits == 7) ? 1 : 0;
  }
  *bit = (b->cur >> 7) & 1;
  b->cur <<= 1;
  b->nbits--;
  b->total_bits++;
  return 1;
}

static int tdj_bitrd_bits(tdj_bitrd *b, int n, ui32 *val) {
  ui32 v = 0;
  int i;
  for (i = 0; i < n; ++i) {
    ui32 bit;
    if (!tdj_bitrd_bit(b, &bit)) return 0;
    v = (v << 1) | bit;
  }
  *val = v;
  return 1;
}

/* Bytes of the packet header consumed (rounds the partial byte up). */
static size_t tdj_bitrd_bytes_used(const tdj_bitrd *b, size_t avail) {
  size_t bytes_read = avail - b->left;
  size_t n = (b->nbits == 0) ? bytes_read : 0;
  if (b->nbits != 0) {
    /* the last byte was read but `nbits` of its bits remain unused */
    if (bytes_read == 0) return 0;
    n = bytes_read - 1 + ((8 - b->nbits) ? 1 : 0);
  }
  /* A header that ends on a 0xFF byte is followed by one pad byte (the
     encoder's 0xFF handling leaves the next byte's MSB unused). Match
     OpenJPH's bb_terminate, which consumes it so the codeblock data
     starts at the correct offset. */
  if (b->unstuff && bytes_read > 0) ++n;
  return n;
}

/* ------------------------------------------------------------------ */
/* Tag trees                                                           */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Tag trees and packet parsing                                          */
/* ------------------------------------------------------------------ */

typedef struct tdj_tt {
  int w, h;       /* leaf grid size */
  int nlevels;
  int *val;       /* node values (includes the virtual root slot) */
  ui8 *flg;       /* node flags (value finalized) */
} tdj_tt;

static int tdj_tt_offset(const tdj_tt *t, int level, int x, int y) {
  int off = 0, l;
  int n = t->nlevels;
  if (level >= n) {
    /* virtual root: a single node at the end of the arrays */
    off = 0;
    for (l = 0; l < n; ++l)
      off += tdj_div_ceil(t->w, 1 << l) * tdj_div_ceil(t->h, 1 << l);
    return off;
  }
  for (l = 0; l < level; ++l)
    off += tdj_div_ceil(t->w, 1 << l) * tdj_div_ceil(t->h, 1 << l);
  return off + y * tdj_div_ceil(t->w, 1 << level) + x;
}

static void tdj_tt_setup(tdj_tt *t, int w, int h, int *val, ui8 *flg) {
  t->w = w;
  t->h = h;
  t->nlevels = 1 + tdj_max(tdj_log2ceil((ui32)tdj_max(w, 1)),
                           tdj_log2ceil((ui32)tdj_max(h, 1)));
  t->val = val;
  t->flg = flg;
}

/* Decode the inclusion state for leaf (x,y). Returns 1 if included, 0 if
   the codeblock is empty, -1 on truncation. */
static int tdj_tt_inclusion(tdj_tt *t, int x, int y, tdj_bitrd *b) {
  int lvl;
  for (lvl = t->nlevels - 1; lvl >= 0; --lvl) {
    int idx = tdj_tt_offset(t, lvl, x >> lvl, y >> lvl);
    if (t->val[idx] == 1) return 0; /* empty */
    if (!t->flg[idx]) {
      ui32 bit;
      if (!tdj_bitrd_bit(b, &bit)) return -1;
      t->val[idx] = 1 - (int)bit;
      t->flg[idx] = 1;
      if (bit == 0) return 0; /* empty */
    }
  }
  return 1;
}

/* Decode the missing-msbs value for leaf (x,y). Returns 0 on truncation.
   The value read at each step comes from the node one level ABOVE the node
   being decoded (the parent carries the cumulative value). */
static int tdj_tt_mmsbs(tdj_tt *t, int x, int y, tdj_bitrd *b, ui32 *out) {
  int levp1;
  ui32 value = 0;
  for (levp1 = t->nlevels; levp1 > 0; --levp1) {
    int cur_lev = levp1 - 1;
    /* value = parent node value (level levp1; nlevels = virtual root 0) */
    value = (ui32)t->val[tdj_tt_offset(t, levp1, x >> levp1, y >> levp1)];
    {
      int idx = tdj_tt_offset(t, cur_lev, x >> cur_lev, y >> cur_lev);
      if (!t->flg[idx]) {
        ui32 bit = 0;
        while (bit == 0) {
          if (!tdj_bitrd_bit(b, &bit)) return 0;
          value += 1 - bit;
          if (value > 4096) return 0;
        }
        t->val[idx] = (int)value;
        t->flg[idx] = 1;
      }
    }
  }
  *out = value;
  return 1;
}

/* Parse one packet (one precinct) for a component's resolution.
   `data` points at the packet; `*size` is the available bytes and is
   reduced by the number consumed. */
/* Parse one packet (one precinct = whole resolution) for a component's
   resolution. `data` points at the packet; `size` is the available bytes;
   `*consumed` receives the number of bytes consumed (header + codeblocks). */
static int tdj_parse_packet(tdj_cs *cs, int comp_idx, int res_num,
                            const ui8 *data, size_t size, size_t *consumed) {
  tdj_comp *cp = &cs->comps[comp_idx];
  tdj_res *res = &cp->res[res_num];
  tdj_bitrd bb;
  int nb = (res_num == 0) ? 1 : 4;
  int s;
  ui32 bit;
  int empty_packet = 1;
  size_t header_bytes;
  size_t data_off;

  (void)cs;
  tdj_bitrd_init(&bb, data, size);

  for (s = 0; s < nb; ++s) {
    tdj_band *b = &res->bands[s];
    int x, y;
    if (b->nc_x == 0 || b->nc_y == 0) continue;

    if (empty_packet) {
      if (!tdj_bitrd_bit(&bb, &bit)) return 0;
      if (bit == 0) {
        /* empty packet */
        *consumed = tdj_bitrd_bytes_used(&bb, size);
        return 1;
      }
      empty_packet = 0;
    }

    {
      /* inclusion + msbs tag trees */
      tdj_tt inc, msb;
      size_t nn;
      {
        int nl = 1 + tdj_max(tdj_log2ceil((ui32)tdj_max(b->nc_x, 1)),
                             tdj_log2ceil((ui32)tdj_max(b->nc_y, 1)));
        int l;
        nn = 1; /* virtual root */
        for (l = 0; l < nl; ++l)
          nn += tdj_div_ceil(b->nc_x, 1 << l) * tdj_div_ceil(b->nc_y, 1 << l);
      }
      int *inc_val = (int *)malloc(nn * sizeof(int));
      ui8 *inc_flg = (ui8 *)malloc(nn);
      int *msb_val = (int *)malloc(nn * sizeof(int));
      ui8 *msb_flg = (ui8 *)malloc(nn);
      if (!inc_val || !inc_flg || !msb_val || !msb_flg) {
        free(inc_val); free(inc_flg); free(msb_val); free(msb_flg);
        return 0;
      }
      tdj_tt_setup(&inc, b->nc_x, b->nc_y, inc_val, inc_flg);
      tdj_tt_setup(&msb, b->nc_x, b->nc_y, msb_val, msb_flg);
      memset(inc_val, 0, nn * sizeof(int));
      memset(inc_flg, 0, nn);
      memset(msb_val, 0, nn * sizeof(int));
      memset(msb_flg, 0, nn);

      for (y = 0; y < b->nc_y; ++y) {
        for (x = 0; x < b->nc_x; ++x) {
          tdj_cb *cb = &b->cbs[y * b->nc_x + x];
          int inc_ret;
          ui32 mmsbs = 0;
          ui32 num_passes = 1;
          int Lblock = 3;
          int num_phld_passes;
          int bits;
          ui32 len;

          inc_ret = tdj_tt_inclusion(&inc, x, y, &bb);
          if (inc_ret < 0) {
            free(inc_val); free(inc_flg); free(msb_val); free(msb_flg);
            return 0;
          }
          if (inc_ret == 0) {
            cb->pass_len[0] = cb->pass_len[1] = 0;
            cb->num_passes = 0;
            cb->missing_msbs = 0;
            cb->data = NULL;
            continue;
          }

          if (!tdj_tt_mmsbs(&msb, x, y, &bb, &mmsbs)) {
            free(inc_val); free(inc_flg); free(msb_val); free(msb_flg);
            return 0;
          }
          if (mmsbs > 60) {
            free(inc_val); free(inc_flg); free(msb_val); free(msb_flg);
            return 0;
          }
          cb->missing_msbs = mmsbs;

          /* num_passes */
          if (!tdj_bitrd_bit(&bb, &bit)) {
            free(inc_val); free(inc_flg); free(msb_val); free(msb_flg);
            return 0;
          }
          if (bit) {
            num_passes = 2;
            if (!tdj_bitrd_bit(&bb, &bit)) {
              free(inc_val); free(inc_flg); free(msb_val); free(msb_flg);
              return 0;
            }
            if (bit) {
              if (!tdj_bitrd_bits(&bb, 2, &bit)) {
                free(inc_val); free(inc_flg); free(msb_val); free(msb_flg);
                return 0;
              }
              num_passes = 3 + bit;
              if (bit == 3) {
                if (!tdj_bitrd_bits(&bb, 5, &bit)) {
                  free(inc_val); free(inc_flg); free(msb_val); free(msb_flg);
                  return 0;
                }
                num_passes = 6 + bit;
                if (bit == 31) {
                  if (!tdj_bitrd_bits(&bb, 7, &bit)) {
                    free(inc_val); free(inc_flg); free(msb_val); free(msb_flg);
                    return 0;
                  }
                  num_passes = 37 + bit;
                }
              }
            }
          }

          num_phld_passes = ((int)num_passes - 1) / 3;
          cb->missing_msbs += (ui32)num_phld_passes;
          num_passes -= (ui32)(num_phld_passes * 3);
          cb->num_passes = num_passes;

          /* Lblock */
          bit = 1;
          while (bit) {
            if (!tdj_bitrd_bit(&bb, &bit)) {
              free(inc_val); free(inc_flg); free(msb_val); free(msb_flg);
              return 0;
            }
            Lblock += (int)bit;
          }
          bits = Lblock + (int)(31u - tdj_clz32((ui32)(num_phld_passes + 1)));
          if (bits < 0) bits = 0;
          if (!tdj_bitrd_bits(&bb, bits, &len)) {
            free(inc_val); free(inc_flg); free(msb_val); free(msb_flg);
            return 0;
          }
          if (len < 2 || len >= 65535) {
            free(inc_val); free(inc_flg); free(msb_val); free(msb_flg);
            return 0;
          }
          cb->pass_len[0] = len;

          if (cb->num_passes > 1) {
            bits = Lblock + (cb->num_passes > 2 ? 1 : 0);
            if (!tdj_bitrd_bits(&bb, bits, &len)) {
              free(inc_val); free(inc_flg); free(msb_val); free(msb_flg);
              return 0;
            }
            if (len >= 2047) {
              free(inc_val); free(inc_flg); free(msb_val); free(msb_flg);
              return 0;
            }
            cb->pass_len[1] = len;
          } else {
            cb->pass_len[1] = 0;
          }
        }
      }
      free(inc_val); free(inc_flg); free(msb_val); free(msb_flg);
    }
  }

  header_bytes = tdj_bitrd_bytes_used(&bb, size);
  if (header_bytes > size) header_bytes = size;

  /* read codeblock data in decode order */
  data_off = header_bytes;
  for (s = 0; s < nb; ++s) {
    tdj_band *b = &res->bands[s];
    int x, y;
    if (b->nc_x == 0 || b->nc_y == 0) continue;
    for (y = 0; y < b->nc_y; ++y) {
      for (x = 0; x < b->nc_x; ++x) {
        tdj_cb *cb = &b->cbs[y * b->nc_x + x];
        ui32 num_bytes = cb->pass_len[0] + cb->pass_len[1];
        if (num_bytes == 0) { cb->data = NULL; continue; }
        if (data_off + num_bytes > size) return 0;
        cb->data = data + data_off;
        data_off += num_bytes;
      }
    }
  }
  *consumed = data_off;
  return 1;
}

/* ------------------------------------------------------------------ */
/* Tile decode: packets + block codec                                  */
/* ------------------------------------------------------------------ */

static int tdj_decode_blocks(tdj_cs *cs, tdj_comp *cp) {
  int r, i;
  tdng_htj2k_ctx hctx;
  int32_t *tmp = NULL;
  size_t tmp_cap = 0;
  tdng_htj2k_ctx_init(&hctx);
  for (r = 0; r <= (int)cp->num_decomps; ++r) {
    tdj_res *res = &cp->res[r];
    int nb = (r == 0) ? 1 : 4;
    for (i = 0; i < nb; ++i) {
      tdj_band *b = &res->bands[i];
      int j;
      if (b->nc_x == 0 || b->nc_y == 0) continue;
      for (j = 0; j < b->nc_x * b->nc_y; ++j) {
        tdj_cb *cb = &b->cbs[j];
        if (cb->data == NULL || cb->pass_len[0] == 0) continue;
        {
          int32_t *dst = b->samples + (size_t)cb->y * cb->h * b->stride +
                         (size_t)cb->x * cb->w;
          size_t need = (size_t)(cb->h + 4) * cb->w * sizeof(int32_t);
          int rr, cc;
          if (need > tmp_cap) {
            int32_t *nbuf = (int32_t *)realloc(tmp, need);
            if (!nbuf) {
              tdng_htj2k_ctx_free(&hctx);
              free(tmp);
              return 0;
            }
            tmp = nbuf;
            tmp_cap = need;
          }
          rr = tdng_htj2k_decode_codeblock32_ctx(
              &hctx, cb->data, (uint32_t *)tmp, cb->missing_msbs,
              cb->num_passes, cb->pass_len[0], cb->pass_len[1], (uint32_t)cb->w,
              (uint32_t)cb->h, (uint32_t)cb->w, cs->stripe_causal);
          if (rr != TDNG_HTJ2K_OK) {
            /* corrupt codeblock: leave the block zeroed */
            tdng_htj2k_ctx_free(&hctx);
            continue;
          }
          if (!cs->reversible) {
            float delta = tdj_subband_delta(cp, r, i);
            float *df = b->samples_f + (size_t)cb->y * cb->h * b->stride +
                        (size_t)cb->x * cb->w;
            if (tdj_cpu_avx2()) {
              for (cc = 0; cc < cb->h; ++cc)
                tdj_sm_to_float_avx2(df + (size_t)cc * b->stride,
                                     tmp + (size_t)cc * cb->w, cb->w, delta);
            } else {
              for (cc = 0; cc < cb->h; ++cc) {
                int k;
                for (k = 0; k < cb->w; ++k) {
                  int32_t v = tmp[cc * cb->w + k];
                  float mag = (float)(v & 0x7FFFFFFFu) * delta;
                  df[cc * b->stride + k] = (v & 0x80000000u) ? -mag : mag;
                }
              }
            }
          } else {
            int kmax = tdj_subband_kmax(cp, r, i);
            uint32_t shift = (kmax > 0) ? (uint32_t)(31 - kmax) : 30u;
            if (tdj_cpu_avx2()) {
              for (cc = 0; cc < cb->h; ++cc)
                tdj_sm_to_int_avx2(dst + (size_t)cc * b->stride,
                                   tmp + (size_t)cc * cb->w, cb->w, shift);
            } else {
              for (cc = 0; cc < cb->h; ++cc) {
                int k;
                for (k = 0; k < cb->w; ++k) {
                  int32_t v = tmp[cc * cb->w + k];
                  si32 mag = (si32)((v & 0x7FFFFFFFu) >> shift);
                  si32 sv = (v & 0x80000000u) ? -mag : mag;
                  dst[cc * b->stride + k] = sv;
                }
              }
            }
          }
        }
      }
    }
  }
  tdng_htj2k_ctx_free(&hctx);
  free(tmp);
  return 1;
}

/* ------------------------------------------------------------------ */
/* Inverse wavelet                                                     */
/* ------------------------------------------------------------------ */

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
static int tdj_cpu_avx2(void);
static void tdj_v53_update_even_avx2(int32_t *, const int32_t *, const int32_t *, int);
static void tdj_v53_predict_even_avx2(int32_t *, const int32_t *, const int32_t *, int);
static void tdj_v53_update_even_to_avx2(int32_t *, const int32_t *, const int32_t *, const int32_t *, int);
static void tdj_v53_predict_even_to_avx2(int32_t *, const int32_t *, const int32_t *, const int32_t *, int);
static void tdj_interleave_even_avx2(int32_t *, const int32_t *, const int32_t *, int);
#else
static int tdj_cpu_avx2(void);
#endif

/* 1D 5/3 inverse synthesis (lifting form). Combines `low` (even-indexed
   output samples) and `high` (odd-indexed) into `out` of length
   n_low + n_high. `even` is true when the first output sample is low.
   Modifies low/high in place. Boundary samples are handled outside the
   interior loops. */
static void tdj_synth_53_1d(int32_t *out, int32_t *low, int32_t *high,
                            int n_low, int n_high, int even) {
  int i;
  if (n_low == 0) {
    for (i = 0; i < n_high; ++i) out[i] = high[i] >> 1;
    return;
  }
  if (n_high == 0) {
    for (i = 0; i < n_low; ++i) out[i] = low[i];
    return;
  }
  if (even) {
    if (tdj_cpu_avx2() && n_low == n_high) {
      tdj_v53_update_even_avx2(low + 1, high, high + 1, n_low - 1);
      low[0] -= (high[0] + high[0] + 2) >> 2;
      tdj_v53_predict_even_avx2(high, low, low + 1, n_high - 1);
      high[n_high - 1] += (low[n_high - 1] + low[n_high - 1]) >> 1;
      tdj_interleave_even_avx2(out, low, high, n_high);
    } else {
      low[0] -= (high[0] + high[0] + 2) >> 2;
      for (i = 1; i < n_low; ++i) {
        int hp = (i < n_high) ? high[i] : high[n_high - 1];
        low[i] -= (high[i - 1] + hp + 2) >> 2;
      }
      for (i = 0; i < n_high - 1; ++i)
        high[i] += (low[i] + low[i + 1]) >> 1;
      high[n_high - 1] +=
          (low[n_high - 1] + (n_high < n_low ? low[n_high] : low[n_high - 1])) >>
          1;
      for (i = 0; i < n_high; ++i) {
        out[2 * i] = low[i];
        out[2 * i + 1] = high[i];
      }
    }
    if (n_low > n_high) out[2 * n_high] = low[n_high];
  } else {
    {
      int hp0 = (n_high > 1) ? high[1] : high[0];
      low[0] -= (high[0] + hp0 + 2) >> 2;
    }
    for (i = 1; i < n_low; ++i) {
      int hp1 = (i + 1 < n_high) ? high[i + 1] : high[n_high - 1];
      int hi = (i < n_high) ? high[i] : high[n_high - 1];
      low[i] -= (hp1 + hi + 2) >> 2;
    }
    high[0] += (low[0] + low[0]) >> 1;
    for (i = 1; i < n_high; ++i)
      high[i] += (low[i - 1] + low[i]) >> 1;
    for (i = 0; i < n_low; ++i) {
      out[2 * i] = high[i];
      out[2 * i + 1] = low[i];
    }
    if (n_high > n_low) out[2 * n_low] = high[n_low];
  }
}

/* 1D irreversible 9/7 inverse synthesis (floating point).
   Mirrors OpenJPH's gen_irv_horz_syn: uniform lifting steps with symmetric
   extension written into one-slot slack on each side of the low/high buffers
   (low[-1], low[n_low], high[-1], high[n_high]). This handles odd lengths
   (n_low != n_high) without out-of-bounds access. Callers must pass buffers
   with >= 1 float of slack on both sides. */
static void tdj_synth_97_1d(float *out, float *low, float *high, int n_low,
                            int n_high, int even) {
  static const float a[4] = {0.443506852043971f, 0.882911075530934f,
                             -0.052980118572961f, -1.586134342059924f};
  static const float K = 1.230174104914001f;
  int i, j, ev = even;
  if (n_low == 0) {
    out[0] = high[0] * 0.5f;
    return;
  }
  if (n_high == 0) {
    out[0] = low[0];
    return;
  }
  for (i = 0; i < n_low; ++i) low[i] *= K;
  for (i = 0; i < n_high; ++i) high[i] /= K;
  for (j = 0; j < 4; ++j) {
    float *aug = (j & 1) ? high : low;
    float *oth = (j & 1) ? low : high;
    int aug_w = (j & 1) ? n_high : n_low;
    int oth_w = (j & 1) ? n_low : n_high;
    float *sp;
    oth[-1] = oth[0];
    oth[oth_w] = oth[oth_w - 1];
    sp = oth + (ev ? 0 : 1);
    for (i = 0; i < aug_w; ++i, ++sp) aug[i] -= a[j] * (sp[-1] + sp[0]);
    ev = !ev;
  }
  {
    float *sph = high, *spl = low, *dp = out;
    int w = n_low + n_high;
    if (!even) {
      *dp++ = *sph++;
      --w;
    }
    for (; w > 1; w -= 2) {
      *dp++ = *spl++;
      *dp++ = *sph++;
    }
    if (w) *dp++ = *spl++;
  }
}

/* Irreversible 9/7 synthesis for one resolution (float). */
static int tdj_synth_res_irv(tdj_comp *cp, int r,
                             const float *ll_src, int ll_stride, int ll_w,
                             int ll_h) {
  tdj_res *res = &cp->res[r];
  int out_w = res->w, out_h = res->h;
  int vert_even = (res->y0 & 1) == 0;
  int horz_even = (res->x0 & 1) == 0;
  float *tmp = cp->irv_tmp;
  int n_low_y, n_high_y, n_low_x, n_high_x;
  int x, y;

  if (r == 0) {
    int j, k;
    for (j = 0; j < out_h; ++j)
      for (k = 0; k < out_w; ++k)
        cp->recon_f[j * out_w + k] = ll_src[j * ll_stride + k];
    return 1;
  }

  tdj_band *hl = &res->bands[1];
  tdj_band *lh = &res->bands[2];
  tdj_band *hh = &res->bands[3];
  n_low_y = ll_h;
  n_high_y = lh->h;
  n_low_x = ll_w;
  n_high_x = hl->w;

  if (!tmp) return 0;

  {
    int n_rows_low = ll_h, n_rows_high = lh->h;
    for (y = 0; y < tdj_max(n_rows_low, n_rows_high); ++y) {
      if (y < n_rows_low) {
        const float *ll_row = ll_src + (size_t)y * ll_stride;
        const float *hl_row = (y < hl->h) ? hl->samples_f + (size_t)y * hl->stride : NULL;
        float *lcopy = cp->irv_line_a;
        float *hcopy = cp->irv_line_b;
        if (!lcopy || !hcopy) return 0;
        int k;
        for (k = 0; k < n_low_x; ++k) lcopy[k + 1] = ll_row[k];
        for (k = 0; k < n_high_x; ++k) hcopy[k + 1] = hl_row ? hl_row[k] : 0.0f;
        tdj_synth_97_1d(tmp + (size_t)(2 * y) * out_w, lcopy + 1, hcopy + 1,
                        n_low_x, n_high_x, horz_even);
      }
      if (y < n_rows_high) {
        const float *lh_row = lh->samples_f + (size_t)y * lh->stride;
        const float *hh_row = (y < hh->h) ? hh->samples_f + (size_t)y * hh->stride : NULL;
        float *lcopy = cp->irv_line_a;
        float *hcopy = cp->irv_line_b;
        if (!lcopy || !hcopy) return 0;
        int k;
        for (k = 0; k < n_low_x; ++k) lcopy[k + 1] = lh_row[k];
        for (k = 0; k < n_high_x; ++k) hcopy[k + 1] = hh_row ? hh_row[k] : 0.0f;
        tdj_synth_97_1d(tmp + (size_t)(2 * y + 1) * out_w, lcopy + 1, hcopy + 1,
                        n_low_x, n_high_x, horz_even);
      }
    }
  }

  {
    float *vcol = cp->irv_vcol;
    if (!vcol) return 0;
    for (x = 0; x < out_w; ++x) {
      float *low = cp->irv_line_a;
      float *high = cp->irv_line_b;
      if (!low || !high) return 0;
      int i;
      for (i = 0; i < n_low_y; ++i) low[i + 1] = tmp[(size_t)(2 * i) * out_w + x];
      for (i = 0; i < n_high_y; ++i) high[i + 1] = tmp[(size_t)(2 * i + 1) * out_w + x];
      tdj_synth_97_1d(vcol, low + 1, high + 1, n_low_y, n_high_y, vert_even);
      for (i = 0; i < out_h; ++i) tmp[(size_t)i * out_w + x] = vcol[i];
    }
  }
  {
    int j, k;
    for (j = 0; j < out_h; ++j)
      for (k = 0; k < out_w; ++k)
        cp->recon_f[j * out_w + k] = tmp[j * out_w + k];
  }
  return 1;
}

/* Synthesize the full component into cp->recon_f (irreversible 9/7). */
static int tdj_synth_component_irv(tdj_comp *cp) {
  int r;
  int max_side = cp->res[cp->num_decomps].w;
  if (cp->res[cp->num_decomps].h > max_side) max_side = cp->res[cp->num_decomps].h;
  size_t tmp_cap = (size_t)max_side * max_side;
  size_t line_cap = (size_t)max_side + 2;
  float *p;
  if (cp->irv_tmp_cap < tmp_cap) {
    p = (float *)realloc(cp->irv_tmp, tmp_cap * sizeof(float));
    if (!p) return 0;
    cp->irv_tmp = p; cp->irv_tmp_cap = tmp_cap;
  }
  if (cp->irv_line_cap < line_cap) {
    p = (float *)realloc(cp->irv_line_a, line_cap * sizeof(float));
    if (!p) return 0;
    cp->irv_line_a = p;
    p = (float *)realloc(cp->irv_line_b, line_cap * sizeof(float));
    if (!p) return 0;
    cp->irv_line_b = p; cp->irv_line_cap = line_cap;
  }
  if (cp->irv_vcol_cap < line_cap) {
    p = (float *)realloc(cp->irv_vcol, line_cap * sizeof(float));
    if (!p) return 0;
    cp->irv_vcol = p; cp->irv_vcol_cap = line_cap;
  }
  if (cp->num_decomps == 0) {
    tdj_band *b = &cp->res[0].bands[0];
    int j, k;
    for (j = 0; j < cp->height; ++j)
      for (k = 0; k < cp->width; ++k)
        cp->recon_f[j * cp->width + k] = b->samples_f[j * b->stride + k];
    return 1;
  }
  for (r = 1; r <= (int)cp->num_decomps; ++r) {
    int ok;
    if (r == 1) {
      tdj_band *b = &cp->res[0].bands[0];
      ok = tdj_synth_res_irv(cp, r, b->samples_f, b->stride, b->w, b->h);
    } else {
      tdj_res *prev = &cp->res[r - 1];
      ok = tdj_synth_res_irv(cp, r, cp->recon_f, prev->w, prev->w, prev->h);
    }
    if (!ok) return 0;
  }
  return 1;
}

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>

__attribute__((target("avx2")))
static int tdj_cpu_avx2(void) {
  static int cached = -1;
  if (cached >= 0) return cached;
  __builtin_cpu_init();
  cached = __builtin_cpu_supports("avx2") != 0;
  return cached;
}

/* AVX2: even-row update, low[i] -= (up[i] + dn[i] + 2) >> 2 */
__attribute__((target("avx2")))
static void tdj_v53_update_even_avx2(int32_t *r0, const int32_t *up,
                                     const int32_t *dn, int n) {
  int i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256i u = _mm256_loadu_si256((const __m256i *)(up + i));
    __m256i d = _mm256_loadu_si256((const __m256i *)(dn + i));
    __m256i r = _mm256_loadu_si256((const __m256i *)(r0 + i));
    __m256i s = _mm256_add_epi32(_mm256_add_epi32(u, d), _mm256_set1_epi32(2));
    r = _mm256_sub_epi32(r, _mm256_srai_epi32(s, 2));
    _mm256_storeu_si256((__m256i *)(r0 + i), r);
  }
  for (; i < n; ++i) r0[i] -= (up[i] + dn[i] + 2) >> 2;
}

/* AVX2: odd-row predict, high[i] += (up[i] + dn[i]) >> 1 */
__attribute__((target("avx2")))
static void tdj_v53_predict_even_avx2(int32_t *r1, const int32_t *up,
                                      const int32_t *dn, int n) {
  int i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256i u = _mm256_loadu_si256((const __m256i *)(up + i));
    __m256i d = _mm256_loadu_si256((const __m256i *)(dn + i));
    __m256i r = _mm256_loadu_si256((const __m256i *)(r1 + i));
    r = _mm256_add_epi32(r, _mm256_srai_epi32(_mm256_add_epi32(u, d), 1));
    _mm256_storeu_si256((__m256i *)(r1 + i), r);
  }
  for (; i < n; ++i) r1[i] += (up[i] + dn[i]) >> 1;
}

/* AVX2: dst[i] = src[i] - ((up[i] + dn[i] + 2) >> 2) */
__attribute__((target("avx2")))
static void tdj_v53_update_even_to_avx2(int32_t *dst, const int32_t *src,
                                        const int32_t *up, const int32_t *dn,
                                        int n) {
  int i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256i u = _mm256_loadu_si256((const __m256i *)(up + i));
    __m256i d = _mm256_loadu_si256((const __m256i *)(dn + i));
    __m256i r = _mm256_loadu_si256((const __m256i *)(src + i));
    __m256i s = _mm256_add_epi32(_mm256_add_epi32(u, d), _mm256_set1_epi32(2));
    r = _mm256_sub_epi32(r, _mm256_srai_epi32(s, 2));
    _mm256_storeu_si256((__m256i *)(dst + i), r);
  }
  for (; i < n; ++i) dst[i] = src[i] - ((up[i] + dn[i] + 2) >> 2);
}

/* AVX2: dst[i] = src[i] + ((up[i] + dn[i]) >> 1) */
__attribute__((target("avx2")))
static void tdj_v53_predict_even_to_avx2(int32_t *dst, const int32_t *src,
                                         const int32_t *up, const int32_t *dn,
                                         int n) {
  int i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256i u = _mm256_loadu_si256((const __m256i *)(up + i));
    __m256i d = _mm256_loadu_si256((const __m256i *)(dn + i));
    __m256i r = _mm256_loadu_si256((const __m256i *)(src + i));
    r = _mm256_add_epi32(r, _mm256_srai_epi32(_mm256_add_epi32(u, d), 1));
    _mm256_storeu_si256((__m256i *)(dst + i), r);
  }
  for (; i < n; ++i) dst[i] = src[i] + ((up[i] + dn[i]) >> 1);
}

/* AVX2: interleave low and high into out (even case). 32-bit interleave
   across 8 elements needs a cross-lane combine. */
__attribute__((target("avx2")))
static void tdj_interleave_even_avx2(int32_t *out, const int32_t *low,
                                     const int32_t *high, int n) {
  int i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256i lo = _mm256_loadu_si256((const __m256i *)(low + i));
    __m256i hi = _mm256_loadu_si256((const __m256i *)(high + i));
    __m128i lo0 = _mm256_castsi256_si128(lo);
    __m128i hi0 = _mm256_castsi256_si128(hi);
    __m128i p0 = _mm_unpacklo_epi32(lo0, hi0);
    __m128i p1 = _mm_unpackhi_epi32(lo0, hi0);
    __m128i lo1 = _mm256_extracti128_si256(lo, 1);
    __m128i hi1 = _mm256_extracti128_si256(hi, 1);
    __m128i p2 = _mm_unpacklo_epi32(lo1, hi1);
    __m128i p3 = _mm_unpackhi_epi32(lo1, hi1);
    _mm256_storeu_si256((__m256i *)(out + 2 * i), _mm256_set_m128i(p1, p0));
    _mm256_storeu_si256((__m256i *)(out + 2 * i + 8),
                        _mm256_set_m128i(p3, p2));
  }
  for (; i < n; ++i) {
    out[2 * i] = low[i];
    out[2 * i + 1] = high[i];
  }
}

/* AVX2: interleave high and low into out (odd case). */
__attribute__((target("avx2")))
static void __attribute__((unused)) tdj_interleave_odd_avx2(int32_t *out, const int32_t *low,
                                    const int32_t *high, int n) {
  int i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256i lo = _mm256_loadu_si256((const __m256i *)(low + i));
    __m256i hi = _mm256_loadu_si256((const __m256i *)(high + i));
    __m128i lo0 = _mm256_castsi256_si128(lo);
    __m128i hi0 = _mm256_castsi256_si128(hi);
    __m128i p0 = _mm_unpacklo_epi32(hi0, lo0);
    __m128i p1 = _mm_unpackhi_epi32(hi0, lo0);
    __m128i lo1 = _mm256_extracti128_si256(lo, 1);
    __m128i hi1 = _mm256_extracti128_si256(hi, 1);
    __m128i p2 = _mm_unpacklo_epi32(hi1, lo1);
    __m128i p3 = _mm_unpackhi_epi32(hi1, lo1);
    _mm256_storeu_si256((__m256i *)(out + 2 * i), _mm256_set_m128i(p1, p0));
    _mm256_storeu_si256((__m256i *)(out + 2 * i + 8),
                        _mm256_set_m128i(p3, p2));
  }
  for (; i < n; ++i) {
    out[2 * i] = high[i];
    out[2 * i + 1] = low[i];
  }
}
/* AVX2: convert sign-magnitude block samples to signed integers.
   sv = ((v & 0x7FFFFFFF) >> shift) with sign applied, arithmetic. */
__attribute__((target("avx2")))
static void tdj_sm_to_float_avx2(float *dst, const int32_t *src, int n,
                                 float delta) {
  int i = 0;
  __m256 deltav = _mm256_set1_ps(delta);
  __m256i mask = _mm256_set1_epi32(0x7FFFFFFF);
  for (; i + 8 <= n; i += 8) {
    __m256i v = _mm256_loadu_si256((const __m256i *)(src + i));
    __m256i m = _mm256_and_si256(v, mask);
    __m256 f = _mm256_cvtepi32_ps(m);
    f = _mm256_mul_ps(f, deltav);
    /* negate by flipping the float sign bit when the integer sign is set */
    __m256 sign = _mm256_and_ps(_mm256_castsi256_ps(v),
                                _mm256_set1_ps(-0.0f));
    __m256 r = _mm256_xor_ps(f, sign);
    _mm256_storeu_ps(dst + i, r);
  }
  for (; i < n; ++i) {
    int32_t v = src[i];
    float f = (float)((uint32_t)v & 0x7FFFFFFFu) * delta;
    dst[i] = (v & 0x80000000u) ? -f : f;
  }
}

__attribute__((target("avx2")))
static void tdj_sm_to_int_avx2(int32_t *dst, const int32_t *src, int n,
                               uint32_t shift) {
  int i = 0;
  __m256i sh = _mm256_set1_epi32((int)shift);
  __m256i mask = _mm256_set1_epi32(0x7FFFFFFF);
  for (; i + 8 <= n; i += 8) {
    __m256i v = _mm256_loadu_si256((const __m256i *)(src + i));
    __m256i m = _mm256_and_si256(v, mask);
    m = _mm256_srlv_epi32(m, sh);
    __m256i sgn = _mm256_srai_epi32(v, 31);
    __m256i r = _mm256_sub_epi32(_mm256_xor_si256(m, sgn), sgn);
    _mm256_storeu_si256((__m256i *)(dst + i), r);
  }
  for (; i < n; ++i) {
    int32_t v = src[i];
    int32_t m = (int32_t)((uint32_t)v & 0x7FFFFFFFu) >> shift;
    dst[i] = (v & 0x80000000u) ? -m : m;
  }
}

/* AVX2: 3-component interleaved pack with DC shifts folded in.
   out[3i+0]=a[i]+s0, out[3i+1]=b[i]+s1, out[3i+2]=c[i]+s2 for i in [0,n). */
__attribute__((target("avx2")))
static void tdj_pack3_avx2(int32_t *out, const int32_t *a, const int32_t *b,
                           const int32_t *c, int n, int32_t s0, int32_t s1,
                           int32_t s2) {
  __m256i idx012 = _mm256_setr_epi32(0, 0, 0, 1, 1, 1, 2, 2);
  __m256i idx345 = _mm256_setr_epi32(3, 3, 3, 4, 4, 4, 5, 5);
  __m256i idx567 = _mm256_setr_epi32(5, 5, 5, 6, 6, 6, 7, 7);
  __m256i idx234 = _mm256_setr_epi32(2, 2, 2, 3, 3, 3, 4, 4);
  __m256i sv0 = _mm256_set1_epi32(s0);
  __m256i sv1 = _mm256_set1_epi32(s1);
  __m256i sv2 = _mm256_set1_epi32(s2);
  int i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256i va = _mm256_add_epi32(
        _mm256_loadu_si256((const __m256i *)(a + i)), sv0);
    __m256i vb = _mm256_add_epi32(
        _mm256_loadu_si256((const __m256i *)(b + i)), sv1);
    __m256i vc = _mm256_add_epi32(
        _mm256_loadu_si256((const __m256i *)(c + i)), sv2);
    __m256i pa0 = _mm256_permutevar8x32_epi32(va, idx012);
    __m256i pb0 = _mm256_permutevar8x32_epi32(vb, idx012);
    __m256i pc0 = _mm256_permutevar8x32_epi32(vc, idx012);
    __m256i r = _mm256_blend_epi32(pb0, pa0, 0x49);
    r = _mm256_blend_epi32(r, pc0, 0x24);
    _mm256_storeu_si256((__m256i *)(out + 3 * i), r);
    __m256i pa1 = _mm256_permutevar8x32_epi32(va, idx345);
    __m256i pb1 = _mm256_permutevar8x32_epi32(vb, idx345);
    __m256i pc1 = _mm256_permutevar8x32_epi32(vc, idx234);
    r = _mm256_blend_epi32(pb1, pa1, 0x92);
    r = _mm256_blend_epi32(r, pc1, 0x49);
    _mm256_storeu_si256((__m256i *)(out + 3 * i + 8), r);
    __m256i pa2 = _mm256_permutevar8x32_epi32(
        va, _mm256_setr_epi32(0, 0, 6, 0, 0, 7, 0, 0));
    __m256i pb2 = _mm256_permutevar8x32_epi32(vb, idx567);
    __m256i pc2 = _mm256_permutevar8x32_epi32(vc, idx567);
    r = _mm256_blend_epi32(pb2, pa2, 0x24);
    r = _mm256_blend_epi32(r, pc2, 0x92);
    _mm256_storeu_si256((__m256i *)(out + 3 * i + 16), r);
  }
  for (; i < n; ++i) {
    out[3 * i + 0] = a[i] + s0;
    out[3 * i + 1] = b[i] + s1;
    out[3 * i + 2] = c[i] + s2;
  }
}

#else
#endif

/* Row-wise 5/3 vertical synthesis of a horizontal-synthesized buffer.
   Low rows and high rows are combined along columns, processing each row
   contiguously (cache friendly). */
/* Row-wise 5/3 vertical synthesis of a horizontal-synthesized buffer.
   Low rows and high rows are combined along columns, processing each row
   contiguously (cache friendly). */
static void __attribute__((unused)) tdj_synth_53_vert_to(int32_t *dst, const int32_t *tmp, int out_w,
                                 int out_h, int n_low_y, int n_high_y,
                                 int even) {
  int i, k;
  int use_avx = tdj_cpu_avx2();
  if (out_h == 1) { /* single row: no vertical filtering */
    for (k = 0; k < out_w; ++k) dst[k] = tmp[k];
    return;
  }
  if (even) {
    for (i = 0; i < n_low_y; ++i) {
      int32_t *r0 = dst + (size_t)(2 * i) * out_w;
      const int32_t *s0 = tmp + (size_t)(2 * i) * out_w;
      const int32_t *up = tmp + (size_t)(2 * i - 1 >= 0 ? 2 * i - 1 : 1) * out_w;
      const int32_t *dn = tmp + (size_t)(2 * i + 1 < out_h ? 2 * i + 1 : 2 * i - 1) * out_w;
      if (use_avx) tdj_v53_update_even_to_avx2(r0, s0, up, dn, out_w);
      else for (k = 0; k < out_w; ++k) r0[k] = s0[k] - ((up[k] + dn[k] + 2) >> 2);
    }
    for (i = 0; i < n_high_y; ++i) {
      int32_t *r1 = dst + (size_t)(2 * i + 1) * out_w;
      const int32_t *s1 = tmp + (size_t)(2 * i + 1) * out_w;
      const int32_t *up = dst + (size_t)(2 * i) * out_w;
      const int32_t *dn = dst + (size_t)(2 * i + 2 < out_h ? 2 * i + 2 : 2 * i) * out_w;
      if (use_avx) tdj_v53_predict_even_to_avx2(r1, s1, up, dn, out_w);
      else for (k = 0; k < out_w; ++k) r1[k] = s1[k] + ((up[k] + dn[k]) >> 1);
    }
  } else {
    for (i = 0; i < n_low_y; ++i) {
      int32_t *r0 = dst + (size_t)(2 * i + 1) * out_w;
      const int32_t *s0 = tmp + (size_t)(2 * i + 1) * out_w;
      const int32_t *up = tmp + (size_t)(2 * i) * out_w;
      const int32_t *dn = tmp + (size_t)(2 * i + 2 < out_h ? 2 * i + 2 : 2 * i) * out_w;
      if (use_avx) tdj_v53_update_even_to_avx2(r0, s0, up, dn, out_w);
      else for (k = 0; k < out_w; ++k) r0[k] = s0[k] - ((up[k] + dn[k] + 2) >> 2);
    }
    for (i = 0; i < n_high_y; ++i) {
      int32_t *r1 = dst + (size_t)(2 * i) * out_w;
      const int32_t *s1 = tmp + (size_t)(2 * i) * out_w;
      const int32_t *up = dst + (size_t)(2 * i - 1 >= 0 ? 2 * i - 1 : 1) * out_w;
      const int32_t *dn = dst + (size_t)(2 * i + 1 < out_h ? 2 * i + 1 : 2 * i - 1) * out_w;
      if (use_avx) tdj_v53_predict_even_to_avx2(r1, s1, up, dn, out_w);
      else for (k = 0; k < out_w; ++k) r1[k] = s1[k] + ((up[k] + dn[k]) >> 1);
    }
  }
}

static void tdj_synth_53_load_row(const tdj_res *res, int row,
                                  const int32_t *ll_src, int ll_stride,
                                  int ll_w, int ll_h, int horz_even,
                                  int32_t *dst, int32_t *lcopy,
                                  int32_t *hcopy) {
  const tdj_band *hl = &res->bands[1];
  const tdj_band *lh = &res->bands[2];
  const tdj_band *hh = &res->bands[3];
  const int32_t *low, *high = NULL;
  int low_n = ll_w, high_n = hl->w;
  int y = row >> 1;
  int k;
  (void)ll_h;

  if ((row & 1) == 0) {
    low = ll_src + (size_t)y * ll_stride;
    if (y < hl->h) high = hl->samples + (size_t)y * hl->stride;
  } else {
    low = lh->samples + (size_t)y * lh->stride;
    high_n = hh->w;
    if (y < hh->h) high = hh->samples + (size_t)y * hh->stride;
  }
  for (k = 0; k < low_n; ++k) lcopy[k] = low[k];
  for (k = 0; k < high_n; ++k) hcopy[k] = high ? high[k] : 0;
  tdj_synth_53_1d(dst, lcopy, hcopy, low_n, high_n, horz_even);
}

/* Synthesize resolution `r` of component cp. `ll_src` is the input
   low-low (either the coarsest LL band or the previous resolution output),
   laid out with stride `ll_stride`. Result written to cp->recon. */
static int tdj_synth_res(tdj_comp *cp, int r,
                         const int32_t *ll_src, int ll_stride, int ll_w,
                         int ll_h, int32_t *tmp, int32_t *lcopy,
                         int32_t *hcopy, int32_t *dst) {
  tdj_res *res = &cp->res[r];
  int out_w = res->w, out_h = res->h;
  int vert_even = (res->y0 & 1) == 0;
  int horz_even = (res->x0 & 1) == 0;
  int n_low_y, n_high_y;

  if (r == 0) {
    int j, k;
    for (j = 0; j < out_h; ++j)
      for (k = 0; k < out_w; ++k)
        dst[j * out_w + k] = ll_src[j * ll_stride + k];
    return 1;
  }

  n_low_y = ll_h;
  n_high_y = res->bands[2].h;

  /* A bounded row ring is sufficient: the ping-pong destination is separate
     from the lower-resolution source, so vertical lifting can consume its
     neighboring horizontal rows without materializing a frame-sized temp. */
  if (out_h == 1) {
    tdj_synth_53_load_row(res, 0, ll_src, ll_stride, ll_w, ll_h,
                          horz_even, dst, lcopy, hcopy);
    return 1;
  }
  if (vert_even) {
    int i;
    for (i = 0; i < n_low_y; ++i) {
      int e = 2 * i, o = e + 1;
      int32_t *re = tmp;
      int32_t *rdn = tmp + (size_t)out_w;
      int32_t *rnext_raw = tmp + (size_t)2 * out_w;
      int32_t *rno = tmp + (size_t)3 * out_w;
      int32_t *rprev = tmp + (size_t)6 * out_w;
      int32_t *rnext_recon = tmp + (size_t)7 * out_w;
      if (i == 0) {
        tdj_synth_53_load_row(res, e, ll_src, ll_stride, ll_w, ll_h,
                              horz_even, re, lcopy, hcopy);
        if (o < out_h)
          tdj_synth_53_load_row(res, o, ll_src, ll_stride, ll_w, ll_h,
                                horz_even, rdn, lcopy, hcopy);
        else
          tdj_synth_53_load_row(res, e - 1, ll_src, ll_stride, ll_w, ll_h,
                                horz_even, rdn, lcopy, hcopy);
        if (e + 2 < out_h) {
          tdj_synth_53_load_row(res, e + 2, ll_src, ll_stride, ll_w, ll_h,
                                horz_even, rnext_raw, lcopy, hcopy);
          tdj_synth_53_load_row(res, e + 3 < out_h ? e + 3 : e + 1,
                                ll_src, ll_stride, ll_w, ll_h,
                                horz_even, rno, lcopy, hcopy);
        }
        if (e + 4 < out_h) {
          tdj_synth_53_load_row(res, e + 4, ll_src, ll_stride, ll_w, ll_h,
                                horz_even, tmp + (size_t)4 * out_w,
                                lcopy, hcopy);
          tdj_synth_53_load_row(res, e + 5 < out_h ? e + 5 : e + 3,
                                ll_src, ll_stride, ll_w, ll_h,
                                horz_even, tmp + (size_t)5 * out_w,
                                lcopy, hcopy);
        }
      } else {
        int k;
        for (k = 0; k < out_w; ++k) {
          re[k] = rnext_raw[k];
          rdn[k] = rno[k];
          rnext_raw[k] = tmp[(size_t)4 * out_w + k];
          rno[k] = tmp[(size_t)5 * out_w + k];
        }
        if (e + 4 < out_h) {
          tdj_synth_53_load_row(res, e + 4, ll_src, ll_stride, ll_w, ll_h,
                                horz_even, tmp + (size_t)4 * out_w,
                                lcopy, hcopy);
          tdj_synth_53_load_row(res, e + 5 < out_h ? e + 5 : e + 3,
                                ll_src, ll_stride, ll_w, ll_h,
                                horz_even, tmp + (size_t)5 * out_w,
                                lcopy, hcopy);
        }
      }
      {
        int32_t *outp = dst + (size_t)e * out_w;
        const int32_t *up = (i == 0) ? rdn : rprev;
        if (tdj_cpu_avx2()) tdj_v53_update_even_to_avx2(outp, re, up, rdn, out_w);
        else {
          int k; for (k = 0; k < out_w; ++k)
            outp[k] = re[k] - ((up[k] + rdn[k] + 2) >> 2);
        }
      }
      if (o < out_h) {
        int ne = e + 2;
        if (ne < out_h) {
          /* Keep the next even result in the ring.  The next iteration
             reloads its source row before that source is overwritten. */
          if (tdj_cpu_avx2()) tdj_v53_update_even_to_avx2(
              rnext_recon, rnext_raw, rdn, rno, out_w);
          else {
            int k; for (k = 0; k < out_w; ++k)
              rnext_recon[k] = rnext_raw[k] - ((rdn[k] + rno[k] + 2) >> 2);
          }
        } else {
          rnext_recon = dst + (size_t)e * out_w;
        }
        {
          int32_t *outp = dst + (size_t)o * out_w;
          const int32_t *next = (ne < out_h) ? rnext_recon : dst + (size_t)e * out_w;
          if (tdj_cpu_avx2()) tdj_v53_predict_even_to_avx2(outp, rdn,
                                                             dst + (size_t)e * out_w,
                                                             next, out_w);
          else {
            int k; for (k = 0; k < out_w; ++k)
              outp[k] = rdn[k] + ((dst[(size_t)e * out_w + k] + next[k]) >> 1);
          }
        }
      }
      for (int k = 0; k < out_w; ++k) rprev[k] = rdn[k];
    }
  } else {
    int i;
    for (i = 0; i < n_high_y; ++i) {
      int e = 2 * i, o = e + 1, ne = e + 2;
      int32_t *re = tmp;
      int32_t *ro = tmp + (size_t)out_w;
      int32_t *rne = tmp + (size_t)2 * out_w;
      tdj_synth_53_load_row(res, e, ll_src, ll_stride, ll_w, ll_h,
                            horz_even, re, lcopy, hcopy);
      tdj_synth_53_load_row(res, o, ll_src, ll_stride, ll_w, ll_h,
                            horz_even, ro, lcopy, hcopy);
      tdj_synth_53_load_row(res, ne < out_h ? ne : e, ll_src, ll_stride,
                            ll_w, ll_h, horz_even, rne, lcopy, hcopy);
      {
        int32_t *outp = dst + (size_t)o * out_w;
        const int32_t *up = re;
        const int32_t *dn = (ne < out_h) ? rne : re;
        if (tdj_cpu_avx2()) tdj_v53_update_even_to_avx2(outp, ro, up, dn, out_w);
        else {
          int k; for (k = 0; k < out_w; ++k)
            outp[k] = ro[k] - ((up[k] + dn[k] + 2) >> 2);
        }
      }
      {
        int32_t *outp = dst + (size_t)e * out_w;
        const int32_t *up = (e ? dst + (size_t)(e - 1) * out_w
                               : dst + (size_t)o * out_w);
        const int32_t *dn = dst + (size_t)o * out_w;
        if (tdj_cpu_avx2()) tdj_v53_predict_even_to_avx2(outp, re, up, dn, out_w);
        else {
          int k; for (k = 0; k < out_w; ++k)
            outp[k] = re[k] + ((up[k] + dn[k]) >> 1);
        }
      }
    }
  }
  return 1;
}

/* Synthesize the full component into cp->recon. */
static int tdj_synth_component(tdj_cs *cs, tdj_comp *cp) {
  int r;
  int max_side = cp->res[cp->num_decomps].w;
  if (cp->res[cp->num_decomps].h > max_side)
    max_side = cp->res[cp->num_decomps].h;
  int32_t *tmp, *lcopy, *hcopy;
  size_t need_tmp = (size_t)max_side * 8 * sizeof(int32_t);
  size_t need_row = (size_t)(max_side / 2 + 1) * sizeof(int32_t);
  if (cs->synth_tmp_cap < need_tmp) {
    void *nb = realloc(cs->synth_tmp, need_tmp);
    if (!nb) return 0;
    cs->synth_tmp = nb;
    cs->synth_tmp_cap = need_tmp;
  }
  if (cs->scratch32_cap < need_row * 2) {
    void *nb = realloc(cs->scratch32, need_row * 2);
    if (!nb) return 0;
    cs->scratch32 = (int32_t *)(void *)nb;
    cs->scratch32_cap = need_row * 2;
  }
  /* the buffers are only used as int32 scratch (fits in any 8-bit region) */
  tmp = (int32_t *)(void *)cs->synth_tmp;
  lcopy = (int32_t *)(void *)cs->scratch32;
  hcopy = (int32_t *)(void *)cs->scratch32 + need_row / sizeof(int32_t);
  if (cp->num_decomps == 0) {
    tdj_band *b = &cp->res[0].bands[0];
    int j, k;
    for (j = 0; j < cp->height; ++j)
      for (k = 0; k < cp->width; ++k)
        cp->recon[j * cp->width + k] = b->samples[j * b->stride + k];
    return 1;
  }
  for (r = 1; r <= (int)cp->num_decomps; ++r) {
    int ok;
    if (r == 1) {
      tdj_band *b = &cp->res[0].bands[0];
      {
        const char *env = getenv("TDJ_DUMP_LL");
        if (env) {
          FILE *f = fopen(env, "wb");
          if (f) {
            fwrite(b->samples, sizeof(int32_t), (size_t)b->w * b->h, f);
            fclose(f);
          }
        }
      }
      ok = tdj_synth_res(cp, r, b->samples, b->stride, b->w, b->h, tmp,
                         lcopy, hcopy, cp->recon);
    } else {
      tdj_res *prev = &cp->res[r - 1];
      ok = tdj_synth_res(cp, r, cp->recon, prev->w, prev->w, prev->h, tmp,
                         lcopy, hcopy, cp->recon_alt);
      if (ok) {
        int32_t *swap = cp->recon;
        cp->recon = cp->recon_alt;
        cp->recon_alt = swap;
      }
    }
    if (!ok) return 0;
    {
      const char *env = getenv("TDJ_DUMP_RECON");
      if (env) {
        char fn[128];
        snprintf(fn, sizeof(fn), "%s_res%d.bin", env, r);
        FILE *f = fopen(fn, "wb");
        if (f) {
          fwrite(cp->recon, sizeof(int32_t),
                 (size_t)cp->res[r].w * cp->res[r].h, f);
          fclose(f);
        }
      }
    }
  }
  return 1;
}

/* Synthesize the full component into cp->recon_f (irreversible 9/7). */
/* ------------------------------------------------------------------ */
/* Colour transform (RCT)                                              */
/* ------------------------------------------------------------------ */

/* Reversible colour transform inverse: Yuv -> RGB */
static void tdj_ict_rev(int32_t *c0, int32_t *c1, int32_t *c2, int n) {
  int i;
  for (i = 0; i < n; ++i) {
    int32_t y = c0[i], u = c1[i], v = c2[i];
    int32_t g = y - ((u + v) >> 2);
    int32_t r = v + g;
    int32_t b = u + g;
    c0[i] = r;
    c1[i] = g;
    c2[i] = b;
  }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

static int tdj_locate_tile_data(tdj_cs *cs, uint32_t tile_idx,
                                const ui8 **tile_data, size_t *tile_size) {
  /* scan for SOT markers; find the one with the matching tile index */
  const ui8 *p = cs->data;
  size_t left = cs->size;
  const ui8 *end = cs->data + cs->size;
  (void)cs;
  while (left >= 2) {
    if (p[0] == 0xFF && p[1] == 0x90 && left >= 10) {
      /* SOT: 8-byte header (after the 2-byte marker): Lsot, Isot, Psot, TPsot, TNsot */
      ui32 isot = tdj_rd16(p + 4);
      ui32 psot = tdj_rd32(p + 6);
      if (isot == tile_idx) {
        const ui8 *sod = NULL;
        size_t sod_len = 0;
        /* Psot counts from after the marker (i.e., includes the 8-byte
           header + SOD + data). Find SOD within the tile part. */
        if (psot == 0) psot = (ui32)(end - p); /* last tile part */
        {
          const ui8 *tp_end = p + psot;
          if (tp_end > end) tp_end = end;
          /* find SOD marker */
          {
            const ui8 *q = p + 10;
            while (q + 2 <= tp_end) {
              if (q[0] == 0xFF && q[1] == 0x93) {
                sod = q + 2;
                sod_len = (size_t)(tp_end - sod);
                break;
              }
              if (q[0] == 0xFF && (q[1] == 0x90 || q[1] == 0xD9)) break;
              /* skip marker segment */
              if (q[0] == 0xFF) {
                ui32 l;
                if (q + 4 > tp_end) break;
                l = tdj_rd16(q + 2);
                if (l < 2) break;
                q += 2 + l;
              } else {
                q++;
              }
            }
          }
        }
        if (sod) {
          *tile_data = sod;
          *tile_size = sod_len;
          return 1;
        }
      }
      {
        size_t step = psot ? psot : (size_t)(end - p);
        if (step > (size_t)(end - p)) step = (size_t)(end - p);
        p += step;
        left = (size_t)(end - p);
      }
    } else {
      p++;
      left--;
    }
  }
  return 0;
}

int tdng_j2k_open(tdng_j2k **out, const uint8_t *data, size_t size,
                  uint32_t *image_w, uint32_t *image_h, uint32_t *num_comps,
                  uint32_t *bitdepth_out, uint32_t *tile_w, uint32_t *tile_h,
                  uint32_t *num_tiles_x, uint32_t *num_tiles_y) {
  tdj_cs *cs;
  tdj_rd r;
  ui32 marker;
  int have_siz = 0, have_cod = 0;
  size_t i;

  tdj_is_le = !tdj_host_big();
  cs = (tdj_cs *)calloc(1, sizeof(tdj_cs));
  if (!cs) return TDNG_J2K_ERROR_MEMORY;
  cs->data = data;
  cs->size = size;

  r.p = data;
  r.left = size;
  if (!tdj_rd_u16(&r, &marker) || marker != TJ_SOC) goto corrupt;

  while (r.left >= 2) {
    ui32 m;
    /* look for a marker */
    if (r.p[0] != 0xFF) {
      r.p++;
      r.left--;
      continue;
    }
    m = tdj_rd16(r.p);
    if (m == TJ_EOC) break;
    if (!tdj_rd_u16(&r, &marker)) break;
    switch (marker) {
      case TJ_SIZ:
        if (!tdj_parse_siz(cs, &r)) goto corrupt;
        have_siz = 1;
        break;
      case TJ_COD:
        if (!tdj_parse_cod(cs, &r)) goto corrupt;
        have_cod = 1;
        break;
      case TJ_COC:
      case TJ_RGN:
      case TJ_POC:
      case TJ_CRG:
      case TJ_COM:
      case TJ_CAP:
      case TJ_PPM:
      case TJ_PLM:
      case TJ_TLM:
      case TJ_PPT:
      case TJ_PLT:
      case TJ_SOP:
      case TJ_EPH:
      case TJ_NLT:
      case TJ_DFS:
      case TJ_ATK:
      case TJ_CPF:
      case TJ_PRF:
        {
          ui32 l;
          if (!tdj_rd_u16(&r, &l)) goto corrupt;
          if (l < 2 || !tdj_skip(&r, l - 2)) goto corrupt;
        }
        break;
      case TJ_QCD:
        {
          ui32 l, sqcd, i;
          if (!tdj_rd_u16(&r, &l) || !tdj_rd_u8(&r, &sqcd)) goto corrupt;
          if (l < 3) goto corrupt;
          cs->qcd_guard = (sqcd >> 5) & 0x7;
          if ((sqcd & 0x1F) == 0) {
            ui32 n = l - 3;
            if (n > 97) goto corrupt;
            cs->qcd_count = n;
            for (i = 0; i < n; ++i) {
              ui32 v;
              if (!tdj_rd_u8(&r, &v)) goto corrupt;
              cs->qcd_bytes[i] = (ui8)v;
            }
          } else if ((sqcd & 0x1F) == 2) {
            ui32 n = (l - 3) / 2;
            if (n > 97) goto corrupt;
            cs->qcd_count = n;
            cs->qcd_expounded = 1;
            for (i = 0; i < n; ++i) {
              ui32 v;
              if (!tdj_rd_u16(&r, &v)) goto corrupt;
              cs->qcd_u16[i] = (ui16)v;
              cs->qcd_bytes[i] = (ui8)((v >> 8) & 0xFF);
            }
          } else {
            goto corrupt;
          }
        }
        break;
      case TJ_QCC:
        {
          ui32 l, cqcc, sqcd, i;
          if (!tdj_rd_u16(&r, &l) || !tdj_rd_u8(&r, &cqcc) ||
              !tdj_rd_u8(&r, &sqcd))
            goto corrupt;
          if (l < 4) goto corrupt;
          if (cqcc >= cs->num_comps || cqcc >= 16) goto corrupt;
          cs->qcc_guard[cqcc] = (sqcd >> 5) & 0x7;
          if ((sqcd & 0x1F) == 0) {
            ui32 n = l - 4;
            if (n > 97) goto corrupt;
            cs->qcc_count[cqcc] = n;
            for (i = 0; i < n; ++i) {
              ui32 v;
              if (!tdj_rd_u8(&r, &v)) goto corrupt;
              cs->qcc_bytes[cqcc][i] = (ui8)v;
            }
          } else if ((sqcd & 0x1F) == 2) {
            ui32 n = (l - 4) / 2;
            if (n > 97) goto corrupt;
            cs->qcc_count[cqcc] = n;
            cs->qcc_expounded[cqcc] = 1;
            for (i = 0; i < n; ++i) {
              ui32 v;
              if (!tdj_rd_u16(&r, &v)) goto corrupt;
              cs->qcc_u16[cqcc][i] = (ui16)v;
              cs->qcc_bytes[cqcc][i] = (ui8)((v >> 8) & 0xFF);
            }
          } else {
            goto corrupt;
          }
        }
        break;
      case TJ_SOT:
      case TJ_SOD:
        /* reached the tile data; stop header parsing */
        goto header_done;
      default:
        /* unknown marker: skip length-prefixed if possible */
        {
          ui32 l;
          if (r.left < 2) goto header_done;
          l = tdj_rd16(r.p);
          if (l < 2) goto corrupt;
          if (!tdj_skip(&r, 2 + l)) goto header_done;
        }
        break;
    }
    continue;
  }

header_done:
  if (!have_siz || !have_cod) goto corrupt;

  cs->lossy = !cs->reversible;

  *image_w = cs->xsiz - cs->xosiz;
  *image_h = cs->ysiz - cs->yosiz;
  *num_comps = cs->num_comps;
  *tile_w = cs->tile_w;
  *tile_h = cs->tile_h;
  *num_tiles_x = cs->num_tiles_x;
  *num_tiles_y = cs->num_tiles_y;
  for (i = 0; i < cs->num_comps; ++i)
    bitdepth_out[i] = (uint32_t)cs->comp_bits[i];

  *out = (tdng_j2k *)cs;
  return TDNG_J2K_OK;

corrupt:
  free(cs->comp_dx);
  free(cs->comp_dy);
  free(cs->comp_bits);
  free(cs->comp_signed);
  free(cs);
  return TDNG_J2K_ERROR_CORRUPT;
}

int tdng_j2k_is_lossy(tdng_j2k *j2k) {
  tdj_cs *cs = (tdj_cs *)j2k;
  return cs ? cs->lossy : 0;
}

void tdng_j2k_close(tdng_j2k *j2k) {
  tdj_cs *cs = (tdj_cs *)j2k;
  uint32_t c;
  if (!cs) return;
  free(cs->synth_tmp);
  free(cs->scratch32);
  if (cs->comps) {
    for (c = 0; c < cs->num_comps; ++c) {
      tdj_comp *cp = &cs->comps[c];
      int r, i;
      if (cp->res) {
        for (r = 0; r <= (int)cp->num_decomps; ++r) {
          int nb = (r == 0) ? 1 : 4;
          for (i = 0; i < nb; ++i) {
            tdj_band *b = &cp->res[r].bands[i];
            free(b->cbs);
            free(b->samples);
            free(b->samples_f);
          }
        }
        free(cp->res);
      }
      free(cp->recon);
      free(cp->recon_alt);
      free(cp->recon_f);
      free(cp->irv_tmp);
      free(cp->irv_line_a);
      free(cp->irv_line_b);
      free(cp->irv_vcol);
    }
  }
  free(cs->comps);
  free(cs->comp_dx);
  free(cs->comp_dy);
  free(cs->comp_bits);
  free(cs->comp_signed);
  free(cs);
}

int tdng_j2k_decode_tile(tdng_j2k *j2k, uint32_t tile_idx, int32_t *out) {
  tdj_cs *cs = (tdj_cs *)j2k;
  const ui8 *tile_data;
  size_t tile_size;
  uint32_t tx = tile_idx % cs->num_tiles_x;
  uint32_t ty = tile_idx / cs->num_tiles_x;
  int trx0, trx1, try0, try1;
  uint32_t c;

  if (!cs) return TDNG_J2K_ERROR_CORRUPT;
  if (tile_idx >= cs->num_tiles_x * cs->num_tiles_y)
    return TDNG_J2K_ERROR_CORRUPT;

  if (!tdj_locate_tile_data(cs, tile_idx, &tile_data, &tile_size))
    return TDNG_J2K_ERROR_CORRUPT;

  tdj_tile_rect(cs, (int)tx, (int)ty, &trx0, &try0, &trx1, &try1);

  /* (re)build per-tile component geometry */
  if (!cs->comps) {
    cs->comps = (tdj_comp *)calloc(cs->num_comps, sizeof(tdj_comp));
    if (!cs->comps) return TDNG_J2K_ERROR_MEMORY;
  }
  for (c = 0; c < cs->num_comps; ++c) {
    tdj_comp *cp = &cs->comps[c];
    int r, i;
    /* free previous tile's structures */
    if (cp->res) {
      for (r = 0; r <= (int)cp->num_decomps; ++r) {
        int nb = (r == 0) ? 1 : 4;
        for (i = 0; i < nb; ++i) {
          free(cp->res[r].bands[i].cbs);
          free(cp->res[r].bands[i].samples);
          free(cp->res[r].bands[i].samples_f);
          cp->res[r].bands[i].cbs = NULL;
          cp->res[r].bands[i].samples = NULL;
          cp->res[r].bands[i].samples_f = NULL;
        }
      }
      free(cp->res);
      cp->res = NULL;
    }
    free(cp->recon);
    free(cp->recon_alt);
    free(cp->recon_f);
    cp->recon = NULL;
    cp->recon_alt = NULL;
    cp->recon_f = NULL;
    cp->num_decomps = (int)cs->num_decomps;
    cp->dx = cs->comp_dx[c];
    cp->dy = cs->comp_dy[c];
    cp->bitdepth = cs->comp_bits[c];
    if (cs->qcc_count[c]) {
      memcpy(cp->qcd_bytes, cs->qcc_bytes[c], sizeof(cs->qcc_bytes[c]));
      memcpy(cp->qcd_u16, cs->qcc_u16[c], sizeof(cs->qcc_u16[c]));
      cp->qcd_count = cs->qcc_count[c];
      cp->qcd_guard = cs->qcc_guard[c];
      cp->qcd_expounded = cs->qcc_expounded[c];
    } else {
      tdj_qcd_apply_main(cs, cp);
    }
    if (!tdj_build_comp(cs, cp, (int)c, trx0, trx1, try0, try1)) {
      return TDNG_J2K_ERROR_MEMORY;
    }
  }

  /* decode all components: packets are ordered by progression. With a single
     precinct per resolution, RPCL/LRCP/RLCP all reduce to resolution-major
     component-major packet order. */
  {
    int nres = (int)cs->num_decomps + 1;
    int r;
    size_t off = 0;
    for (r = 0; r < nres; ++r) {
      for (c = 0; c < cs->num_comps; ++c) {
        size_t consumed;
        if (off >= tile_size) break;
        if (!tdj_parse_packet(cs, (int)c, r, tile_data + off,
                              tile_size - off, &consumed))
          return TDNG_J2K_ERROR_CORRUPT;
        off += consumed;
      }
    }
  }

   /* decode codeblocks and synthesize (DC level shift applied below) */
   for (c = 0; c < cs->num_comps; ++c) {
     tdj_comp *cp = &cs->comps[c];
     if (!tdj_decode_blocks(cs, cp)) return TDNG_J2K_ERROR_CORRUPT;
     if (cs->reversible) {
       if (!tdj_synth_component(cs, cp)) return TDNG_J2K_ERROR_CORRUPT;
     } else {
       if (!tdj_synth_component_irv(cp)) return TDNG_J2K_ERROR_CORRUPT;
     }
   }

   if (cs->reversible) {
      /* colour transform (before the DC level shift) */
      if (cs->mct && cs->num_comps >= 3 &&
          cs->comps[0].width == cs->comps[1].width &&
          cs->comps[0].height == cs->comps[1].height &&
          cs->comps[0].width == cs->comps[2].width &&
          cs->comps[0].height == cs->comps[2].height) {
       int w = cs->comps[0].width;
       int h = cs->comps[0].height;
       int32_t *c0 = cs->comps[0].recon;
       int32_t *c1 = cs->comps[1].recon;
       int32_t *c2 = cs->comps[2].recon;
       int y;
       for (y = 0; y < h; ++y)
         tdj_ict_rev(c0 + (size_t)y * w, c1 + (size_t)y * w,
                     c2 + (size_t)y * w, w);
     }
     /* DC level shift folded into the pack loop below. */
   } else {
      /* irreversible: colour transform + float-to-int conversion */
      if (cs->mct && cs->num_comps >= 3 &&
          cs->comps[0].width == cs->comps[1].width &&
          cs->comps[0].height == cs->comps[1].height &&
          cs->comps[0].width == cs->comps[2].width &&
          cs->comps[0].height == cs->comps[2].height) {
       int w = cs->comps[0].width;
       int h = cs->comps[0].height;
       float *c0 = cs->comps[0].recon_f;
       float *c1 = cs->comps[1].recon_f;
       float *c2 = cs->comps[2].recon_f;
       int y;
       for (y = 0; y < h; ++y) {
         int x;
         for (x = 0; x < w; ++x) {
           float yy = c0[y * w + x], u = c1[y * w + x], v = c2[y * w + x];
           float r = yy + 1.4019999504f * v;
           float g = yy - 0.7141362429f * v - 0.3441362679f * u;
           float b = yy + 1.7719999552f * u;
           c0[y * w + x] = r;
           c1[y * w + x] = g;
           c2[y * w + x] = b;
         }
       }
     }
     for (c = 0; c < cs->num_comps; ++c) {
       tdj_comp *cp = &cs->comps[c];
       size_t n = (size_t)cp->width * cp->height;
       size_t i;
        float mul = (float)(1ull << cp->bitdepth);
        int32_t half = (cs->comp_signed[c]) ? 0 : (1 << (cp->bitdepth - 1));
        int32_t neg_limit = -(1 << (cp->bitdepth - 1));
        float fl_low = (float)neg_limit;
        float fl_up = -(float)neg_limit;
        int32_t up_lim = (1 << (cp->bitdepth - 1)) - 1;
        for (i = 0; i < n; ++i) {
          float t = cp->recon_f[i] * mul;
          int32_t v = (int32_t)(t >= 0 ? t + 0.5f : t - 0.5f);
          v = t >= fl_low ? v : neg_limit;
          v = t < fl_up ? v : up_lim;
          cp->recon[i] = v + half;
        }
     }
   }

  /* pack interleaved output (with the DC level shift for unsigned comps) */
  {
    int max_w = cs->comps[0].width;
    int max_h = cs->comps[0].height;
    int same = 1;
    for (c = 1; c < cs->num_comps; ++c)
      if (cs->comps[c].width != max_w || cs->comps[c].height != max_h)
        same = 0;
    if (same && cs->reversible) {
      /* all components same size; process per pixel, all comps together */
      size_t n = (size_t)max_w * max_h;
      size_t i;
      int32_t shift[4];
      for (c = 0; c < cs->num_comps; ++c)
        shift[c] = (!cs->comp_signed[c]) ? (1 << (cs->comps[c].bitdepth - 1))
                                         : 0;
      /* lossy: the float->int conversion already applied the shift */
      if (!cs->reversible)
        for (c = 0; c < cs->num_comps; ++c) shift[c] = 0;
      if (cs->num_comps == 3 && tdj_cpu_avx2()) {
        tdj_pack3_avx2(out, cs->comps[0].recon, cs->comps[1].recon,
                       cs->comps[2].recon, (int)n, shift[0], shift[1],
                       shift[2]);
      } else if (cs->num_comps == 1) {
        int32_t *r0 = cs->comps[0].recon;
        int32_t s0 = shift[0];
        for (i = 0; i < n; ++i) out[i] = r0[i] + s0;
      } else if (cs->num_comps == 3) {
        int32_t *r0 = cs->comps[0].recon, *r1 = cs->comps[1].recon,
                *r2 = cs->comps[2].recon;
        int32_t s0 = shift[0], s1 = shift[1], s2 = shift[2];
        for (i = 0; i < n; ++i) {
          out[3 * i + 0] = r0[i] + s0;
          out[3 * i + 1] = r1[i] + s1;
          out[3 * i + 2] = r2[i] + s2;
        }
      } else {
        for (i = 0; i < n; ++i) {
          for (c = 0; c < cs->num_comps; ++c)
            out[i * cs->num_comps + c] = cs->comps[c].recon[i] + shift[c];
        }
      }
    } else {
      for (c = 0; c < cs->num_comps; ++c) {
        tdj_comp *cp = &cs->comps[c];
        int x, y;
        int32_t shift = (!cs->comp_signed[c] && cs->reversible)
                             ? (1 << (cp->bitdepth - 1))
                             : 0;
        for (y = 0; y < max_h; ++y) {
          for (x = 0; x < max_w; ++x) {
            int32_t v = 0;
            if (x < cp->width && y < cp->height)
              v = cp->recon[y * cp->width + x] + shift;
            out[((size_t)y * max_w + x) * cs->num_comps + c] = v;
          }
        }
      }
    }
  }

  return TDNG_J2K_OK;
}
