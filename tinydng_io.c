/*
 * tinydng_io.c - IO backends (memory / mmap / stdio) + td_io_view.
 * SPDX-License-Identifier: MIT
 */
/* Feature-test macros must precede all system includes so that POSIX
   fseeko/mmap/fstat are visible even under a strict -std=c11 compile. */
#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#endif

#include "td_internal.h"

#include <limits.h>
#include <stdio.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define TINYDNG_HAS_MMAP 1
#elif defined(__unix__) || defined(__APPLE__) || defined(__linux__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#define TINYDNG_HAS_MMAP 1
#else
#define TINYDNG_HAS_MMAP 0
#endif

/* ------------------------------------------------------------------ */
/* td_io_view                                                         */
/* ------------------------------------------------------------------ */

const uint8_t *td_io_view(tinydng_io *io, uint64_t io_size, uint64_t off,
                          size_t len, uint8_t *scratch, size_t scratch_cap) {
  if (!io) {
    return NULL;
  }
  if (len == 0u) {
    return scratch; /* zero-length window: nothing readable, valid */
  }
  /* Bounds check without computing off+len (overflow-safe). */
  if (io_size != UINT64_MAX) {
    if (off > io_size || (uint64_t)len > (io_size - off)) {
      return NULL;
    }
  }
  if (io->map) {
    const uint8_t *p = io->map(io, off, len);
    if (p) {
      return p;
    }
  }
  if (!scratch || len > scratch_cap || !io->read) {
    return NULL;
  }
  if (io->read(io, off, scratch, len) != len) {
    return NULL;
  }
  return scratch;
}

/* ================================================================== */
/* Memory backend                                                     */
/* ================================================================== */

typedef struct td_io_mem {
  tinydng_context *ctx;
  const uint8_t *data;
  size_t size;
} td_io_mem;

static size_t td_mem_read(tinydng_io *io, uint64_t off, void *dst, size_t len) {
  td_io_mem *m = (td_io_mem *)io->backend;
  if (off > m->size || (uint64_t)len > (m->size - off)) {
    return 0;
  }
  memcpy(dst, m->data + (size_t)off, len);
  return len;
}

static uint64_t td_mem_size(tinydng_io *io) {
  td_io_mem *m = (td_io_mem *)io->backend;
  return (uint64_t)m->size;
}

static const uint8_t *td_mem_map(tinydng_io *io, uint64_t off, size_t len) {
  td_io_mem *m = (td_io_mem *)io->backend;
  if (off > m->size || (uint64_t)len > (m->size - off)) {
    return NULL;
  }
  return m->data + (size_t)off;
}

static void td_mem_close(tinydng_io *io) {
  td_io_mem *m = (td_io_mem *)io->backend;
  if (m) {
    td_ctx_free(m->ctx, m);
  }
  io->backend = NULL;
}

tinydng_status tinydng_io_open_memory(tinydng_context *ctx, const uint8_t *data,
                                      size_t n, tinydng_io *out,
                                      tinydng_error *err) {
  td_io_mem *m;
  if (!ctx || !data || !out) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_IO, 0, 0, 0,
                 "null argument to io_open_memory");
    return TINYDNG_E_INVALID_ARG;
  }
  m = (td_io_mem *)td_ctx_alloc(ctx, sizeof(*m), err);
  if (!m) {
    return TINYDNG_E_OOM;
  }
  m->ctx = ctx;
  m->data = data;
  m->size = n;
  memset(out, 0, sizeof(*out));
  out->read = td_mem_read;
  out->size = td_mem_size;
  out->map = td_mem_map;
  out->close = td_mem_close;
  out->backend = m;
  return TINYDNG_OK;
}

/* ================================================================== */
/* stdio backend (pull-based; map() == NULL)                          */
/* ================================================================== */

typedef struct td_io_stdio {
  tinydng_context *ctx;
  FILE *fp;
  uint64_t size;
} td_io_stdio;

static int td_seek64(FILE *fp, uint64_t off) {
#if defined(_WIN32)
  return _fseeki64(fp, (__int64)off, SEEK_SET);
#elif defined(__unix__) || defined(__APPLE__) || defined(__linux__)
  return fseeko(fp, (off_t)off, SEEK_SET);
#else
  if (off > (uint64_t)LONG_MAX) {
    return -1;
  }
  return fseek(fp, (long)off, SEEK_SET);
#endif
}

static size_t td_stdio_read(tinydng_io *io, uint64_t off, void *dst,
                            size_t len) {
  td_io_stdio *s = (td_io_stdio *)io->backend;
  if (off > s->size || (uint64_t)len > (s->size - off)) {
    return 0;
  }
  if (td_seek64(s->fp, off) != 0) {
    return 0;
  }
  return fread(dst, 1, len, s->fp);
}

static uint64_t td_stdio_size(tinydng_io *io) {
  td_io_stdio *s = (td_io_stdio *)io->backend;
  return s->size;
}

static void td_stdio_close(tinydng_io *io) {
  td_io_stdio *s = (td_io_stdio *)io->backend;
  if (s) {
    if (s->fp) {
      fclose(s->fp);
    }
    td_ctx_free(s->ctx, s);
  }
  io->backend = NULL;
}

tinydng_status tinydng_io_open_stdio(tinydng_context *ctx, const char *path,
                                     tinydng_io *out, tinydng_error *err) {
  td_io_stdio *s;
  FILE *fp;
  long len_long;
  if (!ctx || !path || !out) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_IO, 0, 0, 0,
                 "null argument to io_open_stdio");
    return TINYDNG_E_INVALID_ARG;
  }
  fp = fopen(path, "rb");
  if (!fp) {
    td_set_error(err, TINYDNG_E_IO, TINYDNG_STAGE_IO, 0, 0, 0,
                 "fopen failed for '%s'", path);
    return TINYDNG_E_IO;
  }
  if (fseek(fp, 0, SEEK_END) != 0 || (len_long = ftell(fp)) < 0 ||
      fseek(fp, 0, SEEK_SET) != 0) {
    fclose(fp);
    td_set_error(err, TINYDNG_E_IO, TINYDNG_STAGE_IO, 0, 0, 0,
                 "failed to size '%s'", path);
    return TINYDNG_E_IO;
  }
  s = (td_io_stdio *)td_ctx_alloc(ctx, sizeof(*s), err);
  if (!s) {
    fclose(fp);
    return TINYDNG_E_OOM;
  }
  s->ctx = ctx;
  s->fp = fp;
  s->size = (uint64_t)len_long;
  memset(out, 0, sizeof(*out));
  out->read = td_stdio_read;
  out->size = td_stdio_size;
  out->map = NULL;
  out->close = td_stdio_close;
  out->backend = s;
  return TINYDNG_OK;
}

/* ================================================================== */
/* mmap backend                                                       */
/* ================================================================== */

#if TINYDNG_HAS_MMAP

typedef struct td_io_mmap {
  tinydng_context *ctx;
  const uint8_t *base;
  size_t size;
#if defined(_WIN32)
  HANDLE file;
  HANDLE mapping;
#else
  int fd;
#endif
} td_io_mmap;

static size_t td_mmap_read(tinydng_io *io, uint64_t off, void *dst,
                           size_t len) {
  td_io_mmap *m = (td_io_mmap *)io->backend;
  if (off > m->size || (uint64_t)len > (m->size - off)) {
    return 0;
  }
  memcpy(dst, m->base + (size_t)off, len);
  return len;
}

static uint64_t td_mmap_size(tinydng_io *io) {
  td_io_mmap *m = (td_io_mmap *)io->backend;
  return (uint64_t)m->size;
}

static const uint8_t *td_mmap_map(tinydng_io *io, uint64_t off, size_t len) {
  td_io_mmap *m = (td_io_mmap *)io->backend;
  if (off > m->size || (uint64_t)len > (m->size - off)) {
    return NULL;
  }
  return m->base + (size_t)off;
}

static void td_mmap_close(tinydng_io *io) {
  td_io_mmap *m = (td_io_mmap *)io->backend;
  if (!m) {
    io->backend = NULL;
    return;
  }
#if defined(_WIN32)
  if (m->base) {
    UnmapViewOfFile((LPCVOID)m->base);
  }
  if (m->mapping) {
    CloseHandle(m->mapping);
  }
  if (m->file && m->file != INVALID_HANDLE_VALUE) {
    CloseHandle(m->file);
  }
#else
  if (m->base && m->size) {
    munmap((void *)m->base, m->size);
  }
  if (m->fd >= 0) {
    close(m->fd);
  }
#endif
  td_ctx_free(m->ctx, m);
  io->backend = NULL;
}

tinydng_status tinydng_io_open_mmap(tinydng_context *ctx, const char *path,
                                    tinydng_io *out, tinydng_error *err) {
  td_io_mmap *m;
  if (!ctx || !path || !out) {
    td_set_error(err, TINYDNG_E_INVALID_ARG, TINYDNG_STAGE_IO, 0, 0, 0,
                 "null argument to io_open_mmap");
    return TINYDNG_E_INVALID_ARG;
  }
  m = (td_io_mmap *)td_ctx_calloc(ctx, sizeof(*m), err);
  if (!m) {
    return TINYDNG_E_OOM;
  }
  m->ctx = ctx;

#if defined(_WIN32)
  {
    LARGE_INTEGER li;
    m->file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (m->file == INVALID_HANDLE_VALUE) {
      td_ctx_free(ctx, m);
      td_set_error(err, TINYDNG_E_IO, TINYDNG_STAGE_IO, 0, 0, 0,
                   "CreateFile failed for '%s'", path);
      return TINYDNG_E_IO;
    }
    if (!GetFileSizeEx(m->file, &li) || li.QuadPart <= 0) {
      CloseHandle(m->file);
      td_ctx_free(ctx, m);
      td_set_error(err, TINYDNG_E_IO, TINYDNG_STAGE_IO, 0, 0, 0,
                   "GetFileSizeEx failed for '%s'", path);
      return TINYDNG_E_IO;
    }
    m->size = (size_t)li.QuadPart;
    m->mapping =
        CreateFileMappingA(m->file, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!m->mapping) {
      CloseHandle(m->file);
      td_ctx_free(ctx, m);
      td_set_error(err, TINYDNG_E_IO, TINYDNG_STAGE_IO, 0, 0, 0,
                   "CreateFileMapping failed for '%s'", path);
      return TINYDNG_E_IO;
    }
    m->base = (const uint8_t *)MapViewOfFile(m->mapping, FILE_MAP_READ, 0, 0, 0);
    if (!m->base) {
      CloseHandle(m->mapping);
      CloseHandle(m->file);
      td_ctx_free(ctx, m);
      td_set_error(err, TINYDNG_E_IO, TINYDNG_STAGE_IO, 0, 0, 0,
                   "MapViewOfFile failed for '%s'", path);
      return TINYDNG_E_IO;
    }
  }
#else
  {
    struct stat st;
    void *map;
    m->fd = open(path, O_RDONLY);
    if (m->fd < 0) {
      td_ctx_free(ctx, m);
      td_set_error(err, TINYDNG_E_IO, TINYDNG_STAGE_IO, 0, 0, 0,
                   "open failed for '%s'", path);
      return TINYDNG_E_IO;
    }
    if (fstat(m->fd, &st) != 0 || st.st_size <= 0) {
      close(m->fd);
      td_ctx_free(ctx, m);
      td_set_error(err, TINYDNG_E_IO, TINYDNG_STAGE_IO, 0, 0, 0,
                   "fstat failed for '%s'", path);
      return TINYDNG_E_IO;
    }
    m->size = (size_t)st.st_size;
    map = mmap(NULL, m->size, PROT_READ, MAP_PRIVATE, m->fd, 0);
    if (map == MAP_FAILED) {
      close(m->fd);
      td_ctx_free(ctx, m);
      td_set_error(err, TINYDNG_E_IO, TINYDNG_STAGE_IO, 0, 0, 0,
                   "mmap failed for '%s'", path);
      return TINYDNG_E_IO;
    }
    m->base = (const uint8_t *)map;
  }
#endif

  memset(out, 0, sizeof(*out));
  out->read = td_mmap_read;
  out->size = td_mmap_size;
  out->map = td_mmap_map;
  out->close = td_mmap_close;
  out->backend = m;
  return TINYDNG_OK;
}

#else /* !TINYDNG_HAS_MMAP */

tinydng_status tinydng_io_open_mmap(tinydng_context *ctx, const char *path,
                                    tinydng_io *out, tinydng_error *err) {
  (void)ctx;
  (void)path;
  (void)out;
  td_set_error(err, TINYDNG_E_UNSUPPORTED, TINYDNG_STAGE_IO, 0, 0, 0,
               "mmap not supported on this platform");
  return TINYDNG_E_UNSUPPORTED;
}

#endif /* TINYDNG_HAS_MMAP */
