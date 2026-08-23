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

// ---------------------------------------------------------------------------
// SIMD kernel selection.
//
// Kernels are selected at RUNTIME on x86 when the compiler supports
// __attribute__((target)) + __builtin_cpu_supports (GCC >= 4.8 / Clang): the
// SSE2 and AVX2 prefix-sum kernels are compiled into the library regardless of
// the host -march, tagged with their ISA via target attributes, and the
// dispatcher below picks the widest one the CPU supports. This keeps the
// library portable while consumers still get SIMD without special build
// flags.
//
// Manual overrides:
//   -DTINY_DNG_LJPEG92_V2_USE_AVX2  : always use the AVX2 kernel (compile-time)
//   -DTINY_DNG_LJPEG92_V2_USE_SSE2  : always use the SSE2 kernel (compile-time)
//   -DTINY_DNG_LJPEG92_NO_SIMD      : scalar only (disables runtime dispatch)
// ---------------------------------------------------------------------------
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || \
    defined(_M_IX86)
#define TDNG_LJ92_X86 1
#endif

#if defined(TINY_DNG_LJPEG92_V2_USE_AVX2) && \
    !defined(TINY_DNG_LJPEG92_V2_USE_SSE2)
#define TINY_DNG_LJPEG92_V2_USE_SSE2 1
#endif

#if defined(TINY_DNG_LJPEG92_V2_USE_AVX2)
#include <immintrin.h>
#elif defined(TINY_DNG_LJPEG92_V2_USE_SSE2)
#include <emmintrin.h>
#elif defined(TDNG_LJ92_X86) && !defined(TINY_DNG_LJPEG92_NO_SIMD) && \
    !defined(_MSC_VER) && (defined(__GNUC__) || defined(__clang__))
#define TDNG_LJ92_RUNTIME_SIMD 1
#include <immintrin.h>
#endif

#if defined(__aarch64__) && !defined(TINY_DNG_LJPEG92_NO_SIMD)
#define TDNG_LJ92_NEON 1
#include <arm_neon.h>
#endif

#if defined(TDNG_LJ92_RUNTIME_SIMD)
#define TDNG_LJ92_TARGET_ISA(isa) __attribute__((target(isa)))
#else
#define TDNG_LJ92_TARGET_ISA(isa)
#endif

#define TINY_DNG_DPRINTF(...) ((void)0)

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

static int lj92_allocator_resolve(const tdng_lj92_allocator* src,
                                  tdng_lj92_allocator* dst) {
  memset(dst, 0, sizeof(*dst));
  if (!src) return 1;
  if (!src->alloc || !src->free) return 0;
  *dst = *src;
  return 1;
}

static void* lj92_alloc(const tdng_lj92_allocator* a, size_t size) {
  if (size == 0) size = 1;
  return a->alloc ? a->alloc(a->user, size) : malloc(size);
}

static void* lj92_alloc_zero(const tdng_lj92_allocator* a, size_t size) {
  void* p = lj92_alloc(a, size);
  if (p) memset(p, 0, size);
  return p;
}

static void lj92_free(const tdng_lj92_allocator* a, void* p) {
  if (!p) return;
  if (a->free)
    a->free(a->user, p);
  else
    free(p);
}

static void* lj92_realloc_copy(const tdng_lj92_allocator* a, void* old_ptr,
                               size_t old_size, size_t new_size) {
  void* p = lj92_alloc(a, new_size);
  if (!p) return NULL;
  if (old_ptr && old_size) {
    memcpy(p, old_ptr, old_size < new_size ? old_size : new_size);
  }
  lj92_free(a, old_ptr);
  return p;
}

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
  tdng_lj92_allocator allocator;
  u8* data;
  int datalen;
  int scanstart;
  int ix;
  int x;           // Width
  int y;           // Height
  int bits;        // Bit depth
  int components;  // Number of components (Nf)
  int sof_marker;  // The SOF marker found (0xC0..0xC3, or 0 if none)
  int height_from_dnl;
  int skiplen;     // Pixels to skip after each output row
  u16* linearize;  // Linearization table (or NULL)
  int linlen;

  // DC Huffman lookup tables indexed by the JPEG table id (0..3). Entry =
  // (mask << 6) | total, where
  // mask = (1 << ssss) - 1 and total = code_length + ssss: the entropy loop
  // extracts the residual mask without recomputing it per sample.
  u32* hufflut[4];
  int huffbits[4];
  u8 huff_defined[4];
  u8 component_id[LJ92_MAX_COMPONENTS];
  u8 component_h[LJ92_MAX_COMPONENTS];
  u8 component_v[LJ92_MAX_COMPONENTS];
  u8 scan_huff_id[LJ92_MAX_COMPONENTS];
  int  num_huff_idx;
  int  huff_maxbits;  // uniform peek width after expand_luts_uniform
  // Arithmetic-lossless (SOF11) DC conditioning. T.81 defaults are L=0,
  // U=1 when a DAC marker does not override a table.
  u8 arith_dc_l[4];
  u8 arith_dc_u[4];

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
    uint64_t want = TDNG_LJ92_STREAM_CHUNK;
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
  const u8* p_start;
  const u8* p;
  const u8* p_end;   // one past last real destuffed byte
  uint64_t bb;
  int nbits;         // valid bit count in bb, in [0, 64]
  uint64_t available_bits;
  uint32_t last_entry;
} bitio_t;

static inline void bitio_init(bitio_t* bio, const u8* p, const u8* p_end) {
  bio->p_start = p;
  bio->p = p;
  bio->p_end = p_end;
  bio->bb = 0;
  bio->nbits = 0;
  bio->available_bits = (uint64_t)(p_end - p) * 8u;
  bio->last_entry = 0;
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
static inline void bitio_refill(bitio_t* restrict bio) {
  int nbits = bio->nbits;
  if (nbits >= 32) return;
  if (TDNG_UNLIKELY(bio->p > bio->p_end + LJ92_ENTROPY_PAD)) {
    bio->nbits = 64;
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
  bio->bb |= x >> nbits;
  // nbits < 32 here, so `(63 - nbits) >> 3` advances in whole bytes and
  // `nbits |= 56` is exactly nbits + 56 without the add (see sandbox/lj92).
  bio->p += (63 - nbits) >> 3;
  bio->nbits = nbits | 56;
}

static inline int bitio_check_overrun(const bitio_t* bio) {
  // `p` advances in 8-byte blocks during refill; LJ92_ENTROPY_PAD bytes of
  // zero-padded space past p_end make those loads safe. Overshoot past the
  // padding means we ran off the end of the real data.
  uint64_t loaded_bits;
  uint64_t consumed_bits;
  int nbits = bio->nbits;
  if ((bio->last_entry >> 31) || bio->p > bio->p_end + LJ92_ENTROPY_PAD ||
      bio->p < bio->p_start || nbits < 0 || nbits > 64) {
    return 1;
  }
  loaded_bits = (uint64_t)(bio->p - bio->p_start) * 8u;
  if (loaded_bits < (uint64_t)nbits) return 1;
  consumed_bits = loaded_bits - (uint64_t)nbits;
  // A scan ends on a byte boundary with at most seven pad bits. This catches
  // both reads into the synthetic zero pad and unconsumed entropy garbage.
  return consumed_bits > bio->available_bits ||
         bio->available_bits - consumed_bits > 7u;
}

// Fused Huffman + ssss-value decode with branchless sign-extension.
// The LUT entry packs (mask << 6) | total, where mask = (1 << ssss) - 1 and
// total = code_length + ssss, so ONE shift of the accumulator consumes the
// code and its residual together and the residual mask comes straight from
// the table (no per-sample `1 << ssss` recomputation; mask is 0 for
// ssss == 0 with no special case).
// Assumes nbits >= 32 (caller refilled) and maxbits <= 16 (total <= 32 fits
// the pre-refill bits; the single `bb <<= total` is defined for total < 64).
static inline int bitio_decode_diff(bitio_t* restrict bio,
                                    const u32* restrict hufflut, int maxbits) {
  uint32_t idx = (uint32_t)(bio->bb >> (64 - maxbits));
  uint32_t e = hufflut[idx];
  /* Keep the invalid-prefix bit sticky until the scan boundary check. A later
     valid symbol must not erase evidence that corrupt entropy hit a LUT hole. */
  bio->last_entry |= e;
  uint32_t total = e & 0x3fu;
  uint32_t mask = (e >> 6) & 0xffffu;
  uint32_t resid = (uint32_t)(bio->bb >> (64 - total)) & mask;
  bio->bb <<= total;
  bio->nbits -= (int)total;
  int m = (int)(mask + 1u);  // m == 1 << ssss
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
static int build_huff_lut(const tdng_lj92_allocator* allocator, const u8* table,
                          int avail, int* consumed_out, int* table_id_out,
                          u32** lut_out, int* maxbits_out) {
  // One table is Tc/Th(1) + L[1..16] + V[].
  u8 bits[17];  // local copy so we never mutate the (possibly read-only) input
  int L;
  int table_class, table_id;
  if (avail < 17) return TDNG_LJ92_ERROR_CORRUPT;
  table_class = table[0] >> 4;
  table_id = table[0] & 15;
  if (table_class != 0 || table_id > 3) {
    return TDNG_LJ92_ERROR_UNSUPPORTED;
  }
  // bits[0] is the length-0 sentinel (no codes); bits[1..16] are the DHT
  // code-length counts L1..L16 at huffhead[3..18].
  bits[0] = 0;
  for (L = 1; L <= 16; L++) bits[L] = table[L];

  const u8* huffvals = table + 17;
  int total_codes = 0;
  for (L = 1; L <= 16; L++) total_codes += bits[L];
  if (total_codes < 1 || total_codes > 256 || avail - 17 < total_codes) {
    return TDNG_LJ92_ERROR_CORRUPT;
  }
  // Reject an oversubscribed canonical tree. Kraft-incomplete tables are
  // legal, but unused prefixes remain explicit invalid LUT entries below.
  {
    int left = 1;
    for (L = 1; L <= 16; L++) {
      left = (left << 1) - bits[L];
      if (left < 0) return TDNG_LJ92_ERROR_CORRUPT;
    }
  }
  // Lossless JPEG SSSS categories are 0..16. A larger symbol value becomes the
  // `ssss` used by bitio_decode_diff, where `1 << ssss` and the `64 - ssss` /
  // `bb <<= ssss` shifts are undefined for ssss >= 17/31/64. Such a table is
  // not valid lossless JPEG (it would be a baseline DC/AC table) -- reject it
  // here rather than decoding it.
  for (int v = 0; v < total_codes; v++) {
    if (huffvals[v] > 16) return TDNG_LJ92_ERROR_CORRUPT;
  }

  int maxbits = 16;
  while (maxbits > 0 && !bits[maxbits]) maxbits--;
  if (maxbits <= 0) return TDNG_LJ92_ERROR_CORRUPT;

  size_t lut_entries = (size_t)1 << maxbits;
  u32* hufflut = (u32*)lj92_alloc(allocator, lut_entries * sizeof(u32));
  if (!hufflut) return TDNG_LJ92_ERROR_NO_MEMORY;
  // Kraft-incomplete tables leave holes. Mark them invalid while consuming
  // one bit so corrupt data still terminates in bounded time.
  for (size_t s = 0; s < lut_entries; s++) hufflut[s] = 0x80000001u;

  int i = 0, hv = 0, rv = 0, vl = 0, bitsused = 1;
  while (i < (1 << maxbits)) {
    if (bitsused > maxbits) break;
    if (vl >= bits[bitsused]) { bitsused++; vl = 0; continue; }
    if (rv == (1 << (maxbits - bitsused))) { rv = 0; vl++; hv++; continue; }
    // Entry packs (mask << 6) | total so the entropy loop consumes code +
    // residual with a single shift and takes the residual mask straight
    // from the table (total <= 32 fits 6 bits, mask <= 0xFFFF fits 16).
    {
      u32 sym = huffvals[hv];
      hufflut[i++] =
          ((sym ? ((1u << sym) - 1u) : 0u) << 6) | (u32)(bitsused + (int)sym);
    }
    rv++;
  }
  *consumed_out = 17 + total_codes;
  *table_id_out = table_id;
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
  int hufflen, pos;
  if (self->datalen - self->ix < 2) return TDNG_LJ92_ERROR_CORRUPT;
  hufflen = be16(self->data + self->ix);
  if (hufflen < 2 || hufflen > self->datalen - self->ix) {
    return TDNG_LJ92_ERROR_CORRUPT;
  }
  pos = 2;
  while (pos < hufflen) {
    int used = 0, table_id = 0, maxbits = 0;
    u32* lut = NULL;
    int ret = build_huff_lut(&self->allocator, self->data + self->ix + pos,
                             hufflen - pos, &used, &table_id, &lut, &maxbits);
    if (ret != TDNG_LJ92_ERROR_NONE) return ret;
    lj92_free(&self->allocator, self->hufflut[table_id]);
    if (!self->huff_defined[table_id]) self->num_huff_idx++;
    self->hufflut[table_id] = lut;
    self->huffbits[table_id] = maxbits;
    self->huff_defined[table_id] = 1;
    pos += used;
  }
  if (pos != hufflen) return TDNG_LJ92_ERROR_CORRUPT;
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
  if (Lf < 11 || Lf > self->datalen - self->ix) {
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
  if (Lf != 8 + 3 * self->components || self->x <= 0) {
    return TDNG_LJ92_ERROR_CORRUPT;
  }
  for (int c = 0; c < self->components; c++) {
    const u8* cp = self->data + self->ix - Lf + 8 + 3 * c;
    u8 hv = cp[1];
    if ((hv >> 4) == 0 || (hv & 15) == 0) return TDNG_LJ92_ERROR_CORRUPT;
    for (int p = 0; p < c; p++) {
      if (self->component_id[p] == cp[0]) return TDNG_LJ92_ERROR_CORRUPT;
    }
    self->component_id[c] = cp[0];
    self->component_h[c] = hv >> 4;
    self->component_v[c] = hv & 15;
    if (cp[2] != 0) return TDNG_LJ92_ERROR_CORRUPT;
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

static int parseDac(ljp* self) {
  int len, pos;
  if (self->datalen - self->ix < 2) return TDNG_LJ92_ERROR_CORRUPT;
  len = be16(self->data + self->ix);
  if (len < 4 || (len & 1) != 0 || len > self->datalen - self->ix) {
    return TDNG_LJ92_ERROR_CORRUPT;
  }
  for (pos = 2; pos < len; pos += 2) {
    int tc = self->data[self->ix + pos] >> 4;
    int tb = self->data[self->ix + pos] & 15;
    int cs = self->data[self->ix + pos + 1];
    if (tc != 0 || tb > 3 || (cs & 15) > (cs >> 4)) {
      return TDNG_LJ92_ERROR_CORRUPT;
    }
    self->arith_dc_l[tb] = (u8)(cs & 15);
    self->arith_dc_u[tb] = (u8)(cs >> 4);
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

/* ISA level for encoder hot paths: 0 scalar, 1 SSE2, 2 AVX2. Do not cache
   this in writable static storage: independent decoder handles may first run
   concurrently. GCC/Clang's CPU feature query is process-safe and cheap
   compared with an image operation. */
static int tdng_simd_level(void) {
#if defined(TINY_DNG_LJPEG92_V2_USE_AVX2)
  return 2;
#elif defined(TINY_DNG_LJPEG92_V2_USE_SSE2)
  return 1;
#elif defined(TDNG_LJ92_RUNTIME_SIMD)
#if defined(__GNUC__) || defined(__clang__)
  if (__builtin_cpu_supports("avx2")) {
    return 2;
  }
  if (__builtin_cpu_supports("sse2")) {
    return 1;
  }
  return 0;
#else
  return 0;
#endif
#else
  return 0;
#endif
}

/* One 16-sample left-diff block: blk[k] = (u16)(row[i+k] - row[i+k-1]).
   Target-attributed so library builds stay portable (runtime dispatch). */
#if defined(TINY_DNG_LJPEG92_V2_USE_SSE2) || defined(TDNG_LJ92_RUNTIME_SIMD)
static TDNG_LJ92_TARGET_ISA("sse2") void tdng_diff16_sse2(u16* blk,
                                                          const u16* row,
                                                          size_t i) {
  __m128i cur = _mm_loadu_si128((const __m128i*)(const void*)(row + i));
  __m128i prv = _mm_loadu_si128((const __m128i*)(const void*)(row + i - 1));
  _mm_storeu_si128((__m128i*)(void*)blk, _mm_sub_epi16(cur, prv));
}
#endif
#if defined(TINY_DNG_LJPEG92_V2_USE_AVX2) || defined(TDNG_LJ92_RUNTIME_SIMD)
static TDNG_LJ92_TARGET_ISA("avx2") void tdng_diff16_avx2(u16* blk,
                                                          const u16* row,
                                                          size_t i) {
  __m256i cur = _mm256_loadu_si256((const __m256i*)(const void*)(row + i));
  __m256i prv = _mm256_loadu_si256((const __m256i*)(const void*)(row + i - 1));
  _mm256_storeu_si256((__m256i*)(void*)blk, _mm256_sub_epi16(cur, prv));
}
#endif

/* Fill blk[0..15] with (u16)(row[i+k] - row[i+k-1]) via the widest SIMD the
   host supports; scalar fallback keeps every build configuration working. */
static void tdng_diff16(u16* blk, const u16* row, size_t i) {
#if defined(TINY_DNG_LJPEG92_V2_USE_AVX2)
  tdng_diff16_avx2(blk, row, i);
  return;
#elif defined(TINY_DNG_LJPEG92_V2_USE_SSE2)
  tdng_diff16_sse2(blk, row, i);
  return;
#elif defined(TDNG_LJ92_RUNTIME_SIMD)
  {
    int lvl = tdng_simd_level();
    if (lvl >= 2) {
      tdng_diff16_avx2(blk, row, i);
      return;
    }
    if (lvl == 1) {
      tdng_diff16_sse2(blk, row, i);
      return;
    }
  }
#endif
  {
    int k;
    for (k = 0; k < 16; k++) {
      size_t at = i + (size_t)k;
      blk[k] = (u16)(row[at] - row[at - 1u]);
    }
  }
}

static int td_wrap_diff_u16(int sample, int predictor);
static int enc_px_from_neighbors(int pred, int left, int above, int abovel,
                                 int initpx, int row, int col);
static int enc_ssss(int diff);

static void tdng_enc_diff_row_scalar(const u16* restrict cur,
                                     const u16* restrict prev, int width,
                                     int nc, int pred, int initpx, int row,
                                     int16_t* restrict diff) {
  int n = width * nc;
  for (int c = 0; c < nc; c++) {
    int px = (row == 0) ? initpx : (int)prev[c];
    diff[c] = (int16_t)td_wrap_diff_u16((int)cur[c], px);
  }
  for (int i = nc; i < n; i++) {
    int col = i / nc;
    int left = cur[i - nc];
    int above = prev ? prev[i] : 0;
    int abovel = prev ? prev[i - nc] : 0;
    int px = enc_px_from_neighbors(pred, left, above, abovel, initpx, row, col);
    diff[i] = (int16_t)td_wrap_diff_u16((int)cur[i], px);
  }
}

#if defined(TINY_DNG_LJPEG92_V2_USE_SSE2) || defined(TDNG_LJ92_RUNTIME_SIMD)
static TDNG_LJ92_TARGET_ISA("sse2") void tdng_enc_diff_row_sse2(
    const u16* cur, const u16* prev, int width, int nc, int pred, int initpx,
    int row, int16_t* diff) {
  int n = width * nc;
  int i = nc;
  int mode = row == 0 ? 1 : pred;
  const __m128i one = _mm_set1_epi16(1);
  for (int c = 0; c < nc; c++) {
    int px = row == 0 ? initpx : (int)prev[c];
    diff[c] = (int16_t)td_wrap_diff_u16((int)cur[c], px);
  }
#define TDNG_SSE_DIFF(PXCODE)                                             \
  for (; i + 8 <= n; i += 8) {                                            \
    __m128i cv = _mm_loadu_si128((const __m128i*)(const void*)(cur + i)); \
    __m128i px;                                                           \
    PXCODE;                                                               \
    _mm_storeu_si128((__m128i*)(void*)(diff + i), _mm_sub_epi16(cv, px)); \
  }
  if (mode == 1) {
    TDNG_SSE_DIFF(
        px = _mm_loadu_si128((const __m128i*)(const void*)(cur + i - nc)));
  } else if (mode == 2) {
    TDNG_SSE_DIFF(px =
                      _mm_loadu_si128((const __m128i*)(const void*)(prev + i)));
  } else if (mode == 3) {
    TDNG_SSE_DIFF(
        px = _mm_loadu_si128((const __m128i*)(const void*)(prev + i - nc)));
  } else if (mode == 4) {
    TDNG_SSE_DIFF(
        __m128i l =
            _mm_loadu_si128((const __m128i*)(const void*)(cur + i - nc));
        __m128i a = _mm_loadu_si128((const __m128i*)(const void*)(prev + i));
        __m128i al =
            _mm_loadu_si128((const __m128i*)(const void*)(prev + i - nc));
        px = _mm_sub_epi16(_mm_add_epi16(l, a), al));
  } else if (mode == 7) {
    TDNG_SSE_DIFF(
        __m128i l =
            _mm_loadu_si128((const __m128i*)(const void*)(cur + i - nc));
        __m128i a = _mm_loadu_si128((const __m128i*)(const void*)(prev + i));
        px = _mm_sub_epi16(_mm_avg_epu16(l, a),
                           _mm_and_si128(_mm_xor_si128(l, a), one)));
  }
#undef TDNG_SSE_DIFF
  for (; i < n; i++) {
    int col = i / nc;
    int px = enc_px_from_neighbors(pred, cur[i - nc], prev ? prev[i] : 0,
                                   prev ? prev[i - nc] : 0, initpx, row, col);
    diff[i] = (int16_t)td_wrap_diff_u16((int)cur[i], px);
  }
}

static TDNG_LJ92_TARGET_ISA("sse2") void tdng_ssss_row_sse2(const int16_t* diff,
                                                            int n,
                                                            u8* categories) {
  const __m128i bias = _mm_set1_epi32(126);
  const __m128i zero = _mm_setzero_si128();
  int i = 0;
  for (; i + 8 <= n; i += 8) {
    __m128i v = _mm_loadu_si128((const __m128i*)(const void*)(diff + i));
    __m128i sign = _mm_srai_epi16(v, 15);
    __m128i mag = _mm_sub_epi16(_mm_xor_si128(v, sign), sign);
    __m128i lo = _mm_unpacklo_epi16(mag, zero);
    __m128i hi = _mm_unpackhi_epi16(mag, zero);
    __m128i elo = _mm_srli_epi32(_mm_castps_si128(_mm_cvtepi32_ps(lo)), 23);
    __m128i ehi = _mm_srli_epi32(_mm_castps_si128(_mm_cvtepi32_ps(hi)), 23);
    elo = _mm_sub_epi32(elo, bias);
    ehi = _mm_sub_epi32(ehi, bias);
    elo = _mm_andnot_si128(_mm_srai_epi32(elo, 31), elo);
    ehi = _mm_andnot_si128(_mm_srai_epi32(ehi, 31), ehi);
    {
      __m128i p16 = _mm_packs_epi32(elo, ehi);
      __m128i p8 = _mm_packus_epi16(p16, p16);
      _mm_storel_epi64((__m128i*)(void*)(categories + i), p8);
    }
  }
  for (; i < n; i++) categories[i] = (u8)enc_ssss((int)diff[i]);
}
#endif

#if defined(TINY_DNG_LJPEG92_V2_USE_AVX2) || defined(TDNG_LJ92_RUNTIME_SIMD)
static TDNG_LJ92_TARGET_ISA("avx2") void tdng_enc_diff_row_avx2(
    const u16* cur, const u16* prev, int width, int nc, int pred, int initpx,
    int row, int16_t* diff) {
  int n = width * nc;
  int i = nc;
  int mode = row == 0 ? 1 : pred;
  const __m256i one = _mm256_set1_epi16(1);
  for (int c = 0; c < nc; c++) {
    int px = row == 0 ? initpx : (int)prev[c];
    diff[c] = (int16_t)td_wrap_diff_u16((int)cur[c], px);
  }
#define TDNG_AVX_DIFF(PXCODE)                                                \
  for (; i + 16 <= n; i += 16) {                                             \
    __m256i cv = _mm256_loadu_si256((const __m256i*)(const void*)(cur + i)); \
    __m256i px;                                                              \
    PXCODE;                                                                  \
    _mm256_storeu_si256((__m256i*)(void*)(diff + i),                         \
                        _mm256_sub_epi16(cv, px));                           \
  }
  if (mode == 1) {
    TDNG_AVX_DIFF(
        px = _mm256_loadu_si256((const __m256i*)(const void*)(cur + i - nc)));
  } else if (mode == 2) {
    TDNG_AVX_DIFF(
        px = _mm256_loadu_si256((const __m256i*)(const void*)(prev + i)));
  } else if (mode == 3) {
    TDNG_AVX_DIFF(
        px = _mm256_loadu_si256((const __m256i*)(const void*)(prev + i - nc)));
  } else if (mode == 4) {
    TDNG_AVX_DIFF(
        __m256i l =
            _mm256_loadu_si256((const __m256i*)(const void*)(cur + i - nc));
        __m256i a = _mm256_loadu_si256((const __m256i*)(const void*)(prev + i));
        __m256i al =
            _mm256_loadu_si256((const __m256i*)(const void*)(prev + i - nc));
        px = _mm256_sub_epi16(_mm256_add_epi16(l, a), al));
  } else if (mode == 7) {
    TDNG_AVX_DIFF(
        __m256i l =
            _mm256_loadu_si256((const __m256i*)(const void*)(cur + i - nc));
        __m256i a = _mm256_loadu_si256((const __m256i*)(const void*)(prev + i));
        px = _mm256_sub_epi16(_mm256_avg_epu16(l, a),
                              _mm256_and_si256(_mm256_xor_si256(l, a), one)));
  }
#undef TDNG_AVX_DIFF
  for (; i < n; i++) {
    int col = i / nc;
    int px = enc_px_from_neighbors(pred, cur[i - nc], prev ? prev[i] : 0,
                                   prev ? prev[i - nc] : 0, initpx, row, col);
    diff[i] = (int16_t)td_wrap_diff_u16((int)cur[i], px);
  }
}
#endif

#if defined(TDNG_LJ92_NEON)
static void tdng_enc_diff_row_neon(const u16* cur, const u16* prev, int width,
                                   int nc, int pred, int initpx, int row,
                                   int16_t* diff) {
  int n = width * nc;
  int i = nc;
  int mode = row == 0 ? 1 : pred;
  for (int c = 0; c < nc; c++) {
    int px = row == 0 ? initpx : (int)prev[c];
    diff[c] = (int16_t)td_wrap_diff_u16((int)cur[c], px);
  }
  for (; i + 8 <= n &&
         (mode == 1 || mode == 2 || mode == 3 || mode == 4 || mode == 7);
       i += 8) {
    uint16x8_t cv = vld1q_u16(cur + i);
    uint16x8_t px;
    if (mode == 1)
      px = vld1q_u16(cur + i - nc);
    else if (mode == 2)
      px = vld1q_u16(prev + i);
    else if (mode == 3)
      px = vld1q_u16(prev + i - nc);
    else if (mode == 4) {
      px = vsubq_u16(vaddq_u16(vld1q_u16(cur + i - nc), vld1q_u16(prev + i)),
                     vld1q_u16(prev + i - nc));
    } else {
      px = vhaddq_u16(vld1q_u16(cur + i - nc), vld1q_u16(prev + i));
    }
    vst1q_s16(diff + i, vreinterpretq_s16_u16(vsubq_u16(cv, px)));
  }
  for (; i < n; i++) {
    int col = i / nc;
    int px = enc_px_from_neighbors(pred, cur[i - nc], prev ? prev[i] : 0,
                                   prev ? prev[i - nc] : 0, initpx, row, col);
    diff[i] = (int16_t)td_wrap_diff_u16((int)cur[i], px);
  }
}

static void tdng_ssss_row_neon(const int16_t* diff, int n, u8* categories) {
  int i = 0;
  const uint16x8_t sixteen = vdupq_n_u16(16);
  for (; i + 8 <= n; i += 8) {
    int16x8_t v = vld1q_s16(diff + i);
    uint16x8_t mag = vreinterpretq_u16_s16(vabsq_s16(v));
    uint16x8_t ssss = vsubq_u16(sixteen, vclzq_u16(mag));
    vst1_u8(categories + i, vmovn_u16(ssss));
  }
  for (; i < n; i++) categories[i] = (u8)enc_ssss((int)diff[i]);
}
#endif

#if defined(TINY_DNG_LJPEG92_V2_USE_SSE2) || defined(TDNG_LJ92_RUNTIME_SIMD)
static TDNG_LJ92_TARGET_ISA("sse2") void tdng_prefix_sum_u16_sse2(
    u16* dst, const u16* diff, int count, u16 seed) {
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

#if defined(TINY_DNG_LJPEG92_V2_USE_AVX2) || defined(TDNG_LJ92_RUNTIME_SIMD)
static TDNG_LJ92_TARGET_ISA("avx2") void tdng_prefix_sum_u16_avx2(
    u16* dst, const u16* diff, int count, u16 seed) {
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

static void tdng_prefix_sum_u16(u16* dst, const u16* diff, int count,
                                u16 seed) {
#if defined(TINY_DNG_LJPEG92_V2_USE_AVX2)
  tdng_prefix_sum_u16_avx2(dst, diff, count, seed);
#elif defined(TINY_DNG_LJPEG92_V2_USE_SSE2)
  tdng_prefix_sum_u16_sse2(dst, diff, count, seed);
#elif defined(TDNG_LJ92_RUNTIME_SIMD)
  int level = tdng_simd_level();
  if (level == 2) {
    tdng_prefix_sum_u16_avx2(dst, diff, count, seed);
  } else if (level == 1) {
    tdng_prefix_sum_u16_sse2(dst, diff, count, seed);
  } else {
    tdng_prefix_sum_u16_scalar(dst, diff, count, seed);
  }
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
    u8* nb = (u8*)lj92_realloc_copy(&self->allocator, self->ebuf,
                                    (size_t)self->ebuf_cap, (size_t)cap_needed);
    if (!nb) return TDNG_LJ92_ERROR_NO_MEMORY;
    self->ebuf = nb;
    self->ebuf_cap = cap_needed;
  }
  const u8* src = self->data + start_off;
  const u8* end = self->data + self->datalen;
  u8* dst = self->ebuf;
  {
    int marker = -1;
    while (src < end) {
      u8 c = *src++;
      if (c != 0xFF) {
        *dst++ = c;
        continue;
      }
      if (src >= end) return TDNG_LJ92_ERROR_CORRUPT;
      while (src < end && *src == 0xFF) src++; /* legal marker fill bytes */
      if (src >= end) return TDNG_LJ92_ERROR_CORRUPT;
      c = *src++;
      if (c == 0x00) {
        *dst++ = 0xFF; /* entropy byte stuffing */
        continue;
      }
      marker = c;
      break;
    }
    // The legacy decoder handles one scan and no restart intervals. Do not
    // treat an arbitrary marker or physical EOF as successful entropy data.
    if (marker != 0xD9)
      return marker >= 0xD0 && marker <= 0xD7 ? TDNG_LJ92_ERROR_UNSUPPORTED
                                              : TDNG_LJ92_ERROR_CORRUPT;
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

/* T.81 predictors 5 and 6 require mathematical floor division. Right-shift
   of a negative signed integer is implementation-defined in C11. */
static TDNG_ALWAYS_INLINE int td_floor_half(int v) {
  return v >= 0 ? v / 2 : -(((-v) + 1) / 2);
}

/* Preserve the codec's modulo-2^16 residual convention without relying on
   implementation-defined conversion from an out-of-range int to int16_t. */
static TDNG_ALWAYS_INLINE int td_wrap_diff_u16(int sample, int predictor) {
  uint16_t u = (uint16_t)((uint32_t)sample - (uint32_t)predictor);
  return u >= 0x8000u ? (int)u - 0x10000 : (int)u;
}

static TDNG_ALWAYS_INLINE int parseScanRun(ljp* self, bitio_t* bio,
                                           const int PRED, const int NC,
                                           const int LIN) {
  const int W = self->x;
  const int H = self->y;
  const int out_stride = W * NC + self->skiplen;
  const int init_px = 1 << (self->bits - 1);
  const u16* lin = self->linearize;
  const int linlen = self->linlen;

  const u32* hl[LJ92_MAX_COMPONENTS];
  int hb[LJ92_MAX_COMPONENTS];
  if (self->num_huff_idx < 1) return TDNG_LJ92_ERROR_CORRUPT;
  for (int c = 0; c < NC; c++) {
    int idx = self->scan_huff_id[c];
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
          case 5:
            Px = left + td_floor_half(above - abovel);
            break;
          case 6:
            Px = above + td_floor_half(left - abovel);
            break;
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

  const u32* hl[LJ92_MAX_COMPONENTS];
  int hb[LJ92_MAX_COMPONENTS];
  if (self->num_huff_idx < 1) return TDNG_LJ92_ERROR_CORRUPT;
  for (int c = 0; c < NC; c++) {
    int idx = self->scan_huff_id[c];
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
          case 5:
            Px = left + td_floor_half(above - abovel);
            break;
          case 6:
            Px = above + td_floor_half(left - abovel);
            break;
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

  const int table_id = self->scan_huff_id[0];
  const u32* hl = self->hufflut[table_id];
  const int hb = self->huffbits[table_id];
  if (!hl) return TDNG_LJ92_ERROR_CORRUPT;

  const int W = self->x;
  const u16* lin = self->linearize;
  const int linlen = self->linlen;

  // skiplen is always 0 (tdng_lj92_decode rejects non-zero), so for the
  // non-linearized case the previous output row IS the raw predictor row:
  // prefix-sum straight into `out` and drop the row cache + row memcpy.
  if (!lin) {
    for (int row = 0; row < self->y; row++) {
      int first_px = (row == 0) ? (1 << (self->bits - 1)) : (int)out[-W];
      u16* diffs = self->diffcache;
      for (int col = 0; col < W; col++) {
        bitio_refill(bio);
        diffs[col] = (u16)bitio_decode_diff(bio, hl, hb);
      }
      tdng_prefix_sum_u16(out, diffs, W, (u16)first_px);
      out += W;
    }
    if (bitio_check_overrun(bio)) return TDNG_LJ92_ERROR_CORRUPT;
    return TDNG_LJ92_ERROR_NONE;
  }

  for (int row = 0; row < self->y; row++) {
    int first_px = (row == 0) ? (1 << (self->bits - 1)) : (int)lastrow[0];

    // Decode W diffs into diffcache with the new bit IO.
    for (int col = 0; col < W; col++) {
      bitio_refill(bio);
      self->diffcache[col] = (u16)bitio_decode_diff(bio, hl, hb);
    }

    tdng_prefix_sum_u16(thisrow, self->diffcache, W, (u16)first_px);

    for (int col = 0; col < W; col++) {
      if (thisrow[col] >= linlen) return TDNG_LJ92_ERROR_CORRUPT;
      out[col] = lin[thisrow[col]];
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
static int parse_sos_payload(ljp* self, const u8* payload, int payload_len,
                             int expected_comps, int* comps_out, int* pred_out,
                             int* point_transform_out, int* sos_len_out) {
  // Hardening: SOS = Ls(2) Ns(1) [Cs Td/Ta]*Ns Ss Se Ah/Al.
  // Need at least 3 bytes to read Ls and Ns.
  if (payload_len < 3) return TDNG_LJ92_ERROR_CORRUPT;
  int Ls = be16(&payload[0]);
  int compcount = payload[2];
  if (Ls != 6 + 2 * compcount || Ls > payload_len) {
    return TDNG_LJ92_ERROR_CORRUPT;
  }
  if (compcount < 1 || compcount > LJ92_MAX_COMPONENTS) {
    return TDNG_LJ92_ERROR_CORRUPT;
  }
  // If SOF3 has been seen, SOS must name the same number of components.
  if (expected_comps > 0 && compcount != expected_comps) {
    return TDNG_LJ92_ERROR_CORRUPT;
  }
  for (int s = 0; s < compcount; s++) {
    u8 selector = payload[3 + 2 * s];
    u8 tables = payload[4 + 2 * s];
    int frame_c = -1;
    if ((tables & 15) != 0 || (tables >> 4) > 3) {
      return TDNG_LJ92_ERROR_CORRUPT;
    }
    for (int c = 0; c < self->components; c++) {
      if (self->component_id[c] == selector) {
        frame_c = c;
        break;
      }
    }
    if (frame_c < 0 || frame_c != s ||
        (self->sof_marker == 0xC3 && !self->huff_defined[tables >> 4])) {
      return TDNG_LJ92_ERROR_CORRUPT;
    }
    self->scan_huff_id[frame_c] = tables >> 4;
  }
  int pred = payload[3 + 2 * compcount];
  int se = payload[4 + 2 * compcount];
  int ahal = payload[5 + 2 * compcount];
  int pt = ahal & 15;
  if (pred < 1 || pred > 7 || se != 0 || (ahal >> 4) != 0 || pt >= self->bits) {
    return TDNG_LJ92_ERROR_CORRUPT;
  }
  *comps_out = compcount;
  *pred_out = pred;
  *point_transform_out = pt;
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
      /* Mono predictor-1: the non-LIN fast path prefix-sums directly into
         the output and predicts from the previous output row, so no row
         cache is required (LIN keeps it). */
      need_rows = pred != 7 && pred != 1;
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
    self->rowcache =
        (u16*)lj92_alloc_zero(&self->allocator, row_slots * 2u * sizeof(u16));
    if (!self->rowcache) return TDNG_LJ92_ERROR_NO_MEMORY;
    self->outrow[0] = self->rowcache;
    self->outrow[1] = self->rowcache + row_slots;
  }
  if (need_diff && !self->diffcache) {
    if (row_slots > SIZE_MAX / sizeof(u16)) {
      return TDNG_LJ92_ERROR_NO_MEMORY;
    }
    self->diffcache =
        (u16*)lj92_alloc_zero(&self->allocator, row_slots * sizeof(u16));
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
  if (self->sof_marker == 0xCB) return TDNG_LJ92_ERROR_UNSUPPORTED;
  self->ix = self->scanstart;
  // Hardening: SOS = Ls(2) Ns(1) [Cs Td/Ta]*Ns Ss Se Ah/Al.
  int compcount = 0, pred = 0, pt = 0, Ls = 0;
  int ret =
      parse_sos_payload(self, self->data + self->ix, self->datalen - self->ix,
                        self->components, &compcount, &pred, &pt, &Ls);
  if (ret != TDNG_LJ92_ERROR_NONE) return ret;
  if (pt != 0) return TDNG_LJ92_ERROR_UNSUPPORTED;

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
    } else if (nextMarker == 0xcc) {
      ret = parseDac(self);
    } else if (nextMarker == 0xc3 || nextMarker == 0xcb) {
      ret = parseSof3(self, nextMarker);
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
    } else if (nextMarker == 0x01) {  // TEM: standalone arithmetic marker
      continue;
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
  for (int i = 0; i < 4; i++) {
    if (self->huff_defined[i] && self->huffbits[i] > M) M = self->huffbits[i];
  }
  self->huff_maxbits = M;
  for (int i = 0; i < 4; i++) {
    if (!self->huff_defined[i]) continue;
    int m = self->huffbits[i];
    if (m == M) continue;
    size_t oldn = (size_t)1 << m;
    int step = 1 << (M - m);
    u32* nl =
        (u32*)lj92_alloc(&self->allocator, ((size_t)1 << M) * sizeof(u32));
    if (!nl) return TDNG_LJ92_ERROR_NO_MEMORY;
    const u32* ol = self->hufflut[i];
    for (size_t j = 0; j < oldn; j++) {
      for (int k = 0; k < step; k++) nl[j * (size_t)step + (size_t)k] = ol[j];
    }
    lj92_free(&self->allocator, self->hufflut[i]);
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
    if (ret == TDNG_LJ92_ERROR_NONE && self->sof_marker != 0xC3 &&
        self->sof_marker != 0xCB) {
      return TDNG_LJ92_ERROR_NOT_LOSSLESS;
    }
    if (ret == TDNG_LJ92_ERROR_NONE && self->sof_marker == 0xC3) {
      ret = expand_luts_uniform(self);
    }
  } else {
    TINY_DNG_DPRINTF("findSoI: corrupt\n");
  }
  return ret;
}

static int find_dnl_height_memory(ljp* self) {
  size_t pos = (size_t)self->scanstart;
  while (pos + 5u < (size_t)self->datalen) {
    if (self->data[pos++] != 0xffu) continue;
    while (pos < (size_t)self->datalen && self->data[pos] == 0xffu) pos++;
    if (pos >= (size_t)self->datalen) break;
    if (self->data[pos] == 0x00u) {
      pos++;
      continue;
    }
    if (self->data[pos++] == 0xdcu) {
      if (pos + 4u > (size_t)self->datalen || be16(self->data + pos) != 4) {
        return TDNG_LJ92_ERROR_CORRUPT;
      }
      self->y = be16(self->data + pos + 2u);
      if (self->y <= 0) return TDNG_LJ92_ERROR_CORRUPT;
      self->height_from_dnl = 1;
      return TDNG_LJ92_ERROR_NONE;
    }
  }
  return TDNG_LJ92_ERROR_CORRUPT;
}

static void free_memory(ljp* self) {
  for (int i = 0; i < 4; i++) {
    lj92_free(&self->allocator, self->hufflut[i]);
    self->hufflut[i] = NULL;
  }
  lj92_free(&self->allocator, self->rowcache);
  self->rowcache = NULL;
  lj92_free(&self->allocator, self->diffcache);
  self->diffcache = NULL;
  lj92_free(&self->allocator, self->ebuf);
  self->ebuf = NULL;
  self->ebuf_len = 0;
  self->ebuf_cap = 0;
}

int tdng_lj92_open_ex(tdng_lj92* lj, const uint8_t* data, int datalen,
                      const tdng_lj92_allocator* allocator, int* width,
                      int* height, int* bitdepth, int* components) {
  tdng_lj92_allocator resolved;
  if (!lj || !data || datalen < 4 || !width || !height || !bitdepth ||
      !components) {
    return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
  }
  if (!lj92_allocator_resolve(allocator, &resolved)) {
    return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
  }
  *lj = NULL;
  ljp* self = (ljp*)lj92_alloc_zero(&resolved, sizeof(ljp));
  if (!self) return TDNG_LJ92_ERROR_NO_MEMORY;
  self->allocator = resolved;
  for (int t = 0; t < 4; t++) self->arith_dc_u[t] = 1;

  self->data = (u8*)data;
  self->datalen = datalen;

  int ret = findSoI(self);

  if (ret == TDNG_LJ92_ERROR_NONE && self->y == 0) {
    ret = find_dnl_height_memory(self);
  }

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
    lj92_free(&self->allocator, self);
  } else {
    *width = self->x;
    *height = self->y;
    *bitdepth = self->bits;
    *components = self->components;
    *lj = self;
  }
  return ret;
}

int tdng_lj92_open(tdng_lj92* lj, const uint8_t* data, int datalen, int* width,
                   int* height, int* bitdepth, int* components) {
  return tdng_lj92_open_ex(lj, data, datalen, NULL, width, height, bitdepth,
                           components);
}

int tdng_lj92_decode(tdng_lj92 lj, uint16_t* target, int writeLength,
                     int skipLength, uint16_t* linearize,
                     int linearizeLength) {
  ljp* self = lj;
  if (!self) return TDNG_LJ92_ERROR_BAD_HANDLE;
  if (!target || writeLength < 0 || linearizeLength < 0 ||
      (linearize != NULL && linearizeLength == 0) ||
      (linearize == NULL && linearizeLength != 0)) {
    return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
  }
  // Hardening: legacy per-row skip support was removed from the decoder, so a
  // non-zero skipLength would make the scan loops write `skipLength` extra
  // samples past `target` every row (out_stride = W*NC + skipLength). Reject it
  // rather than risking an out-of-bounds write. The integrated tinydng codec
  // always passes skipLength = 0.
  if (skipLength != 0) return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
  // Hardening: reject degenerate/overflowing dimensions. The scan loops index
  // `out_stride = W*NC` samples per row; guard the per-row stride against int
  // overflow and the pixel count against size_t overflow before writing.
  if (self->x <= 0 || self->y <= 0 || self->components <= 0) {
    return TDNG_LJ92_ERROR_CORRUPT;
  }
  for (int c = 0; c < self->components; c++) {
    if (self->component_h[c] != 1 || self->component_v[c] != 1) {
      return TDNG_LJ92_ERROR_UNSUPPORTED;
    }
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
      if (writeLength < self->x * self->components) {
        return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
      }
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

int tdng_lj92_get_frame_info(tdng_lj92 lj, tdng_lj92_frame_info* info) {
  ljp* self = lj;
  int hmax = 0, vmax = 0;
  if (!self || !info) return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
  memset(info, 0, sizeof(*info));
  for (int c = 0; c < self->components; c++) {
    if (self->component_h[c] > hmax) hmax = self->component_h[c];
    if (self->component_v[c] > vmax) vmax = self->component_v[c];
  }
  if (hmax <= 0 || vmax <= 0) return TDNG_LJ92_ERROR_CORRUPT;
  info->width = (uint32_t)self->x;
  info->height = (uint32_t)self->y;
  info->precision = (uint8_t)self->bits;
  info->component_count = (uint8_t)self->components;
  info->sof_marker = (uint8_t)self->sof_marker;
  for (int c = 0; c < self->components; c++) {
    tdng_lj92_component_info* ci = &info->components[c];
    ci->id = self->component_id[c];
    ci->h_sampling = self->component_h[c];
    ci->v_sampling = self->component_v[c];
    ci->width = ((uint32_t)self->x * ci->h_sampling + (uint32_t)hmax - 1u) /
                (uint32_t)hmax;
    ci->height = ((uint32_t)self->y * ci->v_sampling + (uint32_t)vmax - 1u) /
                 (uint32_t)vmax;
  }
  return TDNG_LJ92_ERROR_NONE;
}

static int lj92_mem_next_marker(ljp* self, size_t* pos_io) {
  size_t pos = *pos_io;
  while (pos + 1u < (size_t)self->datalen) {
    if (self->data[pos++] != 0xffu) continue;
    while (pos < (size_t)self->datalen && self->data[pos] == 0xffu) pos++;
    if (pos >= (size_t)self->datalen) return -1;
    if (self->data[pos] == 0x00u) {
      pos++;
      continue;
    }
    *pos_io = pos + 1u;
    return self->data[pos];
  }
  return -1;
}

static int lj92_mem_destuff_chunk(ljp* self, size_t start, size_t* next_pos,
                                  int* marker_out) {
  size_t remain;
  size_t need;
  const u8* src;
  const u8* end;
  u8* dst;
  int marker = -1;
  if (start >= (size_t)self->datalen) return TDNG_LJ92_ERROR_CORRUPT;
  remain = (size_t)self->datalen - start;
  if (remain > SIZE_MAX - LJ92_ENTROPY_ALLOC_PAD) {
    return TDNG_LJ92_ERROR_LIMIT;
  }
  need = remain + LJ92_ENTROPY_ALLOC_PAD;
  if ((size_t)self->ebuf_cap < need) {
    u8* nb = (u8*)lj92_realloc_copy(&self->allocator, self->ebuf,
                                    (size_t)self->ebuf_cap, need);
    if (!nb) return TDNG_LJ92_ERROR_NO_MEMORY;
    self->ebuf = nb;
    self->ebuf_cap = (int)need;
  }
  src = self->data + start;
  end = self->data + self->datalen;
  dst = self->ebuf;
  while (src < end) {
    u8 b = *src++;
    if (b != 0xffu) {
      *dst++ = b;
      continue;
    }
    if (src >= end) return TDNG_LJ92_ERROR_CORRUPT;
    while (src < end && *src == 0xffu) src++;
    if (src >= end) return TDNG_LJ92_ERROR_CORRUPT;
    b = *src++;
    if (b == 0x00u) {
      *dst++ = 0xffu;
      continue;
    }
    marker = b;
    break;
  }
  if (marker < 0) return TDNG_LJ92_ERROR_CORRUPT;
  self->ebuf_len = (int)(dst - self->ebuf);
  memset(dst, 0, LJ92_ENTROPY_ALLOC_PAD);
  *next_pos = (size_t)(src - self->data);
  *marker_out = marker;
  return TDNG_LJ92_ERROR_NONE;
}

static int lj92_parse_dht_segment(ljp* self, const u8* segment, int length) {
  int pos = 2;
  if (length < 2 || be16(segment) != length) return TDNG_LJ92_ERROR_CORRUPT;
  while (pos < length) {
    int used = 0, table_id = 0, maxbits = 0;
    u32* lut = NULL;
    int ret = build_huff_lut(&self->allocator, segment + pos, length - pos,
                             &used, &table_id, &lut, &maxbits);
    if (ret != TDNG_LJ92_ERROR_NONE) return ret;
    lj92_free(&self->allocator, self->hufflut[table_id]);
    if (!self->huff_defined[table_id]) self->num_huff_idx++;
    self->hufflut[table_id] = lut;
    self->huffbits[table_id] = maxbits;
    self->huff_defined[table_id] = 1;
    pos += used;
  }
  return pos == length ? TDNG_LJ92_ERROR_NONE : TDNG_LJ92_ERROR_CORRUPT;
}

typedef struct lj92_scan_desc {
  int count;
  int component[4];
  int table[4];
  int predictor;
  int point_transform;
} lj92_scan_desc;

/* T.81 Table D.2, packed as Qe | NM | (NL + switch-MPS).  This table is
 * normative codec data; keeping it compact makes both coder directions use
 * the exact same state transitions. */
#define ARI(qe, nl, nm, sw)                         \
  (((uint32_t)(qe) << 16) | ((uint32_t)(nm) << 8) | \
   (uint32_t)((nl) | ((sw) << 7)))
static const uint32_t lj92_aritab[113] = {
    ARI(0x5a1d, 1, 1, 1),     ARI(0x2586, 14, 2, 0),
    ARI(0x1114, 16, 3, 0),    ARI(0x080b, 18, 4, 0),
    ARI(0x03d8, 20, 5, 0),    ARI(0x01da, 23, 6, 0),
    ARI(0x00e5, 25, 7, 0),    ARI(0x006f, 28, 8, 0),
    ARI(0x0036, 30, 9, 0),    ARI(0x001a, 33, 10, 0),
    ARI(0x000d, 35, 11, 0),   ARI(0x0006, 9, 12, 0),
    ARI(0x0003, 10, 13, 0),   ARI(0x0001, 12, 13, 0),
    ARI(0x5a7f, 15, 15, 1),   ARI(0x3f25, 36, 16, 0),
    ARI(0x2cf2, 38, 17, 0),   ARI(0x207c, 39, 18, 0),
    ARI(0x17b9, 40, 19, 0),   ARI(0x1182, 42, 20, 0),
    ARI(0x0cef, 43, 21, 0),   ARI(0x09a1, 45, 22, 0),
    ARI(0x072f, 46, 23, 0),   ARI(0x055c, 48, 24, 0),
    ARI(0x0406, 49, 25, 0),   ARI(0x0303, 51, 26, 0),
    ARI(0x0240, 52, 27, 0),   ARI(0x01b1, 54, 28, 0),
    ARI(0x0144, 56, 29, 0),   ARI(0x00f5, 57, 30, 0),
    ARI(0x00b7, 59, 31, 0),   ARI(0x008a, 60, 32, 0),
    ARI(0x0068, 62, 33, 0),   ARI(0x004e, 63, 34, 0),
    ARI(0x003b, 32, 35, 0),   ARI(0x002c, 33, 9, 0),
    ARI(0x5ae1, 37, 37, 1),   ARI(0x484c, 64, 38, 0),
    ARI(0x3a0d, 65, 39, 0),   ARI(0x2ef1, 67, 40, 0),
    ARI(0x261f, 68, 41, 0),   ARI(0x1f33, 69, 42, 0),
    ARI(0x19a8, 70, 43, 0),   ARI(0x1518, 72, 44, 0),
    ARI(0x1177, 73, 45, 0),   ARI(0x0e74, 74, 46, 0),
    ARI(0x0bfb, 75, 47, 0),   ARI(0x09f8, 77, 48, 0),
    ARI(0x0861, 78, 49, 0),   ARI(0x0706, 79, 50, 0),
    ARI(0x05cd, 48, 51, 0),   ARI(0x04de, 50, 52, 0),
    ARI(0x040f, 50, 53, 0),   ARI(0x0363, 51, 54, 0),
    ARI(0x02d4, 52, 55, 0),   ARI(0x025c, 53, 56, 0),
    ARI(0x01f8, 54, 57, 0),   ARI(0x01a4, 55, 58, 0),
    ARI(0x0160, 56, 59, 0),   ARI(0x0125, 57, 60, 0),
    ARI(0x00f6, 58, 61, 0),   ARI(0x00cb, 59, 62, 0),
    ARI(0x00ab, 61, 63, 0),   ARI(0x008f, 61, 32, 0),
    ARI(0x5b12, 65, 65, 1),   ARI(0x4d04, 80, 66, 0),
    ARI(0x412c, 81, 67, 0),   ARI(0x37d8, 82, 68, 0),
    ARI(0x2fe8, 83, 69, 0),   ARI(0x293c, 84, 70, 0),
    ARI(0x2379, 86, 71, 0),   ARI(0x1edf, 87, 72, 0),
    ARI(0x1aa9, 87, 73, 0),   ARI(0x174e, 72, 74, 0),
    ARI(0x1424, 72, 75, 0),   ARI(0x119c, 74, 76, 0),
    ARI(0x0f6b, 74, 77, 0),   ARI(0x0d51, 75, 78, 0),
    ARI(0x0bb6, 77, 79, 0),   ARI(0x0a40, 77, 48, 0),
    ARI(0x5832, 80, 81, 1),   ARI(0x4d1c, 88, 82, 0),
    ARI(0x438e, 89, 83, 0),   ARI(0x3bdd, 90, 84, 0),
    ARI(0x34ee, 91, 85, 0),   ARI(0x2eae, 92, 86, 0),
    ARI(0x299a, 93, 87, 0),   ARI(0x2516, 86, 71, 0),
    ARI(0x5570, 88, 89, 1),   ARI(0x4ca9, 95, 90, 0),
    ARI(0x44d9, 96, 91, 0),   ARI(0x3e22, 97, 92, 0),
    ARI(0x3824, 99, 93, 0),   ARI(0x32b4, 99, 94, 0),
    ARI(0x2e17, 93, 86, 0),   ARI(0x56a8, 95, 96, 1),
    ARI(0x4f46, 101, 97, 0),  ARI(0x47e5, 102, 98, 0),
    ARI(0x41cf, 103, 99, 0),  ARI(0x3c3d, 104, 100, 0),
    ARI(0x375e, 99, 93, 0),   ARI(0x5231, 105, 102, 0),
    ARI(0x4c0f, 106, 103, 0), ARI(0x4639, 107, 104, 0),
    ARI(0x415e, 103, 99, 0),  ARI(0x5627, 105, 106, 1),
    ARI(0x50e7, 108, 107, 0), ARI(0x4b85, 109, 103, 0),
    ARI(0x5597, 110, 109, 0), ARI(0x504f, 111, 107, 0),
    ARI(0x5a10, 110, 111, 1), ARI(0x5522, 112, 109, 0),
    ARI(0x59eb, 112, 111, 1)};
#undef ARI

typedef struct lj92_arith_decoder {
  const u8* p;
  const u8* end;
  uint32_t a;
  uint32_t c;
  int ct;
  int synthetic_bytes;
} lj92_arith_decoder;

typedef struct lj92_arith_scan_state {
  struct {
    u8 sign_zero[5][5][4]; /* S0, SS, SP, SN */
    u8 mag_low[2][15];     /* X, M */
    u8 mag_high[2][15];
  } table[4];
  int16_t* above[LJ92_MAX_COMPONENTS];
  int left[LJ92_MAX_COMPONENTS];
  lj92_arith_decoder coder;
} lj92_arith_scan_state;

static int lj92_arith_decode_bit(lj92_arith_decoder* d, u8* st) {
  uint32_t packed, qe, temp;
  int sv;
  while (d->a < 0x8000u) {
    if (--d->ct < 0) {
      uint32_t data = 0;
      if (d->p < d->end)
        data = *d->p++;
      else
        d->synthetic_bytes++;
      d->c = (d->c << 8) | data;
      d->ct += 8;
      if (d->ct < 0 && ++d->ct == 0) d->a = 0x8000u;
    }
    d->a <<= 1;
  }
  sv = *st;
  packed = lj92_aritab[sv & 0x7f];
  qe = packed >> 16;
  temp = d->a - qe;
  d->a = temp;
  temp <<= d->ct;
  if (d->c >= temp) {
    d->c -= temp;
    if (d->a < qe) {
      d->a = qe;
      *st = (u8)((sv & 0x80) ^ ((packed >> 8) & 0xff));
    } else {
      d->a = qe;
      *st = (u8)((sv & 0x80) ^ (packed & 0xff));
      sv ^= 0x80;
    }
  } else if (d->a < 0x8000u) {
    if (d->a < qe) {
      *st = (u8)((sv & 0x80) ^ (packed & 0xff));
      sv ^= 0x80;
    } else {
      *st = (u8)((sv & 0x80) ^ ((packed >> 8) & 0xff));
    }
  }
  return sv >> 7;
}

static int lj92_arith_classify(int diff, int l, int u) {
  unsigned int magnitude = (unsigned int)(diff < 0 ? -diff : diff);
  if (magnitude <= ((1u << l) >> 1)) return 0;
  if (magnitude <= (1u << u)) return diff < 0 ? -1 : 1;
  return diff < 0 ? -2 : 2;
}

static int lj92_arith_decode_diff(ljp* self, const lj92_scan_desc* scan,
                                  int scan_component, uint32_t x,
                                  lj92_arith_scan_state* state, int* diff_out) {
  int c = scan->component[scan_component];
  int table = scan->table[scan_component];
  int da = x == 0 ? 0 : state->left[c];
  int db = state->above[c][x];
  int ca = lj92_arith_classify(da, self->arith_dc_l[table],
                               self->arith_dc_u[table]) +
           2;
  int cb = lj92_arith_classify(db, self->arith_dc_l[table],
                               self->arith_dc_u[table]) +
           2;
  u8* z = state->table[table].sign_zero[ca][cb];
  int v;
  if (lj92_arith_decode_bit(&state->coder, z) == 0) {
    v = 0;
  } else {
    int sign = lj92_arith_decode_bit(&state->coder, z + 1);
    int sz = 0;
    u8* sign_mag = z + (sign ? 3 : 2);
    if (lj92_arith_decode_bit(&state->coder, sign_mag)) {
      u8(*mag)[15] = (db > (1 << self->arith_dc_u[table]) ||
                      -db > (1 << self->arith_dc_u[table]))
                         ? state->table[table].mag_high
                         : state->table[table].mag_low;
      int i = 0;
      int m = 2;
      while (lj92_arith_decode_bit(&state->coder, &mag[0][i])) {
        m <<= 1;
        if (++i >= 15) return TDNG_LJ92_ERROR_CORRUPT;
      }
      m >>= 1;
      sz = m;
      while ((m >>= 1) != 0) {
        if (lj92_arith_decode_bit(&state->coder, &mag[1][i])) sz |= m;
      }
    }
    v = sign ? -sz - 1 : sz + 1;
  }
  state->left[c] = v;
  state->above[c][x] = (int16_t)v;
  *diff_out = v;
  return TDNG_LJ92_ERROR_NONE;
}

static int lj92_parse_scan_desc(ljp* self, const u8* segment, int length,
                                lj92_scan_desc* scan) {
  int seen[LJ92_MAX_COMPONENTS] = {0};
  int ns;
  if (length < 8 || be16(segment) != length) return TDNG_LJ92_ERROR_CORRUPT;
  ns = segment[2];
  if (ns < 1 || ns > 4 || length != 6 + 2 * ns) {
    return TDNG_LJ92_ERROR_CORRUPT;
  }
  memset(scan, 0, sizeof(*scan));
  scan->count = ns;
  for (int s = 0; s < ns; s++) {
    int frame_c = -1;
    int td = segment[4 + 2 * s] >> 4;
    if ((segment[4 + 2 * s] & 15) != 0 || td > 3 ||
        (self->sof_marker == 0xC3 && !self->huff_defined[td])) {
      return TDNG_LJ92_ERROR_CORRUPT;
    }
    for (int c = 0; c < self->components; c++) {
      if (self->component_id[c] == segment[3 + 2 * s]) frame_c = c;
    }
    if (frame_c < 0 || seen[frame_c]) return TDNG_LJ92_ERROR_CORRUPT;
    seen[frame_c] = 1;
    scan->component[s] = frame_c;
    scan->table[s] = td;
  }
  scan->predictor = segment[3 + 2 * ns];
  scan->point_transform = segment[5 + 2 * ns] & 15;
  if (scan->predictor < 1 || scan->predictor > 7 || segment[4 + 2 * ns] != 0 ||
      (segment[5 + 2 * ns] >> 4) != 0 || scan->point_transform >= self->bits) {
    return TDNG_LJ92_ERROR_CORRUPT;
  }
  if (ns > 1) {
    int sampling_sum = 0;
    for (int s = 0; s < ns; s++) {
      int c = scan->component[s];
      sampling_sum += self->component_h[c] * self->component_v[c];
    }
    if (sampling_sum > 10) return TDNG_LJ92_ERROR_CORRUPT;
  }
  return TDNG_LJ92_ERROR_NONE;
}

static int lj92_plane_store_diff(ljp* self, const lj92_scan_desc* scan,
                                 tdng_lj92_plane* plane, uint32_t x, uint32_t y,
                                 int reset, int diff) {
  int pt = scan->point_transform;
  int reduced_bits = self->bits - pt;
  uint32_t mask = reduced_bits == 16 ? 0xffffu : (1u << reduced_bits) - 1u;
  size_t at = (size_t)y * plane->row_stride_samples +
              (size_t)x * plane->pixel_stride_samples;
  int px;
  uint32_t raw;
  if ((reset && x == 0) || (x == 0 && y == 0)) {
    px = 1 << (reduced_bits - 1);
  } else if (reset || y == 0) {
    px = plane->data[at - plane->pixel_stride_samples] >> pt;
  } else if (x == 0) {
    px = plane->data[at - plane->row_stride_samples] >> pt;
  } else {
    int left = plane->data[at - plane->pixel_stride_samples] >> pt;
    int above = plane->data[at - plane->row_stride_samples] >> pt;
    int abovel = plane->data[at - plane->row_stride_samples -
                             plane->pixel_stride_samples] >>
                 pt;
    switch (scan->predictor) {
      case 1:
        px = left;
        break;
      case 2:
        px = above;
        break;
      case 3:
        px = abovel;
        break;
      case 4:
        px = left + above - abovel;
        break;
      case 5:
        px = left + td_floor_half(above - abovel);
        break;
      case 6:
        px = above + td_floor_half(left - abovel);
        break;
      default:
        px = (left + above) / 2;
        break;
    }
  }
  raw = ((uint32_t)(px + diff)) & mask;
  plane->data[at] = (uint16_t)(raw << pt);
  return TDNG_LJ92_ERROR_NONE;
}

static int lj92_plane_sample(ljp* self, const lj92_scan_desc* scan,
                             int scan_component, tdng_lj92_plane* plane,
                             uint32_t x, uint32_t y, int reset, bitio_t* bio) {
  int diff;
  bitio_refill(bio);
  diff = bitio_decode_diff(bio, self->hufflut[scan->table[scan_component]],
                           self->huffbits[scan->table[scan_component]]);
  return lj92_plane_store_diff(self, scan, plane, x, y, reset, diff);
}

static int lj92_decode_scan_memory(ljp* self, const lj92_scan_desc* scan,
                                   tdng_lj92_plane* planes, uint32_t dri,
                                   size_t entropy_start, size_t* next_pos,
                                   int* next_marker) {
  tdng_lj92_frame_info info;
  uint32_t hmax = 0, vmax = 0, mcu_cols, mcu_rows;
  uint64_t mcu_total, mcu_done = 0;
  int expected_rst = 0;
  int ret = tdng_lj92_get_frame_info(self, &info);
  if (ret != TDNG_LJ92_ERROR_NONE) return ret;
  for (int c = 0; c < self->components; c++) {
    if (self->component_h[c] > hmax) hmax = self->component_h[c];
    if (self->component_v[c] > vmax) vmax = self->component_v[c];
  }
  if (scan->count == 1) {
    int c = scan->component[0];
    mcu_cols = info.components[c].width;
    mcu_rows = info.components[c].height;
  } else {
    mcu_cols = ((uint32_t)self->x + hmax - 1u) / hmax;
    mcu_rows = ((uint32_t)self->y + vmax - 1u) / vmax;
  }
  mcu_total = (uint64_t)mcu_cols * mcu_rows;
  if (dri && dri % mcu_cols != 0u) return TDNG_LJ92_ERROR_CORRUPT;
  while (mcu_done < mcu_total) {
    size_t after;
    int marker;
    uint64_t chunk_mcus = dri ? dri : mcu_total - mcu_done;
    bitio_t bio;
    if (chunk_mcus > mcu_total - mcu_done) chunk_mcus = mcu_total - mcu_done;
    ret = lj92_mem_destuff_chunk(self, entropy_start, &after, &marker);
    if (ret != TDNG_LJ92_ERROR_NONE) return ret;
    bitio_init(&bio, self->ebuf, self->ebuf + self->ebuf_len);
    for (uint64_t unit = 0; unit < chunk_mcus; unit++) {
      uint64_t mcu = mcu_done + unit;
      int reset_row = unit < mcu_cols;
      if (scan->count == 1) {
        int c = scan->component[0];
        uint32_t x = (uint32_t)(mcu % mcu_cols);
        uint32_t y = (uint32_t)(mcu / mcu_cols);
        ret =
            lj92_plane_sample(self, scan, 0, &planes[c], x, y, reset_row, &bio);
        if (ret != TDNG_LJ92_ERROR_NONE) return ret;
      } else {
        uint32_t mx = (uint32_t)(mcu % mcu_cols);
        uint32_t my = (uint32_t)(mcu / mcu_cols);
        for (int s = 0; s < scan->count; s++) {
          int c = scan->component[s];
          for (uint32_t vy = 0; vy < self->component_v[c]; vy++) {
            for (uint32_t hx = 0; hx < self->component_h[c]; hx++) {
              uint32_t x = mx * self->component_h[c] + hx;
              uint32_t y = my * self->component_v[c] + vy;
              if (x >= planes[c].width || y >= planes[c].height) continue;
              ret = lj92_plane_sample(self, scan, s, &planes[c], x, y,
                                      reset_row && vy == 0, &bio);
              if (ret != TDNG_LJ92_ERROR_NONE) return ret;
            }
          }
        }
      }
    }
    if (bitio_check_overrun(&bio)) return TDNG_LJ92_ERROR_CORRUPT;
    mcu_done += chunk_mcus;
    if (mcu_done < mcu_total) {
      if (!dri || marker != 0xd0 + expected_rst) {
        return TDNG_LJ92_ERROR_CORRUPT;
      }
      expected_rst = (expected_rst + 1) & 7;
      entropy_start = after;
    } else {
      *next_pos = after;
      *next_marker = marker;
    }
  }
  return TDNG_LJ92_ERROR_NONE;
}

static int lj92_decode_scan_arithmetic(ljp* self, const lj92_scan_desc* scan,
                                       tdng_lj92_plane* planes, uint32_t dri,
                                       size_t entropy_start, size_t* next_pos,
                                       int* next_marker) {
  tdng_lj92_frame_info info;
  uint32_t hmax = 0, vmax = 0, mcu_cols, mcu_rows;
  uint64_t mcu_total, mcu_done = 0;
  size_t history_count = 0, history_offset = 0;
  int16_t* history;
  lj92_arith_scan_state state;
  int expected_rst = 0;
  int ret = tdng_lj92_get_frame_info(self, &info);
  if (ret != TDNG_LJ92_ERROR_NONE) return ret;
  for (int c = 0; c < self->components; c++) {
    if (self->component_h[c] > hmax) hmax = self->component_h[c];
    if (self->component_v[c] > vmax) vmax = self->component_v[c];
  }
  if (scan->count == 1) {
    int c = scan->component[0];
    mcu_cols = info.components[c].width;
    mcu_rows = info.components[c].height;
  } else {
    mcu_cols = ((uint32_t)self->x + hmax - 1u) / hmax;
    mcu_rows = ((uint32_t)self->y + vmax - 1u) / vmax;
  }
  mcu_total = (uint64_t)mcu_cols * mcu_rows;
  if (dri && dri % mcu_cols != 0u) return TDNG_LJ92_ERROR_CORRUPT;
  for (int s = 0; s < scan->count; s++) {
    int c = scan->component[s];
    if (history_count > SIZE_MAX - planes[c].width) {
      return TDNG_LJ92_ERROR_LIMIT;
    }
    history_count += planes[c].width;
  }
  if (history_count > SIZE_MAX / sizeof(*history)) {
    return TDNG_LJ92_ERROR_LIMIT;
  }
  history =
      (int16_t*)lj92_alloc(&self->allocator, history_count * sizeof(*history));
  if (!history) return TDNG_LJ92_ERROR_NO_MEMORY;
  memset(&state, 0, sizeof(state));
  for (int s = 0; s < scan->count; s++) {
    int c = scan->component[s];
    state.above[c] = history + history_offset;
    history_offset += planes[c].width;
  }
#define LJ92_ARITH_DECODE_FAIL(code)      \
  do {                                    \
    lj92_free(&self->allocator, history); \
    return (code);                        \
  } while (0)
  while (mcu_done < mcu_total) {
    size_t after;
    int marker;
    uint64_t chunk_mcus = dri ? dri : mcu_total - mcu_done;
    if (chunk_mcus > mcu_total - mcu_done) chunk_mcus = mcu_total - mcu_done;
    ret = lj92_mem_destuff_chunk(self, entropy_start, &after, &marker);
    if (ret != TDNG_LJ92_ERROR_NONE) LJ92_ARITH_DECODE_FAIL(ret);
    memset(state.table, 0, sizeof(state.table));
    memset(state.left, 0, sizeof(state.left));
    memset(history, 0, history_count * sizeof(*history));
    memset(&state.coder, 0, sizeof(state.coder));
    state.coder.p = self->ebuf;
    state.coder.end = self->ebuf + self->ebuf_len;
    state.coder.ct = -16;
    for (uint64_t unit = 0; unit < chunk_mcus; unit++) {
      uint64_t mcu = mcu_done + unit;
      int reset_row = unit < mcu_cols;
      if (scan->count == 1) {
        int c = scan->component[0];
        int diff;
        ret = lj92_arith_decode_diff(self, scan, 0, (uint32_t)(mcu % mcu_cols),
                                     &state, &diff);
        if (ret != TDNG_LJ92_ERROR_NONE) LJ92_ARITH_DECODE_FAIL(ret);
        ret = lj92_plane_store_diff(
            self, scan, &planes[c], (uint32_t)(mcu % mcu_cols),
            (uint32_t)(mcu / mcu_cols), reset_row, diff);
        if (ret != TDNG_LJ92_ERROR_NONE) LJ92_ARITH_DECODE_FAIL(ret);
      } else {
        uint32_t mx = (uint32_t)(mcu % mcu_cols);
        uint32_t my = (uint32_t)(mcu / mcu_cols);
        for (int s = 0; s < scan->count; s++) {
          int c = scan->component[s];
          for (uint32_t vy = 0; vy < self->component_v[c]; vy++) {
            for (uint32_t hx = 0; hx < self->component_h[c]; hx++) {
              uint32_t x = mx * self->component_h[c] + hx;
              uint32_t y = my * self->component_v[c] + vy;
              int diff;
              if (x >= planes[c].width || y >= planes[c].height) continue;
              ret = lj92_arith_decode_diff(self, scan, s, x, &state, &diff);
              if (ret != TDNG_LJ92_ERROR_NONE) LJ92_ARITH_DECODE_FAIL(ret);
              ret = lj92_plane_store_diff(self, scan, &planes[c], x, y,
                                          reset_row && vy == 0, diff);
              if (ret != TDNG_LJ92_ERROR_NONE) LJ92_ARITH_DECODE_FAIL(ret);
            }
          }
        }
      }
    }
    /* Arithmetic termination is allowed to discard trailing zero bytes.  Two
     * virtual initialization bytes plus a small renormalization tail suffice;
     * a larger demand indicates a truncated or non-terminating payload. */
    if (state.coder.synthetic_bytes > 8) {
      LJ92_ARITH_DECODE_FAIL(TDNG_LJ92_ERROR_CORRUPT);
    }
    mcu_done += chunk_mcus;
    if (mcu_done < mcu_total) {
      if (!dri || marker != 0xd0 + expected_rst) {
        LJ92_ARITH_DECODE_FAIL(TDNG_LJ92_ERROR_CORRUPT);
      }
      expected_rst = (expected_rst + 1) & 7;
      entropy_start = after;
    } else {
      *next_pos = after;
      *next_marker = marker;
    }
  }
  lj92_free(&self->allocator, history);
#undef LJ92_ARITH_DECODE_FAIL
  return TDNG_LJ92_ERROR_NONE;
}

int tdng_lj92_decode_planes(tdng_lj92 lj, tdng_lj92_plane* planes,
                            size_t plane_count) {
  ljp* self = lj;
  tdng_lj92_frame_info info;
  size_t pos = 0;
  int marker;
  int pending = -1;
  uint32_t dri = 0;
  uint32_t covered = 0;
  if (!self || !planes || self->is_streaming) {
    return self && self->is_streaming ? TDNG_LJ92_ERROR_UNSUPPORTED
                                      : TDNG_LJ92_ERROR_INVALID_ARGUMENT;
  }
  if (tdng_lj92_get_frame_info(self, &info) != TDNG_LJ92_ERROR_NONE ||
      plane_count != info.component_count) {
    return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
  }
  for (size_t c = 0; c < plane_count; c++) {
    size_t min_row, last;
    tdng_lj92_plane* p = &planes[c];
    if (!p->data || p->width != info.components[c].width ||
        p->height != info.components[c].height ||
        p->pixel_stride_samples == 0 ||
        (p->width > 1u &&
         p->pixel_stride_samples > (SIZE_MAX - 1u) / (size_t)(p->width - 1u))) {
      return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
    }
    min_row = (size_t)(p->width - 1u) * p->pixel_stride_samples + 1u;
    if (p->row_stride_samples < min_row ||
        (p->height > 1u &&
         p->row_stride_samples > SIZE_MAX / (size_t)(p->height - 1u))) {
      return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
    }
    last = (size_t)(p->height - 1u) * p->row_stride_samples;
    if (last > SIZE_MAX - (min_row - 1u)) {
      return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
    }
    last += min_row - 1u;
    if (last >= p->capacity_samples) return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
  }
  for (int t = 0; t < 4; t++) {
    lj92_free(&self->allocator, self->hufflut[t]);
    self->hufflut[t] = NULL;
    self->huffbits[t] = 0;
    self->huff_defined[t] = 0;
  }
  self->num_huff_idx = 0;
  for (int t = 0; t < 4; t++) {
    self->arith_dc_l[t] = 0;
    self->arith_dc_u[t] = 1;
  }
  marker = lj92_mem_next_marker(self, &pos);
  if (marker != 0xd8) return TDNG_LJ92_ERROR_CORRUPT;
  for (;;) {
    if (pending >= 0) {
      marker = pending;
      pending = -1;
    } else {
      marker = lj92_mem_next_marker(self, &pos);
    }
    if (marker < 0) return TDNG_LJ92_ERROR_CORRUPT;
    if (marker == 0xd9) {
      uint32_t all =
          self->components == 32 ? UINT32_MAX : ((1u << self->components) - 1u);
      return covered == all ? TDNG_LJ92_ERROR_NONE : TDNG_LJ92_ERROR_CORRUPT;
    }
    if (marker == 0xd8) continue;
    if (marker == 0x01) continue; /* standalone TEM has no length field */
    if (pos + 2u > (size_t)self->datalen) return TDNG_LJ92_ERROR_CORRUPT;
    {
      int length = be16(self->data + pos);
      const u8* segment = self->data + pos;
      if (length < 2 || (size_t)length > (size_t)self->datalen - pos) {
        return TDNG_LJ92_ERROR_CORRUPT;
      }
      if (marker == 0xc4) {
        int ret = lj92_parse_dht_segment(self, segment, length);
        if (ret != TDNG_LJ92_ERROR_NONE) return ret;
      } else if (marker == 0xcc) {
        if (length < 4 || (length & 1) != 0) {
          return TDNG_LJ92_ERROR_CORRUPT;
        }
        for (int p = 2; p < length; p += 2) {
          int tc = segment[p] >> 4;
          int tb = segment[p] & 15;
          int cs = segment[p + 1];
          if (tc != 0 || tb > 3 || (cs & 15) > (cs >> 4)) {
            return TDNG_LJ92_ERROR_CORRUPT;
          }
          self->arith_dc_l[tb] = (u8)(cs & 15);
          self->arith_dc_u[tb] = (u8)(cs >> 4);
        }
      } else if (marker == 0xdd) {
        if (length != 4) return TDNG_LJ92_ERROR_CORRUPT;
        dri = (uint32_t)be16(segment + 2);
      } else if (marker == 0xda) {
        lj92_scan_desc scan;
        int ret = lj92_parse_scan_desc(self, segment, length, &scan);
        if (ret != TDNG_LJ92_ERROR_NONE) return ret;
        for (int s = 0; s < scan.count; s++) {
          uint32_t bit = 1u << scan.component[s];
          if (covered & bit) return TDNG_LJ92_ERROR_CORRUPT;
          covered |= bit;
        }
        if (self->sof_marker == 0xCB) {
          ret = lj92_decode_scan_arithmetic(
              self, &scan, planes, dri, pos + (size_t)length, &pos, &pending);
        } else {
          ret = lj92_decode_scan_memory(self, &scan, planes, dri,
                                        pos + (size_t)length, &pos, &pending);
        }
        if (ret != TDNG_LJ92_ERROR_NONE) return ret;
        continue;
      } else if (marker == 0xdc) {
        if (length != 4 || be16(segment + 2) != self->y) {
          return TDNG_LJ92_ERROR_CORRUPT;
        }
      } else if (marker >= 0xd0 && marker <= 0xd7) {
        return TDNG_LJ92_ERROR_CORRUPT;
      }
      pos += (size_t)length;
    }
  }
}

void tdng_lj92_close(tdng_lj92 lj) {
  ljp* self = lj;
  if (self == NULL) return;
  if (self->stream_user) {
    tdng_lj92_stream* sctx = (tdng_lj92_stream*)self->stream_user;
    lj92_free(&self->allocator, sctx->buf);
    lj92_free(&self->allocator, sctx);
    self->stream_user = NULL;
  }
  free_memory(self);
  lj92_free(&self->allocator, self);
}

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------
//
// Lossless JPEG (ITU T.81, Annex H) encoder. Produces a single-DHT stream
// shared across all components (Td=0 for every component in SOS). The bit
// stream is emitted MSB-first, with 0xFF 0x00 byte stuffing.

#define LJ92_MAX_SSSS 17  // SSSS values 0..16 plus spec sentinel.

typedef struct _lje {
  tdng_lj92_allocator allocator;
  int width;
  int height;
  int bitdepth;
  int components;
  int predictor;
  int simd_level; /* 0 scalar, 1 SSE2, 2 AVX2, 3 ARM64 NEON */
  int readLength;
  int skipLength;
  const uint16_t* delinearize;
  int delinearizeLength;

  // Streaming output: the encoded stream goes to the sink in staged chunks
  // instead of a full growable buffer. 64KB stage: one sink call per ~64KB
  // of output instead of per 4KB (fewer stdio round-trips on file sinks).
  void* sink_user;
  tdng_lj92_write_fn sink_write;
  uint8_t obuf[64 * 1024];
  int obuf_len;
  int sink_failed;

  // Frequency of SSSS symbols (0..16) across all components.
  // int64: a 65535x65535x4 constant image drives hist[0] past INT32_MAX.
  int64_t hist[LJ92_MAX_SSSS];
  // Canonical Huffman table derived from hist.
  int bits[17];         // bits[L] = number of codes of length L (1..16).
  uint8_t huffval[17];  // symbols ordered by code length.
  int huffval_count;
  uint16_t huffenc[LJ92_MAX_SSSS];  // code for symbol s.
  uint8_t huffbits[LJ92_MAX_SSSS];  // code length for symbol s.
  // Fused emit entry: (code << 16) | codelen, indexed by ssss. One load
  // feeds enc_put_bits directly in the entropy inner loop.
  uint32_t emitlut[LJ92_MAX_SSSS];

  // Bit-output accumulator: bits packed MSB-first. `bitbuf` holds the
  // pending bits left-aligned; the top `nbits` bits are live. The writer
  // drains 4 bytes at a time once >=32 bits are pending, taking a bulk-copy
  // fast path whenever none of them is 0xFF (no stuffing needed).
  uint64_t bitbuf;
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
  int16_t* diffrow;  // reusable vectorized residual row
  uint8_t* ssssrow;  // reusable vectorized category row
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
    case 5:
      return left + td_floor_half(above - abovel);
    case 6:
      return above + td_floor_half(left - abovel);
    case 7: default: return (left + above) >> 1;
  }
}

static TDNG_ALWAYS_INLINE int enc_ssss(int diff) {
  // DNG allows diff in (-32768, 32767]; the "modulo 65536" rule is folded
  // into diff by the caller so diff here fits in int range.
  int a = diff < 0 ? -diff : diff;
  return a == 0 ? 0 : (32 - clz32((unsigned int)a));
}

static void enc_make_diff_row(lje* self, const u16* cur, const u16* prev,
                              int row) {
#if defined(TINY_DNG_LJPEG92_V2_USE_AVX2) || defined(TDNG_LJ92_RUNTIME_SIMD)
  if (self->simd_level == 2) {
    tdng_enc_diff_row_avx2(cur, prev, self->width, self->components,
                           self->predictor, 1 << (self->bitdepth - 1), row,
                           self->diffrow);
    return;
  }
#endif
#if defined(TINY_DNG_LJPEG92_V2_USE_SSE2) || defined(TDNG_LJ92_RUNTIME_SIMD)
  if (self->simd_level == 1) {
    tdng_enc_diff_row_sse2(cur, prev, self->width, self->components,
                           self->predictor, 1 << (self->bitdepth - 1), row,
                           self->diffrow);
    return;
  }
#endif
#if defined(TDNG_LJ92_NEON)
  if (self->simd_level == 3) {
    tdng_enc_diff_row_neon(cur, prev, self->width, self->components,
                           self->predictor, 1 << (self->bitdepth - 1), row,
                           self->diffrow);
    return;
  }
#endif
  tdng_enc_diff_row_scalar(cur, prev, self->width, self->components,
                           self->predictor, 1 << (self->bitdepth - 1), row,
                           self->diffrow);
}

static void enc_make_ssss_row(lje* self, int count) {
#if defined(TINY_DNG_LJPEG92_V2_USE_SSE2) || defined(TDNG_LJ92_RUNTIME_SIMD)
  if (self->simd_level == 1 || self->simd_level == 2) {
    tdng_ssss_row_sse2(self->diffrow, count, self->ssssrow);
    return;
  }
#endif
#if defined(TDNG_LJ92_NEON)
  if (self->simd_level == 3) {
    tdng_ssss_row_neon(self->diffrow, count, self->ssssrow);
    return;
  }
#endif
  for (int i = 0; i < count; i++) {
    self->ssssrow[i] = (u8)enc_ssss((int)self->diffrow[i]);
  }
}

static int enc_frequency_scan_vector(lje* self) {
  const size_t stride = (size_t)self->readLength + (size_t)self->skipLength;
  const int count = self->width * self->components;
  const int maxval = 1 << self->bitdepth;
  for (int row = 0; row < self->height; row++) {
    const u16* cur = self->image + (size_t)row * stride;
    const u16* prev = row ? cur - stride : NULL;
    for (int i = 0; i < count; i++) {
      if (cur[i] >= maxval) return TDNG_LJ92_ERROR_TOO_WIDE;
    }
    enc_make_diff_row(self, cur, prev, row);
    enc_make_ssss_row(self, count);
    for (int i = 0; i < count; i++) self->hist[self->ssssrow[i]]++;
  }
  return TDNG_LJ92_ERROR_NONE;
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

// True if the 32-bit word contains a 0xFF byte (needs stuffing).
static TDNG_ALWAYS_INLINE int enc_word_has_ff(uint32_t w) {
  // Classic zero-byte predicate applied to ~w: x has a zero byte
  // <=> w has an 0xFF byte.
  uint32_t x = ~w;
  return ((x - 0x01010101u) & ~x & 0x80808080u) != 0u;
}

// Drain exactly 32 pending bits (the accumulator is left-aligned, so the
// top half is always the oldest data). Fast path: no 0xFF byte in the word
// -> append 4 bytes to the staging buffer with one capacity check.
static void enc_drain32(lje* self) {
  uint32_t w = (uint32_t)(self->bitbuf >> 32);
  self->bitbuf <<= 32;
  self->nbits -= 32;
  if (!self->sink_failed && !enc_word_has_ff(w)) {
    if (self->obuf_len > (int)sizeof(self->obuf) - 4) enc_stage_flush(self);
    if (!self->sink_failed) {
      self->obuf[self->obuf_len + 0] = (uint8_t)(w >> 24);
      self->obuf[self->obuf_len + 1] = (uint8_t)(w >> 16);
      self->obuf[self->obuf_len + 2] = (uint8_t)(w >> 8);
      self->obuf[self->obuf_len + 3] = (uint8_t)(w);
      self->obuf_len += 4;
    }
    return;
  }
  enc_put_stuffed(self, (uint8_t)(w >> 24));
  enc_put_stuffed(self, (uint8_t)(w >> 16));
  enc_put_stuffed(self, (uint8_t)(w >> 8));
  enc_put_stuffed(self, (uint8_t)w);
}

// Append `nbits` (0..32) bits without masking: callers guarantee v < 2^nbits
// (canonical Huffman codes fit their length; JPEG extend-form residuals fit
// ssss by construction). A fused code+residual emission is at most 32 bits;
// entries are <= 31 because a drain lands there, so n+add <= 63 keeps every
// shift inside the accumulator. Measured faster than a deferred-drain
// two-put scheme (the single update beats the saved drain checks).
static TDNG_ALWAYS_INLINE void enc_put_bits_raw(lje* self, uint32_t v,
                                                int nbits) {
  self->bitbuf |= (uint64_t)v << (64 - self->nbits - nbits);
  self->nbits += nbits;
  if (self->nbits >= 32) enc_drain32(self);
}

static TDNG_ALWAYS_INLINE int enc_emit_diff_ssss(lje* self, int diff,
                                                 int ssss) {
  /* The histogram pass saw the exact same samples, so any symbol reachable
   * here had hist[ssss] > 0 and therefore a nonzero code (validated once in
   * encode_begin). No per-sample validity check on this hot path. */
  uint32_t e = self->emitlut[ssss];
  if (ssss == 0) {
    enc_put_bits_raw(self, e >> 16, (int)(e & 0xFFFFu));
  } else {
    /* Fuse Huffman code + residual into ONE accumulator update:
       width (codelen + ssss) <= 32. */
    uint32_t bits_val;
    int len = (int)(e & 0xFFFFu);
    if (diff < 0) bits_val = (uint32_t)(diff + (1 << ssss) - 1);
    else          bits_val = (uint32_t)diff;
    enc_put_bits_raw(self, (e >> 16 << ssss) | bits_val, len + ssss);
  }
  return TDNG_LJ92_ERROR_NONE;
}

static TDNG_ALWAYS_INLINE int enc_emit_diff(lje* self, int diff) {
  return enc_emit_diff_ssss(self, diff, enc_ssss(diff));
}

// Pad the remaining bits with 1s per JPEG spec and flush.
static void enc_flush_bits(lje* self) {
  while (self->nbits >= 8) {
    uint8_t b = (uint8_t)(self->bitbuf >> 56);
    enc_put_stuffed(self, b);
    self->bitbuf <<= 8;
    self->nbits -= 8;
  }
  if (self->nbits > 0) {
    uint8_t pad = (uint8_t)((1u << (8 - self->nbits)) - 1u);
    uint8_t b = (uint8_t)((self->bitbuf >> 56) | pad);
    enc_put_stuffed(self, b);
    self->bitbuf = 0;
    self->nbits = 0;
  }
}

// Fast histogram scan for the predictor used by the writer. The first NC
// samples in a row use the initial/above predictor; all remaining samples
// use the immediately preceding reconstructed sample. Splitting those cases
// removes row/column and predictor branches from the hot loop.
//
// Core of the pred-1 histogram pass. SKIP0 is compile-time: when skipLength
// == 0 (the tiled/stripped writer case) the per-sample skip bookkeeping is a
// provable no-op and compiles away.
static TDNG_ALWAYS_INLINE int enc_freq_scan_pred1_core(lje* self,
                                                       const int SKIP0) {
  const int H = self->height, NC = self->components;
  const int initpx = 1 << (self->bitdepth - 1);
  const int maxval = 1 << self->bitdepth;
  const size_t row_slots = (size_t)self->width * (size_t)NC;
  uint16_t* thisrow = self->thisrow;
  uint16_t* lastrow = self->lastrow;
  const uint16_t* pix = self->image;
  int scan = self->readLength;
  int row;

  /* Streaming specialization for the common writer configuration
     (skip-free mono, no delinearization): left-neighbor diffs come straight
     off the interleaved input row, so the histogram pass becomes a linear
     scan with no row-cache dependency. Diff arithmetic (u16 wraparound) is
     bit-identical to the generic path below. */
  if (SKIP0 && NC == 1 && self->delinearize == NULL) {
    const int W = self->width;
    const u16* base = self->image;
    u16 blk[16];
    for (row = 0; row < H; row++) {
      const u16* rp = base + (size_t)row * (size_t)W;
      size_t i = 1;
      {
        u16 p0 = rp[0];
        int prev = initpx;
        if (row > 0) prev = rp[-(ptrdiff_t)W];
        int diff;
        if (p0 >= maxval) return TDNG_LJ92_ERROR_TOO_WIDE;
        diff = td_wrap_diff_u16((int)p0, prev);
        self->hist[enc_ssss(diff)]++;
      }
      for (; i + 16 <= (size_t)W; i += 16) {
        int k;
        tdng_diff16(blk, rp, i);
        for (k = 0; k < 16; k++) {
          if (rp[i + (size_t)k] >= maxval) {
            return TDNG_LJ92_ERROR_TOO_WIDE;
          }
          self->hist[enc_ssss(td_wrap_diff_u16((int)blk[k], 0))]++;
        }
      }
      for (; i < (size_t)W; i++) {
        u16 p = rp[i];
        int diff;
        if (p >= maxval) return TDNG_LJ92_ERROR_TOO_WIDE;
        diff = td_wrap_diff_u16((int)p, (int)rp[i - 1]);
        self->hist[enc_ssss(diff)]++;
      }
    }
    return TDNG_LJ92_ERROR_NONE;
  }

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
        int diff = td_wrap_diff_u16((int)p, row > 0 ? (int)lastrow[i] : initpx);
        self->hist[enc_ssss(diff)]++;
      }
      thisrow[i] = p;
      if (!SKIP0 && --scan == 0) {
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
        int diff = td_wrap_diff_u16((int)p, (int)thisrow[i - (size_t)NC]);
        self->hist[enc_ssss(diff)]++;
      }
      thisrow[i] = p;
      if (!SKIP0 && --scan == 0) {
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

static int enc_frequency_scan_pred1(lje* self) {
  if (self->skipLength == 0) return enc_freq_scan_pred1_core(self, 1);
  return enc_freq_scan_pred1_core(self, 0);
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

  if (!self->delinearize && self->simd_level > 0) {
    return enc_frequency_scan_vector(self);
  }

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
        int diff = td_wrap_diff_u16((int)p, Px);
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
  uint64_t freq[18];
  int codesize[18];
  int others[18];
  for (int i = 0; i < 18; i++) {
    freq[i] = 0;
    codesize[i] = 0;
    others[i] = -1;
  }
  for (int s = 0; s < LJ92_MAX_SSSS; s++) {
    // Scale real frequencies by two so the reserved symbol can have a
    // strictly lower positive weight without floating-point ordering.
    if (self->hist[s] > 0) freq[s] = (uint64_t)self->hist[s] * 2u;
  }
  freq[17] = 1u;  // reserved sentinel

  for (;;) {
    // Find v1 = lowest-freq non-zero entry, v2 = second lowest.
    int v1 = -1, v2 = -1;
    uint64_t f1 = UINT64_MAX, f2 = UINT64_MAX;
    for (int i = 0; i < 18; i++) {
      if (freq[i] > 0 && freq[i] <= f1) {
        f2 = f1;
        v2 = v1;
        f1 = freq[i];
        v1 = i;
      } else if (freq[i] > 0 && freq[i] <= f2) {
        f2 = freq[i];
        v2 = i;
      }
    }
    if (v2 < 0) break;  // only one symbol left
    freq[v1] += freq[v2];
    freq[v2] = 0;
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

  // Build huffval[] in the original nondecreasing code-size order. Annex
  // K.3 changes only the count assigned to each final length; symbols whose
  // original tree depth exceeded 16 must remain in this ordered list.
  int k = 0;
  for (int L = 1; L <= 32; L++) {
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
  // Fused entries for the entropy inner loop.
  for (int s = 0; s < LJ92_MAX_SSSS; s++) {
    self->emitlut[s] =
        ((uint32_t)self->huffenc[s] << 16) | (uint32_t)self->huffbits[s];
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
// SKIP0 compiles away the per-sample skip bookkeeping when skipLength == 0.
static TDNG_ALWAYS_INLINE int enc_write_rows_pred1_core(lje* self, int row0,
                                                        int row_count,
                                                        const int SKIP0) {
  const int NC = self->components;
  const int initpx = 1 << (self->bitdepth - 1);
  const size_t row_slots = (size_t)self->width * (size_t)NC;
  uint16_t* thisrow = self->thisrow;
  uint16_t* lastrow = self->lastrow;
  int row;

  /* Streaming twin of the scan-pass specialization: mono, skip-free, no
     delinearization. Diffs come off the raw input rows (bit-identical to
     the generic path), so the row cache is untouched. */
  if (SKIP0 && NC == 1 && self->delinearize == NULL) {
    const int W = self->width;
    const u16* base = self->image;
    u16 blk[16];
    for (row = row0; row < row0 + row_count; row++) {
      const u16* rp = base + (size_t)row * (size_t)W;
      size_t i = 1;
      {
        u16 p0 = rp[0];
        int prev = initpx;
        if (row > 0) prev = rp[-(ptrdiff_t)W];
        int ret;
        ret = enc_emit_diff(self, td_wrap_diff_u16((int)p0, prev));
        if (ret != TDNG_LJ92_ERROR_NONE) return ret;
      }
      for (; i + 16 <= (size_t)W; i += 16) {
        int k;
        int ret;
        tdng_diff16(blk, rp, i);
        for (k = 0; k < 16; k++) {
          ret = enc_emit_diff(self, td_wrap_diff_u16((int)blk[k], 0));
          if (ret != TDNG_LJ92_ERROR_NONE) return ret;
        }
      }
      for (; i < (size_t)W; i++) {
        int ret =
            enc_emit_diff(self, td_wrap_diff_u16((int)rp[i], (int)rp[i - 1]));
        if (ret != TDNG_LJ92_ERROR_NONE) return ret;
      }
    }
    /* Keep the frame-relative read pointer consistent for callers that
       inspect it; bands are strictly sequential so this is exact. */
    self->pix = base + (size_t)(row0 + row_count) * (size_t)W;
    self->thisrow = thisrow;
    self->lastrow = lastrow;
    if (self->sink_failed) return TDNG_LJ92_ERROR_IO;
    return TDNG_LJ92_ERROR_NONE;
  }

  for (row = row0; row < row0 + row_count; row++) {
    size_t i;
    for (i = 0; i < (size_t)NC; i++) {
      uint16_t p = *self->pix++;
      if (self->delinearize) {
        if (p >= self->delinearizeLength) return TDNG_LJ92_ERROR_TOO_WIDE;
        p = self->delinearize[p];
      }
      {
        int diff = td_wrap_diff_u16((int)p, row > 0 ? (int)lastrow[i] : initpx);
        enc_emit_diff(self, diff);
      }
      thisrow[i] = p;
      if (!SKIP0 && --self->scan_remain == 0) {
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
        int diff = td_wrap_diff_u16((int)p, (int)thisrow[i - (size_t)NC]);
        enc_emit_diff(self, diff);
      }
      thisrow[i] = p;
      if (!SKIP0 && --self->scan_remain == 0) {
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

static int enc_write_rows_pred1(lje* self, int row0, int row_count) {
  if (self->skipLength == 0)
    return enc_write_rows_pred1_core(self, row0, row_count, 1);
  return enc_write_rows_pred1_core(self, row0, row_count, 0);
}

static int enc_write_rows_vector(lje* self, int row0, int row_count) {
  const size_t stride = (size_t)self->readLength + (size_t)self->skipLength;
  const int count = self->width * self->components;
  for (int row = row0; row < row0 + row_count; row++) {
    const u16* cur = self->image + (size_t)row * stride;
    const u16* prev = row ? cur - stride : NULL;
    enc_make_diff_row(self, cur, prev, row);
    enc_make_ssss_row(self, count);
    for (int i = 0; i < count; i++) {
      int ret =
          enc_emit_diff_ssss(self, (int)self->diffrow[i], self->ssssrow[i]);
      if (ret != TDNG_LJ92_ERROR_NONE) return ret;
    }
  }
  self->pix = self->image + (size_t)(row0 + row_count) * stride;
  return self->sink_failed ? TDNG_LJ92_ERROR_IO : TDNG_LJ92_ERROR_NONE;
}

static int enc_write_rows(lje* self, int row0, int row_count) {
  const int W = self->width, NC = self->components;
  const int initpx = 1 << (self->bitdepth - 1);

  if (!self->delinearize && self->simd_level > 0) {
    return enc_write_rows_vector(self, row0, row_count);
  }
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
        int diff = td_wrap_diff_u16((int)p, Px);
        int ssss = enc_ssss(diff);
        thisrow[base + c] = p;

        /* See enc_emit_diff: the scan pass guarantees a code exists. */
        {
          uint32_t e = self->emitlut[ssss];
          if (ssss == 0) {
            enc_put_bits_raw(self, e >> 16, (int)(e & 0xFFFFu));
          } else {
            uint32_t bits_val;
            int len = (int)(e & 0xFFFFu);
            if (diff < 0)
              bits_val = (uint32_t)(diff + (1 << ssss) - 1);
            else
              bits_val = (uint32_t)diff;
            enc_put_bits_raw(self, (e >> 16 << ssss) | bits_val, len + ssss);
          }
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

typedef struct lj92_arith_encoder {
  lje* sink;
  uint32_t c;
  uint32_t a;
  int sc;
  int zc;
  int ct;
  int buffer;
  struct {
    u8 sign_zero[5][5][4];
    u8 mag_low[2][15];
    u8 mag_high[2][15];
  } table[4];
  int16_t* above[LJ92_MAX_COMPONENTS];
  int left[LJ92_MAX_COMPONENTS];
} lj92_arith_encoder;

static void lj92_arith_emit_byte(lj92_arith_encoder* e, int value) {
  enc_put_u8(e->sink, (uint8_t)value);
}

static void lj92_arith_encode_bit(lj92_arith_encoder* e, u8* st, int value) {
  uint32_t packed = lj92_aritab[*st & 0x7f];
  uint32_t qe = packed >> 16;
  uint32_t temp;
  int sv = *st;
  e->a -= qe;
  if (value != (sv >> 7)) {
    if (e->a >= qe) {
      e->c += e->a;
      e->a = qe;
    }
    *st = (u8)((sv & 0x80) ^ (packed & 0xff));
  } else {
    if (e->a >= 0x8000u) return;
    if (e->a < qe) {
      e->c += e->a;
      e->a = qe;
    }
    *st = (u8)((sv & 0x80) ^ ((packed >> 8) & 0xff));
  }
  do {
    e->a <<= 1;
    e->c <<= 1;
    if (--e->ct == 0) {
      temp = e->c >> 19;
      if (temp > 0xffu) {
        if (e->buffer >= 0) {
          while (e->zc > 0) {
            lj92_arith_emit_byte(e, 0);
            e->zc--;
          }
          lj92_arith_emit_byte(e, e->buffer + 1);
          if (e->buffer + 1 == 0xff) lj92_arith_emit_byte(e, 0);
        }
        e->zc += e->sc;
        e->sc = 0;
        e->buffer = (int)(temp & 0xffu);
      } else if (temp == 0xffu) {
        e->sc++;
      } else {
        if (e->buffer == 0)
          e->zc++;
        else if (e->buffer >= 0) {
          while (e->zc > 0) {
            lj92_arith_emit_byte(e, 0);
            e->zc--;
          }
          lj92_arith_emit_byte(e, e->buffer);
        }
        while (e->sc > 0) {
          while (e->zc > 0) {
            lj92_arith_emit_byte(e, 0);
            e->zc--;
          }
          lj92_arith_emit_byte(e, 0xff);
          lj92_arith_emit_byte(e, 0);
          e->sc--;
        }
        e->buffer = (int)(temp & 0xffu);
      }
      e->c &= 0x7ffffu;
      e->ct += 8;
    }
  } while (e->a < 0x8000u);
}

static void lj92_arith_finish(lj92_arith_encoder* e) {
  uint32_t temp = (e->a - 1u + e->c) & 0xffff0000u;
  if (temp < e->c)
    e->c = temp + 0x8000u;
  else
    e->c = temp;
  e->c <<= e->ct;
  if ((e->c & 0xf8000000u) != 0) {
    if (e->buffer >= 0) {
      while (e->zc > 0) {
        lj92_arith_emit_byte(e, 0);
        e->zc--;
      }
      lj92_arith_emit_byte(e, e->buffer + 1);
      if (e->buffer + 1 == 0xff) lj92_arith_emit_byte(e, 0);
    }
    e->zc += e->sc;
    e->sc = 0;
  } else {
    if (e->buffer == 0)
      e->zc++;
    else if (e->buffer >= 0) {
      while (e->zc > 0) {
        lj92_arith_emit_byte(e, 0);
        e->zc--;
      }
      lj92_arith_emit_byte(e, e->buffer);
    }
    while (e->sc > 0) {
      while (e->zc > 0) {
        lj92_arith_emit_byte(e, 0);
        e->zc--;
      }
      lj92_arith_emit_byte(e, 0xff);
      lj92_arith_emit_byte(e, 0);
      e->sc--;
    }
  }
  if ((e->c & 0x07fff800u) != 0) {
    int value;
    while (e->zc > 0) {
      lj92_arith_emit_byte(e, 0);
      e->zc--;
    }
    value = (int)((e->c >> 19) & 0xffu);
    lj92_arith_emit_byte(e, value);
    if (value == 0xff) lj92_arith_emit_byte(e, 0);
    if ((e->c & 0x0007f800u) != 0) {
      value = (int)((e->c >> 11) & 0xffu);
      lj92_arith_emit_byte(e, value);
      if (value == 0xff) lj92_arith_emit_byte(e, 0);
    }
  }
}

static void lj92_arith_encoder_reset(lj92_arith_encoder* e, lje* sink) {
  memset(e->table, 0, sizeof(e->table));
  memset(e->left, 0, sizeof(e->left));
  e->sink = sink;
  e->c = 0;
  e->a = 0x10000u;
  e->sc = 0;
  e->zc = 0;
  e->ct = 11;
  e->buffer = -1;
}

static int lj92_arith_encode_diff(const tdng_lj92_scan_plan* scan,
                                  int scan_component, uint32_t x, int diff,
                                  lj92_arith_encoder* e) {
  int c = scan->component_index[scan_component];
  int da = x == 0 ? 0 : e->left[c];
  int db = e->above[c][x];
  int ca = lj92_arith_classify(da, 0, 1) + 2;
  int cb = lj92_arith_classify(db, 0, 1) + 2;
  u8* z = e->table[0].sign_zero[ca][cb];
  if (diff == 0) {
    lj92_arith_encode_bit(e, z, 0);
  } else {
    int sign = diff < 0;
    int sz = sign ? -(diff + 1) : diff - 1;
    lj92_arith_encode_bit(e, z, 1);
    lj92_arith_encode_bit(e, z + 1, sign);
    if (sz >= 1) {
      u8(*mag)[15] =
          (db > 2 || -db > 2) ? e->table[0].mag_high : e->table[0].mag_low;
      int i = 0;
      int m = 2;
      lj92_arith_encode_bit(e, z + (sign ? 3 : 2), 1);
      while (sz >= m) {
        if (i >= 15 || m > 0x8000) return TDNG_LJ92_ERROR_UNSUPPORTED;
        lj92_arith_encode_bit(e, &mag[0][i], 1);
        m <<= 1;
        i++;
      }
      if (i >= 15) return TDNG_LJ92_ERROR_UNSUPPORTED;
      lj92_arith_encode_bit(e, &mag[0][i], 0);
      m >>= 1;
      while ((m >>= 1) != 0) {
        lj92_arith_encode_bit(e, &mag[1][i], (m & sz) != 0);
      }
    } else {
      lj92_arith_encode_bit(e, z + (sign ? 3 : 2), 0);
    }
  }
  e->left[c] = diff;
  e->above[c][x] = (int16_t)diff;
  return e->sink->sink_failed ? TDNG_LJ92_ERROR_IO : TDNG_LJ92_ERROR_NONE;
}

static int adv_plane_value(const tdng_lj92_const_plane* plane, uint32_t x,
                           uint32_t y, int pt, uint32_t* value) {
  size_t at = (size_t)y * plane->row_stride_samples +
              (size_t)x * plane->pixel_stride_samples;
  uint32_t v = plane->data[at];
  if (pt && (v & ((1u << pt) - 1u)) != 0u) {
    return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
  }
  *value = v >> pt;
  return TDNG_LJ92_ERROR_NONE;
}

static int adv_diff_at(const tdng_lj92_frame_info* frame,
                       const tdng_lj92_const_plane* plane,
                       const tdng_lj92_scan_plan* scan, uint32_t x, uint32_t y,
                       int reset, int* diff_out) {
  int reduced_bits = frame->precision - scan->point_transform;
  uint32_t mask = (1u << reduced_bits) - 1u;
  uint32_t sample, lv = 0, av = 0, alv = 0;
  int px;
  int ret = adv_plane_value(plane, x, y, scan->point_transform, &sample);
  if (ret != TDNG_LJ92_ERROR_NONE || sample > mask) {
    return ret == TDNG_LJ92_ERROR_NONE ? TDNG_LJ92_ERROR_TOO_WIDE : ret;
  }
  if ((reset && x == 0) || (x == 0 && y == 0)) {
    px = 1 << (reduced_bits - 1);
  } else if (reset || y == 0) {
    (void)adv_plane_value(plane, x - 1u, y, scan->point_transform, &lv);
    px = (int)lv;
  } else if (x == 0) {
    (void)adv_plane_value(plane, x, y - 1u, scan->point_transform, &av);
    px = (int)av;
  } else {
    (void)adv_plane_value(plane, x - 1u, y, scan->point_transform, &lv);
    (void)adv_plane_value(plane, x, y - 1u, scan->point_transform, &av);
    (void)adv_plane_value(plane, x - 1u, y - 1u, scan->point_transform, &alv);
    switch (scan->predictor) {
      case 1:
        px = (int)lv;
        break;
      case 2:
        px = (int)av;
        break;
      case 3:
        px = (int)alv;
        break;
      case 4:
        px = (int)lv + (int)av - (int)alv;
        break;
      case 5:
        px = (int)lv + td_floor_half((int)av - (int)alv);
        break;
      case 6:
        px = (int)av + td_floor_half((int)lv - (int)alv);
        break;
      default:
        px = ((int)lv + (int)av) / 2;
        break;
    }
  }
  *diff_out = td_wrap_diff_u16((int)sample, px);
  return TDNG_LJ92_ERROR_NONE;
}

static int adv_process_scan(lje* enc, const tdng_lj92_frame_info* frame,
                            const tdng_lj92_const_plane* planes,
                            const tdng_lj92_scan_plan* scan, int emit) {
  uint32_t hmax = 0, vmax = 0, mcu_cols, mcu_rows;
  uint64_t total, done = 0;
  int rst = 0;
  for (int c = 0; c < frame->component_count; c++) {
    if (frame->components[c].h_sampling > hmax) {
      hmax = frame->components[c].h_sampling;
    }
    if (frame->components[c].v_sampling > vmax) {
      vmax = frame->components[c].v_sampling;
    }
  }
  if (scan->component_count == 1) {
    int c = scan->component_index[0];
    mcu_cols = frame->components[c].width;
    mcu_rows = frame->components[c].height;
  } else {
    mcu_cols = (frame->width + hmax - 1u) / hmax;
    mcu_rows = (frame->height + vmax - 1u) / vmax;
  }
  if (scan->restart_interval_mcus &&
      scan->restart_interval_mcus % mcu_cols != 0u) {
    return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
  }
  total = (uint64_t)mcu_cols * mcu_rows;
  while (done < total) {
    uint64_t chunk = scan->restart_interval_mcus ? scan->restart_interval_mcus
                                                 : total - done;
    if (chunk > total - done) chunk = total - done;
    for (uint64_t unit = 0; unit < chunk; unit++) {
      uint64_t mcu = done + unit;
      int reset_row = unit < mcu_cols;
      if (scan->component_count == 1) {
        int c = scan->component_index[0];
        int diff;
        int ret =
            adv_diff_at(frame, &planes[c], scan, (uint32_t)(mcu % mcu_cols),
                        (uint32_t)(mcu / mcu_cols), reset_row, &diff);
        if (ret != TDNG_LJ92_ERROR_NONE) return ret;
        if (emit)
          enc_emit_diff(enc, diff);
        else
          enc->hist[enc_ssss(diff)]++;
      } else {
        uint32_t mx = (uint32_t)(mcu % mcu_cols);
        uint32_t my = (uint32_t)(mcu / mcu_cols);
        for (int s = 0; s < scan->component_count; s++) {
          int c = scan->component_index[s];
          for (uint32_t vy = 0; vy < frame->components[c].v_sampling; vy++) {
            for (uint32_t hx = 0; hx < frame->components[c].h_sampling; hx++) {
              uint32_t x = mx * frame->components[c].h_sampling + hx;
              uint32_t y = my * frame->components[c].v_sampling + vy;
              int diff;
              int ret;
              if (x >= planes[c].width || y >= planes[c].height) continue;
              ret = adv_diff_at(frame, &planes[c], scan, x, y,
                                reset_row && vy == 0, &diff);
              if (ret != TDNG_LJ92_ERROR_NONE) return ret;
              if (emit)
                enc_emit_diff(enc, diff);
              else
                enc->hist[enc_ssss(diff)]++;
            }
          }
        }
      }
    }
    done += chunk;
    if (emit) {
      enc_flush_bits(enc);
      if (done < total) {
        enc_put_u8(enc, 0xffu);
        enc_put_u8(enc, (uint8_t)(0xd0 + rst));
        rst = (rst + 1) & 7;
      }
    }
  }
  return enc->sink_failed ? TDNG_LJ92_ERROR_IO : TDNG_LJ92_ERROR_NONE;
}

static int adv_process_scan_arithmetic(lje* enc,
                                       const tdng_lj92_frame_info* frame,
                                       const tdng_lj92_const_plane* planes,
                                       const tdng_lj92_scan_plan* scan) {
  uint32_t hmax = 0, vmax = 0, mcu_cols, mcu_rows;
  uint64_t total, done = 0;
  size_t history_count = 0, history_offset = 0;
  int16_t* history;
  lj92_arith_encoder arithmetic;
  int rst = 0;
  for (int c = 0; c < frame->component_count; c++) {
    if (frame->components[c].h_sampling > hmax) {
      hmax = frame->components[c].h_sampling;
    }
    if (frame->components[c].v_sampling > vmax) {
      vmax = frame->components[c].v_sampling;
    }
  }
  if (scan->component_count == 1) {
    int c = scan->component_index[0];
    mcu_cols = frame->components[c].width;
    mcu_rows = frame->components[c].height;
  } else {
    mcu_cols = (frame->width + hmax - 1u) / hmax;
    mcu_rows = (frame->height + vmax - 1u) / vmax;
  }
  if (scan->restart_interval_mcus &&
      scan->restart_interval_mcus % mcu_cols != 0u) {
    return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
  }
  total = (uint64_t)mcu_cols * mcu_rows;
  for (int s = 0; s < scan->component_count; s++) {
    int c = scan->component_index[s];
    if (history_count > SIZE_MAX - planes[c].width) {
      return TDNG_LJ92_ERROR_LIMIT;
    }
    history_count += planes[c].width;
  }
  if (history_count > SIZE_MAX / sizeof(*history)) {
    return TDNG_LJ92_ERROR_LIMIT;
  }
  history =
      (int16_t*)lj92_alloc(&enc->allocator, history_count * sizeof(*history));
  if (!history) return TDNG_LJ92_ERROR_NO_MEMORY;
  memset(&arithmetic, 0, sizeof(arithmetic));
  for (int s = 0; s < scan->component_count; s++) {
    int c = scan->component_index[s];
    arithmetic.above[c] = history + history_offset;
    history_offset += planes[c].width;
  }
#define LJ92_ARITH_ENCODE_FAIL(code)     \
  do {                                   \
    lj92_free(&enc->allocator, history); \
    return (code);                       \
  } while (0)
  while (done < total) {
    uint64_t chunk = scan->restart_interval_mcus ? scan->restart_interval_mcus
                                                 : total - done;
    if (chunk > total - done) chunk = total - done;
    memset(history, 0, history_count * sizeof(*history));
    lj92_arith_encoder_reset(&arithmetic, enc);
    for (uint64_t unit = 0; unit < chunk; unit++) {
      uint64_t mcu = done + unit;
      int reset_row = unit < mcu_cols;
      if (scan->component_count == 1) {
        int c = scan->component_index[0];
        int diff;
        int ret =
            adv_diff_at(frame, &planes[c], scan, (uint32_t)(mcu % mcu_cols),
                        (uint32_t)(mcu / mcu_cols), reset_row, &diff);
        if (ret != TDNG_LJ92_ERROR_NONE) LJ92_ARITH_ENCODE_FAIL(ret);
        ret = lj92_arith_encode_diff(scan, 0, (uint32_t)(mcu % mcu_cols), diff,
                                     &arithmetic);
        if (ret != TDNG_LJ92_ERROR_NONE) LJ92_ARITH_ENCODE_FAIL(ret);
      } else {
        uint32_t mx = (uint32_t)(mcu % mcu_cols);
        uint32_t my = (uint32_t)(mcu / mcu_cols);
        for (int s = 0; s < scan->component_count; s++) {
          int c = scan->component_index[s];
          for (uint32_t vy = 0; vy < frame->components[c].v_sampling; vy++) {
            for (uint32_t hx = 0; hx < frame->components[c].h_sampling; hx++) {
              uint32_t x = mx * frame->components[c].h_sampling + hx;
              uint32_t y = my * frame->components[c].v_sampling + vy;
              int diff;
              int ret;
              if (x >= planes[c].width || y >= planes[c].height) continue;
              ret = adv_diff_at(frame, &planes[c], scan, x, y,
                                reset_row && vy == 0, &diff);
              if (ret != TDNG_LJ92_ERROR_NONE) LJ92_ARITH_ENCODE_FAIL(ret);
              ret = lj92_arith_encode_diff(scan, s, x, diff, &arithmetic);
              if (ret != TDNG_LJ92_ERROR_NONE) LJ92_ARITH_ENCODE_FAIL(ret);
            }
          }
        }
      }
    }
    lj92_arith_finish(&arithmetic);
    done += chunk;
    if (done < total) {
      enc_put_u8(enc, 0xffu);
      enc_put_u8(enc, (uint8_t)(0xd0 + rst));
      rst = (rst + 1) & 7;
    }
  }
  lj92_free(&enc->allocator, history);
#undef LJ92_ARITH_ENCODE_FAIL
  return enc->sink_failed ? TDNG_LJ92_ERROR_IO : TDNG_LJ92_ERROR_NONE;
}

int tdng_lj92_encode_frame(const tdng_lj92_frame_info* frame,
                           const tdng_lj92_const_plane* planes,
                           size_t plane_count, const tdng_lj92_scan_plan* scans,
                           size_t scan_count, void* user,
                           tdng_lj92_write_fn write_fn,
                           const tdng_lj92_allocator* allocator) {
  tdng_lj92_allocator resolved;
  lje* enc;
  uint32_t covered = 0;
  uint32_t current_dri = UINT32_MAX;
  int hmax = 0, vmax = 0;
  int arithmetic;
  int ret = TDNG_LJ92_ERROR_NONE;
  if (!frame || !planes || !scans || !write_fn ||
      !lj92_allocator_resolve(allocator, &resolved) || frame->width == 0 ||
      frame->width > 0xffffu || frame->height == 0 || frame->height > 0xffffu ||
      frame->precision < 2 || frame->precision > 16 ||
      frame->component_count < 1 ||
      frame->component_count > TDNG_LJ92_MAX_COMPONENTS ||
      plane_count != frame->component_count || scan_count < 1 ||
      scan_count > TDNG_LJ92_MAX_COMPONENTS) {
    return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
  }
  if (frame->sof_marker != 0 && frame->sof_marker != 0xC3 &&
      frame->sof_marker != 0xCB) {
    return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
  }
  arithmetic = frame->sof_marker == 0xCB;
  for (int c = 0; c < frame->component_count; c++) {
    if (frame->components[c].h_sampling < 1 ||
        frame->components[c].h_sampling > 4 ||
        frame->components[c].v_sampling < 1 ||
        frame->components[c].v_sampling > 4) {
      return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
    }
    if (frame->components[c].h_sampling > hmax) {
      hmax = frame->components[c].h_sampling;
    }
    if (frame->components[c].v_sampling > vmax) {
      vmax = frame->components[c].v_sampling;
    }
    for (int p = 0; p < c; p++) {
      if (frame->components[p].id == frame->components[c].id) {
        return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
      }
    }
  }
  for (int c = 0; c < frame->component_count; c++) {
    uint32_t cw =
        (frame->width * frame->components[c].h_sampling + (uint32_t)hmax - 1u) /
        (uint32_t)hmax;
    uint32_t ch = (frame->height * frame->components[c].v_sampling +
                   (uint32_t)vmax - 1u) /
                  (uint32_t)vmax;
    size_t min_row, last;
    if (!planes[c].data || planes[c].width != cw || planes[c].height != ch ||
        planes[c].pixel_stride_samples == 0 ||
        (cw > 1u && planes[c].pixel_stride_samples >
                        (SIZE_MAX - 1u) / (size_t)(cw - 1u))) {
      return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
    }
    min_row = (size_t)(cw - 1u) * planes[c].pixel_stride_samples + 1u;
    if (planes[c].row_stride_samples < min_row ||
        (ch > 1u &&
         planes[c].row_stride_samples > SIZE_MAX / (size_t)(ch - 1u))) {
      return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
    }
    last = (size_t)(ch - 1u) * planes[c].row_stride_samples;
    if (last > SIZE_MAX - (min_row - 1u)) {
      return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
    }
    last += min_row - 1u;
    if (last >= planes[c].capacity_samples) {
      return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
    }
  }
  for (size_t s = 0; s < scan_count; s++) {
    int sum = 0;
    if (scans[s].component_count < 1 || scans[s].component_count > 4 ||
        scans[s].predictor < 1 || scans[s].predictor > 7 ||
        scans[s].point_transform >= frame->precision) {
      return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
    }
    for (int j = 0; j < scans[s].component_count; j++) {
      int c = scans[s].component_index[j];
      uint32_t bit;
      if (c >= frame->component_count ||
          scans[s].restart_interval_mcus > 0xffffu) {
        return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
      }
      bit = 1u << c;
      if (covered & bit) return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
      covered |= bit;
      sum += frame->components[c].h_sampling * frame->components[c].v_sampling;
    }
    if (scans[s].component_count > 1 && sum > 10) {
      return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
    }
  }
  if (covered != ((1u << frame->component_count) - 1u)) {
    return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
  }
  enc = (lje*)lj92_alloc_zero(&resolved, sizeof(*enc));
  if (!enc) return TDNG_LJ92_ERROR_NO_MEMORY;
  enc->allocator = resolved;
  enc->sink_user = user;
  enc->sink_write = write_fn;
  if (!arithmetic) {
    for (size_t s = 0; s < scan_count; s++) {
      ret = adv_process_scan(enc, frame, planes, &scans[s], 0);
      if (ret != TDNG_LJ92_ERROR_NONE) goto done;
    }
    enc_build_huffman_table(enc);
    for (int s = 0; s < LJ92_MAX_SSSS; s++) {
      if (enc->hist[s] > 0 && enc->huffbits[s] == 0) {
        ret = TDNG_LJ92_ERROR_CORRUPT;
        goto done;
      }
    }
  }
  enc_put_u8(enc, 0xffu);
  enc_put_u8(enc, 0xd8u);
  enc_put_u8(enc, 0xffu);
  enc_put_u8(enc, arithmetic ? 0xcbu : 0xc3u);
  {
    int lf = 8 + 3 * frame->component_count;
    enc_put_u8(enc, (uint8_t)(lf >> 8));
    enc_put_u8(enc, (uint8_t)lf);
  }
  enc_put_u8(enc, frame->precision);
  enc_put_u8(enc, (uint8_t)(frame->height >> 8));
  enc_put_u8(enc, (uint8_t)frame->height);
  enc_put_u8(enc, (uint8_t)(frame->width >> 8));
  enc_put_u8(enc, (uint8_t)frame->width);
  enc_put_u8(enc, frame->component_count);
  for (int c = 0; c < frame->component_count; c++) {
    enc_put_u8(enc, frame->components[c].id);
    enc_put_u8(enc, (uint8_t)((frame->components[c].h_sampling << 4) |
                              frame->components[c].v_sampling));
    enc_put_u8(enc, 0);
  }
  if (arithmetic) {
    /* One shared DC conditioning table with the T.81 defaults L=0,U=1. */
    enc_put_u8(enc, 0xffu);
    enc_put_u8(enc, 0xccu);
    enc_put_u8(enc, 0);
    enc_put_u8(enc, 4);
    enc_put_u8(enc, 0);
    enc_put_u8(enc, 0x10);
  } else {
    enc_put_u8(enc, 0xffu);
    enc_put_u8(enc, 0xc4u);
    {
      int lh = 19 + enc->huffval_count;
      enc_put_u8(enc, (uint8_t)(lh >> 8));
      enc_put_u8(enc, (uint8_t)lh);
    }
    enc_put_u8(enc, 0);
    for (int l = 1; l <= 16; l++) enc_put_u8(enc, (uint8_t)enc->bits[l]);
    for (int i = 0; i < enc->huffval_count; i++) {
      enc_put_u8(enc, enc->huffval[i]);
    }
  }
  for (size_t s = 0; s < scan_count; s++) {
    const tdng_lj92_scan_plan* scan = &scans[s];
    if (scan->restart_interval_mcus != current_dri) {
      enc_put_u8(enc, 0xffu);
      enc_put_u8(enc, 0xddu);
      enc_put_u8(enc, 0);
      enc_put_u8(enc, 4);
      enc_put_u8(enc, (uint8_t)(scan->restart_interval_mcus >> 8));
      enc_put_u8(enc, (uint8_t)scan->restart_interval_mcus);
      current_dri = scan->restart_interval_mcus;
    }
    enc_put_u8(enc, 0xffu);
    enc_put_u8(enc, 0xdau);
    {
      int ls = 6 + 2 * scan->component_count;
      enc_put_u8(enc, (uint8_t)(ls >> 8));
      enc_put_u8(enc, (uint8_t)ls);
    }
    enc_put_u8(enc, scan->component_count);
    for (int j = 0; j < scan->component_count; j++) {
      enc_put_u8(enc, frame->components[scan->component_index[j]].id);
      enc_put_u8(enc, 0);
    }
    enc_put_u8(enc, scan->predictor);
    enc_put_u8(enc, 0);
    enc_put_u8(enc, scan->point_transform);
    if (arithmetic) {
      ret = adv_process_scan_arithmetic(enc, frame, planes, scan);
    } else {
      enc->bitbuf = 0;
      enc->nbits = 0;
      ret = adv_process_scan(enc, frame, planes, scan, 1);
    }
    if (ret != TDNG_LJ92_ERROR_NONE) goto done;
  }
  enc_put_u8(enc, 0xffu);
  enc_put_u8(enc, 0xd9u);
  enc_stage_flush(enc);
  if (enc->sink_failed) ret = TDNG_LJ92_ERROR_IO;
done:
  lj92_free(&resolved, enc);
  return ret;
}

/* ------------------------------------------------------------------ */
/* Streaming encoder                                                  */
/*                                                                     */
/* Two-pass by design (Huffman needs the SSSS histogram before the     */
/* header can be emitted): tdng_lj92_encode_scan reads the whole       */
/* image once, tdng_lj92_encode_begin emits SOI..SOS to the sink, and  */
/* tdng_lj92_encode_rows feeds the entropy pass incrementally in row   */
/* bands (the predictor row cache persists across calls). Output is    */
/* staged in 64KB chunks and never materialized as a whole.            */
/* ------------------------------------------------------------------ */

int tdng_lj92_encode_open_ex(tdng_lj92_enc* lj, int width, int height,
                             int bitdepth, int components, int predictor,
                             int readLength, int skipLength, void* user,
                             tdng_lj92_write_fn write_fn,
                             const tdng_lj92_allocator* allocator) {
  size_t row_slots;
  lje* self;
  tdng_lj92_allocator resolved;
  if (!lj || !write_fn) return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
  if (!lj92_allocator_resolve(allocator, &resolved)) {
    return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
  }
  *lj = NULL;
  if (width <= 0 || width > 0xFFFF || height <= 0 || height > 0xFFFF ||
      bitdepth < 2 || bitdepth > 16 || components < 1 || components > 4 ||
      predictor < 1 || predictor > 7 || readLength < 0 || skipLength < 0) {
    return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
  }
  if (width > INT_MAX / components) return TDNG_LJ92_ERROR_LIMIT;
  if (readLength != 0 && readLength != width * components) {
    return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
  }
  self = (lje*)lj92_alloc_zero(&resolved, sizeof(lje));
  if (!self) return TDNG_LJ92_ERROR_NO_MEMORY;
  self->allocator = resolved;
#if defined(TDNG_LJ92_NEON)
  self->simd_level = 3;
#else
  self->simd_level = tdng_simd_level();
#endif
  row_slots = (size_t)width * (size_t)components;
  self->rc = (uint16_t*)lj92_alloc_zero(&self->allocator,
                                        row_slots * 2u * sizeof(uint16_t));
  if (!self->rc) {
    lj92_free(&self->allocator, self);
    return TDNG_LJ92_ERROR_NO_MEMORY;
  }
  if (self->simd_level > 0) {
    self->diffrow = (int16_t*)lj92_alloc(&self->allocator,
                                         row_slots * sizeof(*self->diffrow));
    self->ssssrow =
        (uint8_t*)lj92_alloc(&self->allocator, row_slots * sizeof(uint8_t));
    if (!self->diffrow || !self->ssssrow) {
      lj92_free(&self->allocator, self->diffrow);
      lj92_free(&self->allocator, self->ssssrow);
      lj92_free(&self->allocator, self->rc);
      lj92_free(&self->allocator, self);
      return TDNG_LJ92_ERROR_NO_MEMORY;
    }
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

int tdng_lj92_encode_open(tdng_lj92_enc* lj, int width, int height,
                          int bitdepth, int components, int predictor,
                          int readLength, int skipLength, void* user,
                          tdng_lj92_write_fn write_fn) {
  return tdng_lj92_encode_open_ex(lj, width, height, bitdepth, components,
                                  predictor, readLength, skipLength, user,
                                  write_fn, NULL);
}

/* Pass 1: read the whole image and build the SSSS histogram. `image` is
   the base pointer for all later tdng_lj92_encode_rows calls. */
int tdng_lj92_encode_scan(tdng_lj92_enc lj, const uint16_t* image,
                          const uint16_t* delinearize, int delinearizeLength) {
  lje* self = lj;
  int ret;
  if (!self || !image) return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
  if ((delinearize && delinearizeLength <= 0) ||
      (!delinearize && delinearizeLength != 0)) {
    return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
  }
  if (self->phase != 0) return TDNG_LJ92_ERROR_STATE;
  self->image = image;
  self->delinearize = delinearize;
  self->delinearizeLength = delinearizeLength;
  ret = enc_frequency_scan(self);
  if (ret != TDNG_LJ92_ERROR_NONE) {
    self->phase = 3;
    return ret;
  }
  self->phase = 1;
  return TDNG_LJ92_ERROR_NONE;
}

/* Emit SOI / SOF3 / DHT / SOS to the sink. Resets the pass-2 predictor
   state so tdng_lj92_encode_rows can start at row 0. */
int tdng_lj92_encode_begin(tdng_lj92_enc lj) {
  lje* self = lj;
  int ret;
  size_t row_slots;
  if (!self) return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
  if (self->phase != 1) return TDNG_LJ92_ERROR_STATE;
  enc_build_huffman_table(self);
  /* Insurance for the branchless emit path: every symbol the histogram pass
   * observed must have a code. (Annex K.2 with the sentinel always assigns
   * one; this also catches a tampered table between passes.) */
  for (int s = 0; s < LJ92_MAX_SSSS; s++) {
    if (self->hist[s] > 0 && self->huffbits[s] == 0) {
      self->phase = 3;
      return TDNG_LJ92_ERROR_CORRUPT;
    }
  }
  ret = enc_write_header(self);
  if (ret != TDNG_LJ92_ERROR_NONE) {
    self->phase = 3;
    return ret;
  }
  enc_stage_flush(self);
  if (self->sink_failed) {
    self->phase = 3;
    return TDNG_LJ92_ERROR_IO;
  }
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
  if (!self || !image) return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
  if (self->phase != 2) return TDNG_LJ92_ERROR_STATE;
  if (row0 < 0 || row_count <= 0 || row0 != self->next_row ||
      row_count > self->height - row0) {
    return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
  }
  if (image != self->image) return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
  ret = enc_write_rows(self, row0, row_count);
  if (ret != TDNG_LJ92_ERROR_NONE) {
    // Poison the phase machine: the internal read pointer has already
    // advanced partway through this band, so a retry would resume from the
    // wrong position and eventually walk out of the caller's image buffer.
    self->phase = 3;
    return ret;
  }
  enc_stage_flush(self);
  if (self->sink_failed) {
    self->phase = 3;
    return TDNG_LJ92_ERROR_IO;
  }
  self->next_row = row0 + row_count;
  return TDNG_LJ92_ERROR_NONE;
}

/* Flush the final bits, emit EOI and release the encoder. The handle is
   invalid after this call; the return value is the final encode status. */
int tdng_lj92_encode_finish(tdng_lj92_enc lj) {
  lje* self = lj;
  tdng_lj92_allocator allocator;
  int ret = TDNG_LJ92_ERROR_NONE;
  if (!self) return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
  allocator = self->allocator;
  if (self->phase != 2) {
    ret = TDNG_LJ92_ERROR_STATE;  // begin never ran or prior terminal error
  } else if (self->next_row != self->height) {
    ret = TDNG_LJ92_ERROR_STATE;  // all rows must be supplied
  } else {
    enc_flush_bits(self);
    ret = enc_write_eoi(self);
    enc_stage_flush(self);
    if (self->sink_failed) ret = TDNG_LJ92_ERROR_IO;
  }
  lj92_free(&allocator, self->diffrow);
  lj92_free(&allocator, self->ssssrow);
  lj92_free(&allocator, self->rc);
  lj92_free(&allocator, self);
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
  {
    int finish_ret = tdng_lj92_encode_finish(lj); /* always frees encoder */
    if (ret == TDNG_LJ92_ERROR_NONE) ret = finish_ret;
  }
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
    u8* nb = (u8*)lj92_realloc_copy(&self->allocator, self->ebuf,
                                    (size_t)self->ebuf_cap, (size_t)need);
    if (!nb) return TDNG_LJ92_ERROR_NO_MEMORY;
    self->ebuf = nb;
    self->ebuf_cap = need;
  }
  u8* dst = self->ebuf;
  uint64_t pos = stream_pos;
  {
    int marker = -1;
    while (pos < end) {
      if (!srefill(s, pos, 1)) break;
      u8 b = s->buf[(size_t)(pos - s->buf_off)];
      pos++;
      if (b != 0xFF) {
        *dst++ = b;
        continue;
      }
      do {
        if (pos >= end || !srefill(s, pos, 1)) {
          return TDNG_LJ92_ERROR_CORRUPT;
        }
        b = s->buf[(size_t)(pos - s->buf_off)];
        pos++;
      } while (b == 0xFF);
      if (b == 0x00) {
        *dst++ = 0xFF;
        continue;
      }
      marker = b;
      break;
    }
    if (marker != 0xD9)
      return marker >= 0xD0 && marker <= 0xD7 ? TDNG_LJ92_ERROR_UNSUPPORTED
                                              : TDNG_LJ92_ERROR_CORRUPT;
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
  /* Heap scratch instead of a 64KB stack array: this runs on whatever
   * thread calls open_streaming (decode workers may have small stacks). */
  u8* scratch = (u8*)lj92_alloc(&self->allocator, TDNG_LJ92_STREAM_CHUNK);
  int result;
  if (!scratch) return TDNG_LJ92_ERROR_NO_MEMORY;
  result = TDNG_LJ92_ERROR_CORRUPT; /* default status for the error paths */
  if (UINT64_MAX - soi_off < 2u) goto out;
  pos = soi_off + 2u;
  self->x = 0;
  self->y = 0;
  self->components = 0;
  self->bits = 0;
  self->sof_marker = 0;
  for (;;) {
    uint64_t mo = 0;
    int m = stream_find_marker(self, pos, &mo);
    if (m < 0) goto out;
    if (m == 0xD8) {
      if (UINT64_MAX - mo < 2u) goto out;
      pos = mo + 2u;
      continue;
    }
    if (m == 0x01) {
      if (UINT64_MAX - mo < 2u) goto out;
      pos = mo + 2u;
      continue;
    }
    if (m == 0xD9) goto out;  // EOI before SOS
    if (UINT64_MAX - mo < 2u) goto out;
    uint16_t segsize = 0;
    if (!sread(s, mo + 2, &segsize, 2)) goto out;
    segsize = (uint16_t)((segsize >> 8) | (segsize << 8));
    if (segsize < 2) goto out;
    uint64_t sd, se;  // sd is the segment's Ln length field
    if (UINT64_MAX - mo < 2u) {
      goto out;
    }
    sd = mo + 2u;
    if (UINT64_MAX - sd < (uint64_t)segsize) {
      goto out;
    }
    se = sd + (uint64_t)segsize;
    size_t payload_len = (size_t)segsize;
    if (payload_len > (size_t)TDNG_LJ92_STREAM_CHUNK) goto out;

    if (m == 0xDA) {  // SOS: parse and record the SOS header offset
      result = TDNG_LJ92_ERROR_CORRUPT;
      if (!sread(s, sd, scratch, payload_len)) goto out;
      {
        int compcount = 0, pred = 0, pt = 0, Ls = 0;
        int ret =
            parse_sos_payload(self, scratch, (int)payload_len, self->components,
                              &compcount, &pred, &pt, &Ls);
        if (ret != TDNG_LJ92_ERROR_NONE) {
          result = ret;
          goto out;
        }
      }
      self->stream_scanstart = sd;  // the entropy data starts at sd + Ls
      result = expand_luts_uniform(self);
      goto out;
    }
    if (m >= 0xC0 && m <= 0xCF && m != 0xC4) {  // SOF frames
      u8 precision, nf;
      uint16_t yh, xh;
      if (segsize < 11 || !sread(s, sd, scratch, payload_len)) goto out;
      precision = scratch[2];
      yh = (uint16_t)be16(scratch + 3);
      xh = (uint16_t)be16(scratch + 5);
      nf = scratch[7];
      if (nf < 1 || nf > LJ92_MAX_COMPONENTS ||
          segsize != (uint16_t)(8 + 3 * nf) || xh == 0 || yh == 0) {
        goto out;
      }
      self->sof_marker = (uint8_t)m;
      self->bits = precision;
      self->y = (int)yh;
      self->x = (int)xh;
      self->components = (int)nf;
      // A4: bitdepth must be in [2,16] so that 1 << (bits-1) is defined.
      if (self->bits < 2 || self->bits > 16) goto out;
      for (int c = 0; c < self->components; c++) {
        const u8* cp = scratch + 8 + 3 * c;
        u8 hv = cp[1];
        if ((hv >> 4) == 0 || (hv & 15) == 0 || cp[2] != 0) goto out;
        for (int p = 0; p < c; p++) {
          if (self->component_id[p] == cp[0]) goto out;
        }
        self->component_id[c] = cp[0];
        self->component_h[c] = hv >> 4;
        self->component_v[c] = hv & 15;
      }
      pos = se;
      continue;
    }
    if (m == 0xC4) {  // DHT: build a Huffman LUT
      if (!sread(s, sd, scratch, payload_len)) goto out;
      {
        int dht_pos = 2;
        while (dht_pos < (int)payload_len) {
          int used = 0, table_id = 0, maxbits = 0;
          u32* lut = NULL;
          int ret = build_huff_lut(&self->allocator, scratch + dht_pos,
                                   (int)payload_len - dht_pos, &used, &table_id,
                                   &lut, &maxbits);
          if (ret != TDNG_LJ92_ERROR_NONE) {
            result = ret;
            goto out;
          }
          lj92_free(&self->allocator, self->hufflut[table_id]);
          if (!self->huff_defined[table_id]) self->num_huff_idx++;
          self->hufflut[table_id] = lut;
          self->huffbits[table_id] = maxbits;
          self->huff_defined[table_id] = 1;
          dht_pos += used;
        }
        if (dht_pos != (int)payload_len) goto out;
      }
      pos = se;
      continue;
    }
    // APPn, COM, DQT, DRI, ...: skip the segment.
    pos = se;
  }
out:
  lj92_free(&self->allocator, scratch);
  return result;
}

// Streaming scan: re-read the SOS payload from the stream, destuff the
// entropy payload from the stream into ebuf, then run the same specialized
// scan runners as the memory path.
static int parseScanStreaming(ljp* self) {
  if (self->sof_marker == 0xCB) return TDNG_LJ92_ERROR_UNSUPPORTED;
  tdng_lj92_stream* s = (tdng_lj92_stream*)self->stream_user;
  uint64_t so = self->stream_scanstart;  // SOS Ls field offset
  u8 payload[64];  // SOS payload is at most 6 + 2*16 + 2 = 40 bytes
  uint16_t ls16 = 0;
  if (!sread(s, so, &ls16, 2)) return TDNG_LJ92_ERROR_CORRUPT;
  int Ls = (int)(((ls16 >> 8) | (ls16 << 8)) & 0xFFFFu);
  if (Ls < 3 || Ls > (int)sizeof(payload)) return TDNG_LJ92_ERROR_CORRUPT;
  if (!sread(s, so, payload, (size_t)Ls)) return TDNG_LJ92_ERROR_CORRUPT;
  int compcount = 0, pred = 0, pt = 0, sos_len = 0;
  int ret = parse_sos_payload(self, payload, Ls, self->components, &compcount,
                              &pred, &pt, &sos_len);
  if (ret != TDNG_LJ92_ERROR_NONE) return ret;
  if (pt != 0) return TDNG_LJ92_ERROR_UNSUPPORTED;

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

int tdng_lj92_open_streaming_ex(tdng_lj92* lj, void* user,
                                tdng_lj92_read_fn read_fn,
                                tdng_lj92_size_fn size_fn,
                                const tdng_lj92_allocator* allocator,
                                int* width, int* height, int* bitdepth,
                                int* components) {
  ljp* self;
  tdng_lj92_stream* sctx;
  tdng_lj92_allocator resolved;
  int ret;
  uint64_t mo = 0;

  if (!lj || !read_fn || !width || !height || !bitdepth || !components) {
    return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
  }
  if (!lj92_allocator_resolve(allocator, &resolved)) {
    return TDNG_LJ92_ERROR_INVALID_ARGUMENT;
  }
  *lj = NULL;
  self = (ljp*)lj92_alloc_zero(&resolved, sizeof(ljp));
  if (!self) return TDNG_LJ92_ERROR_NO_MEMORY;
  self->allocator = resolved;
  for (int t = 0; t < 4; t++) self->arith_dc_u[t] = 1;
  sctx = (tdng_lj92_stream*)lj92_alloc_zero(&self->allocator, sizeof(*sctx));
  if (!sctx) {
    lj92_free(&self->allocator, self);
    return TDNG_LJ92_ERROR_NO_MEMORY;
  }
  sctx->user = user;
  sctx->read_fn = read_fn;
  sctx->size_fn = size_fn;
  sctx->size = size_fn ? size_fn(user) : UINT64_MAX;
  sctx->buf_cap = TDNG_LJ92_STREAM_CHUNK;
  if (sctx->size != UINT64_MAX && sctx->size < (uint64_t)sctx->buf_cap) {
    sctx->buf_cap = (size_t)sctx->size;
  }
  if (sctx->buf_cap == 0u) sctx->buf_cap = 1u;
  sctx->buf = (u8*)lj92_alloc(&self->allocator, sctx->buf_cap);
  if (!sctx->buf) {
    lj92_free(&self->allocator, sctx);
    lj92_free(&self->allocator, self);
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
  if (self->x <= 0 || self->components <= 0 ||
      (self->sof_marker != 0xC3 && self->sof_marker != 0xCB)) {
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
  lj92_free(&self->allocator, sctx->buf);
  lj92_free(&self->allocator, sctx);
  lj92_free(&self->allocator, self);
  return ret;
}

int tdng_lj92_open_streaming(tdng_lj92* lj, void* user,
                             tdng_lj92_read_fn read_fn,
                             tdng_lj92_size_fn size_fn, int* width, int* height,
                             int* bitdepth, int* components) {
  return tdng_lj92_open_streaming_ex(lj, user, read_fn, size_fn, NULL, width,
                                     height, bitdepth, components);
}
// End liblj92 ---------------------------------------------------------
