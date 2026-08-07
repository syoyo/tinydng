// Library-level HTJ2K decode benchmark against the OpenJPH codestream API.
//
// Loads a real .j2c/.jph codestream produced by ojph_compress, decodes it
// through ojph::codestream (headers + create + pull per line), and reports
// MPix/s. Excludes file I/O (codestream is in memory). This is the number
// our clean-room tinydng HTJ2K decoder is measured against.
//
// Build:  make -C benchmark/htj2k
// Run:    benchmark/htj2k/bench_ojph_expand <file.j2c> [iterations]
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "ojph_codestream.h"
#include "ojph_file.h"
#include "ojph_mem.h"
#include "ojph_params.h"

using namespace ojph;

static double now_s() {
  auto t = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(t.time_since_epoch()).count();
}

static size_t load_file(const char *path, std::vector<uint8_t> &out) {
  FILE *f = fopen(path, "rb");
  if (!f) { perror(path); return 0; }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  out.resize((size_t)sz);
  if (sz > 0 && fread(out.data(), 1, (size_t)sz, f) != (size_t)sz) {
    fclose(f);
    return 0;
  }
  fclose(f);
  return (size_t)sz;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <file.j2c> [iterations]\n", argv[0]);
    return 1;
  }
  int iters = argc > 2 ? atoi(argv[2]) : 20;

  std::vector<uint8_t> data;
  size_t sz = load_file(argv[1], data);
  if (sz == 0) {
    fprintf(stderr, "failed to load %s\n", argv[1]);
    return 2;
  }

  // Decode once to get geometry + verify.
  mem_infile infile;
  infile.open(data.data(), sz);
  codestream cs;
  cs.read_headers(&infile);
  param_siz siz = cs.access_siz();
  ui32 image_w = siz.get_image_extent().x - siz.get_image_offset().x;
  ui32 image_h = siz.get_image_extent().y - siz.get_image_offset().y;
  ui32 comps = siz.get_num_components();
  cs.set_planar(false);
  cs.create();

  // Buffer for one full line (max component width).
  std::vector<si32> line_store(image_w * comps);

  double t0 = now_s();
  uint64_t checksum = 0;
  for (int it = 0; it < iters; ++it) {
    // Fresh codestream + rewound infile per iteration.
    mem_infile infile;
    infile.open(data.data(), sz);
    codestream cs;
    cs.read_headers(&infile);
    cs.set_planar(false);
    cs.create();
    ui32 height = cs.access_siz().get_recon_height(0);
    for (ui32 y = 0; y < height; ++y) {
      for (ui32 c = 0; c < comps; ++c) {
        ui32 comp = 0;
        line_buf *line = cs.pull(comp);
        if (!line) break;
        ui32 cw = (comp == 0) ? image_w : siz.get_recon_width(comp);
        si32 *p = line->i32;
        if (p) checksum += (uint64_t)p[0] + (uint64_t)p[cw - 1];
      }
    }
    cs.close();
  }
  double t1 = now_s();

  double mpix = (double)image_w * (double)image_h;
  double samples = mpix * (double)comps;
  double per_iter = (t1 - t0) / iters;
  printf("file=%s %ux%u comps=%u iters=%d\n", argv[1], image_w, image_h, comps, iters);
  printf("decode_throughput_mpix_s=%.2f  samples_mpix_s=%.2f  per_iter_ms=%.3f  checksum=%llu\n",
         (mpix / 1e6) / per_iter, (samples / 1e6) / per_iter, per_iter * 1000.0,
         (unsigned long long)checksum);
  return 0;
}
