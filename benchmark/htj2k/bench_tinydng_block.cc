#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "tiny_dng_htj2k.h"

static double now_s() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char **argv) {
  const int iters = argc > 1 ? atoi(argv[1]) : 20;
  const uint32_t w = 64, h = 64, kmax = 12, missing = kmax - 1;
  const size_t n = (size_t)w * h;
  std::vector<uint32_t> src(n), dst(n);
  uint64_t seed = 0x123456789abcdef0ull;
  for (size_t i = 0; i < n; ++i) {
    seed = seed * 6364136223846793005ull + 1442695040888963407ull;
    uint32_t mag = (uint32_t)((seed >> 32) & 255u);
    if ((seed & 7u) == 0) mag = 0;
    src[i] = ((seed >> 61) & 1u ? 0x80000000u : 0u) | (mag << (31-kmax));
  }
  uint8_t *coded = nullptr;
  size_t cap = 0;
  uint32_t lengths[3] = {0, 0, 0};
  if (tdng_htj2k_encode_codeblock32(src.data(), missing, w, h, w,
                                    lengths, &coded, &cap) != TDNG_HTJ2K_OK)
    return 2;
  tdng_htj2k_ctx ctx;
  tdng_htj2k_ctx_init(&ctx);
  for (int i = 0; i < 2; ++i)
    tdng_htj2k_decode_codeblock32_ctx(&ctx, coded, dst.data(), missing, 1,
                                      lengths[0], 0, w, h, w, 1);
  double t0 = now_s();
  uint64_t checksum = 0;
  for (int i = 0; i < iters; ++i) {
    int r = tdng_htj2k_decode_codeblock32_ctx(&ctx, coded, dst.data(), missing,
                                              1, lengths[0], 0, w, h, w, 1);
    if (r != TDNG_HTJ2K_OK) return 3;
    checksum += dst[0] + dst[n - 1];
  }
  double sec = now_s() - t0;
  printf("codec=tinydng block=%ux%u K=%u iters=%d bytes=%u throughput_mpix_s=%.2f checksum=%llu\n",
         w, h, kmax, iters, lengths[0], (n * iters / 1e6) / sec,
         (unsigned long long)checksum);
  tdng_htj2k_ctx_free(&ctx);
  free(coded);
  return 0;
}
