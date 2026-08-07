/*
 * tiny_dng_j2k_enc.h - clean-room C11 JPEG 2000 codestream encoder for
 * HTJ2K (JPEG 2000 Part 15 / ITU-T T.814) streams.
 *
 * Produces a single-tile HTJ2K codestream (RPCL progression, one layer,
 * whole-image precincts, codeblock size 2^cb_log_w x 2^cb_log_h) built on
 * the clean-room HT block encoder in tiny_dng_htj2k.c. Supports reversible
 * 5/3 (lossless) and irreversible 9/7 (lossy) wavelets, the forward
 * RCT/ICT colour transforms, and the DC level shift.
 *
 * Output is decodable by this library's own decoder and by OpenJPH.
 */

#ifndef TINY_DNG_J2K_ENC_H
#define TINY_DNG_J2K_ENC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return codes match the tiny_dng_j2k.h enum: TDNG_J2K_OK = 0 and the
   negative TDNG_J2K_ERROR_* values; include tiny_dng_j2k.h for them. */

/* Return codes match the tiny_dng_j2k.h enum (TDNG_J2K_OK = 0 and the
   negative TDNG_J2K_ERROR_* values). These aliases are only used when the
   decoder header (which defines the canonical enum) is not included. */

/*
 * Encode an interleaved image as a single-tile HTJ2K codestream.
 *
 *   pixels    - interleaved samples, num_comps per pixel. Each sample is
 *               `bits[c]` bits wide (1..31), stored in the low bits of an
 *               int32_t (signed samples in two's complement).
 *   width/height - image dimensions (>0).
 *   num_comps - number of components (1..8).
 *   bits[]    - bit depth per component (1..31).
 *   is_signed[] - 1 if a component is signed.
 *   num_decomps - number of DWT decomposition levels (0..32).
 *   reversible - 1 = reversible 5/3 (lossless), 0 = irreversible 9/7.
 *   mct       - 1 to apply the colour transform to the first three
 *               components (reversible RCT / irreversible ICT).
 *   cb_log_w/cb_log_h - codeblock log2 sizes (2..8; OpenJPH default 6,6).
 *   out       - receives the allocated codestream buffer (caller frees).
 *   out_cap   - receives the buffer capacity.
 *   out_len   - receives the codestream length in bytes.
 *
 * Returns TDNG_J2K_OK on success.
 */
int tdng_j2k_encode(const int32_t *pixels, int width, int height,
                    int num_comps, const int bits[8], const int is_signed[8],
                    int num_decomps, int reversible, int mct, int cb_log_w,
                    int cb_log_h, float qstep, uint8_t **out, size_t *out_cap,
                    size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* TINY_DNG_J2K_ENC_H */
