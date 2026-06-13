// One-time tool: extract the LJPEG (lossless JPEG / ITU T.81 Annex H) tile
// streams from a DNG and dump them to sandbox/lj92/testdata/tile_XXX.lj92,
// while printing per-stream characteristics (predictor, components, bitdepth,
// per-DHT max code length). Links tiny_dng_v2 + the v2 ljpeg decoder only to
// locate the compressed segments inside the DNG; the resulting raw files make
// the sandbox benchmark self-contained.
#if defined(__unix__) || defined(__APPLE__)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../../tiny_dng_v2.h"
#include "../../tiny_dng_ljpeg92_v2.h"

// Minimal standalone parse of an LJPEG stream header to report the predictor
// (from SOS) and the maximum Huffman code length per DHT (which drives the
// decoder's LUT size). Big-endian 16-bit helper.
static int be16(const uint8_t* p) { return (p[0] << 8) | p[1]; }

static void analyze_stream(const uint8_t* d, int n) {
  int ix = 0;
  // expect SOI
  int comps = -1, bits = -1, pred = -1;
  int dht_count = 0;
  while (ix + 4 <= n) {
    if (d[ix] != 0xFF) { ix++; continue; }
    int marker = d[ix + 1];
    if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
      ix += 2;
      continue;
    }
    if (marker == 0xD9) break;  // EOI
    if (ix + 4 > n) break;
    int len = be16(&d[ix + 2]);
    if (len < 2) break;
    const uint8_t* seg = &d[ix + 4];
    if (marker == 0xC3) {  // SOF3 lossless
      bits = seg[0];
      comps = seg[5];
    } else if (marker == 0xC4) {  // DHT
      // Tc/Th(1) then L1..L16
      int maxbits = 16;
      const uint8_t* L = &seg[1];
      while (maxbits > 0 && L[maxbits - 1] == 0) maxbits--;
      if (dht_count < 8) {
        printf("    DHT[%d] maxbits=%d  counts=[", dht_count, maxbits);
        for (int i = 0; i < 16; i++) printf("%d%s", L[i], i < 15 ? "," : "");
        printf("]\n");
      }
      dht_count++;
    } else if (marker == 0xDA) {  // SOS
      int ns = seg[0];
      pred = seg[1 + 2 * ns];  // Ss = predictor for lossless
      break;
    }
    ix += 2 + len;
  }
  printf("    => components=%d bitdepth=%d predictor=%d dht_count=%d\n", comps,
         bits, pred, dht_count);
}

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <dng_file> [out_dir]\n", argv[0]);
    return 1;
  }
  const char* out_dir = (argc >= 3) ? argv[2] : "testdata";
  mkdir(out_dir, 0755);  /* create output dir if missing (ignore EEXIST) */

  tinydng_v2_error err;
  tinydng_v2_context* ctx = tinydng_v2_context_create(NULL, &err);
  if (!ctx) {
    fprintf(stderr, "ctx create failed: %s\n", err.message);
    return 2;
  }
  tinydng_v2_load_options lopt;
  memset(&lopt, 0, sizeof(lopt));
  lopt.flags = TINYDNG_V2_LOAD_FLAG_PARSE_IMAGE_AS_IS;
  tinydng_v2_document* doc = NULL;
  if (tinydng_v2_load_from_file_with_options(ctx, argv[1], &lopt, &doc, &err) !=
      TINYDNG_V2_STATUS_OK) {
    fprintf(stderr, "load failed: %s\n", err.message);
    return 3;
  }

  size_t base_size = 0;
  const uint8_t* base = tinydng_v2_document_memory(doc, &base_size);
  size_t img_count = tinydng_v2_document_image_count(doc);

  // Find an LJPEG image: prefer index 1, then 0, 2, then scan.
  size_t prefs[] = {1, 0, 2};
  int found_idx = -1;
  for (size_t j = 0; j < 3 && found_idx < 0; j++) {
    size_t i = prefs[j];
    if (i >= img_count) continue;
    const tinydng_v2_image* img = tinydng_v2_document_image_at(doc, i);
    if ((img->compression == 6 || img->compression == 7) &&
        img->segment_count > 0) {
      tdng_lj92 probe = NULL;
      int w, h, b, c;
      if (tdng_lj92_open(&probe, base + img->segments[0].offset,
                         (int)img->segments[0].size, &w, &h, &b, &c) ==
              TDNG_LJ92_ERROR_NONE &&
          probe && w > 0) {
        tdng_lj92_close(probe);
        found_idx = (int)i;
      } else if (probe) {
        tdng_lj92_close(probe);
      }
    }
  }
  if (found_idx < 0) {
    fprintf(stderr, "no LJPEG image found\n");
    return 4;
  }

  const tinydng_v2_image* img = tinydng_v2_document_image_at(doc, (size_t)found_idx);
  printf("image_index=%d compression=%u %ux%u spp=%u segments=%zu\n", found_idx,
         img->compression, img->width, img->height, img->samples_per_pixel,
         img->segment_count);

  uint64_t total_bytes = 0;
  for (size_t k = 0; k < img->segment_count; k++) {
    const uint8_t* sp = base + img->segments[k].offset;
    int ss = (int)img->segments[k].size;
    char path[512];
    snprintf(path, sizeof(path), "%s/tile_%03zu.lj92", out_dir, k);
    FILE* f = fopen(path, "wb");
    if (!f) {
      fprintf(stderr, "cannot open %s\n", path);
      return 5;
    }
    fwrite(sp, 1, (size_t)ss, f);
    fclose(f);
    total_bytes += (uint64_t)ss;
    if (k < 4 || k == img->segment_count - 1) {
      printf("  tile_%03zu.lj92  %d bytes\n", k, ss);
      analyze_stream(sp, ss);
    }
  }
  printf("wrote %zu streams, %llu total bytes to %s/\n", img->segment_count,
         (unsigned long long)total_bytes, out_dir);

  tinydng_v2_document_destroy(ctx, doc);
  tinydng_v2_context_destroy(ctx);
  return 0;
}
