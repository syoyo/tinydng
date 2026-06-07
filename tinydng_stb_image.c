/*
 * tinydng_stb_image.c - vendored stb_image implementation TU (baseline JPEG).
 * Only the JPEG decoder is compiled in; no stdio dependency.
 * SPDX-License-Identifier: MIT (wrapper) / public-domain (stb_image)
 */
#ifndef TINYDNG_NO_BASELINE_JPEG

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_ONLY_JPEG
#include "stb_image.h"

#endif /* TINYDNG_NO_BASELINE_JPEG */
