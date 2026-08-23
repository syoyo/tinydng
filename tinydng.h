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
  uint32_t max_psd_layers;     /* PSD layer cap; 0 => 4096             */
  uint32_t max_psd_resources;  /* PSD 8BIM resource cap; 0 => 2048     */
  uint32_t max_psd_segments;   /* composite (RLE) seg cap; 0 => 1<<20  */
  uint32_t max_embed_depth;    /* smart-object recursion; 0 => 4       */
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

/* Common LightSource values (EXIF tag 0x9206 / DNG CalibrationIlluminant). */
#define TINYDNG_LIGHTSOURCE_UNKNOWN       0
#define TINYDNG_LIGHTSOURCE_DAYLIGHT      1
#define TINYDNG_LIGHTSOURCE_FLUORESCENT   2
#define TINYDNG_LIGHTSOURCE_TUNGSTEN      3
#define TINYDNG_LIGHTSOURCE_FLASH         4
#define TINYDNG_LIGHTSOURCE_FINE_WEATHER  9
#define TINYDNG_LIGHTSOURCE_CLOUDY        10
#define TINYDNG_LIGHTSOURCE_SHADE         11
#define TINYDNG_LIGHTSOURCE_DAYLIGHT_FLUORESCENT 12
#define TINYDNG_LIGHTSOURCE_D50           20
#define TINYDNG_LIGHTSOURCE_D55           21
#define TINYDNG_LIGHTSOURCE_D65           22
#define TINYDNG_LIGHTSOURCE_D75           23

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
  int8_t profile_embed_policy; /* 0=allow copy, 1=embed if used, 2=embed never; -1=not set */
  uint8_t has_profile_embed_policy;
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
  uint64_t jpeg_byte_count;

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
/* Quick format check (no full parse)                                  */
/* ------------------------------------------------------------------ */

/* Returns TINYDNG_OK if `path` starts with a valid TIFF/PSD/PSB header. */
tinydng_status tinydng_is_dng(const char *path, tinydng_error *err);

/* Returns TINYDNG_OK if `data`/`size` starts with a valid TIFF/PSD/PSB header. */
tinydng_status tinydng_is_dng_memory(const void *data, size_t size,
                                     tinydng_error *err);

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
  /* TINYDNG_DEC_UNPREDICT is currently a no-op by design: the decoded output
     already has the TIFF/LJPEG predictor applied by default, so there is no
     separate "unpredict" step to enable. Kept for API compatibility. */
  TINYDNG_DEC_UNPREDICT = 1u << 0,
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
/* PSD / PSB (Adobe Photoshop)                                        */
/*                                                                    */
/* A '8BPS' file opened through tinydng_open_* exposes its composite   */
/* (merged) image as doc image 0 (planar, big-endian; decode with the  */
/* normal tinydng_decode_* entry points). Everything PSD-specific --   */
/* layers, image resources, tagged blocks, smart objects -- lives in   */
/* the tinydng_psd_info reachable via tinydng_document_psd().          */
/* Compile out with TINYDNG_NO_PSD.                                    */
/* ------------------------------------------------------------------ */

typedef enum tinydng_psd_color_mode {
  TINYDNG_PSD_BITMAP = 0,
  TINYDNG_PSD_GRAYSCALE = 1,
  TINYDNG_PSD_INDEXED = 2,
  TINYDNG_PSD_RGB = 3,
  TINYDNG_PSD_CMYK = 4,
  TINYDNG_PSD_MULTICHANNEL = 7,
  TINYDNG_PSD_DUOTONE = 8,
  TINYDNG_PSD_LAB = 9
} tinydng_psd_color_mode;

typedef enum tinydng_psd_compression {
  TINYDNG_PSD_COMP_RAW = 0,
  TINYDNG_PSD_COMP_RLE = 1, /* PackBits, per scanline           */
  TINYDNG_PSD_COMP_ZIP = 2, /* zlib, whole channel              */
  TINYDNG_PSD_COMP_ZIP_PRED = 3 /* zlib + per-row delta prediction */
} tinydng_psd_compression;

#define TINYDNG_PSD_FOURCC(a, b, c, d)                                   \
  (((uint32_t)(uint8_t)(a) << 24) | ((uint32_t)(uint8_t)(b) << 16) |     \
   ((uint32_t)(uint8_t)(c) << 8) | (uint32_t)(uint8_t)(d))

typedef enum tinydng_psd_blend_mode {
  TINYDNG_PSD_BLEND_PASS_THROUGH = TINYDNG_PSD_FOURCC('p', 'a', 's', 's'),
  TINYDNG_PSD_BLEND_NORMAL = TINYDNG_PSD_FOURCC('n', 'o', 'r', 'm'),
  TINYDNG_PSD_BLEND_DISSOLVE = TINYDNG_PSD_FOURCC('d', 'i', 's', 's'),
  TINYDNG_PSD_BLEND_DARKEN = TINYDNG_PSD_FOURCC('d', 'a', 'r', 'k'),
  TINYDNG_PSD_BLEND_MULTIPLY = TINYDNG_PSD_FOURCC('m', 'u', 'l', ' '),
  TINYDNG_PSD_BLEND_COLOR_BURN = TINYDNG_PSD_FOURCC('i', 'd', 'i', 'v'),
  TINYDNG_PSD_BLEND_LINEAR_BURN = TINYDNG_PSD_FOURCC('l', 'b', 'r', 'n'),
  TINYDNG_PSD_BLEND_DARKER_COLOR = TINYDNG_PSD_FOURCC('d', 'k', 'C', 'l'),
  TINYDNG_PSD_BLEND_LIGHTEN = TINYDNG_PSD_FOURCC('l', 'i', 't', 'e'),
  TINYDNG_PSD_BLEND_SCREEN = TINYDNG_PSD_FOURCC('s', 'c', 'r', 'n'),
  TINYDNG_PSD_BLEND_COLOR_DODGE = TINYDNG_PSD_FOURCC('d', 'i', 'v', ' '),
  TINYDNG_PSD_BLEND_LINEAR_DODGE = TINYDNG_PSD_FOURCC('l', 'd', 'd', 'g'),
  TINYDNG_PSD_BLEND_LIGHTER_COLOR = TINYDNG_PSD_FOURCC('l', 'g', 'C', 'l'),
  TINYDNG_PSD_BLEND_OVERLAY = TINYDNG_PSD_FOURCC('o', 'v', 'e', 'r'),
  TINYDNG_PSD_BLEND_SOFT_LIGHT = TINYDNG_PSD_FOURCC('s', 'L', 'i', 't'),
  TINYDNG_PSD_BLEND_HARD_LIGHT = TINYDNG_PSD_FOURCC('h', 'L', 'i', 't'),
  TINYDNG_PSD_BLEND_VIVID_LIGHT = TINYDNG_PSD_FOURCC('v', 'L', 'i', 't'),
  TINYDNG_PSD_BLEND_LINEAR_LIGHT = TINYDNG_PSD_FOURCC('l', 'L', 'i', 't'),
  TINYDNG_PSD_BLEND_PIN_LIGHT = TINYDNG_PSD_FOURCC('p', 'L', 'i', 't'),
  TINYDNG_PSD_BLEND_HARD_MIX = TINYDNG_PSD_FOURCC('h', 'M', 'i', 'x'),
  TINYDNG_PSD_BLEND_DIFFERENCE = TINYDNG_PSD_FOURCC('d', 'i', 'f', 'f'),
  TINYDNG_PSD_BLEND_EXCLUSION = TINYDNG_PSD_FOURCC('s', 'm', 'u', 'd'),
  TINYDNG_PSD_BLEND_SUBTRACT = TINYDNG_PSD_FOURCC('f', 's', 'u', 'b'),
  TINYDNG_PSD_BLEND_DIVIDE = TINYDNG_PSD_FOURCC('f', 'd', 'i', 'v'),
  TINYDNG_PSD_BLEND_HUE = TINYDNG_PSD_FOURCC('h', 'u', 'e', ' '),
  TINYDNG_PSD_BLEND_SATURATION = TINYDNG_PSD_FOURCC('s', 'a', 't', ' '),
  TINYDNG_PSD_BLEND_COLOR = TINYDNG_PSD_FOURCC('c', 'o', 'l', 'r'),
  TINYDNG_PSD_BLEND_LUMINOSITY = TINYDNG_PSD_FOURCC('l', 'u', 'm', ' ')
} tinydng_psd_blend_mode;

typedef enum tinydng_psd_section {
  TINYDNG_PSD_SECTION_LAYER = 0,
  TINYDNG_PSD_SECTION_OPEN_FOLDER = 1,
  TINYDNG_PSD_SECTION_CLOSED_FOLDER = 2,
  TINYDNG_PSD_SECTION_DIVIDER = 3 /* hidden group-end marker */
} tinydng_psd_section;

/* Channel ids: 0..n-1 image components, -1 transparency (alpha),
   -2 user-supplied layer mask, -3 real user mask (when both exist). */
typedef struct tinydng_psd_channel {
  int16_t id;
  uint16_t compression; /* tinydng_psd_compression                   */
  uint64_t data_offset; /* absolute offset of payload (past the tag) */
  uint64_t data_length; /* payload bytes (excludes the 2-byte tag)   */
} tinydng_psd_channel;

typedef struct tinydng_psd_mask {
  int32_t top, left, bottom, right;
  uint8_t default_color;
  uint8_t flags;
  uint8_t present;
} tinydng_psd_mask;

/* A tagged block ('8BIM'/'8B64' + fourcc key), exposed as a byte range;
   fetch the raw contents with tinydng_psd_read_block(). */
typedef struct tinydng_psd_block {
  uint32_t key;    /* fourcc, e.g. 'luni','lsct','SoLd'    */
  uint64_t offset; /* absolute file offset of block data   */
  uint64_t length;
} tinydng_psd_block;

typedef struct tinydng_psd_layer {
  int32_t top, left, bottom, right; /* content rect (may be negative) */
  uint32_t width, height;           /* right-left / bottom-top        */
  char *name;          /* UTF-8; from 'luni' if present, else Pascal  */
  uint32_t blend_mode; /* fourcc (tinydng_psd_blend_mode)             */
  uint8_t opacity;     /* 0..255                                      */
  uint8_t clipping;    /* 0 base, 1 non-base                          */
  uint8_t flags;       /* bit1: visible==0, bit4: pixel-data-irrelevant */
  uint8_t section;     /* tinydng_psd_section (from 'lsct')           */
  uint32_t parent;     /* enclosing group layer index; UINT32_MAX=root */
  tinydng_psd_mask mask;
  tinydng_psd_channel *channels; /* context-owned */
  size_t channel_count;
  tinydng_psd_block *blocks; /* per-layer tagged blocks (context-owned) */
  size_t block_count;
} tinydng_psd_layer;

typedef struct tinydng_psd_resource { /* 8BIM image resource */
  uint16_t id;
  char *name;      /* Pascal name as UTF-8 (usually "") */
  uint64_t offset; /* absolute offset of resource data  */
  uint64_t length;
} tinydng_psd_resource;

typedef struct tinydng_psd_smart_object {
  uint32_t kind;     /* 'liFD' embedded, 'liFE' external, 'liFA' alias */
  char *uid;         /* unique id linking placed layers to this file   */
  char *filename;    /* original file name, UTF-8                      */
  uint32_t filetype; /* fourcc: '8BPS','JPEG','png ',...               */
  uint64_t data_offset; /* embedded raw bytes ('liFD' only; else 0)    */
  uint64_t data_length;
} tinydng_psd_smart_object;

typedef struct tinydng_psd_info {
  uint8_t is_psb;         /* header version 2 (large document)  */
  uint16_t channel_count; /* composite channels (1..56)         */
  uint16_t depth;         /* 1 / 8 / 16 / 32                    */
  uint16_t color_mode;    /* tinydng_psd_color_mode             */
  uint32_t width, height;
  uint16_t composite_compression; /* tinydng_psd_compression     */
  uint8_t has_transparency;       /* layer count was negative    */
  uint8_t has_composite;          /* composite section present   */
  /* Indexed palette (768 bytes, RGB planar) or duotone blob. */
  uint8_t *color_mode_data;
  size_t color_mode_data_size;
  tinydng_psd_layer *layers; /* file (bottom-up) order */
  size_t layer_count;
  tinydng_psd_resource *resources;
  size_t resource_count;
  tinydng_psd_block *global_blocks;
  size_t global_block_count;
  tinydng_psd_smart_object *smart_objects;
  size_t smart_object_count;
} tinydng_psd_info;

/* NULL when the document is not a PSD/PSB. */
const tinydng_psd_info *tinydng_document_psd(const tinydng_document *doc);

/* Decode a layer to interleaved pixels: image channels in id order followed
   by transparency (-1) when present; mask channels are excluded. 1-bit
   layers decode to 8-bit (0/255, black=255 inverted to intensity).
   opts->num_threads parallelizes across channels. */
tinydng_status tinydng_psd_decode_layer(tinydng_context *ctx,
                                        const tinydng_document *doc,
                                        size_t layer_idx,
                                        const tinydng_decode_options *opts,
                                        tinydng_pixels *out,
                                        tinydng_error *err);

/* Decode a single channel (by index into layer->channels; mask channels
   use the mask rect). Output is one plane. */
tinydng_status tinydng_psd_decode_layer_channel(
    tinydng_context *ctx, const tinydng_document *doc, size_t layer_idx,
    size_t channel_idx, const tinydng_decode_options *opts,
    tinydng_pixels *out, tinydng_error *err);

/* Copy `length` raw bytes at absolute `offset` (bounds-checked against the
   file). Free the returned buffer with tinydng_buffer_free(). */
tinydng_status tinydng_psd_read_block(tinydng_context *ctx,
                                      const tinydng_document *doc,
                                      uint64_t offset, uint64_t length,
                                      uint8_t **out_data, size_t *out_size,
                                      tinydng_error *err);

/* Decode image resource 1036 (or legacy 1033) JPEG thumbnail via stb. */
tinydng_status tinydng_psd_decode_thumbnail(tinydng_context *ctx,
                                            const tinydng_document *doc,
                                            tinydng_pixels *out,
                                            tinydng_error *err);

/* Open an embedded smart-object payload (PSD/PSB/TIFF/DNG) as a new
   document on the same context. Depth-limited by max_embed_depth. The new
   document views the payload in place (no copy): it aliases the parent
   document's io, so destroy the child before the parent and avoid
   concurrent reads of both on a seek-based (stdio) backend. */
tinydng_status tinydng_psd_smart_object_open(tinydng_context *ctx,
                                             const tinydng_document *doc,
                                             size_t so_idx,
                                             const tinydng_open_options *opts,
                                             tinydng_document **out,
                                             tinydng_error *err);

/* Decode an embedded smart-object payload to pixels: JPEG/PNG via stb,
   PSD/PSB/TIFF/DNG via a recursive open + decode of image 0. */
tinydng_status tinydng_psd_smart_object_decode(tinydng_context *ctx,
                                               const tinydng_document *doc,
                                               size_t so_idx,
                                               tinydng_pixels *out,
                                               tinydng_error *err);

/* ---- PSD writer ---- */

typedef struct tinydng_psd_write_channel {
  int16_t id;          /* 0..n-1 image, -1 transparency          */
  const uint8_t *data; /* one plane, host byte order, w*h samples */
  size_t size;         /* must equal w*h*depth/8                  */
} tinydng_psd_write_channel;

typedef struct tinydng_psd_write_layer {
  int32_t top, left, bottom, right;
  const char *name;    /* UTF-8; emitted as Pascal + 'luni'. NULL => "" */
  uint32_t blend_mode; /* 0 => 'norm'                                   */
  uint8_t opacity;     /* 0 => treated as 255                           */
  uint8_t clipping;
  uint8_t flags;
  uint8_t section; /* tinydng_psd_section; != 0 emits 'lsct' */
  const tinydng_psd_write_channel *channels;
  uint16_t channel_count;
} tinydng_psd_write_layer;

typedef struct tinydng_psd_write_doc {
  uint32_t width, height;
  uint16_t depth;         /* 8 / 16 / 32                            */
  uint16_t color_mode;    /* tinydng_psd_color_mode                 */
  uint16_t channel_count; /* composite channels                     */
  const uint8_t *composite; /* interleaved, host order; NULL => zeros */
  size_t composite_size;
  const uint8_t *palette; /* 768 bytes when color_mode == INDEXED   */
  const tinydng_psd_write_layer *layers;
  size_t layer_count;
  const uint8_t *icc; /* optional ICC profile => resource 1039      */
  size_t icc_size;
} tinydng_psd_write_doc;

typedef struct tinydng_psd_write_options {
  uint16_t compression; /* tinydng_psd_compression: RAW or RLE (default) */
  uint8_t as_psb;       /* version 2: 64-bit lengths, u32 RLE counts     */
} tinydng_psd_write_options;

tinydng_status tinydng_psd_write_memory(tinydng_context *ctx,
                                        const tinydng_psd_write_doc *doc,
                                        const tinydng_psd_write_options *opts,
                                        uint8_t **out_data, size_t *out_size,
                                        tinydng_error *err);
tinydng_status tinydng_psd_write_file(tinydng_context *ctx, const char *path,
                                      const tinydng_psd_write_doc *doc,
                                      const tinydng_psd_write_options *opts,
                                      tinydng_error *err);

/* ------------------------------------------------------------------ */
/* Writer (uncompressed TIFF / DNG, single image)                     */
/* ------------------------------------------------------------------ */

typedef struct tinydng_write_options {
  uint8_t big_endian;   /* 0 => little-endian */
  uint8_t as_dng;       /* emit DNG-specific tags from `raw`/`cfa`     */
  uint8_t bigtiff;      /* emit BigTIFF (version 43, 8-byte offsets)   */
  uint8_t ljpeg_arithmetic; /* compression=7: SOF11 instead of SOF3   */
  uint8_t ljpeg_predictor;  /* 0 => 1; otherwise Annex H selector 1..7 */
  uint16_t ljpeg_restart_interval_mcus; /* 0 => none; whole MCU rows   */
  uint16_t compression; /* 0/1 none, 5 LZW, 7 lossless JPEG, 32773 PackBits */
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

/* ------------------------------------------------------------------ */
/* Streaming writer (tiled / multi-strip TIFF and DNG)                */
/*                                                                     */
/* Writes the pixel payloads incrementally as each tile/strip is       */
/* encoded, keeping only the (small) offset/count table in memory.     */
/* The image pixels are never buffered by the library.                 */
/*                                                                     */
/* The sink uses absolute-offset writes because TIFF requires          */
/* patching the header's first-IFD pointer at finish: a seekable sink  */
/* (file, memory) is required. tinydng_write_io_open_file/memory build */
/* the two built-in backends.                                          */
/* ------------------------------------------------------------------ */

typedef struct tinydng_write_io tinydng_write_io;
struct tinydng_write_io {
  /* Write len bytes at absolute off. Returns bytes written; a return
     < len is a sink failure (aborts the write). */
  size_t (*write)(tinydng_write_io *io, uint64_t off, const void *data,
                  size_t len);
  /* Current stream size (end of written data). */
  uint64_t (*size)(tinydng_write_io *io);
  /* Release backend resources. May be NULL. */
  void (*close)(tinydng_write_io *io);
  void *backend;
};

tinydng_status tinydng_write_io_open_file(tinydng_context *ctx,
                                          const char *path,
                                          tinydng_write_io *out,
                                          tinydng_error *err);
/* Note: open_file creates/truncates the destination immediately ("w+b");
   a failed write leaves a partial file behind. */
tinydng_status tinydng_write_io_open_memory(tinydng_context *ctx,
                                            tinydng_write_io *out,
                                            tinydng_error *err);
/* Detach the memory backend's buffer (owned by ctx; free with
   tinydng_buffer_free). The io is still closed with io->close. */
tinydng_status tinydng_write_io_memory_take(tinydng_context *ctx,
                                            tinydng_write_io *io,
                                            uint8_t **out_data,
                                            size_t *out_size,
                                            tinydng_error *err);

typedef struct tinydng_tiling {
  uint32_t tile_width;    /* >0 => tiled layout (TileWidth/TileLength) */
  uint32_t tile_length;
  uint32_t rows_per_strip; /* strips: >0 => multi-strip, 0 => whole image */
} tinydng_tiling;

typedef struct tinydng_writer tinydng_writer;

/* Create a streaming writer. `meta` supplies the geometry + DNG metadata;
   meta->data/data_size are ignored (pixels arrive per tile/strip).
   On failure the sink is NOT closed (the caller keeps ownership); on
   success tinydng_writer_finish releases writer state but the sink still
   needs io.close() from the caller. */
tinydng_status tinydng_writer_create(tinydng_context *ctx,
                                     tinydng_write_io sink,
                                     const tinydng_write_image *meta,
                                     const tinydng_write_options *opts,
                                     const tinydng_tiling *tiling,
                                     tinydng_writer **out,
                                     tinydng_error *err);

/* Encode + write one tile (tiled layout; row-major, chunky). `pixels`
   holds w_tile*h_tile*spp*(bps/8) bytes in host byte order where w_tile/
   h_tile are the edge-cropped tile dims (min(tile_width, width-x), ...).
   Lossless JPEG requires bps == 16. */
tinydng_status tinydng_writer_write_tile(tinydng_writer *w,
                                         uint32_t tile_index,
                                         const void *pixels,
                                         tinydng_error *err);

/* Encode + write one strip (striped layout). `pixels` holds
   w*strip_h*spp*(bps/8) bytes (the last strip may be shorter). */
tinydng_status tinydng_writer_write_strip(tinydng_writer *w,
                                          uint32_t strip_index,
                                          const void *pixels,
                                          tinydng_error *err);

/* Write the IFD + extras, patch the header, and release the writer.
   The handle is invalid after this call. */
tinydng_status tinydng_writer_finish(tinydng_writer *w, tinydng_error *err);

#ifdef __cplusplus
}
#endif

#endif /* TINYDNG_H_ */
