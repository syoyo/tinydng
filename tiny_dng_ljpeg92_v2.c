#include "tiny_dng_ljpeg92_v2.h"

#include <stdlib.h>
#include <string.h>

#if defined(TINY_DNG_LJPEG92_V2_USE_AVX2)
#include <immintrin.h>
#elif defined(TINY_DNG_LJPEG92_V2_USE_SSE2)
#include <emmintrin.h>
#endif

#define TINY_DNG_DPRINTF(...)
#define TINY_DNG_CHECK_AND_RETURN_C(cond, retcode) \
  do { \
    if (!(cond)) { \
      return (retcode); \
    } \
  } while (0)

#define lj92_open tdng_lj92_open
#define lj92_close tdng_lj92_close
#define lj92_decode tdng_lj92_decode
#define lj92_encode tdng_lj92_encode

enum LJ92_ERRORS {
  LJ92_ERROR_NONE = 0,
  LJ92_ERROR_CORRUPT = -1,
  LJ92_ERROR_NO_MEMORY = -2,
  LJ92_ERROR_BAD_HANDLE = -3,
  LJ92_ERROR_TOO_WIDE = -4
};

typedef struct _ljp* lj92;

/* Parse a lossless JPEG (1992) structure returning
 * - a handle that can be used to decode the data
 * - width/height/bitdepth/components of the data
 * Returns status code.
 * If status == LJ92_ERROR_NONE, handle must be closed with lj92_close
 */
int lj92_open(lj92* lj,                          // Return handle here
              const uint8_t* data, int datalen,  // The encoded data
              int* width, int* height,
              int* bitdepth, int* components);  // Width, height, bitdepth and components

/* Release a decoder object */
void lj92_close(lj92 lj);

/*
 * Decode previously opened lossless JPEG (1992) into a 2D tile of memory
 * Starting at target, write writeLength 16bit values, then skip 16bit
 * skipLength value before writing again
 * If linearize is not NULL, use table at linearize to convert data values from
 * output value to target value
 * Data is only correct if LJ92_ERROR_NONE is returned
 */
int lj92_decode(
    lj92 lj, uint16_t* target, int writeLength,
    int skipLength,  // The image is written to target as a tile
    uint16_t* linearize,
    int linearizeLength);  // If not null, linearize the data using this table

#if 0
/*
 * Encode a grayscale image supplied as 16bit values within the given bitdepth
 * Read from tile in the image
 * Apply delinearization if given
 * Return the encoded lossless JPEG stream
 */
int lj92_encode(uint16_t* image, int width, int height, int bitdepth,
                       int readLength, int skipLength, uint16_t* delinearize,
                       int delinearizeLength, uint8_t** encoded,
                       int* encodedLength);
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

//#define SLOW_HUFF
//#define LJ92_DEBUG

#define LJ92_MAX_COMPONENTS (16)

typedef struct _ljp {
  u8* data;
  u8* dataend;
  int datalen;
  int scanstart;
  int ix;
  int x;           // Width
  int y;           // Height
  int bits;        // Bit depth
  int components;  // Components(Nf)
  int writelen;    // Write rows this long
  int skiplen;     // Skip this many values after each row
  u16* linearize;  // Linearization table
  int linlen;
  int sssshist[16];

// Huffman table - only one supported, and probably needed
#ifdef SLOW_HUFF
  // NOTE: Huffman table for each components is not supported for SLOW_HUFF code
  // path.
  int* maxcode;
  int* mincode;
  int* valptr;
  u8* huffval;
  int* huffsize;
  int* huffcode;
#else
  // Huffman table for each components
  u16* hufflut[LJ92_MAX_COMPONENTS];
  int huffbits[LJ92_MAX_COMPONENTS];
  int num_huff_idx;
#endif
  // Parse state
  int cnt;
  u32 b;
  u16* image;
  u16* rowcache;
  u16* diffcache;
  u16* outrow[2];
} ljp;

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

static int parseHuff(ljp* self) {
  int ret = LJ92_ERROR_CORRUPT;
  u8* huffhead =
      &self->data
           [self->ix];  // xstruct.unpack('>HB16B',self.data[self.ix:self.ix+19])
  u8* bits = &huffhead[2];
  bits[0] = 0;  // Because table starts from 1
  int hufflen = BEH(huffhead[0]);
  if ((self->ix + hufflen) >= self->datalen) return ret;
#ifdef SLOW_HUFF
  u8* huffval = calloc(hufflen - 19, sizeof(u8));
  if (huffval == NULL) return LJ92_ERROR_NO_MEMORY;
  self->huffval = huffval;
  for (int hix = 0; hix < (hufflen - 19); hix++) {
    huffval[hix] = self->data[self->ix + 19 + hix];
#ifdef LJ92_DEBUG
    TINY_DNG_DPRINTF("huffval[%d]=%d\n", hix, huffval[hix]);
#endif
  }
  self->ix += hufflen;
  // Generate huffman table
  int k = 0;
  int i = 1;
  int j = 1;
  int huffsize_needed = 1;
  // First calculate how long huffsize needs to be
  while (i <= 16) {
    while (j <= bits[i]) {
      huffsize_needed++;
      k = k + 1;
      j = j + 1;
    }
    i = i + 1;
    j = 1;
  }
  // Now allocate and do it
  int* huffsize = calloc(huffsize_needed, sizeof(int));
  if (huffsize == NULL) return LJ92_ERROR_NO_MEMORY;
  self->huffsize = huffsize;
  k = 0;
  i = 1;
  j = 1;
  // First calculate how long huffsize needs to be
  int hsix = 0;
  while (i <= 16) {
    while (j <= bits[i]) {
      huffsize[hsix++] = i;
      k = k + 1;
      j = j + 1;
    }
    i = i + 1;
    j = 1;
  }
  huffsize[hsix++] = 0;

  // Calculate the size of huffcode array
  int huffcode_needed = 0;
  k = 0;
  int code = 0;
  int si = huffsize[0];
  while (1) {
    while (huffsize[k] == si) {
      huffcode_needed++;
      code = code + 1;
      k = k + 1;
    }
    if (huffsize[k] == 0) break;
    while (huffsize[k] != si) {
      code = code << 1;
      si = si + 1;
    }
  }
  // Now fill it
  int* huffcode = calloc(huffcode_needed, sizeof(int));
  if (huffcode == NULL) return LJ92_ERROR_NO_MEMORY;
  self->huffcode = huffcode;
  int hcix = 0;
  k = 0;
  code = 0;
  si = huffsize[0];
  while (1) {
    while (huffsize[k] == si) {
      huffcode[hcix++] = code;
      code = code + 1;
      k = k + 1;
    }
    if (huffsize[k] == 0) break;
    while (huffsize[k] != si) {
      code = code << 1;
      si = si + 1;
    }
  }

  i = 0;
  j = 0;

  int* maxcode = calloc(17, sizeof(int));
  if (maxcode == NULL) return LJ92_ERROR_NO_MEMORY;
  self->maxcode = maxcode;
  int* mincode = calloc(17, sizeof(int));
  if (mincode == NULL) return LJ92_ERROR_NO_MEMORY;
  self->mincode = mincode;
  int* valptr = calloc(17, sizeof(int));
  if (valptr == NULL) return LJ92_ERROR_NO_MEMORY;
  self->valptr = valptr;

  while (1) {
    while (1) {
      i++;
      if (i > 16) break;
      if (bits[i] != 0) break;
      maxcode[i] = -1;
    }
    if (i > 16) break;
    valptr[i] = j;
    mincode[i] = huffcode[j];
    j = j + bits[i] - 1;
    maxcode[i] = huffcode[j];
    j++;
  }
  free(huffsize);
  self->huffsize = NULL;
  free(huffcode);
  self->huffcode = NULL;
  ret = LJ92_ERROR_NONE;
#else
  /* Calculate huffman direct lut */
  // How many bits in the table - find highest entry
  u8* huffvals = &self->data[self->ix + 19];
  int maxbits = 16;
  while (maxbits > 0) {
    if (bits[maxbits]) break;
    maxbits--;
  }
  self->huffbits[self->num_huff_idx] = maxbits;
  TINY_DNG_DPRINTF("huffbuts[%d] = %d\n", self->num_huff_idx, maxbits);

  /* Now fill the lut */
  u16* hufflut = (u16*)malloc((1 << maxbits) * sizeof(u16));
  // TINY_DNG_DPRINTF("maxbits = %d\n", maxbits);
  if (hufflut == NULL) return LJ92_ERROR_NO_MEMORY;
  self->hufflut[self->num_huff_idx] = hufflut;
  int i = 0;
  int hv = 0;
  int rv = 0;
  int vl = 0;  // i
  int hcode;
  int bitsused = 1;
#ifdef LJ92_DEBUG
  TINY_DNG_DPRINTF("%04x:%x:%d:%x\n", i, huffvals[hv], bitsused,
                   1 << (maxbits - bitsused));
#endif
  while (i < 1 << maxbits) {
    if (bitsused > maxbits) {
      break;  // Done. Should never get here!
    }
    if (vl >= bits[bitsused]) {
      bitsused++;
      vl = 0;
      continue;
    }
    if (rv == 1 << (maxbits - bitsused)) {
      rv = 0;
      vl++;
      hv++;
#ifdef LJ92_DEBUG
      TINY_DNG_DPRINTF("%04x:%x:%d:%x\n", i, huffvals[hv], bitsused,
                       1 << (maxbits - bitsused));
#endif
      continue;
    }
    hcode = huffvals[hv];
    hufflut[i] = hcode << 8 | bitsused;
    TINY_DNG_DPRINTF("idx[%d] hufflut[%d] = %d(bitsused = %d, hcode = %d\n",self->num_huff_idx, i, hufflut[i], bitsused,hcode);
    i++;
    rv++;
  }
  ret = LJ92_ERROR_NONE;
#endif
  self->num_huff_idx++;

  return ret;
}

static int parseSof3(ljp* self) {
  if (self->ix + 6 >= self->datalen) return LJ92_ERROR_CORRUPT;
  self->y = BEH(self->data[self->ix + 3]);
  self->x = BEH(self->data[self->ix + 5]);
  self->bits = self->data[self->ix + 2];
  self->components = self->data[self->ix + 7];
  self->ix += BEH(self->data[self->ix]);

  if ((self->components >= 1) && (self->components < 6)) {
    // ok
  } else {
    // Invalid number of components.
    return LJ92_ERROR_CORRUPT;
  }
  //TINY_DNG_ASSERT(self->components >= 1 && self->components < 6,
  //                "Invalid number of components.");

  return LJ92_ERROR_NONE;
}

static int parseBlock(ljp* self, int marker) {
  (void)marker;
  self->ix += BEH(self->data[self->ix]);
  if (self->ix >= self->datalen) {
    TINY_DNG_DPRINTF("parseBlock: ix %d, datalen %d\n", self->ix,
                     self->datalen);
    return LJ92_ERROR_CORRUPT;
  }
  return LJ92_ERROR_NONE;
}

#ifdef SLOW_HUFF
static int nextbit(ljp* self) {
  u32 b = self->b;
  if (self->cnt == 0) {
    u8* data = &self->data[self->ix];
    u32 next = *data++;
    b = next;
    if (next == 0xff) {
      data++;
      self->ix++;
    }
    self->ix++;
    self->cnt = 8;
  }
  int bit = b >> 7;
  self->cnt--;
  self->b = (b << 1) & 0xFF;
  return bit;
}

static int decode(ljp* self) {
  int i = 1;
  int code = nextbit(self);
  while (code > self->maxcode[i]) {
    i++;
    code = (code << 1) + nextbit(self);
  }
  int j = self->valptr[i];
  j = j + code - self->mincode[i];
  int value = self->huffval[j];
  return value;
}

static int receive(ljp* self, int ssss) {
  int i = 0;
  int v = 0;
  while (i != ssss) {
    i++;
    v = (v << 1) + nextbit(self);
  }
  return v;
}

static int extend(ljp* self, int v, int t) {
  int vt = 1 << (t - 1);
  if (v < vt) {
    vt = (-1 << t) + 1;
    v = v + vt;
  }
  return v;
}
#endif

inline static int nextdiff(ljp* self, int component_idx, int Px, int *errcode) {
  (void)Px;
  if (component_idx >= self->num_huff_idx) {
    if (errcode) (*errcode) = LJ92_ERROR_CORRUPT;
    return 0;
  }
  u32 b = self->b;
  int cnt = self->cnt;
  int huffbits = self->huffbits[component_idx];
  int ix = self->ix;

  while (cnt < huffbits) {
    if (ix >= self->datalen) {
      if (errcode) (*errcode) = LJ92_ERROR_CORRUPT;
      return 0;
    }
    int val = self->data[ix++];
    b = (b << 8) | val;
    cnt += 8;
    if (val == 0xFF) {
      if (ix < self->datalen) {
        if (self->data[ix] == 0x00) ix++; // skip stuffed zero
        else if (self->data[ix] == 0xFF) {
          // multiple 0xFF, skip one
        } else {
          // marker! shouldn't be here in data, but handle it by not consuming more
          // b >>= 8; cnt -= 8; // already added, but it's a marker.
        }
      }
    }
  }
  int index = b >> (cnt - huffbits);
  u16 ssssused = self->hufflut[component_idx][index];
  int usedbits = ssssused & 0xFF;
  int t = ssssused >> 8;
  self->sssshist[t]++;
  cnt -= usedbits;
  u32 keepbitsmask = (1u << cnt) - 1;
  b &= keepbitsmask;

  while (cnt < t) {
    if (ix >= self->datalen) {
      if (errcode) (*errcode) = LJ92_ERROR_CORRUPT;
      return 0;
    }
    int val = self->data[ix++];
    b = (b << 8) | val;
    cnt += 8;
    if (val == 0xFF) {
      if (ix < self->datalen) {
        if (self->data[ix] == 0x00) ix++;
      }
    }
  }
  cnt -= t;
  int diff = b >> cnt;
  int vt = 1 << (t - 1);
  if (diff < vt) {
    vt = -((1 << t) - 1);
    diff += vt;
  }
  keepbitsmask = (1u << cnt) - 1;
  self->b = b & keepbitsmask;
  self->cnt = cnt;
  self->ix = ix;
  return diff;
}

#if 0 // not used
static int parsePred6(ljp* self) {
  // TODO: Consider self->components
  TINY_DNG_DPRINTF("parsePred6\n");
  int ret = LJ92_ERROR_CORRUPT;
  self->ix = self->scanstart;
  // int compcount = self->data[self->ix+2];
  self->ix += BEH(self->data[self->ix]);
  self->cnt = 0;
  self->b = 0;
  int write = self->writelen;
  // Now need to decode huffman coded values
  int c = 0;
  int pixels = self->y * self->x;
  u16* out = self->image;
  u16* temprow;
  u16* thisrow = self->outrow[0];
  u16* lastrow = self->outrow[1];

  // First pixel predicted from base value
  int diff;
  int Px;
  int col = 0;
  int row = 0;
  int left = 0;
  int linear;
  if (self->num_huff_idx <= self->components) {
    // ok
  } else {
    //TINY_DNG_ASSERT(self->num_huff_idx <= self->components,
    //                "Invalid number of huff indices.");
    return LJ92_ERROR_CORRUPT;
  }
  int errcode = LJ92_ERROR_NONE;
  // First pixel
  diff = nextdiff(self, self->num_huff_idx - 1,
                  0, &errcode);  // FIXME(syoyo): Is using (self->num_huff_idx-1) correct?
  if (errcode != LJ92_ERROR_NONE) {
    return errcode;
  }

  Px = 1 << (self->bits - 1);
  left = Px + diff;
  left = (u16)(left % 65536);
  if (self->linearize)
    linear = self->linearize[left];
  else
    linear = left;
  thisrow[col++] = left;
  out[c++] = linear;
  if (self->ix >= self->datalen) {
    TINY_DNG_DPRINTF("ix = %d, datalen = %d\n", self->ix, self->datalen);
    return ret;
  }
  --write;
  int rowcount = self->x - 1;
  while (rowcount--) {
    int _errcode = LJ92_ERROR_NONE;
    diff = nextdiff(self, self->num_huff_idx - 1, 0, &_errcode);
    if (_errcode != LJ92_ERROR_NONE) {
      return _errcode;
    }
    Px = left;
    left = Px + diff;
    left = (u16)(left % 65536);
    if (self->linearize)
      linear = self->linearize[left];
    else
      linear = left;
    thisrow[col++] = left;
    out[c++] = linear;
    // TINY_DNG_DPRINTF("%d %d %d %d
    // %x\n",col-1,diff,left,thisrow[col-1],&thisrow[col-1]);
    if (self->ix >= self->datalen) {
      TINY_DNG_DPRINTF("a: self->ix = %d, datalen = %d\n", self->ix,
                       self->datalen);
      return ret;
    }
    if (--write == 0) {
      out += self->skiplen;
      write = self->writelen;
    }
  }
  temprow = lastrow;
  lastrow = thisrow;
  thisrow = temprow;
  row++;
  // TINY_DNG_DPRINTF("%x %x\n",thisrow,lastrow);
  while (c < pixels) {
    col = 0;
    int _errcode = LJ92_ERROR_NONE;
    diff = nextdiff(self, self->num_huff_idx - 1, 0, &_errcode);
    if (_errcode != LJ92_ERROR_NONE) {
      return _errcode;
    }
    Px = lastrow[col];  // Use value above for first pixel in row
    left = Px + diff;
    left = (u16)(left % 65536);
    if (self->linearize) {
      if (left > self->linlen) return LJ92_ERROR_CORRUPT;
      linear = self->linearize[left];
    } else
      linear = left;
    thisrow[col++] = left;
    // TINY_DNG_DPRINTF("%d %d %d %d\n",col,diff,left,lastrow[col]);
    out[c++] = linear;
    if (self->ix >= self->datalen) break;
    rowcount = self->x - 1;
    if (--write == 0) {
      out += self->skiplen;
      write = self->writelen;
    }
    while (rowcount--) {
      int errcode_d2 = LJ92_ERROR_NONE;
      diff = nextdiff(self, self->num_huff_idx - 1, 0, &errcode_d2);
      if (errcode_d2 != LJ92_ERROR_NONE) {
        return errcode_d2;
      }

      Px = lastrow[col] + ((left - lastrow[col - 1]) >> 1);
      left = Px + diff;
      left = (u16)(left % 65536);
      // TINY_DNG_DPRINTF("%d %d %d %d %d
      // %x\n",col,diff,left,lastrow[col],lastrow[col-1],&lastrow[col]);
      if (self->linearize) {
        if (left > self->linlen) return LJ92_ERROR_CORRUPT;
        linear = self->linearize[left];
      } else
        linear = left;
      thisrow[col++] = left;
      out[c++] = linear;
      if (--write == 0) {
        out += self->skiplen;
        write = self->writelen;
      }
    }
    temprow = lastrow;
    lastrow = thisrow;
    thisrow = temprow;
    if (self->ix >= self->datalen) break;
  }
  if (c >= pixels) ret = LJ92_ERROR_NONE;
  return ret;
}
#endif

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

static int parseScanPred1MonoFast(ljp* self) {
  u16* out = self->image;
  u16* thisrow = self->outrow[0];
  u16* lastrow = self->outrow[1];
  int huff_idx = 0;

  if (self->num_huff_idx < 1) {
    return LJ92_ERROR_CORRUPT;
  }
  if (!self->diffcache) {
    return LJ92_ERROR_NO_MEMORY;
  }

  for (int row = 0; row < self->y; row++) {
    int errcode = LJ92_ERROR_NONE;
    int diff;
    int first_px = (row == 0) ? (1 << (self->bits - 1)) : (int)lastrow[0];

    // First pixel in row.
    diff = nextdiff(self, huff_idx, first_px, &errcode);
    if (errcode != LJ92_ERROR_NONE) {
      return errcode;
    }
    self->diffcache[0] = (u16)diff;

    // Remaining pixels use left predictor (Px = previous reconstructed sample).
    for (int col = 1; col < self->x; col++) {
      diff = nextdiff(self, huff_idx, 0, &errcode);
      if (errcode != LJ92_ERROR_NONE) {
        return errcode;
      }
      self->diffcache[col] = (u16)diff;
    }

    {
      u16 seed = (u16)first_px;
      tdng_prefix_sum_u16(thisrow, self->diffcache, self->x, seed);
    }

    if (self->linearize) {
      for (int col = 0; col < self->x; col++) {
        if (thisrow[col] > self->linlen) {
          return LJ92_ERROR_CORRUPT;
        }
        out[col] = self->linearize[thisrow[col]];
      }
    } else {
      memcpy(out, thisrow, (size_t)self->x * sizeof(u16));
    }

    {
      u16* temprow = lastrow;
      lastrow = thisrow;
      thisrow = temprow;
    }
    out += self->x + self->skiplen;
  }

  return LJ92_ERROR_NONE;
}

static int parseScan(ljp* self) {
  int ret = LJ92_ERROR_CORRUPT;
  memset(self->sssshist, 0, sizeof(self->sssshist));
  self->ix = self->scanstart;
  int compcount = self->data[self->ix + 2];
  TINY_DNG_DPRINTF("comp count = %d\n", compcount);
  int pred = self->data[self->ix + 3 + 2 * compcount];
  TINY_DNG_DPRINTF("predicator %d\n", pred);

  if (pred < 0 || pred > 7) return ret;

  // Disable until parsePred6() consideres self->components.
  // if (pred == 6) return parsePred6(self);  // Fast path

  // TINY_DNG_DPRINTF("pref = %d\n", pred);
  self->ix += BEH(self->data[self->ix]);
  self->cnt = 0;
  self->b = 0;

  // Hot path for common grayscale/Bayer LJPEG: predictor-1.
  if ((pred == 1) && (self->components == 1)) {
    return parseScanPred1MonoFast(self);
  }

  // int write = self->writelen;
  // Now need to decode huffman coded values
  // int c = 0;
  // int pixels = self->y * self->x * self->components;
  u16* out = self->image;
  u16* thisrow = self->outrow[0];
  u16* lastrow = self->outrow[1];

  // First pixel predicted from base value
  int diff;
  int Px = 0;
  // int col = 0;
  // int row = 0;
  int left = 0;
  // TINY_DNG_DPRINTF("w = %d, h = %d, components = %d, skiplen = %d\n",
  // self->x,
  // self->y,
  //       self->components, self->skiplen);
  for (int row = 0; row < self->y; row++) {
    // TINY_DNG_DPRINTF("row = %d / %d\n", row, self->y);
    // TINY_DNG_DPRINTF("thisrow %p, lastrow %p\n", thisrow, lastrow);
    for (int col = 0; col < self->x; col++) {
      int colx = col * self->components;

      //
      // NOTE: pixel data is stored in interleaved manner(RGBRGBRGB...)
      //
      for (int c = 0; c < self->components; c++) {
        // TINY_DNG_DPRINTF("c = %d, col = %d, row = %d\n", c, col, row);
        if ((col == 0) && (row == 0)) {
          Px = 1 << (self->bits - 1);
        } else if (row == 0) {
          // Px = left;
          if (col > 0) {
            // ok
          } else {
            //TINY_DNG_ASSERT(col > 0, "Unexpected col.");
            return LJ92_ERROR_CORRUPT;
          }
          Px = thisrow[(col - 1) * self->components + c];
        } else if (col == 0) {
          Px = lastrow[c];  // Use value above for first pixel in row
        } else {
          int prev_colx = (col - 1) * self->components;

          // previous pixel
          left = thisrow[prev_colx + c];

          // TINY_DNG_DPRINTF("pred = %d\n", pred);
          switch (pred) {
            case 0:
              Px = 0;
              break;  // No prediction... should not be used
            case 1:
              Px = thisrow[prev_colx + c];
              break;
            case 2:
              Px = lastrow[colx + c];
              break;
            case 3:
              Px = lastrow[prev_colx + c];
              break;
            case 4:
              Px = left + lastrow[colx + c] - lastrow[prev_colx + c];
              break;
            case 5:
              Px = left + ((lastrow[colx + c] - lastrow[prev_colx + c]) >> 1);
              break;
            case 6:
              Px = lastrow[colx + c] + ((left - lastrow[prev_colx + c]) >> 1);
              break;
            case 7:
              Px = (left + lastrow[colx + c]) >> 1;
              // printf("Px = %d, left = %d, lastrow[colx + c] = %d\n", Px,
              // left, lastrow[colx + c]);
              break;
          }
        }

        int huff_idx = c;
        if (c >= self->num_huff_idx) {
          // Invalid huffman table index.
          // Currently we assume # of huffman tables is 1.
          TINY_DNG_CHECK_AND_RETURN_C(self->num_huff_idx == 1, LJ92_ERROR_CORRUPT);
          huff_idx = 0;  // Look up the first huffman table.
        }

        int errcode = LJ92_ERROR_NONE;
        diff = nextdiff(self, huff_idx, Px, &errcode);
        if (errcode != LJ92_ERROR_NONE) {
          return errcode;
        }

        left = Px + diff;

        // issue https://github.com/syoyo/tinydng/issues/37
        // The spec says the prediction(left) is calculated by adding the difference, then take a modulo(2^16)
        // (`left` could be negative or 65536+ before taking a modulo)
        left = (u16)(left % 65536);

        // TINY_DNG_DPRINTF("row[%d] col[%d] c[%d] Px = %d, diff = %d, left =
        // %d\n", row, col, c, Px, diff, left);
        // Apple ProRAW gives -1 for `left`(=65535?), so uncommented negative
        // left value check.
        // TINY_DNG_ASSERT(left >= 0 && left < (1 << self->bits),
        //                "Error huffman decoding.");
        // TINY_DNG_DPRINTF("pix = %d\n", left);
        // TINY_DNG_DPRINTF("%d %d %d\n",c,diff,left);
        int linear;  // TODO: use u16?
        if (self->linearize) {
          if (left > self->linlen) return LJ92_ERROR_CORRUPT;
          linear = self->linearize[(u16)left];
        } else {
          linear = left;
        }

        // TINY_DNG_DPRINTF("linear = %d\n", linear);
        thisrow[colx + c] = left;
        out[colx + c] = linear;
      }  // c
    }    // col

    // Swap pointers for input and working row buffer
    u16* temprow = lastrow;
    lastrow = thisrow;
    thisrow = temprow;

    // Advance row of output buffer.
    // NOTE: multiply
    out += self->x * self->components + self->skiplen;
    // TINY_DNG_DPRINTF("out = %p, %p, diff = %lld\n", out, self->image, out -
    // self->image);

  }  // row

  ret = LJ92_ERROR_NONE;

  // TINY_DNG_DPRINTF("out written = %d\n", int(out - self->image));

  // if (++col == self->x) {
  //	col = 0;
  //	row++;
  //}
  // if (--write == 0) {
  //	out += self->skiplen;
  //	write = self->writelen;
  //}
  // if (self->ix >= self->datalen + 2) break;

  // if (c >= pixels) ret = LJ92_ERROR_NONE;
  /*for (int h=0;h<17;h++) {
      TINY_DNG_DPRINTF("ssss:%d=%d
  (%f)\n",h,self->sssshist[h],(float)self->sssshist[h]/(float)(pixels));
  }*/
  return ret;
}

static int parseImage(ljp* self) {
  // TINY_DNG_DPRINTF("parseImage\n");
  int ret = LJ92_ERROR_NONE;
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
      ret = LJ92_ERROR_NONE;
      break;
    } else if (nextMarker == -1) {
      ret = LJ92_ERROR_CORRUPT;
      break;
    } else
      ret = parseBlock(self, nextMarker);
    if (ret != LJ92_ERROR_NONE) break;
  }
  return ret;
}

static int findSoI(ljp* self) {
  int ret = LJ92_ERROR_CORRUPT;
  if (find(self) == 0xd8) {
    ret = parseImage(self);
  } else {
    TINY_DNG_DPRINTF("findSoI: corrupt\n");
  }
  return ret;
}

static void free_memory(ljp* self) {
#ifdef SLOW_HUFF
  free(self->maxcode);
  self->maxcode = NULL;
  free(self->mincode);
  self->mincode = NULL;
  free(self->valptr);
  self->valptr = NULL;
  free(self->huffval);
  self->huffval = NULL;
  free(self->huffsize);
  self->huffsize = NULL;
  free(self->huffcode);
  self->huffcode = NULL;
#else
  for (int i = 0; i < self->num_huff_idx; i++) {
    free(self->hufflut[i]);
    self->hufflut[i] = NULL;
  }
#endif
  free(self->rowcache);
  self->rowcache = NULL;
  free(self->diffcache);
  self->diffcache = NULL;
}

int lj92_open(lj92* lj, const uint8_t* data, int datalen, int* width,
              int* height, int* bitdepth, int* components) {
  ljp* self = (ljp*)calloc(sizeof(ljp), 1);
  if (self == NULL) return LJ92_ERROR_NO_MEMORY;

  self->data = (u8*)data;
  self->dataend = self->data + datalen;
  self->datalen = datalen;
  self->num_huff_idx = 0;

  int ret = findSoI(self);

  if (ret == LJ92_ERROR_NONE) {
    u16* rowcache = (u16*)calloc(self->x * self->components * 2, sizeof(u16));
    u16* diffcache = NULL;
    if (rowcache == NULL)
      ret = LJ92_ERROR_NO_MEMORY;
    else {
      diffcache = (u16*)calloc((size_t)self->x * (size_t)self->components,
                               sizeof(u16));
      if (diffcache == NULL) {
        free(rowcache);
        ret = LJ92_ERROR_NO_MEMORY;
      }
    }
    if (ret == LJ92_ERROR_NONE) {
      self->rowcache = rowcache;
      self->diffcache = diffcache;
      self->outrow[0] = rowcache;
      self->outrow[1] = &rowcache[self->x * self->components];
    }
  }

  if (ret != LJ92_ERROR_NONE) {  // Failed, clean up
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

int lj92_decode(lj92 lj, uint16_t* target, int writeLength, int skipLength,
                uint16_t* linearize, int linearizeLength) {
  int ret = LJ92_ERROR_NONE;
  ljp* self = lj;
  if (self == NULL) return LJ92_ERROR_BAD_HANDLE;
  self->image = target;
  self->writelen = writeLength;
  self->skiplen = skipLength;
  self->linearize = linearize;
  self->linlen = linearizeLength;
  ret = parseScan(self);
  return ret;
}

void lj92_close(lj92 lj) {
  ljp* self = lj;
  if (self != NULL) free_memory(self);
  free(self);
}

#if 1  // enabled in v2 for shared encode/decode core.
// Fix of https://github.com/ilia3101/MLV-App/pull/151/files is not reflected here fully.
/* Encoder implementation */

// Very simple count leading zero implementation.
static int clz32(unsigned int x) {
  int n;
  if (x == 0) return 32;
  for (n = 0; ((x & 0x80000000) == 0); n++, x <<= 1)
    ;
  return n;
}

typedef struct _lje {
  uint16_t* image;
  int width;
  int height;
  int bitdepth;
  int components;
  int readLength;
  int skipLength;
  uint16_t* delinearize;
  int delinearizeLength;
  uint8_t* encoded;
  int encodedWritten;
  int encodedLength;
  int hist[18];  // SSSS frequency histogram
  int bits[18];
  int huffval[18];
  u16 huffenc[18];
  u16 huffbits[18];
  int huffsym[18];
} lje;

int frequencyScan(lje* self) {
  // Scan through the tile using the standard type 6 prediction
  // Need to cache the previous 2 row in target coordinates because of tiling
  uint16_t* pixel = self->image;
  int pixcount = self->width * self->height;
  int scan = self->readLength;
  uint16_t* rowcache = (uint16_t*)calloc(1, self->width * self->components * 4);
  uint16_t* rows[2];
  rows[0] = rowcache;
  rows[1] = &rowcache[self->width * self->components];

  int col = 0;
  int row = 0;
  int Px = 0;
  int32_t diff = 0;
  int maxval = (1 << self->bitdepth);

  // TODO: consider self->components
  while (pixcount--) {
    uint16_t p = *pixel;
    if (self->delinearize) {
      if (p >= self->delinearizeLength) {
        free(rowcache);
        return LJ92_ERROR_TOO_WIDE;
      }
      p = self->delinearize[p];
    }
    if (p >= maxval) {
      free(rowcache);
      return LJ92_ERROR_TOO_WIDE;
    }
    rows[1][col] = p;

    if ((row == 0) && (col == 0))
      Px = 1 << (self->bitdepth - 1);
    else if (row == 0)
      Px = rows[1][col - 1];
    else if (col == 0)
      Px = rows[0][col];
    else
      Px = rows[0][col] + ((rows[1][col - 1] - rows[0][col - 1]) >> 1);
    diff = rows[1][col] - Px;
    diff = diff%65536;
    // int ssss = 32 - __builtin_clz(abs(diff));
    int ssss = 32 - clz32(abs(diff));
    if (diff == 0) ssss = 0;
    self->hist[ssss]++;
    // TINY_DNG_DPRINTF("%d %d %d %d %d %d\n",col,row,p,Px,diff,ssss);
    pixel++;
    scan--;
    col++;
    if (scan == 0) {
      pixel += self->skipLength;
      scan = self->readLength;
    }
    if (col == self->width) {
      uint16_t* tmprow = rows[1];
      rows[1] = rows[0];
      rows[0] = tmprow;
      col = 0;
      row++;
    }
  }
#ifdef LJ92_DEBUG
  int sort[17];
  for (int h = 0; h < 17; h++) {
    sort[h] = h;
    TINY_DNG_DPRINTF("%d:%d\n", h, self->hist[h]);
  }
#endif
  free(rowcache);
  return LJ92_ERROR_NONE;
}

void createEncodeTable(lje* self) {
  float freq[18];
  int codesize[18];
  int others[18];

  // Calculate frequencies
  float totalpixels = self->width * self->height;
  for (int i = 0; i < 17; i++) {
    freq[i] = (float)(self->hist[i]) / totalpixels;
#ifdef LJ92_DEBUG
    TINY_DNG_DPRINTF("%d:%f\n", i, freq[i]);
#endif
    codesize[i] = 0;
    others[i] = -1;
  }
  codesize[17] = 0;
  others[17] = -1;
  freq[17] = 1.0f;

  float v1f, v2f;
  int v1, v2;

  while (1) {
    v1f = 3.0f;
    v1 = -1;
    for (int i = 0; i < 18; i++) {
      if ((freq[i] <= v1f) && (freq[i] > 0.0f)) {
        v1f = freq[i];
        v1 = i;
      }
    }
#ifdef LJ92_DEBUG
    TINY_DNG_DPRINTF("v1:%d,%f\n", v1, v1f);
#endif
    v2f = 3.0f;
    v2 = -1;
    for (int i = 0; i < 18; i++) {
      if (i == v1) continue;
      if ((freq[i] < v2f) && (freq[i] > 0.0f)) {
        v2f = freq[i];
        v2 = i;
      }
    }
    if (v2 == -1) break;  // Done

    freq[v1] += freq[v2];
    freq[v2] = 0.0f;

    while (1) {
      codesize[v1]++;
      if (others[v1] == -1) break;
      v1 = others[v1];
    }
    others[v1] = v2;
    while (1) {
      codesize[v2]++;
      if (others[v2] == -1) break;
      v2 = others[v2];
    }
  }
  int* bits = self->bits;
  memset(bits, 0, sizeof(self->bits));
  for (int i = 0; i < 18; i++) {
    if (codesize[i] != 0) {
      bits[codesize[i]]++;
    }
  }
#ifdef LJ92_DEBUG
  for (int i = 0; i < 17; i++) {
    TINY_DNG_DPRINTF("bits:%d,%d,%d\n", i, bits[i], codesize[i]);
  }
#endif
  int* huffval = self->huffval;
  int i = 1;
  int k = 0;
  int j;
  memset(huffval, 0, sizeof(self->huffval));
  while (i <= 32) {
    j = 0;
    while (j < 17) {
      if (codesize[j] == i) {
        huffval[k++] = j;
      }
      j++;
    }
    i++;
  }
#ifdef LJ92_DEBUG
  for (i = 0; i < 17; i++) {
    TINY_DNG_DPRINTF("i=%d,huffval[i]=%x\n", i, huffval[i]);
  }
#endif
  int maxbits = 16;
  while (maxbits > 0) {
    if (bits[maxbits]) break;
    maxbits--;
  }
  u16* huffenc = self->huffenc;
  u16* huffbits = self->huffbits;
  int* huffsym = self->huffsym;
  memset(huffenc, 0, sizeof(self->huffenc));
  memset(huffbits, 0, sizeof(self->huffbits));
  memset(self->huffsym, 0, sizeof(self->huffsym));
  i = 0;
  int hv = 0;
  int rv = 0;
  int vl = 0;  // i
  // int hcode;
  int bitsused = 1;
  int sym = 0;
  // TINY_DNG_DPRINTF("%04x:%x:%d:%x\n",i,huffvals[hv],bitsused,1<<(maxbits-bitsused));
  while (i < 1 << maxbits) {
    if (bitsused > maxbits) {
      break;  // Done. Should never get here!
    }
    if (vl >= bits[bitsused]) {
      bitsused++;
      vl = 0;
      continue;
    }
    if (rv == 1 << (maxbits - bitsused)) {
      rv = 0;
      vl++;
      hv++;
      // TINY_DNG_DPRINTF("%04x:%x:%d:%x\n",i,huffvals[hv],bitsused,1<<(maxbits-bitsused));
      continue;
    }
    huffbits[sym] = bitsused;
    huffenc[sym++] = i >> (maxbits - bitsused);
    // TINY_DNG_DPRINTF("%d %d %d\n",i,bitsused,hcode);
    i += (1 << (maxbits - bitsused));
    rv = 1 << (maxbits - bitsused);
  }
  for (i = 0; i < 17; i++) {
    if (huffbits[i] > 0) {
      huffsym[huffval[i]] = i;
    }
#ifdef LJ92_DEBUG
    TINY_DNG_DPRINTF("huffval[%d]=%d,huffenc[%d]=%x,bits=%d\n", i, huffval[i],
                     i, huffenc[i], huffbits[i]);
#endif
    if (huffbits[i] > 0) {
      huffsym[huffval[i]] = i;
    }
  }
#ifdef LJ92_DEBUG
  for (i = 0; i < 17; i++) {
    TINY_DNG_DPRINTF("huffsym[%d]=%d\n", i, huffsym[i]);
  }
#endif
}

// TODO: Support components of 2>
void writeHeader(lje* self) {
  int w = self->encodedWritten;
  uint8_t* e = self->encoded;
  e[w++] = 0xff;
  e[w++] = 0xd8;  // SOI
  e[w++] = 0xff;
  e[w++] = 0xc3;  // SOF3
  // Write SOF
  e[w++] = 0x0;
  e[w++] = 11;  // Lf, frame header length
  e[w++] = self->bitdepth;
  e[w++] = self->height >> 8;
  e[w++] = self->height & 0xFF;
  e[w++] = self->width >> 8;
  e[w++] = self->width & 0xFF;
  e[w++] = 1;     // Components
  e[w++] = 0;     // Component ID
  e[w++] = 0x11;  // Component X/Y
  e[w++] = 0;     // Unused (Quantisation)
  e[w++] = 0xff;
  e[w++] = 0xc4;  // HUFF
  // Write HUFF
  int count = 0;
  for (int i = 0; i < 17; i++) {
    count += self->bits[i];
  }
  e[w++] = 0x0;
  e[w++] = 17 + 2 + count;  // Lf, frame header length
  e[w++] = 0;               // Table ID
  for (int i = 1; i < 17; i++) {
    e[w++] = self->bits[i];
  }
  for (int i = 0; i < count; i++) {
    e[w++] = self->huffval[i];
  }
  e[w++] = 0xff;
  e[w++] = 0xda;  // SCAN
  // Write SCAN
  e[w++] = 0x0;
  e[w++] = 8;  // Ls, scan header length
  e[w++] = 1;  // Components
  e[w++] = 0;  //
  e[w++] = 0;  //
  e[w++] = 6;  // Predictor
  e[w++] = 0;  //
  e[w++] = 0;  //
  self->encodedWritten = w;
}

void writePost(lje* self) {
  int w = self->encodedWritten;
  uint8_t* e = self->encoded;
  e[w++] = 0xff;
  e[w++] = 0xd9;  // EOI
  self->encodedWritten = w;
}

void writeBody(lje* self) {
  // Scan through the tile using the standard type 6 prediction
  // Need to cache the previous 2 row in target coordinates because of tiling
  uint16_t* pixel = self->image;
  int pixcount = self->width * self->height;
  int scan = self->readLength;
  uint16_t* rowcache = (uint16_t*)calloc(1, self->width * self->components * 4);
  uint16_t* rows[2];
  rows[0] = rowcache;
  rows[1] = &rowcache[self->width];

  int col = 0;
  int row = 0;
  int Px = 0;
  int32_t diff = 0;
  int bitcount = 0;
  uint8_t* out = self->encoded;
  int w = self->encodedWritten;
  uint8_t next = 0;
  uint8_t nextbits = 8;
  while (pixcount--) {
    uint16_t p = *pixel;
    if (self->delinearize) p = self->delinearize[p];
    rows[1][col] = p;

    if ((row == 0) && (col == 0))
      Px = 1 << (self->bitdepth - 1);
    else if (row == 0)
      Px = rows[1][col - 1];
    else if (col == 0)
      Px = rows[0][col];
    else
      Px = rows[0][col] + ((rows[1][col - 1] - rows[0][col - 1]) >> 1);
    diff = rows[1][col] - Px;
    // int ssss = 32 - __builtin_clz(abs(diff));
    int ssss = 32 - clz32(abs(diff));
    if (diff == 0) ssss = 0;
    // TINY_DNG_DPRINTF("%d %d %d %d %d\n",col,row,Px,diff,ssss);

    // Write the huffman code for the ssss value
    int huffcode = self->huffsym[ssss];
    int huffenc = self->huffenc[huffcode];
    int huffbits = self->huffbits[huffcode];
    bitcount += huffbits + ssss;

    int vt = ssss > 0 ? (1 << (ssss - 1)) : 0;
// TINY_DNG_DPRINTF("%d %d %d %d\n",rows[1][col],Px,diff,Px+diff);
#ifdef LJ92_DEBUG
#endif
    if (diff < vt) diff += (1 << (ssss)) - 1;

    // Write the ssss
    while (huffbits > 0) {
      int usebits = huffbits > nextbits ? nextbits : huffbits;
      // Add top usebits from huffval to next usebits of nextbits
      int tophuff = huffenc >> (huffbits - usebits);
      next |= (tophuff << (nextbits - usebits));
      nextbits -= usebits;
      huffbits -= usebits;
      huffenc &= (1 << huffbits) - 1;
      if (nextbits == 0) {
        out[w++] = next;
        if (next == 0xff) out[w++] = 0x0;
        next = 0;
        nextbits = 8;
      }
    }
    // Write the rest of the bits for the value

    while (ssss > 0) {
      int usebits = ssss > nextbits ? nextbits : ssss;
      // Add top usebits from huffval to next usebits of nextbits
      int tophuff = diff >> (ssss - usebits);
      next |= (tophuff << (nextbits - usebits));
      nextbits -= usebits;
      ssss -= usebits;
      diff &= (1 << ssss) - 1;
      if (nextbits == 0) {
        out[w++] = next;
        if (next == 0xff) out[w++] = 0x0;
        next = 0;
        nextbits = 8;
      }
    }

    // TINY_DNG_DPRINTF("%d %d\n",diff,ssss);
    pixel++;
    scan--;
    col++;
    if (scan == 0) {
      pixel += self->skipLength;
      scan = self->readLength;
    }
    if (col == self->width) {
      uint16_t* tmprow = rows[1];
      rows[1] = rows[0];
      rows[0] = tmprow;
      col = 0;
      row++;
    }
  }
  // Flush the final bits
  if (nextbits < 8) {
    out[w++] = next;
    if (next == 0xff) out[w++] = 0x0;
  }
#ifdef LJ92_DEBUG
  int sort[17];
  for (int h = 0; h < 17; h++) {
    sort[h] = h;
    TINY_DNG_DPRINTF("%d:%d\n", h, self->hist[h]);
  }
  TINY_DNG_DPRINTF("Total bytes: %d\n", bitcount >> 3);
#endif
  free(rowcache);
  self->encodedWritten = w;
}

/* Encoder
 * Read tile from an image and encode in one shot
 * Return the encoded data
 */
int lj92_encode(uint16_t* image, int width, int height, int bitdepth,
                int readLength, int skipLength, uint16_t* delinearize,
                int delinearizeLength, uint8_t** encoded, int* encodedLength) {
  int ret = LJ92_ERROR_NONE;

  lje* self = (lje*)calloc(sizeof(lje), 1);
  if (self == NULL) return LJ92_ERROR_NO_MEMORY;
  self->image = image;
  self->width = width;
  self->height = height;
  self->bitdepth = bitdepth;
  self->readLength = readLength;
  self->skipLength = skipLength;
  self->delinearize = delinearize;
  self->delinearizeLength = delinearizeLength;
  self->encodedLength = width * height * 3 + 200;
  self->encoded = (uint8_t*)malloc(self->encodedLength);
  if (self->encoded == NULL) {
    free(self);
    return LJ92_ERROR_NO_MEMORY;
  }
  // Scan through data to gather frequencies of ssss prefixes
  ret = frequencyScan(self);
  if (ret != LJ92_ERROR_NONE) {
    free(self->encoded);
    free(self);
    return ret;
  }
  // Create encoded table based on frequencies
  createEncodeTable(self);
  // Write JPEG head and scan header
  writeHeader(self);
  // Scan through and do the compression
  writeBody(self);
  // Finish
  writePost(self);
#ifdef LJ92_DEBUG
  TINY_DNG_DPRINTF("written:%d\n", self->encodedWritten);
#endif
  self->encoded = (uint8_t*)realloc(self->encoded, self->encodedWritten);
  self->encodedLength = self->encodedWritten;
  *encoded = self->encoded;
  *encodedLength = self->encodedLength;

  free(self);

  return ret;
}
#endif

// End liblj92 ---------------------------------------------------------
