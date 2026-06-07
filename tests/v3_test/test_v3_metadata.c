/* Metadata tests: a tracked sample DNG (colorchart.dng) and an in-memory
   OpcodeList fixture exercising GainMap/WarpRectilinear/FixVignetteRadial.
   argv[1] = repository source dir. */
#include "td_test_util.h"

static int test_colorchart(tinydng_context *ctx, const char *root) {
  char path[1024];
  unsigned char *blob;
  size_t n = 0;
  tinydng_error err;
  tinydng_document *doc = NULL;
  const tinydng_image_info *im;
  int rc = 0;
  snprintf(path, sizeof(path), "%s/colorchart.dng", root);
  blob = tdt_slurp(path, &n);
  if (!blob) {
    CHECK(0, "colorchart.dng missing at %s", path);
    return 1;
  }
  if (tinydng_open_memory(ctx, blob, n, NULL, &doc, &err) != TINYDNG_OK) {
    CHECK(0, "colorchart open: %s", err.message);
    free(blob);
    return 1;
  }
  im = tinydng_image_get(doc, 0);
  CHECK(im && im->width == 1888 && im->height == 1182, "colorchart dims");
  CHECK(im && im->bits_per_sample == 14, "colorchart bps=14");
  CHECK(im && im->raw.has_dng_version && im->raw.dng_version[0] == 1, "dngver");
  CHECK(im && im->raw.black_level_present && im->raw.black_level[0] == 2056,
        "black=2056");
  CHECK(im && im->raw.white_level_present && im->raw.white_level[0] == 15000,
        "white=15000");
  CHECK(im && im->cfa.present && im->cfa.pattern_dim[0] == 2 &&
            im->cfa.pattern_size == 4,
        "cfa 2x2");
  CHECK(im && im->raw.color_matrix_present, "color matrix present");
  {
    const tinydng_exif *ex = tinydng_document_exif(doc);
    CHECK(ex && ex->make && strcmp(ex->make, "Canon") == 0, "make=Canon");
  }
  if (!g_fail) {
    printf("  colorchart.dng metadata OK (1888x1182, 14-bit, CFA, matrices)\n");
  }
  tinydng_document_destroy(ctx, doc);
  free(blob);
  return rc;
}

/* Build an in-memory TIFF with an OpcodeList2 tag holding GainMap (9),
   WarpRectilinear (1) and FixVignetteRadial (3). */
static int test_opcodes(tinydng_context *ctx) {
  uint8_t op[512];
  size_t p = 0;
  size_t i;
  uint8_t img[4] = {10, 20, 30, 40};
  uint8_t *buf, *e;
  size_t strip_off = 8, op_off, ifd_off, total, oplen;
  const int NENT = 10;
  int n = 0;
  tinydng_error err;
  tinydng_document *doc = NULL;
  const tinydng_image_info *im;
  int rc = 0;

#define PU32(v) do { tdt_pu32(op + p, (uint32_t)(v), 1); p += 4; } while (0)
#define PF64(v) do { tdt_pf64be(op + p, (double)(v)); p += 8; } while (0)
#define PF32(v) do { tdt_pf32be(op + p, (float)(v)); p += 4; } while (0)
  PU32(3); /* num opcodes */
  /* GainMap (id 9): 10 u32, 4 doubles, 1 u32, then v*h*planes floats */
  PU32(9); PU32(0x01030000); PU32(0); PU32(92);
  PU32(1); PU32(2); PU32(3); PU32(4);   /* top,left,bottom,right */
  PU32(0); PU32(1); PU32(1); PU32(1);   /* plane,planes,row,col */
  PU32(2); PU32(2);                     /* map_points_v, map_points_h */
  PF64(0.1); PF64(0.2); PF64(0.3); PF64(0.4); /* spacing/origin */
  PU32(1);                              /* map_planes */
  PF32(1.0f); PF32(1.1f); PF32(1.2f); PF32(1.3f);
  /* WarpRectilinear (id 1): N, N*6 doubles, 2 doubles center */
  PU32(1); PU32(0x01030000); PU32(0); PU32(68);
  PU32(1);
  PF64(1.0); PF64(0.01); PF64(0.02); PF64(0.03); PF64(0.0); PF64(0.0);
  PF64(0.5); PF64(0.55);
  /* FixVignetteRadial (id 3): 5 doubles k, 2 doubles center */
  PU32(3); PU32(0x01030000); PU32(0); PU32(56);
  PF64(1.0); PF64(0.1); PF64(0.2); PF64(0.3); PF64(0.4);
  PF64(0.5); PF64(0.5);
#undef PU32
#undef PF64
#undef PF32
  oplen = p;

  op_off = strip_off + sizeof(img);
  ifd_off = op_off + oplen + (oplen & 1u);
  total = ifd_off + 2u + (size_t)NENT * 12u + 4u;
  buf = (uint8_t *)calloc(1, total);
  buf[0] = 'I';
  buf[1] = 'I';
  tdt_pu16(buf + 2, 42, 0);
  tdt_pu32(buf + 4, (uint32_t)ifd_off, 0);
  memcpy(buf + strip_off, img, sizeof(img));
  memcpy(buf + op_off, op, oplen);
  tdt_pu16(buf + ifd_off, (uint16_t)NENT, 0);
  e = buf + ifd_off + 2u;
#define ENT(tag, type, cnt, val)        \
  do {                                  \
    tdt_pu16(e + n * 12 + 0, (tag), 0); \
    tdt_pu16(e + n * 12 + 2, (type), 0);\
    tdt_pu32(e + n * 12 + 4, (cnt), 0); \
    tdt_pu32(e + n * 12 + 8, (val), 0); \
    n++;                                \
  } while (0)
  ENT(256, 4, 1, 2);
  ENT(257, 4, 1, 2);
  ENT(258, 3, 1, 8);
  ENT(259, 3, 1, 1);
  ENT(262, 3, 1, 1);
  ENT(273, 4, 1, (uint32_t)strip_off);
  ENT(277, 3, 1, 1);
  ENT(278, 4, 1, 2);
  ENT(279, 4, 1, (uint32_t)sizeof(img));
  ENT(51009, 7, (uint32_t)oplen, (uint32_t)op_off); /* OpcodeList2 */
#undef ENT
  tdt_pu32(buf + ifd_off + 2u + (size_t)NENT * 12u, 0, 0);

  if (tinydng_open_memory(ctx, buf, total, NULL, &doc, &err) != TINYDNG_OK) {
    CHECK(0, "opcode tiff open: %s", err.message);
    free(buf);
    return 1;
  }
  im = tinydng_image_get(doc, 0);
  CHECK(im && im->raw.opcode_count == 3, "3 opcodes captured");
  CHECK(im && im->raw.gainmap_count == 1, "1 gainmap");
  CHECK(im && im->raw.warp_count == 1, "1 warp");
  CHECK(im && im->raw.vignette_count == 1, "1 vignette");
  if (im && im->raw.opcode_count == 3) {
    CHECK(im->raw.opcodes[0].id == 9 && im->raw.opcodes[0].list == 2,
          "opcode[0]=gainmap list2");
    CHECK(im->raw.opcodes[1].id == 1, "opcode[1]=warp");
    CHECK(im->raw.opcodes[2].id == 3, "opcode[2]=vignette");
  }
  if (im && im->raw.gainmap_count == 1) {
    const tinydng_gainmap *gm = &im->raw.gainmaps[0];
    CHECK(gm->top == 1 && gm->right == 4 && gm->map_points_v == 2 &&
              gm->map_points_h == 2 && gm->map_planes == 1 &&
              gm->pixel_count == 4,
          "gainmap fields");
  }
  if (im && im->raw.warp_count == 1) {
    const tinydng_warp_rectilinear *wp = &im->raw.warps[0];
    CHECK(wp->plane_count == 1 && wp->coeff[0][0] > 0.999 &&
              wp->coeff[0][0] < 1.001 && wp->center[0] > 0.499 &&
              wp->center[0] < 0.501,
          "warp fields");
  }
  if (im && im->raw.vignette_count == 1) {
    const tinydng_vignette_radial *vg = &im->raw.vignettes[0];
    CHECK(vg->k[0] > 0.999 && vg->k[0] < 1.001 && vg->k[4] > 0.399 &&
              vg->k[4] < 0.401,
          "vignette fields");
  }
  if (!g_fail) {
    printf("  in-memory OpcodeList: gainmap + warp + vignette parsed OK\n");
  }
  (void)i;
  tinydng_document_destroy(ctx, doc);
  free(buf);
  return rc;
}

int main(int argc, char **argv) {
  tinydng_context *ctx = tinydng_context_create(NULL, NULL);
  const char *root = (argc > 1) ? argv[1] : ".";
  if (!ctx) {
    return 1;
  }
  printf("== metadata: colorchart.dng ==\n");
  test_colorchart(ctx, root);
  printf("== metadata: opcode list ==\n");
  test_opcodes(ctx);
  if (tinydng_context_memory_used(ctx) != 0u) {
    fprintf(stderr, "  FAIL: leak, used=%zu\n",
            tinydng_context_memory_used(ctx));
    g_fail = 1;
  }
  tinydng_context_destroy(ctx);
  printf(g_fail ? "METADATA: FAILURES\n" : "METADATA: ALL PASS\n");
  return g_fail;
}
