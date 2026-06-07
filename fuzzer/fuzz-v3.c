/*
 * fuzz-v3.c - libFuzzer harness for the clean-room tinydng parser.
 *
 *   clang -std=c11 -g -O1 -fsanitize=address,undefined,fuzzer -I.. \
 *     fuzz-v3.c ../tinydng_api.c ../tinydng_io.c ../tinydng_tiff.c \
 *     ../tinydng_dng.c ../tinydng_codec.c ../tiny_dng_ljpeg92_v2.c -o fuzz-v3
 */
#include "../tinydng.h"

#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  tinydng_config cfg;
  tinydng_context *ctx;
  tinydng_document *doc = NULL;
  tinydng_error err;
  tinydng_open_options opts;

  memset(&cfg, 0, sizeof(cfg));
  cfg.memory_cap_bytes = 256u * 1024u * 1024u; /* bound OOM */
  cfg.max_images = 64;

  ctx = tinydng_context_create(&cfg, &err);
  if (!ctx) {
    return 0;
  }

  memset(&opts, 0, sizeof(opts));
  /* Toggle SubIFD descent on a bit of the input for coverage. */
  if (size > 0 && (data[0] & 1u)) {
    opts.flags |= TINYDNG_OPEN_PARSE_SUBIFDS;
  }

  if (tinydng_open_memory(ctx, data, size, &opts, &doc, &err) == TINYDNG_OK) {
    size_t i, n = tinydng_image_count(doc);
    for (i = 0; i < n; i++) {
      const tinydng_image_info *img = tinydng_image_get(doc, i);
      size_t s, sc = tinydng_image_segment_count(img);
      tinydng_pixels px;
      tinydng_error derr;
      for (s = 0; s < sc; s++) {
        tinydng_segment seg;
        (void)tinydng_image_segment(img, s, &seg);
      }
      /* Exercise the decode path (bounded by the context memory cap). */
      if (tinydng_decode_image(ctx, doc, i, NULL, &px, &derr) == TINYDNG_OK) {
        tinydng_pixels_free(ctx, &px);
      }
    }
    tinydng_document_destroy(ctx, doc);
  }
  tinydng_context_destroy(ctx);
  return 0;
}
