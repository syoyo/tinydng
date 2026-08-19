/*
 * tinydng_api.c - allocator, safe math, error, context/document lifecycle,
 * threading primitives.
 * SPDX-License-Identifier: MIT
 */
/* Feature-test macros must precede system includes so sysconf() is visible
   under a strict -std=c11 compile. */
#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#endif

#include "td_internal.h"

#include <stdio.h>
#include <stdlib.h>

#if defined(TINYDNG_ENABLE_THREADS)
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif
#endif

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
/* Quick format check                                                  */
/* ------------------------------------------------------------------ */

static int td_is_tiff_header(const uint8_t *p, size_t n) {
  if (n < 4) {
    return 0;
  }
  /* II = little-endian, MM = big-endian. */
  if (p[0] == 'I' && p[1] == 'I') {
    uint16_t ver = (uint16_t)p[2] | ((uint16_t)p[3] << 8);
    return ver == 0x002a || ver == 0x002b; /* TIFF 6.0 or BigTIFF */
  }
  if (p[0] == 'M' && p[1] == 'M') {
    uint16_t ver = ((uint16_t)p[2] << 8) | (uint16_t)p[3];
    return ver == 0x002a || ver == 0x002b;
  }
  return 0;
}

static int td_is_psd_header(const uint8_t *p, size_t n) {
  if (n < 4) {
    return 0;
  }
  return p[0] == '8' && p[1] == 'B' && p[2] == 'P' && p[3] == 'S';
}

tinydng_status tinydng_is_dng_memory(const void *data, size_t size,
                                     tinydng_error *err) {
  const uint8_t *p;
  tinydng_error_clear(err);
  if (!data || size < 4) {
    if (err) {
      td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_NONE, 0, 0, 0,
                   "is_dng: data is NULL or too small (%zu bytes)", size);
    }
    return TINYDNG_E_INVALID_ARG;
  }
  p = (const uint8_t *)data;
  if (td_is_tiff_header(p, size) || td_is_psd_header(p, size)) {
    return TINYDNG_OK;
  }
  if (err) {
    td_set_error(err, TINYDNG_E_PARSE, TINYDNG_STAGE_HEADER, 0, 0, 0,
                 "is_dng: not a TIFF/DNG/PSD header");
  }
  return TINYDNG_E_PARSE;
}

tinydng_status tinydng_is_dng(const char *path, tinydng_error *err) {
  uint8_t hdr[4];
  FILE *f;
  size_t n;
  tinydng_error_clear(err);
  if (!path) {
    if (err) {
      td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_NONE, 0, 0, 0,
                   "is_dng: path is NULL");
    }
    return TINYDNG_E_INVALID_ARG;
  }
  f = fopen(path, "rb");
  if (!f) {
    if (err) {
      td_set_error(err, TINYDNG_E_IO, TINYDNG_STAGE_IO, 0, 0, 0,
                   "is_dng: cannot open '%s'", path);
    }
    return TINYDNG_E_IO;
  }
  n = fread(hdr, 1, 4, f);
  fclose(f);
  return tinydng_is_dng_memory(hdr, n, err);
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
/* Threading (all platform code is confined here)                     */
/* ------------------------------------------------------------------ */

struct td_mutex {
#if defined(TINYDNG_ENABLE_THREADS)
#if defined(_WIN32)
  CRITICAL_SECTION cs;
#else
  pthread_mutex_t m;
#endif
#else
  int dummy;
#endif
};

td_mutex *td_mutex_create(tinydng_context *ctx) {
#if defined(TINYDNG_ENABLE_THREADS)
  td_mutex *m;
  if (!ctx) {
    return NULL;
  }
  m = (td_mutex *)ctx->allocator.alloc(ctx->allocator.user_data, sizeof(*m));
  if (!m) {
    return NULL;
  }
#if defined(_WIN32)
  InitializeCriticalSection(&m->cs);
#else
  if (pthread_mutex_init(&m->m, NULL) != 0) {
    ctx->allocator.free(ctx->allocator.user_data, m);
    return NULL;
  }
#endif
  return m;
#else
  (void)ctx;
  return NULL;
#endif
}

/* Recursive variant: required so a decode that internally triggers another
   decode on the same context (e.g. a PSD smart-object) does not deadlock the
   inter-decode guard. CRITICAL_SECTION is already recursive on Windows. */
td_mutex *td_mutex_create_recursive(tinydng_context *ctx) {
#if defined(TINYDNG_ENABLE_THREADS) && !defined(_WIN32)
  td_mutex *m;
  pthread_mutexattr_t attr;
  if (!ctx) {
    return NULL;
  }
  if (pthread_mutexattr_init(&attr) != 0) {
    return NULL;
  }
  if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) != 0) {
    pthread_mutexattr_destroy(&attr);
    return NULL;
  }
  m = (td_mutex *)ctx->allocator.alloc(ctx->allocator.user_data, sizeof(*m));
  if (!m) {
    pthread_mutexattr_destroy(&attr);
    return NULL;
  }
  if (pthread_mutex_init(&m->m, &attr) != 0) {
    ctx->allocator.free(ctx->allocator.user_data, m);
    pthread_mutexattr_destroy(&attr);
    return NULL;
  }
  pthread_mutexattr_destroy(&attr);
  return m;
#else
  /* Windows CRITICAL_SECTION is recursive; threads-disabled returns NULL. */
  return td_mutex_create(ctx);
#endif
}

void td_mutex_destroy(tinydng_context *ctx, td_mutex *m) {
  if (!ctx || !m) {
    return;
  }
#if defined(TINYDNG_ENABLE_THREADS)
#if defined(_WIN32)
  DeleteCriticalSection(&m->cs);
#else
  pthread_mutex_destroy(&m->m);
#endif
  ctx->allocator.free(ctx->allocator.user_data, m);
#endif
}

void td_mutex_lock(td_mutex *m) {
#if defined(TINYDNG_ENABLE_THREADS)
  if (!m) {
    return;
  }
#if defined(_WIN32)
  EnterCriticalSection(&m->cs);
#else
  pthread_mutex_lock(&m->m);
#endif
#else
  (void)m;
#endif
}

void td_mutex_unlock(td_mutex *m) {
#if defined(TINYDNG_ENABLE_THREADS)
  if (!m) {
    return;
  }
#if defined(_WIN32)
  LeaveCriticalSection(&m->cs);
#else
  pthread_mutex_unlock(&m->m);
#endif
#else
  (void)m;
#endif
}

int td_threads_available(void) {
#if defined(TINYDNG_ENABLE_THREADS)
  return 1;
#else
  return 0;
#endif
}

unsigned td_cpu_count(void) {
#if defined(TINYDNG_ENABLE_THREADS) && defined(_WIN32)
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return si.dwNumberOfProcessors ? (unsigned)si.dwNumberOfProcessors : 1u;
#elif defined(TINYDNG_ENABLE_THREADS) && defined(_SC_NPROCESSORS_ONLN)
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? (unsigned)n : 1u;
#else
  return 1u;
#endif
}

#if defined(TINYDNG_ENABLE_THREADS)
typedef struct {
  td_thread_fn fn;
  void *arg;
} td_thread_ctx;
#if defined(_WIN32)
static DWORD WINAPI td_thread_trampoline(LPVOID p) {
  td_thread_ctx *tc = (td_thread_ctx *)p;
  tc->fn(tc->arg);
  return 0u;
}
#else
static void *td_thread_trampoline(void *p) {
  td_thread_ctx *tc = (td_thread_ctx *)p;
  return tc->fn(tc->arg);
}
#endif
#endif /* TINYDNG_ENABLE_THREADS */

int td_threads_run(td_thread_fn fn, void *args, size_t arg_stride, unsigned n) {
  unsigned i;
  if (n == 0u || !fn) {
    return 0;
  }
  if (n > TD_MAX_DECODE_THREADS) {
    n = TD_MAX_DECODE_THREADS;
  }
  if (n == 1u || !td_threads_available()) {
    for (i = 0u; i < n; i++) {
      fn((char *)args + (size_t)i * arg_stride);
    }
    return 0;
  }
#if defined(TINYDNG_ENABLE_THREADS)
  {
    td_thread_ctx tc[TD_MAX_DECODE_THREADS];
    int created[TD_MAX_DECODE_THREADS];
#if defined(_WIN32)
    HANDLE th[TD_MAX_DECODE_THREADS];
#else
    pthread_t th[TD_MAX_DECODE_THREADS];
#endif
    for (i = 1u; i < n; i++) {
      tc[i].fn = fn;
      tc[i].arg = (char *)args + (size_t)i * arg_stride;
#if defined(_WIN32)
      th[i] = CreateThread(NULL, 0, td_thread_trampoline, &tc[i], 0, NULL);
      created[i] = (th[i] != NULL);
#else
      created[i] =
          (pthread_create(&th[i], NULL, td_thread_trampoline, &tc[i]) == 0);
#endif
    }
    /* This thread runs task 0 and helps drain the shared queue. */
    fn((char *)args);
    for (i = 1u; i < n; i++) {
      if (created[i]) {
#if defined(_WIN32)
        WaitForSingleObject(th[i], INFINITE);
        CloseHandle(th[i]);
#else
        pthread_join(th[i], NULL);
#endif
      } else {
        /* Spawn failed: run inline. The queue is typically already drained,
           so this returns immediately; correctness holds regardless. */
        fn((char *)args + (size_t)i * arg_stride);
      }
    }
  }
#endif
  return 0;
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
  /* Serialize only while a multi-threaded decode is in flight; the single-
     threaded parse/decode paths take no lock (L stays NULL -> no-op). */
  td_mutex *L;

  if (!ctx) {
    return NULL;
  }
  L = ctx->mt_active ? ctx->lock : NULL;
  /* Disallow zero-size allocations returning a usable header-only block. */
  if (size == 0u) {
    size = 1u;
  }
  if (!td_safe_add_size(sizeof(td_alloc_header), size, &total)) {
    td_set_error(err, TINYDNG_E_OOM, TINYDNG_STAGE_ALLOC, 0, 0, 0,
                 "allocation size overflow (%zu + %zu)",
                 sizeof(td_alloc_header), size);
    td_mutex_lock(L);
    ctx->alloc_failed = 1;
    td_mutex_unlock(L);
    return NULL;
  }

  td_mutex_lock(L);
  if ((ctx->memory_cap_bytes > 0u) &&
      (size > (ctx->memory_cap_bytes - ctx->memory_used))) {
    td_set_error(err, TINYDNG_E_OOM, TINYDNG_STAGE_ALLOC, 0, 0, 0,
                 "memory cap exceeded: requested=%zu used=%zu cap=%zu", size,
                 ctx->memory_used, ctx->memory_cap_bytes);
    ctx->alloc_failed = 1;
    td_mutex_unlock(L);
    return NULL;
  }

  raw = ctx->allocator.alloc(ctx->allocator.user_data, total);
  if (!raw) {
    td_set_error(err, TINYDNG_E_OOM, TINYDNG_STAGE_ALLOC, 0, 0, 0,
                 "allocator returned null for %zu bytes", total);
    ctx->alloc_failed = 1;
    td_mutex_unlock(L);
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
  td_mutex_unlock(L);
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
  td_mutex *L;
  if (!ctx || !ptr) {
    return;
  }
  L = ctx->mt_active ? ctx->lock : NULL;
  h = ((td_alloc_header *)ptr) - 1;
  td_mutex_lock(L);
  if (h->magic != TINYDNG_ALLOC_MAGIC) {
    td_mutex_unlock(L);
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
  td_mutex_unlock(L);
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
  /* To avoid a transient 2× memory spike (old + new both live), temporarily
     discount the old allocation from memory_used before the new allocation,
     then let td_ctx_free re-subtract it after the copy.  This keeps the
     allocator's accounting accurate while allowing grows that would
     otherwise fail the cap check. */
  td_alloc_header *h = NULL;
  td_mutex *L;
  void *np;
  size_t copy;
  if (!ctx) {
    return NULL;
  }
  L = ctx->mt_active ? ctx->lock : NULL;
  /* Validate old pointer. */
  if (ptr && old_size > 0u) {
    h = ((td_alloc_header *)ptr) - 1;
    td_mutex_lock(L);
    if (h->magic != TINYDNG_ALLOC_MAGIC || h->size != old_size) {
      td_mutex_unlock(L);
      td_set_error(err, TINYDNG_E_INTERNAL, TINYDNG_STAGE_ALLOC, 0, 0, 0,
                   "td_ctx_realloc: invalid old block");
      return NULL;
    }
    /* Temporarily discount old allocation from accounting. */
    if (ctx->memory_used >= h->size) {
      ctx->memory_used -= h->size;
    } else {
      ctx->memory_used = 0;
    }
    td_mutex_unlock(L);
  }
  np = td_ctx_alloc(ctx, new_size, err);
  if (!np) {
    /* Restore accounting if we discounted above. */
    if (h) {
      td_mutex_lock(L);
      ctx->memory_used += old_size;
      td_mutex_unlock(L);
    }
    return NULL;
  }
  if (h) {
    copy = old_size < new_size ? old_size : new_size;
    memcpy(np, ptr, copy);
    /* Unlink and free the old header.  Its size was already discounted
       before allocating the replacement, so do not subtract it again. */
    td_mutex_lock(L);
    if (h->prev) {
      h->prev->next = h->next;
    } else {
      ctx->alloc_head = h->next;
    }
    if (h->next) {
      h->next->prev = h->prev;
    }
    h->magic = 0;
    ctx->allocator.free(ctx->allocator.user_data, h);
    td_mutex_unlock(L);
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
  tmp.max_psd_layers =
      (config && config->max_psd_layers > 0u) ? config->max_psd_layers : 4096u;
  tmp.max_psd_resources = (config && config->max_psd_resources > 0u)
                              ? config->max_psd_resources
                              : 2048u;
  tmp.max_psd_segments = (config && config->max_psd_segments > 0u)
                             ? config->max_psd_segments
                             : (1u << 20);
  tmp.max_embed_depth =
      (config && config->max_embed_depth > 0u) ? config->max_embed_depth : 4u;

  ctx = (tinydng_context *)tmp.allocator.alloc(tmp.allocator.user_data,
                                               sizeof(*ctx));
  if (!ctx) {
    td_set_error(err, TINYDNG_E_OOM, TINYDNG_STAGE_ALLOC, 0, 0, 0,
                 "failed to allocate context");
    return NULL;
  }
  *ctx = tmp;
  /* Lock for multi-threaded decode; NULL when threads are disabled (decode
     then stays serial). Allocation failure here is non-fatal for the same
     reason -- MT simply won't engage. */
  ctx->lock = td_mutex_create(ctx);
  ctx->decode_guard = td_mutex_create_recursive(ctx);
   ctx->mt_active = 0;
   return ctx;
}

void tinydng_context_destroy(tinydng_context *ctx) {
  tinydng_allocator alloc;
  if (!ctx) {
    return;
  }
  alloc = ctx->allocator;
  td_mutex_destroy(ctx, ctx->lock);
  td_mutex_destroy(ctx, ctx->decode_guard);
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

void td_free_image_payload(tinydng_context *ctx, tinydng_image_info *img) {
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
#ifndef TINYDNG_NO_PSD
  if (doc->psd) {
    td_psd_free_info(ctx, doc->psd);
    doc->psd = NULL;
  }
#endif
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

const tinydng_psd_info *tinydng_document_psd(const tinydng_document *doc) {
#ifndef TINYDNG_NO_PSD
  return doc ? doc->psd : NULL;
#else
  (void)doc;
  return NULL;
#endif
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
