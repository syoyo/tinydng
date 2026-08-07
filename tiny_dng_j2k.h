/*
 * tiny_dng_j2k.h - clean-room C11 JPEG 2000 codestream decoder for HTJ2K
 * (ISO/IEC 15444-15 / ITU-T T.814) codestreams, built on the block codec in
 * tiny_dng_htj2k.c.
 *
 * Parses the J2K main/tile-part headers (SOC/SIZ/COD/COC/QCD/QCC/SOT/SOD),
 * extracts codeblock data from the packet streams, decodes with the HT block
 * codec, and applies the inverse wavelet transform (reversible 5/3 or
 * irreversible 9/7) plus the colour transform (RCT/ICT).
 *
 * Scope: single tile-part per tile, one packet layer, precincts defaulting
 * to the whole resolution (OpenJPH encoder defaults). Progression orders
 * RPCL, LRCP, RLCP, PCRL, CPRL are handled.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef TINY_DNG_J2K_H
#define TINY_DNG_J2K_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  TDNG_J2K_OK = 0,
  TDNG_J2K_ERROR_CORRUPT = -1,
  TDNG_J2K_ERROR_UNSUPPORTED = -2,
  TDNG_J2K_ERROR_MEMORY = -3,
  TDNG_J2K_ERROR_PARAM = -4
};

typedef struct tdng_j2k tdng_j2k;

/*
 * Opens a JPEG 2000 codestream and parses its headers. On success the
 * geometry is reported through the out parameters. The `data` buffer must
 * remain valid until tdng_j2k_close.
 *
 *  image_w/h     - image dimensions in samples
 *  num_comps     - number of components
 *  bitdepth_out  - per-component bit depths (>= num_comps entries)
 *  tile_w/h      - nominal tile size
 *  num_tiles_x/y - tile grid size
 */
int tdng_j2k_open(tdng_j2k **j2k, const uint8_t *data, size_t size,
                  uint32_t *image_w, uint32_t *image_h, uint32_t *num_comps,
                  uint32_t *bitdepth_out, uint32_t *tile_w, uint32_t *tile_h,
                  uint32_t *num_tiles_x, uint32_t *num_tiles_y);

/*
 * Decodes one tile into `out` as interleaved samples (num_comps channels,
 * row-major, tile_w*tile_h samples per channel). Samples are in the native
 * signed integer domain (two's complement). Caller must provide at least
 * tile_w * tile_h * num_comps * 4 bytes.
 */
int tdng_j2k_decode_tile(tdng_j2k *j2k, uint32_t tile_idx, int32_t *out);

/* Whether the codestream uses the irreversible (9/7) transform. */
int tdng_j2k_is_lossy(tdng_j2k *j2k);

void tdng_j2k_close(tdng_j2k *j2k);

#ifdef __cplusplus
}
#endif

#endif /* TINY_DNG_J2K_H */
