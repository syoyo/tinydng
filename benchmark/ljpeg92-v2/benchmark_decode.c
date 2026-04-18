#if defined(__unix__) || defined(__APPLE__)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../tiny_dng_v2.h"
#include "../../tiny_dng_ljpeg92_v2.h"

typedef struct ljpeg_view {
  const uint8_t* data;
  size_t size;
} ljpeg_view;

typedef struct ljpeg_workload {
  ljpeg_view* streams;
  size_t stream_count;
  uint32_t width;
  uint32_t height;
  uint32_t spp;
  uint32_t image_index;
  uint32_t compression;
} ljpeg_workload;

static double now_ms(void) {
#if defined(CLOCK_MONOTONIC)
  struct timespec ts;
  (void)clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
#else
  struct timespec ts;
  (void)timespec_get(&ts, TIME_UTC);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
#endif
}

int main(int argc, char** argv) {
  tinydng_v2_context* ctx = NULL;
  tinydng_v2_document* doc = NULL;
  tinydng_v2_error err;
  tinydng_v2_load_options lopt;
  tinydng_v2_status st;
  ljpeg_workload work;
  uint16_t* out = NULL;
  int iters = 100;
  double t0;
  double t1;
  uint64_t checksum = 0;
  int bits = 0;
  int max_w = 0;
  int max_h = 0;
  uint64_t pixels_per_iter = 0;

  if (argc < 2) {
    fprintf(stderr, "usage: %s <dng_file> [iterations]\n", argv[0]);
    return 1;
  }

  if (argc >= 3) {
    iters = atoi(argv[2]);
    if (iters <= 0) {
      fprintf(stderr, "invalid iterations: %s\n", argv[2]);
      return 1;
    }
  }

  ctx = tinydng_v2_context_create(NULL, &err);
  if (!ctx) {
    fprintf(stderr, "failed to create v2 context: %s\n", err.message);
    return 2;
  }

  memset(&lopt, 0, sizeof(lopt));
  lopt.flags = TINYDNG_V2_LOAD_FLAG_PARSE_IMAGE_AS_IS;

  st = tinydng_v2_load_from_file_with_options(ctx, argv[1], &lopt, &doc, &err);
  if (st != TINYDNG_V2_STATUS_OK) {
    fprintf(stderr, "failed to load DNG %s: %s\n", argv[1], err.message);
    tinydng_v2_context_destroy(ctx);
    return 3;
  }

  memset(&work, 0, sizeof(work));
  {
    size_t img_count = tinydng_v2_document_image_count(doc);
    size_t i;
    int found = 0;
    // Prefer image index 1 (often the Raw image in ProRAW) then index 0.
    size_t target_indices[] = {1, 0, 2};
    for (size_t j = 0; j < 3; j++) {
      i = target_indices[j];
      if (i >= img_count) continue;
      const tinydng_v2_image* img = tinydng_v2_document_image_at(doc, i);
      // Compression 6 (Old JPEG) or 7 (New JPEG, often LJPEG in DNG)
      if ((img->compression == 6 || img->compression == 7) && img->segment_count > 0) {
        // Try to open first segment to see if it is really LJPEG (contains SOF3)
        tdng_lj92 probe = NULL;
        int w, h, b, c_probe;
        size_t base_size = 0;
        const uint8_t* base_ptr = tinydng_v2_document_memory(doc, &base_size);
        int ret = tdng_lj92_open(&probe, base_ptr + img->segments[0].offset,
                                 (int)img->segments[0].size, &w, &h, &b, &c_probe);
        if (ret == TDNG_LJ92_ERROR_NONE && probe && w > 0) {
          tdng_lj92_close(probe);
          work.image_index = (uint32_t)i;
          work.compression = img->compression;
          work.width = img->width;
          work.height = img->height;
          work.spp = img->samples_per_pixel;
          work.stream_count = img->segment_count;
          work.streams = (ljpeg_view*)calloc(work.stream_count, sizeof(ljpeg_view));
          if (!work.streams) {
            fprintf(stderr, "oom\n");
            tinydng_v2_document_destroy(ctx, doc);
            tinydng_v2_context_destroy(ctx);
            return 4;
          }
          for (size_t k = 0; k < work.stream_count; k++) {
            work.streams[k].data = base_ptr + img->segments[k].offset;
            work.streams[k].size = img->segments[k].size;
          }
          found = 1;
          break;
        }
        if (probe) tdng_lj92_close(probe);
      }
    }
    if (!found) {
      // Fallback: search all images
      for (i = 0; i < img_count; i++) {
        const tinydng_v2_image* img = tinydng_v2_document_image_at(doc, i);
        if ((img->compression == 6 || img->compression == 7) && img->segment_count > 0) {
          tdng_lj92 probe = NULL;
          int w, h, b, c_probe;
          size_t base_size = 0;
          const uint8_t* base_ptr = tinydng_v2_document_memory(doc, &base_size);
          int ret = tdng_lj92_open(&probe, base_ptr + img->segments[0].offset,
                                   (int)img->segments[0].size, &w, &h, &b, &c_probe);
          if (ret == TDNG_LJ92_ERROR_NONE && probe && w > 0) {
            tdng_lj92_close(probe);
            work.image_index = (uint32_t)i;
            work.compression = img->compression;
            work.width = img->width;
            work.height = img->height;
            work.spp = img->samples_per_pixel;
            work.stream_count = img->segment_count;
            work.streams = (ljpeg_view*)calloc(work.stream_count, sizeof(ljpeg_view));
            if (!work.streams) {
              fprintf(stderr, "oom\n");
              tinydng_v2_document_destroy(ctx, doc);
              tinydng_v2_context_destroy(ctx);
              return 4;
            }
            for (size_t k = 0; k < work.stream_count; k++) {
              work.streams[k].data = base_ptr + img->segments[k].offset;
              work.streams[k].size = img->segments[k].size;
            }
            found = 1;
            break;
          }
          if (probe) tdng_lj92_close(probe);
        }
      }
    }
    if (!found) {
      fprintf(stderr, "failed to find LJPEG compressed image in %s\n", argv[1]);
      tinydng_v2_document_destroy(ctx, doc);
      tinydng_v2_context_destroy(ctx);
      return 5;
    }
  }

  {
    for (size_t i = 0; i < work.stream_count; i++) {
      tdng_lj92 probe = NULL;
      int w = 0;
      int h = 0;
      int comp = 0;
      int ret = tdng_lj92_open(&probe, work.streams[i].data,
                               (int)work.streams[i].size, &w, &h, &bits, &comp);
      if (ret != TDNG_LJ92_ERROR_NONE || !probe || w <= 0 || h <= 0 || bits <= 0) {
        if (probe) {
          tdng_lj92_close(probe);
        }
        fprintf(stderr,
                "unsupported or invalid ljpeg tile stream[%zu] (ret=%d w=%d h=%d bits=%d)\n",
                i, ret, w, h, bits);
        free(work.streams);
        tinydng_v2_document_destroy(ctx, doc);
        tinydng_v2_context_destroy(ctx);
        return 6;
      }
      tdng_lj92_close(probe);
      if (w > max_w) max_w = w;
      if (h > max_h) max_h = h;
      if (comp > (int)work.spp) work.spp = (uint32_t)comp;
      pixels_per_iter += (uint64_t)w * (uint64_t)h;
    }
  }

  // Allocate output buffer for one tile/strip. Add some padding for safety.
  out = (uint16_t*)malloc(((size_t)max_w * (size_t)max_h * (size_t)work.spp + 1024) * sizeof(uint16_t));
  if (!out) {
    fprintf(stderr, "out buffer alloc failed\n");
    free(work.streams);
    tinydng_v2_document_destroy(ctx, doc);
    tinydng_v2_context_destroy(ctx);
    return 7;
  }

  t0 = now_ms();
  for (int i = 0; i < iters; i++) {
    for (size_t k = 0; k < work.stream_count; k++) {
      tdng_lj92 lj = NULL;
      int w = 0;
      int h = 0;
      int comp = 0;
      int ret = tdng_lj92_open(&lj, work.streams[k].data,
                               (int)work.streams[k].size, &w, &h, &bits, &comp);
      if (ret != TDNG_LJ92_ERROR_NONE || !lj) {
        fprintf(stderr, "tdng_lj92_open failed stream=%zu ret=%d\n", k, ret);
        free(out);
        free(work.streams);
        tinydng_v2_document_destroy(ctx, doc);
        tinydng_v2_context_destroy(ctx);
        return 8;
      }
      ret = tdng_lj92_decode(lj, out, w * comp, 0, NULL, 0);
      tdng_lj92_close(lj);
      if (ret != TDNG_LJ92_ERROR_NONE) {
        fprintf(stderr, "tdng_lj92_decode failed stream=%zu ret=%d\n", k, ret);
        free(out);
        free(work.streams);
        tinydng_v2_document_destroy(ctx, doc);
        tinydng_v2_context_destroy(ctx);
        return 9;
      }
      checksum += (uint64_t)out[0];
      checksum += (uint64_t)out[(size_t)w * (size_t)comp * (size_t)h - 1u];
    }
  }
  t1 = now_ms();

  {
    double elapsed_ms = t1 - t0;
    double mpix = ((double)pixels_per_iter * (double)iters) / 1000000.0;
    double mpix_per_sec = (elapsed_ms > 0.0) ? (mpix * 1000.0 / elapsed_ms) : 0.0;
    printf("file=%s\n", argv[1]);
    printf("image_index=%u compression=%u image=%ux%u streams=%zu max_tile=%dx%d spp=%u bits=%d\n",
           work.image_index, work.compression, work.width, work.height,
           work.stream_count, max_w, max_h, work.spp, bits);
    printf("iters=%d elapsed_ms=%.3f per_iter_ms=%.3f throughput_mpix_s=%.3f\n",
           iters, elapsed_ms, elapsed_ms / (double)iters, mpix_per_sec);
    printf("checksum=%llu\n", (unsigned long long)checksum);
  }

  free(out);
  free(work.streams);
  tinydng_v2_document_destroy(ctx, doc);
  tinydng_v2_context_destroy(ctx);
  return 0;
}
