/*
 * tinydng internal header - shared by all translation units.
 * Not installed. SPDX-License-Identifier: MIT
 */
#ifndef TINYDNG_INTERNAL_H_
#define TINYDNG_INTERNAL_H_

#include "tinydng.h"

#include <stdarg.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Tracked allocator                                                  */
/* ------------------------------------------------------------------ */

#define TINYDNG_ALLOC_MAGIC 0x54444E47u /* 'TDNG' */

typedef struct td_alloc_header {
  uint32_t magic;
  size_t size;
  struct td_alloc_header *prev;
  struct td_alloc_header *next;
} td_alloc_header;

/* ------------------------------------------------------------------ */
/* Threading (opaque; all platform code lives in tinydng_api.c).      */
/* Enabled on POSIX / Win32 unless TINYDNG_DISABLE_THREADS is set.    */
/* ------------------------------------------------------------------ */

#if !defined(TINYDNG_DISABLE_THREADS) && \
    (defined(_WIN32) || defined(__unix__) || defined(__APPLE__) || \
     defined(__linux__))
#define TINYDNG_ENABLE_THREADS 1
#endif

typedef struct td_mutex td_mutex; /* opaque */

#define TD_MAX_DECODE_THREADS 64

struct tinydng_context {
  tinydng_allocator allocator;
  size_t memory_cap_bytes;
  size_t memory_used;
  size_t memory_peak;
  uint32_t max_images;
  uint64_t max_image_pixels;
  uint32_t max_ifd_depth;
  uint32_t max_ifd_entries;
  uint32_t max_psd_layers;
  uint32_t max_psd_resources;
  uint32_t max_embed_depth;
  td_alloc_header *alloc_head;
  int alloc_failed;
  td_mutex *lock;  /* guards allocator + stdio reads while mt_active (may be NULL) */
  int mt_active;   /* set only for the duration of a multi-threaded decode */
};

#define TD_DOC_FORMAT_TIFF 0u
#define TD_DOC_FORMAT_PSD 1u

struct tinydng_document {
  tinydng_io io;  /* owned; closed on destroy */
  uint8_t has_io; /* io.close should be called */
  uint64_t io_size;
  uint8_t big_endian;
  uint8_t bigtiff;
  uint8_t format;       /* TD_DOC_FORMAT_* */
  uint32_t embed_depth; /* smart-object nesting level (0 = top file) */
  tinydng_image_info *images;
  size_t image_count;
  tinydng_exif global_exif;
  uint8_t has_global_exif;
  struct tinydng_psd_info *psd; /* non-NULL for PSD/PSB documents */
};

void *td_ctx_alloc(tinydng_context *ctx, size_t size, tinydng_error *err);
void *td_ctx_calloc(tinydng_context *ctx, size_t size, tinydng_error *err);
void td_ctx_free(tinydng_context *ctx, void *ptr);
void td_ctx_free_all(tinydng_context *ctx);

/* Reallocate-like growth that preserves contents. Frees old on success. */
void *td_ctx_realloc(tinydng_context *ctx, void *ptr, size_t old_size,
                     size_t new_size, tinydng_error *err);

/* Free all heap payload owned by an image_info (exif/raw/opcodes/gainmaps/
   segments/custom_fields) and zero it. Used by document destroy and to discard
   a partially-parsed scratch image on an error path. */
void td_free_image_payload(tinydng_context *ctx, tinydng_image_info *img);

/* ------------------------------------------------------------------ */
/* Threading helpers (no-ops / serial when threads are disabled)      */
/* ------------------------------------------------------------------ */

/* Create/destroy a mutex (heap-allocated via the ctx allocator, untracked).
   Returns NULL on failure or when threads are disabled; lock/unlock are
   NULL-safe no-ops, so callers never branch on the build configuration. */
td_mutex *td_mutex_create(tinydng_context *ctx);
void td_mutex_destroy(tinydng_context *ctx, td_mutex *m);
void td_mutex_lock(td_mutex *m);
void td_mutex_unlock(td_mutex *m);

/* Run `n` tasks in parallel: task i calls fn((char *)args + i * arg_stride).
   Every task is guaranteed to run and complete before return -- up to `n`
   OS threads are used, with inline fallback for any that fail to spawn (and a
   fully serial loop when threads are disabled). Returns 0. */
typedef void *(*td_thread_fn)(void *);
int td_threads_run(td_thread_fn fn, void *args, size_t arg_stride, unsigned n);

/* 1 if real OS threads are available in this build, else 0. */
int td_threads_available(void);

/* Online CPU count (>= 1); 1 when undeterminable or threads disabled. */
unsigned td_cpu_count(void);

/* ------------------------------------------------------------------ */
/* Safe arithmetic                                                    */
/* ------------------------------------------------------------------ */

int td_safe_add_size(size_t a, size_t b, size_t *out);
int td_safe_mul_size(size_t a, size_t b, size_t *out);
int td_safe_add_u64(uint64_t a, uint64_t b, uint64_t *out);
int td_safe_mul_u64(uint64_t a, uint64_t b, uint64_t *out);

/* ------------------------------------------------------------------ */
/* Error helper                                                       */
/* ------------------------------------------------------------------ */

void td_set_error(tinydng_error *err, tinydng_status code, tinydng_stage stage,
                  uint32_t ifd_index, uint16_t tag, uint64_t offset,
                  const char *fmt, ...);

/* ------------------------------------------------------------------ */
/* IO view helper                                                     */
/* ------------------------------------------------------------------ */

/* Return a readable pointer to [off, off+len). Zero-copy if io->map
   succeeds; else reads into `scratch` (must be >= len). Returns NULL on
   any overflow / out-of-range / short read. Never triggers UB. */
const uint8_t *td_io_view(tinydng_io *io, uint64_t io_size, uint64_t off,
                          size_t len, uint8_t *scratch, size_t scratch_cap);

/* ------------------------------------------------------------------ */
/* Reader layer                                                       */
/* ------------------------------------------------------------------ */

typedef struct td_reader {
  tinydng_io *io;
  uint64_t size;     /* cached io->size() */
  uint8_t big_endian;
  uint8_t bigtiff;   /* version 43 => 8-byte offsets/counts */
} td_reader;

/* All bounds-checked. Return 1 on success, 0 on out-of-range / short read. */
int td_r_u16(const td_reader *r, uint64_t at, uint16_t *out);
int td_r_u32(const td_reader *r, uint64_t at, uint32_t *out);
int td_r_u64(const td_reader *r, uint64_t at, uint64_t *out);
int td_r_i32(const td_reader *r, uint64_t at, int32_t *out);

/* Size in bytes of a TIFF data type (0 => unknown). */
size_t td_tiff_type_size(uint16_t type);

/* Read element at byte `at` as the given TIFF type, widening to the
   destination. RATIONAL/SRATIONAL/FLOAT/DOUBLE for the _real variant. */
int td_r_val_uint(const td_reader *r, uint16_t type, uint64_t at,
                  uint64_t *out);
int td_r_val_int(const td_reader *r, uint16_t type, uint64_t at, int64_t *out);
int td_r_val_real(const td_reader *r, uint16_t type, uint64_t at, double *out);

/* ------------------------------------------------------------------ */
/* TIFF data types                                                    */
/* ------------------------------------------------------------------ */

#define TD_TYPE_BYTE 1u
#define TD_TYPE_ASCII 2u
#define TD_TYPE_SHORT 3u
#define TD_TYPE_LONG 4u
#define TD_TYPE_RATIONAL 5u
#define TD_TYPE_SBYTE 6u
#define TD_TYPE_UNDEFINED 7u
#define TD_TYPE_SSHORT 8u
#define TD_TYPE_SLONG 9u
#define TD_TYPE_SRATIONAL 10u
#define TD_TYPE_FLOAT 11u
#define TD_TYPE_DOUBLE 12u
#define TD_TYPE_IFD 13u
#define TD_TYPE_LONG8 16u
#define TD_TYPE_SLONG8 17u
#define TD_TYPE_IFD8 18u

/* ------------------------------------------------------------------ */
/* TIFF / DNG tags                                                    */
/* ------------------------------------------------------------------ */

#define TD_TAG_NEW_SUBFILE_TYPE 254u
#define TD_TAG_IMAGE_WIDTH 256u
#define TD_TAG_IMAGE_LENGTH 257u
#define TD_TAG_BITS_PER_SAMPLE 258u
#define TD_TAG_COMPRESSION 259u
#define TD_TAG_PHOTOMETRIC 262u
#define TD_TAG_IMAGEDESCRIPTION 270u
#define TD_TAG_MAKE 271u
#define TD_TAG_MODEL 272u
#define TD_TAG_STRIP_OFFSETS 273u
#define TD_TAG_ORIENTATION 274u
#define TD_TAG_SAMPLES_PER_PIXEL 277u
#define TD_TAG_ROWS_PER_STRIP 278u
#define TD_TAG_STRIP_BYTE_COUNTS 279u
#define TD_TAG_PLANAR_CONFIGURATION 284u
#define TD_TAG_SOFTWARE 305u
#define TD_TAG_DATETIME 306u
#define TD_TAG_PREDICTOR 317u
#define TD_TAG_TILE_WIDTH 322u
#define TD_TAG_TILE_LENGTH 323u
#define TD_TAG_TILE_OFFSETS 324u
#define TD_TAG_TILE_BYTE_COUNTS 325u
#define TD_TAG_SUB_IFDS 330u
#define TD_TAG_JPEG_IF_OFFSET 513u
#define TD_TAG_JPEG_IF_BYTE_COUNT 514u
#define TD_TAG_SAMPLE_FORMAT 339u
#define TD_TAG_EXIF_IFD 34665u
#define TD_TAG_EXPOSURE_TIME 33434u
#define TD_TAG_ISO_SPEED_RATINGS 34855u
#define TD_TAG_SHUTTER_SPEED_VALUE 37377u
#define TD_TAG_APERTURE_VALUE 37378u

#define TD_TAG_CFA_REPEAT_PATTERN_DIM 33421u
#define TD_TAG_CFA_PATTERN 33422u
#define TD_TAG_DNG_VERSION 50706u
#define TD_TAG_DNG_BACKWARD_VERSION 50707u
#define TD_TAG_CFA_PLANE_COLOR 50710u
#define TD_TAG_CFA_LAYOUT 50711u
#define TD_TAG_LINEARIZATION_TABLE 50712u
#define TD_TAG_DEFAULT_BLACK_RENDER 50713u
#define TD_TAG_BLACK_LEVEL 50714u
#define TD_TAG_WHITE_LEVEL 50717u
#define TD_TAG_COLOR_MATRIX1 50721u
#define TD_TAG_COLOR_MATRIX2 50722u
#define TD_TAG_CAMERA_CALIBRATION1 50723u
#define TD_TAG_CAMERA_CALIBRATION2 50724u
#define TD_TAG_ANALOG_BALANCE 50727u
#define TD_TAG_AS_SHOT_NEUTRAL 50728u
#define TD_TAG_CR2_SLICES 50752u
#define TD_TAG_CALIBRATION_ILLUMINANT1 50778u
#define TD_TAG_CALIBRATION_ILLUMINANT2 50779u
#define TD_TAG_ACTIVE_AREA 50829u
#define TD_TAG_PROFILE_NAME 50936u
#define TD_TAG_PROFILE_TONE_CURVE 50940u
#define TD_TAG_FORWARD_MATRIX1 50964u
#define TD_TAG_FORWARD_MATRIX2 50965u
#define TD_TAG_OPCODE_LIST1 51008u
#define TD_TAG_OPCODE_LIST2 51009u
#define TD_TAG_OPCODE_LIST3 51022u
#define TD_TAG_NOISE_PROFILE 51041u
#define TD_TAG_SEMANTIC_NAME 52526u

/* ------------------------------------------------------------------ */
/* IFD build scratch (populated while parsing one IFD)                */
/* ------------------------------------------------------------------ */

typedef struct td_ifd_build {
  uint32_t width, height;
  uint16_t bits_per_sample;
  uint16_t samples_per_pixel;
  uint16_t compression;
  uint16_t photometric;
  uint16_t sample_format;
  uint16_t planar_configuration;
  uint16_t predictor;
  uint32_t rows_per_strip;
  uint32_t tile_width, tile_length;
  uint32_t jpeg_if_offset;
  uint32_t jpeg_if_byte_count;
  uint32_t new_subfile_type;

  uint64_t *strip_offsets;
  uint64_t *strip_byte_counts;
  size_t strip_offset_count;
  size_t strip_byte_count_count;
  uint64_t *tile_offsets;
  uint64_t *tile_byte_counts;
  size_t tile_offset_count;
  size_t tile_byte_count_count;
  uint64_t *sub_ifds;
  size_t sub_ifd_count;

  uint8_t has_width, has_height, has_bps, has_spp, has_compression;
  uint8_t has_rows_per_strip, has_tile_width, has_tile_length;
} td_ifd_build;

/* Defined in tinydng_dng.c: handle a non-geometry (metadata) tag. Returns
   1 if handled (incl. benign skip), 0 on a fatal parse/bounds error. */
int td_dng_handle_tag(tinydng_context *ctx, const td_reader *r,
                      tinydng_image_info *img, uint32_t ifd_index, uint16_t tag,
                      uint16_t type, uint64_t count, uint64_t data_off,
                      tinydng_error *err);

/* ------------------------------------------------------------------ */
/* Shared codecs                                                      */
/* ------------------------------------------------------------------ */

#ifndef TINYDNG_NO_PACKBITS
/* Defined in tinydng_codec.c. Returns decoded byte count or -1. */
long td_packbits_decode(const uint8_t *in, size_t in_len, uint8_t *out,
                        size_t out_cap);
#endif

/* ------------------------------------------------------------------ */
/* PSD / PSB (tinydng_psd.c, tinydng_psd_write.c)                     */
/* ------------------------------------------------------------------ */

#ifndef TINYDNG_NO_PSD

/* Internal-only compression code for the composite segment table: zlib +
   per-row delta prediction. Never collides with TIFF compression tags. */
#define TD_COMPRESSION_PSD_ZIP_PRED 0xF003u

/* Parse a PSD/PSB stream (magic already verified) and populate `doc`:
   doc->psd plus doc->images[0] for the composite. `r` is positioned on the
   whole file with big_endian=1. */
tinydng_status td_psd_open(tinydng_context *ctx, td_reader *r,
                           tinydng_document *doc, uint32_t open_flags,
                           tinydng_error *err);

/* Free all heap payload owned by a psd_info (incl. the struct itself). */
void td_psd_free_info(tinydng_context *ctx, struct tinydng_psd_info *psd);

/* Undo PSD zip-prediction in place on stored big-endian bytes of one plane
   (w*h samples of `depth` bits, rows independent). Returns 1 ok / 0 error. */
int td_psd_unpredict_plane(tinydng_context *ctx, uint8_t *plane, uint32_t w,
                           uint32_t h, uint16_t depth, tinydng_error *err);

#endif /* TINYDNG_NO_PSD */

#endif /* TINYDNG_INTERNAL_H_ */
