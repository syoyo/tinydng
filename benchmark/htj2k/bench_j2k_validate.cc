// End-to-end validation of tiny_dng_j2k against OpenJPH's expand output.
//
// Decodes a .j2c with our codestream layer and compares against the PGM/PPM
// output of ojph_expand (via the codestream API, avoiding file I/O).
//
// Build:  make -C benchmark/htj2k bench_j2k_validate
// Run:    benchmark/htj2k/bench_j2k_validate <file.j2c> <width> <height> <comps>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "ojph_codestream.h"
#include "ojph_file.h"
#include "ojph_mem.h"
#include "ojph_params.h"
#include "tiny_dng_j2k.h"

using namespace ojph;

static int load_file(const char *path, std::vector<uint8_t> &out) {
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  out.resize(sz);
  if (sz > 0 && fread(out.data(), 1, sz, f) != (size_t)sz) { fclose(f); return 0; }
  fclose(f);
  return 1;
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s <file.j2c>\n", argv[0]); return 1; }
  std::vector<uint8_t> data;
  if (!load_file(argv[1], data)) { fprintf(stderr, "load failed\n"); return 2; }

  // reference: OpenJPH full decode
  std::vector<int32_t> ref;
  uint32_t iw = 0, ih = 0, ic = 0;
  {
    mem_infile infile;
    infile.open(data.data(), data.size());
    codestream cs;
    cs.read_headers(&infile);
    param_siz siz = cs.access_siz();
    iw = siz.get_image_extent().x - siz.get_image_offset().x;
    ih = siz.get_image_extent().y - siz.get_image_offset().y;
    ic = siz.get_num_components();
    ref.resize((size_t)iw * ih * ic);
    cs.set_planar(false);
    cs.create();
    uint32_t height = siz.get_recon_height(0);
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t c = 0; c < ic; ++c) {
        ui32 cn;
        line_buf *l = cs.pull(cn);
        if (!l) return 3;
        uint32_t cw = siz.get_recon_width(c);
        for (uint32_t x = 0; x < cw; ++x)
          ref[((size_t)y * iw + x) * ic + c] = l->i32[x];
      }
    }
    cs.close();
  }

  // our decode
  tdng_j2k *j2k = NULL;
  uint32_t o_w, o_h, ncomp, bits[16], tw, th, ntx, nty;
  int r = tdng_j2k_open(&j2k, data.data(), data.size(), &o_w, &o_h, &ncomp,
                        bits, &tw, &th, &ntx, &nty);
  if (r != TDNG_J2K_OK) { fprintf(stderr, "open failed r=%d\n", r); return 4; }
  if (o_w != iw || o_h != ih || ncomp != ic) {
    fprintf(stderr, "geometry mismatch: %ux%u x%u vs %ux%u x%u\n", o_w, o_h,
            ncomp, iw, ih, ic);
    return 5;
  }

  std::vector<int32_t> mine((size_t)tw * th * ncomp);
  r = tdng_j2k_decode_tile(j2k, 0, mine.data());
  if (r != TDNG_J2K_OK) { fprintf(stderr, "tile decode failed r=%d\n", r); return 6; }
  tdng_j2k_close(j2k);

  uint64_t mism = 0;
  int64_t maxdiff = 0;
  for (uint32_t y = 0; y < ih; ++y) {
    for (uint32_t x = 0; x < iw; ++x) {
      for (uint32_t c = 0; c < ic; ++c) {
        int32_t a = ref[((size_t)y * iw + x) * ic + c];
        int32_t b = mine[((size_t)y * tw + x) * ic + c];
        int64_t d = (int64_t)a - b;
        if (d < 0) d = -d;
        if (d > maxdiff) maxdiff = d;
        if (a != b) {
          if (mism < 8)
            fprintf(stderr, "  mismatch y=%u x=%u c=%u ref=%d mine=%d\n", y, x,
                    c, a, b);
          mism++;
        }
      }
    }
  }
  if (getenv("J2K_PX")) {
    for (uint32_t x = 0; x < 8; ++x)
      printf("px[%u] ref=%d mine=%d\n", x, ref[x * ic], mine[x * ic]);
  }
  if (getenv("J2K_DUMP_ROW")) {
    for (uint32_t x = 0; x < 32; ++x) {
      int32_t a = ref[x * ic], b = mine[x * ic];
      printf("px[%u] ref=%d mine=%d\n", x, a, b);
    }
  }
  {
    uint32_t wy=0, wx=0;
    for (uint32_t y=0;y<ih;y++) for (uint32_t x=0;x<iw;x++){
      int32_t a=ref[((size_t)y*iw+x)*ic], b=mine[((size_t)y*tw+x)*ic];
      int64_t d = (int64_t)a-b; if (d<0) d=-d;
      if (d == maxdiff){ wy=y; wx=x; }
    }
    if (maxdiff > 0) {
      printf("worst at y=%u x=%u\n", wy, wx);
      for (uint32_t yy2 = (wy>2?wy-2:0); yy2 < wy+3 && yy2 < ih; ++yy2) {
        for (uint32_t xx2 = (wx>2?wx-2:0); xx2 < wx+3 && xx2 < iw; ++xx2) {
          int32_t a=ref[((size_t)yy2*iw+xx2)*ic], b=mine[((size_t)yy2*tw+xx2)*ic];
          if (a != b) printf("  (%u,%u) ref=%d mine=%d\n", yy2, xx2, a, b);
        }
      }
    }
  }
  printf("image=%ux%u comps=%u mismatches=%llu maxdiff=%lld\n", iw, ih, ic,
         (unsigned long long)mism, (long long)maxdiff);
  return mism ? 1 : 0;
}
