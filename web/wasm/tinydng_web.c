/*
 * TinyDNG browser ABI.
 *
 * This is deliberately a small C wrapper around the v3 API. It owns the
 * input bytes for the lifetime of a document because the memory IO backend
 * is a view, and it decodes directly into caller-provided WASM memory. The
 * open call takes ownership of the input pointer supplied by JavaScript.
 */
#include "tinydng.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct td_web_document td_web_document;

typedef struct td_web_image_info {
  uint32_t width;
  uint32_t height;
  uint32_t image_size;
  uint32_t pixel_stride;
  uint32_t segment_count;
  uint32_t samples_per_pixel;
  uint32_t bits_per_sample;
  uint32_t bits_per_sample_decoded;
  uint32_t sample_format;
  uint32_t photometric;
  uint32_t compression;
  uint32_t orientation;
  uint32_t flags; /* bit 0 CFA, bit 1 black, bit 2 white, bit 3 matrix,
                     bit 4 as-shot neutral */
  int32_t black_level;
  uint32_t white_level;
  char make[128];
  char model[128];
  char software[128];
  char datetime[32];
  float color_matrix1[9];
  float as_shot_neutral[3];
  uint32_t cfa_pattern_rows;
  uint32_t cfa_pattern_cols;
  uint32_t cfa_pattern_size;
  uint32_t cfa_pattern[16];
  uint32_t plane_color[4];
} td_web_image_info;

_Static_assert(sizeof(td_web_image_info) == 616u,
               "browser image info ABI size changed");

struct td_web_document {
  tinydng_context *ctx;
  tinydng_document *doc;
  uint8_t *input;
  size_t input_size;
  tinydng_error error;
};

static void td_web_error(td_web_document *web, tinydng_status status,
                         const char *message) {
  if (!web) return;
  tinydng_error_clear(&web->error);
  web->error.status = status;
  web->error.stage = TINYDNG_STAGE_NONE;
  strncpy(web->error.message, message, sizeof(web->error.message) - 1u);
  web->error.message[sizeof(web->error.message) - 1u] = '\0';
}

static void td_web_copy_string(char *dst, size_t cap, const char *src) {
  if (!dst || cap == 0u) return;
  if (!src) src = "";
  strncpy(dst, src, cap - 1u);
  dst[cap - 1u] = '\0';
}

static int td_web_sample_bytes(uint16_t bits, uint16_t sample_format) {
  if (sample_format == TINYDNG_SAMPLEFORMAT_IEEEFP) return 4;
  if (bits <= 8u) return 1;
  if (bits <= 16u) return 2;
  return 4;
}

static int td_web_fill_info(td_web_document *web, uint32_t image_index,
                            td_web_image_info *out) {
  const tinydng_image_info *img;
  size_t pixel_stride;
  size_t image_size;
  int sample_bytes;
  uint32_t flags = 0;

  if (!web || !web->doc || !out) {
    if (web) td_web_error(web, TINYDNG_E_INVALID_ARG, "invalid image info request");
    return TINYDNG_E_INVALID_ARG;
  }
  img = tinydng_image_get(web->doc, (size_t)image_index);
  if (!img) {
    td_web_error(web, TINYDNG_E_INVALID_ARG, "image index out of range");
    return TINYDNG_E_INVALID_ARG;
  }
  memset(out, 0, sizeof(*out));

  sample_bytes = td_web_sample_bytes(img->bits_per_sample_decoded,
                                     img->sample_format);
  if (sample_bytes <= 0 ||
      (size_t)img->samples_per_pixel > SIZE_MAX / (size_t)sample_bytes ||
      (size_t)img->width > SIZE_MAX /
          ((size_t)img->samples_per_pixel * (size_t)sample_bytes) ||
      (size_t)img->height > SIZE_MAX /
          ((size_t)img->width * (size_t)img->samples_per_pixel *
           (size_t)sample_bytes)) {
    td_web_error(web, TINYDNG_E_BOUNDS, "decoded image size overflow");
    return TINYDNG_E_BOUNDS;
  }
  pixel_stride = (size_t)img->samples_per_pixel * (size_t)sample_bytes;
  image_size = (size_t)img->width * (size_t)img->height * pixel_stride;
  if (pixel_stride > UINT32_MAX || image_size > UINT32_MAX ||
      img->segment_count > UINT32_MAX) {
    td_web_error(web, TINYDNG_E_UNSUPPORTED,
                 "decoded image is too large for the browser ABI");
    return TINYDNG_E_UNSUPPORTED;
  }

  if (img->cfa.present) flags |= 1u;
  if (img->raw.black_level_present) flags |= 2u;
  if (img->raw.white_level_present) flags |= 4u;
  if (img->raw.color_matrix_present) flags |= 8u;
  if (img->raw.has_as_shot_neutral) flags |= 16u;

  out->width = img->width;
  out->height = img->height;
  out->image_size = (uint32_t)image_size;
  out->pixel_stride = (uint32_t)pixel_stride;
  out->segment_count = (uint32_t)img->segment_count;
  out->samples_per_pixel = img->samples_per_pixel;
  out->bits_per_sample = img->bits_per_sample;
  out->bits_per_sample_decoded = img->bits_per_sample_decoded;
  out->sample_format = img->sample_format;
  /* Photometric is an internal parser field and is not part of the public
     tinydng_image_info ABI. Keep the wrapper slot reserved for compatibility. */
  out->photometric = 0u;
  out->compression = img->compression;
  out->orientation = img->exif.orientation;
  out->flags = flags;
  out->black_level = img->raw.black_level[0];
  out->white_level = (uint32_t)img->raw.white_level[0];
  td_web_copy_string(out->make, sizeof(out->make), img->exif.make);
  td_web_copy_string(out->model, sizeof(out->model), img->exif.model);
  td_web_copy_string(out->software, sizeof(out->software), img->exif.software);
  td_web_copy_string(out->datetime, sizeof(out->datetime), img->exif.datetime);
  {
    size_t i;
    for (i = 0; i < 9u; i++) {
      out->color_matrix1[i] = (float)img->raw.color_matrix1[i];
    }
    for (i = 0; i < 3u; i++) {
      out->as_shot_neutral[i] = (float)img->raw.as_shot_neutral[i];
    }
    out->cfa_pattern_rows = img->cfa.pattern_dim[0];
    out->cfa_pattern_cols = img->cfa.pattern_dim[1];
    out->cfa_pattern_size = img->cfa.pattern_size;
    for (i = 0; i < 16u; i++) {
      out->cfa_pattern[i] = img->cfa.pattern[i];
    }
    for (i = 0; i < 4u; i++) {
      /* DNG's usual three-plane CFA is RGB even when CFAPlaneColor is
         omitted. Fill missing entries with that canonical ordering. */
      out->plane_color[i] = (i < img->cfa.plane_color_count)
                                ? img->cfa.plane_color[i]
                                : (i < 3u ? (uint32_t)i : 0u);
    }
  }
  tinydng_error_clear(&web->error);
  return TINYDNG_OK;
}

uint32_t tinydng_web_info_size(void) {
  return (uint32_t)sizeof(td_web_image_info);
}

td_web_document *tinydng_web_open(const uint8_t *data, uint32_t size) {
  td_web_document *web;
  tinydng_open_options options;
  tinydng_config config;
  tinydng_status status;

  web = (td_web_document *)calloc(1, sizeof(*web));
  if (!web) return NULL;
  if (!data || size == 0u) {
    td_web_error(web, TINYDNG_E_INVALID_ARG, "empty input");
    return web;
  }
  web->input = (uint8_t *)(uintptr_t)data;
  web->input_size = (size_t)size;

  memset(&config, 0, sizeof(config));
  web->ctx = tinydng_context_create(&config, &web->error);
  if (!web->ctx) return web;
  memset(&options, 0, sizeof(options));
  options.flags = TINYDNG_OPEN_PARSE_SUBIFDS;
  status = tinydng_open_memory(web->ctx, web->input, web->input_size,
                               &options, &web->doc, &web->error);
  if (status != TINYDNG_OK) {
    web->doc = NULL;
  }
  return web;
}

void tinydng_web_close(td_web_document *web) {
  if (!web) return;
  if (web->doc) tinydng_document_destroy(web->ctx, web->doc);
  if (web->ctx) tinydng_context_destroy(web->ctx);
  free(web->input);
  free(web);
}

int tinydng_web_is_valid(const td_web_document *web) {
  return web && web->ctx && web->doc;
}

const char *tinydng_web_error(const td_web_document *web) {
  if (!web) return "invalid document handle";
  return web->error.message[0] ? web->error.message : "";
}

int tinydng_web_error_status(const td_web_document *web) {
  return web ? (int)web->error.status : TINYDNG_E_INVALID_ARG;
}

uint32_t tinydng_web_image_count(const td_web_document *web) {
  size_t count;
  if (!tinydng_web_is_valid(web)) return 0u;
  count = tinydng_image_count(web->doc);
  return count > UINT32_MAX ? UINT32_MAX : (uint32_t)count;
}

int tinydng_web_get_image_info(td_web_document *web, uint32_t image_index,
                               td_web_image_info *out) {
  return td_web_fill_info(web, image_index, out);
}

int tinydng_web_decode(td_web_document *web, uint32_t image_index, void *dst,
                       uint32_t capacity) {
  tinydng_decode_options options;
  tinydng_pixels pixels;
  tinydng_status status;
  if (!web || !web->doc || !dst) {
    if (web) td_web_error(web, TINYDNG_E_INVALID_ARG, "invalid decode request");
    return TINYDNG_E_INVALID_ARG;
  }
  memset(&options, 0, sizeof(options));
  options.dst = dst;
  options.dst_capacity = capacity;
  options.num_threads = 1u;
  status = tinydng_decode_image(web->ctx, web->doc, (size_t)image_index,
                                &options, &pixels, &web->error);
  return (int)status;
}

int tinydng_web_decode_region(td_web_document *web, uint32_t image_index,
                              uint32_t x, uint32_t y, uint32_t width,
                              uint32_t height, void *dst, uint32_t capacity) {
  tinydng_decode_options options;
  tinydng_pixels pixels;
  tinydng_status status;
  if (!web || !web->doc || !dst || width == 0u || height == 0u) {
    if (web) td_web_error(web, TINYDNG_E_INVALID_ARG,
                          "invalid region decode request");
    return TINYDNG_E_INVALID_ARG;
  }
  memset(&options, 0, sizeof(options));
  options.dst = dst;
  options.dst_capacity = capacity;
  options.num_threads = 1u;
  status = tinydng_decode_region(web->ctx, web->doc, (size_t)image_index, x, y,
                                 width, height, &options, &pixels,
                                 &web->error);
  return (int)status;
}
