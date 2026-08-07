# TinyDNG Agent Notes

## Build & Test

```bash
cd build && cmake .. && make -j4
./test_dng_v2 <file.dng>        # V2 loader (pure C, no stb_image dependency)
./test_dng_v2 <file.dng> asis   # Store raw JPEG data instead of decoding
./test_dng_loader <file.dng>    # V1 loader (C++, uses stb_image for baseline JPEG)
./test_v2_streaming [tile_dir]  # streaming vs in-memory LJPEG decode parity
ctest                           # v3 tests + v2 streaming tests
```

## Key Files

- `tiny_dng_v2.c/h` - V2 loader (C, header-only API)
- `tiny_dng_ljpeg92_v2.c/h` - Standalone LJPEG92 decoder (lossless JPEG)
- `tiny_dng_loader.h` - V1 loader (C++ header-only, includes stb_image for baseline JPEG)
- `test_v2.c` - V2 test program
- `test_loader.cc` - V1 test program

## Architecture Notes

### V2 SubIFD Handling (CRITICAL)
In `tdng_parse_document()`, SubIFDs are only queued when `TINYDNG_V2_LOAD_FLAG_PARSE_IMAGE_AS_IS` is set (line ~1230 in tiny_dng_v2.c). Without this flag, only IFD0 is processed. This means:
- DNGs with main image in SubIFD (like pixel3.dng) only load the thumbnail IFD in non-asis mode
- CR2 files work because the main image is in IFD0

### V2 Metadata Structure
- Document-level metadata: `tinydng_v2_document_global_exif()` returns `tinydng_v2_basic_exif*` from IFD0
- Per-image metadata: `img->exif` field in `tinydng_v2_image`
- Basic EXIF fields: make, model, software, datetime, image_description, orientation
- Strings are allocated via context allocator (same as image data)
- Destruction: `tinydng_v2_document_destroy()` frees both global and per-image metadata

### LJPEG v2 Error Codes
- `TDNG_LJ92_ERROR_NONE = 0` - Success
- `TDNG_LJ92_ERROR_NOT_LOSSLESS = -5` - Stream is baseline/progressive JPEG (SOF0/1/2), not lossless. V2 decoder gracefully returns this instead of CORRUPT.
- `TDNG_LJ92_ERROR_CORRUPT = -1` - Actual corruption

### LJPEG v2 Fast Decode + Streaming
- Huffman LUT entries pack `(ssss << 8) | (codelen + ssss)`; the entropy loop consumes code + residual with ONE accumulator shift (branchless, no ssss==0 special case). LUTs are expanded to a uniform peek width (`expand_luts_uniform`, `huff_maxbits`) so interleaved loops use a single shift width. Decode ~1.5x vs pre-port (58 -> ~87 MPix/s on proraw-48mp-01.dng, sandbox/lj92 README has the analysis). Byte-identical output verified against sandbox/lj92 (test_correct: 193 checks).
- `tdng_lj92_open_streaming(lj, user, read_fn, size_fn, ...)` parses headers (incl. DHT) through a chunk-cached reader (64KB) and decodes with the regular `tdng_lj92_decode`; the entropy payload is destuffed straight from the callback (no full-segment materialization). `size_fn` may be NULL (unknown length). Tests: `tests/v2_test/test_v2_streaming.c` (memory==streaming byte parity, known/unknown size, skip lengths, truncation/garbage/baseline error paths).
- v3 codec (`tinydng_codec.c` `td_decode_block_ljpeg`) uses the streaming path automatically when `io->map == NULL` (stdio backend).

### LJPEG v2 Streaming Encode
- `tdng_lj92_encode_open/scan/begin/rows/finish` emit the encoded stream to a `tdng_lj92_write_fn` sink (4KB staged chunks, never materialized). Two-pass: `scan` builds the SSSS histogram, `begin` emits SOI/SOF3/DHT/SOS, `rows(lj, image_base, row0, count)` feeds the entropy pass incrementally (predictor row cache persists; must be called in row order with the same base pointer). `TDNG_LJ92_ERROR_IO = -6` on a short sink write. `tdng_lj92_encode_ex` is a thin wrapper over the same API (growable-buffer sink). Tests: `tests/v2_test/test_v2_streamencode.c` (byte parity vs one-shot, chunked rows, DNG tile readLength/skipLength layout, error paths).

### v3 Streaming / Tiled Writer
- `tinydng_write_io` (absolute-offset write/size/close; file + memory backends; `tinydng_write_io_open_file/memory`, `tinydng_write_io_memory_take`) — TIFF needs a seekable sink to patch the header's IFD pointer.
- `tinydng_writer_create(ctx, sink, meta /*data ignored*/, opts, tiling, &w)` / `tinydng_writer_write_tile(w, idx, pixels)` / `tinydng_writer_write_strip(w, idx, pixels)` / `tinydng_writer_finish(w)`: tiled (TileWidth/Length + TileOffsets/ByteCounts arrays in extras) or multi-strip; compression none/LZW/PackBits/lossless-JPEG (16-bit, per-segment streams via the streaming encoder, straight to the sink). Edge tiles are padded to full tile dims (DNG requires full-size tile streams; the loader decodes tile_width x tile_length and blits only the valid region). The extras buffer is re-laid out in sorted-entry order at finish (`td_writer_relayout_extras`) so IFD running offsets match. `tinydng_write_memory/write_file` are thin wrappers (single strip + memory/file sink). Tests: `tests/v3_test/test_v3_streamwrite.c` (incl. a real-corpus round trip: pixel3.dng lossless SubIFD -> tiled LJPEG DNG -> decode identical) + `fuzzer/fuzz-v3-streamwrite.c`. Benchmark: `benchmark/streamwrite`.

### V2 Image Loading Behavior
- Lossless JPEG: decoded into memory, DATA_OWNS_MEMORY flag set
- Baseline JPEG (NOT_LOSSLESS): raw JPEG bytes copied to allocated memory, DATA_OWNS_MEMORY flag set (file buffer is freed after loading)
- As-is mode: stores segment offsets into file buffer, DATA_IS_FILE_VIEW flag set

## Test Files

- `pixel3.dng` - Main IFD=baseline JPEG thumbnail (672x504), SubIFD=lossless JPEG main (4032x3024), Make=Google, Model=Pixel 3 XL, Orientation=6
- `colorchart.dng` - Pure lossless JPEG (1888x1182, 14-bit), Make=Canon, Model=Canon EOS Kiss X4
- `IMG_*.CR2` - Canon RAW files: IFD0=main JPEG, IFD1=thumbnail, IFD2=uncompressed backup, Make=Canon, Model=Canon EOS 7D
- `proraw-48mp-02.dng` - Apple ProRAW, Make=Apple, Model=iPhone 14 Pro Max

## LJPEG v2 Predictors

Predictor values 0-7 are valid for lossless JPEG. Predictor 0 means default (usually 1). The decoder handles all 8 predictors.

## EXIF Tag Parsing

V2 parses the following EXIF tags:
- `0x010F` (Make) - camera manufacturer
- `0x0110` (Model) - camera model
- `0x0131` (Software) - software name
- `0x0132` (DateTime) - date/time string
- `0x010E` (ImageDescription) - image description
- `0x0112` (Orientation) - 1-8, 0=not specified

## Extended Metadata (Phase 2)

V2 parses the following extended metadata tags:

### CFA Pattern Tags
- `0x828E` (CFARepeatPatternDim) - CFA pattern dimensions (rows, cols)
- `0x828F` (CFAPattern) - CFA pattern values
- `0xC616` (CFAPlaneColor) - CFA plane colors
- `0xC617` (CFALayout) - CFA layout (1=normal, 2=flipped, etc)

Access via `tinydng_v2_image_cfa()` which returns `tinydng_v2_cfa_pattern*`

Note: CFA data is typically in SubIFDs, not IFD0. Use as-is mode to see CFA pattern.

### Raw Info Tags
- `0xC618` (BlackLevel) - Black level values (can be 1 or 4 values)
- `0xC61A` (WhiteLevel) - White level values (can be 1 or 4 values)
- `0x828D` (ColorMatrix1) - Color matrix for illuminant 1
- `0x828C` (ColorMatrix2) - Color matrix for illuminant 2
- `0x829A` (ForwardMatrix1) - Forward matrix for illuminant 1
- `0x829B` (ForwardMatrix2) - Forward matrix for illuminant 2
- `0xC612` (DNGVersion) - DNG version (4 bytes)

Access via `tinydng_v2_image_raw_info()` which returns `tinydng_v2_raw_info*`

### Additional Metadata
- `0xC61E` (AsShotNeutral) - As-shot neutral values (3 RATIONAL/SRATIONAL)
- `0xC760` (CalibrationIlluminant1) - Calibration illuminant 1 (SHORT)
- `0xC761` (CalibrationIlluminant2) - Calibration illuminant 2 (SHORT)
- `0xC705` (ActiveArea) - Active area (4 LONG values: top, left, bottom, right)
- `0xC709` (DefaultBlackRender) - Default black render (SHORT)
- `0xC718` (ProfileName) - Profile name (ASCII string)
- `0xC72C` (ProfileToneCurve) - Profile tone curve (up to 16 doubles)
- `0xC741` (NoiseProfile) - Noise profile (up to 8 doubles, type 12)

### Type Handling
- Color matrices support both type 5 (RATIONAL) and type 10 (SRATIONAL)
- proraw-48mp-02.dng uses SRATIONAL (type 10) for color matrices
- colorchart.dng uses RATIONAL (type 5) for color matrices
- Black/White levels support both LONG (type 4) and SHORT (type 3)

### HTJ2K Block Codec (JPEG 2000 Part 15 / ITU-T T.814)
- `tiny_dng_htj2k.c/h` - clean-room C11 HT block decoder + encoder.
- `tiny_dng_htj2k_tables.h` - normative VLC codebook from ITU-T T.814
  (derived decoder/encoder LUTs are built at runtime in `tiny_dng_htj2k.c`).
- Codeblock API operates on sign-magnitude coefficients: 32-bit = sign in
  bit 31, magnitude shifted left by `31-K_max`, `missing_msbs = K_max-1`;
  64-bit = sign in bit 63, shift `63-K_max`. `p = 30-missing_msbs` (or
  `62-`) is the magnitude LSB plane; decoded values carry a half-bin at
  bit `p-1`, so integer extraction is `(v & 0x7FFFFFFF) >> p`.
- Decoder: CUP (VLC+MEL step1, MagSgn step2), SPP, MRP; 32-bit and 64-bit
  paths. Encoder: cleanup pass only (single pass, matching OpenJPH).
- Encoder buffers are heap-allocated (dynamic), so large codeblocks (up to
  1024x1024) are supported unlike OpenJPH's fixed 64x64-sized stack buffers.
- Verified byte-exact against OpenJPH: decoder on real codeblocks
  (48MP lossless, lossy 9/7, 16-bit, 32-bit-precision), encoder byte-identical
  across sizes/K. See `benchmark/htj2k/` (bench_ht_validate, bench_ht_enc).
- Block decode notes: scratch = `(h/2+1)*sstr + 2` entries; SPP/MRP reuse it
  as sigma with `mstr`; odd-height blocks write one row past `h*stride`, so
  the caller must provide a buffer with the nominal codeblock height.

### HTJ2K Benchmarks (`benchmark/htj2k/`)
- `bench_ojph_block` / `bench_ojph_expand` - OpenJPH baselines (block +
  library-level decode). See README for Phase 0 numbers on TR 1950X.
- `bench_ht_validate` / `bench_ht_validate64` - cross-validate our decoder
  against a patched OpenJPH that dumps real codeblocks (`OJPH_DUMP_CB`,
  dump via `dump_cbs` in /tmp/opencode).
- `bench_ht_cross` / `bench_ht_enc` - synthetic block round-trip and
  byte-identity vs OpenJPH's encoder.
- `run_bench.sh` - end-to-end ojph_compress/ojph_expand throughput.

### J2K Codestream Decoder (`tiny_dng_j2k.c/h`)
- Clean-room C11 JPEG 2000 codestream decoder for HTJ2K streams, built on
  `tiny_dng_htj2k.c`. Parses SIZ/COD/COC/QCD/QCC/SOT/SOD, extracts codeblocks
  from packet streams (tag trees), decodes with the block codec, and applies
  the inverse wavelet (reversible 5/3 or irreversible 9/7) plus colour
  transform (RCT/ICT) and the DC level shift.
- Scope: single tile-part per tile, one layer, precincts defaulting to the
  whole resolution (OpenJPH encoder defaults), progression orders RPCL/LRCP/
  RLCP/PCRL/CPRL. Per-component QCC quantization is honored.
- Verified lossless (byte-exact) against OpenJPH on 8-bit gray/RGB, 16-bit,
  29-bit, and lossy 9/7 files; see `benchmark/htj2k/bench_j2k_validate.cc`.
- tinydng v3 integration: `TINYDNG_COMPRESSION_JPEG2000 = 34712` routes DNG
  J2K tiles through `td_decode_block_j2k` (tinydng_codec.c).

### HTJ2K Self-Test (ctest)
- `test_htj2k` (`tests/htj2k_test/test_block_j2k.c`, ctest name `htj2k_self`)
  is self-contained (no OpenJPH): block encoder/decoder integer round-trip
  across sizes/K, decode of a tiny embedded lossless 16x16 codestream with
  expected pixels, and a corrupt-input smoke test. Note: the block decoder
  output carries a half-bin at bit `p-1`, so integer extraction is
  `(v & 0x7FFFFFFF) >> p`.

### J2K Codestream Encoder (`tiny_dng_j2k_enc.c/h`)
- Clean-room C11 HTJ2K codestream encoder (single tile, RPCL, one layer,
  whole-image precincts) built on the clean-room HT block encoder. Supports
  reversible 5/3 (lossless) with the forward RCT, plus an irreversible 9/7
  path (expounded QCD with per-subband deltas, delta_inv quantization, ICT).
  The API takes a `qstep` argument (0 = default 1/256).
  **Both lossless and lossy output are byte-identical to OpenJPH's
  `ojph_compress`** (verified on gray/RGB, square/odd/non-square, 8/16-bit,
  mct 0/1, lossless 5/3 and lossy 9/7 with the same QCD deltas). The earlier
  impression that the lossy path differed was a test-data bug (wrong PPM
  header offset), not an encoder defect.
- **Lossless output is byte-identical to OpenJPH's `ojph_compress`** (verified
  on gray/RGB, square/non-square, multi-codeblock images; the COM comment
  string differs by design). Decodes with both this library and OpenJPH.
- Key correctness details (all verified against OpenJPH):
  - The 5/3 analysis applies the **row filter first, then the column filter**
    (the rounding does not commute; the reverse order gives ±1 coefficient
    differences on random data).
  - The packet header bit writer uses OpenJPH's 0xFF handling: after a
    completed 0xFF byte the next byte's MSB is left as 0 (7 bits available),
    NOT a standard 0xFF 0x00 stuffing byte.
  - Codeblock sizes are the actual subband region (bw x bh), never padded to
    the full codeblock grid.
  - QCD byte order is finest-first (matching OpenJPH's `set_rev_quant`);
    the decoder's `(res_num-1)*3+band` indexing with res=1=coarsest reads
    the same slots both sides use.
  - Tag-tree build must bounds-check children (a 2x1 grid's out-of-bounds
    "children" alias the parent/root slots and corrupt the min).
  - Psot = tile-part bytes after the SOT marker (excludes the 2-byte marker).
  - CAP marker: Pcap=0x00020000, Ccap[0]=Bp where Bp = max(0, MAGB-8).
- API: `tdng_j2k_encode(px, w, h, nc, bits[], signed[], nd, reversible, mct,
  cb_log_w, cb_log_h, &out, &cap, &len)`.
- ctest `j2k_enc_roundtrip` (`tests/htj2k_test/test_j2k_enc.c`) round-trips a
  64x48 RGB lossless encode+decode without OpenJPH.

### J2K DNG writer (tinydng v3)
- `tinydng_write.c`: `TINYDNG_COMPRESSION_JPEG2000` (34712) routes each
  tile/strip through `tdng_j2k_encode` (host-order samples, 1..8 comps,
  1..31 bits; MCT for RGB photometric, none for CFA). `j2k_qstep > 0` in
  `tinydng_write_options` selects the irreversible 9/7 path (lossy). Edge
  tiles are padded to the full tile dims before encoding (DNG requirement).
  The tinydng lib links `m` on POSIX for the encoder's log/roundf.
- Packet-header 0xFF handling: when a packet header ends on a 0xFF byte, the
  encoder emits one pad byte (the next byte's MSB is left 0). The decoder
  must count it (`tdj_bitrd_bytes_used` adds a byte when the last header
  byte was 0xFF), matching OpenJPH's `bb_terminate`; otherwise codeblock
  data starts one byte early and multi-tile/edge-tile streams corrupt.
  Regression: `v3_j2k_tiled` (edge tiles), `v3_j2k_lossy` (lossy + 16-bit).
