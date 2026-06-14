/*
 * tinydng - pure C11 DNG/TIFF loader (clean-room v3).
 *
 * Parser-first, security-hardened. Single public header.
 *
 * SPDX-License-Identifier: MIT
 * Copyright 2016-Present Syoyo Fujita and contributors.
 */
#ifndef TINYDNG_H_
#define TINYDNG_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Status / structured error                                          */
/* ------------------------------------------------------------------ */

typedef enum tinydng_status {
  TINYDNG_OK = 0,
  TINYDNG_E_INVALID_ARG = 1,
  TINYDNG_E_PARSE = 2,
  TINYDNG_E_UNSUPPORTED = 3,
  TINYDNG_E_IO = 4,
  TINYDNG_E_OOM = 5,
  TINYDNG_E_BOUNDS = 6,
  TINYDNG_E_DECODE = 7,
  TINYDNG_E_INTERNAL = 8
} tinydng_status;

typedef enum tinydng_stage {
  TINYDNG_STAGE_NONE = 0,
  TINYDNG_STAGE_ALLOC = 1,
  TINYDNG_STAGE_IO = 2,
  TINYDNG_STAGE_HEADER = 3,
  TINYDNG_STAGE_IFD = 4,
  TINYDNG_STAGE_GEOMETRY = 5,
  TINYDNG_STAGE_METADATA = 6,
  TINYDNG_STAGE_DECODE = 7,
  TINYDNG_STAGE_WRITE = 8
} tinydng_stage;

typedef struct tinydng_error {
  tinydng_status status;
  tinydng_stage stage;
  uint32_t ifd_index; /* which IFD (0 if n/a)        */
  uint16_t tag;       /* which tag (0 if n/a)        */
  uint64_t offset;    /* file offset of fault        */
  char message[256];
} tinydng_error;

/* ------------------------------------------------------------------ */
/* Allocator + config                                                 */
/* ------------------------------------------------------------------ */

typedef struct tinydng_allocator {
  void *(*alloc)(void *user_data, size_t n);
  void (*free)(void *user_data, void *p);
  void *user_data;
} tinydng_allocator;

#define TINYDNG_DEFAULT_MEMORY_CAP_BYTES (512u * 1024u * 1024u)

typedef struct tinydng_config {
  tinydng_allocator allocator; /* zeroed => libc malloc/free          */
  size_t memory_cap_bytes;     /* 0 => TINYDNG_DEFAULT_MEMORY_CAP      */
  uint32_t max_images;         /* 0 => 1024                           */
  uint64_t max_image_pixels;   /* per-image W*H cap; 0 => 1<<30        */
  uint32_t max_ifd_depth;      /* SubIFD recursion guard; 0 => 8       */
  uint32_t max_ifd_entries;    /* per-IFD entry cap; 0 => 4096         */
} tinydng_config;

/* ------------------------------------------------------------------ */
/* IO abstraction                                                     */
/* ------------------------------------------------------------------ */

typedef struct tinydng_io tinydng_io;
struct tinydng_io {
  /* Copy `len` bytes at absolute `off` into `dst`. Returns bytes copied;
     a return < len signals a short read / out-of-range (never a crash). */
  size_t (*read)(tinydng_io *io, uint64_t off, void *dst, size_t len);
  /* Total source size, or UINT64_MAX if unknown (pure stream). */
  uint64_t (*size)(tinydng_io *io);
  /* Zero-copy: pointer to a contiguous [off,off+len) window if the backend
     is fully resident (memory/mmap); NULL otherwise. Valid until close. */
  const uint8_t *(*map)(tinydng_io *io, uint64_t off, size_t len);
  /* Release backend resources. May be NULL. */
  void (*close)(tinydng_io *io);
  void *backend; /* backend-private state */
};

/* ------------------------------------------------------------------ */
/* Context / document                                                 */
/* ------------------------------------------------------------------ */

typedef struct tinydng_context tinydng_context;
typedef struct tinydng_document tinydng_document;

tinydng_context *tinydng_context_create(const tinydng_config *config,
                                        tinydng_error *err);
void tinydng_context_destroy(tinydng_context *ctx);
size_t tinydng_context_memory_used(const tinydng_context *ctx);
size_t tinydng_context_memory_peak(const tinydng_context *ctx);

void tinydng_error_clear(tinydng_error *err);
const char *tinydng_status_string(tinydng_status status);
const char *tinydng_stage_string(tinydng_stage stage);

/* IO backend constructors. Each fills `*out` with a ready vtable. The
   memory/mmap backends allocate private state through `ctx` (cap-tracked). */
tinydng_status tinydng_io_open_memory(tinydng_context *ctx, const uint8_t *data,
                                      size_t n, tinydng_io *out,
                                      tinydng_error *err);
tinydng_status tinydng_io_open_mmap(tinydng_context *ctx, const char *path,
                                    tinydng_io *out, tinydng_error *err);
tinydng_status tinydng_io_open_stdio(tinydng_context *ctx, const char *path,
                                     tinydng_io *out, tinydng_error *err);

/* ------------------------------------------------------------------ */
/* Open options                                                       */
/* ------------------------------------------------------------------ */

typedef enum tinydng_open_flags {
  TINYDNG_OPEN_NONE = 0,
  TINYDNG_OPEN_PARSE_SUBIFDS = 1u << 0, /* descend SubIFDs              */
  TINYDNG_OPEN_PREFER_MMAP = 1u << 1,   /* open_file: mmap over stdio    */
  TINYDNG_OPEN_METADATA_ONLY = 1u << 2  /* never auto-decode             */
} tinydng_open_flags;

typedef struct tinydng_open_options {
  uint32_t flags;
} tinydng_open_options;

/* ------------------------------------------------------------------ */
/* Metadata structs                                                   */
/* ------------------------------------------------------------------ */

typedef enum tinydng_compression {
  TINYDNG_COMPRESSION_NONE = 1,
  TINYDNG_COMPRESSION_LZW = 5,
  TINYDNG_COMPRESSION_OLD_JPEG = 6,
  TINYDNG_COMPRESSION_NEW_JPEG = 7,
  TINYDNG_COMPRESSION_ZIP = 8,
  TINYDNG_COMPRESSION_PACKBITS = 32773,
  TINYDNG_COMPRESSION_LOSSY_JPEG = 34892
} tinydng_compression;

typedef enum tinydng_sample_format {
  TINYDNG_SAMPLEFORMAT_UINT = 1,
  TINYDNG_SAMPLEFORMAT_INT = 2,
  TINYDNG_SAMPLEFORMAT_IEEEFP = 3
} tinydng_sample_format;

typedef struct tinydng_exif {
  char *make;
  char *model;
  char *software;
  char *datetime;
  char *image_description;
  uint16_t orientation;
  int32_t shutter_speed[2]; /* num, den */
  int32_t aperture_value[2];
  int32_t exposure_time[2]; /* num, den */
  uint32_t iso;
  uint8_t has_shutter_speed;
  uint8_t has_aperture_value;
  uint8_t has_exposure_time;
  uint8_t has_iso;
} tinydng_exif;

typedef struct tinydng_cfa {
  uint16_t pattern_dim[2];
  uint8_t pattern[16];
  uint8_t pattern_size;
  uint8_t plane_color[4];
  uint8_t plane_color_count;
  uint16_t layout;
  uint8_t present;
} tinydng_cfa;

typedef struct tinydng_gainmap {
  uint32_t opcode_list; /* 1/2/3 */
  uint32_t top, left, bottom, right;
  uint32_t plane, planes, row_pitch, col_pitch;
  uint32_t map_points_v, map_points_h, map_planes;
  double map_spacing_v, map_spacing_h, map_origin_v, map_origin_h;
  float *pixels; /* context-owned */
  size_t pixel_count;
} tinydng_gainmap;

/* Generic DNG opcode (every opcode is captured here, with raw big-endian
   parameter bytes, so unknown opcodes are still accessible). */
typedef struct tinydng_opcode {
  uint32_t list; /* 1/2/3 */
  uint32_t id;
  uint32_t version;
  uint32_t flags;
  const uint8_t *params; /* context-owned copy, big-endian */
  size_t params_size;
} tinydng_opcode;

/* WarpRectilinear (opcode 1): per-plane radial+tangential lens distortion. */
typedef struct tinydng_warp_rectilinear {
  uint32_t list;
  uint32_t plane_count;  /* 1..4 */
  double coeff[4][6];    /* [plane] = kr0,kr1,kr2,kr3,kt0,kt1 */
  double center[2];      /* cx, cy (normalized) */
} tinydng_warp_rectilinear;

/* FixVignetteRadial (opcode 3): radial vignette correction. */
typedef struct tinydng_vignette_radial {
  uint32_t list;
  double k[5];
  double center[2]; /* cx, cy */
} tinydng_vignette_radial;

typedef struct tinydng_raw_info {
  int32_t black_level[4];
  uint8_t black_level_present;
  int32_t white_level[4];
  uint8_t white_level_present;
  double color_matrix1[9];
  double color_matrix2[9];
  double forward_matrix1[9];
  double forward_matrix2[9];
  double camera_calibration1[9];
  double camera_calibration2[9];
  uint8_t color_matrix_present;
  uint8_t camera_calibration_present;
  double analog_balance[3];
  uint8_t has_analog_balance;
  double as_shot_neutral[3];
  uint8_t has_as_shot_neutral;
  uint16_t calibration_illuminant1;
  uint16_t calibration_illuminant2;
  uint8_t dng_version[4];
  uint8_t has_dng_version;
  uint32_t active_area[4]; /* top, left, bottom, right */
  uint8_t has_active_area;
  uint16_t default_black_render;
  uint8_t has_default_black_render;
  char *profile_name;
  double profile_tone_curve[64];
  uint16_t profile_tone_curve_count;
  double noise_profile[8];
  uint16_t noise_profile_count;
  uint16_t cr2_slices[3];
  uint8_t has_cr2_slices;
  char *semantic_name; /* Apple ProRAW */
  uint16_t *linearization_table;
  size_t linearization_table_count;
  tinydng_gainmap *gainmaps;
  size_t gainmap_count;
  tinydng_opcode *opcodes; /* all opcodes across lists 1/2/3 */
  size_t opcode_count;
  tinydng_warp_rectilinear *warps;
  size_t warp_count;
  tinydng_vignette_radial *vignettes;
  size_t vignette_count;
} tinydng_raw_info;

typedef struct tinydng_field {
  uint16_t tag;
  uint16_t type;
  uint32_t count;
  uint8_t *data;
  size_t size;
} tinydng_field;

typedef enum tinydng_segment_kind {
  TINYDNG_SEG_STRIP = 0,
  TINYDNG_SEG_TILE = 1
} tinydng_segment_kind;

typedef struct tinydng_segment {
  uint64_t offset;     /* absolute file offset of compressed bytes */
  uint64_t byte_count; /* size on disk                            */
  uint32_t index;      /* strip# or tile#                         */
  uint32_t x, y, w, h; /* placement + extent in the output raster */
  uint16_t plane;      /* sample plane (planar config); 0 if chunky */
  tinydng_segment_kind kind;
} tinydng_segment;

typedef struct tinydng_image_info {
  /* container geometry */
  uint32_t width, height;
  uint16_t samples_per_pixel;
  uint16_t bits_per_sample;         /* stored bits per sample            */
  uint16_t bits_per_sample_decoded; /* after decode (e.g. LJPEG -> 16)   */
  uint16_t compression;
  uint16_t sample_format;
  uint16_t planar_configuration; /* 1 chunky, 2 planar */
  uint16_t predictor;            /* 1/2/3              */
  uint32_t rows_per_strip;
  uint32_t tile_width, tile_length;
  uint32_t jpeg_byte_count;

  /* lazy segment table */
  const tinydng_segment *segments;
  size_t segment_count;

  /* grouped metadata */
  tinydng_exif exif;
  tinydng_cfa cfa;
  tinydng_raw_info raw;
  tinydng_field *custom_fields;
  size_t custom_field_count;
} tinydng_image_info;

/* ------------------------------------------------------------------ */
/* Open / accessors                                                   */
/* ------------------------------------------------------------------ */

tinydng_status tinydng_open_memory(tinydng_context *ctx, const uint8_t *data,
                                   size_t n, const tinydng_open_options *opts,
                                   tinydng_document **out, tinydng_error *err);
tinydng_status tinydng_open_file(tinydng_context *ctx, const char *path,
                                 const tinydng_open_options *opts,
                                 tinydng_document **out, tinydng_error *err);
/* Takes ownership of `io` (closes it on document destroy). */
tinydng_status tinydng_open_io(tinydng_context *ctx, tinydng_io io,
                               const tinydng_open_options *opts,
                               tinydng_document **out, tinydng_error *err);
void tinydng_document_destroy(tinydng_context *ctx, tinydng_document *doc);

size_t tinydng_image_count(const tinydng_document *doc);
const tinydng_image_info *tinydng_image_get(const tinydng_document *doc,
                                            size_t index);
const tinydng_exif *tinydng_document_exif(const tinydng_document *doc);

size_t tinydng_image_segment_count(const tinydng_image_info *img);
tinydng_status tinydng_image_segment(const tinydng_image_info *img, size_t i,
                                     tinydng_segment *out);

/* ------------------------------------------------------------------ */
/* Lazy decode                                                        */
/* ------------------------------------------------------------------ */

typedef enum tinydng_decode_flags {
  TINYDNG_DEC_NONE = 0,
  TINYDNG_DEC_UNPREDICT = 1u << 0,  /* apply horizontal/FP predictor    */
  TINYDNG_DEC_KEEP_PACKED = 1u << 1 /* leave sub-byte packing intact     */
} tinydng_decode_flags;

typedef struct tinydng_decode_options {
  void *dst;          /* caller buffer; NULL => library allocates */
  size_t dst_capacity;
  uint32_t flags;
  /* Worker threads for multi-segment (tiled/striped) images: 0 = auto (one per
     online CPU), 1 = serial, N = use N. Capped to the segment count. Ignored
     for single-segment images and in builds compiled with
     TINYDNG_DISABLE_THREADS. Segments decode independently into disjoint
     output regions; a single decode call must not run concurrently with
     another call on the same context. */
  uint32_t num_threads;
} tinydng_decode_options;

typedef struct tinydng_pixels {
  uint8_t *data;
  size_t size;
  uint32_t width, height;
  uint16_t samples_per_pixel;
  uint16_t bits_per_sample;
  uint16_t sample_format;
  uint8_t owns_memory; /* 1 => free with tinydng_pixels_free */
} tinydng_pixels;

tinydng_status tinydng_decode_segment(tinydng_context *ctx,
                                      const tinydng_document *doc,
                                      size_t image_idx, size_t seg_idx,
                                      const tinydng_decode_options *opts,
                                      tinydng_pixels *out, tinydng_error *err);
tinydng_status tinydng_decode_region(tinydng_context *ctx,
                                     const tinydng_document *doc,
                                     size_t image_idx, uint32_t x, uint32_t y,
                                     uint32_t w, uint32_t h,
                                     const tinydng_decode_options *opts,
                                     tinydng_pixels *out, tinydng_error *err);
tinydng_status tinydng_decode_image(tinydng_context *ctx,
                                    const tinydng_document *doc,
                                    size_t image_idx,
                                    const tinydng_decode_options *opts,
                                    tinydng_pixels *out, tinydng_error *err);
void tinydng_pixels_free(tinydng_context *ctx, tinydng_pixels *px);

/* ------------------------------------------------------------------ */
/* Writer (uncompressed TIFF / DNG, single image)                     */
/* ------------------------------------------------------------------ */

typedef struct tinydng_write_options {
  uint8_t big_endian;   /* 0 => little-endian */
  uint8_t as_dng;       /* emit DNG-specific tags from `raw`/`cfa`     */
  uint16_t compression; /* 0/1 none, 5 LZW, 7 lossless JPEG            */
} tinydng_write_options;

typedef struct tinydng_write_image {
  uint32_t width, height;
  uint16_t samples_per_pixel;
  uint16_t bits_per_sample; /* 8/16/32                                 */
  uint16_t sample_format;   /* tinydng_sample_format; 0 => UINT        */
  uint16_t photometric;     /* 0 => auto (CFA/RGB/MINISBLACK)          */
  const uint8_t *data;      /* row-major, chunky, host byte order      */
  size_t data_size;
  const tinydng_cfa *cfa;      /* optional */
  const tinydng_raw_info *raw; /* optional DNG metadata */
} tinydng_write_image;

/* Serialize `img` to an in-memory TIFF/DNG buffer (allocated via ctx;
   free with tinydng_buffer_free). */
tinydng_status tinydng_write_memory(tinydng_context *ctx,
                                    const tinydng_write_image *img,
                                    const tinydng_write_options *opts,
                                    uint8_t **out_data, size_t *out_size,
                                    tinydng_error *err);
tinydng_status tinydng_write_file(tinydng_context *ctx, const char *path,
                                  const tinydng_write_image *img,
                                  const tinydng_write_options *opts,
                                  tinydng_error *err);
void tinydng_buffer_free(tinydng_context *ctx, uint8_t *buf);

#ifdef __cplusplus
}
#endif

#endif /* TINYDNG_H_ */
