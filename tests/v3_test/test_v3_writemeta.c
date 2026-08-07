/* Metadata round-trip tests for the v3 writer: write a small DNG with EXIF
   and raw-info tags, then read it back and verify the values match. */
#include "td_test_util.h"

static tinydng_context *g_ctx;
static tinydng_error g_err;

static void fill8(uint8_t *p, size_t n, uint32_t seed) {
  size_t i;
  for (i = 0; i < n; i++) {
    p[i] = (uint8_t)((i * 97u + seed * 13u) & 0xFFu);
  }
}

static int exif_roundtrip(const tinydng_exif *exif,
                          const tinydng_raw_info *raw,
                          const tinydng_cfa *cfa) {
  tinydng_write_image meta;
  tinydng_write_options opts;
  uint8_t img[32 * 32 * 3]; /* 32x32 RGB */
  uint8_t *blob = NULL;
  size_t blob_len = 0;
  tinydng_document *doc = NULL;
  const tinydng_exif *got;
  const tinydng_raw_info *graw;
  int ok = 1;

  memset(&meta, 0, sizeof(meta));
  meta.width = 32;
  meta.height = 32;
  meta.samples_per_pixel = 3;
  meta.bits_per_sample = 8;
  meta.data = img;
  meta.data_size = sizeof(img);
  meta.exif = exif;
  meta.raw = raw;
  meta.cfa = cfa;
  memset(&opts, 0, sizeof(opts));
  opts.as_dng = 1;
  opts.big_endian = 0;

  memset(img, 0xAB, sizeof(img));

  if (tinydng_write_memory(g_ctx, &meta, &opts, &blob, &blob_len,
                           &g_err) != TINYDNG_OK) {
    CHECK(0, "exif write_memory: %s", g_err.message);
    return 0;
  }

  if (tinydng_open_memory(g_ctx, blob, blob_len, NULL, &doc, &g_err) !=
      TINYDNG_OK) {
    CHECK(0, "exif open: %s", g_err.message);
    tinydng_buffer_free(g_ctx, blob);
    return 0;
  }

  got = tinydng_document_exif(doc);
  CHECK(got != NULL, "document_exif returned non-NULL");
  /* Verify per-image accessor returns the same pointer for image 0 */
  {
    /* IFD0 EXIF is promoted to doc->global_exif; per-image exif is zeroed */
    const tinydng_image_info *im = tinydng_image_get(doc, 0);
    const tinydng_exif *img_exif = tinydng_image_exif(im);
    CHECK(img_exif == NULL || img_exif->make == NULL,
          "image 0 exif is zeroed after promotion to document_exif");
  }

  if (exif && exif->make) {
    CHECK(got->make && strcmp(got->make, exif->make) == 0,
          "make round-trip: want=%s got=%s", exif->make,
          got->make ? got->make : "(null)");
  }
  if (exif && exif->model) {
    CHECK(got->model && strcmp(got->model, exif->model) == 0,
          "model round-trip: want=%s got=%s", exif->model,
          got->model ? got->model : "(null)");
  }
  if (exif && exif->software) {
    CHECK(got->software && strcmp(got->software, exif->software) == 0,
          "software round-trip: want=%s got=%s", exif->software,
          got->software ? got->software : "(null)");
  }
  if (exif && exif->datetime) {
    CHECK(got->datetime && strcmp(got->datetime, exif->datetime) == 0,
          "datetime round-trip: want=%s got=%s", exif->datetime,
          got->datetime ? got->datetime : "(null)");
  }
  if (exif && exif->image_description) {
    CHECK(got->image_description &&
              strcmp(got->image_description, exif->image_description) == 0,
          "image_description round-trip");
  }
  if (exif && exif->orientation) {
    CHECK(got->orientation == exif->orientation,
          "orientation round-trip: want=%u got=%u", exif->orientation,
          got->orientation);
  }

  if (raw) {
    const tinydng_image_info *im = tinydng_image_get(doc, 0);
    graw = &im->raw;
    CHECK(graw->has_dng_version, "dng_version present");
    if (raw->black_level_present) {
      CHECK(graw->black_level_present &&
                graw->black_level[0] == raw->black_level[0],
            "black_level round-trip: want=%d got=%d",
            raw->black_level[0], graw->black_level[0]);
    }
    if (raw->white_level_present) {
      CHECK(graw->white_level_present &&
                graw->white_level[0] == raw->white_level[0],
            "white_level round-trip: want=%d got=%d",
            raw->white_level[0], graw->white_level[0]);
    }
    if (raw->color_matrix_present) {
      CHECK(graw->color_matrix_present, "color_matrix present");
    }
    if (raw->has_as_shot_neutral) {
      CHECK(graw->has_as_shot_neutral, "as_shot_neutral present");
    }
    if (raw->calibration_illuminant1) {
      CHECK(graw->calibration_illuminant1 == raw->calibration_illuminant1,
            "calibration_illuminant1 round-trip");
    }
    if (raw->calibration_illuminant2) {
      CHECK(graw->calibration_illuminant2 == raw->calibration_illuminant2,
            "calibration_illuminant2 round-trip");
    }
    if (raw->forward_matrix1[0] != 0.0) {
      CHECK(graw->forward_matrix1[0] > 0.999 && graw->forward_matrix1[0] < 1.001,
            "forward_matrix1 round-trip");
    }
    if (raw->camera_calibration_present) {
      CHECK(graw->camera_calibration1[0] > 0.999 &&
                graw->camera_calibration1[0] < 1.001,
            "camera_calibration1 round-trip");
    }
    if (raw->has_analog_balance) {
      CHECK(graw->has_analog_balance, "analog_balance present");
    }
    if (raw->has_default_black_render) {
      CHECK(graw->has_default_black_render &&
                graw->default_black_render == raw->default_black_render,
            "default_black_render round-trip");
    }
    if (raw->profile_name) {
      CHECK(graw->profile_name &&
                strcmp(graw->profile_name, raw->profile_name) == 0,
            "profile_name round-trip");
    }
    if (raw->has_active_area) {
      CHECK(graw->has_active_area &&
                graw->active_area[0] == raw->active_area[0] &&
                graw->active_area[1] == raw->active_area[1] &&
                graw->active_area[2] == raw->active_area[2] &&
                graw->active_area[3] == raw->active_area[3],
            "active_area round-trip");
    }
  }

  if (cfa && cfa->present) {
    const tinydng_image_info *im = tinydng_image_get(doc, 0);
    CHECK(im->cfa.present, "cfa present");
  }

  tinydng_document_destroy(g_ctx, doc);
  tinydng_buffer_free(g_ctx, blob);
  return ok;
}

static int test_basic_exif(void) {
  tinydng_exif exif;
  memset(&exif, 0, sizeof(exif));
  exif.make = "TestMake";
  exif.model = "TestModel";
  exif.software = "TestSw";
  exif.datetime = "2024:01:02 03:04:05";
  exif.image_description = "TestDesc";
  exif.orientation = 6;
  exif.exposure_time[0] = 1;
  exif.exposure_time[1] = 100;
  exif.has_exposure_time = 1;
  exif.iso = 400;
  exif.has_iso = 1;

  int ok = exif_roundtrip(&exif, NULL, NULL);
  CHECK(ok, "basic exif round-trip");
  if (ok) {
    printf("  basic EXIF tags round-trip OK\n");
  }
  return ok;
}

static int test_raw_info(void) {
  tinydng_raw_info raw;
  memset(&raw, 0, sizeof(raw));
  raw.has_dng_version = 1;
  raw.dng_version[0] = 1;
  raw.dng_version[1] = 4;
  raw.dng_version[2] = 0;
  raw.dng_version[3] = 0;
  raw.black_level_present = 1;
  raw.black_level[0] = 512;
  raw.white_level_present = 1;
  raw.white_level[0] = 65535;
  raw.color_matrix_present = 1;
  /* ColorMatrix1 = identity-ish */
  raw.color_matrix1[0] = 1.0;
  raw.color_matrix1[4] = 1.0;
  raw.color_matrix1[8] = 1.0;
  raw.has_as_shot_neutral = 1;
  raw.as_shot_neutral[0] = 0.5;
  raw.as_shot_neutral[1] = 0.5;
  raw.as_shot_neutral[2] = 0.5;
  raw.calibration_illuminant1 = 32803u; /* D65 */
  raw.calibration_illuminant2 = 32804u; /* D50 */
  raw.has_analog_balance = 1;
  raw.analog_balance[0] = 1.0;
  raw.analog_balance[1] = 1.0;
  raw.analog_balance[2] = 1.0;
  raw.has_active_area = 1;
  raw.active_area[0] = 4;
  raw.active_area[1] = 8;
  raw.active_area[2] = 28;
  raw.active_area[3] = 24;
  raw.has_default_black_render = 1;
  raw.default_black_render = 1;
  raw.profile_name = "TestProfile";
  raw.forward_matrix1[0] = 1.0;
  raw.forward_matrix1[4] = 1.0;
  raw.forward_matrix1[8] = 1.0;
  raw.camera_calibration_present = 1;
  raw.camera_calibration1[0] = 1.0;
  raw.camera_calibration1[4] = 1.0;
  raw.camera_calibration1[8] = 1.0;

  int ok = exif_roundtrip(NULL, &raw, NULL);
  CHECK(ok, "raw_info round-trip");
  if (ok) {
    printf("  raw_info tags round-trip OK\n");
  }
  return ok;
}

static int test_cfa_raw(void) {
  tinydng_cfa cfa;
  tinydng_raw_info raw;
  tinydng_exif exif;
  memset(&cfa, 0, sizeof(cfa));
  cfa.present = 1;
  cfa.pattern_dim[0] = 2;
  cfa.pattern_dim[1] = 2;
  cfa.pattern[0] = 0;
  cfa.pattern[1] = 1;
  cfa.pattern[2] = 1;
  cfa.pattern[3] = 0;
  cfa.pattern_size = 4;
  cfa.layout = 1;

  memset(&raw, 0, sizeof(raw));
  raw.has_dng_version = 1;
  raw.dng_version[0] = 1;
  raw.dng_version[1] = 4;

  memset(&exif, 0, sizeof(exif));
  exif.make = "Google";
  exif.model = "Pixel 3 XL";

  int ok = exif_roundtrip(&exif, &raw, &cfa);
  CHECK(ok, "CFA + raw + exif round-trip");
  if (ok) {
    printf("  CFA + raw + exif round-trip OK\n");
  }
  return ok;
}


static int test_zip_roundtrip(void) {
  tinydng_write_image meta;
  tinydng_write_options opts;
  uint8_t img[16 * 16 * 3];
  uint8_t *blob = NULL;
  size_t blob_len = 0;
  tinydng_document *doc = NULL;
  const tinydng_image_info *im;
  tinydng_pixels px;
  int ok = 1;

  memset(&meta, 0, sizeof(meta));
  meta.width = 16;
  meta.height = 16;
  meta.samples_per_pixel = 3;
  meta.bits_per_sample = 8;
  meta.data = img;
  meta.data_size = sizeof(img);
  memset(&opts, 0, sizeof(opts));
  opts.big_endian = 0;
  opts.compression = TINYDNG_COMPRESSION_ZIP;

  fill8(img, sizeof(img), 0x55);

  if (tinydng_write_memory(g_ctx, &meta, &opts, &blob, &blob_len,
                           &g_err) != TINYDNG_OK) {
    CHECK(0, "zip write: %s", g_err.message);
    return 0;
  }

  if (tinydng_open_memory(g_ctx, blob, blob_len, NULL, &doc, &g_err) !=
      TINYDNG_OK) {
    CHECK(0, "zip reopen: %s", g_err.message);
    tinydng_buffer_free(g_ctx, blob);
    return 0;
  }
  im = tinydng_image_get(doc, 0);
  CHECK(im && im->compression == TINYDNG_COMPRESSION_ZIP,
        "compression is ZIP (%u)", im ? im->compression : 999);

  if (tinydng_decode_image(g_ctx, doc, 0, NULL, &px, &g_err) != TINYDNG_OK) {
    CHECK(0, "zip decode: %s", g_err.message);
    ok = 0;
  } else if (px.size != sizeof(img) ||
             memcmp(px.data, img, sizeof(img)) != 0) {
    CHECK(0, "zip pixel mismatch");
    ok = 0;
  } else {
    ok = 1;
  }
  if (px.data) {
    tinydng_pixels_free(g_ctx, &px);
  }
  tinydng_document_destroy(g_ctx, doc);
  tinydng_buffer_free(g_ctx, blob);
  if (ok) {
    printf("  ZIP compression round-trip OK\n");
  }
  return ok;
}

/* Read colorchart.dng, write it back as a DNG with EXIF+raw, and verify
   the metadata survives the round-trip. argv[1] = source root. */
static int test_real_file_roundtrip(const char *root) {
  char path[1024];
  unsigned char *blob;
  size_t n = 0;
  tinydng_document *doc = NULL;
  tinydng_error err;
  const tinydng_image_info *im;
  tinydng_write_image wmeta;
  tinydng_write_options wopts;
  uint8_t *out = NULL;
  size_t out_len = 0;
  tinydng_document *rd_doc = NULL;
  const tinydng_exif *gexif;
  int ok = 1;

  snprintf(path, sizeof(path), "%s/colorchart.dng", root);
  blob = tdt_slurp(path, &n);
  if (!blob) {
    CHECK(0, "colorchart.dng missing at %s", path);
    return 1;
  }
  if (tinydng_open_memory(g_ctx, blob, n, NULL, &doc, &err) != TINYDNG_OK) {
    CHECK(0, "colorchart open: %s", err.message);
    free(blob);
    return 1;
  }
  im = tinydng_image_get(doc, 0);
  CHECK(im && im->width == 1888 && im->height == 1182, "dims");

  /* Decode pixels so we can re-encode. */
  {
    tinydng_pixels px;
    if (tinydng_decode_image(g_ctx, doc, 0, NULL, &px, &g_err) != TINYDNG_OK) {
      CHECK(0, "colorchart decode: %s", g_err.message);
      tinydng_buffer_free(g_ctx, blob);
      tinydng_document_destroy(g_ctx, doc);
      return 1;
    }

    memset(&wmeta, 0, sizeof(wmeta));
    wmeta.width = im->width;
    wmeta.height = im->height;
    wmeta.samples_per_pixel = im->samples_per_pixel;
    wmeta.bits_per_sample = 8; /* decode 14-bit to 8-bit for the write */
    wmeta.data = px.data;
    wmeta.data_size = px.size;
    wmeta.exif = tinydng_document_exif(doc);
    wmeta.raw = &im->raw;
    wmeta.cfa = &im->cfa;
    memset(&wopts, 0, sizeof(wopts));
    wopts.as_dng = 1;
    wopts.big_endian = 0;
    wopts.compression = TINYDNG_COMPRESSION_NONE;

    if (tinydng_write_memory(g_ctx, &wmeta, &wopts, &out, &out_len,
                             &g_err) != TINYDNG_OK) {
      CHECK(0, "writemeta write: %s", g_err.message);
      ok = 0;
    }
    if (ok && tinydng_open_memory(g_ctx, out, out_len, NULL, &rd_doc,
                                  &err) != TINYDNG_OK) {
      CHECK(0, "writemeta reopen: %s", err.message);
      ok = 0;
    }
    if (ok) {
      gexif = tinydng_document_exif(rd_doc);
      CHECK(gexif && gexif->make && strcmp(gexif->make, "Canon") == 0,
            "real: make=Canon (got %s)",
            gexif ? (gexif->make ? gexif->make : "(null)") : "(null)");
      CHECK(gexif && gexif->model &&
                strcmp(gexif->model, "Canon EOS Kiss X4") == 0,
            "real: model=Canon EOS Kiss X4 (got %s)",
            gexif ? (gexif->model ? gexif->model : "(null)") : "(null)");
    }

    tinydng_pixels_free(g_ctx, &px);
  }

  if (rd_doc) {
    tinydng_document_destroy(g_ctx, rd_doc);
  }
  if (out) {
    tinydng_buffer_free(g_ctx, out);
  }
  tinydng_document_destroy(g_ctx, doc);
  free(blob);
  if (ok) {
    printf("  real-file EXIF round-trip OK (Canon EOS Kiss X4)\n");
  }
  return ok;
}

int main(int argc, char **argv) {
  const char *root = (argc > 1) ? argv[1] : ".";
  g_ctx = tinydng_context_create(NULL, &g_err);
  if (!g_ctx) {
    return 1;
  }
  printf("== writemeta tests ==\n");
  test_basic_exif();
  test_raw_info();
  test_cfa_raw();
  test_zip_roundtrip();
  test_real_file_roundtrip(root);

  if (tinydng_context_memory_used(g_ctx) != 0u) {
    fprintf(stderr, "  FAIL: leak, used=%zu\n",
            tinydng_context_memory_used(g_ctx));
    g_fail = 1;
  }
  tinydng_context_destroy(g_ctx);
  printf(g_fail ? "WRITEMETA: FAILURES\n" : "WRITEMETA: ALL PASS\n");
  return g_fail;
}
