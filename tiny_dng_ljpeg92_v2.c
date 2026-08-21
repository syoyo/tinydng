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
  int sof_marker;  // The SOF marker found (0xC0..0xC3, or 0 if none)
  int skiplen;     // Pixels to skip after each output row
  u16* linearize;  // Linearization table (or NULL)
  int linlen;

  // Per-component Huffman lookup tables. Entry = (ssss << 8) | total.
  u16* hufflut[LJ92_MAX_COMPONENTS];
  int  huffbits[LJ92_MAX_COMPONENTS];
  int  num_huff_idx;
  int  huff_maxbits;  // uniform peek width after expand_luts_uniform

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

  // Streaming state (used when opened via tdng_lj92_open_streaming).
  int is_streaming;
  void* stream_user;
  uint64_t stream_size;
  uint64_t stream_soi_off;    // offset of SOI within the stream
  uint64_t stream_scanstart;  // absolute stream offset of the SOS header (Ls field)
} ljp;

/* ------------------------------------------------------------------ */
/* Streaming reader: chunk-cached random access over a read callback  */
/* ------------------------------------------------------------------ */

#define TDNG_LJ92_STREAM_CHUNK (64 * 1024)

typedef struct tdng_lj92_stream {
  void* user;
  tdng_lj92_read_fn read_fn;
  tdng_lj92_size_fn size_fn;
  uint64_t size;      /* total stream size, or UINT64_MAX if unknown */
  u8* buf;            /* chunk cache */
  uint64_t buf_off;   /* absolute offset of buf[0] */
  size_t buf_len;     /* valid bytes in buf */
  size_t buf_cap;     /* allocated capacity of buf */
} tdng_lj92_stream;

/* Ensure `buf` covers [off, off+need). Returns 1 on success. */
static int srefill(tdng_lj92_stream* s, uint64_t off, size_t need) {
  if (need == 0) return 1;
  if (need > TDNG_LJ92_STREAM_CHUNK) need = TDNG_LJ92_STREAM_CHUNK;
  if (off >= s->buf_off) {
    uint64_t rel = off - s->buf_off;
    if (rel <= (uint64_t)s->buf_len &&
        (uint64_t)(s->buf_len - (size_t)rel) >= (uint64_t)need) {
      return 1; /* already covered */
    }
  }
  {
    uint64_t want = (s->size == UINT64_MAX) ? (uint64_t)need
                                           : TDNG_LJ92_STREAM_CHUNK;
    if (s->size != UINT64_MAX) {
      if (off >= s->size) {
        s->buf_off = off;
        s->buf_len = 0;
        return 0;
      }
      if (want > s->size - off) want = s->size - off;
    }
    if (want > TDNG_LJ92_STREAM_CHUNK) want = TDNG_LJ92_STREAM_CHUNK;
    if (want > s->buf_cap) want = s->buf_cap;
    size_t got = s->read_fn(s->user, off, s->buf, (size_t)want);
    // Hardening: the callback contract is "return requested bytes or fewer";
    // clamp a misbehaving callback so buf_len can never exceed buf_cap.
    if (got > (size_t)want) got = (size_t)want;
    s->buf_off = off;
    s->buf_len = got;
    return got >= need;
  }
}

/* Read exactly len bytes at absolute off. Returns 1 on success. */
static int sread(tdng_lj92_stream* s, uint64_t off, void* dst, size_t len) {
  if (len > (size_t)TDNG_LJ92_STREAM_CHUNK) return 0; /* exceeds cache capacity */
  if (!srefill(s, off, len)) return 0;
  memcpy(dst, s->buf + (size_t)(off - s->buf_off), len);
  return 1;
}

/* Forward declaration: streaming scan entry used by tdng_lj92_decode. */
static int parseScanStreaming(ljp* self);

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
  // nbits < 32 here, so `(63 - nbits) >> 3` advances in whole bytes and
  // `nbits |= 56` is exactly nbits + 56 without the add (see sandbox/lj92).
  bio->p += (63 - bio->nbits) >> 3;
  bio->nbits |= 56;
}

static inline int bitio_check_overrun(const bitio_t* bio) {
  // `p` advances in 8-byte blocks during refill; LJ92_ENTROPY_PAD bytes of
  // zero-padded space past p_end make those loads safe. Overshoot past the
  // padding means we ran off the end of the real data.
  return bio->p > bio->p_end + LJ92_ENTROPY_PAD ? 1 : 0;
}

// Fused Huffman + ssss-value decode with branchless sign-extension.
// The LUT entry packs (ssss << 8) | total, where total = code_length + ssss,
// so ONE shift of the accumulator consumes the code and its residual
// together; the residual mask is 0 for ssss == 0 with no special case.
// Assumes nbits >= 32 (caller refilled) and maxbits <= 16 (total <= 32 fits
// the pre-refill bits; the single `bb <<= total` is defined for total < 64).
static inline int bitio_decode_diff(bitio_t* bio, const u16* hufflut,
                                    int maxbits) {
  uint32_t idx = (uint32_t)(bio->bb >> (64 - maxbits));
  uint16_t e = hufflut[idx];
  uint32_t total = e & 0xff;
  uint32_t ssss = e >> 8;
  uint32_t resid = (uint32_t)(bio->bb >> (64 - total)) & ((1u << ssss) - 1u);
  bio->bb <<= total;
  bio->nbits -= (int)total;
  int m = 1 << ssss;
  int half = m >> 1;
  int sign = ((int)resid - half) >> 31;     // 0 or -1
  return (int)resid + (sign & (1 - m));     // sign-extend JPEG-style
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
// Big-endian 16-bit read. Replaces the old BEH() macro, which relied on
// `&ptr + 1` pointer arithmetic off a function parameter (fragile / UB-ish).
static inline int be16(const u8* p) { return ((int)p[0] << 8) | (int)p[1]; }

// Build a direct-lookup Huffman table from a DHT payload whose Lh length
// field sits at `huffhead[0]` with `avail` bytes available. On success
// allocates *lut_out (caller owns; 2^maxbits entries) and reports the
// segment length *hufflen_out. Shared by the memory and streaming paths.
static int build_huff_lut(const u8* huffhead, int avail, int* hufflen_out,
                          u16** lut_out, int* maxbits_out) {
  // A2: need at least 2 bytes for Lh.
  if (avail < 2) return TDNG_LJ92_ERROR_CORRUPT;
  int hufflen = be16(&huffhead[0]);
  // A5: DHT payload is Lh + Tc/Th(1) + L[1..16] + V[]; need at least 19
  // bytes and must not run off the end.
  u8 bits[17];  // local copy so we never mutate the (possibly read-only) input
  int L;
  if (hufflen < 19) return TDNG_LJ92_ERROR_CORRUPT;
  if (hufflen > avail) return TDNG_LJ92_ERROR_CORRUPT;
  // bits[0] is the length-0 sentinel (no codes); bits[1..16] are the DHT
  // code-length counts L1..L16 at huffhead[3..18].
  bits[0] = 0;
  for (L = 1; L <= 16; L++) bits[L] = huffhead[2 + L];

  const u8* huffvals = huffhead + 19;
  int total_codes = 0;
  for (L = 1; L <= 16; L++) total_codes += bits[L];
  // Allow hufflen >= 19 + total_codes to tolerate trailing padding some
  // encoders emit; reject only if the declared length is too short.
  if (hufflen - 19 < total_codes) return TDNG_LJ92_ERROR_CORRUPT;
  // Lossless JPEG SSSS categories are 0..16. A larger symbol value becomes the
  // `ssss` used by bitio_decode_diff, where `1 << ssss` and the `64 - ssss` /
  // `bb <<= ssss` shifts are undefined for ssss >= 17/31/64. Such a table is
  // not valid lossless JPEG (it would be a baseline DC/AC table) -- reject it
  // here rather than decoding it.
  for (int v = 0; v < total_codes; v++) {
    if (huffvals[v] > 16) return TDNG_LJ92_ERROR_NOT_LOSSLESS;
  }

  int maxbits = 16;
  while (maxbits > 0 && !bits[maxbits]) maxbits--;
  if (maxbits <= 0) return TDNG_LJ92_ERROR_CORRUPT;

  size_t lut_entries = (size_t)1 << maxbits;
  u16* hufflut = (u16*)malloc(lut_entries * sizeof(u16));
  if (!hufflut) return TDNG_LJ92_ERROR_NO_MEMORY;
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
    // Entry packs (ssss << 8) | total with total = code_length + ssss so the
    // entropy loop consumes code + residual with a single shift (max 32 bits).
    hufflut[i++] = (u16)((huffvals[hv] << 8) | (bitsused + huffvals[hv]));
    rv++;
  }
  *hufflen_out = hufflen;
  *lut_out = hufflut;
  *maxbits_out = maxbits;
  return TDNG_LJ92_ERROR_NONE;
}

// Parse DHT: build a direct LUT of 2^maxbits entries, each holding
// (symbol << 8) | code_length. The caller peeks maxbits of the bit stream
// and indexes in to recover the decoded Huffman symbol plus the number of
// bits actually consumed. All reads past the marker header are bounds
// checked against datalen, and every symbol is validated to be <= 16 so
// that `1 << ssss` in the decode path cannot overflow.
static int parseHuff(ljp* self) {
  if (self->num_huff_idx >= LJ92_MAX_COMPONENTS) return TDNG_LJ92_ERROR_CORRUPT;
  int hufflen = 0, maxbits = 0;
  u16* lut = NULL;
  int ret = build_huff_lut(&self->data[self->ix], self->datalen - self->ix,
                           &hufflen, &lut, &maxbits);
  if (ret != TDNG_LJ92_ERROR_NONE) return ret;
  self->hufflut[self->num_huff_idx] = lut;
  self->huffbits[self->num_huff_idx] = maxbits;
  self->num_huff_idx++;
  self->ix += hufflen;
  return TDNG_LJ92_ERROR_NONE;
}

static int parseSof3(ljp* self, int marker) {
  // SOF markers: 0xC0 (SOF0 baseline), 0xC1 (SOF1 extended sequential),
  // 0xC2 (SOF2 progressive), 0xC3 (SOF3 lossless).
  // All share the same frame header format:
  //   Lf(2) P(1) Y(2) X(2) Nf(1) [Ci Hi/Vi Tqi]*Nf
  // A2: header alone is 8 bytes past the current ix.
  if (self->ix < 0 || self->ix > self->datalen ||
      self->datalen - self->ix < 8) return TDNG_LJ92_ERROR_CORRUPT;
  int Lf = be16(&self->data[self->ix]);
  if (Lf < 8 || Lf > self->datalen - self->ix) {
    return TDNG_LJ92_ERROR_CORRUPT;
  }

  self->bits = self->data[self->ix + 2];
  self->y = be16(&self->data[self->ix + 3]);
  self->x = be16(&self->data[self->ix + 5]);
  self->components = self->data[self->ix + 7];
  self->sof_marker = marker;
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
  if (self->ix < 0 || self->ix > self->datalen ||
      self->datalen - self->ix < 2) return TDNG_LJ92_ERROR_CORRUPT;
  int len = be16(&self->data[self->ix]);
  if (len < 2 || len > self->datalen - self->ix) {
    return TDNG_LJ92_ERROR_CORRUPT;
  }
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
        if ((unsigned)raw >= (unsigned)linlen) return TDNG_LJ92_ERROR_CORRUPT;
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
          if ((unsigned)raw >= (unsigned)linlen) return TDNG_LJ92_ERROR_CORRUPT;
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
        if ((unsigned)raw >= (unsigned)linlen) return TDNG_LJ92_ERROR_CORRUPT;
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
          if ((unsigned)raw >= (unsigned)linlen) return TDNG_LJ92_ERROR_CORRUPT;
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
      if ((unsigned)raw >= (unsigned)linlen) return TDNG_LJ92_ERROR_CORRUPT;
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
        if ((unsigned)raw >= (unsigned)linlen) return TDNG_LJ92_ERROR_CORRUPT;
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
        if ((unsigned)raw >= (unsigned)linlen) return TDNG_LJ92_ERROR_CORRUPT;
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
          if ((unsigned)raw >= (unsigned)linlen) return TDNG_LJ92_ERROR_CORRUPT;
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
        if (thisrow[col] >= linlen) return TDNG_LJ92_ERROR_CORRUPT;
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

// Parse a SOS payload (first byte is the Ls length field). `payload_len` is
// the number of available bytes. Fills *comps_out (Ns) and *pred_out (Ss).
// Shared by the memory and streaming paths.
static int parse_sos_payload(const u8* payload, int payload_len,
                             int expected_comps, int* comps_out,
                             int* pred_out, int* sos_len_out) {
  // Hardening: SOS = Ls(2) Ns(1) [Cs Td/Ta]*Ns Ss Se Ah/Al.
  // Need at least 3 bytes to read Ls and Ns.
  if (payload_len < 3) return TDNG_LJ92_ERROR_CORRUPT;
  int Ls = be16(&payload[0]);
  int compcount = payload[2];
  if (Ls < 6 + 2 * compcount || Ls > payload_len) {
    return TDNG_LJ92_ERROR_CORRUPT;
  }
  if (compcount < 1 || compcount > LJ92_MAX_COMPONENTS) {
    return TDNG_LJ92_ERROR_CORRUPT;
  }
  // If SOF3 has been seen, SOS must name the same number of components.
  if (expected_comps > 0 && compcount != expected_comps) {
    return TDNG_LJ92_ERROR_CORRUPT;
  }
  int pred = payload[3 + 2 * compcount];
  if (pred < 0 || pred > 7) return TDNG_LJ92_ERROR_CORRUPT;
  *comps_out = compcount;
  *pred_out = pred;
  *sos_len_out = Ls;
  return TDNG_LJ92_ERROR_NONE;
}

/* Allocate only the decode scratch required by the selected scan path. The
 * common non-linearized mono/RGB predictor paths reconstruct directly into
 * the caller's output and need neither rowcache nor diffcache. */
static int ensure_decode_scratch(ljp* self, int pred) {
  const int nc = self->components;
  int need_rows = self->linearize != NULL;
  int need_diff = (nc == 1 && pred == 1);
  size_t row_slots;

  if (!need_rows) {
    if (nc == 1) {
      need_rows = pred != 7;
    } else if (nc == 3) {
      need_rows = pred != 1 && pred != 7;
    } else {
      need_rows = 1;
    }
  }
  if (!need_rows && !need_diff) return TDNG_LJ92_ERROR_NONE;

  row_slots = (size_t)self->x * (size_t)nc;
  if (need_rows && !self->rowcache) {
    if (row_slots > SIZE_MAX / (2u * sizeof(u16))) {
      return TDNG_LJ92_ERROR_NO_MEMORY;
    }
    self->rowcache = (u16*)calloc(row_slots * 2u, sizeof(u16));
    if (!self->rowcache) return TDNG_LJ92_ERROR_NO_MEMORY;
    self->outrow[0] = self->rowcache;
    self->outrow[1] = self->rowcache + row_slots;
  }
  if (need_diff && !self->diffcache) {
    if (row_slots > SIZE_MAX / sizeof(u16)) {
      return TDNG_LJ92_ERROR_NO_MEMORY;
    }
    self->diffcache = (u16*)calloc(row_slots, sizeof(u16));
    if (!self->diffcache) return TDNG_LJ92_ERROR_NO_MEMORY;
  }
  return TDNG_LJ92_ERROR_NONE;
}

// Dispatch to the specialized scan runner for (pred, components, linearize).
// `bio` must be initialized over the destuffed entropy buffer.
static int parseScanDispatch(ljp* self, bitio_t* bio, int pred) {
  const int NC = self->components;
  const int LIN = (self->linearize != NULL) ? 1 : 0;

  // Mono predictor-1 keeps its dedicated path so the SIMD prefix-sum
  // (SSE2/AVX2 builds) still kicks in. LIN is handled inside that path.
  if (pred == 1 && NC == 1) {
    return parseScanPred1MonoFast(self, bio);
  }

  // Common interleaved combinations get compile-time-specialized inline
  // runners via always_inline + constant propagation.
  if (NC == 1) {
    if (pred == 7) return LIN ? parseScan_p7_n1_lin(self, bio)
                              : parseScan_p7_n1_nolin(self, bio);
  } else if (NC == 3) {
    if (pred == 1) return LIN ? parseScan_p1_n3_lin(self, bio)
                              : parseScan_p1_n3_nolin(self, bio);
    if (pred == 7) return LIN ? parseScan_p7_n3_lin(self, bio)
                              : parseScan_p7_n3_nolin(self, bio);
  }

  // Generic fallback: runtime pred & NC, still uses the new bit-IO.
  return parseScanGeneric(self, bio, pred);
}

static int parseScan(ljp* self) {
  self->ix = self->scanstart;
  // Hardening: SOS = Ls(2) Ns(1) [Cs Td/Ta]*Ns Ss Se Ah/Al.
  int compcount = 0, pred = 0, Ls = 0;
  int ret = parse_sos_payload(self->data + self->ix,
                              self->datalen - self->ix, self->components,
                              &compcount, &pred, &Ls);
  if (ret != TDNG_LJ92_ERROR_NONE) return ret;

  self->ix += Ls;

  // Degenerate scan (no SOF3, zero dims): preserve legacy "silent success"
  // behavior so callers probing format support get TDNG_LJ92_ERROR_NONE with no
  // output written.
  if (self->x <= 0 || self->y <= 0 || self->components <= 0) {
    return TDNG_LJ92_ERROR_NONE;
  }

  ret = ensure_decode_scratch(self, pred);
  if (ret != TDNG_LJ92_ERROR_NONE) return ret;

  // Destuff the entropy-coded payload into self->ebuf so the inner loops
  // can use bulk 8-byte loads without per-byte 0xFF checks.
  ret = destuff_entropy_stream(self, self->ix);
  if (ret != TDNG_LJ92_ERROR_NONE) return ret;

  bitio_t bio;
  bitio_init(&bio, self->ebuf, self->ebuf + self->ebuf_len);
  return parseScanDispatch(self, &bio, pred);
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
      ret = parseSof3(self, 0xC3);
    } else if (nextMarker >= 0xc0 && nextMarker <= 0xcf && nextMarker != 0xc4) {
      // SOF0 (0xC0), SOF1 (0xC1), SOF2 (0xC2): baseline/progressive DCT.
      // Parse the frame header to extract dimensions and bitdepth, but the
      // stream cannot be decoded as lossless JPEG.
      ret = parseSof3(self, nextMarker);
    } else if (nextMarker == 0xdb) {
      // DQT (Define Quantization Table) - skip it; baseline JPEG uses DCT
      // which we don't support. Just skip the block.
      ret = parseBlock(self, nextMarker);
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

// Expand every Huffman LUT to the widest table's index width so the entropy
// loop peeks with one shift width across components. A direct LUT of m bits
// expands to M bits by replicating each entry 2^(M-m) times (the M-bit
// index's top m bits select the original entry). No-op when no tables were
// parsed (the scan runners reject that case).
static int expand_luts_uniform(ljp* self) {
  int M = 0;
  if (self->num_huff_idx < 1) return TDNG_LJ92_ERROR_NONE;
  for (int i = 0; i < self->num_huff_idx; i++) {
    if (self->huffbits[i] > M) M = self->huffbits[i];
  }
  self->huff_maxbits = M;
  for (int i = 0; i < self->num_huff_idx; i++) {
    int m = self->huffbits[i];
    if (m == M) continue;
    size_t oldn = (size_t)1 << m;
    int step = 1 << (M - m);
    u16* nl = (u16*)malloc(((size_t)1 << M) * sizeof(u16));
    if (!nl) return TDNG_LJ92_ERROR_NO_MEMORY;
    const u16* ol = self->hufflut[i];
    for (size_t j = 0; j < oldn; j++) {
      for (int k = 0; k < step; k++) nl[j * (size_t)step + (size_t)k] = ol[j];
    }
    free(self->hufflut[i]);
    self->hufflut[i] = nl;
    self->huffbits[i] = M;
  }
  return TDNG_LJ92_ERROR_NONE;
}

static int findSoI(ljp* self) {
  int ret = TDNG_LJ92_ERROR_CORRUPT;
  if (find(self) == 0xd8) {
    ret = parseImage(self);
    // Validate that we found a valid JPEG with dimensions.
    if (ret == TDNG_LJ92_ERROR_NONE && (self->x <= 0 || self->components <= 0)) {
      return TDNG_LJ92_ERROR_CORRUPT;
    }
    // Check if this is a lossless JPEG (SOF3) or a non-lossless type.
    // SOF0 (0xC0) = baseline DCT, SOF1 (0xC1) = extended sequential,
    // SOF2 (0xC2) = progressive - none of these are lossless.
    if (ret == TDNG_LJ92_ERROR_NONE && self->sof_marker != 0xC3) {
      return TDNG_LJ92_ERROR_NOT_LOSSLESS;
    }
    if (ret == TDNG_LJ92_ERROR_NONE) {
      ret = expand_luts_uniform(self);
    }
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
  ljp* self = lj;
  if (!self) return TDNG_LJ92_ERROR_BAD_HANDLE;
  // Hardening: legacy per-row skip support was removed from the decoder, so a
  // non-zero skipLength would make the scan loops write `skipLength` extra
  // samples past `target` every row (out_stride = W*NC + skipLength). Reject it
  // rather than risking an out-of-bounds write. The integrated tinydng codec
  // always passes skipLength = 0.
  if (skipLength != 0) return TDNG_LJ92_ERROR_CORRUPT;
  // Hardening: reject degenerate/overflowing dimensions. The scan loops index
  // `out_stride = W*NC` samples per row; guard the per-row stride against int
  // overflow and the pixel count against size_t overflow before writing.
  if (self->x <= 0 || self->y <= 0 || self->components <= 0) {
    return TDNG_LJ92_ERROR_CORRUPT;
  }
  if (self->x > (INT_MAX / self->components)) {
    return TDNG_LJ92_ERROR_CORRUPT;
  }
  {
    uint64_t need = (uint64_t)(uint32_t)self->x * (uint64_t)self->components *
                    (uint64_t)self->y;
    if (need > (uint64_t)SIZE_MAX / sizeof(u16)) {
      return TDNG_LJ92_ERROR_CORRUPT;
    }
    // Hardening: `writeLength` (reserved) is the per-row sample count the caller
    // allocated in `target`. The decoder writes exactly (W*NC)*H samples, so
    // reject if that would exceed the caller's capacity (writeLength * H). This
    // protects the standalone API against an undersized `target` buffer. The
    // integrated tinydng codec always passes writeLength = W*NC, so this is a
    // no-op for the integrated path.
    if (writeLength > 0) {
      // Standalone u64 math (this TU must not depend on td_internal.h).
      uint64_t cap_samples =
          (uint64_t)(uint32_t)writeLength * (uint64_t)(uint32_t)self->y;
      if (cap_samples < need) {
        return TDNG_LJ92_ERROR_CORRUPT;
      }
    }
  }
  self->image = target;
  self->skiplen = 0;
  self->linearize = linearize;
  self->linlen = linearizeLength;
  if (self->is_streaming) return parseScanStreaming(self);
  return parseScan(self);
}

void tdng_lj92_close(tdng_lj92 lj) {
  ljp* self = lj;
  if (self == NULL) return;
  if (self->stream_user) {
    tdng_lj92_stream* sctx = (tdng_lj92_stream*)self->stream_user;
    free(sctx->buf);
    free(sctx);
    self->stream_user = NULL;
  }
  free_memory(self);
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

typedef struct _lje {
  int width;
  int height;
  int bitdepth;
  int components;
  int predictor;
  int readLength;
  int skipLength;
  const uint16_t* delinearize;
  int delinearizeLength;

  // Streaming output: the encoded stream goes to the sink in staged chunks
  // instead of a full growable buffer.
  void* sink_user;
  tdng_lj92_write_fn sink_write;
  uint8_t obuf[4096];
  int obuf_len;
  int sink_failed;

  // Frequency of SSSS symbols (0..16) across all components.
  // int64: a 65535x65535x4 constant image drives hist[0] past INT32_MAX.
  int64_t hist[LJ92_MAX_SSSS];
  // Canonical Huffman table derived from hist.
  int bits[17];          // bits[L] = number of codes of length L (1..16).
  uint8_t huffval[17];   // symbols ordered by code length.
  int huffval_count;
  uint16_t huffenc[LJ92_MAX_SSSS];  // code for symbol s.
  uint8_t  huffbits[LJ92_MAX_SSSS]; // code length for symbol s.

  // Bit-output accumulator: bits packed MSB-first into the top of bitbuf.
  uint32_t bitbuf;
  int nbits;

  // Phase: 0=open, 1=scanned, 2=begin'd, 3=finished.
  int phase;

  // Two-pass state shared across encode_rows calls.
  const uint16_t* image;   // base pointer (frame-relative)
  const uint16_t* pix;     // current read pointer (pass 2)
  int scan_remain;         // samples left before the next skip
  int next_row;            // next row encode_rows must deliver
  uint16_t* rc;            // row cache (2 * width * components)
  uint16_t* thisrow;
  uint16_t* lastrow;
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

static TDNG_ALWAYS_INLINE int enc_px_from_neighbors(int pred, int left,
                                                    int above, int abovel,
                                                    int initpx, int row,
                                                    int col) {
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

static TDNG_ALWAYS_INLINE int enc_ssss(int diff) {
  // DNG allows diff in (-32768, 32767]; the "modulo 65536" rule is folded
  // into diff by the caller so diff here fits in int range.
  int a = diff < 0 ? -diff : diff;
  return a == 0 ? 0 : (32 - clz32((unsigned int)a));
}

// Flush the staged output bytes to the sink. Marks sink_failed on a short
// write; subsequent emit calls become no-ops.
static void enc_stage_flush(lje* self) {
  if (self->sink_failed || self->obuf_len == 0) return;
  if (self->sink_write(self->sink_user, self->obuf, (size_t)self->obuf_len) !=
      (size_t)self->obuf_len) {
    self->sink_failed = 1;
  }
  self->obuf_len = 0;
}

static void enc_put_u8(lje* self, uint8_t b) {
  if (self->sink_failed) return;
  if (self->obuf_len >= (int)sizeof(self->obuf)) enc_stage_flush(self);
  self->obuf[self->obuf_len++] = b;
}

// Emit one byte with 0xFF 0x00 stuffing.
static void enc_put_stuffed(lje* self, uint8_t b) {
  enc_put_u8(self, b);
  if (b == 0xFF) enc_put_u8(self, 0x00);
}

// Emit `nbits` bits from the low bits of `v` into the bit stream.
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

static TDNG_ALWAYS_INLINE int enc_emit_diff(lje* self, int diff) {
  int ssss = enc_ssss(diff);
  /* A symbol with no Huffman code would silently desynchronise the stream. */
  if (self->huffbits[ssss] == 0) return TDNG_LJ92_ERROR_CORRUPT;
  enc_put_bits(self, self->huffenc[ssss], self->huffbits[ssss]);
  if (ssss > 0) {
    uint32_t bits_val;
    if (diff < 0) bits_val = (uint32_t)(diff + (1 << ssss) - 1);
    else          bits_val = (uint32_t)diff;
    enc_put_bits(self, bits_val, ssss);
  }
  return TDNG_LJ92_ERROR_NONE;
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

// Fast histogram scan for the predictor used by the writer. The first NC
// samples in a row use the initial/above predictor; all remaining samples
// use the immediately preceding reconstructed sample. Splitting those cases
// removes row/column and predictor branches from the hot loop.
static int enc_frequency_scan_pred1(lje* self) {
  const int H = self->height, NC = self->components;
  const int initpx = 1 << (self->bitdepth - 1);
  const int maxval = 1 << self->bitdepth;
  const size_t row_slots = (size_t)self->width * (size_t)NC;
  uint16_t* thisrow = self->thisrow;
  uint16_t* lastrow = self->lastrow;
  const uint16_t* pix = self->image;
  int scan = self->readLength;
  int row;

  for (row = 0; row < H; row++) {
    size_t i;
    for (i = 0; i < (size_t)NC; i++) {
      uint16_t p = *pix++;
      if (self->delinearize) {
        if (p >= self->delinearizeLength) return TDNG_LJ92_ERROR_TOO_WIDE;
        p = self->delinearize[p];
      }
      if (p >= maxval) return TDNG_LJ92_ERROR_TOO_WIDE;
      {
        int diff = (int16_t)((int)p - (row > 0 ? lastrow[i] : initpx));
        self->hist[enc_ssss(diff)]++;
      }
      thisrow[i] = p;
      if (--scan == 0) {
        pix += self->skipLength;
        scan = self->readLength;
      }
    }
    for (i = (size_t)NC; i < row_slots; i++) {
      uint16_t p = *pix++;
      if (self->delinearize) {
        if (p >= self->delinearizeLength) return TDNG_LJ92_ERROR_TOO_WIDE;
        p = self->delinearize[p];
      }
      if (p >= maxval) return TDNG_LJ92_ERROR_TOO_WIDE;
      {
        int diff = (int16_t)((int)p - thisrow[i - (size_t)NC]);
        self->hist[enc_ssss(diff)]++;
      }
      thisrow[i] = p;
      if (--scan == 0) {
        pix += self->skipLength;
        scan = self->readLength;
      }
    }
    {
      uint16_t* t = lastrow;
      lastrow = thisrow;
      thisrow = t;
    }
  }
  return TDNG_LJ92_ERROR_NONE;
}

// Single-pass histogram scan using the configured predictor. Caches two
// reconstructed rows worth of raw samples so the predictor sees the same
// values the decoder will. Uses the persistent row cache and read pointer.
static int enc_frequency_scan(lje* self) {
  const int W = self->width, H = self->height, NC = self->components;
  const int initpx = 1 << (self->bitdepth - 1);
  const int maxval = 1 << self->bitdepth;

  uint16_t* thisrow = self->thisrow;
  uint16_t* lastrow = self->lastrow;

  if (self->predictor == 1) return enc_frequency_scan_pred1(self);

  const uint16_t* pix = self->image;
  int scan = self->readLength;

  for (int row = 0; row < H; row++) {
    for (int col = 0; col < W; col++) {
      int base = col * NC;
      int prv = base - NC;
      for (int c = 0; c < NC; c++) {
        uint16_t p = *pix++;
        if (self->delinearize) {
          if (p >= self->delinearizeLength) return TDNG_LJ92_ERROR_TOO_WIDE;
          p = self->delinearize[p];
        }
        if (p >= maxval) return TDNG_LJ92_ERROR_TOO_WIDE;

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
  int64_t total = 0;
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
  return self->sink_failed ? TDNG_LJ92_ERROR_IO : TDNG_LJ92_ERROR_NONE;
}

// Emit the entropy-coded scan for rows [row0, row0+row_count). The caller
// must deliver rows sequentially (encode_rows enforces row0 == next_row).
// The predictor row cache, read pointer and skip counter persist in `self`,
// so the output is identical to encoding the whole image in one call.
static int enc_write_rows_pred1(lje* self, int row0, int row_count) {
  const int NC = self->components;
  const int initpx = 1 << (self->bitdepth - 1);
  const size_t row_slots = (size_t)self->width * (size_t)NC;
  uint16_t* thisrow = self->thisrow;
  uint16_t* lastrow = self->lastrow;
  int row;

  for (row = row0; row < row0 + row_count; row++) {
    size_t i;
    for (i = 0; i < (size_t)NC; i++) {
      uint16_t p = *self->pix++;
      if (self->delinearize) {
        if (p >= self->delinearizeLength) return TDNG_LJ92_ERROR_TOO_WIDE;
        p = self->delinearize[p];
      }
      {
        int diff = (int16_t)((int)p - (row > 0 ? lastrow[i] : initpx));
        int ret = enc_emit_diff(self, diff);
        if (ret != TDNG_LJ92_ERROR_NONE) return ret;
      }
      thisrow[i] = p;
      if (--self->scan_remain == 0) {
        self->pix += self->skipLength;
        self->scan_remain = self->readLength;
      }
    }
    for (i = (size_t)NC; i < row_slots; i++) {
      uint16_t p = *self->pix++;
      if (self->delinearize) {
        if (p >= self->delinearizeLength) return TDNG_LJ92_ERROR_TOO_WIDE;
        p = self->delinearize[p];
      }
      {
        int diff = (int16_t)((int)p - thisrow[i - (size_t)NC]);
        int ret = enc_emit_diff(self, diff);
        if (ret != TDNG_LJ92_ERROR_NONE) return ret;
      }
      thisrow[i] = p;
      if (--self->scan_remain == 0) {
        self->pix += self->skipLength;
        self->scan_remain = self->readLength;
      }
    }
    {
      uint16_t* t = lastrow;
      lastrow = thisrow;
      thisrow = t;
    }
  }
  self->thisrow = thisrow;
  self->lastrow = lastrow;
  if (self->sink_failed) return TDNG_LJ92_ERROR_IO;
  return TDNG_LJ92_ERROR_NONE;
}

static int enc_write_rows(lje* self, int row0, int row_count) {
  const int W = self->width, NC = self->components;
  const int initpx = 1 << (self->bitdepth - 1);

  if (self->predictor == 1) return enc_write_rows_pred1(self, row0, row_count);

  uint16_t* thisrow = self->thisrow;
  uint16_t* lastrow = self->lastrow;

  for (int row = row0; row < row0 + row_count; row++) {
    for (int col = 0; col < W; col++) {
      int base = col * NC;
      int prv  = base - NC;
      for (int c = 0; c < NC; c++) {
        uint16_t p = *self->pix++;
        if (self->delinearize) {
          // Hardening: defense in depth. enc_frequency_scan validates the
          // same indices first, so a mismatch here indicates a tampered
          // input array between the two passes.
          if (p >= self->delinearizeLength) return TDNG_LJ92_ERROR_TOO_WIDE;
          p = self->delinearize[p];
        }
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
        if (self->huffbits[ssss] == 0) return TDNG_LJ92_ERROR_CORRUPT;
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

        if (--self->scan_remain == 0) {
          self->pix += self->skipLength;
          self->scan_remain = self->readLength;
        }
      }
    }
    uint16_t* t = lastrow; lastrow = thisrow; thisrow = t;
  }
  self->thisrow = thisrow;
  self->lastrow = lastrow;
  if (self->sink_failed) return TDNG_LJ92_ERROR_IO;
  return TDNG_LJ92_ERROR_NONE;
}

static int enc_write_eoi(lje* self) {
  enc_put_u8(self, 0xFF);
  enc_put_u8(self, 0xD9);
  return self->sink_failed ? TDNG_LJ92_ERROR_IO : TDNG_LJ92_ERROR_NONE;
}

/* ------------------------------------------------------------------ */
/* Streaming encoder                                                  */
/*                                                                     */
/* Two-pass by design (Huffman needs the SSSS histogram before the     */
/* header can be emitted): tdng_lj92_encode_scan reads the whole       */
/* image once, tdng_lj92_encode_begin emits SOI..SOS to the sink, and  */
/* tdng_lj92_encode_rows feeds the entropy pass incrementally in row   */
/* bands (the predictor row cache persists across calls). Output is    */
/* staged in 4KB chunks and never materialized as a whole.             */
/* ------------------------------------------------------------------ */

int tdng_lj92_encode_open(tdng_lj92_enc* lj, int width, int height,
                          int bitdepth, int components, int predictor,
                          int readLength, int skipLength, void* user,
                          tdng_lj92_write_fn write_fn) {
  size_t row_slots;
  lje* self;
  if (!lj || !write_fn) return TDNG_LJ92_ERROR_BAD_HANDLE;
  *lj = NULL;
  if (width <= 0 || width > 0xFFFF) return TDNG_LJ92_ERROR_BAD_HANDLE;
  if (height <= 0 || height > 0xFFFF) return TDNG_LJ92_ERROR_BAD_HANDLE;
  if (bitdepth < 2 || bitdepth > 16) return TDNG_LJ92_ERROR_BAD_HANDLE;
  if (components < 1 || components > 4) return TDNG_LJ92_ERROR_BAD_HANDLE;
  if (predictor < 1 || predictor > 7) return TDNG_LJ92_ERROR_BAD_HANDLE;
  if (readLength < 0 || skipLength < 0) return TDNG_LJ92_ERROR_BAD_HANDLE;
  self = (lje*)calloc(1, sizeof(lje));
  if (!self) return TDNG_LJ92_ERROR_NO_MEMORY;
  row_slots = (size_t)width * (size_t)components;
  self->rc = (uint16_t*)calloc(row_slots * 2, sizeof(uint16_t));
  if (!self->rc) {
    free(self);
    return TDNG_LJ92_ERROR_NO_MEMORY;
  }
  self->width = width;
  self->height = height;
  self->bitdepth = bitdepth;
  self->components = components;
  self->predictor = predictor;
  self->readLength = readLength > 0 ? readLength : width * components;
  self->skipLength = skipLength;
  self->sink_user = user;
  self->sink_write = write_fn;
  self->thisrow = self->rc;
  self->lastrow = self->rc + row_slots;
  *lj = self;
  return TDNG_LJ92_ERROR_NONE;
}

/* Pass 1: read the whole image and build the SSSS histogram. `image` is
   the base pointer for all later tdng_lj92_encode_rows calls. */
int tdng_lj92_encode_scan(tdng_lj92_enc lj, const uint16_t* image,
                          const uint16_t* delinearize, int delinearizeLength) {
  lje* self = lj;
  int ret;
  if (!self || !image) return TDNG_LJ92_ERROR_BAD_HANDLE;
  if (delinearize && delinearizeLength <= 0) return TDNG_LJ92_ERROR_BAD_HANDLE;
  if (self->phase != 0) return TDNG_LJ92_ERROR_BAD_HANDLE;
  self->image = image;
  self->delinearize = delinearize;
  self->delinearizeLength = delinearizeLength;
  ret = enc_frequency_scan(self);
  if (ret != TDNG_LJ92_ERROR_NONE) return ret;
  self->phase = 1;
  return TDNG_LJ92_ERROR_NONE;
}

/* Emit SOI / SOF3 / DHT / SOS to the sink. Resets the pass-2 predictor
   state so tdng_lj92_encode_rows can start at row 0. */
int tdng_lj92_encode_begin(tdng_lj92_enc lj) {
  lje* self = lj;
  int ret;
  size_t row_slots;
  if (!self) return TDNG_LJ92_ERROR_BAD_HANDLE;
  if (self->phase != 1) return TDNG_LJ92_ERROR_BAD_HANDLE;
  enc_build_huffman_table(self);
  ret = enc_write_header(self);
  if (ret != TDNG_LJ92_ERROR_NONE) return ret;
  enc_stage_flush(self);
  if (self->sink_failed) return TDNG_LJ92_ERROR_IO;
  row_slots = (size_t)self->width * (size_t)self->components;
  memset(self->rc, 0, row_slots * 2 * sizeof(uint16_t));
  self->thisrow = self->rc;
  self->lastrow = self->rc + row_slots;
  self->pix = self->image;
  self->scan_remain = self->readLength;
  self->next_row = 0;
  self->bitbuf = 0;
  self->nbits = 0;
  self->phase = 2;
  return TDNG_LJ92_ERROR_NONE;
}

/* Pass 2, incremental: emit the entropy-coded scan for rows
   [row0, row0+row_count). Must be called strictly in row order with the
   same `image` base pointer passed to tdng_lj92_encode_scan. */
int tdng_lj92_encode_rows(tdng_lj92_enc lj, const uint16_t* image, int row0,
                          int row_count) {
  lje* self = lj;
  int ret;
  if (!self || !image) return TDNG_LJ92_ERROR_BAD_HANDLE;
  if (self->phase != 2) return TDNG_LJ92_ERROR_BAD_HANDLE;
  if (row0 < 0 || row_count <= 0 || row0 != self->next_row ||
      row0 + row_count > self->height) {
    return TDNG_LJ92_ERROR_BAD_HANDLE;
  }
  if (image != self->image) return TDNG_LJ92_ERROR_BAD_HANDLE;
  ret = enc_write_rows(self, row0, row_count);
  if (ret != TDNG_LJ92_ERROR_NONE) {
    // Poison the phase machine: the internal read pointer has already
    // advanced partway through this band, so a retry would resume from the
    // wrong position and eventually walk out of the caller's image buffer.
    self->phase = 3;
    return ret;
  }
  enc_stage_flush(self);
  if (self->sink_failed) return TDNG_LJ92_ERROR_IO;
  self->next_row = row0 + row_count;
  return TDNG_LJ92_ERROR_NONE;
}

/* Flush the final bits, emit EOI and release the encoder. The handle is
   invalid after this call; the return value is the final encode status. */
int tdng_lj92_encode_finish(tdng_lj92_enc lj) {
  lje* self = lj;
  int ret = TDNG_LJ92_ERROR_NONE;
  if (!self) return TDNG_LJ92_ERROR_BAD_HANDLE;
  if (self->phase != 2) {
    ret = TDNG_LJ92_ERROR_BAD_HANDLE;  // begin never ran
  } else if (self->next_row != self->height) {
    ret = TDNG_LJ92_ERROR_BAD_HANDLE;  // all rows must be supplied
  } else {
    enc_flush_bits(self);
    ret = enc_write_eoi(self);
    enc_stage_flush(self);
    if (self->sink_failed) ret = TDNG_LJ92_ERROR_IO;
  }
  free(self->rc);
  free(self);
  return ret;
}

/* Growable-buffer sink used by the one-shot tdng_lj92_encode_ex. */
typedef struct lj92_membuf {
  uint8_t* buf;
  size_t cap;
  size_t len;
} lj92_membuf;

static size_t lj92_membuf_write(void* user, const void* data, size_t len) {
  lj92_membuf* m = (lj92_membuf*)user;
  size_t need = m->len + len;
  if (need < m->len) return 0;  /* size_t wrap */
  if (need > m->cap) {
    size_t cap = m->cap ? m->cap : 4096u;
    while (cap < need) {
      if (cap > (size_t)INT_MAX / 2) { cap = need; break; }
      cap *= 2;
    }
    if (cap > (size_t)INT_MAX) return 0;
    {
      uint8_t* nb = (uint8_t*)realloc(m->buf, cap);
      if (!nb) return 0;
      m->buf = nb;
      m->cap = cap;
    }
  }
  memcpy(m->buf + m->len, data, len);
  m->len += len;
  return len;
}

int tdng_lj92_encode_ex(uint16_t* image, int width, int height, int bitdepth,
                        int components, int predictor, int readLength,
                        int skipLength, uint16_t* delinearize,
                        int delinearizeLength, uint8_t** encoded,
                        int* encodedLength) {
  lj92_membuf mb;
  tdng_lj92_enc lj = NULL;
  int ret;

  if (!image || !encoded || !encodedLength) return TDNG_LJ92_ERROR_BAD_HANDLE;
  *encoded = NULL;
  *encodedLength = 0;
  memset(&mb, 0, sizeof(mb));
  ret = tdng_lj92_encode_open(&lj, width, height, bitdepth, components,
                              predictor, readLength, skipLength, &mb,
                              lj92_membuf_write);
  if (ret != TDNG_LJ92_ERROR_NONE) return ret;
  ret = tdng_lj92_encode_scan(lj, image, delinearize, delinearizeLength);
  if (ret == TDNG_LJ92_ERROR_NONE) ret = tdng_lj92_encode_begin(lj);
  if (ret == TDNG_LJ92_ERROR_NONE) {
    ret = tdng_lj92_encode_rows(lj, image, 0, height);
  }
  tdng_lj92_encode_finish(lj);  /* always frees the encoder */
  if (ret != TDNG_LJ92_ERROR_NONE) {
    free(mb.buf);
    return ret;
  }
  *encoded = mb.buf;
  *encodedLength = (int)mb.len;
  return TDNG_LJ92_ERROR_NONE;
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



/* ------------------------------------------------------------------ */
/* Streaming decoder                                                  */
/*                                                                     */
/* Same decode semantics as the memory path (tdng_lj92_open), but the  */
/* stream is never materialized: headers are parsed and the entropy    */
/* payload is destuffed straight out of the read callback into the     */
/* internal ebuf, which is the only full-size buffer (the entropy scan */
/* is a serial bit stream, so it must be resident while decoding).     */
/*                                                                     */
/* The tdng_lj92_stream chunk cache lives at the top of this file so   */
/* tdng_lj92_close can release it.                                     */
/* ------------------------------------------------------------------ */

// Scan for the next marker (0xFF followed by a byte other than 0xFF/0x00)
// starting at absolute `off`. Returns the marker code and stores the
// absolute offset of the 0xFF byte in *marker_off, or -1 if no marker is
// found before the end of the search range.
static int stream_find_marker(ljp* self, uint64_t off, uint64_t* marker_off) {
  tdng_lj92_stream* s = (tdng_lj92_stream*)self->stream_user;
  uint64_t end = (self->stream_size != UINT64_MAX) ? self->stream_size :
      ((UINT64_MAX - off < 65536u) ? UINT64_MAX : off + 65536u);
  uint64_t pos = off;
  while (pos < end && end - pos >= 2u) {
    if (!srefill(s, pos, 2)) break;
    u8 b0 = s->buf[(size_t)(pos - s->buf_off)];
    if (b0 == 0xFF) {
      u8 b1;
      if (!sread(s, pos + 1, &b1, 1)) break;
      if (b1 != 0xFF && b1 != 0x00) {
        *marker_off = pos;
        return b1;
      }
    }
    pos++;
  }
  return -1;
}

// Destuff the entropy-coded payload starting at absolute `stream_pos` into
// self->ebuf, collapsing 0xFF 0x00 byte stuffing into a plain 0xFF and
// stopping at the first non-entropy marker (0xFF xx with xx in [01..FE]
// except 0xFF). Appends LJ92_ENTROPY_PAD bytes of zero padding so that the
// bit reader's unconditional 8-byte loads are always safe. Byte-for-byte
// equivalent to destuff_entropy_stream() over the equivalent memory range.
static int stream_destuff_entropy(ljp* self, uint64_t stream_pos) {
  tdng_lj92_stream* s = (tdng_lj92_stream*)self->stream_user;
  uint64_t sz = self->stream_size;
  uint64_t end = sz;
  if (end == UINT64_MAX) {
    const uint64_t window = 64ULL * 1024u * 1024u;
    end = (UINT64_MAX - stream_pos < window) ? UINT64_MAX
                                              : stream_pos + window;
  }
  if (end < stream_pos) return TDNG_LJ92_ERROR_CORRUPT;
  // Hardening: reject streams whose entropy payload cannot be addressed with
  // an int-sized destuffed buffer (mirrors destuff_entropy_stream). Clamping
  // only the allocation while looping over the full span would overflow ebuf.
  uint64_t remain = end - stream_pos;
  if (remain > (uint64_t)(INT_MAX - LJ92_ENTROPY_ALLOC_PAD)) {
    return TDNG_LJ92_ERROR_CORRUPT;
  }
  int need = (int)remain + LJ92_ENTROPY_ALLOC_PAD;
  if (self->ebuf_cap < need) {
    u8* nb = (u8*)realloc(self->ebuf, (size_t)need);
    if (!nb) return TDNG_LJ92_ERROR_NO_MEMORY;
    self->ebuf = nb;
    self->ebuf_cap = need;
  }
  u8* dst = self->ebuf;
  uint64_t pos = stream_pos;
  while (pos < end) {
    if (!srefill(s, pos, 1)) break;
    u8 b = s->buf[(size_t)(pos - s->buf_off)];
    pos++;
    *dst++ = b;
    if (b != 0xFF) continue;
    // b == 0xFF: peek the next byte.
    if (pos >= end) break;
    if (!srefill(s, pos, 1)) break;
    u8 c2 = s->buf[(size_t)(pos - s->buf_off)];
    if (c2 == 0x00) { pos++; continue; }  // stuffed zero
    if (c2 == 0xFF) continue;             // fill byte — next iter re-checks
    dst--;                                // real marker; drop the FF
    break;
  }
  self->ebuf_len = (int)(dst - self->ebuf);
  // A1: zero the full safe-overshoot + 8-byte-read window.
  memset(dst, 0, (size_t)LJ92_ENTROPY_ALLOC_PAD);
  return TDNG_LJ92_ERROR_NONE;
}

// Walk the markers of a stream starting right after its SOI. Parses DHT
// (Huffman) tables and SOF frames exactly like the memory path (parseImage),
// skipping all other segments, until the SOS is found. On success sets
// self->stream_scanstart to the absolute offset of the entropy payload.
static int stream_parse_headers(ljp* self, uint64_t soi_off) {
  tdng_lj92_stream* s = (tdng_lj92_stream*)self->stream_user;
  uint64_t pos;
  if (UINT64_MAX - soi_off < 2u) return TDNG_LJ92_ERROR_CORRUPT;
  pos = soi_off + 2u;
  u8 scratch[65536];
  self->x = 0;
  self->y = 0;
  self->components = 0;
  self->bits = 0;
  self->sof_marker = 0;
  for (;;) {
    uint64_t mo = 0;
    int m = stream_find_marker(self, pos, &mo);
    if (m < 0) return TDNG_LJ92_ERROR_CORRUPT;
    if (m == 0xD8) {
      if (UINT64_MAX - mo < 2u) return TDNG_LJ92_ERROR_CORRUPT;
      pos = mo + 2u;
      continue;
    }
    if (m == 0xD9) return TDNG_LJ92_ERROR_CORRUPT;  // EOI before SOS
    if (UINT64_MAX - mo < 2u) return TDNG_LJ92_ERROR_CORRUPT;
    uint16_t segsize = 0;
    if (!sread(s, mo + 2, &segsize, 2)) return TDNG_LJ92_ERROR_CORRUPT;
    segsize = (uint16_t)((segsize >> 8) | (segsize << 8));
    if (segsize < 2) return TDNG_LJ92_ERROR_CORRUPT;
    uint64_t sd, se;  // sd is the segment's Ln length field
    if (UINT64_MAX - mo < 2u) {
      return TDNG_LJ92_ERROR_CORRUPT;
    }
    sd = mo + 2u;
    if (UINT64_MAX - sd < (uint64_t)segsize) {
      return TDNG_LJ92_ERROR_CORRUPT;
    }
    se = sd + (uint64_t)segsize;
    size_t payload_len = (size_t)segsize;
    if (payload_len > sizeof(scratch)) return TDNG_LJ92_ERROR_CORRUPT;

    if (m == 0xDA) {  // SOS: parse and record the SOS header offset
      if (!sread(s, sd, scratch, payload_len)) return TDNG_LJ92_ERROR_CORRUPT;
      int compcount = 0, pred = 0, Ls = 0;
      int ret = parse_sos_payload(scratch, (int)payload_len, self->components,
                                  &compcount, &pred, &Ls);
      if (ret != TDNG_LJ92_ERROR_NONE) return ret;
      self->stream_scanstart = sd;  // the entropy data starts at sd + Ls
      return expand_luts_uniform(self);
    }
    if (m >= 0xC0 && m <= 0xCF && m != 0xC4) {  // SOF frames
      // Lf P Y X Nf layout; Lf >= 8 (see parseSof3).
      if (segsize < 8) return TDNG_LJ92_ERROR_CORRUPT;
      u8 precision, nf;
      uint16_t yh, xh;
      if (!sread(s, sd + 2, &precision, 1)) return TDNG_LJ92_ERROR_CORRUPT;
      if (!sread(s, sd + 3, &yh, 2)) return TDNG_LJ92_ERROR_CORRUPT;
      if (!sread(s, sd + 5, &xh, 2)) return TDNG_LJ92_ERROR_CORRUPT;
      if (!sread(s, sd + 7, &nf, 1)) return TDNG_LJ92_ERROR_CORRUPT;
      yh = (uint16_t)((yh >> 8) | (yh << 8));
      xh = (uint16_t)((xh >> 8) | (xh << 8));
      self->sof_marker = (uint8_t)m;
      self->bits = precision;
      self->y = (int)yh;
      self->x = (int)xh;
      self->components = nf;
      // A4: bitdepth must be in [2,16] so that 1 << (bits-1) is defined.
      if (self->bits < 2 || self->bits > 16) return TDNG_LJ92_ERROR_CORRUPT;
      // A3: accept up to LJ92_MAX_COMPONENTS.
      if (self->components < 1 || self->components > LJ92_MAX_COMPONENTS) {
        return TDNG_LJ92_ERROR_CORRUPT;
      }
      pos = se;
      continue;
    }
    if (m == 0xC4) {  // DHT: build a Huffman LUT
      if (!sread(s, sd, scratch, payload_len)) return TDNG_LJ92_ERROR_CORRUPT;
      if (self->num_huff_idx >= LJ92_MAX_COMPONENTS) {
        return TDNG_LJ92_ERROR_CORRUPT;
      }
      int hufflen = 0, maxbits = 0;
      u16* lut = NULL;
      int ret = build_huff_lut(scratch, (int)payload_len, &hufflen, &lut,
                               &maxbits);
      if (ret != TDNG_LJ92_ERROR_NONE) return ret;
      self->hufflut[self->num_huff_idx] = lut;
      self->huffbits[self->num_huff_idx] = maxbits;
      self->num_huff_idx++;
      pos = se;
      continue;
    }
    // APPn, COM, DQT, DRI, ...: skip the segment.
    pos = se;
  }
}

// Streaming scan: re-read the SOS payload from the stream, destuff the
// entropy payload from the stream into ebuf, then run the same specialized
// scan runners as the memory path.
static int parseScanStreaming(ljp* self) {
  tdng_lj92_stream* s = (tdng_lj92_stream*)self->stream_user;
  uint64_t so = self->stream_scanstart;  // SOS Ls field offset
  u8 payload[64];  // SOS payload is at most 6 + 2*16 + 2 = 40 bytes
  uint16_t ls16 = 0;
  if (!sread(s, so, &ls16, 2)) return TDNG_LJ92_ERROR_CORRUPT;
  int Ls = (int)(((ls16 >> 8) | (ls16 << 8)) & 0xFFFFu);
  if (Ls < 3 || Ls > (int)sizeof(payload)) return TDNG_LJ92_ERROR_CORRUPT;
  if (!sread(s, so, payload, (size_t)Ls)) return TDNG_LJ92_ERROR_CORRUPT;
  int compcount = 0, pred = 0, sos_len = 0;
  int ret = parse_sos_payload(payload, Ls, self->components, &compcount,
                              &pred, &sos_len);
  if (ret != TDNG_LJ92_ERROR_NONE) return ret;

  // Degenerate scan (no SOF3, zero dims): preserve legacy "silent success"
  // behavior so callers probing format support get TDNG_LJ92_ERROR_NONE with
  // no output written.
  if (self->x <= 0 || self->y <= 0 || self->components <= 0) {
    return TDNG_LJ92_ERROR_NONE;
  }

  ret = ensure_decode_scratch(self, pred);
  if (ret != TDNG_LJ92_ERROR_NONE) return ret;

  ret = stream_destuff_entropy(self, so + (uint64_t)sos_len);
  if (ret != TDNG_LJ92_ERROR_NONE) return ret;

  bitio_t bio;
  bitio_init(&bio, self->ebuf, self->ebuf + self->ebuf_len);
  return parseScanDispatch(self, &bio, pred);
}

int tdng_lj92_open_streaming(tdng_lj92* lj, void* user,
                             tdng_lj92_read_fn read_fn,
                             tdng_lj92_size_fn size_fn,
                             int* width, int* height, int* bitdepth,
                             int* components) {
  ljp* self;
  tdng_lj92_stream* sctx;
  int ret;
  uint64_t mo = 0;

  if (!lj || !read_fn) return TDNG_LJ92_ERROR_BAD_HANDLE;
  *lj = NULL;
  self = (ljp*)calloc(sizeof(ljp), 1);
  if (!self) return TDNG_LJ92_ERROR_NO_MEMORY;
  sctx = (tdng_lj92_stream*)calloc(1, sizeof(*sctx));
  if (!sctx) { free(self); return TDNG_LJ92_ERROR_NO_MEMORY; }
  sctx->user = user;
  sctx->read_fn = read_fn;
  sctx->size_fn = size_fn;
  sctx->size = size_fn ? size_fn(user) : UINT64_MAX;
  sctx->buf_cap = TDNG_LJ92_STREAM_CHUNK;
  if (sctx->size != UINT64_MAX && sctx->size < (uint64_t)sctx->buf_cap) {
    sctx->buf_cap = (size_t)sctx->size;
  }
  if (sctx->buf_cap == 0u) sctx->buf_cap = 1u;
  sctx->buf = (u8*)malloc(sctx->buf_cap);
  if (!sctx->buf) {
    free(sctx);
    free(self);
    return TDNG_LJ92_ERROR_NO_MEMORY;
  }
  self->stream_user = sctx;
  self->is_streaming = 1;
  self->stream_size = sctx->size;

  // Find the SOI: the first marker in the stream must be SOI (parity with
  // the memory path's findSoI).
  ret = stream_find_marker(self, 0, &mo);
  if (ret != 0xD8) {
    ret = TDNG_LJ92_ERROR_CORRUPT;
    goto fail;
  }
  self->stream_soi_off = mo;
  ret = stream_parse_headers(self, mo);
  if (ret != TDNG_LJ92_ERROR_NONE) goto fail;
  // Check if this is a lossless JPEG (SOF3) or a non-lossless type.
  if (self->x <= 0 || self->components <= 0 || self->sof_marker != 0xC3) {
    ret = TDNG_LJ92_ERROR_NOT_LOSSLESS;
    goto fail;
  }
  // Hardening: SOF3 width/height are 16-bit so x,y <= 65535 and components
  // <= LJ92_MAX_COMPONENTS; guard the multiplication anyway.
  if (self->x > 0xFFFF || self->components > LJ92_MAX_COMPONENTS) {
    ret = TDNG_LJ92_ERROR_CORRUPT;
    goto fail;
  }
  *width = self->x;
  *height = self->y;
  *bitdepth = self->bits;
  *components = self->components;
  *lj = self;
  return TDNG_LJ92_ERROR_NONE;

fail:
  free_memory(self);  // releases Huffman LUTs / rowcache / ebuf if allocated
  free(sctx->buf);
  free(sctx);
  free(self);
  return ret;
}
// End liblj92 ---------------------------------------------------------
