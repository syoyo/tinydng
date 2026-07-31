#ifndef TINY_DNG_LJPEG92_V2_H_
#define TINY_DNG_LJPEG92_V2_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum tdng_lj92_error {
  TDNG_LJ92_ERROR_NONE = 0,
  TDNG_LJ92_ERROR_CORRUPT = -1,
  TDNG_LJ92_ERROR_NO_MEMORY = -2,
  TDNG_LJ92_ERROR_BAD_HANDLE = -3,
  TDNG_LJ92_ERROR_TOO_WIDE = -4,
  TDNG_LJ92_ERROR_NOT_LOSSLESS = -5,
  TDNG_LJ92_ERROR_IO = -6 /* sink short write / sink failure */
};

struct _ljp;
typedef struct _ljp* tdng_lj92;
struct _lje;
typedef struct _lje* tdng_lj92_enc;

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

/* Streaming decode via an IO callback.
 *
 * Same as tdng_lj92_open, but the stream is read through `read_fn` and is
 * never materialized in memory: headers are parsed and the entropy payload
 * is destuffed straight out of the callback. `size_fn` may be NULL for a
 * pure stream of unknown length (UINT64_MAX). The returned handle decodes
 * with the regular tdng_lj92_decode and is released with tdng_lj92_close.
 *
 * On streams that are not lossless JPEG (SOF0/1/2) this returns
 * TDNG_LJ92_ERROR_NOT_LOSSLESS; on garbage/truncated streams,
 * TDNG_LJ92_ERROR_CORRUPT. The callback must return the exact byte count
 * requested, or fewer bytes at the end of the stream (short read = EOF). */
typedef size_t (*tdng_lj92_read_fn)(void* user, uint64_t off, void* dst, size_t len);
typedef uint64_t (*tdng_lj92_size_fn)(void* user);
int tdng_lj92_open_streaming(tdng_lj92* lj, void* user,
                             tdng_lj92_read_fn read_fn,
                             tdng_lj92_size_fn size_fn,
                             int* width, int* height, int* bitdepth,
                             int* components);
/* Streaming encode: emit the encoded stream to a sink callback instead of
 * a malloc'd buffer.
 *
 *   open(lj, w, h, bitdepth, comps, pred, readLength, skipLength, user, sink)
 *   scan(lj, image, delinearize, delinearizeLen)   pass 1: SSSS histogram
 *   begin(lj)                                      emit SOI/SOF3/DHT/SOS
 *   rows(lj, image, row0, row_count)               pass 2, incremental
 *   finish(lj)                                     flush + EOI; frees lj
 *
 * The sink callback must return the number of bytes accepted; a short
 * return is a sink failure (TDNG_LJ92_ERROR_IO). `image` is the base
 * pointer for the source frame (readLength/skipLength follow the DNG tiled
 * layout) and must be the same pointer in scan() and every rows() call.
 * rows() must be called strictly in row order. Output is staged in 4KB
 * chunks and never materialized as a whole. */
typedef size_t (*tdng_lj92_write_fn)(void* user, const void* data, size_t len);
int tdng_lj92_encode_open(tdng_lj92_enc* lj, int width, int height,
                          int bitdepth, int components, int predictor,
                          int readLength, int skipLength, void* user,
                          tdng_lj92_write_fn write_fn);
int tdng_lj92_encode_scan(tdng_lj92_enc lj, const uint16_t* image,
                          const uint16_t* delinearize, int delinearizeLength);
int tdng_lj92_encode_begin(tdng_lj92_enc lj);
int tdng_lj92_encode_rows(tdng_lj92_enc lj, const uint16_t* image, int row0,
                          int row_count);
int tdng_lj92_encode_finish(tdng_lj92_enc lj);

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
