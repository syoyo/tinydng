/* lj92.h - clean-room pure-C11 Lossless JPEG (ITU-T T.81 Annex H, "LJPEG-1992")
 * decoder and encoder.
 *
 * This is a fresh implementation focused on throughput. The entropy decoder
 * uses a single L1-resident Huffman LUT with a branchless, single-shift inner
 * loop; reconstruction has SSE2/SSE4.1/AVX2 kernels selected at runtime via
 * CPUID (and gated at compile time by the LJ92_HAVE_* macros below).
 *
 * License: same spirit as the surrounding tinydng project (MIT-like / public).
 */
#ifndef LJ92_H_
#define LJ92_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum lj92_error {
  LJ92_OK = 0,
  LJ92_ERR_CORRUPT = -1,
  LJ92_ERR_NO_MEMORY = -2,
  LJ92_ERR_BAD_HANDLE = -3,
  LJ92_ERR_TOO_WIDE = -4,
  LJ92_ERR_NOT_LOSSLESS = -5
} lj92_error;

/* ---- Decoder -------------------------------------------------------------
 * Usage:
 *   lj92_dec d;
 *   lj92_decode_open(&d, data, len, &w, &h, &bits, &comps);
 *   lj92_decode_run(d, out, w*comps, 0, NULL, 0);
 *   lj92_decode_close(d);
 */
typedef struct lj92_decoder* lj92_dec;

/* Parse the headers of an LJPEG stream and report geometry. */
int lj92_decode_open(lj92_dec* dec, const uint8_t* data, int datalen, int* width,
                     int* height, int* bitdepth, int* components);

/* Decode the entropy-coded scan into `target` (interleaved samples).
 *   write_stride : samples per output row before skipping (typ. width*comps).
 *   skip         : samples to skip after each row (tiled output); 0 if packed.
 *   linearize    : optional sample LUT applied to each reconstructed value.
 *   linearize_len: length of the linearize table. */
int lj92_decode_run(lj92_dec dec, uint16_t* target, int write_stride, int skip,
                    const uint16_t* linearize, int linearize_len);

void lj92_decode_close(lj92_dec dec);

/* ---- Encoder -------------------------------------------------------------
 * Encode `image` (interleaved 16-bit samples) into a single-scan LJPEG stream
 * with one Huffman table per component. The caller owns *encoded (free()).
 *   bitdepth   : 2..16
 *   components : 1..4
 *   predictor  : 1 (left), 2 (above), 4, 5, 6, or 7 (avg)
 *   read_len   : samples read per row before skipping (typ. width*components)
 *   skip       : samples skipped after each row (tiled input)
 *   delinearize: optional inverse sample LUT applied before encoding */
int lj92_encode(const uint16_t* image, int width, int height, int bitdepth,
                int components, int predictor, int read_len, int skip,
                const uint16_t* delinearize, int delinearize_len,
                uint8_t** encoded, int* encoded_len);

/* ---- SIMD path selection / introspection --------------------------------
 * Names of the kernel families. lj92_simd_name() returns the path that runtime
 * dispatch selected (after CPUID); lj92_simd_force() pins a path for
 * benchmarking (returns 0 on success, -1 if that path was not compiled in or
 * is unsupported by the CPU). */
typedef enum lj92_simd {
  LJ92_SIMD_AUTO = 0,
  LJ92_SIMD_SCALAR = 1,
  LJ92_SIMD_SSE2 = 2,
  LJ92_SIMD_SSE41 = 3,
  LJ92_SIMD_AVX2 = 4
} lj92_simd;

const char* lj92_simd_name(void);
int lj92_simd_force(lj92_simd which);

#ifdef __cplusplus
}
#endif

#endif /* LJ92_H_ */
