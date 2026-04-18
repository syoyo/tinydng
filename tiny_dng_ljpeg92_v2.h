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
  TDNG_LJ92_ERROR_TOO_WIDE = -4
};

struct _ljp;
typedef struct _ljp* tdng_lj92;

int tdng_lj92_open(tdng_lj92* lj, const uint8_t* data, int datalen,
                   int* width, int* height, int* bitdepth, int* components);
void tdng_lj92_close(tdng_lj92 lj);
int tdng_lj92_decode(tdng_lj92 lj, uint16_t* target, int writeLength,
                     int skipLength, uint16_t* linearize,
                     int linearizeLength);
int tdng_lj92_encode(uint16_t* image, int width, int height, int bitdepth,
                     int readLength, int skipLength, uint16_t* delinearize,
                     int delinearizeLength, uint8_t** encoded,
                     int* encodedLength);

#ifdef __cplusplus
}
#endif

#endif
