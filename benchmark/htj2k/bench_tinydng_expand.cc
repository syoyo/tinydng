#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

#include "tiny_dng_j2k.h"

static double now_s() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <file.j2c> [iterations]\n", argv[0]);
    return 1;
  }
  int iters = argc > 2 ? atoi(argv[2]) : 10;
  std::ifstream f(argv[1], std::ios::binary);
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
  if (data.empty()) return 2;
  tdng_j2k *j2k = nullptr;
  uint32_t w, h, nc, tw, th, ntx, nty, bits[8] = {};
  if (tdng_j2k_open(&j2k, data.data(), data.size(), &w, &h, &nc, bits, &tw,
                    &th, &ntx, &nty) != TDNG_J2K_OK) return 3;
  std::vector<int32_t> out((size_t)tw * th * nc);
  for (int i = 0; i < 2; ++i)
    for (uint32_t tile = 0; tile < ntx * nty; ++tile)
      if (tdng_j2k_decode_tile(j2k, tile, out.data()) != TDNG_J2K_OK) return 4;
  uint64_t checksum = 0;
  double t0 = now_s();
  for (int i = 0; i < iters; ++i)
    for (uint32_t tile = 0; tile < ntx * nty; ++tile) {
      if (tdng_j2k_decode_tile(j2k, tile, out.data()) != TDNG_J2K_OK) return 5;
      checksum += (uint32_t)out[0] + (uint32_t)out[(size_t)tw * th * nc - 1];
    }
  double sec = now_s() - t0;
  printf("codec=tinydng file=%s %ux%u comps=%u tiles=%ux%u lossy=%d iters=%d throughput_mpix_s=%.2f checksum=%llu\n",
         argv[1], w, h, nc, ntx, nty, tdng_j2k_is_lossy(j2k), iters,
         ((double)w * h * iters / 1e6) / sec, (unsigned long long)checksum);
  tdng_j2k_close(j2k);
  return 0;
}
