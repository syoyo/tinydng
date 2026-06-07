/*
 * test_v3.c - smoke test for the clean-room tinydng parser.
 *   usage: test_dng_v3 <file> [subifds] [mmap]
 */
#include "tinydng.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
  tinydng_context *ctx;
  tinydng_document *doc = NULL;
  tinydng_error err;
  tinydng_open_options opts;
  tinydng_status st;
  size_t i, n;
  int arg;

  if (argc < 2) {
    fprintf(stderr, "usage: %s <file.dng|tiff> [subifds] [mmap]\n", argv[0]);
    return 1;
  }

  memset(&opts, 0, sizeof(opts));
  opts.flags = TINYDNG_OPEN_PARSE_SUBIFDS; /* default on for this tool */
  for (arg = 2; arg < argc; arg++) {
    if (strcmp(argv[arg], "nosubifds") == 0) {
      opts.flags &= ~(uint32_t)TINYDNG_OPEN_PARSE_SUBIFDS;
    } else if (strcmp(argv[arg], "mmap") == 0) {
      opts.flags |= TINYDNG_OPEN_PREFER_MMAP;
    }
  }

  ctx = tinydng_context_create(NULL, &err);
  if (!ctx) {
    fprintf(stderr, "context create failed: %s\n", err.message);
    return 1;
  }

  st = tinydng_open_file(ctx, argv[1], &opts, &doc, &err);
  if (st != TINYDNG_OK) {
    fprintf(stderr, "open failed [%s/%s]: %s (ifd=%u tag=%u off=%llu)\n",
            tinydng_status_string(err.status), tinydng_stage_string(err.stage),
            err.message, err.ifd_index, err.tag,
            (unsigned long long)err.offset);
    tinydng_context_destroy(ctx);
    return 1;
  }

  n = tinydng_image_count(doc);
  printf("images: %zu\n", n);
  {
    const tinydng_exif *ex = tinydng_document_exif(doc);
    if (ex) {
      printf("make=%s model=%s software=%s\n", ex->make ? ex->make : "(null)",
             ex->model ? ex->model : "(null)",
             ex->software ? ex->software : "(null)");
    }
  }

  for (i = 0; i < n; i++) {
    const tinydng_image_info *img = tinydng_image_get(doc, i);
    printf("--- image %zu ---\n", i);
    printf("  %ux%u spp=%u bps=%u comp=%u sfmt=%u pred=%u planar=%u\n",
           img->width, img->height, img->samples_per_pixel,
           img->bits_per_sample, img->compression, img->sample_format,
           img->predictor, img->planar_configuration);
    printf("  tile=%ux%u rows_per_strip=%u segments=%zu\n", img->tile_width,
           img->tile_length, img->rows_per_strip, img->segment_count);
    if (img->raw.has_dng_version) {
      printf("  dng_version=%u.%u.%u.%u\n", img->raw.dng_version[0],
             img->raw.dng_version[1], img->raw.dng_version[2],
             img->raw.dng_version[3]);
    }
    if (img->raw.black_level_present) {
      printf("  black_level=%d white_level=%d\n", img->raw.black_level[0],
             img->raw.white_level[0]);
    }
    if (img->cfa.present) {
      printf("  cfa dim=%ux%u pattern=[%u %u %u %u] size=%u\n",
             img->cfa.pattern_dim[0], img->cfa.pattern_dim[1],
             img->cfa.pattern[0], img->cfa.pattern[1], img->cfa.pattern[2],
             img->cfa.pattern[3], img->cfa.pattern_size);
    }
    if (img->raw.color_matrix_present) {
      printf("  color_matrix1[0..2]=%.4f %.4f %.4f\n",
             img->raw.color_matrix1[0], img->raw.color_matrix1[1],
             img->raw.color_matrix1[2]);
    }
    if (img->exif.has_iso || img->exif.has_exposure_time ||
        img->exif.has_aperture_value) {
      printf("  exif iso=%u exposure=%d/%d aperture=%d/%d\n", img->exif.iso,
             img->exif.exposure_time[0], img->exif.exposure_time[1],
             img->exif.aperture_value[0], img->exif.aperture_value[1]);
    }
    if (img->raw.opcode_count) {
      size_t oi;
      printf("  opcodes=%zu (warps=%zu vignettes=%zu gainmaps=%zu)\n",
             img->raw.opcode_count, img->raw.warp_count,
             img->raw.vignette_count, img->raw.gainmap_count);
      for (oi = 0; oi < img->raw.opcode_count; oi++) {
        const tinydng_opcode *op = &img->raw.opcodes[oi];
        printf("    op[%zu] list%u id=%u ver=%u flags=%u params=%zuB\n", oi,
               op->list, op->id, op->version, op->flags, op->params_size);
      }
      if (img->raw.warp_count) {
        const tinydng_warp_rectilinear *wp = &img->raw.warps[0];
        printf("    warp[0] planes=%u kr0=%.5f kr1=%.5f center=(%.3f,%.3f)\n",
               wp->plane_count, wp->coeff[0][0], wp->coeff[0][1],
               wp->center[0], wp->center[1]);
      }
      if (img->raw.vignette_count) {
        const tinydng_vignette_radial *vg = &img->raw.vignettes[0];
        printf("    vignette[0] k0=%.5f k1=%.5f center=(%.3f,%.3f)\n", vg->k[0],
               vg->k[1], vg->center[0], vg->center[1]);
      }
    }
    if (img->raw.gainmap_count) {
      size_t gi;
      printf("  gainmaps=%zu\n", img->raw.gainmap_count);
      for (gi = 0; gi < img->raw.gainmap_count; gi++) {
        const tinydng_gainmap *gm = &img->raw.gainmaps[gi];
        printf("    [%zu] list%u rect=(%u,%u,%u,%u) plane=%u/%u map=%ux%ux%u "
               "pixels=%zu\n",
               gi, gm->opcode_list, gm->top, gm->left, gm->bottom, gm->right,
               gm->plane, gm->planes, gm->map_points_v, gm->map_points_h,
               gm->map_planes, gm->pixel_count);
      }
    }
  }

  /* Decode the largest image and print sample statistics. */
  {
    size_t best = 0;
    uint64_t best_px = 0;
    for (i = 0; i < n; i++) {
      const tinydng_image_info *im = tinydng_image_get(doc, i);
      uint64_t px = (uint64_t)im->width * im->height;
      if (px > best_px) {
        best_px = px;
        best = i;
      }
    }
    {
      tinydng_pixels px;
      tinydng_error derr;
      tinydng_status ds =
          tinydng_decode_image(ctx, doc, best, NULL, &px, &derr);
      printf("--- decode image %zu ---\n", best);
      if (ds != TINYDNG_OK) {
        printf("  decode: %s: %s\n", tinydng_status_string(derr.status),
               derr.message);
      } else {
        double sum = 0.0;
        double mn = 1e30, mx = -1e30;
        size_t count = (size_t)px.width * px.height * px.samples_per_pixel;
        size_t k;
        for (k = 0; k < count; k++) {
          double v;
          if (px.bits_per_sample == 8) {
            v = ((const uint8_t *)px.data)[k];
          } else if (px.bits_per_sample == 16) {
            v = ((const uint16_t *)px.data)[k];
          } else if (px.sample_format == TINYDNG_SAMPLEFORMAT_IEEEFP) {
            v = ((const float *)px.data)[k];
          } else {
            v = ((const uint32_t *)px.data)[k];
          }
          if (v < mn) mn = v;
          if (v > mx) mx = v;
          sum += v;
        }
        printf("  decoded %ux%u spp=%u bps=%u size=%zu\n", px.width, px.height,
               px.samples_per_pixel, px.bits_per_sample, px.size);
        printf("  sample min=%.3f max=%.3f mean=%.3f\n", mn, mx,
               count ? sum / (double)count : 0.0);
        tinydng_pixels_free(ctx, &px);
      }
    }
  }

  printf("memory used=%zu peak=%zu\n", tinydng_context_memory_used(ctx),
         tinydng_context_memory_peak(ctx));

  tinydng_document_destroy(ctx, doc);
  printf("memory after destroy=%zu\n", tinydng_context_memory_used(ctx));
  tinydng_context_destroy(ctx);
  return 0;
}
