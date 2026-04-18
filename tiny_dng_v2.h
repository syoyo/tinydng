#ifndef TINY_DNG_V2_H_
#define TINY_DNG_V2_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum tinydng_v2_status {
  TINYDNG_V2_STATUS_OK = 0,
  TINYDNG_V2_STATUS_INVALID_ARGUMENT = 1,
  TINYDNG_V2_STATUS_PARSE_ERROR = 2,
  TINYDNG_V2_STATUS_UNSUPPORTED = 3,
  TINYDNG_V2_STATUS_IO_ERROR = 4,
  TINYDNG_V2_STATUS_OOM = 5,
  TINYDNG_V2_STATUS_BOUNDS_ERROR = 6,
  TINYDNG_V2_STATUS_INTERNAL_ERROR = 7
} tinydng_v2_status;

typedef enum tinydng_v2_error_stage {
  TINYDNG_V2_STAGE_NONE = 0,
  TINYDNG_V2_STAGE_ALLOCATOR = 1,
  TINYDNG_V2_STAGE_IO = 2,
  TINYDNG_V2_STAGE_PARSE_HEADER = 3,
  TINYDNG_V2_STAGE_PARSE_IFD = 4,
  TINYDNG_V2_STAGE_DECODE = 5,
  TINYDNG_V2_STAGE_ENCODE = 6,
  TINYDNG_V2_STAGE_WRITE = 7
} tinydng_v2_error_stage;

typedef struct tinydng_v2_error {
  tinydng_v2_status code;
  tinydng_v2_error_stage stage;
  uint32_t ifd_index;
  uint16_t tag;
  uint64_t offset;
  char message[256];
} tinydng_v2_error;

typedef void* (*tinydng_v2_malloc_fn)(void* user_data, size_t size);
typedef void (*tinydng_v2_free_fn)(void* user_data, void* ptr);

typedef struct tinydng_v2_allocator {
  tinydng_v2_malloc_fn malloc_fn;
  tinydng_v2_free_fn free_fn;
  void* user_data;
} tinydng_v2_allocator;

typedef struct tinydng_v2_config {
  tinydng_v2_allocator allocator;
  size_t memory_cap_bytes;
  uint32_t max_images;
} tinydng_v2_config;

typedef struct tinydng_v2_context tinydng_v2_context;
typedef struct tinydng_v2_document tinydng_v2_document;

typedef enum tinydng_v2_load_flags {
  TINYDNG_V2_LOAD_FLAG_NONE = 0,
  TINYDNG_V2_LOAD_FLAG_PARSE_IMAGE_AS_IS = 1u << 0,
  TINYDNG_V2_LOAD_FLAG_USE_MMAP = 1u << 1
} tinydng_v2_load_flags;

typedef struct tinydng_v2_load_options {
  uint32_t flags;
} tinydng_v2_load_options;

typedef struct tinydng_v2_image_data_segment {
  uint64_t offset;
  size_t size;
} tinydng_v2_image_data_segment;

typedef enum tinydng_v2_image_flags {
  TINYDNG_V2_IMAGE_FLAG_NONE = 0,
  TINYDNG_V2_IMAGE_FLAG_DATA_OWNS_MEMORY = 1u << 0,
  TINYDNG_V2_IMAGE_FLAG_DATA_IS_FILE_VIEW = 1u << 1
} tinydng_v2_image_flags;

typedef struct tinydng_v2_image {
  uint32_t width;
  uint32_t height;
  uint16_t samples_per_pixel;
  uint16_t bits_per_sample;
  uint16_t bits_per_sample_stored;
  uint16_t compression;
  uint16_t sample_format;
  size_t data_size;
  const uint8_t* data;
  uint64_t data_offset;
  size_t segment_count;
  const tinydng_v2_image_data_segment* segments;
  uint32_t flags;
} tinydng_v2_image;

typedef struct tinydng_v2_write_options {
  uint8_t big_endian;
  uint16_t compression;
  uint32_t rows_per_strip;
} tinydng_v2_write_options;

#define TINYDNG_V2_DEFAULT_MEMORY_CAP_BYTES (512u * 1024u * 1024u)

tinydng_v2_context* tinydng_v2_context_create(const tinydng_v2_config* config,
                                              tinydng_v2_error* err);
void tinydng_v2_context_destroy(tinydng_v2_context* ctx);

size_t tinydng_v2_context_memory_used(const tinydng_v2_context* ctx);
size_t tinydng_v2_context_memory_peak(const tinydng_v2_context* ctx);

void tinydng_v2_error_clear(tinydng_v2_error* err);
const char* tinydng_v2_status_string(tinydng_v2_status status);

tinydng_v2_status tinydng_v2_load_from_memory(tinydng_v2_context* ctx,
                                              const uint8_t* data,
                                              size_t size,
                                              tinydng_v2_document** out_doc,
                                              tinydng_v2_error* err);

tinydng_v2_status tinydng_v2_load_from_memory_with_options(
    tinydng_v2_context* ctx, const uint8_t* data, size_t size,
    const tinydng_v2_load_options* options, tinydng_v2_document** out_doc,
    tinydng_v2_error* err);

tinydng_v2_status tinydng_v2_load_from_file(tinydng_v2_context* ctx,
                                            const char* path,
                                            tinydng_v2_document** out_doc,
                                            tinydng_v2_error* err);

tinydng_v2_status tinydng_v2_load_from_file_with_options(
    tinydng_v2_context* ctx, const char* path,
    const tinydng_v2_load_options* options, tinydng_v2_document** out_doc,
    tinydng_v2_error* err);

void tinydng_v2_document_destroy(tinydng_v2_context* ctx, tinydng_v2_document* doc);
size_t tinydng_v2_document_image_count(const tinydng_v2_document* doc);
const tinydng_v2_image* tinydng_v2_document_image_at(const tinydng_v2_document* doc,
                                                     size_t index);
const uint8_t* tinydng_v2_document_memory(const tinydng_v2_document* doc,
                                          size_t* size);

tinydng_v2_status tinydng_v2_write_file(tinydng_v2_context* ctx,
                                        const char* path,
                                        const tinydng_v2_image* image,
                                        const tinydng_v2_write_options* options,
                                        tinydng_v2_error* err);

#ifdef __cplusplus
}
#endif

#endif
