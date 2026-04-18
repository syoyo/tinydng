#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define TINYDNG_V2_IMPLEMENTATION
#include "tiny_dng_v2.h"

#define NUM_TEST_FILES 4

static const char* test_files[NUM_TEST_FILES] = {
    "/mnt/nvme02/work/tinydng/colorchart.dng",
    "/mnt/nvme02/work/tinydng/pixel3.dng",
    "/mnt/nvme02/work/tinydng/proraw-48mp-02.dng",
    "/mnt/nvme02/work/tinydng/IMG_9747.CR2"
};

static int test_count = 0;
static int pass_count = 0;
static int fail_count = 0;

static void check(int condition, const char* msg) {
    test_count++;
    if (condition) {
        pass_count++;
        printf("  [PASS] %s\n", msg);
    } else {
        fail_count++;
        printf("  [FAIL] %s\n", msg);
    }
}

static void test_colorchart(tinydng_v2_context* ctx, tinydng_v2_error* err) {
    printf("Testing colorchart.dng...\n");

    tinydng_v2_document* doc = NULL;
    tinydng_v2_status st = tinydng_v2_load_from_file_with_options(ctx, test_files[0], NULL, &doc, err);
    check(st == TINYDNG_V2_STATUS_OK, "Load succeeded");
    check(doc != NULL, "Document not NULL");

    if (!doc) return;

    check(tinydng_v2_document_image_count(doc) == 1, "Has 1 image");

    const tinydng_v2_image* img = tinydng_v2_document_image_at(doc, 0);
    check(img != NULL, "Image 0 retrieved");
    if (!img) { tinydng_v2_document_destroy(ctx, doc); return; }

    check(img->width == 1888, "Width = 1888");
    check(img->height == 1182, "Height = 1182");
    check(img->bits_per_sample == 14, "BPS = 14");
    check(img->samples_per_pixel == 1, "SPP = 1");

    const tinydng_v2_raw_info* raw = &img->raw_info;
    check(raw != NULL, "Has raw_info");
    if (raw) {
        check(raw->black_level_present == 1, "Black level present");
        check(raw->black_level[0] == 2056, "Black level[0] = 2056");
        check(raw->white_level_present == 1, "White level present");
        check(raw->white_level[0] == 15000, "White level[0] = 15000");
        check(raw->color_matrix_present == 1, "Color matrix present");
        check(raw->has_as_shot_neutral == 1, "Has as_shot_neutral");
        double neutral_diff = fabs(raw->as_shot_neutral[0] - 1.0) +
                             fabs(raw->as_shot_neutral[1] - 2.477) +
                             fabs(raw->as_shot_neutral[2] - 1.462);
        check(neutral_diff < 0.01, "As-shot neutral values correct");
    }

    tinydng_v2_document_destroy(ctx, doc);
}

static void test_pixel3(tinydng_v2_context* ctx, tinydng_v2_error* err) {
    printf("Testing pixel3.dng...\n");

    tinydng_v2_document* doc = NULL;
    tinydng_v2_status st = tinydng_v2_load_from_file_with_options(ctx, test_files[1], NULL, &doc, err);
    check(st == TINYDNG_V2_STATUS_OK, "Load succeeded");
    check(doc != NULL, "Document not NULL");

    if (!doc) return;

    check(tinydng_v2_document_image_count(doc) == 1, "Has 1 image");

    const tinydng_v2_image* img = tinydng_v2_document_image_at(doc, 0);
    check(img != NULL, "Image 0 retrieved");
    if (!img) { tinydng_v2_document_destroy(ctx, doc); return; }

    check(img->width == 672, "Width = 672");
    check(img->height == 504, "Height = 504");

    const tinydng_v2_basic_exif* exif = &img->exif;
    check(exif != NULL, "Has EXIF data");
    if (exif && exif->orientation) {
        check(exif->orientation == 6, "Orientation = 6");
    }

    const tinydng_v2_raw_info* raw = &img->raw_info;
    check(raw != NULL, "Has raw_info");
    if (raw) {
        check(raw->color_matrix_present == 1, "Color matrix present");
        check(raw->has_as_shot_neutral == 1, "Has as_shot_neutral");
        check(raw->calibration_illuminant1 == 20, "Calibration illuminant1 = 20 (D55)");
        check(raw->calibration_illuminant2 == 17, "Calibration illuminant2 = 17 (Std Light A)");
    }

    tinydng_v2_document_destroy(ctx, doc);
}

static void test_proraw(tinydng_v2_context* ctx, tinydng_v2_error* err) {
    printf("Testing proraw-48mp-02.dng...\n");

    tinydng_v2_document* doc = NULL;
    tinydng_v2_status st = tinydng_v2_load_from_file_with_options(ctx, test_files[2], NULL, &doc, err);
    check(st == TINYDNG_V2_STATUS_OK, "Load succeeded");
    check(doc != NULL, "Document not NULL");

    if (!doc) return;

    check(tinydng_v2_document_image_count(doc) == 1, "Has 1 image");

    const tinydng_v2_image* img = tinydng_v2_document_image_at(doc, 0);
    check(img != NULL, "Image 0 retrieved");
    if (!img) { tinydng_v2_document_destroy(ctx, doc); return; }

    check(img->width == 8064, "Width = 8064");
    check(img->height == 6048, "Height = 6048");
    check(img->samples_per_pixel == 3, "SPP = 3");
    check(img->bits_per_sample == 8, "BPS = 8");

    const tinydng_v2_raw_info* raw = &img->raw_info;
    check(raw != NULL, "Has raw_info");
    if (raw) {
        check(raw->color_matrix_present == 1, "Color matrix present");
        double diff = fabs(raw->color_matrix1[0] - 1.2902) +
                      fabs(raw->color_matrix1[1] - (-0.6245));
        check(diff < 0.01, "Color matrix1[0] ~ 1.2902 (SRATIONAL)");
    }

    tinydng_v2_document_destroy(ctx, doc);
}

static void test_cr2(tinydng_v2_context* ctx, tinydng_v2_error* err) {
    printf("Testing IMG_9747.CR2...\n");

    tinydng_v2_document* doc = NULL;
    tinydng_v2_status st = tinydng_v2_load_from_file_with_options(ctx, test_files[3], NULL, &doc, err);
    check(st == TINYDNG_V2_STATUS_OK, "Load succeeded");
    check(doc != NULL, "Document not NULL");

    if (!doc) return;

    check(tinydng_v2_document_image_count(doc) == 2, "Has 2 images");

    const tinydng_v2_image* img0 = tinydng_v2_document_image_at(doc, 0);
    check(img0 != NULL, "Image 0 retrieved");
    if (img0) {
        check(img0->width == 5184, "Image 0 width = 5184");
        check(img0->height == 3456, "Image 0 height = 3456");
        check(img0->compression == 7, "Image 0 compression = 7 (JPEG)");
    }

    const tinydng_v2_image* img1 = tinydng_v2_document_image_at(doc, 1);
    check(img1 != NULL, "Image 1 retrieved");
    if (img1) {
        check(img1->width == 670, "Image 1 width = 670");
        check(img1->height == 432, "Image 1 height = 432");
        check(img1->samples_per_pixel == 3, "Image 1 SPP = 3");
    }

    tinydng_v2_document_destroy(ctx, doc);
}

int main() {
    printf("=== V2 Loader Unit Tests ===\n\n");

    tinydng_v2_error err;
    tinydng_v2_error_clear(&err);

    tinydng_v2_context* ctx = tinydng_v2_context_create(NULL, &err);
    check(ctx != NULL, "Context created");

    if (!ctx) return EXIT_FAILURE;

    test_colorchart(ctx, &err);
    test_pixel3(ctx, &err);
    test_proraw(ctx, &err);
    test_cr2(ctx, &err);

    tinydng_v2_context_destroy(ctx);

    printf("\n=== Results ===\n");
    printf("Tests: %d, Passed: %d, Failed: %d\n", test_count, pass_count, fail_count);

    return (fail_count > 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}