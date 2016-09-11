//
// TinyDNGLoadr, single header only DNG loader.
//

/*
The MIT License (MIT)

Copyright (c) 2016 Syoyo Fujita.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/
#ifndef TINY_DNG_LOADER_H_
#define TINY_DNG_LOADER_H_

// @note {
// https://www.adobe.com/content/dam/Adobe/en/products/photoshop/pdfs/dng_spec_1.4.0.0.pdf
// }

#include <string>
#include <vector>

namespace tinydng {

typedef enum {
  LIGHTSOURCE_UNKNOWN = 0,
  LIGHTSOURCE_DAYLIGHT = 1,
  LIGHTSOURCE_FLUORESCENT = 2,
  LIGHTSOURCE_TUNGSTEN = 3,
  LIGHTSOURCE_FLASH = 4,
  LIGHTSOURCE_FINE_WEATHER = 9,
  LIGHTSOURCE_CLOUDY_WEATHER = 10,
  LIGHTSOURCE_SHADE = 11,
  LIGHTSOURCE_DAYLIGHT_FLUORESCENT = 12,
  LIGHTSOURCE_DAY_WHITE_FLUORESCENT = 13,
  LIGHTSOURCE_COOL_WHITE_FLUORESCENT = 14,
  LIGHTSOURCE_WHITE_FLUORESCENT = 15,
  LIGHTSOURCE_STANDARD_LIGHT_A = 17,
  LIGHTSOURCE_STANDARD_LIGHT_B = 18,
  LIGHTSOURCE_STANDARD_LIGHT_C = 19,
  LIGHTSOURCE_D55 = 20,
  LIGHTSOURCE_D65 = 21,
  LIGHTSOURCE_D75 = 22,
  LIGHTSOURCE_D50 = 23,
  LIGHTSOURCE_ISO_STUDIO_TUNGSTEN = 24,
  LIGHTSOURCE_OTHER_LIGHT_SOURCE = 255
} LightSource;

struct DNGInfo {
  int black_level;
  int white_level;
  int version;

  int bits_internal;  // BitsPerSample in stored file.

  char cfa_plane_color[4];  // 0:red, 1:green, 2:blue, 3:cyan, 4:magenta,
                            // 5:yellow, 6:white
  int cfa_pattern[2][2];    // @fixme { Support non 2x2 CFA pattern. }
  int cfa_layout;
  int active_area[4];  // top, left, bottom, right

  int tile_width;
  int tile_length;
  unsigned int tile_offset;
  int pad0;

  double color_matrix1[3][3];
  double color_matrix2[3][3];

  double forward_matrix1[3][3];
  double forward_matrix2[3][3];

  double camera_calibration1[3][3];
  double camera_calibration2[3][3];

  LightSource calibration_illuminant1;
  LightSource calibration_illuminant2;
};

// Load DNG image.
// Returns true upon success.
// Returns false upon failure and store error message into `err`.
bool LoadDNG(DNGInfo* info,                     // [out] DNG meta information.
             std::vector<unsigned char>* data,  // [out] DNG image data
             size_t* data_len,                  // [out] DNG image data size
             int* width,                        // [out] DNG image width
             int* height,                       // [out] DNG image height
             int* bits,            // [out] DNG pixel bits(after decoding)
             int* num_components,  // [out] DNG # of components
             std::string* err,     // [out] error message.
             const char* filename,
             bool is_system_big_endian =
                 false);  // Set true if you are running on Big Endian machine.

}  // namespace tinydng

#ifdef TINY_DNG_LOADER_IMPLEMENTATION

#include <cassert>
#include <sstream>

#ifdef TINY_DNG_ENABLE_CHARLS
#include <charls.h>
#endif

namespace tinydng {

// Very simple count leading zero implementation.
static int clz32(unsigned int x) {
  int n;
  if (x == 0) return 32;
  for (n = 0; ((x & 0x80000000) == 0); n++, x <<= 1)
    ;
  return n;
}

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++11-extensions"
#pragma clang diagnostic ignored "-Wold-style-cast"
#pragma clang diagnostic ignored "-Wconversion"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wcast-align"
#pragma clang diagnostic ignored "-Wconditional-uninitialized"
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wpadded"
#pragma clang diagnostic ignored "-Wmissing-prototypes"
#endif

namespace {
// Begin liblj92, Lossless JPEG decode/encoder ------------------------------

/*
lj92.c
(c) Andrew Baldwin 2014

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum LJ92_ERRORS {
  LJ92_ERROR_NONE = 0,
  LJ92_ERROR_CORRUPT = -1,
  LJ92_ERROR_NO_MEMORY = -2,
  LJ92_ERROR_BAD_HANDLE = -3,
  LJ92_ERROR_TOO_WIDE = -4,
};

typedef struct _ljp* lj92;

/* Parse a lossless JPEG (1992) structure returning
 * - a handle that can be used to decode the data
 * - width/height/bitdepth of the data
 * Returns status code.
 * If status == LJ92_ERROR_NONE, handle must be closed with lj92_close
 */
static int lj92_open(lj92* lj,                    // Return handle here
                     uint8_t* data, int datalen,  // The encoded data
                     int* width, int* height,
                     int* bitdepth);  // Width, height and bitdepth

/* Release a decoder object */
static void lj92_close(lj92 lj);

/*
 * Decode previously opened lossless JPEG (1992) into a 2D tile of memory
 * Starting at target, write writeLength 16bit values, then skip 16bit
 * skipLength value before writing again
 * If linearize is not NULL, use table at linearize to convert data values from
 * output value to target value
 * Data is only correct if LJ92_ERROR_NONE is returned
 */
static int lj92_decode(
    lj92 lj, uint16_t* target, int writeLength,
    int skipLength,  // The image is written to target as a tile
    uint16_t* linearize,
    int linearizeLength);  // If not null, linearize the data using this table

/*
 * Encode a grayscale image supplied as 16bit values within the given bitdepth
 * Read from tile in the image
 * Apply delinearization if given
 * Return the encoded lossless JPEG stream
 */
static int lj92_encode(uint16_t* image, int width, int height, int bitdepth,
                       int readLength, int skipLength, uint16_t* delinearize,
                       int delinearizeLength, uint8_t** encoded,
                       int* encodedLength);

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

//#define SLOW_HUFF
//#define LJ92_DEBUG

typedef struct _ljp {
  u8* data;
  u8* dataend;
  int datalen;
  int scanstart;
  int ix;
  int x;           // Width
  int y;           // Height
  int bits;        // Bit depth
  int writelen;    // Write rows this long
  int skiplen;     // Skip this many values after each row
  u16* linearize;  // Linearization table
  int linlen;
  int sssshist[16];

// Huffman table - only one supported, and probably needed
#ifdef SLOW_HUFF
  int* maxcode;
  int* mincode;
  int* valptr;
  u8* huffval;
  int* huffsize;
  int* huffcode;
#else
  u16* hufflut;
  int huffbits;
#endif
  // Parse state
  int cnt;
  u32 b;
  u16* image;
  u16* rowcache;
  u16* outrow[2];
} ljp;

static int find(ljp* self) {
  int ix = self->ix;
  u8* data = self->data;
  while (data[ix] != 0xFF && ix < (self->datalen - 1)) {
    ix += 1;
  }
  ix += 2;
  if (ix >= self->datalen) {
    // printf("idx = %d, datalen = %\d\n", ix, self->datalen);
    return -1;
  }
  self->ix = ix;
  // printf("ix = %d, data = %d\n", ix, data[ix - 1]);
  return data[ix - 1];
}

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
    printf("huffval[%d]=%d\n", hix, huffval[hix]);
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
  self->huffbits = maxbits;
  /* Now fill the lut */
  u16* hufflut = (u16*)malloc((1 << maxbits) * sizeof(u16));
  if (hufflut == NULL) return LJ92_ERROR_NO_MEMORY;
  self->hufflut = hufflut;
  int i = 0;
  int hv = 0;
  int rv = 0;
  int vl = 0;  // i
  int hcode;
  int bitsused = 1;
#ifdef LJ92_DEBUG
  printf("%04x:%x:%d:%x\n", i, huffvals[hv], bitsused,
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
      printf("%04x:%x:%d:%x\n", i, huffvals[hv], bitsused,
             1 << (maxbits - bitsused));
#endif
      continue;
    }
    hcode = huffvals[hv];
    hufflut[i] = hcode << 8 | bitsused;
    // printf("%d %d %d\n",i,bitsused,hcode);
    i++;
    rv++;
  }
  ret = LJ92_ERROR_NONE;
#endif
  return ret;
}

static int parseSof3(ljp* self) {
  if (self->ix + 6 >= self->datalen) return LJ92_ERROR_CORRUPT;
  self->y = BEH(self->data[self->ix + 3]);
  self->x = BEH(self->data[self->ix + 5]);
  self->bits = self->data[self->ix + 2];
  self->ix += BEH(self->data[self->ix]);
  return LJ92_ERROR_NONE;
}

static int parseBlock(ljp* self, int marker) {
  self->ix += BEH(self->data[self->ix]);
  if (self->ix >= self->datalen) return LJ92_ERROR_CORRUPT;
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

inline static int nextdiff(ljp* self, int Px) {
#ifdef SLOW_HUFF
  int t = decode(self);
  int diff = receive(self, t);
  diff = extend(self, diff, t);
// printf("%d %d %d %x\n",Px+diff,Px,diff,t);//,index,usedbits);
#else
  u32 b = self->b;
  int cnt = self->cnt;
  int huffbits = self->huffbits;
  int ix = self->ix;
  int next;
  while (cnt < huffbits) {
    next = *(u16*)&self->data[ix];
    int one = next & 0xFF;
    int two = next >> 8;
    b = (b << 16) | (one << 8) | two;
    cnt += 16;
    ix += 2;
    if (one == 0xFF) {
      // printf("%x %x %x %x %d\n",one,two,b,b>>8,cnt);
      b >>= 8;
      cnt -= 8;
    } else if (two == 0xFF)
      ix++;
  }
  int index = b >> (cnt - huffbits);
  u16 ssssused = self->hufflut[index];
  int usedbits = ssssused & 0xFF;
  int t = ssssused >> 8;
  self->sssshist[t]++;
  cnt -= usedbits;
  int keepbitsmask = (1 << cnt) - 1;
  b &= keepbitsmask;
  while (cnt < t) {
    next = *(u16*)&self->data[ix];
    int one = next & 0xFF;
    int two = next >> 8;
    b = (b << 16) | (one << 8) | two;
    cnt += 16;
    ix += 2;
    if (one == 0xFF) {
      b >>= 8;
      cnt -= 8;
    } else if (two == 0xFF)
      ix++;
  }
  cnt -= t;
  int diff = b >> cnt;
  int vt = 1 << (t - 1);
  if (diff < vt) {
    vt = (-1 << t) + 1;
    diff += vt;
  }
  keepbitsmask = (1 << cnt) - 1;
  self->b = b & keepbitsmask;
  self->cnt = cnt;
  self->ix = ix;
// printf("%d %d\n",t,diff);
// printf("%d %d %d %x %x %d\n",Px+diff,Px,diff,t,index,usedbits);
#ifdef LJ92_DEBUG
#endif
#endif
  return diff;
}

static int parsePred6(ljp* self) {
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

  // First pixel
  diff = nextdiff(self, 0);
  Px = 1 << (self->bits - 1);
  left = Px + diff;
  if (self->linearize)
    linear = self->linearize[left];
  else
    linear = left;
  thisrow[col++] = left;
  out[c++] = linear;
  if (self->ix >= self->datalen) return ret;
  --write;
  int rowcount = self->x - 1;
  while (rowcount--) {
    diff = nextdiff(self, 0);
    Px = left;
    left = Px + diff;
    if (self->linearize)
      linear = self->linearize[left];
    else
      linear = left;
    thisrow[col++] = left;
    out[c++] = linear;
    // printf("%d %d %d %d
    // %x\n",col-1,diff,left,thisrow[col-1],&thisrow[col-1]);
    if (self->ix >= self->datalen) return ret;
    if (--write == 0) {
      out += self->skiplen;
      write = self->writelen;
    }
  }
  temprow = lastrow;
  lastrow = thisrow;
  thisrow = temprow;
  row++;
  // printf("%x %x\n",thisrow,lastrow);
  while (c < pixels) {
    col = 0;
    diff = nextdiff(self, 0);
    Px = lastrow[col];  // Use value above for first pixel in row
    left = Px + diff;
    if (self->linearize) {
      if (left > self->linlen) return LJ92_ERROR_CORRUPT;
      linear = self->linearize[left];
    } else
      linear = left;
    thisrow[col++] = left;
    // printf("%d %d %d %d\n",col,diff,left,lastrow[col]);
    out[c++] = linear;
    if (self->ix >= self->datalen) break;
    rowcount = self->x - 1;
    if (--write == 0) {
      out += self->skiplen;
      write = self->writelen;
    }
    while (rowcount--) {
      diff = nextdiff(self, 0);
      Px = lastrow[col] + ((left - lastrow[col - 1]) >> 1);
      left = Px + diff;
      // printf("%d %d %d %d %d
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

static int parseScan(ljp* self) {
  int ret = LJ92_ERROR_CORRUPT;
  memset(self->sssshist, 0, sizeof(self->sssshist));
  self->ix = self->scanstart;
  int compcount = self->data[self->ix + 2];
  int pred = self->data[self->ix + 3 + 2 * compcount];
  if (pred < 0 || pred > 7) return ret;
  if (pred == 6) return parsePred6(self);  // Fast path
  self->ix += BEH(self->data[self->ix]);
  self->cnt = 0;
  self->b = 0;
  int write = self->writelen;
  // Now need to decode huffman coded values
  int c = 0;
  int pixels = self->y * self->x;
  u16* out = self->image;
  u16* thisrow = self->outrow[0];
  u16* lastrow = self->outrow[1];

  // First pixel predicted from base value
  int diff;
  int Px;
  int col = 0;
  int row = 0;
  int left = 0;
  while (c < pixels) {
    if ((col == 0) && (row == 0)) {
      Px = 1 << (self->bits - 1);
    } else if (row == 0) {
      Px = left;
    } else if (col == 0) {
      Px = lastrow[col];  // Use value above for first pixel in row
    } else {
      switch (pred) {
        case 0:
          Px = 0;
          break;  // No prediction... should not be used
        case 1:
          Px = left;
          break;
        case 2:
          Px = lastrow[col];
          break;
        case 3:
          Px = lastrow[col - 1];
          break;
        case 4:
          Px = left + lastrow[col] - lastrow[col - 1];
          break;
        case 5:
          Px = left + ((lastrow[col] - lastrow[col - 1]) >> 1);
          break;
        case 6:
          Px = lastrow[col] + ((left - lastrow[col - 1]) >> 1);
          break;
        case 7:
          Px = (left + lastrow[col]) >> 1;
          break;
      }
    }
    diff = nextdiff(self, Px);
    left = Px + diff;
    // printf("%d %d %d\n",c,diff,left);
    int linear;
    if (self->linearize) {
      if (left > self->linlen) return LJ92_ERROR_CORRUPT;
      linear = self->linearize[left];
    } else
      linear = left;
    thisrow[col] = left;
    out[c++] = linear;
    if (++col == self->x) {
      col = 0;
      row++;
      u16* temprow = lastrow;
      lastrow = thisrow;
      thisrow = temprow;
    }
    if (--write == 0) {
      out += self->skiplen;
      write = self->writelen;
    }
    if (self->ix >= self->datalen + 2) break;
  }
  if (c >= pixels) ret = LJ92_ERROR_NONE;
  /*for (int h=0;h<17;h++) {
      printf("ssss:%d=%d
  (%f)\n",h,self->sssshist[h],(float)self->sssshist[h]/(float)(pixels));
  }*/
  return ret;
}

static int parseImage(ljp* self) {
  // printf("parseImage\n");
  int ret = LJ92_ERROR_NONE;
  while (1) {
    int nextMarker = find(self);
    // printf("marker = %f\n", nextMarker);
    if (nextMarker == 0xc4)
      ret = parseHuff(self);
    else if (nextMarker == 0xc3)
      ret = parseSof3(self);
    else if (nextMarker == 0xfe)  // Comment
      ret = parseBlock(self, nextMarker);
    else if (nextMarker == 0xd9)  // End of image
      break;
    else if (nextMarker == 0xda) {
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
  if (find(self) == 0xd8) ret = parseImage(self);
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
  free(self->hufflut);
  self->hufflut = NULL;
#endif
  free(self->rowcache);
  self->rowcache = NULL;
}

int lj92_open(lj92* lj, const uint8_t* data, int datalen, int* width,
              int* height, int* bitdepth) {
  ljp* self = (ljp*)calloc(sizeof(ljp), 1);
  if (self == NULL) return LJ92_ERROR_NO_MEMORY;

  self->data = (u8*)data;
  self->dataend = self->data + datalen;
  self->datalen = datalen;

  int ret = findSoI(self);

  if (ret == LJ92_ERROR_NONE) {
    u16* rowcache = (u16*)calloc(self->x * 2, sizeof(u16));
    if (rowcache == NULL)
      ret = LJ92_ERROR_NO_MEMORY;
    else {
      self->rowcache = rowcache;
      self->outrow[0] = rowcache;
      self->outrow[1] = &rowcache[self->x];
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

/* Encoder implementation */

typedef struct _lje {
  uint16_t* image;
  int width;
  int height;
  int bitdepth;
  int readLength;
  int skipLength;
  uint16_t* delinearize;
  int delinearizeLength;
  uint8_t* encoded;
  int encodedWritten;
  int encodedLength;
  int hist[17];  // SSSS frequency histogram
  int bits[17];
  int huffval[17];
  u16 huffenc[17];
  u16 huffbits[17];
  int huffsym[17];
} lje;

int frequencyScan(lje* self) {
  // Scan through the tile using the standard type 6 prediction
  // Need to cache the previous 2 row in target coordinates because of tiling
  uint16_t* pixel = self->image;
  int pixcount = self->width * self->height;
  int scan = self->readLength;
  uint16_t* rowcache = (uint16_t*)calloc(1, self->width * 4);
  uint16_t* rows[2];
  rows[0] = rowcache;
  rows[1] = &rowcache[self->width];

  int col = 0;
  int row = 0;
  int Px = 0;
  int32_t diff = 0;
  int maxval = (1 << self->bitdepth);
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
    // int ssss = 32 - __builtin_clz(abs(diff));
    int ssss = 32 - clz32(abs(diff));
    if (diff == 0) ssss = 0;
    self->hist[ssss]++;
    // printf("%d %d %d %d %d %d\n",col,row,p,Px,diff,ssss);
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
    printf("%d:%d\n", h, self->hist[h]);
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
    printf("%d:%f\n", i, freq[i]);
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
    printf("v1:%d,%f\n", v1, v1f);
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
    printf("bits:%d,%d,%d\n", i, bits[i], codesize[i]);
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
    printf("i=%d,huffval[i]=%x\n", i, huffval[i]);
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
  // printf("%04x:%x:%d:%x\n",i,huffvals[hv],bitsused,1<<(maxbits-bitsused));
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
      // printf("%04x:%x:%d:%x\n",i,huffvals[hv],bitsused,1<<(maxbits-bitsused));
      continue;
    }
    huffbits[sym] = bitsused;
    huffenc[sym++] = i >> (maxbits - bitsused);
    // printf("%d %d %d\n",i,bitsused,hcode);
    i += (1 << (maxbits - bitsused));
    rv = 1 << (maxbits - bitsused);
  }
  for (i = 0; i < 17; i++) {
    if (huffbits[i] > 0) {
      huffsym[huffval[i]] = i;
    }
#ifdef LJ92_DEBUG
    printf("huffval[%d]=%d,huffenc[%d]=%x,bits=%d\n", i, huffval[i], i,
           huffenc[i], huffbits[i]);
#endif
    if (huffbits[i] > 0) {
      huffsym[huffval[i]] = i;
    }
  }
#ifdef LJ92_DEBUG
  for (i = 0; i < 17; i++) {
    printf("huffsym[%d]=%d\n", i, huffsym[i]);
  }
#endif
}

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
  uint16_t* rowcache = (uint16_t*)calloc(1, self->width * 4);
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
    // printf("%d %d %d %d %d\n",col,row,Px,diff,ssss);

    // Write the huffman code for the ssss value
    int huffcode = self->huffsym[ssss];
    int huffenc = self->huffenc[huffcode];
    int huffbits = self->huffbits[huffcode];
    bitcount += huffbits + ssss;

    int vt = ssss > 0 ? (1 << (ssss - 1)) : 0;
// printf("%d %d %d %d\n",rows[1][col],Px,diff,Px+diff);
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

    // printf("%d %d\n",diff,ssss);
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
    printf("%d:%d\n", h, self->hist[h]);
  }
  printf("Total bytes: %d\n", bitcount >> 3);
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
  printf("written:%d\n", self->encodedWritten);
#endif
  self->encoded = (uint8_t*)realloc(self->encoded, self->encodedWritten);
  self->encodedLength = self->encodedWritten;
  *encoded = self->encoded;
  *encodedLength = self->encodedLength;

  free(self);

  return ret;
}

// End liblj92 ---------------------------------------------------------
}  // namespace
#ifdef __clang__
#pragma clang diagnostic pop
#endif

typedef enum {
  TAG_IMAGE_WIDTH = 256,
  TAG_IMAGE_HEIGHT = 257,
  TAG_BITS_PER_SAMPLE = 258,
  TAG_COMPRESSION = 259,
  TAG_STRIP_OFFSET = 273,
  TAG_ORIENTATION = 274,
  TAG_STRIP_BYTE_COUNTS = 279,
  TAG_SUB_IFDS = 330,
  TAG_TILE_WIDTH = 322,
  TAG_TILE_LENGTH = 323,
  TAG_TILE_OFFSETS = 324,
  TAG_TILE_BYTE_COUNTS = 325,
  TAG_CFA_PATTERN = 33422,
  TAG_CFA_PLANE_COLOR = 50710,
  TAG_CFA_LAYOUT = 50711,
  TAG_BLACK_LEVEL = 50714,
  TAG_WHITE_LEVEL = 50717,
  TAG_COLOR_MATRIX1 = 50721,
  TAG_COLOR_MATRIX2 = 50722,
  TAG_CAMERA_CALIBRATION1 = 50723,
  TAG_CAMERA_CALIBRATION2 = 50724,
  TAG_DNG_VERSION = 50706,
  TAG_CALIBRATION_ILLUMINANT1 = 50778,
  TAG_CALIBRATION_ILLUMINANT2 = 50779,
  TAG_ACTIVE_AREA = 50829,
  TAG_FORWARD_MATRIX1 = 50964,
  TAG_FORWARD_MATRIX2 = 50965,

  TAG_INVALID = 65535
} TiffTag;

typedef struct {
  int width;
  int height;
  int bps;
  int compression;
  unsigned int offset;
  int orientation;
  int samples;
  int bytes;
  int strip_byte_count;
  int active_area[4];  // top, left, bottom, right
  bool has_active_area;
  unsigned char pad[3];

  char cfa_plane_color[4];  // 0:red, 1:green, 2:blue, 3:cyan, 4:magenta,
                            // 5:yellow, 6:white
  int cfa_pattern[2][2];    // @fixme { Support non 2x2 CFA pattern. }
  int cfa_layout;

  int tile_width;
  int tile_length;
  unsigned int tile_offset;
  unsigned int tile_byte_count;  // (compressed) size
} TIFFInfo;

static void swap2(unsigned short* val) {
  unsigned short tmp = *val;
  unsigned char* dst = reinterpret_cast<unsigned char*>(val);
  unsigned char* src = reinterpret_cast<unsigned char*>(&tmp);

  dst[0] = src[1];
  dst[1] = src[0];
}

static void swap4(unsigned int* val) {
  unsigned int tmp = *val;
  unsigned char* dst = reinterpret_cast<unsigned char*>(val);
  unsigned char* src = reinterpret_cast<unsigned char*>(&tmp);

  dst[0] = src[3];
  dst[1] = src[2];
  dst[2] = src[1];
  dst[3] = src[0];
}

static unsigned short Read2(FILE* fp, bool swap) {
  unsigned short val;
  size_t ret = fread(&val, 1, 2, fp);
  assert(ret == 2);
  if (swap) {
    swap2(&val);
  }
  return val;
}

static unsigned int Read4(FILE* fp, bool swap) {
  unsigned int val;
  size_t ret = fread(&val, 1, 4, fp);
  assert(ret == 4);
  if (swap) {
    swap4(&val);
  }
  return val;
}

static unsigned int ReadUInt(int type, FILE* fp, bool swap) {
  // @todo {8, 9, 10, 11, 12}
  if (type == 3) {
    unsigned short val;
    size_t ret = fread(&val, 1, 2, fp);
    assert(ret == 2);
    if (swap) {
      swap2(&val);
    }
    return static_cast<unsigned int>(val);
  } else if (type == 5) {
    unsigned int val0;
    size_t ret = fread(&val0, 1, 4, fp);
    assert(ret == 4);
    if (swap) {
      swap4(&val0);
    }

    unsigned int val1;
    ret = fread(&val1, 1, 4, fp);
    assert(ret == 4);
    if (swap) {
      swap4(&val1);
    }

    return static_cast<unsigned int>(val0 / val1);

  } else {  // guess 4.
    assert(type == 4);
    unsigned int val;
    size_t ret = fread(&val, 1, 4, fp);
    assert(ret == 4);
    if (swap) {
      swap4(&val);
    }
    return static_cast<unsigned int>(val);
  }
}

static double ReadReal(int type, FILE* fp, bool swap) {
  assert(type == 10);  // @todo { Support more types. }
  int num = static_cast<int>(Read4(fp, swap));
  int denom = static_cast<int>(Read4(fp, swap));

  return static_cast<double>(num) / static_cast<double>(denom);
}

static void GetTIFFTag(unsigned short* tag, unsigned short* type,
                       unsigned int* len, unsigned int* saved_offt, FILE* fp,
                       bool swap_endian) {
  size_t ret;
  ret = fread(tag, 1, 2, fp);
  assert(ret == 2);
  ret = fread(type, 1, 2, fp);
  assert(ret == 2);
  ret = fread(len, 1, 4, fp);
  assert(ret == 4);

  if (swap_endian) {
    swap2(tag);
    swap2(type);
    swap4(len);
  }

  (*saved_offt) = static_cast<unsigned int>(ftell(fp)) + 4;

  size_t typesize_table[] = {1, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8, 8};

  if ((*len) * (typesize_table[(*type) < 14 ? (*type) : 0]) > 4) {
    unsigned int base = 0;  // fixme
    unsigned int offt;
    ret = fread(&offt, 1, 4, fp);
    assert(ret == 4);
    if (swap_endian) {
      swap4(&offt);
    }
    fseek(fp, offt + base, SEEK_SET);
  }
}

static bool DecompressLosslessJPEG(unsigned short* dst_data, int dst_width,
                                   const unsigned char* src,
                                   const size_t src_length, FILE* fp,
                                   const TIFFInfo& tiff_info,
                                   bool swap_endian) {
  // @todo { Remove FILE dependency. }
  //
  (void)dst_data;
  unsigned int tiff_h = 0, tiff_w = 0;
  int offset = 0;

  // Assume Lossless JPEG data is stored in tiled format.
  assert(tiff_info.tile_width > 0);
  assert(tiff_info.tile_length > 0);

  // printf("w x h = %d, %d\n", tiff_info.width, tiff_info.height);
  // printf("tile = %d, %d\n", tiff_info.tile_width, tiff_info.tile_length);

  // @note { It looks width and height information stored in LJPEG header does
  // not maches with tile width height. Assume actual extent of LJPEG data is
  // tile width and height. }
  //

  // Currently we only support tile data for tile.length == tiff.height.
  assert(tiff_info.tile_length == tiff_info.height);

  size_t i_step = 0;
  while (tiff_h < static_cast<unsigned int>(tiff_info.height)) {
    // Read offset to JPEG data location.
    offset = static_cast<int>(Read4(fp, swap_endian));
    // printf("offt = %d\n", offset);

    int lj_width = 0;
    int lj_height = 0;
    int lj_bits = 0;
    lj92 ljp;

    size_t input_len = src_length - static_cast<size_t>(offset);

    // @fixme { Parse LJPEG header first and set exact compressed LJPEG data
    // length to `data_len` arg. }
    int ret = lj92_open(&ljp, reinterpret_cast<const uint8_t*>(&src[offset]),
                        /* data_len */ static_cast<int>(input_len), &lj_width,
                        &lj_height, &lj_bits);
    // printf("ret = %d\n", ret);
    assert(ret == LJ92_ERROR_NONE);

    // printf("lj %d, %d, %d\n", lj_width, lj_height, lj_bits);

    int write_length = tiff_info.tile_width;
    int skip_length = dst_width - tiff_info.tile_width;
    // printf("write_len = %d, skip_len = %d\n", write_length, skip_length);

    // Just offset row position.
    size_t dst_offset = i_step * static_cast<size_t>(tiff_info.tile_width);
    ret = lj92_decode(ljp, dst_data + dst_offset, write_length, skip_length,
                      NULL, 0);

    assert(ret == LJ92_ERROR_NONE);

    lj92_close(ljp);

    tiff_w += static_cast<unsigned int>(tiff_info.tile_width);
    // printf("tiff_w = %d\n", tiff_w);
    if (tiff_w >= static_cast<unsigned int>(tiff_info.width)) {
      // tiff_h += static_cast<unsigned int>(tiff_info.tile_length);
      tiff_h += static_cast<unsigned int>(tiff_info.tile_length);
      // printf("tiff_h = %d\n", tiff_h);
      tiff_w = 0;
    }

    i_step++;
  }

  return true;
}

static bool ParseTIFFIFD(tinydng::DNGInfo* dng_info, TIFFInfo infos[16],
                         int* num_ifds,  // initial value must be 0
                         FILE* fp, bool swap_endian) {
  int idx = (*num_ifds)++;  // increament num_ifds.

  // printf("id = %d\n", idx);
  unsigned short num_entries = 0;
  size_t ret = fread(&num_entries, 1, 2, fp);
  // printf("ret = %ld\n", ret);
  if (swap_endian) {
    swap2(&num_entries);
  }
  // printf("num_entries = %d\n", num_entries);

  assert(ret == 2);
  if (num_entries > 512) {
    assert(0);
    return false;  // @fixme
  }

  infos[idx].has_active_area = false;
  infos[idx].cfa_plane_color[0] = 0;
  infos[idx].cfa_plane_color[1] = 1;
  infos[idx].cfa_plane_color[2] = 2;
  infos[idx].cfa_plane_color[3] = 0;  // optional?

  // The spec says default is None, thus fill with -1(=invalid).
  infos[idx].cfa_pattern[0][0] = -1;
  infos[idx].cfa_pattern[0][1] = -1;
  infos[idx].cfa_pattern[1][0] = -1;
  infos[idx].cfa_pattern[1][1] = -1;

  infos[idx].cfa_layout = 1;

  infos[idx].offset = 0;

  infos[idx].tile_width = -1;
  infos[idx].tile_length = -1;
  infos[idx].tile_offset = 0;

  while (num_entries--) {
    unsigned short tag, type;
    unsigned int len;
    unsigned int saved_offt;
    GetTIFFTag(&tag, &type, &len, &saved_offt, fp, swap_endian);

    switch (tag) {
      case 2:
      case TAG_IMAGE_WIDTH:
      case 61441:  // ImageWidth
        infos[idx].width = static_cast<int>(ReadUInt(type, fp, swap_endian));
        // printf("width = %d\n", infos[idx].width);
        break;

      case 3:
      case TAG_IMAGE_HEIGHT:
      case 61442:  // ImageHeight
        infos[idx].height = static_cast<int>(ReadUInt(type, fp, swap_endian));
        // printf("height = %d\n", infos[idx].height);
        break;

      case TAG_BITS_PER_SAMPLE:
      case 61443:  // BitsPerSample
        infos[idx].samples = len & 7;
        infos[idx].bps = static_cast<int>(ReadUInt(type, fp, swap_endian));
        dng_info->bits_internal = infos[idx].bps;
        break;

      case TAG_COMPRESSION:  // Compression
        infos[idx].compression =
            static_cast<int>(ReadUInt(type, fp, swap_endian));
        break;

      case TAG_STRIP_OFFSET:              // StripOffset
      case 513:                           // JpegIFOffset
        assert(tag == TAG_STRIP_OFFSET);  // @todo { jpeg data }
        infos[idx].offset = Read4(fp, swap_endian);
        // printf("offset = %d\n", infos[idx].offset);
        break;

      case 514:  // JpegIFByteCount
        // printf("byte count = %d\n", Read4(fp, swap_endian));
        break;

      case TAG_ORIENTATION:
        infos[idx].orientation = Read2(fp, swap_endian);
        break;

      case TAG_STRIP_BYTE_COUNTS:
        infos[idx].strip_byte_count = static_cast<int>(Read4(fp, swap_endian));
        // printf("strip_byte_count = %d\n", infos[idx].strip_byte_count);
        break;

      case TAG_SUB_IFDS:

      {
        while (len--) {
          unsigned int i = static_cast<unsigned int>(ftell(fp));
          unsigned int offt = Read4(fp, swap_endian);
          unsigned int base = 0;  // @fixme
          fseek(fp, offt + base, SEEK_SET);

          ParseTIFFIFD(dng_info, infos, num_ifds, fp,
                       swap_endian);   // recursive call
          fseek(fp, i + 4, SEEK_SET);  // rewind
        }
      }

      break;

      case TAG_TILE_WIDTH:
        infos[idx].tile_width =
            static_cast<int>(ReadUInt(type, fp, swap_endian));
        // printf("tile_width = %d\n", infos[idx].tile_width);
        break;

      case TAG_TILE_LENGTH:
        infos[idx].tile_length =
            static_cast<int>(ReadUInt(type, fp, swap_endian));
        // printf("tile_length = %d\n", infos[idx].tile_length);
        break;

      case TAG_TILE_OFFSETS:
        infos[idx].tile_offset = len > 1 ? static_cast<unsigned int>(ftell(fp))
                                         : Read4(fp, swap_endian);
        // printf("tile_offt = %d\n", infos[idx].tile_offset);
        break;

      case TAG_TILE_BYTE_COUNTS:
        infos[idx].tile_byte_count = len > 1
                                         ? static_cast<unsigned int>(ftell(fp))
                                         : Read4(fp, swap_endian);
        break;

      case TAG_CFA_PATTERN: {
        char buf[16];
        size_t readLen = len;
        if (readLen > 16) readLen = 16;
        // Assume 2x2 CFAPattern.
        assert(readLen == 4);
        fread(buf, 1, readLen, fp);
        infos[idx].cfa_pattern[0][0] = buf[0];
        infos[idx].cfa_pattern[0][1] = buf[1];
        infos[idx].cfa_pattern[1][0] = buf[2];
        infos[idx].cfa_pattern[1][1] = buf[3];
      } break;

      case TAG_DNG_VERSION: {
        char data[4];
        data[0] = static_cast<char>(fgetc(fp));
        data[1] = static_cast<char>(fgetc(fp));
        data[2] = static_cast<char>(fgetc(fp));
        data[3] = static_cast<char>(fgetc(fp));

        dng_info->version =
            (data[3] << 24) | (data[2] << 16) | (data[1] << 8) | data[0];
      } break;

      case TAG_CFA_PLANE_COLOR: {
        char buf[4];
        size_t readLen = len;
        if (readLen > 4) readLen = 4;
        fread(buf, 1, readLen, fp);
        for (size_t i = 0; i < readLen; i++) {
          infos[idx].cfa_plane_color[i] = buf[i];
        }
      } break;

      case TAG_CFA_LAYOUT: {
        int layout = Read2(fp, swap_endian);
        infos[idx].cfa_layout = layout;
      } break;

      case TAG_ACTIVE_AREA:
        infos[idx].has_active_area = true;
        infos[idx].active_area[0] =
            static_cast<int>(ReadUInt(type, fp, swap_endian));
        infos[idx].active_area[1] =
            static_cast<int>(ReadUInt(type, fp, swap_endian));
        infos[idx].active_area[2] =
            static_cast<int>(ReadUInt(type, fp, swap_endian));
        infos[idx].active_area[3] =
            static_cast<int>(ReadUInt(type, fp, swap_endian));
        break;

      case TAG_BLACK_LEVEL:
        dng_info->black_level =
            static_cast<int>(ReadUInt(type, fp, swap_endian));
        break;

      case TAG_WHITE_LEVEL:
        dng_info->white_level =
            static_cast<int>(ReadUInt(type, fp, swap_endian));
        break;

      case TAG_CALIBRATION_ILLUMINANT1:
        dng_info->calibration_illuminant1 =
            static_cast<LightSource>(Read2(fp, swap_endian));
        break;

      case TAG_CALIBRATION_ILLUMINANT2:
        dng_info->calibration_illuminant2 =
            static_cast<LightSource>(Read2(fp, swap_endian));
        break;

      case TAG_COLOR_MATRIX1: {
        for (int c = 0; c < 3; c++) {
          for (int k = 0; k < 3; k++) {
            double val = ReadReal(type, fp, swap_endian);
            dng_info->color_matrix1[c][k] = val;
          }
        }
      } break;

      case TAG_COLOR_MATRIX2: {
        for (int c = 0; c < 3; c++) {
          for (int k = 0; k < 3; k++) {
            double val = ReadReal(type, fp, swap_endian);
            dng_info->color_matrix2[c][k] = val;
          }
        }
      } break;

      case TAG_FORWARD_MATRIX1: {
        for (int c = 0; c < 3; c++) {
          for (int k = 0; k < 3; k++) {
            double val = ReadReal(type, fp, swap_endian);
            dng_info->forward_matrix1[c][k] = val;
          }
        }
      } break;

      case TAG_FORWARD_MATRIX2: {
        for (int c = 0; c < 3; c++) {
          for (int k = 0; k < 3; k++) {
            double val = ReadReal(type, fp, swap_endian);
            dng_info->forward_matrix2[c][k] = val;
          }
        }
      } break;

      case TAG_CAMERA_CALIBRATION1: {
        for (int c = 0; c < 3; c++) {
          for (int k = 0; k < 3; k++) {
            double val = ReadReal(type, fp, swap_endian);
            dng_info->camera_calibration1[c][k] = val;
          }
        }
      } break;

      case TAG_CAMERA_CALIBRATION2: {
        for (int c = 0; c < 3; c++) {
          for (int k = 0; k < 3; k++) {
            double val = ReadReal(type, fp, swap_endian);
            dng_info->camera_calibration2[c][k] = val;
          }
        }
      } break;

      default:
        break;
    }

    fseek(fp, saved_offt, SEEK_SET);
  }

  return true;
}

static bool ParseDNG(tinydng::DNGInfo* dng_info, TIFFInfo infos[16],
                     int* num_ifds, FILE* fp, bool swap_endian) {
  int offt;
  size_t ret = fread(&offt, 1, 4, fp);
  assert(ret == 4);

  if (swap_endian) {
    swap4(reinterpret_cast<unsigned int*>(&offt));
  }

  // Init
  dng_info->color_matrix1[0][0] = 1.0;
  dng_info->color_matrix1[0][1] = 0.0;
  dng_info->color_matrix1[0][2] = 0.0;
  dng_info->color_matrix1[1][0] = 0.0;
  dng_info->color_matrix1[1][1] = 1.0;
  dng_info->color_matrix1[1][2] = 0.0;
  dng_info->color_matrix1[2][0] = 0.0;
  dng_info->color_matrix1[2][1] = 0.0;
  dng_info->color_matrix1[2][2] = 1.0;

  dng_info->color_matrix2[0][0] = 1.0;
  dng_info->color_matrix2[0][1] = 0.0;
  dng_info->color_matrix2[0][2] = 0.0;
  dng_info->color_matrix2[1][0] = 0.0;
  dng_info->color_matrix2[1][1] = 1.0;
  dng_info->color_matrix2[1][2] = 0.0;
  dng_info->color_matrix2[2][0] = 0.0;
  dng_info->color_matrix2[2][1] = 0.0;
  dng_info->color_matrix2[2][2] = 1.0;

  dng_info->forward_matrix1[0][0] = 1.0;
  dng_info->forward_matrix1[0][1] = 0.0;
  dng_info->forward_matrix1[0][2] = 0.0;
  dng_info->forward_matrix1[1][0] = 0.0;
  dng_info->forward_matrix1[1][1] = 1.0;
  dng_info->forward_matrix1[1][2] = 0.0;
  dng_info->forward_matrix1[2][0] = 0.0;
  dng_info->forward_matrix1[2][1] = 0.0;
  dng_info->forward_matrix1[2][2] = 1.0;

  dng_info->forward_matrix2[0][0] = 1.0;
  dng_info->forward_matrix2[0][1] = 0.0;
  dng_info->forward_matrix2[0][2] = 0.0;
  dng_info->forward_matrix2[1][0] = 0.0;
  dng_info->forward_matrix2[1][1] = 1.0;
  dng_info->forward_matrix2[1][2] = 0.0;
  dng_info->forward_matrix2[2][0] = 0.0;
  dng_info->forward_matrix2[2][1] = 0.0;
  dng_info->forward_matrix2[2][2] = 1.0;

  dng_info->camera_calibration1[0][0] = 1.0;
  dng_info->camera_calibration1[0][1] = 0.0;
  dng_info->camera_calibration1[0][2] = 0.0;
  dng_info->camera_calibration1[1][0] = 0.0;
  dng_info->camera_calibration1[1][1] = 1.0;
  dng_info->camera_calibration1[1][2] = 0.0;
  dng_info->camera_calibration1[2][0] = 0.0;
  dng_info->camera_calibration1[2][1] = 0.0;
  dng_info->camera_calibration1[2][2] = 1.0;

  dng_info->camera_calibration2[0][0] = 1.0;
  dng_info->camera_calibration2[0][1] = 0.0;
  dng_info->camera_calibration2[0][2] = 0.0;
  dng_info->camera_calibration2[1][0] = 0.0;
  dng_info->camera_calibration2[1][1] = 1.0;
  dng_info->camera_calibration2[1][2] = 0.0;
  dng_info->camera_calibration2[2][0] = 0.0;
  dng_info->camera_calibration2[2][1] = 0.0;
  dng_info->camera_calibration2[2][2] = 1.0;

  dng_info->calibration_illuminant1 = LIGHTSOURCE_UNKNOWN;
  dng_info->calibration_illuminant2 = LIGHTSOURCE_UNKNOWN;

  dng_info->white_level = -1;  // White level will be set after parsing TAG.
                               // The spec says: The default value for this
                               // tag is (2 ** BitsPerSample)
                               // -1 for unsigned integer images, and 1.0 for
                               // floating point images.
  dng_info->black_level = 0;
  dng_info->bits_internal = 0;

  (*num_ifds) = 0;  // initial value = 0
  while (offt) {
    fseek(fp, offt, SEEK_SET);
    if (ParseTIFFIFD(dng_info, infos, num_ifds, fp, swap_endian)) {
      break;
    }
    ret = fread(&offt, 1, 4, fp);
    assert(ret == 4);
  }

  if (dng_info->white_level == -1) {
    assert(dng_info->bits_internal > 0);
    assert(dng_info->bits_internal < 32);
    dng_info->white_level = (1 << dng_info->bits_internal);
  }

  return true;
}

bool LoadDNG(DNGInfo* info, std::vector<unsigned char>* data, size_t* len,
             int* width, int* height, int* bits, int* compos, std::string* err,
             const char* filename, bool is_system_big_endian) {
  std::stringstream ss;

  assert(info);
  assert(data);
  assert(len);
  assert(width);
  assert(height);
  assert(bits);
  assert(compos);

  FILE* fp = fopen(filename, "rb");
  if (!fp) {
    ss << "File not found or cannot open file " << filename << std::endl;
    if (err) {
      (*err) = ss.str();
    }
    return false;
  }

  unsigned short magic;
  char header[32];

  size_t ret;
  ret = fread(&magic, 1, 2, fp);
  assert(ret == 2);

  int seek_ret = fseek(fp, 0, SEEK_SET);
  assert(seek_ret == 0);

  ret = fread(header, 1, 32, fp);
  assert(ret == 32);
  seek_ret = fseek(fp, 0, SEEK_END);
  assert(seek_ret == 0);

  size_t file_size = static_cast<size_t>(ftell(fp));

  bool is_dng_big_endian = false;

  if (magic == 0x4949) {
    // might be TIFF(DNG).
  } else if (magic == 0x4d4d) {
    // might be TIFF(DNG, bigendian).
    is_dng_big_endian = true;
  } else {
    ss << "Seems the file is not DNG format." << std::endl;
    if (err) {
      (*err) = ss.str();
    }

    fclose(fp);
    return false;
  }

  TIFFInfo infos[16];
  int num_ifds = 0;

  // skip magic header
  fseek(fp, 4, SEEK_SET);
  const bool swap_endian = (is_dng_big_endian && (!is_system_big_endian));
  ret = ParseDNG(info, infos, &num_ifds, fp, swap_endian);

  if (ret) {
    // Choose the largest image
    int max_idx = 0;
    int max_width = 0;
    for (int i = 0; i < num_ifds; i++) {
      if (infos[i].width > max_width) {
        max_idx = i;
        max_width = infos[i].width;
      }
    }

    assert(max_idx < 16);

    int idx = max_idx;

    // printf("compression = %d\n", infos[idx].compression);

    size_t data_offset =
        (infos[idx].offset > 0) ? infos[idx].offset : infos[idx].tile_offset;
    assert(data_offset > 0);

    // std::cout << "offt =\n" << infos[idx].offset << std::endl;
    // std::cout << "tile_offt = \n" << infos[idx].tile_offset << std::endl;
    // std::cout << "data_offset = " << data_offset << std::endl;

    if (infos[idx].compression == 1) {  // no compression
      assert(((infos[idx].width * infos[idx].height * infos[idx].bps) % 8) ==
             0);
      (*len) = static_cast<size_t>(
          (infos[idx].width * infos[idx].height * infos[idx].bps) / 8);
      assert((*len) > 0);
      data->resize((*len), 0);
      fseek(fp, static_cast<long>(data_offset), SEEK_SET);
      ret = fread(&data->at(0), 1, (*len), fp);
      assert(ret == (*len));
    } else if (infos[idx].compression ==
               7) {  //  new JPEG(baseline DCT JPEG or lossless JPEG)

      // lj92 decodes data into 16bits, so modify bps.
      infos[idx].bps = 16;

      assert(((infos[idx].width * infos[idx].height * infos[idx].bps) % 8) ==
             0);
      (*len) = static_cast<size_t>(
          (infos[idx].width * infos[idx].height * infos[idx].bps) / 8);
      assert((*len) > 0);
      data->resize((*len), 0);

      //
      // Read whole file data.
      // @todo { Read only compressed LJPEG data into buffer. }
      //
      fseek(fp, 0, SEEK_SET);
      std::vector<unsigned char> buffer(file_size);
      ret = fread(buffer.data(), 1, file_size, fp);
      assert(ret == file_size);

      // Move to LJPEG data location.
      fseek(fp, static_cast<long>(data_offset), SEEK_SET);

      // printf("data_offset = %d\n", static_cast<int>(data_offset));
      bool ok = DecompressLosslessJPEG(
          reinterpret_cast<unsigned short*>(data->data()), infos[idx].width,
          buffer.data(), buffer.size(), fp, infos[idx], swap_endian);
      if (!ok) {
        if (err) {
          ss << "ZIP compression is not supported." << std::endl;
          (*err) = ss.str();
        }
        return false;
      }

    } else if (infos[idx].compression == 8) {  // ZIP
      if (err) {
        ss << "ZIP compression is not supported." << std::endl;
        (*err) = ss.str();
      }
    } else if (infos[idx].compression == 34892) {  // lossy JPEG
      if (err) {
        ss << "lossy JPEG compression is not supported." << std::endl;
        (*err) = ss.str();
      }
    } else {
      if (err) {
        ss << "Unsupported compression type : " << infos[idx].compression
           << std::endl;
        (*err) = ss.str();
      }
      return false;
    }

    if (infos[idx].has_active_area) {
      info->active_area[0] = infos[idx].active_area[0];
      info->active_area[1] = infos[idx].active_area[1];
      info->active_area[2] = infos[idx].active_area[2];
      info->active_area[3] = infos[idx].active_area[3];
    } else {
      info->active_area[0] = 0;
      info->active_area[1] = 0;
      info->active_area[2] = infos[idx].width;
      info->active_area[3] = infos[idx].height;
    }

    info->cfa_plane_color[0] = infos[idx].cfa_plane_color[0];
    info->cfa_plane_color[1] = infos[idx].cfa_plane_color[1];
    info->cfa_plane_color[2] = infos[idx].cfa_plane_color[2];
    info->cfa_plane_color[3] = infos[idx].cfa_plane_color[3];
    info->cfa_pattern[0][0] = infos[idx].cfa_pattern[0][0];
    info->cfa_pattern[0][1] = infos[idx].cfa_pattern[0][1];
    info->cfa_pattern[1][0] = infos[idx].cfa_pattern[1][0];
    info->cfa_pattern[1][1] = infos[idx].cfa_pattern[1][1];
    info->cfa_layout = infos[idx].cfa_layout;

    info->tile_width = infos[idx].tile_width;
    info->tile_length = infos[idx].tile_length;
    info->tile_offset = infos[idx].tile_offset;

    (*width) = infos[idx].width;
    (*height) = infos[idx].height;
    (*bits) = infos[idx].bps;
    (*compos) = infos[idx].samples;
  }

  fclose(fp);

  return ret;
}

}  // namespace tinydng

#endif

#endif  // TINY_DNG_LOADER_H_
