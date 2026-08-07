/*
 * tiny_dng_htj2k.h - clean-room C11 HTJ2K block coder (JPEG 2000 Part 15,
 * ITU-T T.814 high-throughput block coding engine).
 *
 * This module implements the HT block decoder and encoder at the codeblock
 * level: coded cleanup / significance-propagation / magnitude-refinement
 * passes in, decoded coefficients out (and vice versa). It is a from-scratch
 * implementation written to interoperate with other HTJ2K codecs (verified
 * against OpenJPH), including both the reversible (5/3) and irreversible
 * (9/7) coding styles at the block layer (the codeblock payload is
 * transform-agnostic).
 *
 * Coefficient domain (matching JPEG 2000 conventions used by OpenJPH and
 * the standard):
 *   - 32-bit API: coefficients are sign-magnitude, with the sign in bit 31
 *     and the magnitude shifted left by (31 - K_max); `missing_msbs` is
 *     K_max - 1 (the number of leading zero bit-planes).
 *   - 64-bit API: sign in bit 63, magnitude shifted left by (63 - K_max).
 *
 * The decoded magnitudes carry a half-bin ("0.5") so that refinement passes
 * can be applied; see tdng_htj2k_decode_codeblock32 for details.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef TINY_DNG_HTJ2K_H
#define TINY_DNG_HTJ2K_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
enum {
  TDNG_HTJ2K_OK = 0,
  TDNG_HTJ2K_ERROR_CORRUPT = -1, /* malformed / truncated bitstream        */
  TDNG_HTJ2K_ERROR_UNSUPPORTED = -2, /* parameter combination unsupported */
  TDNG_HTJ2K_ERROR_MEMORY = -3   /* allocation failure                    */
};

/* Decoder context: reusable scratch buffers (avoid per-block allocation).
   Create once per thread / per tile and reuse across codeblocks. */
typedef struct tdng_htj2k_ctx {
  void *scratch;          /* internal scratch buffer */
  size_t scratch_cap;     /* capacity in bytes */
  void *v_n;              /* internal v_n buffer */
  size_t v_n_cap;         /* capacity in bytes */
  void *magsgn;           /* packed MagSgn bit buffer */
  size_t magsgn_cap;      /* capacity in bytes */
} tdng_htj2k_ctx;

int tdng_htj2k_ctx_init(tdng_htj2k_ctx *ctx);
void tdng_htj2k_ctx_free(tdng_htj2k_ctx *ctx);

/*
 * Decodes one HTJ2K codeblock using a caller-provided context.
 * Same parameters as tdng_htj2k_decode_codeblock32. The context must not be
 * shared between concurrent decodes.
 */
int tdng_htj2k_decode_codeblock32_ctx(tdng_htj2k_ctx *ctx,
                                      const uint8_t *coded_data,
                                      uint32_t *decoded_data,
                                      uint32_t missing_msbs,
                                      uint32_t num_passes, uint32_t lengths1,
                                      uint32_t lengths2, uint32_t width,
                                      uint32_t height, uint32_t stride,
                                      int stripe_causal);

int tdng_htj2k_decode_codeblock64_ctx(tdng_htj2k_ctx *ctx,
                                      const uint8_t *coded_data,
                                      uint64_t *decoded_data,
                                      uint32_t missing_msbs,
                                      uint32_t num_passes, uint32_t lengths1,
                                      uint32_t lengths2, uint32_t width,
                                      uint32_t height, uint32_t stride,
                                      int stripe_causal);

/*
 * Decodes one HTJ2K codeblock.
 *
 *   coded_data  - cleanup pass bytes; when num_passes > 1 the SPP/MRP bytes
 *                 immediately follow (their length is `lengths2`).
 *   decoded_data- output coefficient buffer (sign-magnitude domain).
 *   missing_msbs- number of missing (leading-zero) most-significant bits.
 *   num_passes  - 1: CUP only, 2: CUP+SPP, 3: CUP+SPP+MRP.
 *   lengths1    - cleanup pass length in bytes.
 *   lengths2    - SPP+MRP length in bytes (0 unless num_passes > 1).
 *   width/height- codeblock dimensions in samples (>=1, <= 1024 each).
 *   stride      - row stride of decoded_data in samples.
 *   stripe_causal - true when stripe-causal coding was used by the encoder.
 *
 * Returns TDNG_HTJ2K_OK on success, or an error code.
 */
int tdng_htj2k_decode_codeblock32(const uint8_t *coded_data,
                                  uint32_t *decoded_data,
                                  uint32_t missing_msbs, uint32_t num_passes,
                                  uint32_t lengths1, uint32_t lengths2,
                                  uint32_t width, uint32_t height,
                                  uint32_t stride, int stripe_causal);

int tdng_htj2k_decode_codeblock64(const uint8_t *coded_data,
                                  uint64_t *decoded_data,
                                  uint32_t missing_msbs, uint32_t num_passes,
                                  uint32_t lengths1, uint32_t lengths2,
                                  uint32_t width, uint32_t height,
                                  uint32_t stride, int stripe_causal);

/*
 * Encodes one HTJ2K codeblock (cleanup pass only; OpenJPH-compatible output
 * so streams decode with any HTJ2K decoder).
 *
 *   buf        - input coefficients (sign-magnitude domain as above).
 *   missing_msbs - number of missing MSBs (= K_max - 1).
 *   width/height/stride - codeblock geometry.
 *   lengths    - receives the cleanup pass length (lengths[0]).
 *   out        - caller-provided output buffer (>= *out_cap bytes) OR NULL
 *                to have the function allocate. On return *out holds the
 *                encoded bytes and *out_cap its capacity.
 *
 * Returns TDNG_HTJ2K_OK on success. The encoded cleanup pass is stored in
 * `*out` with length `lengths[0]`.
 */
int tdng_htj2k_encode_codeblock32(const uint32_t *buf, uint32_t missing_msbs,
                                  uint32_t width, uint32_t height,
                                  uint32_t stride, uint32_t *lengths,
                                  uint8_t **out, size_t *out_cap);

int tdng_htj2k_encode_codeblock64(const uint64_t *buf, uint32_t missing_msbs,
                                  uint32_t width, uint32_t height,
                                  uint32_t stride, uint32_t *lengths,
                                  uint8_t **out, size_t *out_cap);

#ifdef __cplusplus
}
#endif

#endif /* TINY_DNG_HTJ2K_H */
