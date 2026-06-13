/* gen_v3_corpus.c - generate a diverse libFuzzer seed corpus for fuzz-v3.
 *
 * Produces small, *decodable* TIFF/DNG fixtures covering the parser/codec paths
 * that random mutation rarely reaches on its own: lossless JPEG, LZW, packed
 * 10/12/14-bit, horizontal + floating-point predictors, tiled layout, and
 * BigTIFF. Good seeds let coverage-guided fuzzing explore the neighborhood of
 * each path. Build/run:
 *
 *   cc -std=c11 -I.. fuzzer/gen_v3_corpus.c tinydng_api.c tinydng_io.c \
 *      tinydng_tiff.c tinydng_dng.c tinydng_codec.c tinydng_write.c \
 *      tinydng_miniz.c tinydng_stb_image.c tiny_dng_ljpeg92_v2.c -o gen
 *   ./gen <outdir>
 */
#include "../tinydng.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void le32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void le64(uint8_t *p, uint64_t v) {
  int i; for (i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static int g_n = 0;
static void dump(const char *dir, const char *name, const uint8_t *buf, size_t n) {
  char path[1024];
  FILE *f;
  snprintf(path, sizeof(path), "%s/seed_%02d_%s", dir, g_n++, name);
  f = fopen(path, "wb");
  if (!f) { fprintf(stderr, "cannot write %s\n", path); return; }
  fwrite(buf, 1, n, f);
  fclose(f);
  printf("  %s (%zu bytes)\n", path, n);
}

/* ---- classic little-endian strip TIFF, single component, single strip ---- */
typedef struct { uint16_t tag, type; uint32_t count, val; } ent;

static void emit_strip(const char *dir, const char *name, uint32_t w, uint32_t h,
                       uint16_t bps, uint16_t comp, uint16_t sfmt,
                       uint16_t predictor, uint16_t photo, const uint8_t *strip,
                       size_t striplen) {
  ent ex[4]; int nex = 0;
  uint8_t buf[8192]; uint8_t *e; int nent, i = 0;
  size_t strip_off = 8, after = strip_off + striplen;
  size_t ifd_off = after + (after & 1u);
  if (sfmt != 1u) { ex[nex].tag = 339; ex[nex].type = 3; ex[nex].count = 1; ex[nex].val = sfmt; nex++; }
  if (predictor) { ex[nex].tag = 317; ex[nex].type = 3; ex[nex].count = 1; ex[nex].val = predictor; nex++; }
  nent = 9 + nex;
  if (ifd_off + 2u + (size_t)nent * 12u + 4u > sizeof(buf)) { fprintf(stderr, "buf small\n"); return; }
  memset(buf, 0, sizeof(buf));
  buf[0] = 'I'; buf[1] = 'I'; le16(buf + 2, 42); le32(buf + 4, (uint32_t)ifd_off);
  if (strip) memcpy(buf + strip_off, strip, striplen);
  le16(buf + ifd_off, (uint16_t)nent);
  e = buf + ifd_off + 2u;
#define E(t, ty, c, v) do { le16(e+i*12,(uint16_t)(t)); le16(e+i*12+2,(uint16_t)(ty)); le32(e+i*12+4,(uint32_t)(c)); le32(e+i*12+8,(uint32_t)(v)); i++; } while (0)
  E(256, 4, 1, w); E(257, 4, 1, h); E(258, 3, 1, bps); E(259, 3, 1, comp);
  E(262, 3, 1, photo); E(273, 4, 1, (uint32_t)strip_off); E(277, 3, 1, 1);
  E(278, 4, 1, h); E(279, 4, 1, (uint32_t)striplen);
  { int k; for (k = 0; k < nex; k++) E(ex[k].tag, ex[k].type, ex[k].count, ex[k].val); }
#undef E
  le32(e + (size_t)nent * 12u, 0);
  dump(dir, name, buf, ifd_off + 2u + (size_t)nent * 12u + 4u);
}

/* ---- tiled uncompressed 8-bit mono: 4x4 image, 2x2 tiles (4 tiles) ---- */
static void emit_tiled(const char *dir) {
  uint8_t buf[4096]; uint8_t *e; int nent = 10, i = 0;
  size_t tiles_off = 8;       /* 4 tiles * 4 bytes = 16 */
  size_t toff_off = tiles_off + 16; /* 4 LONG tile offsets */
  size_t tbc_off = toff_off + 16;   /* 4 LONG tile byte counts */
  size_t ifd_off = tbc_off + 16;
  size_t total = ifd_off + 2u + (size_t)nent * 12u + 4u;
  int t;
  memset(buf, 0, sizeof(buf));
  buf[0] = 'I'; buf[1] = 'I'; le16(buf + 2, 42); le32(buf + 4, (uint32_t)ifd_off);
  for (t = 0; t < 16; t++) buf[tiles_off + t] = (uint8_t)(t * 16);
  for (t = 0; t < 4; t++) { le32(buf + toff_off + t * 4, (uint32_t)(tiles_off + t * 4)); le32(buf + tbc_off + t * 4, 4); }
  le16(buf + ifd_off, (uint16_t)nent);
  e = buf + ifd_off + 2u;
#define E(t, ty, c, v) do { le16(e+i*12,(uint16_t)(t)); le16(e+i*12+2,(uint16_t)(ty)); le32(e+i*12+4,(uint32_t)(c)); le32(e+i*12+8,(uint32_t)(v)); i++; } while (0)
  E(256, 4, 1, 4); E(257, 4, 1, 4); E(258, 3, 1, 8); E(259, 3, 1, 1);
  E(262, 3, 1, 1); E(277, 3, 1, 1);
  E(322, 3, 1, 2);                       /* TileWidth=2 */
  E(323, 3, 1, 2);                       /* TileLength=2 */
  E(324, 4, 4, (uint32_t)toff_off);      /* TileOffsets[4] */
  E(325, 4, 4, (uint32_t)tbc_off);       /* TileByteCounts[4] */
#undef E
  le32(e + (size_t)nent * 12u, 0);
  dump(dir, "tiled_u8.tif", buf, total);
}

/* ---- BigTIFF (version 43): uncompressed 8-bit mono 2x2, 20-byte entries ---- */
static void emit_bigtiff(const char *dir) {
  uint8_t buf[4096]; uint8_t *e; int nent = 9, i = 0;
  size_t strip_off = 16;            /* after the 16-byte BigTIFF header */
  size_t ifd_off = strip_off + 4u;  /* 2x2x8 = 4 bytes */
  size_t total = ifd_off + 8u + (size_t)nent * 20u + 8u;
  memset(buf, 0, sizeof(buf));
  buf[0] = 'I'; buf[1] = 'I'; le16(buf + 2, 43);    /* BigTIFF magic */
  le16(buf + 4, 8); le16(buf + 6, 0);               /* bytesize=8, reserved */
  le64(buf + 8, (uint64_t)ifd_off);                 /* 8-byte IFD offset */
  buf[strip_off] = 10; buf[strip_off+1] = 20; buf[strip_off+2] = 30; buf[strip_off+3] = 40;
  le64(buf + ifd_off, (uint64_t)nent);              /* 8-byte entry count */
  e = buf + ifd_off + 8u;
  /* BigTIFF entry: tag(2) type(2) count(8) value/offset(8) = 20 bytes */
#define BE(t, ty, c, v) do { le16(e+i*20,(uint16_t)(t)); le16(e+i*20+2,(uint16_t)(ty)); le64(e+i*20+4,(uint64_t)(c)); le64(e+i*20+12,(uint64_t)(v)); i++; } while (0)
  BE(256, 4, 1, 2); BE(257, 4, 1, 2); BE(258, 3, 1, 8); BE(259, 3, 1, 1);
  BE(262, 3, 1, 1); BE(273, 16, 1, (uint64_t)strip_off); /* LONG8 offset */
  BE(277, 3, 1, 1); BE(278, 4, 1, 2); BE(279, 16, 1, 4);
#undef BE
  le64(e + (size_t)nent * 20u, 0);                  /* next IFD = 0 */
  dump(dir, "bigtiff_u8.tif", buf, total);
}

/* ---- writer-produced DNGs (LJPEG / LZW / uncompressed RGB) ---- */
static void emit_written(const char *dir, tinydng_context *ctx) {
  uint16_t mono[32 * 32];
  uint8_t rgb[16 * 16 * 3];
  int i;
  tinydng_write_image wi;
  tinydng_write_options wo;
  uint8_t *out; size_t osz; tinydng_error err;
  for (i = 0; i < 32 * 32; i++) mono[i] = (uint16_t)((i * 37) & 0x3ff);
  for (i = 0; i < 16 * 16 * 3; i++) rgb[i] = (uint8_t)(i * 7);

  /* lossless JPEG DNG (16-bit) -- exercises the lj92 decode path */
  memset(&wi, 0, sizeof(wi)); memset(&wo, 0, sizeof(wo));
  wi.width = 32; wi.height = 32; wi.samples_per_pixel = 1; wi.bits_per_sample = 16;
  wi.data = (const uint8_t *)mono; wi.data_size = sizeof(mono);
  wo.compression = TINYDNG_COMPRESSION_NEW_JPEG; wo.as_dng = 1;
  if (tinydng_write_memory(ctx, &wi, &wo, &out, &osz, &err) == TINYDNG_OK) {
    dump(dir, "ljpeg16.dng", out, osz); tinydng_buffer_free(ctx, out);
  } else { fprintf(stderr, "ljpeg write: %s\n", err.message); }

  /* LZW TIFF (16-bit mono) */
  memset(&wo, 0, sizeof(wo)); wo.compression = TINYDNG_COMPRESSION_LZW;
  if (tinydng_write_memory(ctx, &wi, &wo, &out, &osz, &err) == TINYDNG_OK) {
    dump(dir, "lzw16.tif", out, osz); tinydng_buffer_free(ctx, out);
  }

  /* uncompressed RGB8 */
  memset(&wi, 0, sizeof(wi)); memset(&wo, 0, sizeof(wo));
  wi.width = 16; wi.height = 16; wi.samples_per_pixel = 3; wi.bits_per_sample = 8;
  wi.data = rgb; wi.data_size = sizeof(rgb);
  if (tinydng_write_memory(ctx, &wi, &wo, &out, &osz, &err) == TINYDNG_OK) {
    dump(dir, "rgb8.tif", out, osz); tinydng_buffer_free(ctx, out);
  }
}

int main(int argc, char **argv) {
  const char *dir = (argc > 1) ? argv[1] : ".";
  uint8_t pix16[8 * 8 * 2], pix32[8 * 8 * 4], pk12[8 * 8 * 2], pk14[8 * 8 * 2];
  int i;
  tinydng_context *ctx = tinydng_context_create(NULL, NULL);
  for (i = 0; i < (int)sizeof(pix16); i++) pix16[i] = (uint8_t)(i * 13);
  for (i = 0; i < (int)sizeof(pix32); i++) pix32[i] = (uint8_t)(i * 11);
  for (i = 0; i < (int)sizeof(pk12); i++) pk12[i] = (uint8_t)(i * 17);
  for (i = 0; i < (int)sizeof(pk14); i++) pk14[i] = (uint8_t)(i * 19);

  printf("generating v3 fuzz seeds into %s\n", dir);
  emit_strip(dir, "u16_mono.tif", 8, 8, 16, 1, 1, 0, 1, pix16, sizeof(pix16));
  emit_strip(dir, "f32.tif", 8, 8, 32, 1, 3 /*IEEEFP*/, 0, 1, pix32, sizeof(pix32));
  emit_strip(dir, "pred2_u16.tif", 8, 8, 16, 1, 1, 2 /*horizontal*/, 1, pix16, sizeof(pix16));
  emit_strip(dir, "pred3_f32.tif", 8, 8, 32, 1, 3 /*IEEEFP*/, 3 /*float pred*/, 1, pix32, sizeof(pix32));
  emit_strip(dir, "packed12.tif", 8, 8, 12, 1, 1, 0, 1, pk12, (size_t)((8 * 12 + 7) / 8) * 8);
  emit_strip(dir, "packed14.tif", 8, 8, 14, 1, 1, 0, 1, pk14, (size_t)((8 * 14 + 7) / 8) * 8);
  emit_tiled(dir);
  emit_bigtiff(dir);
  if (ctx) { emit_written(dir, ctx); tinydng_context_destroy(ctx); }
  printf("done: %d seeds\n", g_n);
  return 0;
}
