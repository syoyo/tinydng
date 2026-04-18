#include "tiny_dng_v2.h"

#include "tiny_dng_ljpeg92_v2.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#define TINYDNG_V2_HAS_MMAP 1
#else
#define TINYDNG_V2_HAS_MMAP 0
#endif

#define TINYDNG_V2_MAGIC 0x54444732u
#define TINYDNG_V2_TYPE_SHORT 3u
#define TINYDNG_V2_TYPE_LONG 4u

#define TINYDNG_V2_TAG_IMAGE_WIDTH 256u
#define TINYDNG_V2_TAG_IMAGE_LENGTH 257u
#define TINYDNG_V2_TAG_BITS_PER_SAMPLE 258u
#define TINYDNG_V2_TAG_COMPRESSION 259u
#define TINYDNG_V2_TAG_STRIP_OFFSETS 273u
#define TINYDNG_V2_TAG_SAMPLES_PER_PIXEL 277u
#define TINYDNG_V2_TAG_ROWS_PER_STRIP 278u
#define TINYDNG_V2_TAG_STRIP_BYTE_COUNTS 279u
#define TINYDNG_V2_TAG_PLANAR_CONFIGURATION 284u
#define TINYDNG_V2_TAG_SUB_IFDS 330u
#define TINYDNG_V2_TAG_TILE_WIDTH 322u
#define TINYDNG_V2_TAG_TILE_LENGTH 323u
#define TINYDNG_V2_TAG_TILE_OFFSETS 324u
#define TINYDNG_V2_TAG_TILE_BYTE_COUNTS 325u
#define TINYDNG_V2_TAG_JPEG_IF_BYTE_COUNT 514u
#define TINYDNG_V2_TAG_SAMPLE_FORMAT 339u
#define TINYDNG_V2_TAG_ORIENTATION 274u
#define TINYDNG_V2_TAG_MAKE 271u
#define TINYDNG_V2_TAG_MODEL 272u
#define TINYDNG_V2_TAG_SOFTWARE 305u
#define TINYDNG_V2_TAG_DATETIME 306u
#define TINYDNG_V2_TAG_IMAGEDESCRIPTION 270u
#define TINYDNG_V2_TAG_CFA_REPEAT_PATTERN_DIM 33421u
#define TINYDNG_V2_TAG_CFA_PATTERN 33422u
#define TINYDNG_V2_TAG_CFA_PLANE_COLOR 50710u
#define TINYDNG_V2_TAG_CFA_LAYOUT 50711u
#define TINYDNG_V2_TAG_BLACK_LEVEL 50714u
#define TINYDNG_V2_TAG_WHITE_LEVEL 50717u
#define TINYDNG_V2_TAG_COLOR_MATRIX1 50721u
#define TINYDNG_V2_TAG_COLOR_MATRIX2 50722u
#define TINYDNG_V2_TAG_FORWARD_MATRIX1 50964u
#define TINYDNG_V2_TAG_FORWARD_MATRIX2 50965u
#define TINYDNG_V2_TAG_DNG_VERSION 50706u
#define TINYDNG_V2_TAG_AS_SHOT_NEUTRAL 50728u
#define TINYDNG_V2_TAG_CALIBRATION_ILLUMINANT1 50778u
#define TINYDNG_V2_TAG_CALIBRATION_ILLUMINANT2 50779u
#define TINYDNG_V2_TAG_ACTIVE_AREA 50829u
#define TINYDNG_V2_TAG_DEFAULT_BLACK_RENDER 50713u
#define TINYDNG_V2_TAG_PROFILE_NAME 50936u
#define TINYDNG_V2_TAG_PROFILE_TONE_CURVE 50940u
#define TINYDNG_V2_TAG_NOISE_PROFILE 51041u
#define TINYDNG_V2_TAG_CAMERA_CALIBRATION1 50723u
#define TINYDNG_V2_TAG_CAMERA_CALIBRATION2 50724u

#define TINYDNG_V2_COMP_NONE 1u
#define TINYDNG_V2_COMP_LZW 5u
#define TINYDNG_V2_COMP_OLD_JPEG 6u
#define TINYDNG_V2_COMP_NEW_JPEG 7u
#define TINYDNG_V2_COMP_ZIP 8u

#define TINYDNG_V2_SAMPLEFORMAT_UINT 1u

typedef struct tdng_alloc_header {
  uint32_t magic;
  size_t size;
  struct tdng_alloc_header* prev;
  struct tdng_alloc_header* next;
} tdng_alloc_header;

struct tinydng_v2_context {
  tinydng_v2_allocator allocator;
  size_t memory_cap_bytes;
  uint32_t max_images;
  size_t memory_used;
  size_t memory_peak;
  uint8_t alloc_failed;
  tdng_alloc_header* alloc_head;
};

struct tinydng_v2_document {
  size_t image_count;
  tinydng_v2_image* images;
  tinydng_v2_basic_exif global_exif;
  const uint8_t* mapped_data;
  size_t mapped_size;
  int mapped_fd;
  uint8_t owns_mmap;
  uint8_t owns_file_buffer;
};

typedef struct tdng_reader {
  const uint8_t* data;
  size_t size;
  uint8_t big_endian;
} tdng_reader;

typedef struct tdng_ifd_build {
  uint32_t width;
  uint32_t height;
  uint16_t bits_per_sample;
  uint16_t samples_per_pixel;
  uint16_t compression;
  uint16_t sample_format;
  uint32_t rows_per_strip;
  uint32_t jpeg_if_byte_count;
  uint32_t tile_width;
  uint32_t tile_length;
  uint32_t* strip_offsets;
  uint32_t* strip_byte_counts;
  size_t strip_count;
  size_t strip_byte_count_count;
  uint32_t* tile_offsets;
  uint32_t* tile_byte_counts;
  size_t tile_count;
  size_t tile_byte_count_count;
  uint32_t* sub_ifds;
  size_t sub_ifd_count;
  uint8_t has_width;
  uint8_t has_height;
  uint8_t has_bps;
  uint8_t has_spp;
  uint8_t has_compression;
  char* exif_make;
  char* exif_model;
  char* exif_software;
  char* exif_datetime;
  char* exif_image_description;
  uint16_t exif_orientation;
  uint8_t has_exif_make;
  uint8_t has_exif_model;
  uint8_t has_exif_software;
  uint8_t has_exif_datetime;
  uint8_t has_exif_image_description;
  uint8_t has_exif_orientation;
  uint16_t cfa_pattern_dim[2];
  uint8_t cfa_pattern[16];
  uint8_t cfa_pattern_size;
  uint8_t has_cfa_pattern;
  uint8_t cfa_plane_color[4];
  uint8_t has_cfa_plane_color;
  uint16_t cfa_layout;
  uint8_t has_cfa_layout;
  int32_t black_level[4];
  uint8_t has_black_level;
  int32_t white_level[4];
  uint8_t has_white_level;
  double color_matrix1[9];
  double color_matrix2[9];
  double forward_matrix1[9];
  double forward_matrix2[9];
  uint8_t has_color_matrix;
  uint8_t dng_version[4];
  uint8_t has_dng_version;
  double as_shot_neutral[3];
  uint8_t has_as_shot_neutral;
  uint16_t calibration_illuminant1;
  uint8_t has_calibration_illuminant1;
  uint16_t calibration_illuminant2;
  uint8_t has_calibration_illuminant2;
  uint32_t active_area[4];
  uint8_t has_active_area;
  uint16_t default_black_render;
  uint8_t has_default_black_render;
  char* profile_name;
  double profile_tone_curve[16];
  uint16_t profile_tone_curve_count;
  double noise_profile[8];
  uint16_t noise_profile_count;
  double camera_calibration1[9];
  double camera_calibration2[9];
  uint8_t has_camera_calibration;
} tdng_ifd_build;

static void tdng_destroy_image_payload(tinydng_v2_context* ctx,
                                       tinydng_v2_image* image);

static void tdng_set_error(tinydng_v2_error* err, tinydng_v2_status code,
                           tinydng_v2_error_stage stage, uint32_t ifd_index,
                           uint16_t tag, uint64_t offset,
                           const char* fmt, ...) {
  va_list args;

  if (!err) {
    return;
  }

  err->code = code;
  err->stage = stage;
  err->ifd_index = ifd_index;
  err->tag = tag;
  err->offset = offset;
  err->message[0] = '\0';

  if (!fmt) {
    return;
  }

  va_start(args, fmt);
  (void)vsnprintf(err->message, sizeof(err->message), fmt, args);
  va_end(args);
}

void tinydng_v2_error_clear(tinydng_v2_error* err) {
  if (!err) {
    return;
  }
  memset(err, 0, sizeof(*err));
}

const char* tinydng_v2_status_string(tinydng_v2_status status) {
  switch (status) {
    case TINYDNG_V2_STATUS_OK:
      return "ok";
    case TINYDNG_V2_STATUS_INVALID_ARGUMENT:
      return "invalid argument";
    case TINYDNG_V2_STATUS_PARSE_ERROR:
      return "parse error";
    case TINYDNG_V2_STATUS_UNSUPPORTED:
      return "unsupported";
    case TINYDNG_V2_STATUS_IO_ERROR:
      return "io error";
    case TINYDNG_V2_STATUS_OOM:
      return "out of memory";
    case TINYDNG_V2_STATUS_BOUNDS_ERROR:
      return "bounds error";
    case TINYDNG_V2_STATUS_INTERNAL_ERROR:
      return "internal error";
    default:
      return "unknown";
  }
}

static void* tdng_default_malloc(void* user_data, size_t size) {
  (void)user_data;
  return malloc(size);
}

static void tdng_default_free(void* user_data, void* ptr) {
  (void)user_data;
  free(ptr);
}

static int tdng_safe_add_size(size_t a, size_t b, size_t* out) {
  if (SIZE_MAX - a < b) {
    return 0;
  }
  *out = a + b;
  return 1;
}

static int tdng_safe_mul_size(size_t a, size_t b, size_t* out) {
  if ((a != 0u) && (b > (SIZE_MAX / a))) {
    return 0;
  }
  *out = a * b;
  return 1;
}

static void* tdng_ctx_alloc(tinydng_v2_context* ctx, size_t size,
                            tinydng_v2_error* err) {
  tdng_alloc_header* h = NULL;
  size_t total = 0;
  void* raw = NULL;

  if (!ctx) {
    return NULL;
  }

  if (!tdng_safe_add_size(sizeof(tdng_alloc_header), size, &total)) {
    tdng_set_error(err, TINYDNG_V2_STATUS_OOM, TINYDNG_V2_STAGE_ALLOCATOR, 0,
                   0, 0, "allocation size overflow (%zu + %zu)",
                   sizeof(tdng_alloc_header), size);
    ctx->alloc_failed = 1;
    return NULL;
  }

  if ((ctx->memory_cap_bytes > 0u) &&
      ((size > (ctx->memory_cap_bytes - ctx->memory_used)))) {
    tdng_set_error(err, TINYDNG_V2_STATUS_OOM, TINYDNG_V2_STAGE_ALLOCATOR, 0,
                   0, 0, "memory cap exceeded: requested=%zu used=%zu cap=%zu",
                   size, ctx->memory_used, ctx->memory_cap_bytes);
    ctx->alloc_failed = 1;
    return NULL;
  }

  raw = ctx->allocator.malloc_fn(ctx->allocator.user_data, total);
  if (!raw) {
    tdng_set_error(err, TINYDNG_V2_STATUS_OOM, TINYDNG_V2_STAGE_ALLOCATOR, 0,
                   0, 0, "allocator returned null for %zu bytes", total);
    ctx->alloc_failed = 1;
    return NULL;
  }

  h = (tdng_alloc_header*)raw;
  h->magic = TINYDNG_V2_MAGIC;
  h->size = size;
  h->prev = NULL;
  h->next = ctx->alloc_head;
  if (ctx->alloc_head) {
    ctx->alloc_head->prev = h;
  }
  ctx->alloc_head = h;

  ctx->memory_used += size;
  if (ctx->memory_used > ctx->memory_peak) {
    ctx->memory_peak = ctx->memory_used;
  }

  return (void*)(h + 1);
}

static void tdng_ctx_free(tinydng_v2_context* ctx, void* ptr) {
  tdng_alloc_header* h;
  if (!ctx || !ptr) {
    return;
  }

  h = ((tdng_alloc_header*)ptr) - 1;
  if (h->magic != TINYDNG_V2_MAGIC) {
    return;
  }

  if (h->prev) {
    h->prev->next = h->next;
  } else {
    ctx->alloc_head = h->next;
  }
  if (h->next) {
    h->next->prev = h->prev;
  }

  if (ctx->memory_used >= h->size) {
    ctx->memory_used -= h->size;
  } else {
    ctx->memory_used = 0;
  }

  h->magic = 0;
  ctx->allocator.free_fn(ctx->allocator.user_data, h);
}

static void tdng_ctx_free_all(tinydng_v2_context* ctx) {
  tdng_alloc_header* h;
  tdng_alloc_header* next;
  if (!ctx) {
    return;
  }

  h = ctx->alloc_head;
  while (h) {
    next = h->next;
    h->magic = 0;
    ctx->allocator.free_fn(ctx->allocator.user_data, h);
    h = next;
  }

  ctx->alloc_head = NULL;
  ctx->memory_used = 0;
}

tinydng_v2_context* tinydng_v2_context_create(const tinydng_v2_config* config,
                                              tinydng_v2_error* err) {
  tinydng_v2_context temp;
  tinydng_v2_context* ctx;

  tinydng_v2_error_clear(err);

  memset(&temp, 0, sizeof(temp));

  if (config && config->allocator.malloc_fn && config->allocator.free_fn) {
    temp.allocator = config->allocator;
  } else {
    temp.allocator.malloc_fn = tdng_default_malloc;
    temp.allocator.free_fn = tdng_default_free;
    temp.allocator.user_data = NULL;
  }

  if (config && (config->memory_cap_bytes > 0u)) {
    temp.memory_cap_bytes = config->memory_cap_bytes;
  } else {
    temp.memory_cap_bytes = (size_t)TINYDNG_V2_DEFAULT_MEMORY_CAP_BYTES;
  }
  temp.max_images = (config && config->max_images > 0u) ? config->max_images : 1024u;

  ctx = (tinydng_v2_context*)temp.allocator.malloc_fn(temp.allocator.user_data,
                                                      sizeof(*ctx));
  if (!ctx) {
    tdng_set_error(err, TINYDNG_V2_STATUS_OOM, TINYDNG_V2_STAGE_ALLOCATOR, 0,
                   0, 0, "failed to allocate context");
    return NULL;
  }

  *ctx = temp;
  return ctx;
}

void tinydng_v2_context_destroy(tinydng_v2_context* ctx) {
  if (!ctx) {
    return;
  }

  tdng_ctx_free_all(ctx);
  ctx->allocator.free_fn(ctx->allocator.user_data, ctx);
}

size_t tinydng_v2_context_memory_used(const tinydng_v2_context* ctx) {
  return ctx ? ctx->memory_used : 0;
}

size_t tinydng_v2_context_memory_peak(const tinydng_v2_context* ctx) {
  return ctx ? ctx->memory_peak : 0;
}

static int tdng_read_u16(const tdng_reader* r, size_t at, uint16_t* out) {
  if (!r || !out || (at + 2u > r->size)) {
    return 0;
  }
  if (r->big_endian) {
    *out = (uint16_t)(((uint16_t)r->data[at] << 8) | (uint16_t)r->data[at + 1]);
  } else {
    *out = (uint16_t)(((uint16_t)r->data[at + 1] << 8) | (uint16_t)r->data[at]);
  }
  return 1;
}

static int tdng_read_u32(const tdng_reader* r, size_t at, uint32_t* out) {
  if (!r || !out || (at + 4u > r->size)) {
    return 0;
  }
  if (r->big_endian) {
    *out = ((uint32_t)r->data[at] << 24) | ((uint32_t)r->data[at + 1] << 16) |
           ((uint32_t)r->data[at + 2] << 8) | (uint32_t)r->data[at + 3];
  } else {
    *out = ((uint32_t)r->data[at + 3] << 24) |
           ((uint32_t)r->data[at + 2] << 16) |
           ((uint32_t)r->data[at + 1] << 8) | (uint32_t)r->data[at];
  }
  return 1;
}

static int tdng_read_i32(const tdng_reader* r, size_t at, int32_t* out) {
  if (!r || !out || (at + 4u > r->size)) {
    return 0;
  }
  if (r->big_endian) {
    *out = ((int32_t)r->data[at] << 24) | ((int32_t)r->data[at + 1] << 16) |
           ((int32_t)r->data[at + 2] << 8) | (int32_t)r->data[at + 3];
  } else {
    *out = ((int32_t)r->data[at + 3] << 24) |
           ((int32_t)r->data[at + 2] << 16) |
           ((int32_t)r->data[at + 1] << 8) | (int32_t)r->data[at];
  }
  return 1;
}

static int tdng_read_u64(const tdng_reader* r, size_t at, uint64_t* out) {
  if (!r || !out || (at + 8u > r->size)) {
    return 0;
  }
  if (r->big_endian) {
    *out = ((uint64_t)r->data[at] << 56) | ((uint64_t)r->data[at + 1] << 48) |
           ((uint64_t)r->data[at + 2] << 40) | ((uint64_t)r->data[at + 3] << 32) |
           ((uint64_t)r->data[at + 4] << 24) | ((uint64_t)r->data[at + 5] << 16) |
           ((uint64_t)r->data[at + 6] << 8) | (uint64_t)r->data[at + 7];
  } else {
    *out = ((uint64_t)r->data[at + 7] << 56) | ((uint64_t)r->data[at + 6] << 48) |
           ((uint64_t)r->data[at + 5] << 40) | ((uint64_t)r->data[at + 4] << 32) |
           ((uint64_t)r->data[at + 3] << 24) | ((uint64_t)r->data[at + 2] << 16) |
           ((uint64_t)r->data[at + 1] << 8) | (uint64_t)r->data[at];
  }
  return 1;
}

static size_t tdng_tiff_type_size(uint16_t type) {
  switch (type) {
    case 1:
    case 2:
    case 6:
    case 7:
      return 1;
    case 3:
    case 8:
      return 2;
    case 4:
    case 9:
    case 11:
    case 13:
      return 4;
    case 5:
    case 10:
    case 12:
      return 8;
    default:
      return 0;
  }
}

static int tdng_extract_inline_u16(const tdng_reader* r, uint32_t v, uint16_t* out) {
  if (!out || !r) {
    return 0;
  }
  if (r->big_endian) {
    *out = (uint16_t)(v >> 16);
  } else {
    *out = (uint16_t)(v & 0xFFFFu);
  }
  return 1;
}

static char* tdng_read_string(tinydng_v2_context* ctx, const tdng_reader* r,
                              size_t offset, size_t count,
                              tinydng_v2_error* err) {
  if (!ctx || !r) return NULL;
  if (count == 0) return NULL;
  if (offset + count > r->size) return NULL;

  char* str = (char*)tdng_ctx_alloc(ctx, count + 1, err);
  if (!str) return NULL;

  memcpy(str, r->data + offset, count);
  str[count] = '\0';
  return str;
}

static int tdng_read_u32_array(tinydng_v2_context* ctx, const tdng_reader* r,
                               uint16_t type, uint32_t count, uint32_t value_or_offset,
                               uint32_t** out_arr, size_t* out_count,
                               tinydng_v2_error* err, uint32_t ifd_index,
                               uint16_t tag) {
  uint32_t* arr;
  size_t i;
  size_t byte_len;
  size_t offset;

  if (!ctx || !r || !out_arr || !out_count) {
    return 0;
  }

  if (!tdng_safe_mul_size((size_t)count, sizeof(uint32_t), &byte_len)) {
    tdng_set_error(err, TINYDNG_V2_STATUS_BOUNDS_ERROR, TINYDNG_V2_STAGE_PARSE_IFD,
                   ifd_index, tag, value_or_offset,
                   "u32 array size overflow (count=%u)", count);
    return 0;
  }

  arr = (uint32_t*)tdng_ctx_alloc(ctx, byte_len, err);
  if (!arr) {
    return 0;
  }

  if (count == 1u) {
    if (type == TINYDNG_V2_TYPE_SHORT) {
      uint16_t s = 0;
      if (!tdng_extract_inline_u16(r, value_or_offset, &s)) {
        tdng_ctx_free(ctx, arr);
        return 0;
      }
      arr[0] = (uint32_t)s;
    } else if (type == TINYDNG_V2_TYPE_LONG) {
      arr[0] = value_or_offset;
    } else {
      tdng_set_error(err, TINYDNG_V2_STATUS_PARSE_ERROR, TINYDNG_V2_STAGE_PARSE_IFD,
                     ifd_index, tag, value_or_offset,
                     "unsupported array type=%u", (unsigned)type);
      tdng_ctx_free(ctx, arr);
      return 0;
    }
  } else {
    offset = (size_t)value_or_offset;
    for (i = 0; i < (size_t)count; i++) {
      if (type == TINYDNG_V2_TYPE_SHORT) {
        uint16_t v16 = 0;
        if (offset > SIZE_MAX - (i * 2u)) {
          tdng_set_error(err, TINYDNG_V2_STATUS_BOUNDS_ERROR,
                         TINYDNG_V2_STAGE_PARSE_IFD, ifd_index, tag,
                         value_or_offset, "overflow in offset calculation");
          tdng_ctx_free(ctx, arr);
          return 0;
        }
        if (!tdng_read_u16(r, offset + i * 2u, &v16)) {
          tdng_set_error(err, TINYDNG_V2_STATUS_BOUNDS_ERROR,
                         TINYDNG_V2_STAGE_PARSE_IFD, ifd_index, tag,
                         value_or_offset, "failed to read SHORT array");
          tdng_ctx_free(ctx, arr);
          return 0;
        }
        arr[i] = (uint32_t)v16;
      } else if (type == TINYDNG_V2_TYPE_LONG) {
        uint32_t v32 = 0;
        if (offset > SIZE_MAX - (i * 4u)) {
          tdng_set_error(err, TINYDNG_V2_STATUS_BOUNDS_ERROR,
                         TINYDNG_V2_STAGE_PARSE_IFD, ifd_index, tag,
                         value_or_offset, "overflow in offset calculation");
          tdng_ctx_free(ctx, arr);
          return 0;
        }
        if (!tdng_read_u32(r, offset + i * 4u, &v32)) {
          tdng_set_error(err, TINYDNG_V2_STATUS_BOUNDS_ERROR,
                         TINYDNG_V2_STAGE_PARSE_IFD, ifd_index, tag,
                         value_or_offset, "failed to read LONG array");
          tdng_ctx_free(ctx, arr);
          return 0;
        }
        arr[i] = v32;
      } else {
        tdng_set_error(err, TINYDNG_V2_STATUS_PARSE_ERROR,
                       TINYDNG_V2_STAGE_PARSE_IFD, ifd_index, tag,
                       value_or_offset, "unsupported array type=%u",
                       (unsigned)type);
        tdng_ctx_free(ctx, arr);
        return 0;
      }
    }
  }

  *out_arr = arr;
  *out_count = (size_t)count;
  return 1;
}

static int tdng_decode_strips_uncompressed(tinydng_v2_context* ctx,
                                           const tdng_reader* r,
                                           const tdng_ifd_build* b,
                                           tinydng_v2_image* out,
                                           tinydng_v2_error* err,
                                           uint32_t ifd_index) {
  size_t bits_total;
  size_t pixel_count;
  size_t expected_size;
  size_t copied = 0;
  size_t i;
  uint8_t* dst;

  if (b->bits_per_sample == 0u) {
    tdng_set_error(err, TINYDNG_V2_STATUS_UNSUPPORTED, TINYDNG_V2_STAGE_DECODE,
                   ifd_index, TINYDNG_V2_TAG_BITS_PER_SAMPLE, 0,
                   "bits_per_sample=%u invalid",
                   (unsigned)b->bits_per_sample);
    return 0;
  }

  if (!tdng_safe_mul_size((size_t)b->width, (size_t)b->height, &pixel_count) ||
      !tdng_safe_mul_size(pixel_count, (size_t)b->samples_per_pixel,
                          &pixel_count) ||
      !tdng_safe_mul_size(pixel_count, (size_t)b->bits_per_sample,
                          &bits_total)) {
    tdng_set_error(err, TINYDNG_V2_STATUS_BOUNDS_ERROR, TINYDNG_V2_STAGE_DECODE,
                   ifd_index, 0, 0, "decoded image size overflow");
    return 0;
  }
  if ((bits_total % 8u) != 0u) {
    tdng_set_error(err, TINYDNG_V2_STATUS_UNSUPPORTED, TINYDNG_V2_STAGE_DECODE,
                   ifd_index, TINYDNG_V2_TAG_BITS_PER_SAMPLE, 0,
                   "packed uncompressed bitstream is not byte-aligned");
    return 0;
  }
  expected_size = bits_total / 8u;

  dst = (uint8_t*)tdng_ctx_alloc(ctx, expected_size, err);
  if (!dst) {
    return 0;
  }

  for (i = 0; i < b->strip_count; i++) {
    size_t off = b->strip_offsets[i];
    size_t len = b->strip_byte_counts[i];
    if ((off + len > r->size) || (copied + len > expected_size)) {
      tdng_set_error(err, TINYDNG_V2_STATUS_BOUNDS_ERROR,
                     TINYDNG_V2_STAGE_DECODE, ifd_index,
                     TINYDNG_V2_TAG_STRIP_OFFSETS, off,
                     "invalid strip[%zu] off=%zu len=%zu expected=%zu copied=%zu",
                     i, off, len, expected_size, copied);
      tdng_ctx_free(ctx, dst);
      return 0;
    }
    memcpy(dst + copied, r->data + off, len);
    copied += len;
  }

  if (copied != expected_size) {
    tdng_set_error(err, TINYDNG_V2_STATUS_PARSE_ERROR, TINYDNG_V2_STAGE_DECODE,
                   ifd_index, TINYDNG_V2_TAG_STRIP_BYTE_COUNTS, 0,
                   "decoded size mismatch copied=%zu expected=%zu", copied,
                   expected_size);
    tdng_ctx_free(ctx, dst);
    return 0;
  }

  out->data = dst;
  out->data_size = expected_size;
  out->bits_per_sample = b->bits_per_sample;
  out->flags |= TINYDNG_V2_IMAGE_FLAG_DATA_OWNS_MEMORY;
  return 1;
}

static int tdng_decode_ljpeg(tinydng_v2_context* ctx, const tdng_reader* r,
                             const tdng_ifd_build* b, tinydng_v2_image* out,
                             tinydng_v2_error* err, uint32_t ifd_index) {
  tdng_lj92 lj = NULL;
  int w = 0;
  int h = 0;
  int bits = 0;
  int components = 0;
  uint16_t* dst_u16;
  uint8_t* dst_bytes;
  size_t pixel_count;
  size_t data_size;
  size_t off;
  size_t len;
  int ret;

  if (b->strip_count < 1u) {
    tdng_set_error(err, TINYDNG_V2_STATUS_PARSE_ERROR, TINYDNG_V2_STAGE_DECODE,
                   ifd_index, TINYDNG_V2_TAG_STRIP_OFFSETS, 0,
                   "jpeg path needs at least one strip");
    return 0;
  }

  off = b->strip_offsets[0];
  len = b->jpeg_if_byte_count ? (size_t)b->jpeg_if_byte_count
                              : (size_t)b->strip_byte_counts[0];
  if ((len == 0u) || (off >= r->size) || (off + len > r->size)) {
    tdng_set_error(err, TINYDNG_V2_STATUS_BOUNDS_ERROR, TINYDNG_V2_STAGE_DECODE,
                   ifd_index, TINYDNG_V2_TAG_JPEG_IF_BYTE_COUNT, off,
                   "invalid jpeg payload off=%zu len=%zu size=%zu", off, len,
                   r->size);
    return 0;
  }

  ret = tdng_lj92_open(&lj, r->data + off, (int)len, &w, &h, &bits, &components);
  if (ret == TDNG_LJ92_ERROR_NOT_LOSSLESS) {
    // This is a baseline/progressive JPEG (SOF0/1/2), not lossless.
    // Copy the raw JPEG data since the file buffer will be freed after loading.
    // Use the image dimensions from the TIFF tags (b->width/height), not from
    // the JPEG header which may differ.
    uint8_t* jpeg_copy = (uint8_t*)tdng_ctx_alloc(ctx, len, err);
    if (!jpeg_copy) {
      tdng_set_error(err, TINYDNG_V2_STATUS_OOM, TINYDNG_V2_STAGE_DECODE,
                     ifd_index, TINYDNG_V2_TAG_COMPRESSION, off,
                     "failed to allocate memory for baseline JPEG");
      return 0;
    }
    memcpy(jpeg_copy, r->data + off, len);
    out->width = b->width;
    out->height = b->height;
    out->bits_per_sample = 8u;  // Baseline JPEG is always 8-bit
    out->bits_per_sample_stored = 8u;
    out->samples_per_pixel = b->samples_per_pixel;
    out->compression = TINYDNG_V2_COMP_NEW_JPEG;
    out->data = jpeg_copy;
    out->data_size = len;
    out->data_offset = (uint64_t)off;
    out->segment_count = 1u;
    out->flags |= TINYDNG_V2_IMAGE_FLAG_DATA_OWNS_MEMORY;
    tdng_set_error(err, TINYDNG_V2_STATUS_UNSUPPORTED, TINYDNG_V2_STAGE_DECODE,
                   ifd_index, TINYDNG_V2_TAG_COMPRESSION, off,
                   "baseline JPEG (not lossless JPEG)");
    // Return OK with the raw data available
    return 1;  // Success, but data is raw JPEG (not decoded)
  }
  if (ret != TDNG_LJ92_ERROR_NONE) {
    tdng_set_error(err, TINYDNG_V2_STATUS_UNSUPPORTED, TINYDNG_V2_STAGE_DECODE,
                   ifd_index, TINYDNG_V2_TAG_COMPRESSION, off,
                   "jpeg stream is not supported lossless ljpeg92 ret=%d", ret);
    return 0;
  }

  if ((w <= 0) || (h <= 0) || (bits <= 0)) {
    tdng_lj92_close(lj);
    tdng_set_error(err, TINYDNG_V2_STATUS_UNSUPPORTED, TINYDNG_V2_STAGE_DECODE,
                   ifd_index, TINYDNG_V2_TAG_COMPRESSION, off,
                   "jpeg stream is not supported lossless ljpeg92");
    return 0;
  }

  if (((uint32_t)w != b->width) || ((uint32_t)h != b->height)) {
    tdng_lj92_close(lj);
    tdng_set_error(err, TINYDNG_V2_STATUS_PARSE_ERROR, TINYDNG_V2_STAGE_DECODE,
                   ifd_index, TINYDNG_V2_TAG_IMAGE_WIDTH, off,
                   "LJPEG dimensions mismatch ljpeg=%dx%d bits=%d ifd=%ux%u",
                   w, h, bits, (unsigned)b->width, (unsigned)b->height);
    return 0;
  }

  if (!tdng_safe_mul_size((size_t)w, (size_t)h, &pixel_count) ||
      !tdng_safe_mul_size(pixel_count, (size_t)b->samples_per_pixel,
                          &pixel_count) ||
      !tdng_safe_mul_size(pixel_count, sizeof(uint16_t), &data_size)) {
    tdng_lj92_close(lj);
    tdng_set_error(err, TINYDNG_V2_STATUS_BOUNDS_ERROR, TINYDNG_V2_STAGE_DECODE,
                   ifd_index, TINYDNG_V2_TAG_COMPRESSION, off,
                   "LJPEG output size overflow");
    return 0;
  }

  dst_bytes = (uint8_t*)tdng_ctx_alloc(ctx, data_size, err);
  if (!dst_bytes) {
    tdng_lj92_close(lj);
    return 0;
  }

  dst_u16 = (uint16_t*)dst_bytes;
  ret = tdng_lj92_decode(lj, dst_u16, (int)((size_t)w * (size_t)b->samples_per_pixel),
                         0, NULL, 0);
  tdng_lj92_close(lj);

  if (ret != TDNG_LJ92_ERROR_NONE) {
    tdng_ctx_free(ctx, dst_bytes);
    tdng_set_error(err, TINYDNG_V2_STATUS_PARSE_ERROR, TINYDNG_V2_STAGE_DECODE,
                   ifd_index, TINYDNG_V2_TAG_COMPRESSION, off,
                   "tdng_lj92_decode failed ret=%d", ret);
    return 0;
  }

  out->data = dst_bytes;
  out->data_size = data_size;
  out->bits_per_sample = 16u;
  out->flags |= TINYDNG_V2_IMAGE_FLAG_DATA_OWNS_MEMORY;
  return 1;
}

static int tdng_parse_image_as_is(tinydng_v2_context* ctx, const tdng_reader* r,
                                  const tdng_ifd_build* b,
                                  tinydng_v2_image* out,
                                  tinydng_v2_error* err,
                                  uint32_t ifd_index) {
  const uint32_t* offsets = NULL;
  const uint32_t* byte_counts = NULL;
  size_t count = 0;
  size_t total_size = 0;
  size_t i;
  tinydng_v2_image_data_segment* segments = NULL;

  if (b->tile_offsets && b->tile_byte_counts && b->tile_count > 0u) {
    offsets = b->tile_offsets;
    byte_counts = b->tile_byte_counts;
    count = b->tile_count;
  } else if (b->strip_offsets && b->strip_byte_counts && b->strip_count > 0u) {
    offsets = b->strip_offsets;
    byte_counts = b->strip_byte_counts;
    count = b->strip_count;
  } else {
    tdng_set_error(err, TINYDNG_V2_STATUS_PARSE_ERROR,
                   TINYDNG_V2_STAGE_PARSE_IFD, ifd_index,
                   TINYDNG_V2_TAG_STRIP_OFFSETS, 0,
                   "image payload offsets/byte counts missing");
    return 0;
  }

  segments = (tinydng_v2_image_data_segment*)tdng_ctx_alloc(
      ctx, sizeof(tinydng_v2_image_data_segment) * count, err);
  if (!segments) {
    return 0;
  }

  for (i = 0; i < count; i++) {
    size_t off = (size_t)offsets[i];
    size_t len = (size_t)byte_counts[i];
    if (len == 0u || off > r->size || len > (r->size - off)) {
      tdng_ctx_free(ctx, segments);
      tdng_set_error(err, TINYDNG_V2_STATUS_BOUNDS_ERROR,
                     TINYDNG_V2_STAGE_PARSE_IFD, ifd_index,
                     TINYDNG_V2_TAG_STRIP_OFFSETS, off,
                     "invalid raw payload segment[%zu] off=%zu len=%zu size=%zu",
                     i, off, len, r->size);
      return 0;
    }
    if (!tdng_safe_add_size(total_size, len, &total_size)) {
      tdng_ctx_free(ctx, segments);
      tdng_set_error(err, TINYDNG_V2_STATUS_BOUNDS_ERROR,
                     TINYDNG_V2_STAGE_PARSE_IFD, ifd_index,
                     TINYDNG_V2_TAG_STRIP_BYTE_COUNTS, off,
                     "raw payload byte count overflow");
      return 0;
    }
    segments[i].offset = (uint64_t)off;
    segments[i].size = len;
  }

  out->data_offset = segments[0].offset;
  out->data_size = total_size;
  out->segment_count = count;
  out->segments = segments;
  out->data = (count == 1u) ? (r->data + (size_t)segments[0].offset) : NULL;
  out->flags |= TINYDNG_V2_IMAGE_FLAG_DATA_IS_FILE_VIEW;
  return 1;
}

static tinydng_v2_status tdng_parse_ifd(
    tinydng_v2_context* ctx, const tdng_reader* r, size_t ifd_off,
    uint32_t ifd_index, tinydng_v2_image* image, uint32_t* next_ifd,
    uint32_t** sub_ifds_out, size_t* sub_ifd_count_out, uint32_t load_flags,
    tinydng_v2_error* err) {
  tdng_ifd_build b;
  uint16_t num_entries;
  size_t entry_base;
  uint32_t i;
  tinydng_v2_status st = TINYDNG_V2_STATUS_OK;

  memset(&b, 0, sizeof(b));
  if (sub_ifds_out) {
    *sub_ifds_out = NULL;
  }
  if (sub_ifd_count_out) {
    *sub_ifd_count_out = 0;
  }
  b.samples_per_pixel = 1u;
  b.bits_per_sample = 8u;
  b.compression = TINYDNG_V2_COMP_NONE;
  b.sample_format = TINYDNG_V2_SAMPLEFORMAT_UINT;

  if (!tdng_read_u16(r, ifd_off, &num_entries)) {
    tdng_set_error(err, TINYDNG_V2_STATUS_BOUNDS_ERROR,
                   TINYDNG_V2_STAGE_PARSE_IFD, ifd_index, 0, ifd_off,
                   "failed to read IFD entry count");
    return TINYDNG_V2_STATUS_BOUNDS_ERROR;
  }

  entry_base = ifd_off + 2u;
  for (i = 0; i < (uint32_t)num_entries; i++) {
    size_t at = entry_base + (size_t)i * 12u;
    uint16_t tag;
    uint16_t type;
    uint32_t count;
    uint32_t value_or_offset;

    if (!tdng_read_u16(r, at + 0u, &tag) || !tdng_read_u16(r, at + 2u, &type) ||
        !tdng_read_u32(r, at + 4u, &count) ||
        !tdng_read_u32(r, at + 8u, &value_or_offset)) {
      tdng_set_error(err, TINYDNG_V2_STATUS_BOUNDS_ERROR,
                     TINYDNG_V2_STAGE_PARSE_IFD, ifd_index, 0, at,
                     "failed to read IFD entry");
      return TINYDNG_V2_STATUS_BOUNDS_ERROR;
    }

    switch (tag) {
      case TINYDNG_V2_TAG_IMAGE_WIDTH:
        if ((type == TINYDNG_V2_TYPE_SHORT) && (count == 1u)) {
          uint16_t v = 0;
          tdng_extract_inline_u16(r, value_or_offset, &v);
          b.width = (uint32_t)v;
          b.has_width = 1;
        } else if ((type == TINYDNG_V2_TYPE_LONG) && (count == 1u)) {
          b.width = value_or_offset;
          b.has_width = 1;
        }
        break;
      case TINYDNG_V2_TAG_IMAGE_LENGTH:
        if ((type == TINYDNG_V2_TYPE_SHORT) && (count == 1u)) {
          uint16_t v = 0;
          tdng_extract_inline_u16(r, value_or_offset, &v);
          b.height = (uint32_t)v;
          b.has_height = 1;
        } else if ((type == TINYDNG_V2_TYPE_LONG) && (count == 1u)) {
          b.height = value_or_offset;
          b.has_height = 1;
        }
        break;
      case TINYDNG_V2_TAG_BITS_PER_SAMPLE:
        if (count == 1u) {
          if (type == TINYDNG_V2_TYPE_SHORT) {
            tdng_extract_inline_u16(r, value_or_offset, &b.bits_per_sample);
            b.has_bps = 1;
          } else if (type == TINYDNG_V2_TYPE_LONG) {
            b.bits_per_sample = (uint16_t)(value_or_offset & 0xFFFFu);
            b.has_bps = 1;
          }
        } else if ((type == TINYDNG_V2_TYPE_SHORT) && (count > 1u)) {
          if (!tdng_read_u16(r, (size_t)value_or_offset, &b.bits_per_sample)) {
            tdng_set_error(err, TINYDNG_V2_STATUS_BOUNDS_ERROR,
                           TINYDNG_V2_STAGE_PARSE_IFD, ifd_index, tag,
                           value_or_offset, "failed to read BitsPerSample array");
            return TINYDNG_V2_STATUS_BOUNDS_ERROR;
          }
          b.has_bps = 1;
        }
        break;
      case TINYDNG_V2_TAG_COMPRESSION:
        if ((type == TINYDNG_V2_TYPE_SHORT) && (count == 1u)) {
          tdng_extract_inline_u16(r, value_or_offset, &b.compression);
          b.has_compression = 1;
        }
        break;
      case TINYDNG_V2_TAG_SAMPLES_PER_PIXEL:
        if ((type == TINYDNG_V2_TYPE_SHORT) && (count == 1u)) {
          tdng_extract_inline_u16(r, value_or_offset, &b.samples_per_pixel);
          b.has_spp = 1;
        }
        break;
      case TINYDNG_V2_TAG_ROWS_PER_STRIP:
        if ((type == TINYDNG_V2_TYPE_SHORT) && (count == 1u)) {
          uint16_t v = 0;
          tdng_extract_inline_u16(r, value_or_offset, &v);
          b.rows_per_strip = v;
        } else if ((type == TINYDNG_V2_TYPE_LONG) && (count == 1u)) {
          b.rows_per_strip = value_or_offset;
        }
        break;
      case TINYDNG_V2_TAG_SUB_IFDS:
        if (!tdng_read_u32_array(ctx, r, type, count, value_or_offset,
                                 &b.sub_ifds, &b.sub_ifd_count, err,
                                 ifd_index, tag)) {
          st = err ? err->code : TINYDNG_V2_STATUS_PARSE_ERROR;
          goto cleanup;
        }
        break;
      case TINYDNG_V2_TAG_TILE_WIDTH:
        if ((type == TINYDNG_V2_TYPE_SHORT) && (count == 1u)) {
          uint16_t v = 0;
          tdng_extract_inline_u16(r, value_or_offset, &v);
          b.tile_width = v;
        } else if ((type == TINYDNG_V2_TYPE_LONG) && (count == 1u)) {
          b.tile_width = value_or_offset;
        }
        break;
      case TINYDNG_V2_TAG_TILE_LENGTH:
        if ((type == TINYDNG_V2_TYPE_SHORT) && (count == 1u)) {
          uint16_t v = 0;
          tdng_extract_inline_u16(r, value_or_offset, &v);
          b.tile_length = v;
        } else if ((type == TINYDNG_V2_TYPE_LONG) && (count == 1u)) {
          b.tile_length = value_or_offset;
        }
        break;
      case TINYDNG_V2_TAG_JPEG_IF_BYTE_COUNT:
        if ((type == TINYDNG_V2_TYPE_LONG) && (count == 1u)) {
          b.jpeg_if_byte_count = value_or_offset;
        }
        break;
      case TINYDNG_V2_TAG_STRIP_OFFSETS:
        if (!tdng_read_u32_array(ctx, r, type, count, value_or_offset,
                                 &b.strip_offsets, &b.strip_count, err,
                                 ifd_index, tag)) {
          st = err ? err->code : TINYDNG_V2_STATUS_PARSE_ERROR;
          goto cleanup;
        }
        break;
      case TINYDNG_V2_TAG_STRIP_BYTE_COUNTS:
        if (!tdng_read_u32_array(ctx, r, type, count, value_or_offset,
                                 &b.strip_byte_counts, &b.strip_byte_count_count, err,
                                 ifd_index, tag)) {
          st = err ? err->code : TINYDNG_V2_STATUS_PARSE_ERROR;
          goto cleanup;
        }
        break;
      case TINYDNG_V2_TAG_TILE_OFFSETS:
        if (!tdng_read_u32_array(ctx, r, type, count, value_or_offset,
                                 &b.tile_offsets, &b.tile_count, err,
                                 ifd_index, tag)) {
          st = err ? err->code : TINYDNG_V2_STATUS_PARSE_ERROR;
          goto cleanup;
        }
        break;
      case TINYDNG_V2_TAG_TILE_BYTE_COUNTS:
        if (!tdng_read_u32_array(ctx, r, type, count, value_or_offset,
                                 &b.tile_byte_counts, &b.tile_byte_count_count, err,
                                 ifd_index, tag)) {
          st = err ? err->code : TINYDNG_V2_STATUS_PARSE_ERROR;
          goto cleanup;
        }
        break;
      case TINYDNG_V2_TAG_SAMPLE_FORMAT:
        if ((type == TINYDNG_V2_TYPE_SHORT) && (count == 1u)) {
          tdng_extract_inline_u16(r, value_or_offset, &b.sample_format);
        }
        break;
      case TINYDNG_V2_TAG_MAKE:
        if (type == 2 && count > 0u) {
          size_t str_off = (count <= 4u) ? value_or_offset : value_or_offset;
          b.exif_make = tdng_read_string(ctx, r, str_off, (size_t)count, err);
          b.has_exif_make = (b.exif_make != NULL) ? 1 : 0;
        }
        break;
      case TINYDNG_V2_TAG_MODEL:
        if (type == 2 && count > 0u) {
          size_t str_off = (count <= 4u) ? value_or_offset : value_or_offset;
          b.exif_model = tdng_read_string(ctx, r, str_off, (size_t)count, err);
          b.has_exif_model = (b.exif_model != NULL) ? 1 : 0;
        }
        break;
      case TINYDNG_V2_TAG_SOFTWARE:
        if (type == 2 && count > 0u) {
          size_t str_off = (count <= 4u) ? value_or_offset : value_or_offset;
          b.exif_software = tdng_read_string(ctx, r, str_off, (size_t)count, err);
          b.has_exif_software = (b.exif_software != NULL) ? 1 : 0;
        }
        break;
      case TINYDNG_V2_TAG_DATETIME:
        if (type == 2 && count > 0u) {
          size_t str_off = (count <= 4u) ? value_or_offset : value_or_offset;
          b.exif_datetime = tdng_read_string(ctx, r, str_off, (size_t)count, err);
          b.has_exif_datetime = (b.exif_datetime != NULL) ? 1 : 0;
        }
        break;
      case TINYDNG_V2_TAG_IMAGEDESCRIPTION:
        if (type == 2 && count > 0u) {
          size_t str_off = (count <= 4u) ? value_or_offset : value_or_offset;
          b.exif_image_description = tdng_read_string(ctx, r, str_off, (size_t)count, err);
          b.has_exif_image_description = (b.exif_image_description != NULL) ? 1 : 0;
        }
        break;
      case TINYDNG_V2_TAG_ORIENTATION:
        if ((type == TINYDNG_V2_TYPE_SHORT) && (count == 1u)) {
          tdng_extract_inline_u16(r, value_or_offset, &b.exif_orientation);
          b.has_exif_orientation = 1;
        }
        break;
      case TINYDNG_V2_TAG_CFA_REPEAT_PATTERN_DIM:
        if ((type == TINYDNG_V2_TYPE_SHORT) && (count == 2u)) {
          uint16_t v0 = 0, v1 = 0;
          tdng_extract_inline_u16(r, value_or_offset, &v0);
          tdng_extract_inline_u16(r, value_or_offset + 2, &v1);
          b.cfa_pattern_dim[0] = v0;
          b.cfa_pattern_dim[1] = v1;
        }
        break;
      case TINYDNG_V2_TAG_CFA_PATTERN:
        if (type == 1 && count > 0u && count <= 16u) {
          size_t off = (count <= 4u) ? value_or_offset : value_or_offset;
          if (off + count <= r->size) {
            b.cfa_pattern_size = (uint8_t)count;
            memcpy(b.cfa_pattern, r->data + off, count);
            b.has_cfa_pattern = 1;
          }
        }
        break;
      case TINYDNG_V2_TAG_CFA_PLANE_COLOR:
        if (type == 1 && count == 4u) {
          size_t off = (count <= 4u) ? value_or_offset : value_or_offset;
          if (off + 4 <= r->size) {
            memcpy(b.cfa_plane_color, r->data + off, 4);
            b.has_cfa_plane_color = 1;
          }
        }
        break;
      case TINYDNG_V2_TAG_CFA_LAYOUT:
        if ((type == TINYDNG_V2_TYPE_SHORT) && (count == 1u)) {
          tdng_extract_inline_u16(r, value_or_offset, &b.cfa_layout);
          b.has_cfa_layout = 1;
        }
        break;
      case TINYDNG_V2_TAG_BLACK_LEVEL:
        if ((type == TINYDNG_V2_TYPE_SHORT || type == TINYDNG_V2_TYPE_LONG) && count > 0u && count <= 4u) {
          if (count == 1) {
            if (type == TINYDNG_V2_TYPE_SHORT) {
              uint16_t v = 0;
              tdng_extract_inline_u16(r, value_or_offset, &v);
              b.black_level[0] = (int32_t)v;
            } else {
              b.black_level[0] = (int32_t)value_or_offset;
            }
          } else {
            size_t i;
            size_t stride = (type == TINYDNG_V2_TYPE_SHORT) ? 2 : 4;
            size_t base_off = (type == TINYDNG_V2_TYPE_SHORT && count <= 2u) ? 0 : value_or_offset;
            for (i = 0; i < count; i++) {
              if (type == TINYDNG_V2_TYPE_SHORT) {
                uint16_t v = 0;
                tdng_extract_inline_u16(r, (uint32_t)(base_off + i * stride), &v);
                b.black_level[i] = (int32_t)v;
              } else {
                uint32_t v = 0;
                tdng_read_u32(r, base_off + i * stride, &v);
                b.black_level[i] = (int32_t)v;
              }
            }
          }
          b.has_black_level = 1;
        }
        break;
      case TINYDNG_V2_TAG_WHITE_LEVEL:
        if ((type == TINYDNG_V2_TYPE_SHORT || type == TINYDNG_V2_TYPE_LONG) && count > 0u && count <= 4u) {
          if (count == 1) {
            if (type == TINYDNG_V2_TYPE_SHORT) {
              uint16_t v = 0;
              tdng_extract_inline_u16(r, value_or_offset, &v);
              b.white_level[0] = (int32_t)v;
            } else {
              b.white_level[0] = (int32_t)value_or_offset;
            }
          } else {
            size_t i;
            size_t stride = (type == TINYDNG_V2_TYPE_SHORT) ? 2 : 4;
            size_t base_off = (type == TINYDNG_V2_TYPE_SHORT && count <= 2u) ? 0 : value_or_offset;
            for (i = 0; i < count; i++) {
              if (type == TINYDNG_V2_TYPE_SHORT) {
                uint16_t v = 0;
                tdng_extract_inline_u16(r, (uint32_t)(base_off + i * stride), &v);
                b.white_level[i] = (int32_t)v;
              } else {
                uint32_t v = 0;
                tdng_read_u32(r, base_off + i * stride, &v);
                b.white_level[i] = (int32_t)v;
              }
            }
          }
          b.has_white_level = 1;
        }
        break;
      case TINYDNG_V2_TAG_COLOR_MATRIX1:
      case TINYDNG_V2_TAG_COLOR_MATRIX2:
      case TINYDNG_V2_TAG_FORWARD_MATRIX1:
      case TINYDNG_V2_TAG_FORWARD_MATRIX2:
        if ((type == 5 || type == 10) && count == 9u) {
          double* mat = (tag == TINYDNG_V2_TAG_COLOR_MATRIX1) ? b.color_matrix1 :
                        (tag == TINYDNG_V2_TAG_COLOR_MATRIX2) ? b.color_matrix2 :
                        (tag == TINYDNG_V2_TAG_FORWARD_MATRIX1) ? b.forward_matrix1 : b.forward_matrix2;
          size_t off = value_or_offset;
          for (size_t j = 0; j < 9; j++) {
            if (type == 5) {
              uint32_t num = 0, den = 0;
              tdng_read_u32(r, off + j * 8, &num);
              tdng_read_u32(r, off + j * 8 + 4, &den);
              mat[j] = (den != 0) ? ((double)num / (double)den) : 0.0;
            } else {
              int32_t num = 0, den = 0;
              tdng_read_i32(r, off + j * 8, &num);
              tdng_read_i32(r, off + j * 8 + 4, &den);
              mat[j] = (den != 0) ? ((double)num / (double)den) : 0.0;
            }
          }
          b.has_color_matrix = 1;
        }
        break;
      case TINYDNG_V2_TAG_DNG_VERSION:
        if (type == 1 && count == 4u) {
          size_t off = (count <= 4u) ? value_or_offset : value_or_offset;
          if (off + 4 <= r->size) {
            memcpy(b.dng_version, r->data + off, 4);
            b.has_dng_version = 1;
          }
        }
        break;
      case TINYDNG_V2_TAG_AS_SHOT_NEUTRAL:
        if ((type == 5 || type == 10) && count == 3u) {
          size_t off = value_or_offset;
          for (size_t j = 0; j < 3; j++) {
            if (type == 5) {
              uint32_t num = 0, den = 0;
              tdng_read_u32(r, off + j * 8, &num);
              tdng_read_u32(r, off + j * 8 + 4, &den);
              b.as_shot_neutral[j] = (den != 0) ? ((double)num / (double)den) : 0.0;
            } else {
              int32_t num = 0, den = 0;
              tdng_read_i32(r, off + j * 8, &num);
              tdng_read_i32(r, off + j * 8 + 4, &den);
              b.as_shot_neutral[j] = (den != 0) ? ((double)num / (double)den) : 0.0;
            }
          }
          b.has_as_shot_neutral = 1;
        }
        break;
      case TINYDNG_V2_TAG_CALIBRATION_ILLUMINANT1:
        if (type == 3 && count == 1u) {
          b.calibration_illuminant1 = (uint16_t)value_or_offset;
          b.has_calibration_illuminant1 = 1;
        }
        break;
      case TINYDNG_V2_TAG_CALIBRATION_ILLUMINANT2:
        if (type == 3 && count == 1u) {
          b.calibration_illuminant2 = (uint16_t)value_or_offset;
          b.has_calibration_illuminant2 = 1;
        }
        break;
      case TINYDNG_V2_TAG_ACTIVE_AREA:
        if ((type == 3 || type == 4) && count == 4u) {
          size_t off = value_or_offset;
          for (size_t j = 0; j < 4; j++) {
            uint32_t val = 0;
            tdng_read_u32(r, off + j * 4, &val);
            b.active_area[j] = val;
          }
          b.has_active_area = 1;
        }
        break;
      case TINYDNG_V2_TAG_DEFAULT_BLACK_RENDER:
        if (type == 3 && count == 1u) {
          b.default_black_render = (uint16_t)value_or_offset;
          b.has_default_black_render = 1;
        }
        break;
      case TINYDNG_V2_TAG_PROFILE_NAME:
        if (type == 2 && count > 0u) {
          size_t str_off = (count <= 4u) ? value_or_offset : value_or_offset;
          b.profile_name = tdng_read_string(ctx, r, str_off, (size_t)count, err);
        }
        break;
      case TINYDNG_V2_TAG_PROFILE_TONE_CURVE:
        if ((type == 5 || type == 12) && count > 0u && count <= 16u) {
          size_t off = value_or_offset;
          uint16_t n = (uint16_t)(count < 16u ? count : 16u);
          for (uint16_t j = 0; j < n; j++) {
            uint32_t num = 0, den = 0;
            tdng_read_u32(r, off + j * 8, &num);
            tdng_read_u32(r, off + j * 8 + 4, &den);
            b.profile_tone_curve[j] = (den != 0) ? ((double)num / (double)den) : 0.0;
          }
          b.profile_tone_curve_count = n;
        }
        break;
      case TINYDNG_V2_TAG_NOISE_PROFILE:
        if (type == 12 && count > 0u && count <= 8u) {
          size_t off = value_or_offset;
          uint16_t n = (uint16_t)(count < 8u ? count : 8u);
          for (uint16_t j = 0; j < n; j++) {
            uint64_t val = 0;
            tdng_read_u64(r, off + j * 8, &val);
            b.noise_profile[j] = *(double*)&val;
          }
          b.noise_profile_count = n;
        }
        break;
      case TINYDNG_V2_TAG_CAMERA_CALIBRATION1:
      case TINYDNG_V2_TAG_CAMERA_CALIBRATION2:
        if ((type == 5 || type == 10) && count == 9u) {
          double* mat = (tag == TINYDNG_V2_TAG_CAMERA_CALIBRATION1) ? b.camera_calibration1 : b.camera_calibration2;
          size_t off = value_or_offset;
          for (size_t j = 0; j < 9; j++) {
            if (type == 5) {
              uint32_t num = 0, den = 0;
              tdng_read_u32(r, off + j * 8, &num);
              tdng_read_u32(r, off + j * 8 + 4, &den);
              mat[j] = (den != 0) ? ((double)num / (double)den) : 0.0;
            } else {
              int32_t num = 0, den = 0;
              tdng_read_i32(r, off + j * 8, &num);
              tdng_read_i32(r, off + j * 8 + 4, &den);
              mat[j] = (den != 0) ? ((double)num / (double)den) : 0.0;
            }
          }
          b.has_camera_calibration = 1;
        }
        break;
      default:
        break;
    }
  }

  if (!tdng_read_u32(r, entry_base + (size_t)num_entries * 12u, next_ifd)) {
    tdng_set_error(err, TINYDNG_V2_STATUS_BOUNDS_ERROR,
                   TINYDNG_V2_STAGE_PARSE_IFD, ifd_index, 0,
                   entry_base + (size_t)num_entries * 12u,
                   "failed to read next IFD offset");
    st = TINYDNG_V2_STATUS_BOUNDS_ERROR;
    goto cleanup;
  }

  // SamplesPerPixel may be missing for some JPEG-based RAW files (e.g., Canon CR2).
  // For JPEG compression (6,7), components will come from the JPEG stream.
  // Also skip IFDs that don't have minimum image tags (e.g., thumbnail IFDs
  // that only have JPEGInterchangeFormat).
  if (!b.has_width || !b.has_height) {
    // IFD doesn't have image dimensions - skip it rather than treating as error.
    // This handles thumbnail IFDs in CR2 and similar formats.
    // Zero the image slot so the caller knows this slot is empty.
    memset(image, 0, sizeof(*image));
    st = TINYDNG_V2_STATUS_OK;
    goto cleanup;
  }

  if (!b.has_bps || !b.has_compression) {
    tdng_set_error(err, TINYDNG_V2_STATUS_PARSE_ERROR, TINYDNG_V2_STAGE_PARSE_IFD,
                   ifd_index, 0, ifd_off,
                   "missing required tags bps=%u compression=%u",
                   b.has_bps, b.has_compression);
    st = TINYDNG_V2_STATUS_PARSE_ERROR;
    goto cleanup;
  }

  // For non-JPEG compression, require samples_per_pixel to be set.
  // For JPEG compression (6=old JPEG, 7=new JPEG), samples_per_pixel may be
  // determined from the JPEG stream, so only check if it was explicitly set.
  if (b.compression != TINYDNG_V2_COMP_NEW_JPEG &&
      b.compression != TINYDNG_V2_COMP_OLD_JPEG) {
    if (!b.has_spp) {
      tdng_set_error(err, TINYDNG_V2_STATUS_PARSE_ERROR, TINYDNG_V2_STAGE_PARSE_IFD,
                     ifd_index, 0, ifd_off,
                     "missing required tag SamplesPerPixel");
      st = TINYDNG_V2_STATUS_PARSE_ERROR;
      goto cleanup;
    }
  }

  if ((b.samples_per_pixel == 0u) || (b.samples_per_pixel > 4u)) {
    tdng_set_error(err, TINYDNG_V2_STATUS_UNSUPPORTED, TINYDNG_V2_STAGE_PARSE_IFD,
                   ifd_index, TINYDNG_V2_TAG_SAMPLES_PER_PIXEL, ifd_off,
                   "samples_per_pixel=%u unsupported", (unsigned)b.samples_per_pixel);
    st = TINYDNG_V2_STATUS_UNSUPPORTED;
    goto cleanup;
  }

  if (b.strip_offsets && b.strip_byte_counts &&
      b.strip_count != b.strip_byte_count_count) {
    tdng_set_error(err, TINYDNG_V2_STATUS_PARSE_ERROR, TINYDNG_V2_STAGE_PARSE_IFD,
                   ifd_index, TINYDNG_V2_TAG_STRIP_BYTE_COUNTS, ifd_off,
                   "StripOffsets/StripByteCounts count mismatch");
    st = TINYDNG_V2_STATUS_PARSE_ERROR;
    goto cleanup;
  }

  if (b.tile_offsets && b.tile_byte_counts &&
      b.tile_count != b.tile_byte_count_count) {
    tdng_set_error(err, TINYDNG_V2_STATUS_PARSE_ERROR, TINYDNG_V2_STAGE_PARSE_IFD,
                   ifd_index, TINYDNG_V2_TAG_TILE_BYTE_COUNTS, ifd_off,
                   "TileOffsets/TileByteCounts count mismatch");
    st = TINYDNG_V2_STATUS_PARSE_ERROR;
    goto cleanup;
  }

  if (!(load_flags & TINYDNG_V2_LOAD_FLAG_PARSE_IMAGE_AS_IS) &&
      (!b.strip_offsets || !b.strip_byte_counts || (b.strip_count == 0u))) {
    tdng_set_error(err, TINYDNG_V2_STATUS_PARSE_ERROR, TINYDNG_V2_STAGE_PARSE_IFD,
                   ifd_index, TINYDNG_V2_TAG_STRIP_OFFSETS, ifd_off,
                   "strip offsets/byte counts missing");
    st = TINYDNG_V2_STATUS_PARSE_ERROR;
    goto cleanup;
  }

  memset(image, 0, sizeof(*image));
  image->width = b.width;
  image->height = b.height;
  image->samples_per_pixel = b.samples_per_pixel;
  image->bits_per_sample_stored = b.bits_per_sample;
  image->bits_per_sample = b.bits_per_sample;
  image->compression = b.compression;
  image->sample_format = b.sample_format;

  if (b.has_exif_make && b.exif_make) {
    image->exif.make = b.exif_make;
    b.exif_make = NULL;
  }
  if (b.has_exif_model && b.exif_model) {
    image->exif.model = b.exif_model;
    b.exif_model = NULL;
  }
  if (b.has_exif_software && b.exif_software) {
    image->exif.software = b.exif_software;
    b.exif_software = NULL;
  }
  if (b.has_exif_datetime && b.exif_datetime) {
    image->exif.datetime = b.exif_datetime;
    b.exif_datetime = NULL;
  }
  if (b.has_exif_image_description && b.exif_image_description) {
    image->exif.image_description = b.exif_image_description;
    b.exif_image_description = NULL;
  }
  if (b.has_exif_orientation) {
    image->exif.orientation = b.exif_orientation;
  }

  if (b.cfa_pattern_dim[0] > 0 && b.cfa_pattern_size > 0 && b.has_cfa_pattern) {
    image->cfa.cfa_pattern_dim[0] = b.cfa_pattern_dim[0];
    image->cfa.cfa_pattern_dim[1] = b.cfa_pattern_dim[1];
    image->cfa.cfa_pattern_size = b.cfa_pattern_size;
    memcpy(image->cfa.cfa_pattern, b.cfa_pattern, b.cfa_pattern_size);
  }
  if (b.has_cfa_plane_color) {
    memcpy(image->cfa.cfa_plane_color, b.cfa_plane_color, 4);
  }
  if (b.has_cfa_layout) {
    image->cfa.cfa_layout = b.cfa_layout;
  }
  if (b.has_black_level) {
    for (int i = 0; i < 4; i++) image->raw_info.black_level[i] = b.black_level[i];
    image->raw_info.black_level_present = 1;
  }
  if (b.has_white_level) {
    for (int i = 0; i < 4; i++) image->raw_info.white_level[i] = b.white_level[i];
    image->raw_info.white_level_present = 1;
  }
  if (b.has_color_matrix) {
    for (int i = 0; i < 9; i++) {
      image->raw_info.color_matrix1[i] = b.color_matrix1[i];
      image->raw_info.color_matrix2[i] = b.color_matrix2[i];
      image->raw_info.forward_matrix1[i] = b.forward_matrix1[i];
      image->raw_info.forward_matrix2[i] = b.forward_matrix2[i];
    }
    image->raw_info.color_matrix_present = 1;
  }
  if (b.has_dng_version) {
    memcpy(image->raw_info.dng_version, b.dng_version, 4);
  }
  if (b.has_as_shot_neutral) {
    for (int i = 0; i < 3; i++) image->raw_info.as_shot_neutral[i] = b.as_shot_neutral[i];
    image->raw_info.has_as_shot_neutral = 1;
  }
  if (b.has_calibration_illuminant1) {
    image->raw_info.calibration_illuminant1 = b.calibration_illuminant1;
  }
  if (b.has_calibration_illuminant2) {
    image->raw_info.calibration_illuminant2 = b.calibration_illuminant2;
  }
  if (b.has_active_area) {
    for (int i = 0; i < 4; i++) image->raw_info.active_area[i] = b.active_area[i];
    image->raw_info.has_active_area = 1;
  }
  if (b.has_default_black_render) {
    image->raw_info.default_black_render = b.default_black_render;
    image->raw_info.has_default_black_render = 1;
  }
  if (b.profile_name) {
    image->raw_info.profile_name = b.profile_name;
  }
  if (b.profile_tone_curve_count > 0) {
    for (uint16_t i = 0; i < b.profile_tone_curve_count; i++) {
      image->raw_info.profile_tone_curve[i] = b.profile_tone_curve[i];
    }
    image->raw_info.profile_tone_curve_count = b.profile_tone_curve_count;
  }
  if (b.noise_profile_count > 0) {
    for (uint16_t i = 0; i < b.noise_profile_count; i++) {
      image->raw_info.noise_profile[i] = b.noise_profile[i];
    }
    image->raw_info.noise_profile_count = b.noise_profile_count;
  }
  if (b.has_camera_calibration) {
    for (int i = 0; i < 9; i++) {
      image->raw_info.camera_calibration1[i] = b.camera_calibration1[i];
      image->raw_info.camera_calibration2[i] = b.camera_calibration2[i];
    }
    image->raw_info.has_camera_calibration = 1;
  }

  if (load_flags & TINYDNG_V2_LOAD_FLAG_PARSE_IMAGE_AS_IS) {
    if (!tdng_parse_image_as_is(ctx, r, &b, image, err, ifd_index)) {
      st = err ? err->code : TINYDNG_V2_STATUS_PARSE_ERROR;
      goto cleanup;
    }
  } else if (b.compression == TINYDNG_V2_COMP_NONE) {
    if (!tdng_decode_strips_uncompressed(ctx, r, &b, image, err, ifd_index)) {
      st = err ? err->code : TINYDNG_V2_STATUS_PARSE_ERROR;
      goto cleanup;
    }
  } else if ((b.compression == TINYDNG_V2_COMP_OLD_JPEG) ||
             (b.compression == TINYDNG_V2_COMP_NEW_JPEG)) {
    if (!tdng_decode_ljpeg(ctx, r, &b, image, err, ifd_index)) {
      st = err ? err->code : TINYDNG_V2_STATUS_PARSE_ERROR;
      goto cleanup;
    }
  } else if ((b.compression == TINYDNG_V2_COMP_LZW) ||
             (b.compression == TINYDNG_V2_COMP_ZIP)) {
    tdng_set_error(err, TINYDNG_V2_STATUS_UNSUPPORTED, TINYDNG_V2_STAGE_DECODE,
                   ifd_index, TINYDNG_V2_TAG_COMPRESSION, ifd_off,
                   "compression=%u not yet implemented in v2", (unsigned)b.compression);
    st = TINYDNG_V2_STATUS_UNSUPPORTED;
    goto cleanup;
  } else {
    tdng_set_error(err, TINYDNG_V2_STATUS_UNSUPPORTED, TINYDNG_V2_STAGE_DECODE,
                   ifd_index, TINYDNG_V2_TAG_COMPRESSION, ifd_off,
                   "compression=%u unsupported", (unsigned)b.compression);
    st = TINYDNG_V2_STATUS_UNSUPPORTED;
    goto cleanup;
  }

cleanup:
  if (st == TINYDNG_V2_STATUS_OK && b.sub_ifds && sub_ifds_out &&
      sub_ifd_count_out) {
    *sub_ifds_out = b.sub_ifds;
    *sub_ifd_count_out = b.sub_ifd_count;
    b.sub_ifds = NULL;
    b.sub_ifd_count = 0;
  }
  if (b.strip_offsets) {
    tdng_ctx_free(ctx, b.strip_offsets);
  }
  if (b.strip_byte_counts) {
    tdng_ctx_free(ctx, b.strip_byte_counts);
  }
  if (b.tile_offsets) {
    tdng_ctx_free(ctx, b.tile_offsets);
  }
  if (b.tile_byte_counts) {
    tdng_ctx_free(ctx, b.tile_byte_counts);
  }
  if (b.sub_ifds) {
    tdng_ctx_free(ctx, b.sub_ifds);
  }
  return st;
}

static tinydng_v2_status tdng_parse_document(tinydng_v2_context* ctx,
                                             const uint8_t* data, size_t size,
                                             uint32_t load_flags,
                                             tinydng_v2_document** out_doc,
                                             tinydng_v2_error* err) {
  tdng_reader r;
  uint16_t version = 0;
  uint32_t first_ifd = 0;
  uint32_t ifd = 0;
  uint32_t ifd_index = 0;
  uint32_t max_images;
  tinydng_v2_document* doc;
  tinydng_v2_image* images;
  uint32_t* ifd_queue;
  size_t qhead = 0;
  size_t qtail = 0;

  if (!ctx || !data || !out_doc) {
    return TINYDNG_V2_STATUS_INVALID_ARGUMENT;
  }

  if (size < 8u) {
    tdng_set_error(err, TINYDNG_V2_STATUS_PARSE_ERROR,
                   TINYDNG_V2_STAGE_PARSE_HEADER, 0, 0, 0,
                   "buffer too small for TIFF header");
    return TINYDNG_V2_STATUS_PARSE_ERROR;
  }

  memset(&r, 0, sizeof(r));
  r.data = data;
  r.size = size;

  if ((data[0] == 'I') && (data[1] == 'I')) {
    r.big_endian = 0;
  } else if ((data[0] == 'M') && (data[1] == 'M')) {
    r.big_endian = 1;
  } else {
    tdng_set_error(err, TINYDNG_V2_STATUS_PARSE_ERROR,
                   TINYDNG_V2_STAGE_PARSE_HEADER, 0, 0, 0,
                   "not TIFF byte order marker");
    return TINYDNG_V2_STATUS_PARSE_ERROR;
  }

  if (!tdng_read_u16(&r, 2u, &version) || (version != 42u)) {
    tdng_set_error(err, TINYDNG_V2_STATUS_PARSE_ERROR,
                   TINYDNG_V2_STAGE_PARSE_HEADER, 0, 0, 2,
                   "invalid TIFF version: %u", (unsigned)version);
    return TINYDNG_V2_STATUS_PARSE_ERROR;
  }

  if (!tdng_read_u32(&r, 4u, &first_ifd)) {
    tdng_set_error(err, TINYDNG_V2_STATUS_BOUNDS_ERROR,
                   TINYDNG_V2_STAGE_PARSE_HEADER, 0, 0, 4,
                   "failed reading first IFD offset");
    return TINYDNG_V2_STATUS_BOUNDS_ERROR;
  }

  max_images = ctx->max_images ? ctx->max_images : 1024u;

  doc = (tinydng_v2_document*)tdng_ctx_alloc(ctx, sizeof(*doc), err);
  if (!doc) {
    return TINYDNG_V2_STATUS_OOM;
  }
  memset(doc, 0, sizeof(*doc));
  doc->mapped_fd = -1;

  images = (tinydng_v2_image*)tdng_ctx_alloc(
      ctx, sizeof(tinydng_v2_image) * (size_t)max_images, err);
  if (!images) {
    tdng_ctx_free(ctx, doc);
    return TINYDNG_V2_STATUS_OOM;
  }
  memset(images, 0, sizeof(tinydng_v2_image) * (size_t)max_images);

  ifd_queue = (uint32_t*)tdng_ctx_alloc(ctx, sizeof(uint32_t) * (size_t)max_images, err);
  if (!ifd_queue) {
    tdng_ctx_free(ctx, images);
    tdng_ctx_free(ctx, doc);
    return TINYDNG_V2_STATUS_OOM;
  }
  if (first_ifd != 0u) {
    ifd_queue[qtail++] = first_ifd;
  }

  while (qhead < qtail) {
    tinydng_v2_status st;
    uint32_t next_ifd = 0;
    uint32_t* sub_ifds = NULL;
    size_t sub_ifd_count = 0;
    size_t k;

    ifd = ifd_queue[qhead++];

    if (ifd_index >= max_images) {
      tdng_set_error(err, TINYDNG_V2_STATUS_UNSUPPORTED,
                     TINYDNG_V2_STAGE_PARSE_IFD, ifd_index, 0, ifd,
                     "too many IFDs (max %u)", (unsigned)max_images);
      while (ifd_index > 0u) {
        ifd_index--;
        tdng_destroy_image_payload(ctx, &images[ifd_index]);
      }
      tdng_ctx_free(ctx, ifd_queue);
      tdng_ctx_free(ctx, images);
      tdng_ctx_free(ctx, doc);
      return TINYDNG_V2_STATUS_UNSUPPORTED;
    }

    st = tdng_parse_ifd(ctx, &r, (size_t)ifd, ifd_index, &images[ifd_index],
                        &next_ifd, &sub_ifds, &sub_ifd_count, load_flags, err);
    if (st != TINYDNG_V2_STATUS_OK) {
      while (ifd_index > 0u) {
        ifd_index--;
        tdng_destroy_image_payload(ctx, &images[ifd_index]);
      }
      if (sub_ifds) {
        tdng_ctx_free(ctx, sub_ifds);
      }
      tdng_ctx_free(ctx, ifd_queue);
      tdng_ctx_free(ctx, images);
      tdng_ctx_free(ctx, doc);
      return st;
    }

    // Copy IFD0 exif data to document global exif (only on first IFD)
    if (ifd_index == 0 && images[0].exif.make) {
      doc->global_exif.make = images[0].exif.make;
      images[0].exif.make = NULL;
    }
    if (ifd_index == 0 && images[0].exif.model) {
      doc->global_exif.model = images[0].exif.model;
      images[0].exif.model = NULL;
    }
    if (ifd_index == 0 && images[0].exif.software) {
      doc->global_exif.software = images[0].exif.software;
      images[0].exif.software = NULL;
    }
    if (ifd_index == 0 && images[0].exif.datetime) {
      doc->global_exif.datetime = images[0].exif.datetime;
      images[0].exif.datetime = NULL;
    }
    if (ifd_index == 0 && images[0].exif.image_description) {
      doc->global_exif.image_description = images[0].exif.image_description;
      images[0].exif.image_description = NULL;
    }
    if (ifd_index == 0 && images[0].exif.orientation) {
      doc->global_exif.orientation = images[0].exif.orientation;
    }

    // Only count this IFD as an image if it has valid dimensions.
    // Skipped IFDs (e.g., thumbnail-only IFDs in CR2) have width/height=0.
    if (images[ifd_index].width != 0u && images[ifd_index].height != 0u) {
      ifd_index++;
    }

    if (load_flags & TINYDNG_V2_LOAD_FLAG_PARSE_IMAGE_AS_IS) {
      for (k = 0; k < sub_ifd_count; k++) {
        if (sub_ifds[k] != 0u && qtail < (size_t)max_images) {
          ifd_queue[qtail++] = sub_ifds[k];
        }
      }
    }
    if (sub_ifds) {
      tdng_ctx_free(ctx, sub_ifds);
    }
    if (next_ifd != 0u && qtail < (size_t)max_images) {
      ifd_queue[qtail++] = next_ifd;
    }
  }

  doc->image_count = (size_t)ifd_index;
  doc->images = images;
  *out_doc = doc;
  tdng_ctx_free(ctx, ifd_queue);

  return TINYDNG_V2_STATUS_OK;
}

static void tdng_destroy_image_payload(tinydng_v2_context* ctx,
                                       tinydng_v2_image* image) {
  if (!ctx || !image) {
    return;
  }
  if ((image->flags & TINYDNG_V2_IMAGE_FLAG_DATA_OWNS_MEMORY) && image->data) {
    tdng_ctx_free(ctx, (void*)(uintptr_t)image->data);
  }
  if (image->segments) {
    tdng_ctx_free(ctx, (void*)(uintptr_t)image->segments);
  }
  if (image->exif.make) { tdng_ctx_free(ctx, image->exif.make); image->exif.make = NULL; }
  if (image->exif.model) { tdng_ctx_free(ctx, image->exif.model); image->exif.model = NULL; }
  if (image->exif.software) { tdng_ctx_free(ctx, image->exif.software); image->exif.software = NULL; }
  if (image->exif.datetime) { tdng_ctx_free(ctx, image->exif.datetime); image->exif.datetime = NULL; }
  if (image->exif.image_description) { tdng_ctx_free(ctx, image->exif.image_description); image->exif.image_description = NULL; }
  image->data = NULL;
  image->segments = NULL;
  image->data_size = 0;
  image->segment_count = 0;
}

tinydng_v2_status tinydng_v2_load_from_memory(tinydng_v2_context* ctx,
                                              const uint8_t* data,
                                              size_t size,
                                              tinydng_v2_document** out_doc,
                                              tinydng_v2_error* err) {
  return tinydng_v2_load_from_memory_with_options(ctx, data, size, NULL,
                                                  out_doc, err);
}

tinydng_v2_status tinydng_v2_load_from_memory_with_options(
    tinydng_v2_context* ctx, const uint8_t* data, size_t size,
    const tinydng_v2_load_options* options, tinydng_v2_document** out_doc,
    tinydng_v2_error* err) {
  if (!ctx || !data || !out_doc) {
    tdng_set_error(err, TINYDNG_V2_STATUS_INVALID_ARGUMENT,
                   TINYDNG_V2_STAGE_NONE, 0, 0, 0,
                   "invalid argument in tinydng_v2_load_from_memory");
    return TINYDNG_V2_STATUS_INVALID_ARGUMENT;
  }

  *out_doc = NULL;
  {
    uint32_t load_flags = options ? options->flags : 0u;
    tinydng_v2_status st = tdng_parse_document(ctx, data, size, load_flags,
                                               out_doc, err);
    if (st == TINYDNG_V2_STATUS_OK &&
        (load_flags & TINYDNG_V2_LOAD_FLAG_PARSE_IMAGE_AS_IS)) {
      (*out_doc)->mapped_data = data;
      (*out_doc)->mapped_size = size;
    }
    return st;
  }
}

tinydng_v2_status tinydng_v2_load_from_file(tinydng_v2_context* ctx,
                                            const char* path,
                                            tinydng_v2_document** out_doc,
                                            tinydng_v2_error* err) {
  return tinydng_v2_load_from_file_with_options(ctx, path, NULL, out_doc, err);
}

tinydng_v2_status tinydng_v2_load_from_file_with_options(
    tinydng_v2_context* ctx, const char* path,
    const tinydng_v2_load_options* options, tinydng_v2_document** out_doc,
    tinydng_v2_error* err) {
  FILE* fp;
  long fsize_long;
  size_t fsize;
  uint8_t* buf;
  size_t nread;
  tinydng_v2_status st;
  uint32_t load_flags = options ? options->flags : 0u;

  if (!ctx || !path || !out_doc) {
    tdng_set_error(err, TINYDNG_V2_STATUS_INVALID_ARGUMENT,
                   TINYDNG_V2_STAGE_IO, 0, 0, 0,
                   "invalid argument in tinydng_v2_load_from_file");
    return TINYDNG_V2_STATUS_INVALID_ARGUMENT;
  }

  *out_doc = NULL;

#if TINYDNG_V2_HAS_MMAP
  if (load_flags & TINYDNG_V2_LOAD_FLAG_USE_MMAP) {
    int fd = -1;
    struct stat stbuf;
    void* map = NULL;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
      tdng_set_error(err, TINYDNG_V2_STATUS_IO_ERROR, TINYDNG_V2_STAGE_IO, 0, 0,
                     0, "open failed for '%s' (errno=%d)", path, errno);
      return TINYDNG_V2_STATUS_IO_ERROR;
    }

    if (fstat(fd, &stbuf) != 0 || stbuf.st_size <= 0) {
      tdng_set_error(err, TINYDNG_V2_STATUS_IO_ERROR, TINYDNG_V2_STAGE_IO, 0, 0,
                     0, "fstat failed for '%s' (errno=%d)", path, errno);
      close(fd);
      return TINYDNG_V2_STATUS_IO_ERROR;
    }

    map = mmap(NULL, (size_t)stbuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
      tdng_set_error(err, TINYDNG_V2_STATUS_IO_ERROR, TINYDNG_V2_STAGE_IO, 0, 0,
                     0, "mmap failed for '%s' (errno=%d)", path, errno);
      close(fd);
      return TINYDNG_V2_STATUS_IO_ERROR;
    }

    st = tdng_parse_document(ctx, (const uint8_t*)map, (size_t)stbuf.st_size,
                             load_flags, out_doc, err);
    if (st != TINYDNG_V2_STATUS_OK) {
      munmap(map, (size_t)stbuf.st_size);
      close(fd);
      return st;
    }

    (*out_doc)->mapped_data = (const uint8_t*)map;
    (*out_doc)->mapped_size = (size_t)stbuf.st_size;
    (*out_doc)->mapped_fd = fd;
    (*out_doc)->owns_mmap = 1;
    return TINYDNG_V2_STATUS_OK;
  }
#endif

  fp = fopen(path, "rb");
  if (!fp) {
    tdng_set_error(err, TINYDNG_V2_STATUS_IO_ERROR, TINYDNG_V2_STAGE_IO, 0, 0,
                   0, "fopen failed for '%s' (errno=%d)", path, errno);
    return TINYDNG_V2_STATUS_IO_ERROR;
  }

  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    tdng_set_error(err, TINYDNG_V2_STATUS_IO_ERROR, TINYDNG_V2_STAGE_IO, 0, 0,
                   0, "fseek(SEEK_END) failed");
    return TINYDNG_V2_STATUS_IO_ERROR;
  }

  fsize_long = ftell(fp);
  if (fsize_long < 0) {
    fclose(fp);
    tdng_set_error(err, TINYDNG_V2_STATUS_IO_ERROR, TINYDNG_V2_STAGE_IO, 0, 0,
                   0, "ftell failed");
    return TINYDNG_V2_STATUS_IO_ERROR;
  }

  if (fseek(fp, 0, SEEK_SET) != 0) {
    fclose(fp);
    tdng_set_error(err, TINYDNG_V2_STATUS_IO_ERROR, TINYDNG_V2_STAGE_IO, 0, 0,
                   0, "fseek(SEEK_SET) failed");
    return TINYDNG_V2_STATUS_IO_ERROR;
  }

  fsize = (size_t)fsize_long;
  buf = (uint8_t*)tdng_ctx_alloc(ctx, fsize, err);
  if (!buf) {
    fclose(fp);
    return TINYDNG_V2_STATUS_OOM;
  }

  nread = fread(buf, 1, fsize, fp);
  fclose(fp);
  if (nread != fsize) {
    tdng_set_error(err, TINYDNG_V2_STATUS_IO_ERROR, TINYDNG_V2_STAGE_IO, 0, 0,
                   0, "fread failed expected=%zu read=%zu", fsize, nread);
    tdng_ctx_free(ctx, buf);
    return TINYDNG_V2_STATUS_IO_ERROR;
  }

  st = tinydng_v2_load_from_memory_with_options(ctx, buf, fsize, options,
                                                out_doc, err);
  if (st != TINYDNG_V2_STATUS_OK) {
    tdng_ctx_free(ctx, buf);
    return st;
  }

  if (load_flags & TINYDNG_V2_LOAD_FLAG_PARSE_IMAGE_AS_IS) {
    (*out_doc)->mapped_data = buf;
    (*out_doc)->mapped_size = fsize;
    (*out_doc)->mapped_fd = -1;
    (*out_doc)->owns_file_buffer = 1;
  } else {
    tdng_ctx_free(ctx, buf);
  }
  return TINYDNG_V2_STATUS_OK;
}

void tinydng_v2_document_destroy(tinydng_v2_context* ctx, tinydng_v2_document* doc) {
  size_t i;
  if (!ctx || !doc) {
    return;
  }
  if (doc->images) {
    for (i = 0; i < doc->image_count; i++) {
      tdng_destroy_image_payload(ctx, &doc->images[i]);
    }
    tdng_ctx_free(ctx, doc->images);
  }
  if (doc->global_exif.make) { tdng_ctx_free(ctx, doc->global_exif.make); }
  if (doc->global_exif.model) { tdng_ctx_free(ctx, doc->global_exif.model); }
  if (doc->global_exif.software) { tdng_ctx_free(ctx, doc->global_exif.software); }
  if (doc->global_exif.datetime) { tdng_ctx_free(ctx, doc->global_exif.datetime); }
  if (doc->global_exif.image_description) { tdng_ctx_free(ctx, doc->global_exif.image_description); }
#if TINYDNG_V2_HAS_MMAP
  if (doc->owns_mmap && doc->mapped_data && doc->mapped_size > 0u) {
    munmap((void*)(uintptr_t)doc->mapped_data, doc->mapped_size);
  }
  if (doc->owns_mmap && doc->mapped_fd >= 0) {
    close(doc->mapped_fd);
  }
#endif
  if (doc->owns_file_buffer && doc->mapped_data) {
    tdng_ctx_free(ctx, (void*)(uintptr_t)doc->mapped_data);
  }
  tdng_ctx_free(ctx, doc);
}

size_t tinydng_v2_document_image_count(const tinydng_v2_document* doc) {
  return doc ? doc->image_count : 0u;
}

const tinydng_v2_image* tinydng_v2_document_image_at(const tinydng_v2_document* doc,
                                                     size_t index) {
  if (!doc || (index >= doc->image_count)) {
    return NULL;
  }
  return &doc->images[index];
}

const uint8_t* tinydng_v2_document_memory(const tinydng_v2_document* doc,
                                          size_t* size) {
  if (!doc || !doc->mapped_data) {
    if (size) {
      *size = 0;
    }
    return NULL;
  }
  if (size) {
    *size = doc->mapped_size;
  }
  return doc->mapped_data;
}

const tinydng_v2_basic_exif* tinydng_v2_document_global_exif(const tinydng_v2_document* doc) {
  return doc ? &doc->global_exif : NULL;
}

void tinydng_v2_exif_init(tinydng_v2_basic_exif* exif) {
  if (!exif) return;
  exif->make = NULL;
  exif->model = NULL;
  exif->software = NULL;
  exif->datetime = NULL;
  exif->image_description = NULL;
  exif->orientation = 0;
}

void tinydng_v2_exif_destroy(tinydng_v2_context* ctx, tinydng_v2_basic_exif* exif) {
  if (!ctx || !exif) return;
  if (exif->make) { tdng_ctx_free(ctx, exif->make); exif->make = NULL; }
  if (exif->model) { tdng_ctx_free(ctx, exif->model); exif->model = NULL; }
  if (exif->software) { tdng_ctx_free(ctx, exif->software); exif->software = NULL; }
  if (exif->datetime) { tdng_ctx_free(ctx, exif->datetime); exif->datetime = NULL; }
  if (exif->image_description) { tdng_ctx_free(ctx, exif->image_description); exif->image_description = NULL; }
}

const char* tinydng_v2_exif_make(const tinydng_v2_basic_exif* exif) {
  return exif ? exif->make : NULL;
}

const char* tinydng_v2_exif_model(const tinydng_v2_basic_exif* exif) {
  return exif ? exif->model : NULL;
}

const char* tinydng_v2_exif_software(const tinydng_v2_basic_exif* exif) {
  return exif ? exif->software : NULL;
}

const char* tinydng_v2_exif_datetime(const tinydng_v2_basic_exif* exif) {
  return exif ? exif->datetime : NULL;
}

const char* tinydng_v2_exif_image_description(const tinydng_v2_basic_exif* exif) {
  return exif ? exif->image_description : NULL;
}

uint16_t tinydng_v2_exif_orientation(const tinydng_v2_basic_exif* exif) {
  return exif ? exif->orientation : 0;
}

const tinydng_v2_cfa_pattern* tinydng_v2_image_cfa(const tinydng_v2_image* img) {
  if (!img) return NULL;
  if (img->cfa.cfa_pattern_dim[0] == 0 && img->cfa.cfa_pattern_dim[1] == 0) {
    return NULL;
  }
  return &img->cfa;
}

const tinydng_v2_raw_info* tinydng_v2_image_raw_info(const tinydng_v2_image* img) {
  return img ? &img->raw_info : NULL;
}

const char* tinydng_v2_image_profile_name(const tinydng_v2_image* img) {
  if (!img) return NULL;
  return img->raw_info.profile_name;
}

static void tdng_write_u16(FILE* fp, uint16_t v, int big_endian) {
  uint8_t b[2];
  if (big_endian) {
    b[0] = (uint8_t)((v >> 8) & 0xFFu);
    b[1] = (uint8_t)(v & 0xFFu);
  } else {
    b[0] = (uint8_t)(v & 0xFFu);
    b[1] = (uint8_t)((v >> 8) & 0xFFu);
  }
  (void)fwrite(b, 1, 2, fp);
}

static void tdng_write_u32(FILE* fp, uint32_t v, int big_endian) {
  uint8_t b[4];
  if (big_endian) {
    b[0] = (uint8_t)((v >> 24) & 0xFFu);
    b[1] = (uint8_t)((v >> 16) & 0xFFu);
    b[2] = (uint8_t)((v >> 8) & 0xFFu);
    b[3] = (uint8_t)(v & 0xFFu);
  } else {
    b[0] = (uint8_t)(v & 0xFFu);
    b[1] = (uint8_t)((v >> 8) & 0xFFu);
    b[2] = (uint8_t)((v >> 16) & 0xFFu);
    b[3] = (uint8_t)((v >> 24) & 0xFFu);
  }
  (void)fwrite(b, 1, 4, fp);
}

static void tdng_write_ifd_entry(FILE* fp, int be, uint16_t tag, uint16_t type,
                                 uint32_t count, uint32_t value_or_offset) {
  tdng_write_u16(fp, tag, be);
  tdng_write_u16(fp, type, be);
  tdng_write_u32(fp, count, be);
  tdng_write_u32(fp, value_or_offset, be);
}

tinydng_v2_status tinydng_v2_write_file(tinydng_v2_context* ctx,
                                        const char* path,
                                        const tinydng_v2_image* image,
                                        const tinydng_v2_write_options* options,
                                        tinydng_v2_error* err) {
  FILE* fp;
  const uint8_t* payload = NULL;
  size_t payload_size = 0;
  uint16_t compression = TINYDNG_V2_COMP_NONE;
  uint32_t strip_offset;
  int be = 0;
  uint32_t rows_per_strip;

  if (!ctx || !path || !image) {
    tdng_set_error(err, TINYDNG_V2_STATUS_INVALID_ARGUMENT,
                   TINYDNG_V2_STAGE_WRITE, 0, 0, 0,
                   "invalid write arguments");
    return TINYDNG_V2_STATUS_INVALID_ARGUMENT;
  }

  if (options) {
    be = options->big_endian ? 1 : 0;
    compression = options->compression;
  }

  if (compression == 0u) {
    compression = image->compression ? image->compression : TINYDNG_V2_COMP_NONE;
  }

  rows_per_strip = options && options->rows_per_strip ? options->rows_per_strip : image->height;

  if ((image->width == 0u) || (image->height == 0u) ||
      (image->samples_per_pixel == 0u) || !image->data) {
    tdng_set_error(err, TINYDNG_V2_STATUS_INVALID_ARGUMENT,
                   TINYDNG_V2_STAGE_WRITE, 0, 0, 0,
                   "invalid image fields");
    return TINYDNG_V2_STATUS_INVALID_ARGUMENT;
  }

  if (compression == TINYDNG_V2_COMP_NONE) {
    payload = image->data;
    payload_size = image->data_size;
  } else {
    tdng_set_error(err, TINYDNG_V2_STATUS_UNSUPPORTED, TINYDNG_V2_STAGE_WRITE, 0,
                   TINYDNG_V2_TAG_COMPRESSION, 0,
                   "compression=%u unsupported for writer", (unsigned)compression);
    return TINYDNG_V2_STATUS_UNSUPPORTED;
  }

  fp = fopen(path, "wb");
  if (!fp) {
    tdng_set_error(err, TINYDNG_V2_STATUS_IO_ERROR, TINYDNG_V2_STAGE_IO, 0, 0,
                   0, "fopen failed for '%s' errno=%d", path, errno);
    return TINYDNG_V2_STATUS_IO_ERROR;
  }

  strip_offset = 8u + 2u + 9u * 12u + 4u;

  if (be) {
    (void)fputc('M', fp);
    (void)fputc('M', fp);
  } else {
    (void)fputc('I', fp);
    (void)fputc('I', fp);
  }
  tdng_write_u16(fp, 42u, be);
  tdng_write_u32(fp, 8u, be);

  tdng_write_u16(fp, 9u, be);
  tdng_write_ifd_entry(fp, be, TINYDNG_V2_TAG_IMAGE_WIDTH, TINYDNG_V2_TYPE_LONG,
                       1u, image->width);
  tdng_write_ifd_entry(fp, be, TINYDNG_V2_TAG_IMAGE_LENGTH, TINYDNG_V2_TYPE_LONG,
                       1u, image->height);
  tdng_write_ifd_entry(fp, be, TINYDNG_V2_TAG_BITS_PER_SAMPLE, TINYDNG_V2_TYPE_SHORT,
                       1u, (uint32_t)image->bits_per_sample);
  tdng_write_ifd_entry(fp, be, TINYDNG_V2_TAG_COMPRESSION, TINYDNG_V2_TYPE_SHORT,
                       1u, (uint32_t)compression);
  tdng_write_ifd_entry(fp, be, TINYDNG_V2_TAG_STRIP_OFFSETS, TINYDNG_V2_TYPE_LONG,
                       1u, strip_offset);
  tdng_write_ifd_entry(fp, be, TINYDNG_V2_TAG_SAMPLES_PER_PIXEL,
                       TINYDNG_V2_TYPE_SHORT, 1u,
                       (uint32_t)image->samples_per_pixel);
  tdng_write_ifd_entry(fp, be, TINYDNG_V2_TAG_ROWS_PER_STRIP,
                       TINYDNG_V2_TYPE_LONG, 1u, rows_per_strip);
  tdng_write_ifd_entry(fp, be, TINYDNG_V2_TAG_STRIP_BYTE_COUNTS,
                       TINYDNG_V2_TYPE_LONG, 1u, (uint32_t)payload_size);
  tdng_write_ifd_entry(fp, be, TINYDNG_V2_TAG_SAMPLE_FORMAT,
                       TINYDNG_V2_TYPE_SHORT, 1u,
                       (uint32_t)(image->sample_format ? image->sample_format
                                                       : TINYDNG_V2_SAMPLEFORMAT_UINT));
  tdng_write_u32(fp, 0u, be);

  if (fwrite(payload, 1, payload_size, fp) != payload_size) {
    fclose(fp);
    tdng_set_error(err, TINYDNG_V2_STATUS_IO_ERROR, TINYDNG_V2_STAGE_WRITE, 0,
                   TINYDNG_V2_TAG_STRIP_BYTE_COUNTS, strip_offset,
                   "failed writing payload bytes");
    return TINYDNG_V2_STATUS_IO_ERROR;
  }

  fclose(fp);

  return TINYDNG_V2_STATUS_OK;
}
