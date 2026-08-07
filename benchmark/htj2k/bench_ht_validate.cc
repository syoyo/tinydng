// Validate the clean-room HTJ2K decoder against OpenJPH on REAL codeblocks
// dumped from a live codestream decode (see dump_cbs.cc + OJPH_DUMP_CB).
//
// Build:  make -C benchmark/htj2k bench_ht_validate
// Run:    benchmark/htj2k/bench_ht_validate <cb_dump.bin>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "ojph_block_decoder.h"
#include "ojph_mem.h"
#include "tiny_dng_htj2k.h"

using namespace ojph::local;
using namespace ojph;

#pragma pack(push, 1)
struct cb_hdr {
  uint32_t missing_msbs;
  uint32_t num_passes;
  uint32_t pass_len[2];
  uint32_t w, h, stride;
  uint8_t stripe_causal;
};
#pragma pack(pop)

int main(int argc, char** argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s <dump.bin>\n", argv[0]); return 1; }
  FILE* f = fopen(argv[1], "rb");
  if (!f) return 2;
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> data(sz);
  if (fread(data.data(), 1, sz, f) != (size_t)sz) return 3;
  fclose(f);

  size_t off = 0;
  uint64_t blocks = 0, fails = 0, ok = 0;
  uint64_t mismatch_samples = 0;
  while (off + sizeof(cb_hdr) <= data.size()) {
    cb_hdr h;
    memcpy(&h, data.data() + off, sizeof(cb_hdr));
    off += sizeof(cb_hdr);
    uint32_t total = h.pass_len[0] + h.pass_len[1];
    if (off + total > data.size()) { fprintf(stderr, "truncated block\n"); break; }
    const uint8_t* cb = data.data() + off;
    off += total;

    // OpenJPH buffers codeblocks with the *nominal* height, so the decoder
    // may write row h (odd-height blocks). Pad generously.
    size_t buf_px = (size_t)(h.h + 8) * h.stride;
    size_t npx = (size_t)h.w * h.h;
    std::vector<uint32_t> ref(buf_px), mine(buf_px);
    bool rok = ojph_decode_codeblock32((uint8_t*)cb, ref.data(), h.missing_msbs,
                                       h.num_passes, h.pass_len[0], h.pass_len[1],
                                       h.w, h.h, h.stride, h.stripe_causal);
    int r = tdng_htj2k_decode_codeblock32(cb, mine.data(), h.missing_msbs,
                                          h.num_passes, h.pass_len[0], h.pass_len[1],
                                          h.w, h.h, h.stride, h.stripe_causal);
    blocks++;
    if (!rok || r != TDNG_HTJ2K_OK) {
      if (fails < 10)
        fprintf(stderr, "decode-fail block %llu: %ux%u np=%u mb=%u len=%u,%u ref_ok=%d mine=%d\n",
                (unsigned long long)blocks, h.w, h.h, h.num_passes,
                h.missing_msbs, h.pass_len[0], h.pass_len[1], rok, r);
      fails++;
      continue;
    }
    ok++;
    for (size_t i = 0; i < buf_px; ++i) {
      if (ref[i] != mine[i]) {
        size_t row = i / h.stride, col = i % h.stride;
        if (row < h.h && col < h.w) {
          if (mismatch_samples < 10)
            fprintf(stderr, "  mismatch block %llu row %zu col %zu: ref=%08x mine=%08x\n",
                    (unsigned long long)blocks, row, col, ref[i], mine[i]);
          mismatch_samples++;
          if (mismatch_samples >= 10) { fails += 1000000; break; }
        }
      }
    }
  }
  printf("blocks=%llu ref_ok_and_mine_ok=%llu mismatches=%llu\n",
         (unsigned long long)blocks, (unsigned long long)ok,
         (unsigned long long)mismatch_samples);
  return fails ? 1 : 0;
}
