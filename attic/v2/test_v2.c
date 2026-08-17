/* DEPRECATED: This v2 test has been superseded by the v3 tests in
   tests/v3_test/. Kept for reference only. */
#include <stdio.h>
#include <string.h>
#include "tiny_dng_v2.h"

int main(int argc, char** argv) {
  const char* path = "colorchart.dng";
  tinydng_v2_context* ctx;
  tinydng_v2_document* doc = NULL;
  tinydng_v2_error err;
  tinydng_v2_status st;
  tinydng_v2_load_options lopt;
  int as_is = 0;

  if (argc > 1) {
    path = argv[1];
  }
  memset(&lopt, 0, sizeof(lopt));
  if (argc > 2 && strcmp(argv[2], "asis") == 0) {
    as_is = 1;
    lopt.flags |= TINYDNG_V2_LOAD_FLAG_PARSE_IMAGE_AS_IS;
  }
  if (argc > 3 && strcmp(argv[3], "mmap") == 0) {
    lopt.flags |= TINYDNG_V2_LOAD_FLAG_USE_MMAP;
  }

  ctx = tinydng_v2_context_create(NULL, &err);
  if (!ctx) {
    fprintf(stderr, "context create failed: %s\n", err.message);
    return 1;
  }

  st = tinydng_v2_load_from_file_with_options(ctx, path,
                                              (lopt.flags != 0u) ? &lopt : NULL,
                                              &doc, &err);
  if (st != TINYDNG_V2_STATUS_OK) {
    fprintf(stderr, "load failed: status=%s stage=%d ifd=%u tag=%u offset=%llu msg=%s\n",
            tinydng_v2_status_string(st), (int)err.stage, err.ifd_index,
            (unsigned)err.tag, (unsigned long long)err.offset, err.message);
    tinydng_v2_context_destroy(ctx);
    return 2;
  }

  printf("images=%zu\n", tinydng_v2_document_image_count(doc));
  for (size_t i = 0; i < tinydng_v2_document_image_count(doc); i++) {
    const tinydng_v2_image* img = tinydng_v2_document_image_at(doc, i);
    tinydng_v2_write_options wopt;
    tinydng_v2_status wst;
    printf("image%zu: %ux%u spp=%u bps=%u comp=%u bytes=%zu offset=%llu segments=%zu flags=%u\n",
           i, img->width, img->height,
           (unsigned)img->samples_per_pixel,
           (unsigned)img->bits_per_sample,
           (unsigned)img->compression,
           img->data_size,
           (unsigned long long)img->data_offset,
           img->segment_count,
           img->flags);
    if (img->exif.make) printf("  make: %s\n", img->exif.make);
    if (img->exif.model) printf("  model: %s\n", img->exif.model);
    if (img->exif.software) printf("  software: %s\n", img->exif.software);
    if (img->exif.datetime) printf("  datetime: %s\n", img->exif.datetime);
    if (img->exif.orientation) printf("  orientation: %u\n", img->exif.orientation);
    
    const tinydng_v2_cfa_pattern* cfa = tinydng_v2_image_cfa(img);
    if (cfa) {
      printf("  CFA: dim=%ux%u pattern_size=%u layout=%u\n",
             cfa->cfa_pattern_dim[0], cfa->cfa_pattern_dim[1],
             cfa->cfa_pattern_size, cfa->cfa_layout);
      printf("  CFA plane colors: %u,%u,%u,%u\n",
             cfa->cfa_plane_color[0], cfa->cfa_plane_color[1],
             cfa->cfa_plane_color[2], cfa->cfa_plane_color[3]);
      if (cfa->cfa_pattern_size > 0) {
        printf("  CFA pattern: ");
        for (size_t p = 0; p < cfa->cfa_pattern_size; p++) {
          if (p > 0) printf(",");
          printf("%u", cfa->cfa_pattern[p]);
        }
        printf("\n");
      }
    }
    const tinydng_v2_raw_info* raw = tinydng_v2_image_raw_info(img);
    if (raw) {
      if (raw->black_level_present) {
        printf("  black_level: %d,%d,%d,%d\n",
               raw->black_level[0], raw->black_level[1],
               raw->black_level[2], raw->black_level[3]);
      }
      if (raw->white_level_present) {
        printf("  white_level: %d,%d,%d,%d\n",
               raw->white_level[0], raw->white_level[1],
               raw->white_level[2], raw->white_level[3]);
      }
      if (raw->color_matrix_present) {
        printf("  color_matrix1: %.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n",
               raw->color_matrix1[0], raw->color_matrix1[1], raw->color_matrix1[2],
               raw->color_matrix1[3], raw->color_matrix1[4], raw->color_matrix1[5],
               raw->color_matrix1[6], raw->color_matrix1[7], raw->color_matrix1[8]);
        printf("  forward_matrix1: %.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n",
               raw->forward_matrix1[0], raw->forward_matrix1[1], raw->forward_matrix1[2],
               raw->forward_matrix1[3], raw->forward_matrix1[4], raw->forward_matrix1[5],
               raw->forward_matrix1[6], raw->forward_matrix1[7], raw->forward_matrix1[8]);
      }
      if (raw->has_dng_version) {
        printf("  DNG version: %u.%u.%u.%u\n",
               raw->dng_version[0], raw->dng_version[1],
               raw->dng_version[2], raw->dng_version[3]);
      }
      if (raw->has_as_shot_neutral) {
        printf("  as_shot_neutral: %.4f,%.4f,%.4f\n",
               raw->as_shot_neutral[0], raw->as_shot_neutral[1], raw->as_shot_neutral[2]);
      }
      if (raw->calibration_illuminant1) {
        printf("  calibration_illuminant1: %u\n", raw->calibration_illuminant1);
      }
      if (raw->calibration_illuminant2) {
        printf("  calibration_illuminant2: %u\n", raw->calibration_illuminant2);
      }
      if (raw->has_default_black_render) {
        printf("  default_black_render: %u\n", raw->default_black_render);
      }
      const char* profile_name = tinydng_v2_image_profile_name(img);
      if (profile_name) {
        printf("  profile_name: %s\n", profile_name);
      }
    }
    if (as_is && img->segments) {
      size_t n = img->segment_count;
      for (size_t k = 0; k < n; k++) {
        printf("  segment%zu: offset=%llu size=%zu\n", k,
               (unsigned long long)img->segments[k].offset,
               img->segments[k].size);
      }
    }

    if (as_is) {
      continue;
    }
    wopt.big_endian = 0;
    wopt.compression = 1;
    wopt.rows_per_strip = img->height;
    wst = tinydng_v2_write_file(ctx, "test_v2_out.dng", img, &wopt, &err);
    if (wst != TINYDNG_V2_STATUS_OK) {
      fprintf(stderr, "write failed: status=%s msg=%s\n",
              tinydng_v2_status_string(wst), err.message);
      tinydng_v2_document_destroy(ctx, doc);
      tinydng_v2_context_destroy(ctx);
      return 3;
    }
  }

  tinydng_v2_document_destroy(ctx, doc);
  tinydng_v2_context_destroy(ctx);
  return 0;
}
