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

  if (!b.has_width || !b.has_height || !b.has_spp || !b.has_bps || !b.has_compression) {
    tdng_set_error(err, TINYDNG_V2_STATUS_PARSE_ERROR, TINYDNG_V2_STAGE_PARSE_IFD,
                   ifd_index, 0, ifd_off,
                   "missing required tags width=%u height=%u spp=%u bps=%u compression=%u",
                   b.has_width, b.has_height, b.has_spp, b.has_bps,
                   b.has_compression);
    st = TINYDNG_V2_STATUS_PARSE_ERROR;
    goto cleanup;
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

    ifd_index++;

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
