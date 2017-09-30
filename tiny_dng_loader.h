//
// TinyDNGLoader, single header only DNG/TIFF loader.
//

/*
The MIT License (MIT)

Copyright (c) 2016 - 2017 Syoyo Fujita and many contributors.

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
// http://lclevy.free.fr/nef/ For NEF file format
// }

#include <string>
#include <vector>

#if !defined(TINY_DNG_NO_EXCEPTION)
#include <stdexcept>
#endif

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

typedef enum {
  COMPRESSION_NONE = 1,
  COMPRESSION_LZW = 5,            // LZW
  COMPRESSION_OLD_JPEG = 6,       // JPEG or lossless JPEG
  COMPRESSION_NEW_JPEG = 7,       // Usually lossles JPEG, may be JPEG
  COMPRESSION_ADOBE_DEFLATE = 8,  // ZIP?
  COMPRESSION_LOSSY = 34892,      // Lossy JPEGG
  COMPRESSION_NEF = 34713         // NIKON RAW
} Compression;

typedef enum {
  TYPE_NOTYPE = 0,
  TYPE_BYTE = 1,
  TYPE_ASCII = 2,  // null-terminated string
  TYPE_SHORT = 3,
  TYPE_LONG = 4,
  TYPE_RATIONAL = 5,  // 64-bit unsigned fraction
  TYPE_SBYTE = 6,
  TYPE_UNDEFINED = 7,  // 8-bit untyped data */
  TYPE_SSHORT = 8,
  TYPE_SLONG = 9,
  TYPE_SRATIONAL = 10,  // 64-bit signed fraction
  TYPE_FLOAT = 11,
  TYPE_DOUBLE = 12,
  TYPE_IFD = 13,     // 32-bit unsigned integer (offset)
  TYPE_LONG8 = 16,   // BigTIFF 64-bit unsigned
  TYPE_SLONG8 = 17,  // BigTIFF 64-bit signed
  TYPE_IFD8 = 18     // BigTIFF 64-bit unsigned integer (offset)
} DataType;

typedef enum {
  SAMPLEFORMAT_UINT = 1,
  SAMPLEFORMAT_INT = 2,
  SAMPLEFORMAT_IEEEFP = 3,  // floating point
  SAMPLEFORMAT_VOID = 4,
  SAMPLEFORMAT_COMPLEXINT = 5,
  SAMPLEFORMAT_COMPLEXIEEEFP = 6
} SampleFormat;

struct FieldInfo {
  int tag;
  short read_count;
  short write_count;
  DataType type;
  unsigned short bit;
  unsigned char ok_to_change;
  unsigned char pass_count;
  std::string name;

  FieldInfo()
      : tag(0),
        read_count(-1),
        write_count(-1),
        type(TYPE_NOTYPE),
        bit(0),
        ok_to_change(0),
        pass_count(0) {}
};

struct FieldData {
  int tag;
  DataType type;
  std::string name;
  std::vector<unsigned char> data;

  FieldData() : tag(0), type(TYPE_NOTYPE) {}
};

struct DNGImage {
  int black_level[4];  // for each spp(up to 4)
  int white_level[4];  // for each spp(up to 4)
  int version;         // DNG version

  int samples_per_pixel;
  int rows_per_strip;

  int bits_per_sample_original;  // BitsPerSample in stored file.
  int bits_per_sample;  // Bits per sample after reading(decoding) DNG image.

  char cfa_plane_color[4];  // 0:red, 1:green, 2:blue, 3:cyan, 4:magenta,
                            // 5:yellow, 6:white
  int cfa_pattern[2][2];    // @fixme { Support non 2x2 CFA pattern. }
  int cfa_pattern_dim;
  int cfa_layout;
  int active_area[4];  // top, left, bottom, right
  bool has_active_area;
  unsigned char pad_has_active_area[3];

  int tile_width;
  int tile_length;
  int pad_tile0;
  std::vector<unsigned int> tile_offsets;
  std::vector<unsigned int> tile_byte_counts;  // (compressed) size
  int pad_tile1;

  int pad0;
  double analog_balance[3];
  bool has_analog_balance;
  unsigned char pad1[7];

  double as_shot_neutral[3];
  int pad3;
  bool has_as_shot_neutral;
  unsigned char pad4[7];

  int pad5;
  double color_matrix1[3][3];
  double color_matrix2[3][3];

  double forward_matrix1[3][3];
  double forward_matrix2[3][3];

  double camera_calibration1[3][3];
  double camera_calibration2[3][3];

  LightSource calibration_illuminant1;
  LightSource calibration_illuminant2;

  int width;
  int height;
  int compression;
  unsigned int offset;
  int orientation;
  int strip_byte_count;
  int jpeg_byte_count;
  int planar_configuration;  // 1: chunky, 2: planar
  int predictor;  // tag 317. 1 = no prediction, 2 = horizontal differencing, 3
                  // = floating point horizontal differencing

  SampleFormat sample_format;
  int pad_sf;

  // For an image with multiple strips.
  int strips_per_image;
  std::vector<unsigned int> strip_byte_counts;
  std::vector<unsigned int> strip_offsets;

  // CR2(Canon RAW) specific
  unsigned short cr2_slices[3];
  unsigned short pad_c;

  std::vector<unsigned char>
      data;  // Decoded pixel data(len = spp * width * height * bps / 8)

  // Custom fields
  std::vector<FieldData> custom_fields;

  void initialize() {
    this->version = 0;

    this->color_matrix1[0][0] = 1.0;
    this->color_matrix1[0][1] = 0.0;
    this->color_matrix1[0][2] = 0.0;
    this->color_matrix1[1][0] = 0.0;
    this->color_matrix1[1][1] = 1.0;
    this->color_matrix1[1][2] = 0.0;
    this->color_matrix1[2][0] = 0.0;
    this->color_matrix1[2][1] = 0.0;
    this->color_matrix1[2][2] = 1.0;

    this->color_matrix2[0][0] = 1.0;
    this->color_matrix2[0][1] = 0.0;
    this->color_matrix2[0][2] = 0.0;
    this->color_matrix2[1][0] = 0.0;
    this->color_matrix2[1][1] = 1.0;
    this->color_matrix2[1][2] = 0.0;
    this->color_matrix2[2][0] = 0.0;
    this->color_matrix2[2][1] = 0.0;
    this->color_matrix2[2][2] = 1.0;

    this->forward_matrix1[0][0] = 1.0;
    this->forward_matrix1[0][1] = 0.0;
    this->forward_matrix1[0][2] = 0.0;
    this->forward_matrix1[1][0] = 0.0;
    this->forward_matrix1[1][1] = 1.0;
    this->forward_matrix1[1][2] = 0.0;
    this->forward_matrix1[2][0] = 0.0;
    this->forward_matrix1[2][1] = 0.0;
    this->forward_matrix1[2][2] = 1.0;

    this->forward_matrix2[0][0] = 1.0;
    this->forward_matrix2[0][1] = 0.0;
    this->forward_matrix2[0][2] = 0.0;
    this->forward_matrix2[1][0] = 0.0;
    this->forward_matrix2[1][1] = 1.0;
    this->forward_matrix2[1][2] = 0.0;
    this->forward_matrix2[2][0] = 0.0;
    this->forward_matrix2[2][1] = 0.0;
    this->forward_matrix2[2][2] = 1.0;

    this->camera_calibration1[0][0] = 1.0;
    this->camera_calibration1[0][1] = 0.0;
    this->camera_calibration1[0][2] = 0.0;
    this->camera_calibration1[1][0] = 0.0;
    this->camera_calibration1[1][1] = 1.0;
    this->camera_calibration1[1][2] = 0.0;
    this->camera_calibration1[2][0] = 0.0;
    this->camera_calibration1[2][1] = 0.0;
    this->camera_calibration1[2][2] = 1.0;

    this->camera_calibration2[0][0] = 1.0;
    this->camera_calibration2[0][1] = 0.0;
    this->camera_calibration2[0][2] = 0.0;
    this->camera_calibration2[1][0] = 0.0;
    this->camera_calibration2[1][1] = 1.0;
    this->camera_calibration2[1][2] = 0.0;
    this->camera_calibration2[2][0] = 0.0;
    this->camera_calibration2[2][1] = 0.0;
    this->camera_calibration2[2][2] = 1.0;

    this->calibration_illuminant1 = LIGHTSOURCE_UNKNOWN;
    this->calibration_illuminant2 = LIGHTSOURCE_UNKNOWN;

    this->white_level[0] = -1;  // White level will be set after parsing TAG.
                                // The spec says: The default value for this
                                // tag is (2 ** BitsPerSample)
                                // -1 for unsigned integer images, and 1.0 for
                                // floating point images.

    this->white_level[1] = -1;
    this->white_level[2] = -1;
    this->white_level[3] = -1;
    this->black_level[0] = 0;
    this->black_level[1] = 0;
    this->black_level[2] = 0;
    this->black_level[3] = 0;

    this->bits_per_sample = 0;

    this->has_active_area = false;
    this->active_area[0] = -1;
    this->active_area[1] = -1;
    this->active_area[2] = -1;
    this->active_area[3] = -1;

    this->cfa_plane_color[0] = 0;
    this->cfa_plane_color[1] = 1;
    this->cfa_plane_color[2] = 2;
    this->cfa_plane_color[3] = 0;  // optional?

    this->cfa_pattern_dim = 2;

    // The spec says default is None, thus fill with -1(=invalid).
    this->cfa_pattern[0][0] = -1;
    this->cfa_pattern[0][1] = -1;
    this->cfa_pattern[1][0] = -1;
    this->cfa_pattern[1][1] = -1;

    this->cfa_layout = 1;

    this->offset = 0;

    this->tile_width = -1;
    this->tile_length = -1;
    this->tile_offsets.clear();
    this->tile_byte_counts.clear();

    this->planar_configuration = 1;  // chunky

    this->predictor = 1;  // no prediction scheme

    this->has_analog_balance = false;
    this->has_as_shot_neutral = false;

    this->jpeg_byte_count = -1;
    this->strip_byte_count = -1;

    this->samples_per_pixel = 1;
    this->rows_per_strip = -1;  // 2^32 - 1
    this->bits_per_sample_original = 1;

    this->sample_format = SAMPLEFORMAT_UINT;

    this->compression = COMPRESSION_NONE;

    this->orientation = 1;

    this->strips_per_image = -1;  // 2^32 - 1

    // CR2 specific
    this->cr2_slices[0] = 0;
    this->cr2_slices[1] = 0;
    this->cr2_slices[2] = 0;
  }

  DNGImage() { initialize(); }
};

///
/// Loads DNG image and store it to `images`
///
/// If DNG contains multiple images(e.g. full-res image + thumnail image),
/// The function creates `DNGImage` data strucure for each images.
///
/// C++ exception would be trigerred inside the function unless
/// TINY_DNG_NO_EXCEPTION macro is defined.
///
/// @param[in] filename DNG filename.
/// @param[in] custom_fields List of custom fields to parse(optional. can pass
/// empty array).
/// @param[out] images Loaded DNG images.
/// @param[out] err Error message.
///
/// @return true upon success.
/// @return false upon failure and store error message into `err`.
///
bool LoadDNG(const char* filename, std::vector<FieldInfo>& custom_fields,
             std::vector<DNGImage>* images, std::string* err);

}  // namespace tinydng

#ifdef TINY_DNG_LOADER_IMPLEMENTATION

#include <stdint.h>  // for lj92
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>

#ifdef TINY_DNG_LOADER_PROFILING
// Requires C++11 feature
#include <chrono>
#endif

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wvariadic-macros"
#pragma clang diagnostic ignored "-Wc++98-compat-pedantic"
#endif

#ifdef TINY_DNG_LOADER_DEBUG
#define TINY_DNG_DPRINTF(...) printf(__VA_ARGS__)
#else
#define TINY_DNG_DPRINTF(...)
#endif

#if !defined(TINY_DNG_NO_EXCEPTION)
#define TINY_DNG_ASSERT(assertion, text) \
  do {                                   \
    if ((assertion) == 0) {              \
      throw std::runtime_error(text);    \
    }                                    \
  } while (false)

#define TINY_DNG_ABORT(text)        \
  do {                              \
    throw std::runtime_error(text); \
  } while (false)

#else
#define TINY_DNG_ASSERT(assertion, text)                                    \
  do {                                                                      \
    if ((assertion) == 0) {                                                 \
      std::cerr << __FILE__ << ":" << __LINE__ << " " << text << std::endl; \
      abort();                                                              \
    }                                                                       \
  } while (false)
#define TINY_DNG_ABORT(text)                                              \
  do {                                                                    \
    std::cerr << __FILE__ << ":" << __LINE__ << " " << text << std::endl; \
    abort();                                                              \
  } while (false)
#endif

#ifdef __clang__
#pragma clang diagnostic pop
#endif

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
#pragma clang diagnostic ignored "-Wreserved-id-macro"
#pragma clang diagnostic ignored "-Wdisabled-macro-expansion"
#pragma clang diagnostic ignored "-Wdouble-promotion"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#if __has_warning("-Wcomma")
#pragma clang diagnostic ignored "-Wcomma"
#endif
#endif

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4100)
#pragma warning(disable : 4334)
#pragma warning(disable : 4244)
#endif

// STB image to decode jpeg image.
// Assume STB_IMAGE_IMPLEMENTATION is defined elsewhere
#include "stb_image.h"

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace tinydng {

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
#pragma clang diagnostic ignored "-Wreserved-id-macro"
#pragma clang diagnostic ignored "-Wdisabled-macro-expansion"
#pragma clang diagnostic ignored "-Wdouble-promotion"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#if __has_warning("-Wcomma")
#pragma clang diagnostic ignored "-Wcomma"
#endif
#endif

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4100)
#pragma warning(disable : 4334)
#pragma warning(disable : 4244)
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
 * - width/height/bitdepth of the data
 * Returns status code.
 * If status == LJ92_ERROR_NONE, handle must be closed with lj92_close
 */
int lj92_open(lj92* lj,                          // Return handle here
              const uint8_t* data, int datalen,  // The encoded data
              int* width, int* height,
              int* bitdepth);  // Width, height and bitdepth

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
    // TINY_DNG_DPRINTF("idx = %d, datalen = %\d\n", ix, self->datalen);
    return -1;
  }
  self->ix = ix;
  // TINY_DNG_DPRINTF("ix = %d, data = %d\n", ix, data[ix - 1]);
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
    // TINY_DNG_DPRINTF("%d %d %d\n",i,bitsused,hcode);
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

  TINY_DNG_ASSERT(self->components >= 1 && self->components < 6,
                  "Invalid number of components.");

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

inline static int nextdiff(ljp* self, int component_idx, int Px) {
  (void)Px;
#ifdef SLOW_HUFF
  int t = decode(self);
  int diff = receive(self, t);
  diff = extend(self, diff, t);
// TINY_DNG_DPRINTF("%d %d %d %x\n",Px+diff,Px,diff,t);//,index,usedbits);
#else
  TINY_DNG_ASSERT(component_idx <= self->num_huff_idx, "Invalid huff index.");
  u32 b = self->b;
  int cnt = self->cnt;
  int huffbits = self->huffbits[component_idx];
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
      // TINY_DNG_DPRINTF("%x %x %x %x %d\n",one,two,b,b>>8,cnt);
      b >>= 8;
      cnt -= 8;
    } else if (two == 0xFF)
      ix++;
  }
  int index = b >> (cnt - huffbits);
  // TINY_DNG_DPRINTF("component_idx = %d / %d, index = %d\n", component_idx,
  // self->components, index);

  u16 ssssused = self->hufflut[component_idx][index];
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
// TINY_DNG_DPRINTF("%d %d\n",t,diff);
// TINY_DNG_DPRINTF("%d %d %d %x %x %d\n",Px+diff,Px,diff,t,index,usedbits);
#ifdef LJ92_DEBUG
#endif
#endif
  return diff;
}

static int parsePred6(ljp* self) {
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
  TINY_DNG_ASSERT(self->num_huff_idx <= self->components,
                  "Invalid number of huff indices.");
  // First pixel
  diff = nextdiff(self, self->num_huff_idx - 1,
                  0);  // FIXME(syoyo): Is using (self->num_huff_idx-1) correct?
  Px = 1 << (self->bits - 1);
  left = Px + diff;
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
    diff = nextdiff(self, self->num_huff_idx - 1, 0);
    Px = left;
    left = Px + diff;
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
    diff = nextdiff(self, self->num_huff_idx - 1, 0);
    Px = lastrow[col];  // Use value above for first pixel in row
    left = Px + diff;
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
      diff = nextdiff(self, self->num_huff_idx - 1, 0);
      Px = lastrow[col] + ((left - lastrow[col - 1]) >> 1);
      left = Px + diff;
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

static int parseScan(ljp* self) {
  int ret = LJ92_ERROR_CORRUPT;
  memset(self->sssshist, 0, sizeof(self->sssshist));
  self->ix = self->scanstart;
  int compcount = self->data[self->ix + 2];
  int pred = self->data[self->ix + 3 + 2 * compcount];
  if (pred < 0 || pred > 7) return ret;
  if (pred == 6) return parsePred6(self);  // Fast path
  // TINY_DNG_DPRINTF("pref = %d\n", pred);
  self->ix += BEH(self->data[self->ix]);
  self->cnt = 0;
  self->b = 0;
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
    for (int col = 0; col < self->x; col++) {
      int colx = col * self->components;
      for (int c = 0; c < self->components; c++) {
        // TINY_DNG_DPRINTF("c = %d, col = %d, row = %d\n", c, col, row);
        if ((col == 0) && (row == 0)) {
          Px = 1 << (self->bits - 1);
        } else if (row == 0) {
          // Px = left;
          TINY_DNG_ASSERT(col > 0, "Unexpected col.");
          Px = thisrow[(col - 1) * self->components + c];
        } else if (col == 0) {
          Px = lastrow[c];  // Use value above for first pixel in row
        } else {
          int prev_colx = (col - 1) * self->components;
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
              break;
          }
        }

        int huff_idx = c;
        if (c >= self->num_huff_idx) {
          // It looks huffman tables are shared for all components.
          // Currently we assume # of huffman tables is 1.
          TINY_DNG_ASSERT(self->num_huff_idx == 1,
                          "Cannot handle >1 huffman tables.");
          huff_idx = 0;  // Look up the first huffman table.
        }

        diff = nextdiff(self, huff_idx, Px);
        left = Px + diff;
        // TINY_DNG_DPRINTF("c[%d] Px = %d, diff = %d, left = %d\n", c, Px,
        // diff, left);
        TINY_DNG_ASSERT(left >= 0 && left < (1 << self->bits),
                        "Error huffman decoding.");
        // TINY_DNG_DPRINTF("pix = %d\n", left);
        // TINY_DNG_DPRINTF("%d %d %d\n",c,diff,left);
        int linear;
        if (self->linearize) {
          if (left > self->linlen) return LJ92_ERROR_CORRUPT;
          linear = self->linearize[left];
        } else {
          linear = left;
        }

        // TINY_DNG_DPRINTF("linear = %d\n", linear);
        thisrow[colx + c] = left;
        out[colx + c] = linear;  // HACK
      }                          // c
    }                            // col

    u16* temprow = lastrow;
    lastrow = thisrow;
    thisrow = temprow;

    out += self->x * self->components + self->skiplen;
    // out += self->skiplen;
    // TINY_DNG_DPRINTF("out = %p, %p, diff = %lld\n", out, self->image, out -
    // self->image);

  }  // row

  ret = LJ92_ERROR_NONE;

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
}

int lj92_open(lj92* lj, const uint8_t* data, int datalen, int* width,
              int* height, int* bitdepth) {
  ljp* self = (ljp*)calloc(sizeof(ljp), 1);
  if (self == NULL) return LJ92_ERROR_NO_MEMORY;

  self->data = (u8*)data;
  self->dataend = self->data + datalen;
  self->datalen = datalen;
  self->num_huff_idx = 0;

  int ret = findSoI(self);

  if (ret == LJ92_ERROR_NONE) {
    u16* rowcache = (u16*)calloc(self->x * self->components * 2, sizeof(u16));
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

#if 0  // not used in tinydngloader
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
  uint16_t* rowcache = (uint16_t*)calloc(1, self->width * self->components * 4);
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
}  // namespace

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#ifdef _MSC_VER
#pragma warning(pop)
#endif

typedef enum {
  TAG_NEW_SUBFILE_TYPE = 254,
  TAG_SUBFILE_TYPE = 255,
  TAG_IMAGE_WIDTH = 256,
  TAG_IMAGE_HEIGHT = 257,
  TAG_BITS_PER_SAMPLE = 258,
  TAG_COMPRESSION = 259,
  TAG_STRIP_OFFSET = 273,
  TAG_ORIENTATION = 274,
  TAG_SAMPLES_PER_PIXEL = 277,
  TAG_ROWS_PER_STRIP = 278,
  TAG_STRIP_BYTE_COUNTS = 279,
  TAG_PLANAR_CONFIGURATION = 284,
  TAG_PREDICTOR = 317,
  TAG_SUB_IFDS = 330,
  TAG_TILE_WIDTH = 322,
  TAG_TILE_LENGTH = 323,
  TAG_TILE_OFFSETS = 324,
  TAG_TILE_BYTE_COUNTS = 325,
  TAG_SAMPLE_FORMAT = 339,
  TAG_JPEG_IF_OFFSET = 513,
  TAG_JPEG_IF_BYTE_COUNT = 514,
  TAG_CFA_PATTERN_DIM = 33421,
  TAG_CFA_PATTERN = 33422,
  TAG_EXIF = 34665,
  TAG_CFA_PLANE_COLOR = 50710,
  TAG_CFA_LAYOUT = 50711,
  TAG_BLACK_LEVEL = 50714,
  TAG_WHITE_LEVEL = 50717,
  TAG_COLOR_MATRIX1 = 50721,
  TAG_COLOR_MATRIX2 = 50722,
  TAG_CAMERA_CALIBRATION1 = 50723,
  TAG_CAMERA_CALIBRATION2 = 50724,
  TAG_DNG_VERSION = 50706,
  TAG_ANALOG_BALANCE = 50727,
  TAG_AS_SHOT_NEUTRAL = 50728,
  TAG_CALIBRATION_ILLUMINANT1 = 50778,
  TAG_CALIBRATION_ILLUMINANT2 = 50779,
  TAG_ACTIVE_AREA = 50829,
  TAG_FORWARD_MATRIX1 = 50964,
  TAG_FORWARD_MATRIX2 = 50965,

  // CR2 extension
  // http://lclevy.free.fr/cr2/
  TAG_CR2_META0 = 50648,
  TAG_CR2_META1 = 50656,
  TAG_CR2_SLICES = 50752,
  TAG_CR2_META2 = 50885,

  TAG_INVALID = 65535
} TiffTag;

// http://www.awaresystems.be/imaging/tiff/tifftags/privateifd/exif.html
// TODO(syoyo): Support more EXIF tags.
typedef enum {
  EXIF_EXPOSURE_TIME = 33434,
  EXIF_F_NUMBER = 33437,
  EXIF_SHUTTER_SPEED_VALUE = 37377,
  EXIF_MAKER_NOTE = 37500
} ExifTag;

struct EXIF {
  double exposure_time;
  double f_number;
  double shutter_speed;

  std::vector<unsigned char> maker_note;  // Manufacturer specific information.

  EXIF()
      :  // negative = invalid values.
        exposure_time(-1.0),
        f_number(-1.0),
        shutter_speed(-1.0) {}
};

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

static bool IsBigEndian() {
  uint32_t i = 0x01020304;
  char c[4];
  memcpy(c, &i, 4);
  return (c[0] == 1);
}

struct NikonMakerNote {
  int compression;  // 1 = lossy type1, 2 = uncompressed, 3 = lossy type2
  int pad0;
  double white_balance[4];  // R, G1, B, G2
  size_t linearization_table_offset;  // Offset to linearization table.

  NikonMakerNote() : linearization_table_offset(0) {}
};

///
/// Simple stream reder
///
class StreamReader {
 public:
  explicit StreamReader(const uint8_t* binary, const size_t length,
                        const bool swap_endian)
      : binary_(binary), length_(length), swap_endian_(swap_endian), idx_(0) {
    (void)pad_;
  }

  bool seek_set(const uint64_t offset) {
    if (offset > length_) {
      return false;
    }

    idx_ = offset;
    return true;
  }

  bool seek_from_currect(const int64_t offset) {
    if ((int64_t(idx_) + offset) < 0) {
      return false;
    }

    if (size_t((int64_t(idx_) + offset)) > length_) {
      return false;
    }

    idx_ = size_t(int64_t(idx_) + offset);
    return true;
  }

  size_t read(const size_t n, const uint64_t dst_len, unsigned char* dst) {
    size_t len = n;
    if ((idx_ + len) > length_) {
      len = length_ - idx_;
    }

    if (len > 0) {
      if (dst_len < len) {
        // dst does not have enough space. return 0 for a while.
        return 0;
      }

      memcpy(dst, &binary_[idx_], len);
      return len;

    } else {
      return 0;
    }
  }

  bool read1(unsigned char* ret) {
    if ((idx_ + 1) > length_) {
      return false;
    }

    const unsigned char val = binary_[idx_];

    (*ret) = val;
    idx_ += 1;

    return true;
  }

  bool read2(unsigned short* ret) {
    if ((idx_ + 2) > length_) {
      return false;
    }

    unsigned short val =
        *(reinterpret_cast<const unsigned short*>(&binary_[idx_]));

    if (swap_endian_) {
      swap2(&val);
    }

    (*ret) = val;
    idx_ += 2;

    return true;
  }

  bool read4(unsigned int* ret) {
    if ((idx_ + 4) > length_) {
      return false;
    }

    unsigned int val = *(reinterpret_cast<const unsigned int*>(&binary_[idx_]));

    if (swap_endian_) {
      swap4(&val);
    }

    (*ret) = val;
    idx_ += 4;

    return true;
  }

  bool read_uint(int type, uint32_t* ret) {
    if (!ret) {
      return false;
    }

    if (type == TYPE_SHORT) {
      unsigned short val;
      if (!read2(&val)) {
        return false;
      }
      (*ret) = val;
    } else if (type == TYPE_RATIONAL) {
      unsigned int num;
      unsigned int denom;

      if (!read4(&num)) {
        return false;
      }

      if (!read4(&denom)) {
        return false;
      }

      (*ret) = num / denom;

    } else if (type == TYPE_LONG) {
      unsigned int val;

      if (!read4(&val)) {
        return false;
      }

      (*ret) = val;

    } else {
      // TODO(syoyo): Implement
      TINY_DNG_DPRINTF("Unimplemented uint type : %d\n", type);
      return false;
    }

    return true;
  }

  bool read_real(int type, double* ret) {
    if (type == TYPE_RATIONAL || type == TYPE_SRATIONAL) {
      // OK.
    } else {
      // TODO(syoyo): Support more real type.
      return false;
    }

    if (!ret) {
      return false;
    }

    if (type == TYPE_RATIONAL) {
      unsigned int num;
      unsigned int denom;

      if (!read4(&num)) {
        return false;
      }

      if (!read4(&denom)) {
        return false;
      }

      (*ret) = static_cast<double>(num) / static_cast<double>(denom);
    } else if (type == TYPE_SRATIONAL) {
      int num;
      int denom;

      if (!read4(reinterpret_cast<unsigned int*>(&num))) {
        return false;
      }

      if (!read4(reinterpret_cast<unsigned int*>(&denom))) {
        return false;
      }

      (*ret) = static_cast<double>(num) / static_cast<double>(denom);
    }

    return true;
  }

  size_t tell() const { return idx_; }

  const uint8_t* data() const { return binary_; }

  bool swap_endian() const { return swap_endian_; }

  size_t size() const { return length_; }

  ///
  /// Get TIFF Tag
  ///
  bool GetTIFFTag(unsigned short* tag, unsigned short* type, unsigned int* len,
                  unsigned int* saved_offt, const size_t additional_offset = 0);

 private:
  const uint8_t* binary_;
  const size_t length_;
  bool swap_endian_;
  char pad_[7];
  uint64_t idx_;
};

class DNGLoader {
 public:
  // TODO(syoyo): Support more tags.
  typedef enum {
    NEF_MAKERNOTE_NEF_RB_WHITE_LEVEL = 12,
    NEF_MAKERNOTE_NEF_COMPRESSION = 147,
    NEF_MAKERNOTE_LINEARLIZATION_TABLE = 150
  } NefMakerNoteTag;

  explicit DNGLoader(const std::vector<uint8_t>& binary, const bool swap_endian)
      : reader_(binary.data(), binary.size(), swap_endian) {}

  ~DNGLoader() {}

  ///
  /// Parse DNG
  ///
  bool ParseDNG(const std::vector<FieldInfo>& custom_fields,
                std::vector<tinydng::DNGImage>* images);

  ///
  /// Get error string(if exists)
  ///
  std::string error() const { return err_.str(); }

 private:
  ///
  /// Parse custom TIFF field
  ///
  bool ParseCustomField(const std::vector<FieldInfo>& field_lists,
                        const unsigned short tag, const unsigned short type,
                        const unsigned int len, FieldData* data);

  ///
  /// Parse TIFF IFD.
  /// Returns true upon success.
  ///
  bool ParseTIFFIFD(const std::vector<FieldInfo>& custom_field_lists,
                    std::vector<tinydng::DNGImage>* images);

  ///
  /// Parse EXIF tags
  ///
  bool ParseEXIFTags(StreamReader* reader);

  ///
  /// Parse Nikon maker note tags
  /// (Same with TIFF IFD format)
  ///
  bool ParseNikonMakerNoteTags(StreamReader* reader);

  ///
  /// Parse Nikon linearization table tag and construct linearization table.
  ///
  bool ParseAndConstructNikonLinearizationTable(
      StreamReader* reader, const size_t linearization_table_offset,
      const int bps, unsigned short vpred[2][2], unsigned int* split_value,
      int* tree_idx, std::vector<unsigned short>* curve);

  ///
  /// Decompress lossless JPEG data into 16bit images.
  ///
  bool DecompressLosslessJPEG(const unsigned char* src, const size_t src_length,
                              const DNGImage& image_info, const int dst_width,
                              unsigned short* dst_data);

  ///
  /// Decode Nikon RAW.
  ///
  bool DecodeNikonRAW(StreamReader* reader, const unsigned char* src,
                      const size_t src_length, const DNGImage& image_info,
                      const int dst_width, unsigned short* dst_data);

  ///
  /// Decode images. Valid after `ParseTIFFIFD'
  ///
  bool DecodeImages(std::vector<tinydng::DNGImage>* images);

  StreamReader reader_;
  EXIF exif_;
  NikonMakerNote nikon_maker_note_;
  std::stringstream err_;
};

bool StreamReader::GetTIFFTag(unsigned short* tag, unsigned short* type,
                              unsigned int* len, unsigned int* saved_offt,
                              const size_t additional_offset) {
  if (!this->read2(tag)) {
    return false;
  }

  if (!this->read2(type)) {
    return false;
  }

  if (!this->read4(len)) {
    return false;
  }

  (*saved_offt) = static_cast<unsigned int>(this->tell()) + 4;

  size_t typesize_table[] = {1, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8, 4};

  if ((*len) * (typesize_table[(*type) < 14 ? (*type) : 0]) > 4) {
    unsigned int offset;
    if (!this->read4(&offset)) {
      return false;
    }
    TINY_DNG_DPRINTF("offet = 0x%08x\n", offset);
    if (!this->seek_set(offset + additional_offset)) {
      return false;
    }
  }

  return true;
}

#if defined(TINY_DNG_ENABLE_GPL)

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wconversion"
#pragma clang diagnostic ignored "-Wold-style-cast"
#pragma clang diagnostic ignored "-Wmissing-noreturn"
#endif

#include "tiny_dng_nef_decoder.inc"

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#endif

static bool IsNikonMakerNote(StreamReader* reader) {
  size_t saved_offset = reader->tell();

  unsigned char header[16];  // 16 bytes should be enough to parse the header of
                             // makernote.

  if (!reader->read(16, 16, header)) {
    reader->seek_set(saved_offset);
    return false;
  }

  reader->seek_set(saved_offset);  // rewind.

  if (0 == std::memcmp(header, "Nikon\0", 6)) {
    return true;
  }

  return false;
}

bool DNGLoader::ParseAndConstructNikonLinearizationTable(
    StreamReader* reader, const size_t linearization_table_offset,
    const int bps, unsigned short vpred[2][2], unsigned int* split_value,
    int* tree_idx, std::vector<unsigned short>* curve) {
  uint8_t ver0, ver1;

  if (!reader->seek_set(linearization_table_offset)) {
    err_ << "Failed to seek to NEF linearization table." << std::endl;
    return false;
  }

  if (!reader->read1(&ver0)) {
    err_ << "Failed to parse NEF linearization table ver0." << std::endl;
    return false;
  }

  if (!reader->read1(&ver1)) {
    err_ << "Failed to parse NEF linearization table ver1." << std::endl;
    return false;
  }

  TINY_DNG_DPRINTF("lintable: ver0 = 0x%x\n", ver0);
  TINY_DNG_DPRINTF("lintable: ver1 = 0x%x\n", ver1);

  if (ver0 == 0x49 || ver1 == 0x58) {
    err_ << "This version of NEF is not supported yet. ver0 = " << std::hex
         << ver0 << std::dec << ", ver1 = " << std::hex << ver1 << std::dec
         << std::endl;
    return false;
  }

  (*tree_idx) = 0;
  if (ver0 == 0x46) {
    (*tree_idx) = 2;
  }
  if (bps == 14) {
    (*tree_idx) += 3;
  }

  // Read predictor(4 shorts)
  if (!reader->read2(reinterpret_cast<unsigned short*>(&vpred[0][0]))) {
    err_ << "Failed to parse NEF vpred." << std::endl;
    return false;
  }

  if (!reader->read2(reinterpret_cast<unsigned short*>(&vpred[0][1]))) {
    err_ << "Failed to parse NEF vpred." << std::endl;
    return false;
  }

  if (!reader->read2(reinterpret_cast<unsigned short*>(&vpred[1][0]))) {
    err_ << "Failed to parse NEF vpred." << std::endl;
    return false;
  }

  if (!reader->read2(reinterpret_cast<unsigned short*>(&vpred[1][1]))) {
    err_ << "Failed to parse NEF vpred." << std::endl;
    return false;
  }

  TINY_DNG_DPRINTF("vpred = %d, %d, %d, %d\n", vpred[0][0], vpred[0][1],
                   vpred[1][0], vpred[1][1]);

  // Init curve with 16bit linear table.
  curve->resize(1 << 16);
  for (size_t i = 0; i < (1 << 16); i++) {
    (*curve)[i] = static_cast<unsigned short>(i);
  }

  // Read curve size.
  unsigned short curve_size;
  if (!reader->read2(&curve_size)) {
    err_ << "Failed to parse NEF curve size." << std::endl;
    return false;
  }

  TINY_DNG_DPRINTF("lintable: curve size = %d\n", curve_size);

  int max_curve_size = (1 << bps) & 0x7FFF;

  int step = 0;
  if (curve_size > 1) {
    step = max_curve_size / (curve_size - 1);
  }

  if ((ver0 == 0x44) && (ver1 == 0x20) && (step > 0)) {  // lossy
    // read curves with iterpolation.
    for (size_t i = 0; i < curve_size; i++) {
      if (!reader->read2(&(*curve)[i * size_t(step)])) {
        err_ << "Failed to read curve data." << std::endl;
        return false;
      }
    }

    // interpolate
    for (size_t i = 0; i < size_t(max_curve_size); i++) {
      (*curve)[i] = static_cast<unsigned short>(
          ((*curve)[i - i % size_t(step)] * (size_t(step) - i % size_t(step)) +
           (*curve)[i - i % size_t(step) + size_t(step)] * (i % size_t(step))) /
          size_t(step));
    }

    // csize seems 257 for recent models.
    if (!reader->seek_set(linearization_table_offset + 562)) {
      err_ << "Failed to seek for reading split value." << std::endl;
      return false;
    }

    unsigned short split;
    if (!reader->read2(&split)) {
      err_ << "Failed to read split value." << std::endl;
      return false;
    }

    (*split_value) = split;

  } else if ((ver0 != 0x46) && (curve_size <= 16385)) {  // if not lossless
    max_curve_size = curve_size;

    curve->resize(curve_size);
    for (size_t i = 0; i < curve_size; i++) {
      if (!reader->read2(&(*curve)[i])) {
        err_ << "Failed to read curve data." << std::endl;
        return false;
      }
    }
  } else {
    // No linearization curve.
  }

  // Truncate curve if necessary
  {
    TINY_DNG_DPRINTF("max_curve_size %d\n", max_curve_size);
    TINY_DNG_ASSERT(max_curve_size >= 2, "`max_curve_size' must be >= 2.");

    while ((*curve)[size_t(max_curve_size) - 2] ==
           (*curve)[size_t(max_curve_size) - 1]) {
      TINY_DNG_DPRINTF("max_curve_size %d\n", max_curve_size);
      max_curve_size--;
    }
    curve->resize(size_t(max_curve_size));
  }

  return false;
}

// Parse Nikon maker note tags
// (Same with TIFF IFD format)
bool DNGLoader::ParseNikonMakerNoteTags(StreamReader* reader) {
  // [0: 5] Magic value

  // read version
  short ver0;
  short ver1;

  if (!reader->seek_from_currect(6)) {  // skip magic value(6 bytes)
    err_ << "Failed to seek in ParseNikonMakerNoteTags." << std::endl;
    return false;
  }

  // [6:7]
  if (!reader->read2(reinterpret_cast<unsigned short*>(&ver0))) {
    err_ << "Failed to read ver0." << std::endl;
    return false;
  }

  // [8:9]
  if (!reader->read2(reinterpret_cast<unsigned short*>(&ver1))) {
    err_ << "Failed to read ver1." << std::endl;
    return false;
  }

  TINY_DNG_DPRINTF("NEF MakerNote ver0 %d, ver1 %d\n", ver0, ver1);

  // Save the beginning of MakerNote TIFF to calculate offset position
  // for linearization table.
  size_t makernote_offset = reader->tell();

  // Usually 0x4D4D
  // [10:11]
  short byte_order;
  if (!reader->read2(reinterpret_cast<unsigned short*>(&byte_order))) {
    err_ << "Failed to read byte order." << std::endl;
    return false;
  }

  TINY_DNG_DPRINTF("NEF makernote byte_order 0x%08x\n", byte_order);

  // [12:13] TIFF magic value.
  // skip TIFF magic value.
  if (!reader->seek_from_currect(2)) {
    err_ << "Failed to seek." << std::endl;
    return false;
  }

  // [14:17] TIFF offset.
  unsigned int tiff_offset;
  if (!reader->read4(&tiff_offset)) {
    err_ << "Failed to read Nikon MakerNote TIFF offset." << std::endl;
    return false;
  }

  TINY_DNG_DPRINTF("NEF makernote tiff_offset 0x%08x\n",
                   tiff_offset);  // must be 8

  // [18:] First IFD
  unsigned short num_entries;
  if (!reader->read2(&num_entries)) {
    err_ << "Failed to read NEF Makernote entries." << std::endl;
    return false;
  }

  TINY_DNG_DPRINTF("NEF Makernote entries = %d\n", num_entries);
  if (num_entries == 0) {
    err_ << "# of NEF Makernote entries are zero." << std::endl;
    return false;
  }

  while (num_entries--) {
    unsigned short tag = 0, type = 0;
    unsigned int len = 0;
    unsigned int saved_offset = 0;
    // Offset is based on makernote[12]. Add additional offsets to get actual
    // offset in the input data stream.
    if (!reader->GetTIFFTag(&tag, &type, &len, &saved_offset,
                            makernote_offset)) {
      err_ << "Failed to parse Makernote tag." << std::endl;
      return false;
    }
    TINY_DNG_DPRINTF(
        "NEF Makernote tag. tag = %d, type = %d, len = %d, saved = %d\n", tag,
        type, len, saved_offset);

    // http://www.exiv2.org/tags-nikon.html
    if (tag == NEF_MAKERNOTE_NEF_RB_WHITE_LEVEL) {
      double red = 0.0, green1 = 1.0, blue = 0.0, green2 = 1.0;
      if (len == 4) {
        if (!reader->read_real(type, &red)) {
          err_ << "Failed to read RB white level." << std::endl;
          return false;
        }
        if (!reader->read_real(type, &blue)) {
          err_ << "Failed to read RB white level." << std::endl;
          return false;
        }
        if (!reader->read_real(type, &green1)) {
          err_ << "Failed to read RB white level." << std::endl;
          return false;
        }
        if (!reader->read_real(type, &green2)) {
          err_ << "Failed to read RB white level." << std::endl;
          return false;
        }

        // Red and Blue are normalized so that Green become 1.0,
        // so assume green1 and green2 is 1.0
        nikon_maker_note_.white_balance[0] = red;
        nikon_maker_note_.white_balance[1] = green1;
        nikon_maker_note_.white_balance[2] = blue;
        nikon_maker_note_.white_balance[3] = green2;
        TINY_DNG_DPRINTF("while_level %f, %f, %f, %f\n",
          red, green1, blue, green2);
      }
        

      
    } if (tag == NEF_MAKERNOTE_NEF_COMPRESSION) {
      unsigned short compression;
      if (!reader->read2(&compression)) {
        err_ << "Failed to read NEF Makernote compression tag." << std::endl;
        return false;
      }
      TINY_DNG_DPRINTF("NEF compression = %d\n", compression);
      nikon_maker_note_.compression = compression;
    } else if (tag == NEF_MAKERNOTE_LINEARLIZATION_TABLE) {
      TINY_DNG_DPRINTF("NEF linearization table tag len = %d\n", len);

      // NOTE Offset to lineratization table is based on the beginning of EXIF
      // IFDs.
      // Actual offset to linearization table in the input data is already
      // adjusted by `offset_to_exif`.
      // Actual decoding of linearization table will be done in decoing images.
      // Just save offset value to lineratization table here.
      nikon_maker_note_.linearization_table_offset = reader->tell();
    } else {
      // TODO(syoyo): Implement other tags.
    }

    if (!reader->seek_set(saved_offset)) {
      err_ << "Failed to restore position." << std::endl;
      return false;
    }
  }

  return true;
}

// Parse some EXIF tag
bool DNGLoader::ParseEXIFTags(StreamReader* reader) {
  unsigned short num_entries;
  if (!reader->read2(&num_entries)) {
    err_ << "Failed to read # of IFDs in ParseEXIFTags." << std::endl;
    return false;
  }

  TINY_DNG_DPRINTF("EXIF entries = %d\n", num_entries);
  if (num_entries == 0) {
    assert(0);
    return false;  // @fixme
  }

  while (num_entries--) {
    unsigned short tag = 0, type = 0;
    unsigned int len = 0;
    unsigned int saved_offet = 0;
    reader->GetTIFFTag(&tag, &type, &len, &saved_offet);

    TINY_DNG_DPRINTF("EXIF tag = %d, type = %d, len = %d\n", tag, type, len);

    if (tag == EXIF_MAKER_NOTE) {
      TINY_DNG_DPRINTF("maker note! len = %d\n", len);

      if (IsNikonMakerNote(reader)) {
        TINY_DNG_DPRINTF("Nikon NEF\n");

        bool ret = ParseNikonMakerNoteTags(reader);
        if (!ret) {
          return false;
        }
      }
    } else {
      // Unsupported or unimplemented tag.
    }

    if (!reader->seek_set(saved_offet)) {
      err_ << "Failed to restore position." << std::endl;
      return false;
    }
  }

  return true;
}

bool DNGLoader::ParseCustomField(const std::vector<FieldInfo>& field_lists,
                                 const unsigned short tag,
                                 const unsigned short type,
                                 const unsigned int len, FieldData* data) {
  bool found = false;

  (void)len;

  // Simple linear search.
  // TODO(syoyo): Use binary search for faster procesing.
  for (size_t i = 0; i < field_lists.size(); i++) {
    if ((field_lists[i].tag > TAG_NEW_SUBFILE_TYPE) &&
        (field_lists[i].tag == tag) && (field_lists[i].type == type)) {
      if ((type == TYPE_BYTE) || (type == TYPE_SBYTE)) {
        data->name = field_lists[i].name;
        data->type = static_cast<DataType>(type);
        unsigned char val;
        size_t n = reader_.read(1, 1, &val);
        if (n != 1) {
          err_ << "Failed to read custom TIFF field with TYPE_BYTE or "
                  "TYPE_SBYTE."
               << std::endl;
          return false;
        }
        data->data.resize(1);
        data->data[0] = val;
        found = true;
      } else if ((type == TYPE_SHORT) || (type == TYPE_SSHORT)) {
        data->name = field_lists[i].name;
        data->type = static_cast<DataType>(type);

        unsigned short val;
        if (!reader_.read2(&val)) {
          err_ << "Failed to read custom TIFF field with TYPE_SHORT or "
                  "TYPE_SSHORT."
               << std::endl;
          return false;
        }

        data->data.resize(sizeof(short));
        memcpy(data->data.data(), &val, sizeof(short));
        found = true;
      } else if ((type == TYPE_LONG) || (type == TYPE_SLONG) ||
                 (type == TYPE_FLOAT)) {
        data->name = field_lists[i].name;
        data->type = static_cast<DataType>(type);
        unsigned int val;
        if (!reader_.read4(&val)) {
          err_ << "Failed to read custom TIFF field with TYPE_LONG, TYPE_SLONG "
                  "or TYPE_FLOAT."
               << std::endl;
          return false;
        }

        data->data.resize(sizeof(int));
        memcpy(data->data.data(), &val, sizeof(int));
        found = true;
      } else if ((type == TYPE_RATIONAL) || (type == TYPE_SRATIONAL)) {
        data->name = field_lists[i].name;
        data->type = static_cast<DataType>(type);
        unsigned int num;
        unsigned int denom;

        {
          size_t n = reader_.read(4, 4, reinterpret_cast<unsigned char*>(&num));
          if (n != 4) {
            err_ << "Failed to read custom TIFF field with TYPE_RATIONAL or "
                    "TYPE_SRATIONAL."
                 << std::endl;
            return false;
          }
        }

        {
          size_t n =
              reader_.read(4, 4, reinterpret_cast<unsigned char*>(&denom));
          if (n != 4) {
            err_ << "Failed to read custom TIFF field with TYPE_RATIONAL or "
                    "TYPE_SRATIONAL."
                 << std::endl;
            return false;
          }
        }

        data->data.resize(sizeof(int) * 2);

        // Store rational value as is.
        memcpy(&data->data[0], &num, 4);
        memcpy(&data->data[4], &denom, 4);
      } else {
        // TODO(syoyo): Support more data types.
      }
    }
  }

  return found;
}

// Parse TIFF IFD.
// Returns true upon success, false if failed to parse.
bool DNGLoader::ParseTIFFIFD(const std::vector<FieldInfo>& custom_field_lists,
                             std::vector<tinydng::DNGImage>* images) {
  tinydng::DNGImage image;

  // TINY_DNG_DPRINTF("id = %d\n", idx);
  unsigned short num_entries;
  if (!reader_.read2(&num_entries)) {
    err_ << "Failed to read # of entries." << std::endl;
    return false;
  }

  if (num_entries == 0) {
    err_ << "# of IFD entries is zero." << std::endl;
    return false;
  }

  TINY_DNG_DPRINTF("----------\n");
  TINY_DNG_DPRINTF("num entries %d\n", num_entries);

  // For delayed reading of strip offsets and strip byte counts.
  long offt_strip_offset = 0;
  long offt_strip_byte_counts = 0;

  while (num_entries--) {
    unsigned short tag = 0, type = 0;
    unsigned int len = 0;
    unsigned int saved_offt = 0;
    if (!reader_.GetTIFFTag(&tag, &type, &len, &saved_offt)) {
      err_ << "Failed to read TIFF tag." << std::endl;
      return false;
    }

    TINY_DNG_DPRINTF("tag %d\n", tag);
    TINY_DNG_DPRINTF("type %d\n", tag);
    TINY_DNG_DPRINTF("len %d\n", tag);
    TINY_DNG_DPRINTF("saved_offt %d\n", saved_offt);

    if (tag < TAG_NEW_SUBFILE_TYPE) {
      err_ << "Invalid tag value. The value must be greater than or equal to "
           << TAG_NEW_SUBFILE_TYPE << ", but got " << tag << std::endl;
      return false;
    }

    // TINY_DNG_DPRINTF("tag = %d\n", tag);

    switch (tag) {
      case 2:
      case TAG_IMAGE_WIDTH:
      case 61441:  // ImageWidth
      {
        uint32_t val;
        if (!reader_.read_uint(type, &val)) {
          err_ << "Failed to read image width value." << std::endl;
          return false;
        }
        image.width = static_cast<int>(val);
        TINY_DNG_DPRINTF("image width = %d\n", image.width);
      } break;

      case 3:
      case TAG_IMAGE_HEIGHT:
      case 61442:  // ImageHeight
      {
        uint32_t val;
        if (!reader_.read_uint(type, &val)) {
          err_ << "Failed to read image height value." << std::endl;
          return false;
        }
        image.height = static_cast<int>(val);
        TINY_DNG_DPRINTF("image height = %d\n", image.height);
      } break;

      case TAG_BITS_PER_SAMPLE:
      case 61443:  // BitsPerSample
      {
        uint32_t val;
        if (!reader_.read_uint(type, &val)) {
          err_ << "Failed to read bits per sample value." << std::endl;
          return false;
        }
        image.bits_per_sample_original = static_cast<int>(val);
        TINY_DNG_DPRINTF("bits per sample = %d\n", val);
      } break;

      case TAG_SAMPLES_PER_PIXEL: {
        unsigned short val;
        if (!reader_.read2(&val)) {
          err_ << "Failed to read samples per pixel value." << std::endl;
          return false;
        }

        image.samples_per_pixel = static_cast<int>(val);
        TINY_DNG_DPRINTF("spp = %d\n", image.samples_per_pixel);
        TINY_DNG_ASSERT(image.samples_per_pixel <= 4,
                        "Samples per pixel must be less than or equal to 4.");
      } break;

      case TAG_ROWS_PER_STRIP: {
        // The TIFF Spec says data type may be SHORT, but assume LONG for a
        // while.

        unsigned int val;
        if (!reader_.read4(&val)) {
          err_ << "Failed to read rows per strip." << std::endl;
        }
        image.rows_per_strip = static_cast<int>(val);
        TINY_DNG_ASSERT(
            image.height > 0,
            "Must have image height before parsing ROWS_PER_STRIP tag.");

        // http://www.awaresystems.be/imaging/tiff/tifftags/rowsperstrip.html
        image.strips_per_image = static_cast<int>(
            floor(double(image.height + image.rows_per_strip - 1) /
                  double(image.rows_per_strip)));
        TINY_DNG_DPRINTF("rows_per_strip = %d\n", image.samples_per_pixel);
        TINY_DNG_DPRINTF("strips_per_image = %d\n", image.strips_per_image);
      } break;

      case TAG_COMPRESSION: {
        unsigned int val;
        if (!reader_.read_uint(type, &val)) {
          err_ << "Failed to read compression value." << std::endl;
          return false;
        }

        image.compression = static_cast<int>(val);
        TINY_DNG_DPRINTF("compression = %d\n", image.compression);
      } break;

      case TAG_STRIP_OFFSET:
      case TAG_JPEG_IF_OFFSET: {
        offt_strip_offset = static_cast<long>(reader_.tell());
        unsigned int offset;
        if (!reader_.read4(&offset)) {
          err_ << "Failed to read offset tag [" << tag << "]." << std::endl;
          return false;
        }
        image.offset = offset;
        TINY_DNG_DPRINTF("strip_offset = %d\n", image.offset);
      } break;

      case TAG_JPEG_IF_BYTE_COUNT: {
        unsigned int val;
        if (!reader_.read4(&val)) {
          err_ << "Failed to read JPEG IF byte count." << std::endl;
          return false;
        }
        image.jpeg_byte_count = static_cast<int>(val);
      } break;

      case TAG_ORIENTATION: {
        unsigned short val;
        if (!reader_.read2(&val)) {
          err_ << "Failed to read orientation." << std::endl;
          return false;
        }
        image.orientation = val;
      } break;

      case TAG_STRIP_BYTE_COUNTS: {
        offt_strip_byte_counts = static_cast<long>(reader_.tell());

        unsigned int val;
        if (!reader_.read4(&val)) {
          err_ << "Failed to read strip byte counts." << std::endl;
          return false;
        }
        image.strip_byte_count = static_cast<int>(val);
        TINY_DNG_DPRINTF("strip_byte_count = %d\n", image.strip_byte_count);
      } break;

      case TAG_PLANAR_CONFIGURATION: {
        unsigned short val;
        if (!reader_.read2(&val)) {
          err_ << "Failed to read planar configuration." << std::endl;
          return false;
        }
        image.planar_configuration = val;
      }

      break;

      case TAG_PREDICTOR: {
        unsigned short val;
        if (!reader_.read2(&val)) {
          err_ << "Failed to read predictor." << std::endl;
          return false;
        }
        TINY_DNG_ASSERT((image.predictor >= 1 && image.predictor <= 3),
                        "Invalid predictor value.");
        image.predictor = val;
      } break;

      case TAG_SAMPLE_FORMAT: {
        unsigned short val;
        if (!reader_.read2(&val)) {
          err_ << "Failed to read sample format." << std::endl;
          return false;
        }
        short format = static_cast<short>(val);
        if ((format == SAMPLEFORMAT_INT) || (format == SAMPLEFORMAT_UINT) ||
            (format == SAMPLEFORMAT_IEEEFP)) {
          image.sample_format = static_cast<SampleFormat>(format);
        }
      } break;

      case TAG_SUB_IFDS:

      {
        // TINY_DNG_DPRINTF("sub_ifds = %d\n", len);
        for (size_t k = 0; k < len; k++) {
          unsigned int i = static_cast<unsigned int>(reader_.tell());
          unsigned int offt;
          if (!reader_.read4(&offt)) {
            err_ << "Failed to read offset in sub ifds." << std::endl;
            return false;
          }
          unsigned int base = 0;  // @fixme

          if (!reader_.seek_set(offt + base)) {
            err_ << "Failed to seekin sub ifds." << std::endl;
            return false;
          }

          ParseTIFFIFD(custom_field_lists, images);  // recursive call

          if (!reader_.seek_set(i + 4)) {  // rewind
            err_ << "Failed to rewind." << std::endl;
            return false;
          }
        }
        // TINY_DNG_DPRINTF("sub_ifds DONE\n");
      }

      break;

      case TAG_TILE_WIDTH: {
        unsigned int val;
        if (!reader_.read_uint(type, &val)) {
          err_ << "Failed to read tile width." << std::endl;
          return false;
        }
        image.tile_width = static_cast<int>(val);
        TINY_DNG_DPRINTF("tile_width = %d\n", image.tile_width);
      } break;

      case TAG_TILE_LENGTH: {
        unsigned int val;
        if (!reader_.read_uint(type, &val)) {
          err_ << "Failed to read tile lentgh." << std::endl;
          return false;
        }
        image.tile_length = static_cast<int>(val);
        TINY_DNG_DPRINTF("tile_length = %d\n", image.tile_length);
      } break;

      case TAG_TILE_OFFSETS: {
        // Count N depends on PLANAR_CONFIGURATION parameter.
        // Assume PLANAR_CONFIGURATION tag is already parsed.
        size_t n = 0;
        int tiles_across =
            (image.width + image.tile_width - 1) / image.tile_width;
        int tiles_down =
            (image.height + image.tile_length - 1) / image.tile_length;
        int tiles_per_image = tiles_across * tiles_down;

        TINY_DNG_DPRINTF("tiles_across = %d\n", tiles_across);
        TINY_DNG_DPRINTF("tiles_down = %d\n", tiles_down);
        TINY_DNG_DPRINTF("tiles_per_image = %d\n", tiles_per_image);

        if (image.planar_configuration == 1) {
          n = size_t(tiles_per_image);
        } else if (image.planar_configuration == 2) {
          n = size_t(image.samples_per_pixel * tiles_per_image);
        } else {
          err_ << "Invalid planar configuration value ["
               << image.planar_configuration << "]." << std::endl;
        }

        for (size_t k = 0; k < n; k++) {
          unsigned int val;
          if (!reader_.read4(&val)) {
            err_ << "Failed to read tile byte offsets." << std::endl;
            return false;
          }
          image.tile_offsets.push_back(val);
          TINY_DNG_DPRINTF("tile_offsets[%d] = %d\n", int(k), int(val));
        }
      } break;

      case TAG_TILE_BYTE_COUNTS: {
        // Count N depends on PLANAR_CONFIGURATION parameter.
        // Assume PLANAR_CONFIGURATION tag is already parsed.
        size_t n = 0;
        int tiles_across =
            (image.width + image.tile_width - 1) / image.tile_width;
        int tiles_down =
            (image.height + image.tile_length - 1) / image.tile_length;
        int tiles_per_image = tiles_across * tiles_down;

        TINY_DNG_DPRINTF("tiles_across = %d\n", tiles_across);
        TINY_DNG_DPRINTF("tiles_down = %d\n", tiles_down);
        TINY_DNG_DPRINTF("tiles_per_image = %d\n", tiles_per_image);

        if (image.planar_configuration == 1) {
          n = size_t(tiles_per_image);
        } else if (image.planar_configuration == 2) {
          n = size_t(image.samples_per_pixel * tiles_per_image);
        } else {
          err_ << "Invalid planar configuration value ["
               << image.planar_configuration << "]." << std::endl;
        }

        for (size_t k = 0; k < n; k++) {
          unsigned int val;
          if (!reader_.read4(&val)) {
            err_ << "Failed to read tile byte offsets." << std::endl;
            return false;
          }
          image.tile_byte_counts.push_back(val);
          TINY_DNG_DPRINTF("tile_byte_counts[%d] = %d\n", int(k), int(val));
        }

      } break;

      case TAG_EXIF: {
        EXIF exif;
        uint32_t offset;
        if (!reader_.read4(&offset)) {
          err_ << "Failed to read EXIF offset." << std::endl;
          return false;
        }

        TINY_DNG_DPRINTF("exif offset = %d\n", int(offset));
        if (!reader_.seek_set(offset)) {
          err_ << "Failed to seek EXIF offset." << std::endl;
          return false;
        }

        bool ret = ParseEXIFTags(&reader_);
        if (!ret) {
          err_ << "Failed to parse EXIF tags." << std::endl;
          return false;
        }
      } break;

      case TAG_CFA_PATTERN_DIM: {
        unsigned short val;
        if (!reader_.read2(&val)) {
          err_ << "Failed to read CFA pattern dim." << std::endl;
          return false;
        }
        image.cfa_pattern_dim = val;
      } break;

      case TAG_CFA_PATTERN: {
        char buf[16];
        size_t readLen = len;
        if (readLen > 16) readLen = 16;
        // Assume 2x2 CFAPattern.
        TINY_DNG_ASSERT(readLen == 4, "non 2x2 CFA pattern is not supported.");
        if (!reader_.read(readLen, 16, reinterpret_cast<unsigned char*>(buf))) {
          err_ << "Failed to read CFA pattern." << std::endl;
          return false;
        }
        image.cfa_pattern[0][0] = buf[0];
        image.cfa_pattern[0][1] = buf[1];
        image.cfa_pattern[1][0] = buf[2];
        image.cfa_pattern[1][1] = buf[3];
      } break;

      case TAG_DNG_VERSION: {
        unsigned char data[4];
        for (size_t k = 0; k < 4; k++) {
          if (!reader_.read1(&data[k])) {
            err_ << "Failed to read CFA pattern." << std::endl;
            return false;
          }
        }

        image.version =
            (data[3] << 24) | (data[2] << 16) | (data[1] << 8) | data[0];
      } break;

      case TAG_CFA_PLANE_COLOR: {
        char buf[4];
        size_t readLen = len;
        if (readLen > 4) readLen = 4;
        if (!reader_.read(4, 4, reinterpret_cast<unsigned char*>(buf))) {
          err_ << "Failed to read CFA plane color." << std::endl;
          return false;
        }
        for (size_t i = 0; i < readLen; i++) {
          image.cfa_plane_color[i] = buf[i];
        }
      } break;

      case TAG_CFA_LAYOUT: {
        unsigned short val;
        if (!reader_.read2(&val)) {
          err_ << "Failed to read CFA layout." << std::endl;
          return false;
        }
        image.cfa_layout = val;
      } break;

      case TAG_ACTIVE_AREA: {
        for (size_t k = 0; k < 4; k++) {
          unsigned int val;
          if (!reader_.read_uint(type, &val)) {
            err_ << "Failed to read active area." << std::endl;
            return false;
          }
          image.active_area[k] = static_cast<int>(val);
        }
        image.has_active_area = true;
      } break;

      case TAG_BLACK_LEVEL: {
        // Assume TAG_SAMPLES_PER_PIXEL is read before
        // FIXME(syoyo): scan TAG_SAMPLES_PER_PIXEL in IFD table in advance.
        for (int s = 0; s < image.samples_per_pixel; s++) {
          unsigned int val;
          if (!reader_.read_uint(type, &val)) {
            err_ << "Failed to read black level." << std::endl;
            return false;
          }
          image.black_level[s] = static_cast<int>(val);
        }
      } break;

      case TAG_WHITE_LEVEL: {
        // Assume TAG_SAMPLES_PER_PIXEL is read before
        // FIXME(syoyo): scan TAG_SAMPLES_PER_PIXEL in IFD table in advance.
        for (int s = 0; s < image.samples_per_pixel; s++) {
          unsigned int val;
          if (!reader_.read_uint(type, &val)) {
            err_ << "Failed to read white level." << std::endl;
            return false;
          }
          image.white_level[s] = static_cast<int>(val);
        }
      } break;

      case TAG_ANALOG_BALANCE:
        // Assume RGB
        if (!reader_.read_real(type, &image.analog_balance[0])) {
          err_ << "Failed to read alalog balance0." << std::endl;
          return false;
        }
        if (!reader_.read_real(type, &image.analog_balance[1])) {
          err_ << "Failed to read alalog balance1." << std::endl;
          return false;
        }
        if (!reader_.read_real(type, &image.analog_balance[2])) {
          err_ << "Failed to read alalog balance2." << std::endl;
          return false;
        }
        image.has_analog_balance = true;
        break;

      case TAG_AS_SHOT_NEUTRAL:
        // Assume RGB
        // TINY_DNG_DPRINTF("ty = %d\n", type);
        if (!reader_.read_real(type, &image.as_shot_neutral[0])) {
          err_ << "Failed to read as shot neutral0." << std::endl;
          return false;
        }
        if (!reader_.read_real(type, &image.as_shot_neutral[1])) {
          err_ << "Failed to read as shot neutral1." << std::endl;
          return false;
        }
        if (!reader_.read_real(type, &image.as_shot_neutral[2])) {
          err_ << "Failed to read as shot neutral2." << std::endl;
          return false;
        }
        image.has_as_shot_neutral = true;
        break;

      case TAG_CALIBRATION_ILLUMINANT1: {
        unsigned short val;
        if (!reader_.read2(&val)) {
          err_ << "Failed to read calibration illuminant1." << std::endl;
          return false;
        }
        image.calibration_illuminant1 = static_cast<LightSource>(val);
      } break;

      case TAG_CALIBRATION_ILLUMINANT2: {
        unsigned short val;
        if (!reader_.read2(&val)) {
          err_ << "Failed to read calibration illuminant2." << std::endl;
          return false;
        }
        image.calibration_illuminant2 = static_cast<LightSource>(val);
      } break;

      case TAG_COLOR_MATRIX1: {
        for (int c = 0; c < 3; c++) {
          for (int k = 0; k < 3; k++) {
            double val;
            if (!reader_.read_real(type, &val)) {
              err_ << "Failed to read color matrix1." << std::endl;
              return false;
            }
            image.color_matrix1[c][k] = val;
          }
        }
      } break;

      case TAG_COLOR_MATRIX2: {
        for (int c = 0; c < 3; c++) {
          for (int k = 0; k < 3; k++) {
            double val;
            if (!reader_.read_real(type, &val)) {
              err_ << "Failed to read color matrix2." << std::endl;
              return false;
            }
            image.color_matrix2[c][k] = val;
          }
        }
      } break;

      case TAG_FORWARD_MATRIX1: {
        {
          for (int c = 0; c < 3; c++) {
            for (int k = 0; k < 3; k++) {
              double val;
              if (!reader_.read_real(type, &val)) {
                err_ << "Failed to read forward matrix1." << std::endl;
                return false;
              }
              image.forward_matrix1[c][k] = val;
            }
          }
        }
      } break;

      case TAG_FORWARD_MATRIX2: {
        for (int c = 0; c < 3; c++) {
          for (int k = 0; k < 3; k++) {
            double val;
            if (!reader_.read_real(type, &val)) {
              err_ << "Failed to read forward matrix2." << std::endl;
              return false;
            }
            image.forward_matrix2[c][k] = val;
          }
        }
      } break;

      case TAG_CAMERA_CALIBRATION1: {
        for (int c = 0; c < 3; c++) {
          for (int k = 0; k < 3; k++) {
            double val;
            if (!reader_.read_real(type, &val)) {
              err_ << "Failed to read camera calibration1." << std::endl;
              return false;
            }
            image.camera_calibration1[c][k] = val;
          }
        }
      } break;

      case TAG_CAMERA_CALIBRATION2: {
        for (int c = 0; c < 3; c++) {
          for (int k = 0; k < 3; k++) {
            double val;
            if (!reader_.read_real(type, &val)) {
              err_ << "Failed to read camera calibration2." << std::endl;
              return false;
            }
            image.camera_calibration2[c][k] = val;
          }
        }
      } break;

      case TAG_CR2_SLICES: {
        {
          // Assume 3 ushorts;
          unsigned short vals[3];
          if (!reader_.read2(&vals[0])) {
            err_ << "Failed to read CR2 slice0 value." << std::endl;
          }
          if (!reader_.read2(&vals[1])) {
            err_ << "Failed to read CR2 slice1 value." << std::endl;
          }
          if (!reader_.read2(&vals[2])) {
            err_ << "Failed to read CR2 slice2 value." << std::endl;
          }

          image.cr2_slices[0] = vals[0];
          image.cr2_slices[1] = vals[1];
          image.cr2_slices[2] = vals[2];
          TINY_DNG_DPRINTF("cr2_slices = %d, %d, %d\n", image.cr2_slices[0],
                           image.cr2_slices[1], image.cr2_slices[2]);
        }
      } break;

      default: {
        FieldData data;
        bool found =
            ParseCustomField(custom_field_lists, tag, type, len, &data);
        if (found) {
          image.custom_fields.push_back(data);
        }
        // TINY_DNG_DPRINTF("unknown or unsupported tag = %d\n", tag);
      }
    }

    if (!reader_.seek_set(saved_offt)) {
      err_ << "Failed to seek saved offset position." << std::endl;
      return false;
    }
  }

  // Delayed read of strip offsets and strip byte counts
  if (image.strips_per_image > 0) {
    image.strip_byte_counts.clear();
    image.strip_offsets.clear();

    size_t curr_offt = reader_.tell();

    if (offt_strip_byte_counts > 0) {
      if (!reader_.seek_set(size_t(offt_strip_byte_counts))) {
        err_ << "Failed to seek the position for strip byte counts."
             << std::endl;
        return false;
      }

      for (int k = 0; k < image.strips_per_image; k++) {
        unsigned int strip_byte_count;
        if (!reader_.read4(&strip_byte_count)) {
          err_ << "Failed to read strip byte count." << std::endl;
          return false;
        }
        TINY_DNG_DPRINTF("strip_byte_counts[%d] = %u\n", k, strip_byte_count);
        image.strip_byte_counts.push_back(strip_byte_count);
      }
    }

    if (offt_strip_offset > 0) {
      if (!reader_.seek_set(size_t(offt_strip_offset))) {
        err_ << "Failed to seek the position for strip offset." << std::endl;
        return false;
      }

      for (int k = 0; k < image.strips_per_image; k++) {
        unsigned int strip_offset;
        if (!reader_.read4(&strip_offset)) {
          err_ << "Failed to read strip offset." << std::endl;
          return false;
        }
        TINY_DNG_DPRINTF("strip_offset[%d] = %u\n", k, strip_offset);
        image.strip_offsets.push_back(strip_offset);
      }
    }

    if (!reader_.seek_set(curr_offt)) {
      err_ << "Failed to seek saved offset position(image strip)." << std::endl;
      return false;
    }
  }

  // Add to images.
  images->push_back(image);

  TINY_DNG_DPRINTF("DONE ---------\n");

  return true;
}

// C++03 and tiff lzw port of https://github.com/glampert/compression-algorithms
// =============================================================================
//
// File: lzw.hpp
// Author: Guilherme R. Lampert
// Created on: 17/02/16
// Brief: LZW encoder/decoder in C++11 with varying length dictionary codes.
//
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wpadded"
#endif
namespace lzw {

class Dictionary {
 public:
  struct Entry {
    int code;
    int value;
  };

  // Dictionary entries 0-255 are always reserved to the byte/ASCII range.
  int size_;
  Entry entries_[4096];

  Dictionary();
  int findIndex(int code, int value) const;
  bool add(int code, int value);
  bool flush(int& codeBitsWidth);
  void init();
  int size() { return size_; }
};

// ========================================================
// class Dictionary:
// ========================================================

Dictionary::Dictionary() { init(); }

void Dictionary::init() {
  // First 256 dictionary entries are reserved to the byte/ASCII
  // range. Additional entries follow for the character sequences
  // found in the input. Up to 4096 - 256 (MaxDictEntries - FirstCode).
  size_ = 256;
  for (int i = 0; i < size_; ++i) {
    entries_[i].code = -1;
    entries_[i].value = i;
  }

  // 256 is reserved for ClearCode, 257 is reserved for end of stream, thus
  // FistCode starts with 258
  size_ = 258;  // = 256 + 2
}

int Dictionary::findIndex(const int code, const int value) const {
  if (code == -1) {
    return value;
  }

  // Linear search for now.
  // TODO: Worth optimizing with a proper hash-table?
  for (int i = 0; i < size_; ++i) {
    if (entries_[i].code == code && entries_[i].value == value) {
      return i;
    }
  }

  return -1;
}

bool Dictionary::add(const int code, const int value) {
  TINY_DNG_ASSERT(code <= size_,
                  "`code' must be less than or equal to dictionary size.");
  if (size_ == 4096) {
    TINY_DNG_DPRINTF("Dictionary overflowed!");
    return false;
  }

  TINY_DNG_DPRINTF("add[%d].code = %d\n", size_, code);
  TINY_DNG_DPRINTF("add[%d].value = %d\n", size_, value);
  entries_[size_].code = code;
  entries_[size_].value = value;
  ++size_;
  return true;
}

bool Dictionary::flush(int& codeBitsWidth) {
  if (size_ == ((1 << codeBitsWidth) - 1)) {
    ++codeBitsWidth;
    TINY_DNG_DPRINTF("expand: bits %d\n", codeBitsWidth);
    if (codeBitsWidth > 12)  // MaxDictBits
    {
      // Clear the dictionary (except the first 256 byte entries).
      codeBitsWidth = 9;  // StartBits
      size_ = 256 + 2;    // 256 is reserved for ClearCode, 257 is reserved for
                          // end of stream, thus FistCode starts with 258
      return true;
    }
  }
  return false;
}

class BitStreamReader {
 public:
  // No copy/assignment.
  // BitStreamReader(const BitStreamReader &) = delete;
  // BitStreamReader & operator = (const BitStreamReader &) = delete;

  // BitStreamReader(const BitStreamWriter & bitStreamWriter);
  BitStreamReader(const uint8_t* bitStream, int byteCount, int bitCount);

  bool isEndOfStream() const;
  bool readNextBitLE(int& bitOut);       // little endian
  bool readNextBitBE(int& bitOut);       // big endian
  uint64_t readBitsU64LE(int bitCount);  // little endian
  uint64_t readBitsU64BE(int bitCount);  // big endian
  void reset();

 private:
  const uint8_t*
      stream;  // Pointer to the external bit stream. Not owned by the reader.
  const int
      sizeInBytes;  // Size of the stream *in bytes*. Might include padding.
  const int sizeInBits;  // Size of the stream *in bits*, padding *not* include.
  int currBytePos;       // Current byte being read in the stream.
  int nextBitPos;   // Bit position within the current byte to access next. 0 to
                    // 7.
  int numBitsRead;  // Total bits read from the stream so far. Never includes
                    // byte-rounding padding.
};

// BitStreamReader::BitStreamReader(const BitStreamWriter & bitStreamWriter)
//    : stream(bitStreamWriter.getBitStream())
//    , sizeInBytes(bitStreamWriter.getByteCount())
//    , sizeInBits(bitStreamWriter.getBitCount())
//{
//    reset();
//}

BitStreamReader::BitStreamReader(const unsigned char* bitStream,
                                 const int byteCount, const int bitCount)
    : stream(bitStream), sizeInBytes(byteCount), sizeInBits(bitCount) {
  (void)sizeInBytes;
  reset();
}

bool BitStreamReader::readNextBitLE(int& bitOut) {
  if (numBitsRead >= sizeInBits) {
    return false;  // We are done.
  }

  const uint32_t mask = uint32_t(1) << nextBitPos;
  bitOut = !!(stream[currBytePos] & mask);
  ++numBitsRead;

  if (++nextBitPos == 8) {
    nextBitPos = 0;
    ++currBytePos;
  }
  return true;
}

bool BitStreamReader::readNextBitBE(int& bitOut) {
  if (numBitsRead >= sizeInBits) {
    return false;  // We are done.
  }

  const uint32_t mask = uint32_t(1) << (7 - nextBitPos);
  bitOut = !!(stream[currBytePos] & mask);
  ++numBitsRead;

  if (++nextBitPos == 8) {
    nextBitPos = 0;
    ++currBytePos;
  }
  return true;
}

uint64_t BitStreamReader::readBitsU64LE(const int bitCount) {
  TINY_DNG_ASSERT(bitCount <= 64,
                  "`bitCount' must be less than or equal to 64.");

  uint64_t num = 0;
  for (int b = 0; b < bitCount; ++b) {
    int bit;
    if (!readNextBitLE(bit)) {
      TINY_DNG_DPRINTF(
          "LE: Failed to read bits from stream! Unexpected end.\n");
      break;
    }

    // Based on a "Stanford bit-hack":
    // http://graphics.stanford.edu/~seander/bithacks.html#ConditionalSetOrClearBitsWithoutBranching
    const uint64_t mask = uint64_t(1) << b;
    num = (num & ~mask) | (uint64_t(-bit) & mask);
  }

  return num;
}

uint64_t BitStreamReader::readBitsU64BE(const int bitCount) {
  TINY_DNG_ASSERT(bitCount <= 64,
                  "`bitCount' must be less than or equal to 64.");

  uint64_t num = 0;
  for (int b = 0; b < bitCount; ++b) {
    int bit;
    if (!readNextBitBE(bit)) {
      TINY_DNG_DPRINTF("BE: Failed to read bits from stream! Unexpected end.");
      break;
    }

    TINY_DNG_DPRINTF("bit[%d](count %d) = %d\n", b, bitCount, bit);

    // Based on a "Stanford bit-hack":
    // http://graphics.stanford.edu/~seander/bithacks.html#ConditionalSetOrClearBitsWithoutBranching
    const uint64_t mask = uint64_t(1) << (bitCount - b - 1);
    num = (num & ~mask) | (uint64_t(-bit) & mask);
  }

  TINY_DNG_DPRINTF("num = %d\n", int(num));
  return num;
}

void BitStreamReader::reset() {
  currBytePos = 0;
  nextBitPos = 0;
  numBitsRead = 0;
}

bool BitStreamReader::isEndOfStream() const {
  return numBitsRead >= sizeInBits;
}

// ========================================================
// easyDecode() and helpers:
// ========================================================

static bool outputByte(int code, unsigned char*& output, int outputSizeBytes,
                       int& bytesDecodedSoFar) {
  if (bytesDecodedSoFar >= outputSizeBytes) {
    // LZW_ERROR("Decoder output buffer too small!");
    return false;
  }

  TINY_DNG_ASSERT(code >= 0 && code < 256, "`code' must be within [0, 255].");
  *output++ = static_cast<unsigned char>(code);
  ++bytesDecodedSoFar;
  return true;
}

static bool outputSequence(const Dictionary& dict, int code,
                           unsigned char*& output, int outputSizeBytes,
                           int& bytesDecodedSoFar, int& firstByte) {
  const int MaxDictEntries = 4096;
  (void)MaxDictEntries;

  // A sequence is stored backwards, so we have to write
  // it to a temp then output the buffer in reverse.
  int i = 0;
  unsigned char sequence[4096];
  do {
    TINY_DNG_ASSERT(i < MaxDictEntries - 1 && code >= 0,
                    "Invalid value for `i' or `code'.");
    TINY_DNG_DPRINTF("i = %d, ent[%d].value = %d\n", i, code,
                     dict.entries_[code].value);
    sequence[i++] = static_cast<unsigned char>(dict.entries_[code].value);
    code = dict.entries_[code].code;
  } while (code >= 0);

  firstByte = sequence[--i];
  for (; i >= 0; --i) {
    if (!outputByte(sequence[i], output, outputSizeBytes, bytesDecodedSoFar)) {
      return false;
    }
  }
  return true;
}

static int easyDecode(const unsigned char* compressed,
                      const int compressedSizeBytes,
                      const int compressedSizeBits, unsigned char* uncompressed,
                      const int uncompressedSizeBytes, const bool swap_endian) {
  const int Nil = -1;
  const int MaxDictBits = 12;
  const int StartBits = 9;

  // TIFF specific values
  const int ClearCode = 256;
  const int EndOfInformation = 257;

  if (compressed == NULL || uncompressed == NULL) {
    TINY_DNG_DPRINTF("lzw::easyDecode(): Null data pointer(s)!\n");
    return 0;
  }

  if (compressedSizeBytes <= 0 || compressedSizeBits <= 0 ||
      uncompressedSizeBytes <= 0) {
    TINY_DNG_DPRINTF("lzw::easyDecode(): Bad in/out sizes!n");
    return 0;
  }

  int code = Nil;
  int prevCode = Nil;
  int firstByte = 0;
  int bytesDecoded = 0;
  int codeBitsWidth = StartBits;

  // We'll reconstruct the dictionary based on the
  // bit stream codes. Unlike Huffman encoding, we
  // don't store the dictionary as a prefix to the data.
  Dictionary dictionary;
  BitStreamReader bitStream(compressed, compressedSizeBytes,
                            compressedSizeBits);

  // We check to avoid an overflow of the user buffer.
  // If the buffer is smaller than the decompressed size,
  // TINY_DNG_DPRINTF() is called. If that doesn't throw or
  // terminate we break the loop and return the current
  // decompression count.
  while (!bitStream.isEndOfStream()) {
    TINY_DNG_ASSERT(
        codeBitsWidth <= MaxDictBits,
        "`codeBitsWidth must be less than or equal to `MaxDictBits'.");
    (void)MaxDictBits;

    if (!swap_endian) {  // TODO(syoyo): Detect BE or LE depending on endianness
                         // in stored format and host endian
      code = static_cast<int>(bitStream.readBitsU64BE(codeBitsWidth));
    } else {
      code = static_cast<int>(bitStream.readBitsU64LE(codeBitsWidth));
    }

    TINY_DNG_DPRINTF("code = %d(swap_endian = %d)\n", code, swap_endian);

    TINY_DNG_ASSERT(code <= dictionary.size(),
                    "`code' must be less than or equal to dictionary size.");

    if (code == EndOfInformation) {
      TINY_DNG_DPRINTF("EoI\n");
      break;
    }

    if (code == ClearCode) {
      // Initialize dict.
      dictionary.init();
      codeBitsWidth = StartBits;

      if (!swap_endian) {
        code = static_cast<int>(bitStream.readBitsU64BE(codeBitsWidth));
      } else {
        code = static_cast<int>(bitStream.readBitsU64LE(codeBitsWidth));
      }

      if (code == EndOfInformation) {
        TINY_DNG_DPRINTF("EoI\n");
        break;
      }

      if (!outputByte(code, uncompressed, uncompressedSizeBytes,
                      bytesDecoded)) {
        break;
      }

      prevCode = code;
      continue;
    }

    if (prevCode == Nil) {
      if (!outputByte(code, uncompressed, uncompressedSizeBytes,
                      bytesDecoded)) {
        break;
      }
      firstByte = code;
      prevCode = code;
      continue;
    }

    if (code >= dictionary.size()) {
      if (!outputSequence(dictionary, prevCode, uncompressed,
                          uncompressedSizeBytes, bytesDecoded, firstByte)) {
        break;
      }
      if (!outputByte(firstByte, uncompressed, uncompressedSizeBytes,
                      bytesDecoded)) {
        break;
      }
    } else {
      if (!outputSequence(dictionary, code, uncompressed, uncompressedSizeBytes,
                          bytesDecoded, firstByte)) {
        break;
      }
    }

    dictionary.add(prevCode, firstByte);
    if (dictionary.flush(codeBitsWidth)) {
      TINY_DNG_DPRINTF("flush\n");
      prevCode = Nil;
    } else {
      prevCode = code;
    }
  }

  return bytesDecoded;
}

// =============================================================================
#ifdef __clang__
#pragma clang diagnostic pop
#endif

}  // namespace lzw

// Check if JPEG data is lossless JPEG or not(baseline JPEG)
static bool IsLosslessJPEG(const uint8_t* header_addr, int data_len, int* width,
                           int* height, int* bits, int* components) {
  TINY_DNG_DPRINTF("islossless jpeg\n");
  int lj_width = 0;
  int lj_height = 0;
  int lj_bits = 0;
  lj92 ljp;
  int ret =
      lj92_open(&ljp, header_addr, data_len, &lj_width, &lj_height, &lj_bits);
  if (ret == LJ92_ERROR_NONE) {
    // TINY_DNG_DPRINTF("w = %d, h = %d, bits = %d, components = %d\n",
    // lj_width,
    // lj_height, lj_bits, ljp->components);

    if ((lj_width == 0) || (lj_height == 0) || (lj_bits == 0) ||
        (lj_bits == 8)) {
      // Looks like baseline JPEG
      lj92_close(ljp);
      return false;
    }

    if (components) (*components) = ljp->components;

    lj92_close(ljp);

    if (width) (*width) = lj_width;
    if (height) (*height) = lj_height;
    if (bits) (*bits) = lj_bits;
  }
  return (ret == LJ92_ERROR_NONE) ? true : false;
}

bool DNGLoader::DecodeNikonRAW(StreamReader* reader, const unsigned char* src,
                               const size_t src_length,
                               const DNGImage& image_info, const int dst_width,
                               unsigned short* dst_data) {
#if defined(TINY_DNG_ENABLE_GPL)

  if (nikon_maker_note_.linearization_table_offset == 0) {
    err_ << "Nikon Makernote information is not parsed correctly." << std::endl;
    return false;
  }

  int tree_idx = 0;
  unsigned int split_value = 0;
  std::vector<unsigned short> curve;
  unsigned short vpred[2][2];

  if ((image_info.bits_per_sample_original == 12) ||
      (image_info.bits_per_sample_original == 14)) {
    // OK
  } else {
    err_ << "Invalid bits per sample value for NEF : "
         << image_info.bits_per_sample_original << std::endl;
    return false;
  }

  if (ParseAndConstructNikonLinearizationTable(
          reader, nikon_maker_note_.linearization_table_offset,
          image_info.bits_per_sample_original, vpred, &split_value, &tree_idx,
          &curve)) {
    err_ << "Failed to parse or construct NEF linearization table."
         << std::endl;
    return false;
  }

  int ret = nikon_decode_raw(tree_idx, int(split_value), int(curve.size()),
                             curve.data(), vpred, src,
                             static_cast<unsigned int>(src_length), dst_width,
                             image_info.height, dst_data);

  if (!ret) {
    err_ << "Failed to decode Nikon RAW." << std::endl;
    return false;
  }

  return true;
#else
  (void)reader;
  (void)src;
  (void)src_length;
  (void)image_info;
  (void)dst_width;
  (void)dst_data;
  err_ << "NEF decoding requres `TINY_DNG_ENABLE_GPL' define." << std::endl;
  return false;
#endif
}

// Decompress LosslesJPEG adta.
// Need to pass whole DNG file content to `src` and `src_length` to support
// decoding LJPEG in tiled format.
// (i.e, src = the beginning of the file, src_len = file size)
bool DNGLoader::DecompressLosslessJPEG(const unsigned char* src,
                                       const size_t src_length,
                                       const DNGImage& image_info,
                                       const int dst_width,
                                       unsigned short* dst_data) {
  unsigned int tiff_h = 0, tiff_w = 0;
// int offset = 0;

#ifdef TINY_DNG_LOADER_PROFILING
  auto start_t = std::chrono::system_clock::now();
#endif

  if ((image_info.tile_width > 0) && (image_info.tile_length > 0)) {
    // Assume Lossless JPEG data is stored in tiled format.

      TINY_DNG_ASSERT(image_info.tile_offsets.size() > 0, "Invalid tile offsets.");
      TINY_DNG_ASSERT(image_info.tile_byte_counts.size() == image_info.tile_offsets.size(), "Mismatch of array size of tile byte counts and tile offsets.");

    // <-       image width(skip len)           ->
    // +-----------------------------------------+
    // |                                         |
    // |              <- tile w  ->              |
    // |              +-----------+              |
    // |              |           | \            |
    // |              |           | |            |
    // |              |           | | tile h     |
    // |              |           | |            |
    // |              |           | /            |
    // |              +-----------+              |
    // |                                         |
    // |                                         |
    // +-----------------------------------------+

    // TINY_DNG_DPRINTF("tile = %d, %d\n", image_info.tile_width,
    // image_info.tile_length);

    // Currently we only support tile data for tile.length == tiff.height.
    // TINY_DNG_ASSERT(image_info.tile_length == image_info.height);

    size_t column_step = 0;
    unsigned int tile_count = 0;
    while (tiff_h < static_cast<unsigned int>(image_info.height)) {
      TINY_DNG_ASSERT(tile_count < image_info.tile_offsets.size(), "Invalid tile counts.");
      // Read offset to JPEG data location.

      unsigned int offset = image_info.tile_offsets[tile_count];

      int lj_width = 0;
      int lj_height = 0;
      int lj_bits = 0;
      lj92 ljp;

      size_t input_len = image_info.tile_byte_counts[tile_count];
      TINY_DNG_DPRINTF("tile[%d] offt = %d, len = %d\n", int(tile_count), int(offset), int(input_len));


      // @fixme { Parse LJPEG header first and set exact compressed LJPEG data
      // length to `data_len` arg. }
      int ret = lj92_open(&ljp, reinterpret_cast<const uint8_t*>(&src[offset]),
                          /* data_len */ static_cast<int>(input_len), &lj_width,
                          &lj_height, &lj_bits);
      TINY_DNG_DPRINTF("ret = %d\n", ret);
      TINY_DNG_ASSERT(ret == LJ92_ERROR_NONE,
                      "Invalid return value for lj92_open().");

      TINY_DNG_DPRINTF("lj %d, %d, %d\n", lj_width, lj_height, lj_bits);
      TINY_DNG_DPRINTF("ljp x %d, y %d, c %d\n", ljp->x, ljp->y,
                       ljp->components);
      TINY_DNG_DPRINTF("tile width = %d\n", image_info.tile_width);
      TINY_DNG_DPRINTF("tile height = %d\n", image_info.tile_length);
      // TINY_DNG_DPRINTF("col = %d, tiff_w = %d / %d\n", column_step, tiff_w,
      // image_info.width);

      TINY_DNG_ASSERT((lj_width * ljp->components * lj_height) ==
                          image_info.tile_width * image_info.tile_length,
                      "Data length mismatch.");

      // int write_length = image_info.tile_width;
      // int skip_length = dst_width - image_info.tile_width;
      // TINY_DNG_DPRINTF("write_len = %d, skip_len = %d\n", write_length,
      // skip_length);

      // size_t dst_offset =
      //    column_step * static_cast<size_t>(image_info.tile_width) +
      //    static_cast<unsigned int>(dst_width) * tiff_h;

      // Decode into temporary buffer.

      std::vector<uint16_t> tmpbuf;
      tmpbuf.resize(
          static_cast<size_t>(lj_width * lj_height * ljp->components));

      ret = lj92_decode(ljp, tmpbuf.data(), image_info.tile_width, 0, NULL, 0);
      TINY_DNG_ASSERT(ret == LJ92_ERROR_NONE,
                      "Invalid return value for lj92_decode().");
      // ret = lj92_decode(ljp, dst_data + dst_offset, write_length,
      // skip_length,
      //                  NULL, 0);

      // Copy to dest buffer.
      // NOTE: For some DNG file, tiled image may exceed the extent of target
      // image resolution.
      for (unsigned int y = 0;
           y < static_cast<unsigned int>(image_info.tile_length); y++) {
        unsigned int y_offset = y + tiff_h;
        if (y_offset >= static_cast<unsigned int>(image_info.height)) {
          continue;
        }

        size_t dst_offset =
            tiff_w + static_cast<unsigned int>(dst_width) * y_offset;

        size_t x_len = static_cast<size_t>(image_info.tile_width);
        if ((tiff_w + static_cast<unsigned int>(image_info.tile_width)) >=
            static_cast<unsigned int>(dst_width)) {
          x_len = static_cast<size_t>(dst_width) - tiff_w;
        }
        for (size_t x = 0; x < x_len; x++) {
          dst_data[dst_offset + x] =
              tmpbuf[y * static_cast<size_t>(image_info.tile_width) + x];
        }
      }

      lj92_close(ljp);

      tiff_w += static_cast<unsigned int>(image_info.tile_width);
      column_step++;
      // TINY_DNG_DPRINTF("col = %d, tiff_w = %d / %d\n", column_step, tiff_w,
      // image_info.width);
      if (tiff_w >= static_cast<unsigned int>(image_info.width)) {
        // tiff_h += static_cast<unsigned int>(image_info.tile_length);
        tiff_h += static_cast<unsigned int>(image_info.tile_length);
        // TINY_DNG_DPRINTF("tiff_h = %d\n", tiff_h);
        tiff_w = 0;
        column_step = 0;
      }
      tile_count++;
    }
  } else {
    // Assume LJPEG data is not stored in tiled format.

    // Read offset to JPEG data location.
    // offset = static_cast<int>(Read4(fp, swap_endian));
    // TINY_DNG_DPRINTF("offt = %d\n", offset);

    TINY_DNG_ASSERT(image_info.offset > 0, "Invalid image offset.");
    size_t offset = static_cast<size_t>(image_info.offset);

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
    // TINY_DNG_DPRINTF("ret = %d\n", ret);
    TINY_DNG_ASSERT(ret == LJ92_ERROR_NONE,
                    "Invalid return code for lj92_open.");

    // TINY_DNG_DPRINTF("lj %d, %d, %d\n", lj_width, lj_height, lj_bits);

    int write_length = image_info.width;
    int skip_length = 0;

    ret = lj92_decode(ljp, dst_data, write_length, skip_length, NULL, 0);
    // TINY_DNG_DPRINTF("ret = %d\n", ret);

    TINY_DNG_ASSERT(ret == LJ92_ERROR_NONE,
                    "Invalid return code for lj92_decode.");

    lj92_close(ljp);
  }

#ifdef TINY_DNG_LOADER_PROFILING
  auto end_t = std::chrono::system_clock::now();
  auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(end_t - start_t);

  std::cout << "DecompressLosslessJPEG : " << ms.count() << " [ms]"
            << std::endl;
#endif

  return true;
}

bool DNGLoader::DecodeImages(std::vector<tinydng::DNGImage>* images) {
  for (size_t i = 0; i < images->size(); i++) {
    tinydng::DNGImage* image = &((*images)[i]);
    // TINY_DNG_DPRINTF("[%lu] compression = %d\n", i, image->compression);

    const size_t data_offset = (image->offset > 0)
                                   ? image->offset
                                   : image->tile_offsets[0];  // FIXME(syoyo)
    TINY_DNG_ASSERT(data_offset > 0, "Unexpected data offset.");

    // std::cout << "offt =\n" << image->offset << std::endl;
    // std::cout << "tile_offt = \n" << image->tile_offset << std::endl;
    // std::cout << "data_offset = " << data_offset << std::endl;

    if (image->compression == COMPRESSION_NONE) {  // no compression

      if (image->jpeg_byte_count > 0) {
        // Looks like CR2 IFD#1(thumbnail jpeg image)
        // Currently skip parsing jpeg data.
        // TODO(syoyo): Decode jpeg data.
        image->width = 0;
        image->height = 0;
      } else {
        image->bits_per_sample = image->bits_per_sample_original;
        TINY_DNG_ASSERT(
            ((image->width * image->height * image->bits_per_sample) % 8) == 0,
            "Image size must be multiple of 8.");
        const size_t len =
            static_cast<size_t>((image->samples_per_pixel * image->width *
                                 image->height * image->bits_per_sample) /
                                8);
        TINY_DNG_ASSERT(len > 0, "Unexpected length.");
        image->data.resize(len);
        memcpy(image->data.data(), reader_.data() + data_offset, len);
      }
    } else if (image->compression == COMPRESSION_LZW) {  // lzw compression
      image->bits_per_sample = image->bits_per_sample_original;
      TINY_DNG_DPRINTF("bps = %d\n", image->bits_per_sample);
      TINY_DNG_DPRINTF("counts = %d\n", int(image->strip_byte_counts.size()));
      TINY_DNG_DPRINTF("offsets = %d\n", int(image->strip_offsets.size()));

      image->data.clear();

      if ((image->strip_byte_counts.size() > 0) &&
          (image->strip_byte_counts.size() == image->strip_offsets.size())) {
        for (size_t k = 0; k < image->strip_byte_counts.size(); k++) {
          std::vector<unsigned char> src(image->strip_byte_counts[k]);

          const size_t dst_len = static_cast<size_t>(
              (image->samples_per_pixel * image->width * image->rows_per_strip *
               image->bits_per_sample) /
              8);
          std::vector<unsigned char> dst(dst_len);

          memcpy(src.data(), reader_.data() + image->strip_offsets[k],
                 image->strip_byte_counts[k]);
          TINY_DNG_DPRINTF("easyDecode begin\n");
          int decoded_bytes = lzw::easyDecode(
              src.data(), int(image->strip_byte_counts[k]),
              int(image->strip_byte_counts[k]) *
                  image->bits_per_sample /* FIXME(syoyo): Is this correct? */,
              dst.data(), int(dst_len), reader_.swap_endian());
          TINY_DNG_DPRINTF("easyDecode done\n");
          TINY_DNG_ASSERT(decoded_bytes > 0,
                          "decoded_ bytes must be non-zero positive.");

          if (image->predictor == 1) {
            // no prediction shceme
          } else if (image->predictor == 2) {
            // horizontal diff

            const size_t stride =
                size_t(image->width * image->samples_per_pixel);
            const size_t spp = size_t(image->samples_per_pixel);
            for (size_t row = 0; row < size_t(image->rows_per_strip); row++) {
              for (size_t c = 0; c < size_t(image->samples_per_pixel); c++) {
                unsigned int b = dst[row * stride + c];
                for (size_t col = 1; col < size_t(image->width); col++) {
                  // value may overflow(wrap over), but its expected behavior.
                  b += dst[stride * row + spp * col + c];
                  dst[stride * row + spp * col + c] =
                      static_cast<unsigned char>(b & 0xFF);
                }
              }
            }

          } else if (image->predictor == 3) {
            // fp horizontal diff.
            TINY_DNG_ABORT("[TODO] FP horizontal differencing predictor.");
          } else {
            TINY_DNG_ABORT("Invalid predictor value.");
          }

          std::copy(dst.begin(), dst.end(), std::back_inserter(image->data));
        }
      } else {
        TINY_DNG_ABORT("Unsupported image strip configuration.");
      }
    } else if (image->compression ==
               COMPRESSION_OLD_JPEG) {  // old jpeg compression

      // std::cout << "IFD " << i << std::endl;

      // First check if JPEG is lossless JPEG
      // TODO(syoyo): Compure conservative data_len.
      TINY_DNG_ASSERT(reader_.size() > data_offset, "Unexpected data offset.");
      size_t data_len = reader_.size() - data_offset;
      int lj_width = -1, lj_height = -1, lj_bits = -1, lj_components = -1;
      if (IsLosslessJPEG(reader_.data() + data_offset,
                         static_cast<int>(data_len), &lj_width, &lj_height,
                         &lj_bits, &lj_components)) {
        // std::cout << "IFD " << i << " is LJPEG" << std::endl;

        TINY_DNG_ASSERT(
            lj_width > 0 && lj_height > 0 && lj_bits > 0 && lj_components > 0,
            "Image dimensions must be > 0.");

        // Assume not in tiled format.
        TINY_DNG_ASSERT(image->tile_width == -1 && image->tile_length == -1,
                        "Tiled format not supported tile size.");

        image->height = lj_height;

        // Is Canon CR2?
        const bool is_cr2 = (image->cr2_slices[0] != 0) ? true : false;

        if (is_cr2) {
          // For CR2 RAW, slices[0] * slices[1] + slices[2] = image width
          image->width = image->cr2_slices[0] * image->cr2_slices[1] +
                         image->cr2_slices[2];
        } else {
          image->width = lj_width;
        }

        image->bits_per_sample_original = lj_bits;

        // lj92 decodes data into 16bits, so modify bps.
        image->bits_per_sample = 16;

        TINY_DNG_ASSERT(
            ((image->width * image->height * image->bits_per_sample) % 8) == 0,
            "Image size must be multiple of 8.");
        const size_t len =
            static_cast<size_t>((image->samples_per_pixel * image->width *
                                 image->height * image->bits_per_sample) /
                                8);
        // std::cout << "spp = " << image->samples_per_pixel;
        // std::cout << ", w = " << image->width << ", h = " << image->height <<
        // ", bps = " << image->bits_per_sample << std::endl;
        TINY_DNG_ASSERT(len > 0, "Invalid length.");
        image->data.resize(len);

        TINY_DNG_ASSERT(reader_.size() > data_offset, "Unexpected data size.");

        std::vector<unsigned short> buf;
        buf.resize(static_cast<size_t>(image->width * image->height *
                                       image->samples_per_pixel));

        bool ok = DecompressLosslessJPEG(reader_.data(), reader_.size(),
                                         (*image), image->width, &buf.at(0));
        if (!ok) {
          err_ << "Failed to decompress LJPEG." << std::endl;
          return false;
        }

        if (is_cr2) {
          // CR2 stores image in tiled format(image slices. left to right).
          // Convert it to scanline format.
          int nslices = image->cr2_slices[0];
          int slice_width = image->cr2_slices[1];
          int slice_remainder_width = image->cr2_slices[2];
          size_t src_offset = 0;

          unsigned short* dst_ptr =
              reinterpret_cast<unsigned short*>(image->data.data());

          for (int slice = 0; slice < nslices; slice++) {
            int x_offset = slice * slice_width;
            for (int y = 0; y < image->height; y++) {
              size_t dst_offset =
                  static_cast<size_t>(y * image->width + x_offset);
              memcpy(&dst_ptr[dst_offset], &buf[src_offset],
                     sizeof(unsigned short) * static_cast<size_t>(slice_width));
              src_offset += static_cast<size_t>(slice_width);
            }
          }

          // remainder(the last slice).
          {
            int x_offset = nslices * slice_width;
            for (int y = 0; y < image->height; y++) {
              size_t dst_offset =
                  static_cast<size_t>(y * image->width + x_offset);
              // std::cout << "y = " << y << ", dst = " << dst_offset << ", src
              // = " << src_offset << ", len = " << buf.size() << std::endl;
              memcpy(&dst_ptr[dst_offset], &buf[src_offset],
                     sizeof(unsigned short) *
                         static_cast<size_t>(slice_remainder_width));
              src_offset += static_cast<size_t>(slice_remainder_width);
            }
          }

        } else {
          memcpy(image->data.data(), static_cast<void*>(&(buf.at(0))), len);
        }

      } else {
        // Baseline 8bit JPEG

        image->bits_per_sample = 8;

        size_t jpeg_len = static_cast<size_t>(image->jpeg_byte_count);
        if (image->jpeg_byte_count == -1) {
          // No jpeg datalen. Set to the size of file - offset.
          TINY_DNG_ASSERT(reader_.size() > data_offset,
                          "Unexpected data size.");
          jpeg_len = reader_.size() - data_offset;
        }
        TINY_DNG_ASSERT(jpeg_len > 0, "Invalid length.");

        // Assume RGB jpeg
        int w = 0, h = 0, components = 0;
        unsigned char* decoded_image = stbi_load_from_memory(
            reader_.data() + data_offset, static_cast<int>(jpeg_len), &w, &h,
            &components, /* desired_channels */ 3);
        TINY_DNG_ASSERT(decoded_image, "Could not decode JPEG image.");

        // Currently we just discard JPEG image(since JPEG image would be just a
        // thumbnail or LDR image of RAW).
        // TODO(syoyo): Do not discard JPEG image.
        free(decoded_image);

        // std::cout << "w = " << w << std::endl;
        // std::cout << "h = " << w << std::endl;
        // std::cout << "c = " << components << std::endl;

        TINY_DNG_ASSERT(w > 0 && h > 0, "Image dimensions must be > 0.");

        image->width = w;
        image->height = h;
      }

    } else if (image->compression ==
               COMPRESSION_NEW_JPEG) {  //  new JPEG(baseline DCT JPEG or
                                        //  lossless JPEG)

      // lj92 decodes data into 16bits, so modify bps.
      image->bits_per_sample = 16;

      // std::cout << "w = " << image->width << ", h = " << image->height <<
      // std::endl;

      TINY_DNG_ASSERT(
          ((image->width * image->height * image->bits_per_sample) % 8) == 0,
          "Image must be multiple of 8.");
      const size_t len =
          static_cast<size_t>((image->samples_per_pixel * image->width *
                               image->height * image->bits_per_sample) /
                              8);
      TINY_DNG_ASSERT(len > 0, "Invalid length.");
      image->data.resize(len);

      TINY_DNG_ASSERT(reader_.size() > data_offset, "Unexpected length.");

      reader_.seek_set(data_offset);  // FIXME(syoyo): Pass data offset to
                                      // `DecompressLosslessJPEG`.

      bool ok = DecompressLosslessJPEG(
          reader_.data(), reader_.size(), (*image), image->width,
          reinterpret_cast<unsigned short*>(image->data.data()));
      if (!ok) {
        err_ << "Failed to decompress LJPEG." << std::endl;
        return false;
      }

    } else if (image->compression == COMPRESSION_ADOBE_DEFLATE) {  // ZIP?
      err_ << "Adobe deflate compression is not supported." << std::endl;
      return false;
    } else if (image->compression == COMPRESSION_LOSSY) {  // lossy JPEG
      err_ << "lossy JPEG compression is not supported." << std::endl;
      return false;
    } else if (image->compression == COMPRESSION_NEF) {  // NEF lossless?

      TINY_DNG_DPRINTF("data_offset = %d\n", int(data_offset));
      const unsigned char* src = reader_.data() + data_offset;

      image->bits_per_sample = 16;
      TINY_DNG_ASSERT(
          ((image->width * image->height * image->bits_per_sample) % 8) == 0,
          "Image must be multiple of 8.");
      const size_t len =
          static_cast<size_t>((image->samples_per_pixel * image->width *
                               image->height * image->bits_per_sample) /
                              8);
      TINY_DNG_ASSERT(len > 0, "Invalid length.");
      image->data.resize(len);

      const size_t src_length = reader_.size() - data_offset;

      bool ok =
          DecodeNikonRAW(&reader_, src, src_length, (*image), image->width,
                         reinterpret_cast<unsigned short*>(image->data.data()));

      if (!ok) {
        err_ << "Failed to decompress NEF image." << std::endl;
        return false;
      }

    } else {
      err_ << "IFD [" << i
           << "] Unsupported compression type : " << image->compression
           << std::endl;
      return false;
    }
  }

  return true;
}

bool DNGLoader::ParseDNG(const std::vector<FieldInfo>& custom_fields,
                         std::vector<tinydng::DNGImage>* images) {
  TINY_DNG_ASSERT(images, "Invalid images pointer.");

  // skip magic header(4 bytes)
  if (!reader_.seek_set(4)) {
    err_ << "Failed to seek in ParseDNG." << std::endl;
    return false;
  }

  unsigned int offt;
  if (!reader_.read4(&offt)) {
    err_ << "Failed to read offset in ParseDNG." << std::endl;
    return false;
  }

  TINY_DNG_DPRINTF("First IFD offt: %d\n", offt);

  // 1. Parse IFDs
  while (offt) {
    if (!reader_.seek_set(offt)) {
      err_ << "Failed to seek in ParseDNG." << std::endl;
      return false;
    }

    // TINY_DNG_DPRINTF("Parse TIFF IFD\n");
    if (!ParseTIFFIFD(custom_fields, images)) {
      return false;
      ;
    }

    // Get next IFD offset(0 = end of file).
    if (!reader_.read4(&offt)) {
      err_ << "Failed to red next IFD offset." << std::endl;
      return false;
    }
    TINY_DNG_DPRINTF("Next IFD offset = %d\n", offt);
  }

  // 2. Post-process IFD values.
  // Compute while level depends on bpp.
  for (size_t i = 0; i < images->size(); i++) {
    tinydng::DNGImage* image = &((*images)[i]);
    TINY_DNG_ASSERT(image->samples_per_pixel <= 4,
                    "Cannot handle > 4 samples per pixel.");
    for (int s = 0; s < image->samples_per_pixel; s++) {
      if (image->white_level[s] == -1) {
        // Set white level with (2 ** BitsPerSample) according to the DNG spec.
        TINY_DNG_ASSERT(image->bits_per_sample_original > 0,
                        "White level has to be > 0.");

        if (image->bits_per_sample_original >=
            32) {  // workaround for 32bit floating point TIFF.
          image->white_level[s] = -1;
        } else {
          TINY_DNG_ASSERT(image->bits_per_sample_original < 32,
                          "Cannot handle >= 32 bits per sample.");
          image->white_level[s] = (1 << image->bits_per_sample_original);
        }
      }
    }
  }

  // 3 Decode image data.
  if (!DecodeImages(images)) {
    err_ << "Failed to decode image data." << std::endl;
    return false;
  }

  return true;
}

bool LoadDNG(const char* filename, std::vector<FieldInfo>& custom_fields,
             std::vector<DNGImage>* images, std::string* err) {
  std::stringstream ss;

  if (!filename) {
    if (err) {
      (*err) = "`filename' argument is null string.\n";
    }
    return false;
  }

  if (!images) {
    if (err) {
      (*err) = "`images' argument is null pointer.\n";
    }
    return false;
  }

  std::vector<unsigned char> whole_data;
  bool is_dng_big_endian = false;
  size_t file_size = 0;
  {
    std::ifstream ifs(filename, std::ifstream::binary);
    if (!ifs) {
      ss << "File not found or cannot open file : " << filename << std::endl;
      if (err) {
        (*err) = ss.str();
      }
      return false;
    }

    unsigned short magic;
    char header[32];

    if (!ifs.read(reinterpret_cast<char*>(&magic), 2)) {
      if (err) {
        (*err) = "Failed to read magic number. Empty file?\n";
      }
      return false;
    }

    if (magic == 0x4949) {
      // might be TIFF(DNG).
    } else if (magic == 0x4d4d) {
      // might be TIFF(DNG, bigendian).
      is_dng_big_endian = true;
      TINY_DNG_DPRINTF("DNG is big endian\n");
    } else {
      ss << "It looks like a file is not a DNG format." << std::endl;
      if (err) {
        (*err) = ss.str();
      }

      return false;
    }

    // rewind
    ifs.seekg(0, ifs.beg);

    if (!ifs.read(header, 32)) {
      if (err) {
        (*err) =
            "Failed to read DNG header. File too short size or corrupted?\n";
      }
      return false;
    }

    ifs.seekg(0, ifs.end);
    file_size = static_cast<size_t>(ifs.tellg());

    if (file_size <= (32 + 8)) {
      if (err) {
        (*err) = "File size too short.\n";
      }
      return false;
    }

    // Buffer to hold whole DNG file data.
    // TODO(syoyo) Implement mmap() based data access to save memory?.
    {
      whole_data.resize(file_size);
      ifs.seekg(0, ifs.beg);
      if (!ifs.read(reinterpret_cast<char*>(whole_data.data()),
                    static_cast<std::streamsize>(file_size))) {
        if (err) {
          (*err) = "Failed to read file.\n";
        }
        return false;
      }
    }
  }

  const bool swap_endian = (is_dng_big_endian && (!IsBigEndian()));

  DNGLoader loader(whole_data, swap_endian);

  bool ret = loader.ParseDNG(custom_fields, images);

  if (err) {
    std::string error_str = loader.error();
    if (!error_str.empty()) {
      (*err) = error_str;
    }
  }

  return ret;
}

}  // namespace tinydng

#endif

#endif  // TINY_DNG_LOADER_H_
