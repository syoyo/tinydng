/*
 * tinydng_miniz.c - vendored miniz implementation TU (Deflate/ZIP inflate).
 * SPDX-License-Identifier: MIT
 */
#ifndef TINYDNG_NO_ZIP
/* Force miniz to use byte-wise, alignment-safe integer reads. Its default
 * MZ_READ_LE32 does a misaligned 32-bit load (benign on x86 hardware, but
 * undefined behavior that aborts a strict UBSan build); the byte-wise form is
 * correct everywhere at a negligible cost for our (decode-only) use. */
#define MINIZ_USE_UNALIGNED_LOADS_AND_STORES 0
#include "miniz.c"
#endif
