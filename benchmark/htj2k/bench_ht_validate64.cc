// Validate the 64-bit HTJ2K decoder: for the same coded bytes, the 64-bit
// decoder (sign bit 63, missing_msbs+32) must produce the same coefficients
// as the 32-bit decoder (sign bit 31, missing_msbs).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "ojph_block_decoder.h"
#include "ojph_mem.h"
#include "tiny_dng_htj2k.h"
using namespace ojph::local;
using namespace ojph;
int main() {
  const char* path = "/tmp/opencode/cb_real.bin";
  FILE* f = fopen(path, "rb");
  if (!f) return 2;
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> data(sz);
  if (fread(data.data(), 1, sz, f) != (size_t)sz) return 3;
  fclose(f);
  size_t off = 0;
  uint64_t ok = 0, fails = 0, blocks = 0;
  while (off + 29 <= data.size()) {
    uint32_t mb, np, l1, l2, w, h, st, sc;
    memcpy(&mb, data.data()+off, 4);
    memcpy(&np, data.data()+off+4, 4);
    memcpy(&l1, data.data()+off+8, 4);
    memcpy(&l2, data.data()+off+12, 4);
    memcpy(&w, data.data()+off+16, 4);
    memcpy(&h, data.data()+off+20, 4);
    memcpy(&st, data.data()+off+24, 4);
    sc = data.data()[off+28];
    off += 29;
    uint32_t total = l1 + l2;
    if (off + total > data.size()) break;
    const uint8_t* cb = data.data() + off;
    off += total;
    blocks++;
    size_t bufpx = (size_t)(h + 8) * st;
    std::vector<uint64_t> ref(bufpx);
    std::vector<uint64_t> mine(bufpx);
    if (!ojph_decode_codeblock64((uint8_t*)cb, ref.data(), mb, np, l1, l2, w, h, st, sc)) {
      if (fails < 5) fprintf(stderr, "ojph64 decode fail block %llu %ux%u mb=%u\n", (unsigned long long)blocks, w, h, mb);
      fails++; continue;
    }
    int r = tdng_htj2k_decode_codeblock64(cb, mine.data(), mb, np, l1, l2, w, h, st, sc);
    if (r != TDNG_HTJ2K_OK) {
      if (fails < 5) fprintf(stderr, "64-bit decode fail block %llu %ux%u mb=%u\n", (unsigned long long)blocks, w, h, mb);
      fails++; continue;
    }
    uint64_t bad = 0;
    for (size_t i = 0; i < (size_t)h * st; ++i) {
      if (mine[i] != ref[i]) {
        if (bad < 3) fprintf(stderr, "  mismatch block %llu i=%zu mine=%016llx ref=%016llx\n", (unsigned long long)blocks, i, (unsigned long long)mine[i], (unsigned long long)ref[i]);
        bad++;
      }
    }
    if (bad) fails += 1000000; else ok++;
  }
  printf("blocks=%llu ok=%llu fails=%llu\n", (unsigned long long)blocks, (unsigned long long)ok, (unsigned long long)fails);
  return fails ? 1 : 0;
}
