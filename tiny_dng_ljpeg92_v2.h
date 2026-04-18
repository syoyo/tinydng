#ifndef TINY_DNG_LJPEG92_V2_H_
#define TINY_DNG_LJPEG92_V2_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum tdng_lj92_error {
  TDNG_LJ92_ERROR_NONE = 0,
  TDNG_LJ92_ERROR_CORRUPT = -1,
  TDNG_LJ92_ERROR_NO_MEMORY = -2,
  TDNG_LJ92_ERROR_BAD_HANDLE = -3,
  TDNG_LJ92_ERROR_TOO_WIDE = -4,
  TDNG_LJ92_ERROR_NOT_LOSSLESS = -5
};

struct _ljp;
typedef struct _ljp* tdng_lj92;

int tdng_lj92_open(tdng_lj92* lj, const uint8_t* data, int datalen,
                   int* width, int* height, int* bitdepth, int* components);
void tdng_lj92_close(tdng_lj92 lj);
int tdng_lj92_decode(tdng_lj92 lj, uint16_t* target, int writeLength,
                     int skipLength, uint16_t* linearize,
                     int linearizeLength);
/* Encode a single-component (mono) 16-bit tile with predictor 1.
 * Thin wrapper around tdng_lj92_encode_ex. */
int tdng_lj92_encode(uint16_t* image, int width, int height, int bitdepth,
                     int readLength, int skipLength, uint16_t* delinearize,
                     int delinearizeLength, uint8_t** encoded,
                     int* encodedLength);

/* Extended encode entry point.
 *   image        : interleaved samples (R0 G0 B0 R1 G1 B1 ...) in 16-bit
 *   width, height: tile dimensions
 *   bitdepth     : 2..16
 *   components   : 1..4 (each gets its own Huffman table)
 *   predictor    : 1 (left), 2 (above) or 7 (average); 1 is the safest default
 *   readLength   : samples (components counted separately) read per row before
 *                  skipping skipLength samples. Typical: width*components.
 *   skipLength   : samples skipped after each row (for tiled images).
 *   delinearize  : optional sample mapping table applied before encoding.
 * On success, *encoded receives a malloc'd buffer the caller must free. */
int tdng_lj92_encode_ex(uint16_t* image, int width, int height, int bitdepth,
                        int components, int predictor, int readLength,
                        int skipLength, uint16_t* delinearize,
                        int delinearizeLength, uint8_t** encoded,
                        int* encodedLength);

#ifdef __cplusplus
}
#endif

#endif
