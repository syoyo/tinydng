/*
 * tinydng_api.c - allocator, safe math, error, context/document lifecycle.
 * SPDX-License-Identifier: MIT
 */
#include "td_internal.h"

#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Error                                                              */
/* ------------------------------------------------------------------ */

void td_set_error(tinydng_error *err, tinydng_status code, tinydng_stage stage,
                  uint32_t ifd_index, uint16_t tag, uint64_t offset,
                  const char *fmt, ...) {
  va_list args;
  if (!err) {
    return;
  }
  err->status = code;
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

void tinydng_error_clear(tinydng_error *err) {
  if (err) {
    memset(err, 0, sizeof(*err));
  }
}

const char *tinydng_status_string(tinydng_status status) {
  switch (status) {
    case TINYDNG_OK:
      return "ok";
    case TINYDNG_E_INVALID_ARG:
      return "invalid argument";
    case TINYDNG_E_PARSE:
      return "parse error";
    case TINYDNG_E_UNSUPPORTED:
      return "unsupported";
    case TINYDNG_E_IO:
      return "io error";
    case TINYDNG_E_OOM:
      return "out of memory";
    case TINYDNG_E_BOUNDS:
      return "bounds error";
    case TINYDNG_E_DECODE:
      return "decode error";
    case TINYDNG_E_INTERNAL:
      return "internal error";
    default:
      return "unknown";
  }
}

const char *tinydng_stage_string(tinydng_stage stage) {
  switch (stage) {
    case TINYDNG_STAGE_NONE:
      return "none";
    case TINYDNG_STAGE_ALLOC:
      return "alloc";
    case TINYDNG_STAGE_IO:
      return "io";
    case TINYDNG_STAGE_HEADER:
      return "header";
    case TINYDNG_STAGE_IFD:
      return "ifd";
    case TINYDNG_STAGE_GEOMETRY:
      return "geometry";
    case TINYDNG_STAGE_METADATA:
      return "metadata";
    case TINYDNG_STAGE_DECODE:
      return "decode";
    case TINYDNG_STAGE_WRITE:
      return "write";
    default:
      return "unknown";
  }
}

/* ------------------------------------------------------------------ */
/* Safe arithmetic                                                    */
/* ------------------------------------------------------------------ */

int td_safe_add_size(size_t a, size_t b, size_t *out) {
  if (SIZE_MAX - a < b) {
    return 0;
  }
  *out = a + b;
  return 1;
}

int td_safe_mul_size(size_t a, size_t b, size_t *out) {
  if ((a != 0u) && (b > (SIZE_MAX / a))) {
    return 0;
  }
  *out = a * b;
  return 1;
}

int td_safe_add_u64(uint64_t a, uint64_t b, uint64_t *out) {
  if (UINT64_MAX - a < b) {
    return 0;
  }
  *out = a + b;
  return 1;
}

int td_safe_mul_u64(uint64_t a, uint64_t b, uint64_t *out) {
  if ((a != 0u) && (b > (UINT64_MAX / a))) {
    return 0;
  }
  *out = a * b;
  return 1;
}

/* ------------------------------------------------------------------ */
/* Allocator                                                          */
/* ------------------------------------------------------------------ */

static void *td_default_malloc(void *user_data, size_t size) {
  (void)user_data;
  return malloc(size);
}

static void td_default_free(void *user_data, void *ptr) {
  (void)user_data;
  free(ptr);
}

void *td_ctx_alloc(tinydng_context *ctx, size_t size, tinydng_error *err) {
  td_alloc_header *h;
  size_t total = 0;
  void *raw;

  if (!ctx) {
    return NULL;
  }
  /* Disallow zero-size allocations returning a usable header-only block. */
  if (size == 0u) {
    size = 1u;
  }
  if (!td_safe_add_size(sizeof(td_alloc_header), size, &total)) {
    td_set_error(err, TINYDNG_E_OOM, TINYDNG_STAGE_ALLOC, 0, 0, 0,
                 "allocation size overflow (%zu + %zu)",
                 sizeof(td_alloc_header), size);
    ctx->alloc_failed = 1;
    return NULL;
  }
  if ((ctx->memory_cap_bytes > 0u) &&
      (size > (ctx->memory_cap_bytes - ctx->memory_used))) {
    td_set_error(err, TINYDNG_E_OOM, TINYDNG_STAGE_ALLOC, 0, 0, 0,
                 "memory cap exceeded: requested=%zu used=%zu cap=%zu", size,
                 ctx->memory_used, ctx->memory_cap_bytes);
    ctx->alloc_failed = 1;
    return NULL;
  }

  raw = ctx->allocator.alloc(ctx->allocator.user_data, total);
  if (!raw) {
    td_set_error(err, TINYDNG_E_OOM, TINYDNG_STAGE_ALLOC, 0, 0, 0,
                 "allocator returned null for %zu bytes", total);
    ctx->alloc_failed = 1;
    return NULL;
  }

  h = (td_alloc_header *)raw;
  h->magic = TINYDNG_ALLOC_MAGIC;
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
  return (void *)(h + 1);
}

void *td_ctx_calloc(tinydng_context *ctx, size_t size, tinydng_error *err) {
  void *p = td_ctx_alloc(ctx, size, err);
  if (p) {
    memset(p, 0, size == 0u ? 1u : size);
  }
  return p;
}

void td_ctx_free(tinydng_context *ctx, void *ptr) {
  td_alloc_header *h;
  if (!ctx || !ptr) {
    return;
  }
  h = ((td_alloc_header *)ptr) - 1;
  if (h->magic != TINYDNG_ALLOC_MAGIC) {
    return; /* not ours / double free guard */
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
  ctx->allocator.free(ctx->allocator.user_data, h);
}

void td_ctx_free_all(tinydng_context *ctx) {
  td_alloc_header *h, *next;
  if (!ctx) {
    return;
  }
  h = ctx->alloc_head;
  while (h) {
    next = h->next;
    h->magic = 0;
    ctx->allocator.free(ctx->allocator.user_data, h);
    h = next;
  }
  ctx->alloc_head = NULL;
  ctx->memory_used = 0;
}

void *td_ctx_realloc(tinydng_context *ctx, void *ptr, size_t old_size,
                     size_t new_size, tinydng_error *err) {
  void *np;
  if (!ctx) {
    return NULL;
  }
  np = td_ctx_alloc(ctx, new_size, err);
  if (!np) {
    return NULL;
  }
  if (ptr && old_size > 0u) {
    size_t copy = old_size < new_size ? old_size : new_size;
    memcpy(np, ptr, copy);
    td_ctx_free(ctx, ptr);
  }
  return np;
}

/* ------------------------------------------------------------------ */
/* Context                                                            */
/* ------------------------------------------------------------------ */

tinydng_context *tinydng_context_create(const tinydng_config *config,
                                        tinydng_error *err) {
  tinydng_context tmp;
  tinydng_context *ctx;

  tinydng_error_clear(err);
  memset(&tmp, 0, sizeof(tmp));

  if (config && config->allocator.alloc && config->allocator.free) {
    tmp.allocator = config->allocator;
  } else {
    tmp.allocator.alloc = td_default_malloc;
    tmp.allocator.free = td_default_free;
    tmp.allocator.user_data = NULL;
  }
  tmp.memory_cap_bytes = (config && config->memory_cap_bytes > 0u)
                             ? config->memory_cap_bytes
                             : (size_t)TINYDNG_DEFAULT_MEMORY_CAP_BYTES;
  tmp.max_images =
      (config && config->max_images > 0u) ? config->max_images : 1024u;
  tmp.max_image_pixels = (config && config->max_image_pixels > 0u)
                             ? config->max_image_pixels
                             : (uint64_t)1u << 30;
  tmp.max_ifd_depth =
      (config && config->max_ifd_depth > 0u) ? config->max_ifd_depth : 8u;
  tmp.max_ifd_entries =
      (config && config->max_ifd_entries > 0u) ? config->max_ifd_entries : 4096u;

  ctx = (tinydng_context *)tmp.allocator.alloc(tmp.allocator.user_data,
                                               sizeof(*ctx));
  if (!ctx) {
    td_set_error(err, TINYDNG_E_OOM, TINYDNG_STAGE_ALLOC, 0, 0, 0,
                 "failed to allocate context");
    return NULL;
  }
  *ctx = tmp;
  return ctx;
}

void tinydng_context_destroy(tinydng_context *ctx) {
  tinydng_allocator alloc;
  if (!ctx) {
    return;
  }
  alloc = ctx->allocator;
  td_ctx_free_all(ctx);
  alloc.free(alloc.user_data, ctx);
}

size_t tinydng_context_memory_used(const tinydng_context *ctx) {
  return ctx ? ctx->memory_used : 0u;
}

size_t tinydng_context_memory_peak(const tinydng_context *ctx) {
  return ctx ? ctx->memory_peak : 0u;
}

/* ------------------------------------------------------------------ */
/* Document lifecycle helpers                                         */
/* ------------------------------------------------------------------ */

static void td_free_exif(tinydng_context *ctx, tinydng_exif *e) {
  td_ctx_free(ctx, e->make);
  td_ctx_free(ctx, e->model);
  td_ctx_free(ctx, e->software);
  td_ctx_free(ctx, e->datetime);
  td_ctx_free(ctx, e->image_description);
  memset(e, 0, sizeof(*e));
}

static void td_free_image_payload(tinydng_context *ctx,
                                  tinydng_image_info *img) {
  size_t i;
  td_free_exif(ctx, &img->exif);
  td_ctx_free(ctx, img->raw.profile_name);
  td_ctx_free(ctx, img->raw.semantic_name);
  td_ctx_free(ctx, img->raw.linearization_table);
  if (img->raw.gainmaps) {
    for (i = 0; i < img->raw.gainmap_count; i++) {
      td_ctx_free(ctx, img->raw.gainmaps[i].pixels);
    }
    td_ctx_free(ctx, img->raw.gainmaps);
  }
  if (img->raw.opcodes) {
    for (i = 0; i < img->raw.opcode_count; i++) {
      td_ctx_free(ctx, (void *)(uintptr_t)img->raw.opcodes[i].params);
    }
    td_ctx_free(ctx, img->raw.opcodes);
  }
  td_ctx_free(ctx, img->raw.warps);
  td_ctx_free(ctx, img->raw.vignettes);
  /* segments table is allocated as a single block (const cast for free) */
  td_ctx_free(ctx, (void *)(uintptr_t)img->segments);
  if (img->custom_fields) {
    for (i = 0; i < img->custom_field_count; i++) {
      td_ctx_free(ctx, img->custom_fields[i].data);
    }
    td_ctx_free(ctx, img->custom_fields);
  }
  memset(img, 0, sizeof(*img));
}

void tinydng_document_destroy(tinydng_context *ctx, tinydng_document *doc) {
  size_t i;
  if (!ctx || !doc) {
    return;
  }
  if (doc->images) {
    for (i = 0; i < doc->image_count; i++) {
      td_free_image_payload(ctx, &doc->images[i]);
    }
    td_ctx_free(ctx, doc->images);
  }
  td_free_exif(ctx, &doc->global_exif);
  if (doc->has_io && doc->io.close) {
    doc->io.close(&doc->io);
  }
  td_ctx_free(ctx, doc);
}

/* ------------------------------------------------------------------ */
/* Accessors                                                          */
/* ------------------------------------------------------------------ */

size_t tinydng_image_count(const tinydng_document *doc) {
  return doc ? doc->image_count : 0u;
}

const tinydng_image_info *tinydng_image_get(const tinydng_document *doc,
                                            size_t index) {
  if (!doc || index >= doc->image_count) {
    return NULL;
  }
  return &doc->images[index];
}

const tinydng_exif *tinydng_document_exif(const tinydng_document *doc) {
  if (!doc || doc->image_count == 0u) {
    return NULL;
  }
  return &doc->images[0].exif; /* IFD0 EXIF */
}

size_t tinydng_image_segment_count(const tinydng_image_info *img) {
  return img ? img->segment_count : 0u;
}

tinydng_status tinydng_image_segment(const tinydng_image_info *img, size_t i,
                                     tinydng_segment *out) {
  if (!img || !out || i >= img->segment_count) {
    return TINYDNG_E_INVALID_ARG;
  }
  *out = img->segments[i];
  return TINYDNG_OK;
}

void tinydng_pixels_free(tinydng_context *ctx, tinydng_pixels *px) {
  if (!ctx || !px) {
    return;
  }
  if (px->owns_memory && px->data) {
    td_ctx_free(ctx, px->data);
  }
  memset(px, 0, sizeof(*px));
}

/* ------------------------------------------------------------------ */
/* open_memory / open_file (thin wrappers around open_io)             */
/* ------------------------------------------------------------------ */

tinydng_status tinydng_open_memory(tinydng_context *ctx, const uint8_t *data,
                                   size_t n, const tinydng_open_options *opts,
                                   tinydng_document **out, tinydng_error *err) {
  tinydng_io io;
  tinydng_status st;
  if (!ctx || !data || !out) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_NONE, 0, 0, 0,
                 "null argument to tinydng_open_memory");
    return TINYDNG_E_INVALID_ARG;
  }
  st = tinydng_io_open_memory(ctx, data, n, &io, err);
  if (st != TINYDNG_OK) {
    return st;
  }
  return tinydng_open_io(ctx, io, opts, out, err);
}

tinydng_status tinydng_open_file(tinydng_context *ctx, const char *path,
                                 const tinydng_open_options *opts,
                                 tinydng_document **out, tinydng_error *err) {
  tinydng_io io;
  tinydng_status st;
  uint32_t flags = opts ? opts->flags : 0u;
  if (!ctx || !path || !out) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_NONE, 0, 0, 0,
                 "null argument to tinydng_open_file");
    return TINYDNG_E_INVALID_ARG;
  }
  if (flags & TINYDNG_OPEN_PREFER_MMAP) {
    st = tinydng_io_open_mmap(ctx, path, &io, err);
    if (st == TINYDNG_OK) {
      return tinydng_open_io(ctx, io, opts, out, err);
    }
    /* fall through to stdio on mmap failure */
  }
  st = tinydng_io_open_stdio(ctx, path, &io, err);
  if (st != TINYDNG_OK) {
    return st;
  }
  return tinydng_open_io(ctx, io, opts, out, err);
}
