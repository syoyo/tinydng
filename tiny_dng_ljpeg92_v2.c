// Lossless JPEG (ITU T.81, Annex H / "JPEG-LS 1992") decoder & encoder.
//
// Public API lives in tiny_dng_ljpeg92_v2.h. All `tdng_lj92_*` symbols are
// defined here. The decoder fast path destuffs the entropy-coded bitstream
// once per tile and runs specialized inner loops per (predictor, components,
// linearize) combination; see parseScan() below. The encoder supports 1..4
// components and predictors 1/2/7 via tdng_lj92_encode_ex(); the legacy
// tdng_lj92_encode() entry point delegates to _ex with components=1,
// predictor=1.

#include "tiny_dng_ljpeg92_v2.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#if defined(TINY_DNG_LJPEG92_V2_USE_AVX2)
#include <immintrin.h>
#elif defined(TINY_DNG_LJPEG92_V2_USE_SSE2)
#include <emmintrin.h>
#endif

#define TINY_DNG_DPRINTF(...) ((void)0)

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

#define LJ92_MAX_COMPONENTS (16)

// A1: the bit reader refills in 8-byte blocks. On corrupt input, it may
// overshoot the real end of the entropy stream by up to LJ92_ENTROPY_PAD
// bytes before bitio_check_overrun declares corruption. The destuff
// allocator reserves (LJ92_ENTROPY_PAD + 8) trailing bytes so that the
// final 8-byte memcpy load is still inside the allocation even right at
// the threshold.
#define LJ92_ENTROPY_PAD (32)
#define LJ92_ENTROPY_ALLOC_PAD (LJ92_ENTROPY_PAD + 8)

typedef struct _ljp {
  u8* data;
  int datalen;
  int scanstart;
  int ix;
  int x;           // Width
  int y;           // Height
  int bits;        // Bit depth
  int components;  // Number of components (Nf)
  int skiplen;     // Pixels to skip after each output row
  u16* linearize;  // Linearization table (or NULL)
  int linlen;

  // Per-component Huffman lookup tables. Entry = (symbol << 8) | bitsused.
  u16* hufflut[LJ92_MAX_COMPONENTS];
  int  huffbits[LJ92_MAX_COMPONENTS];
  int  num_huff_idx;

  u16* image;       // Output target for current decode.
  u16* rowcache;    // outrow[0..1] backing storage.
  u16* diffcache;   // Per-row diff scratch (mono pred-1 SIMD path).
  u16* outrow[2];   // Ping-pong raw-sample buffers (linearize path).

  // Destuffed entropy-coded bitstream. Every 0xFF 0x00 pair in the source
  // becomes a single 0xFF here, followed by >=16 bytes of zero padding so
  // that the refill path can do unconditional 8-byte loads.
  u8* ebuf;
  int ebuf_len;
  int ebuf_cap;
} ljp;

// Left-aligned 64-bit bit buffer. `bb` holds the next up-to-`nbits` bits in
// the high bits; peeking N bits is `(u32)(bb >> (64 - N))`.
typedef struct {
  const u8* p;
  const u8* p_end;   // one past last real destuffed byte
  uint64_t bb;
  int nbits;         // valid bit count in bb, in [0, 64]
} bitio_t;

static inline void bitio_init(bitio_t* bio, const u8* p, const u8* p_end) {
  bio->p = p;
  bio->p_end = p_end;
  bio->bb = 0;
  bio->nbits = 0;
}

#if defined(__GNUC__) || defined(__clang__)
#define TDNG_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define TDNG_UNLIKELY(x) (x)
#endif

// Refill so that `nbits >= 32`. The destuffed buffer has LJ92_ENTROPY_PAD
// bytes of zeros past `p_end` plus 8 more bytes of allocation slack so that
// an 8-byte memcpy at `p == p_end + LJ92_ENTROPY_PAD` is still in-bounds.
// On corrupt inputs the decoder may keep consuming bits past the real end;
// once `p` crosses `p_end + LJ92_ENTROPY_PAD` we clamp (feed zero, mark the
// buffer "full" so refill becomes a no-op) and let the end-of-scan
// bitio_check_overrun declare corruption. One branch per refill, predicted
// false on every non-corrupt input.
static inline void bitio_refill(bitio_t* bio) {
  if (bio->nbits >= 32) return;
  if (TDNG_UNLIKELY(bio->p > bio->p_end + LJ92_ENTROPY_PAD)) {
    bio->nbits = 64;  // poison: refill becomes no-op; bits are whatever was left
    return;
  }
  uint64_t x;
  memcpy(&x, bio->p, 8);
#if defined(__GNUC__) || defined(__clang__)
  x = __builtin_bswap64(x);
#else
  x = ((x & 0xff00000000000000ULL) >> 56) |
      ((x & 0x00ff000000000000ULL) >> 40) |
      ((x & 0x0000ff0000000000ULL) >> 24) |
      ((x & 0x000000ff00000000ULL) >> 8)  |
      ((x & 0x00000000ff000000ULL) << 8)  |
      ((x & 0x0000000000ff0000ULL) << 24) |
      ((x & 0x000000000000ff00ULL) << 40) |
      ((x & 0x00000000000000ffULL) << 56);
#endif
  bio->bb |= x >> bio->nbits;
  int consumed = (64 - bio->nbits) >> 3;
  bio->p += consumed;
  bio->nbits += consumed << 3;
}

static inline int bitio_check_overrun(const bitio_t* bio) {
  // `p` advances in 8-byte blocks during refill; LJ92_ENTROPY_PAD bytes of
  // zero-padded space past p_end make those loads safe. Overshoot past the
  // padding means we ran off the end of the real data.
  return bio->p > bio->p_end + LJ92_ENTROPY_PAD ? 1 : 0;
}

// Fused Huffman + ssss-value decode with branchless sign-extension.
// Assumes bitbuf already refilled to nbits >= 32 and maxbits <= 16.
// ssss + maxbits <= 32 in the worst case so no second refill is needed.
static inline int bitio_decode_diff(bitio_t* bio, const u16* hufflut,
                                    int maxbits) {
  uint32_t idx = (uint32_t)(bio->bb >> (64 - maxbits));
  uint16_t e = hufflut[idx];
  int used = e & 0xff;
  int ssss = e >> 8;
  bio->bb <<= used;
  bio->nbits -= used;
  if (ssss == 0) return 0;
  uint32_t v = (uint32_t)(bio->bb >> (64 - ssss));
  bio->bb <<= ssss;
  bio->nbits -= ssss;
  int m = 1 << ssss;
  int half = m >> 1;
  int sign = ((int)v - half) >> 31;     // 0 or -1
  int diff = (int)v + (sign & (1 - m)); // sign-extend JPEG-style
  return diff;
}

static int find(ljp* self) {
  int ix = self->ix;
  u8* data = self->data;
  while (ix < (self->datalen - 1)) {
    if (data[ix] == 0xFF) {
      if (data[ix + 1] != 0xFF && data[ix + 1] != 0x00) {
        self->ix = ix + 2;
        return data[ix + 1];
      }
    }
    ix++;
  }
  return -1;
}

// swap endian
#define BEH(ptr) ((((int)(*&ptr)) << 8) | (*(&ptr + 1)))

// Parse DHT: build a direct LUT of 2^maxbits entries, each holding
// (symbol << 8) | code_length. The caller peeks maxbits of the bit stream
// and indexes in to recover the decoded Huffman symbol plus the number of
// bits actually consumed. All reads past the marker header are bounds
// checked against datalen, and every symbol is validated to be <= 16 so
// that `1 << ssss` in the decode path cannot overflow.
static int parseHuff(ljp* self) {
  // A2: need at least 2 bytes for Lh.
  if (self->ix + 2 > self->datalen) return TDNG_LJ92_ERROR_CORRUPT;
  u8* huffhead = &self->data[self->ix];
  u8* bits = &huffhead[2];
  int hufflen = BEH(huffhead[0]);
  // A5: DHT payload is Lh + Tc/Th(1) + L[1..16] + V[]; need at least 19
  // bytes and must not run off the end.
  if (hufflen < 19) return TDNG_LJ92_ERROR_CORRUPT;
  if (self->ix + hufflen > self->datalen) return TDNG_LJ92_ERROR_CORRUPT;
  if (self->num_huff_idx >= LJ92_MAX_COMPONENTS) return TDNG_LJ92_ERROR_CORRUPT;
  bits[0] = 0;  // sentinel: no length-0 codes

  u8* huffvals = &self->data[self->ix + 19];
  int total_codes = 0;
  for (int L = 1; L <= 16; L++) total_codes += bits[L];
  // Allow hufflen >= 19 + total_codes to tolerate trailing padding some
  // encoders emit; reject only if the declared length is too short.
  if (hufflen - 19 < total_codes) return TDNG_LJ92_ERROR_CORRUPT;
  for (int i = 0; i < total_codes; i++) {
    if (huffvals[i] > 16) return TDNG_LJ92_ERROR_CORRUPT;  // SSSS in [0,16]
  }

  int maxbits = 16;
  while (maxbits > 0 && !bits[maxbits]) maxbits--;
  if (maxbits <= 0) return TDNG_LJ92_ERROR_CORRUPT;
  self->huffbits[self->num_huff_idx] = maxbits;

  size_t lut_entries = (size_t)1 << maxbits;
  u16* hufflut = (u16*)malloc(lut_entries * sizeof(u16));
  if (!hufflut) return TDNG_LJ92_ERROR_NO_MEMORY;
  self->hufflut[self->num_huff_idx] = hufflut;
  // Hardening: DHTs in real LJPEG streams may be Kraft-incomplete (some
  // prefixes have no assigned code). Pre-fill every slot with a safe
  // sentinel (ssss=0, used=1) so that if a corrupt bit stream peeks one of
  // the never-filled slots, the decoder still makes forward progress (one
  // bit consumed, zero diff) instead of looping with used=0. The
  // bitio_check_overrun at the end of each scan catches the resulting
  // bit-count mismatch. Only one memset — no per-sample cost.
  for (size_t s = 0; s < lut_entries; s++) hufflut[s] = 0x0001;

  int i = 0, hv = 0, rv = 0, vl = 0, bitsused = 1;
  while (i < (1 << maxbits)) {
    if (bitsused > maxbits) break;
    if (vl >= bits[bitsused]) { bitsused++; vl = 0; continue; }
    if (rv == (1 << (maxbits - bitsused))) { rv = 0; vl++; hv++; continue; }
    hufflut[i++] = (u16)((huffvals[hv] << 8) | bitsused);
    rv++;
  }
  self->num_huff_idx++;
  self->ix += hufflen;
  return TDNG_LJ92_ERROR_NONE;
}

static int parseSof3(ljp* self) {
  // SOF3: Lf(2) P(1) Y(2) X(2) Nf(1) [Ci Hi/Vi Tqi]*Nf.
  // A2: header alone is 8 bytes past the current ix.
  if (self->ix + 8 > self->datalen) return TDNG_LJ92_ERROR_CORRUPT;
  int Lf = BEH(self->data[self->ix]);
  if (Lf < 8 || self->ix + Lf > self->datalen) return TDNG_LJ92_ERROR_CORRUPT;

  self->bits = self->data[self->ix + 2];
  self->y = BEH(self->data[self->ix + 3]);
  self->x = BEH(self->data[self->ix + 5]);
  self->components = self->data[self->ix + 7];
  self->ix += Lf;

  // A4: bitdepth must be in [2,16] so that 1 << (bits-1) is defined.
  if (self->bits < 2 || self->bits > 16) return TDNG_LJ92_ERROR_CORRUPT;
  // A3: accept up to LJ92_MAX_COMPONENTS (decoder inner loops handle that).
  if (self->components < 1 || self->components > LJ92_MAX_COMPONENTS) {
    return TDNG_LJ92_ERROR_CORRUPT;
  }
  return TDNG_LJ92_ERROR_NONE;
}

static int parseBlock(ljp* self, int marker) {
  (void)marker;
  // A2: need 2 bytes for the segment length.
  if (self->ix + 2 > self->datalen) return TDNG_LJ92_ERROR_CORRUPT;
  int len = BEH(self->data[self->ix]);
  if (len < 2 || self->ix + len > self->datalen) return TDNG_LJ92_ERROR_CORRUPT;
  self->ix += len;
  return TDNG_LJ92_ERROR_NONE;
}

static void tdng_prefix_sum_u16_scalar(u16* dst, const u16* diff, int count,
                                       u16 seed) {
  u16 acc = seed;
  for (int i = 0; i < count; i++) {
    acc = (u16)(acc + diff[i]);
    dst[i] = acc;
  }
}

#if defined(TINY_DNG_LJPEG92_V2_USE_SSE2) || defined(TINY_DNG_LJPEG92_V2_USE_AVX2)
static void tdng_prefix_sum_u16_sse2(u16* dst, const u16* diff, int count,
                                     u16 seed) {
  u16 carry = seed;
  int i = 0;
  for (; (i + 8) <= count; i += 8) {
    __m128i v = _mm_loadu_si128((const __m128i*)(diff + i));
    __m128i s = v;
    __m128i t = _mm_slli_si128(s, 2);
    s = _mm_add_epi16(s, t);
    t = _mm_slli_si128(s, 4);
    s = _mm_add_epi16(s, t);
    t = _mm_slli_si128(s, 8);
    s = _mm_add_epi16(s, t);
    s = _mm_add_epi16(s, _mm_set1_epi16((short)carry));
    _mm_storeu_si128((__m128i*)(dst + i), s);
    carry = (u16)_mm_extract_epi16(s, 7);
  }
  if (i < count) {
    tdng_prefix_sum_u16_scalar(dst + i, diff + i, count - i, carry);
  }
}
#endif

#if defined(TINY_DNG_LJPEG92_V2_USE_AVX2)
static void tdng_prefix_sum_u16_avx2(u16* dst, const u16* diff, int count,
                                     u16 seed) {
  const __m256i upper_mask =
      _mm256_setr_epi16(0, 0, 0, 0, 0, 0, 0, 0, -1, -1, -1, -1, -1, -1, -1,
                        -1);
  u16 carry = seed;
  int i = 0;
  for (; (i + 16) <= count; i += 16) {
    __m256i v = _mm256_loadu_si256((const __m256i*)(diff + i));
    __m256i s = v;
    __m256i t = _mm256_slli_si256(s, 2);
    s = _mm256_add_epi16(s, t);
    t = _mm256_slli_si256(s, 4);
    s = _mm256_add_epi16(s, t);
    t = _mm256_slli_si256(s, 8);
    s = _mm256_add_epi16(s, t);
    t = _mm256_and_si256(_mm256_set1_epi16((short)_mm_extract_epi16(
                                                _mm256_castsi256_si128(s), 7)),
                         upper_mask);
    s = _mm256_add_epi16(s, t);
    s = _mm256_add_epi16(s, _mm256_set1_epi16((short)carry));
    _mm256_storeu_si256((__m256i*)(dst + i), s);
    carry = (u16)_mm_extract_epi16(_mm256_extracti128_si256(s, 1), 7);
  }
  if (i < count) {
    tdng_prefix_sum_u16_sse2(dst + i, diff + i, count - i, carry);
  }
}
#endif

static void tdng_prefix_sum_u16(u16* dst, const u16* diff, int count, u16 seed) {
#if defined(TINY_DNG_LJPEG92_V2_USE_AVX2)
  tdng_prefix_sum_u16_avx2(dst, diff, count, seed);
#elif defined(TINY_DNG_LJPEG92_V2_USE_SSE2)
  tdng_prefix_sum_u16_sse2(dst, diff, count, seed);
#else
  tdng_prefix_sum_u16_scalar(dst, diff, count, seed);
#endif
}

// Copy the entropy-coded payload starting at `start_off` in self->data into
// self->ebuf, collapsing 0xFF 0x00 byte stuffing into a plain 0xFF and
// stopping at the first non-entropy marker (0xFF xx with xx in [01..FE]
// except 0xFF). Appends LJ92_ENTROPY_PAD bytes of zero padding so that the
// bit reader's unconditional 8-byte loads are always safe even when a
// corrupt stream makes the decoder over-read.
static int destuff_entropy_stream(ljp* self, int start_off) {
  if (start_off < 0 || start_off >= self->datalen) return TDNG_LJ92_ERROR_CORRUPT;
  // Hardening: cap_needed would overflow `int` for datalen close to INT_MAX.
  if (self->datalen > INT_MAX - LJ92_ENTROPY_ALLOC_PAD) return TDNG_LJ92_ERROR_CORRUPT;
  int remain = self->datalen - start_off;
  int cap_needed = remain + LJ92_ENTROPY_ALLOC_PAD;
  if (self->ebuf_cap < cap_needed) {
    u8* nb = (u8*)realloc(self->ebuf, (size_t)cap_needed);
    if (!nb) return TDNG_LJ92_ERROR_NO_MEMORY;
    self->ebuf = nb;
    self->ebuf_cap = cap_needed;
  }
  const u8* src = self->data + start_off;
  const u8* end = self->data + self->datalen;
  u8* dst = self->ebuf;
  while (src < end) {
    u8 c = *src++;
    *dst++ = c;
    if (c == 0xFF) {
      if (src >= end) break;
      u8 c2 = *src;
      if (c2 == 0x00) { src++; continue; }     // stuffed zero
      if (c2 == 0xFF) continue;                // fill byte — next iter re-checks
      dst--;                                   // real marker; drop the FF
      break;
    }
  }
  self->ebuf_len = (int)(dst - self->ebuf);
  // A1: zero the full safe-overshoot + 8-byte-read window. An 8-byte refill
  // at the threshold reads bytes [ebuf_len+PAD, ebuf_len+PAD+8) which must
  // all be in-bounds zeros.
  memset(dst, 0, (size_t)LJ92_ENTROPY_ALLOC_PAD);
  return TDNG_LJ92_ERROR_NONE;
}

// Generic specialized scan runner. `PRED` (0..7), `NC` (components) and `LIN`
// (linearize present: 0/1) are passed as `const` and inlined away, producing
// specialized code per (PRED, NC, LIN) via __attribute__((always_inline)).
#if defined(__GNUC__) || defined(__clang__)
#define TDNG_ALWAYS_INLINE inline __attribute__((always_inline))
#else
#define TDNG_ALWAYS_INLINE inline
#endif

static TDNG_ALWAYS_INLINE int parseScanRun(ljp* self, bitio_t* bio,
                                           const int PRED, const int NC,
                                           const int LIN) {
  const int W = self->x;
  const int H = self->y;
  const int out_stride = W * NC + self->skiplen;
  const int init_px = 1 << (self->bits - 1);
  const u16* lin = self->linearize;
  const int linlen = self->linlen;

  const u16* hl[LJ92_MAX_COMPONENTS];
  int hb[LJ92_MAX_COMPONENTS];
  if (self->num_huff_idx < 1) return TDNG_LJ92_ERROR_CORRUPT;
  for (int c = 0; c < NC; c++) {
    int idx = (c < self->num_huff_idx) ? c : 0;
    hl[c] = self->hufflut[idx];
    hb[c] = self->huffbits[idx];
    if (!hl[c]) return TDNG_LJ92_ERROR_CORRUPT;
  }

  u16* out = self->image;
  u16* rowbuf = NULL;         // used only when LIN: holds raw samples
  u16* lastraw = NULL;
  if (LIN) {
    rowbuf  = self->outrow[0];
    lastraw = self->outrow[1];
    if (!rowbuf || !lastraw) return TDNG_LJ92_ERROR_NO_MEMORY;
  }

  // --- Row 0: predictor 1 (DNG/JPEG lossless spec), per-col Px = left ---
  {
    // col 0: Px = 1 << (bits - 1) for each component
    int base = 0;
    for (int c = 0; c < NC; c++) {
      bitio_refill(bio);
      int d = bitio_decode_diff(bio, hl[c], hb[c]);
      int raw = (int)(uint16_t)(init_px + d);
      if (LIN) {
        if ((unsigned)raw > (unsigned)linlen) return TDNG_LJ92_ERROR_CORRUPT;
        rowbuf[base + c] = (u16)raw;
        out[base + c] = lin[raw];
      } else {
        out[base + c] = (u16)raw;
      }
    }
    for (int col = 1; col < W; col++) {
      int cur = col * NC;
      int prv = cur - NC;
      for (int c = 0; c < NC; c++) {
        bitio_refill(bio);
        int Px = LIN ? rowbuf[prv + c] : out[prv + c];
        int d = bitio_decode_diff(bio, hl[c], hb[c]);
        int raw = (int)(uint16_t)(Px + d);
        if (LIN) {
          if ((unsigned)raw > (unsigned)linlen) return TDNG_LJ92_ERROR_CORRUPT;
          rowbuf[cur + c] = (u16)raw;
          out[cur + c] = lin[raw];
        } else {
          out[cur + c] = (u16)raw;
        }
      }
    }
  }

  // --- Rows 1..H-1 ---
  for (int row = 1; row < H; row++) {
    const u16* prevraw;
    if (LIN) {
      u16* tmp = lastraw; lastraw = rowbuf; rowbuf = tmp;
      prevraw = lastraw;
    } else {
      prevraw = out;   // still pointing at the previous row; advance `out` below
    }
    u16* curout = out + out_stride;
    u16* currow = LIN ? rowbuf : curout;

    // col 0: predictor = above (lastrow[c]) for all predictors
    for (int c = 0; c < NC; c++) {
      bitio_refill(bio);
      int Px = prevraw[c];
      int d = bitio_decode_diff(bio, hl[c], hb[c]);
      int raw = (int)(uint16_t)(Px + d);
      if (LIN) {
        if ((unsigned)raw > (unsigned)linlen) return TDNG_LJ92_ERROR_CORRUPT;
        currow[c] = (u16)raw;
        curout[c] = lin[raw];
      } else {
        currow[c] = (u16)raw;
      }
    }

    // cols 1..W-1: predictor per PRED
    for (int col = 1; col < W; col++) {
      int cur = col * NC;
      int prv = cur - NC;
      for (int c = 0; c < NC; c++) {
        bitio_refill(bio);
        int left   = currow[prv + c];
        int above  = prevraw[cur + c];
        int abovel = prevraw[prv + c];
        int Px;
        switch (PRED) {
          case 0:  Px = 0; break;
          case 1:  Px = left; break;
          case 2:  Px = above; break;
          case 3:  Px = abovel; break;
          case 4:  Px = left + above - abovel; break;
          case 5:  Px = left + ((above - abovel) >> 1); break;
          case 6:  Px = above + ((left - abovel) >> 1); break;
          case 7:
          default: Px = (left + above) >> 1; break;
        }
        int d = bitio_decode_diff(bio, hl[c], hb[c]);
        int raw = (int)(uint16_t)(Px + d);
        if (LIN) {
          if ((unsigned)raw > (unsigned)linlen) return TDNG_LJ92_ERROR_CORRUPT;
          currow[cur + c] = (u16)raw;
          curout[cur + c] = lin[raw];
        } else {
          currow[cur + c] = (u16)raw;
        }
      }
    }

    out = curout;
  }

  if (bitio_check_overrun(bio)) return TDNG_LJ92_ERROR_CORRUPT;
  return TDNG_LJ92_ERROR_NONE;
}

// Instantiations for the hot combinations. (pred=1, nc=1) has its own
// dedicated path with SIMD prefix-sum below so isn't instantiated here.
static int parseScan_p1_n3_nolin(ljp* self, bitio_t* bio) { return parseScanRun(self, bio, 1, 3, 0); }
static int parseScan_p1_n3_lin  (ljp* self, bitio_t* bio) { return parseScanRun(self, bio, 1, 3, 1); }
static int parseScan_p7_n3_nolin(ljp* self, bitio_t* bio) { return parseScanRun(self, bio, 7, 3, 0); }
static int parseScan_p7_n3_lin  (ljp* self, bitio_t* bio) { return parseScanRun(self, bio, 7, 3, 1); }
static int parseScan_p7_n1_nolin(ljp* self, bitio_t* bio) { return parseScanRun(self, bio, 7, 1, 0); }
static int parseScan_p7_n1_lin  (ljp* self, bitio_t* bio) { return parseScanRun(self, bio, 7, 1, 1); }

// Generic fallback: runtime PRED/NC via a switch but with new bit-IO.
// NC up to LJ92_MAX_COMPONENTS. PRED is passed at runtime.
static int parseScanGeneric(ljp* self, bitio_t* bio, int pred) {
  const int NC = self->components;
  if (NC < 1 || NC > LJ92_MAX_COMPONENTS) return TDNG_LJ92_ERROR_CORRUPT;
  const int W = self->x;
  const int H = self->y;
  const int out_stride = W * NC + self->skiplen;
  const int init_px = 1 << (self->bits - 1);
  const u16* lin = self->linearize;
  const int linlen = self->linlen;
  const int has_lin = (lin != NULL);

  const u16* hl[LJ92_MAX_COMPONENTS];
  int hb[LJ92_MAX_COMPONENTS];
  if (self->num_huff_idx < 1) return TDNG_LJ92_ERROR_CORRUPT;
  for (int c = 0; c < NC; c++) {
    int idx = (c < self->num_huff_idx) ? c : 0;
    hl[c] = self->hufflut[idx];
    hb[c] = self->huffbits[idx];
    if (!hl[c]) return TDNG_LJ92_ERROR_CORRUPT;
  }

  u16* out = self->image;
  u16* rowbuf  = self->outrow[0];
  u16* lastraw = self->outrow[1];

  // Row 0
  for (int c = 0; c < NC; c++) {
    bitio_refill(bio);
    int d = bitio_decode_diff(bio, hl[c], hb[c]);
    int raw = (int)(uint16_t)(init_px + d);
    rowbuf[c] = (u16)raw;
    if (has_lin) {
      if ((unsigned)raw > (unsigned)linlen) return TDNG_LJ92_ERROR_CORRUPT;
      out[c] = lin[raw];
    } else {
      out[c] = (u16)raw;
    }
  }
  for (int col = 1; col < W; col++) {
    int cur = col * NC;
    int prv = cur - NC;
    for (int c = 0; c < NC; c++) {
      bitio_refill(bio);
      int Px = rowbuf[prv + c];
      int d = bitio_decode_diff(bio, hl[c], hb[c]);
      int raw = (int)(uint16_t)(Px + d);
      rowbuf[cur + c] = (u16)raw;
      if (has_lin) {
        if ((unsigned)raw > (unsigned)linlen) return TDNG_LJ92_ERROR_CORRUPT;
        out[cur + c] = lin[raw];
      } else {
        out[cur + c] = (u16)raw;
      }
    }
  }
  out += out_stride;

  // Rows 1..H-1
  for (int row = 1; row < H; row++) {
    u16* tmp = lastraw; lastraw = rowbuf; rowbuf = tmp;

    // col 0: predictor = above
    for (int c = 0; c < NC; c++) {
      bitio_refill(bio);
      int d = bitio_decode_diff(bio, hl[c], hb[c]);
      int raw = (int)(uint16_t)(lastraw[c] + d);
      rowbuf[c] = (u16)raw;
      if (has_lin) {
        if ((unsigned)raw > (unsigned)linlen) return TDNG_LJ92_ERROR_CORRUPT;
        out[c] = lin[raw];
      } else {
        out[c] = (u16)raw;
      }
    }
    for (int col = 1; col < W; col++) {
      int cur = col * NC;
      int prv = cur - NC;
      for (int c = 0; c < NC; c++) {
        bitio_refill(bio);
        int left   = rowbuf[prv + c];
        int above  = lastraw[cur + c];
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
        int d = bitio_decode_diff(bio, hl[c], hb[c]);
        int raw = (int)(uint16_t)(Px + d);
        rowbuf[cur + c] = (u16)raw;
        if (has_lin) {
          if ((unsigned)raw > (unsigned)linlen) return TDNG_LJ92_ERROR_CORRUPT;
          out[cur + c] = lin[raw];
        } else {
          out[cur + c] = (u16)raw;
        }
      }
    }
    out += out_stride;
  }

  if (bitio_check_overrun(bio)) return TDNG_LJ92_ERROR_CORRUPT;
  return TDNG_LJ92_ERROR_NONE;
}

static int parseScanPred1MonoFast(ljp* self, bitio_t* bio) {
  u16* out = self->image;
  u16* thisrow = self->outrow[0];
  u16* lastrow = self->outrow[1];

  if (self->num_huff_idx < 1) return TDNG_LJ92_ERROR_CORRUPT;
  if (!self->diffcache) return TDNG_LJ92_ERROR_NO_MEMORY;

  const u16* hl = self->hufflut[0];
  const int hb = self->huffbits[0];
  if (!hl) return TDNG_LJ92_ERROR_CORRUPT;

  const int W = self->x;
  const u16* lin = self->linearize;
  const int linlen = self->linlen;

  for (int row = 0; row < self->y; row++) {
    int first_px = (row == 0) ? (1 << (self->bits - 1)) : (int)lastrow[0];

    // Decode W diffs into diffcache with the new bit IO.
    for (int col = 0; col < W; col++) {
      bitio_refill(bio);
      self->diffcache[col] = (u16)bitio_decode_diff(bio, hl, hb);
    }

    tdng_prefix_sum_u16(thisrow, self->diffcache, W, (u16)first_px);

    if (lin) {
      for (int col = 0; col < W; col++) {
        if (thisrow[col] > linlen) return TDNG_LJ92_ERROR_CORRUPT;
        out[col] = lin[thisrow[col]];
      }
    } else {
      memcpy(out, thisrow, (size_t)W * sizeof(u16));
    }

    { u16* t = lastrow; lastrow = thisrow; thisrow = t; }
    out += W + self->skiplen;
  }

  if (bitio_check_overrun(bio)) return TDNG_LJ92_ERROR_CORRUPT;
  return TDNG_LJ92_ERROR_NONE;
}

static int parseScan(ljp* self) {
  self->ix = self->scanstart;
  // Hardening: SOS = Ls(2) Ns(1) [Cs Td/Ta]*Ns Ss Se Ah/Al.
  // Need at least 3 bytes to read Ls and Ns.
  if (self->ix + 3 > self->datalen) return TDNG_LJ92_ERROR_CORRUPT;
  int Ls = BEH(self->data[self->ix]);
  int compcount = self->data[self->ix + 2];
  if (Ls < 6 + 2 * compcount || self->ix + Ls > self->datalen) {
    return TDNG_LJ92_ERROR_CORRUPT;
  }
  if (compcount < 1 || compcount > LJ92_MAX_COMPONENTS) {
    return TDNG_LJ92_ERROR_CORRUPT;
  }
  // If SOF3 has been seen, SOS must name the same number of components.
  if (self->components > 0 && compcount != self->components) {
    return TDNG_LJ92_ERROR_CORRUPT;
  }
  int pred = self->data[self->ix + 3 + 2 * compcount];
  if (pred < 0 || pred > 7) return TDNG_LJ92_ERROR_CORRUPT;

  self->ix += Ls;

  // Degenerate scan (no SOF3, zero dims): preserve legacy "silent success"
  // behavior so callers probing format support get TDNG_LJ92_ERROR_NONE with no
  // output written.
  if (self->x <= 0 || self->y <= 0 || self->components <= 0) {
    return TDNG_LJ92_ERROR_NONE;
  }

  // Destuff the entropy-coded payload into self->ebuf so the inner loops
  // can use bulk 8-byte loads without per-byte 0xFF checks.
  int ret = destuff_entropy_stream(self, self->ix);
  if (ret != TDNG_LJ92_ERROR_NONE) return ret;

  bitio_t bio;
  bitio_init(&bio, self->ebuf, self->ebuf + self->ebuf_len);

  const int NC = self->components;
  const int LIN = (self->linearize != NULL) ? 1 : 0;

  // Mono predictor-1 keeps its dedicated path so the SIMD prefix-sum
  // (SSE2/AVX2 builds) still kicks in. LIN is handled inside that path.
  if (pred == 1 && NC == 1) {
    return parseScanPred1MonoFast(self, &bio);
  }

  // Common interleaved combinations get compile-time-specialized inline
  // runners via always_inline + constant propagation.
  if (NC == 1) {
    if (pred == 7) return LIN ? parseScan_p7_n1_lin(self, &bio)
                              : parseScan_p7_n1_nolin(self, &bio);
  } else if (NC == 3) {
    if (pred == 1) return LIN ? parseScan_p1_n3_lin(self, &bio)
                              : parseScan_p1_n3_nolin(self, &bio);
    if (pred == 7) return LIN ? parseScan_p7_n3_lin(self, &bio)
                              : parseScan_p7_n3_nolin(self, &bio);
  }

  // Generic fallback: runtime pred & NC, still uses the new bit-IO.
  return parseScanGeneric(self, &bio, pred);
}

static int parseImage(ljp* self) {
  // TINY_DNG_DPRINTF("parseImage\n");
  int ret = TDNG_LJ92_ERROR_NONE;
  while (1) {
    int nextMarker = find(self);
    TINY_DNG_DPRINTF("marker = 0x%08x\n", nextMarker);
    if (nextMarker == 0xc4) {
      TINY_DNG_DPRINTF("Parse huffman table.\n");
      ret = parseHuff(self);
    } else if (nextMarker == 0xc3) {
      ret = parseSof3(self);
    } else if (nextMarker == 0xfe) {  // Comment
      ret = parseBlock(self, nextMarker);
    } else if (nextMarker == 0xd9) {  // End of image
      break;
    } else if (nextMarker == 0xda) {
      self->scanstart = self->ix;
      ret = TDNG_LJ92_ERROR_NONE;
      break;
    } else if (nextMarker == -1) {
      ret = TDNG_LJ92_ERROR_CORRUPT;
      break;
    } else
      ret = parseBlock(self, nextMarker);
    if (ret != TDNG_LJ92_ERROR_NONE) break;
  }
  return ret;
}

static int findSoI(ljp* self) {
  int ret = TDNG_LJ92_ERROR_CORRUPT;
  if (find(self) == 0xd8) {
    ret = parseImage(self);
  } else {
    TINY_DNG_DPRINTF("findSoI: corrupt\n");
  }
  return ret;
}

static void free_memory(ljp* self) {
  for (int i = 0; i < self->num_huff_idx; i++) {
    free(self->hufflut[i]);
    self->hufflut[i] = NULL;
  }
  free(self->rowcache);
  self->rowcache = NULL;
  free(self->diffcache);
  self->diffcache = NULL;
  free(self->ebuf);
  self->ebuf = NULL;
  self->ebuf_len = 0;
  self->ebuf_cap = 0;
}

int tdng_lj92_open(tdng_lj92* lj, const uint8_t* data, int datalen, int* width,
                   int* height, int* bitdepth, int* components) {
  ljp* self = (ljp*)calloc(sizeof(ljp), 1);
  if (!self) return TDNG_LJ92_ERROR_NO_MEMORY;

  self->data = (u8*)data;
  self->datalen = datalen;

  int ret = findSoI(self);

  if (ret == TDNG_LJ92_ERROR_NONE && self->x > 0 && self->components > 0) {
    // Hardening: SOF3 width/height are 16-bit so x,y <= 65535 and
    // components <= LJ92_MAX_COMPONENTS; row_slots cannot exceed
    // 65535 * 16 = ~1M. Guard the multiplication anyway in case the struct
    // fields ever widen.
    if (self->x > 0xFFFF || self->components > LJ92_MAX_COMPONENTS) {
      ret = TDNG_LJ92_ERROR_CORRUPT;
    } else {
      size_t row_slots = (size_t)self->x * (size_t)self->components;
      self->rowcache  = (u16*)calloc(row_slots * 2, sizeof(u16));
      self->diffcache = (u16*)calloc(row_slots,     sizeof(u16));
      if (!self->rowcache || !self->diffcache) {
        ret = TDNG_LJ92_ERROR_NO_MEMORY;
      } else {
        self->outrow[0] = self->rowcache;
        self->outrow[1] = self->rowcache + row_slots;
      }
    }
  }

  if (ret != TDNG_LJ92_ERROR_NONE) {
    *lj = NULL;
    free_memory(self);
    free(self);
  } else {
    *width = self->x;
    *height = self->y;
    *bitdepth = self->bits;
    *components = self->components;
    *lj = self;
  }
  return ret;
}

int tdng_lj92_decode(tdng_lj92 lj, uint16_t* target, int writeLength,
                     int skipLength, uint16_t* linearize,
                     int linearizeLength) {
  (void)writeLength;  // reserved; legacy parsePred6 row-chunking was removed
  ljp* self = lj;
  if (!self) return TDNG_LJ92_ERROR_BAD_HANDLE;
  self->image = target;
  self->skiplen = skipLength;
  self->linearize = linearize;
  self->linlen = linearizeLength;
  return parseScan(self);
}

void tdng_lj92_close(tdng_lj92 lj) {
  ljp* self = lj;
  if (self != NULL) free_memory(self);
  free(self);
}

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------
//
// Lossless JPEG (ITU T.81, Annex H) encoder. Produces a single-DHT stream
// shared across all components (Td=0 for every component in SOS). The bit
// stream is emitted MSB-first, with 0xFF 0x00 byte stuffing.

#define LJ92_MAX_SSSS     17   // SSSS values 0..16 plus spec sentinel.

typedef struct {
  uint16_t* image;
  int width;
  int height;
  int bitdepth;
  int components;
  int predictor;
  int readLength;
  int skipLength;
  uint16_t* delinearize;
  int delinearizeLength;

  uint8_t* encoded;
  int encodedCap;
  int encodedWritten;

  // Frequency of SSSS symbols (0..16) across all components.
  int hist[LJ92_MAX_SSSS];
  // Canonical Huffman table derived from hist.
  int bits[17];          // bits[L] = number of codes of length L (1..16).
  uint8_t huffval[17];   // symbols ordered by code length.
  int huffval_count;
  uint16_t huffenc[LJ92_MAX_SSSS];  // code for symbol s.
  uint8_t  huffbits[LJ92_MAX_SSSS]; // code length for symbol s.

  // Bit-output accumulator: bits packed MSB-first into the top of bitbuf.
  uint32_t bitbuf;
  int nbits;
} lje;

static int clz32(unsigned int x) {
#if defined(__GNUC__) || defined(__clang__)
  return x ? __builtin_clz(x) : 32;
#else
  int n = 0;
  if (!x) return 32;
  while ((x & 0x80000000u) == 0) { n++; x <<= 1; }
  return n;
#endif
}

static int enc_px_from_neighbors(int pred, int left, int above, int abovel,
                                 int initpx, int row, int col) {
  if (row == 0 && col == 0) return initpx;
  if (row == 0)             return left;     // left predictor
  if (col == 0)             return above;    // above predictor
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

static int enc_ssss(int diff) {
  // DNG allows diff in (-32768, 32767]; the "modulo 65536" rule is folded
  // into diff by the caller so diff here fits in int range.
  int a = diff < 0 ? -diff : diff;
  return a == 0 ? 0 : (32 - clz32((unsigned int)a));
}

static int enc_reserve(lje* self, size_t extra) {
  size_t need = (size_t)self->encodedWritten + extra;
  if (need <= (size_t)self->encodedCap) return TDNG_LJ92_ERROR_NONE;
  size_t cap = self->encodedCap ? (size_t)self->encodedCap : 4096u;
  while (cap < need) {
    if (cap > (size_t)INT_MAX / 2) { cap = need; break; }
    cap *= 2;
  }
  if (cap > (size_t)INT_MAX) return TDNG_LJ92_ERROR_TOO_WIDE;
  uint8_t* nb = (uint8_t*)realloc(self->encoded, cap);
  if (!nb) return TDNG_LJ92_ERROR_NO_MEMORY;
  self->encoded = nb;
  self->encodedCap = (int)cap;
  return TDNG_LJ92_ERROR_NONE;
}

static void enc_put_u8(lje* self, uint8_t b) {
  self->encoded[self->encodedWritten++] = b;
}

// Emit one entropy-coded byte with 0xFF 0x00 stuffing.
static void enc_put_stuffed(lje* self, uint8_t b) {
  self->encoded[self->encodedWritten++] = b;
  if (b == 0xFF) self->encoded[self->encodedWritten++] = 0x00;
}

// Emit `nbits` bits from the low bits of `v` into the bit stream.
// Caller must have reserved space for up to (nbits/8 + 2) bytes.
static void enc_put_bits(lje* self, uint32_t v, int nbits) {
  self->bitbuf |= (v & ((1u << nbits) - 1u)) << (32 - self->nbits - nbits);
  self->nbits += nbits;
  while (self->nbits >= 8) {
    uint8_t b = (uint8_t)(self->bitbuf >> 24);
    enc_put_stuffed(self, b);
    self->bitbuf <<= 8;
    self->nbits -= 8;
  }
}

// Pad the remaining bits with 1s per JPEG spec and flush.
static void enc_flush_bits(lje* self) {
  if (self->nbits > 0) {
    uint8_t pad = (uint8_t)((1u << (8 - self->nbits)) - 1u);
    uint8_t b = (uint8_t)(self->bitbuf >> 24) | pad;
    enc_put_stuffed(self, b);
    self->bitbuf = 0;
    self->nbits = 0;
  }
}

// Single-pass histogram scan using the configured predictor. Caches two
// reconstructed rows worth of raw samples so the predictor sees the same
// values the decoder will.
static int enc_frequency_scan(lje* self) {
  const int W = self->width, H = self->height, NC = self->components;
  const int initpx = 1 << (self->bitdepth - 1);
  const int maxval = 1 << self->bitdepth;

  size_t row_slots = (size_t)W * (size_t)NC;
  uint16_t* rc = (uint16_t*)calloc(row_slots * 2, sizeof(uint16_t));
  if (!rc) return TDNG_LJ92_ERROR_NO_MEMORY;
  uint16_t* thisrow = rc;
  uint16_t* lastrow = rc + row_slots;

  const uint16_t* pix = self->image;
  int scan = self->readLength;

  for (int row = 0; row < H; row++) {
    for (int col = 0; col < W; col++) {
      for (int c = 0; c < NC; c++) {
        uint16_t p = *pix++;
        if (self->delinearize) {
          if (p >= self->delinearizeLength) { free(rc); return TDNG_LJ92_ERROR_TOO_WIDE; }
          p = self->delinearize[p];
        }
        if (p >= maxval) { free(rc); return TDNG_LJ92_ERROR_TOO_WIDE; }

        int base = col * NC;
        int prv  = base - NC;
        int left   = (col > 0) ? thisrow[prv + c] : 0;
        int above  = (row > 0) ? lastrow[base + c] : 0;
        int abovel = (row > 0 && col > 0) ? lastrow[prv + c] : 0;
        int Px = enc_px_from_neighbors(self->predictor, left, above, abovel,
                                       initpx, row, col);
        int diff = (int)p - Px;
        diff = (int16_t)diff;  // modulo 2^16 to match decoder arithmetic
        int ssss = enc_ssss(diff);
        self->hist[ssss]++;
        thisrow[base + c] = p;

        if (--scan == 0) {
          pix += self->skipLength;
          scan = self->readLength;
        }
      }
    }
    uint16_t* t = lastrow; lastrow = thisrow; thisrow = t;
  }
  free(rc);
  return TDNG_LJ92_ERROR_NONE;
}

// Limited-length Huffman table generation (ITU T.81, Annex K.2). Outputs
// self->bits[L], self->huffval[], self->huffenc[s], self->huffbits[s].
static void enc_build_huffman_table(lje* self) {
  // Annex K uses an extra "reserved" symbol at index 17 with freq 1 so that
  // the longest real code is at most 16 bits.
  float freq[18];
  int codesize[18];
  int others[18];
  for (int i = 0; i < 18; i++) {
    freq[i] = 0.0f;
    codesize[i] = 0;
    others[i] = -1;
  }
  int total = 0;
  for (int s = 0; s < LJ92_MAX_SSSS; s++) total += self->hist[s];
  if (total > 0) {
    for (int s = 0; s < LJ92_MAX_SSSS; s++) {
      if (self->hist[s] > 0) freq[s] = (float)self->hist[s] / (float)total;
    }
  }
  freq[17] = 1e-30f;  // reserved sentinel

  for (;;) {
    // Find v1 = lowest-freq non-zero entry, v2 = second lowest.
    int v1 = -1, v2 = -1;
    float f1 = 1e30f, f2 = 1e30f;
    for (int i = 0; i < 18; i++) {
      if (freq[i] > 0.0f && freq[i] <= f1) { f2 = f1; v2 = v1; f1 = freq[i]; v1 = i; }
      else if (freq[i] > 0.0f && freq[i] <= f2) { f2 = freq[i]; v2 = i; }
    }
    if (v2 < 0) break;  // only one symbol left
    freq[v1] += freq[v2];
    freq[v2] = 0.0f;
    for (;;) { codesize[v1]++; if (others[v1] < 0) break; v1 = others[v1]; }
    others[v1] = v2;
    for (;;) { codesize[v2]++; if (others[v2] < 0) break; v2 = others[v2]; }
  }

  int bits[33];
  for (int i = 0; i < 33; i++) bits[i] = 0;
  for (int i = 0; i < 18; i++) {
    if (codesize[i]) bits[codesize[i]]++;
  }

  // Annex K.3 adjustment: if any code is longer than 16 bits, redistribute
  // so that the longest code fits in 16 bits.
  for (int i = 32; i > 16; i--) {
    while (bits[i] > 0) {
      int j = i - 2;
      while (j > 0 && bits[j] == 0) j--;
      if (j == 0) break;
      bits[i] -= 2;
      bits[i - 1] += 1;
      bits[j + 1] += 2;
      bits[j] -= 1;
    }
  }
  // Remove the sentinel code (freed up by Annex K.3).
  for (int i = 16; i > 0; i--) {
    if (bits[i] > 0) { bits[i]--; break; }
  }

  for (int i = 0; i < 17; i++) self->bits[i] = bits[i];

  // Build huffval[] ordered by code length, then by symbol index.
  int k = 0;
  for (int L = 1; L <= 16; L++) {
    for (int s = 0; s < LJ92_MAX_SSSS; s++) {
      if (codesize[s] == L) self->huffval[k++] = (uint8_t)s;
    }
  }
  self->huffval_count = k;

  // Assign canonical codes in symbol order.
  for (int i = 0; i < LJ92_MAX_SSSS; i++) {
    self->huffenc[i] = 0;
    self->huffbits[i] = 0;
  }
  int code = 0;
  k = 0;
  for (int L = 1; L <= 16; L++) {
    for (int j = 0; j < bits[L]; j++) {
      uint8_t sym = self->huffval[k++];
      self->huffenc[sym]  = (uint16_t)code;
      self->huffbits[sym] = (uint8_t)L;
      code++;
    }
    code <<= 1;
  }
}

static int enc_write_header(lje* self) {
  const int NC = self->components;
  size_t need = 2 + 2 + 2 + 6 + 3 * NC;      // SOI + SOF3 + frame
  need += 2 + 2 + 1 + 16 + (size_t)self->huffval_count; // DHT
  need += 2 + 2 + 1 + 2 * NC + 3;            // SOS
  int ret = enc_reserve(self, need);
  if (ret != TDNG_LJ92_ERROR_NONE) return ret;

  enc_put_u8(self, 0xFF); enc_put_u8(self, 0xD8);  // SOI
  enc_put_u8(self, 0xFF); enc_put_u8(self, 0xC3);  // SOF3
  int Lf = 8 + 3 * NC;
  enc_put_u8(self, (uint8_t)(Lf >> 8));
  enc_put_u8(self, (uint8_t)(Lf & 0xFF));
  enc_put_u8(self, (uint8_t)self->bitdepth);       // P
  enc_put_u8(self, (uint8_t)(self->height >> 8));  // Y
  enc_put_u8(self, (uint8_t)(self->height & 0xFF));
  enc_put_u8(self, (uint8_t)(self->width >> 8));   // X
  enc_put_u8(self, (uint8_t)(self->width & 0xFF));
  enc_put_u8(self, (uint8_t)NC);                   // Nf
  for (int c = 0; c < NC; c++) {
    enc_put_u8(self, (uint8_t)c);  // Ci
    enc_put_u8(self, 0x11);        // Hi=1, Vi=1
    enc_put_u8(self, 0);           // Tqi (unused for lossless)
  }

  enc_put_u8(self, 0xFF); enc_put_u8(self, 0xC4);  // DHT
  int Lh = 2 + 1 + 16 + self->huffval_count;
  enc_put_u8(self, (uint8_t)(Lh >> 8));
  enc_put_u8(self, (uint8_t)(Lh & 0xFF));
  enc_put_u8(self, 0);  // Tc=0 (DC/lossless), Th=0
  for (int L = 1; L <= 16; L++) enc_put_u8(self, (uint8_t)self->bits[L]);
  for (int i = 0; i < self->huffval_count; i++) enc_put_u8(self, self->huffval[i]);

  enc_put_u8(self, 0xFF); enc_put_u8(self, 0xDA);  // SOS
  int Ls = 3 + 2 * NC + 3;
  enc_put_u8(self, (uint8_t)(Ls >> 8));
  enc_put_u8(self, (uint8_t)(Ls & 0xFF));
  enc_put_u8(self, (uint8_t)NC);
  for (int c = 0; c < NC; c++) {
    enc_put_u8(self, (uint8_t)c);  // Csj
    enc_put_u8(self, 0);           // Tdj=0, Taj=0 (shared single DHT)
  }
  enc_put_u8(self, (uint8_t)self->predictor);  // Ss = predictor
  enc_put_u8(self, 0);                         // Se = 0
  enc_put_u8(self, 0);                         // Ah=0, Al=0 (point transform)
  return TDNG_LJ92_ERROR_NONE;
}

static int enc_write_body(lje* self) {
  const int W = self->width, H = self->height, NC = self->components;
  const int initpx = 1 << (self->bitdepth - 1);

  size_t row_slots = (size_t)W * (size_t)NC;
  uint16_t* rc = (uint16_t*)calloc(row_slots * 2, sizeof(uint16_t));
  if (!rc) return TDNG_LJ92_ERROR_NO_MEMORY;
  uint16_t* thisrow = rc;
  uint16_t* lastrow = rc + row_slots;

  const uint16_t* pix = self->image;
  int scan = self->readLength;
  self->bitbuf = 0;
  self->nbits = 0;

  // A6: upper bound on entropy-coded bytes. Per sample: up to 16 bits of
  // Huffman code + 16 bits of residual = 32 bits = 4 bytes, doubled to 8 to
  // account for worst-case 0xFF -> 0xFF 0x00 byte stuffing. Use size_t so
  // large tiles cannot overflow.
  size_t worst = (size_t)W * (size_t)H * (size_t)NC * 8u + 64u;
  int ret = enc_reserve(self, worst);
  if (ret != TDNG_LJ92_ERROR_NONE) { free(rc); return ret; }

  for (int row = 0; row < H; row++) {
    for (int col = 0; col < W; col++) {
      for (int c = 0; c < NC; c++) {
        uint16_t p = *pix++;
        if (self->delinearize) {
          // Hardening: defense in depth. enc_frequency_scan validates the
          // same indices first, so a mismatch here indicates a tampered
          // input array between the two passes.
          if (p >= self->delinearizeLength) { free(rc); return TDNG_LJ92_ERROR_TOO_WIDE; }
          p = self->delinearize[p];
        }
        int base = col * NC;
        int prv  = base - NC;
        int left   = (col > 0) ? thisrow[prv + c] : 0;
        int above  = (row > 0) ? lastrow[base + c] : 0;
        int abovel = (row > 0 && col > 0) ? lastrow[prv + c] : 0;
        int Px = enc_px_from_neighbors(self->predictor, left, above, abovel,
                                       initpx, row, col);
        int diff = (int16_t)((int)p - Px);
        int ssss = enc_ssss(diff);
        thisrow[base + c] = p;

        // Hardening: a symbol with no Huffman code would silently emit zero
        // bits and desynchronise the stream. Since frequency_scan ran first,
        // this cannot happen with well-formed input — treat as corruption.
        if (self->huffbits[ssss] == 0) { free(rc); return TDNG_LJ92_ERROR_CORRUPT; }
        enc_put_bits(self, self->huffenc[ssss], self->huffbits[ssss]);
        if (ssss > 0) {
          // Map negative diffs into JPEG extend form:
          //   diff in [-(2^ssss - 1), 2^ssss - 1]
          //   if diff < 0: emitted_bits = diff + (2^ssss - 1)
          //   else:        emitted_bits = diff
          uint32_t bits_val;
          if (diff < 0) bits_val = (uint32_t)(diff + (1 << ssss) - 1);
          else          bits_val = (uint32_t)diff;
          enc_put_bits(self, bits_val, ssss);
        }

        if (--scan == 0) {
          pix += self->skipLength;
          scan = self->readLength;
        }
      }
    }
    uint16_t* t = lastrow; lastrow = thisrow; thisrow = t;
  }

  enc_flush_bits(self);
  free(rc);
  return TDNG_LJ92_ERROR_NONE;
}

static int enc_write_eoi(lje* self) {
  int ret = enc_reserve(self, 2);
  if (ret != TDNG_LJ92_ERROR_NONE) return ret;
  enc_put_u8(self, 0xFF);
  enc_put_u8(self, 0xD9);
  return TDNG_LJ92_ERROR_NONE;
}

int tdng_lj92_encode_ex(uint16_t* image, int width, int height, int bitdepth,
                        int components, int predictor, int readLength,
                        int skipLength, uint16_t* delinearize,
                        int delinearizeLength, uint8_t** encoded,
                        int* encodedLength) {
  if (!image || !encoded || !encodedLength) return TDNG_LJ92_ERROR_BAD_HANDLE;
  if (width <= 0 || width > 0xFFFF) return TDNG_LJ92_ERROR_BAD_HANDLE;
  if (height <= 0 || height > 0xFFFF) return TDNG_LJ92_ERROR_BAD_HANDLE;
  if (bitdepth < 2 || bitdepth > 16) return TDNG_LJ92_ERROR_BAD_HANDLE;
  if (components < 1 || components > 4) return TDNG_LJ92_ERROR_BAD_HANDLE;
  if (predictor < 1 || predictor > 7) return TDNG_LJ92_ERROR_BAD_HANDLE;
  if (readLength < 0 || skipLength < 0) return TDNG_LJ92_ERROR_BAD_HANDLE;
  if (delinearize && delinearizeLength <= 0) return TDNG_LJ92_ERROR_BAD_HANDLE;

  lje* self = (lje*)calloc(1, sizeof(lje));
  if (!self) return TDNG_LJ92_ERROR_NO_MEMORY;
  self->image = image;
  self->width = width;
  self->height = height;
  self->bitdepth = bitdepth;
  self->components = components;
  self->predictor = predictor;
  self->readLength = readLength > 0 ? readLength : width * components;
  self->skipLength = skipLength;
  self->delinearize = delinearize;
  self->delinearizeLength = delinearizeLength;

  int ret = enc_frequency_scan(self);
  if (ret != TDNG_LJ92_ERROR_NONE) goto done;
  enc_build_huffman_table(self);
  ret = enc_write_header(self);
  if (ret != TDNG_LJ92_ERROR_NONE) goto done;
  ret = enc_write_body(self);
  if (ret != TDNG_LJ92_ERROR_NONE) goto done;
  ret = enc_write_eoi(self);
  if (ret != TDNG_LJ92_ERROR_NONE) goto done;

  {
    uint8_t* shrunk = (uint8_t*)realloc(self->encoded, (size_t)self->encodedWritten);
    if (shrunk) { self->encoded = shrunk; self->encodedCap = self->encodedWritten; }
  }
  *encoded = self->encoded;
  *encodedLength = self->encodedWritten;
  self->encoded = NULL;  // ownership transferred

done:
  free(self->encoded);
  free(self);
  return ret;
}

int tdng_lj92_encode(uint16_t* image, int width, int height, int bitdepth,
                     int readLength, int skipLength, uint16_t* delinearize,
                     int delinearizeLength, uint8_t** encoded,
                     int* encodedLength) {
  return tdng_lj92_encode_ex(image, width, height, bitdepth,
                             /*components=*/1, /*predictor=*/1,
                             readLength, skipLength, delinearize,
                             delinearizeLength, encoded, encodedLength);
}


// End liblj92 ---------------------------------------------------------
