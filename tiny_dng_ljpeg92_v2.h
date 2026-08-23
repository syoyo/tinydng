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
  TDNG_LJ92_ERROR_IO = -6, /* sink short write / sink failure */
  TDNG_LJ92_ERROR_INVALID_ARGUMENT = -7,
  TDNG_LJ92_ERROR_STATE = -8,
  TDNG_LJ92_ERROR_LIMIT = -9,
  TDNG_LJ92_ERROR_UNSUPPORTED = -10
};

struct _ljp;
typedef struct _ljp* tdng_lj92;
struct _lje;
typedef struct _lje* tdng_lj92_enc;

typedef void* (*tdng_lj92_alloc_fn)(void* user, size_t size);
typedef void (*tdng_lj92_free_fn)(void* user, void* ptr);
typedef struct tdng_lj92_allocator {
  tdng_lj92_alloc_fn alloc;
  tdng_lj92_free_fn free;
  void* user;
} tdng_lj92_allocator;

#define TDNG_LJ92_MAX_COMPONENTS 16
typedef struct tdng_lj92_component_info {
  uint8_t id;
  uint8_t h_sampling;
  uint8_t v_sampling;
  uint32_t width;
  uint32_t height;
} tdng_lj92_component_info;

typedef struct tdng_lj92_frame_info {
  uint32_t width;
  uint32_t height;
  uint8_t precision;
  uint8_t component_count;
  uint8_t sof_marker; /* 0xC3: Huffman lossless; 0xCB: arithmetic lossless */
  tdng_lj92_component_info components[TDNG_LJ92_MAX_COMPONENTS];
} tdng_lj92_frame_info;

typedef struct tdng_lj92_plane {
  uint16_t* data;
  size_t capacity_samples;
  size_t row_stride_samples;
  size_t pixel_stride_samples;
  uint32_t width;
  uint32_t height;
} tdng_lj92_plane;

typedef struct tdng_lj92_const_plane {
  const uint16_t* data;
  size_t capacity_samples;
  size_t row_stride_samples;
  size_t pixel_stride_samples;
  uint32_t width;
  uint32_t height;
} tdng_lj92_const_plane;

typedef struct tdng_lj92_scan_plan {
  uint8_t component_count;
  uint8_t component_index[4];
  uint8_t predictor;
  uint8_t point_transform;
  uint32_t restart_interval_mcus;
} tdng_lj92_scan_plan;

int tdng_lj92_open(tdng_lj92* lj, const uint8_t* data, int datalen,
                   int* width, int* height, int* bitdepth, int* components);
int tdng_lj92_open_ex(tdng_lj92* lj, const uint8_t* data, int datalen,
                      const tdng_lj92_allocator* allocator, int* width,
                      int* height, int* bitdepth, int* components);
void tdng_lj92_close(tdng_lj92 lj);
int tdng_lj92_get_frame_info(tdng_lj92 lj, tdng_lj92_frame_info* info);
/* Decode each component at its native sampling resolution. The plane geometry
 * must exactly match get_frame_info(), and strides/capacities are measured in
 * uint16_t samples. Components are not implicitly upsampled. This advanced
 * multi-scan/SOF11 entry point currently requires a memory-backed handle;
 * streaming handles return TDNG_LJ92_ERROR_UNSUPPORTED. */
int tdng_lj92_decode_planes(tdng_lj92 lj, tdng_lj92_plane* planes,
                            size_t plane_count);
/* Decode into `target` (row stride `writeLength` samples; a positive value
 * is the caller's declared per-row capacity and decode fails if
 * writeLength*H < W*Nf*H; 0 skips the capacity check -- legacy behavior,
 * prefer passing the real stride).
 * `linearize`, if non-NULL, is a mapping table with exactly `linearizeLength`
 * entries: any decoded sample >= linearizeLength fails the decode with
 * TDNG_LJ92_ERROR_CORRUPT (samples are indices into the table). */
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
 * pure stream of unknown length (UINT64_MAX). A SOF3 handle decodes with
 * regular tdng_lj92_decode and is released with tdng_lj92_close. SOF11
 * headers can be inspected through get_frame_info(), but native-plane SOF11
 * decode requires reopening a memory-backed handle.
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
int tdng_lj92_open_streaming_ex(tdng_lj92* lj, void* user,
                                tdng_lj92_read_fn read_fn,
                                tdng_lj92_size_fn size_fn,
                                const tdng_lj92_allocator* allocator,
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
 * rows() must be called strictly in row order, and all image rows must be
 * supplied before finish(). Output is staged in 64KB chunks and never
 * materialized as a whole. */
typedef size_t (*tdng_lj92_write_fn)(void* user, const void* data, size_t len);
/* One-shot full lossless-JPEG encoder operating on native component planes.
 * frame->sof_marker selects SOF3 Huffman (0 or 0xC3) or SOF11 arithmetic
 * (0xCB). Every component must occur in exactly one scan. Interleaved scans
 * contain 2..4 components; one-component scans are noninterleaved. Predictor
 * selectors 1..7 and point transforms are supported. A nonzero restart
 * interval must span an integer number of complete MCU rows. SOF11 emits the
 * Annex H default DC conditioning values L=0, U=1. */
int tdng_lj92_encode_frame(const tdng_lj92_frame_info* frame,
                           const tdng_lj92_const_plane* planes,
                           size_t plane_count, const tdng_lj92_scan_plan* scans,
                           size_t scan_count, void* user,
                           tdng_lj92_write_fn write_fn,
                           const tdng_lj92_allocator* allocator);
int tdng_lj92_encode_open(tdng_lj92_enc* lj, int width, int height,
                          int bitdepth, int components, int predictor,
                          int readLength, int skipLength, void* user,
                          tdng_lj92_write_fn write_fn);
int tdng_lj92_encode_open_ex(tdng_lj92_enc* lj, int width, int height,
                             int bitdepth, int components, int predictor,
                             int readLength, int skipLength, void* user,
                             tdng_lj92_write_fn write_fn,
                             const tdng_lj92_allocator* allocator);
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
 *   components   : 1..4, sharing one optimized Huffman table
 *   predictor    : Annex H selector 1..7; 1 is the safest default
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
