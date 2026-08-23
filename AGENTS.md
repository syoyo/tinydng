# TinyDNG Agent Notes

## Build & Test

```bash
cd build && cmake .. && make -j4
./test_dng_v3 <file.dng>           # V3 loader (pure C11, all features)
ctest                              # All v3 tests (7 tests)
```

## Key Files

- `tinydng.h` - V3 public API header (pure C11)
- `tinydng_api.c` - Context, allocator, lifecycle, `tinydng_is_dng`
- `tinydng_io.c` - IO backends (memory, mmap, stdio)
- `tinydng_tiff.c` - TIFF/IFD parser
- `tinydng_dng.c` - DNG metadata tag handler
- `tinydng_codec.c` - Block decoder dispatch (LJPEG, ZIP, LZW, PackBits, baseline JPEG)
- `tinydng_write.c` - TIFF/DNG writer (single-strip + streaming tiled)
- `tinydng_psd.c` - PSD/PSB reader
- `tinydng_psd_write.c` - PSD/PSB writer
- `tinydng_miniz.c` - Vendored miniz (ZIP/Deflate)
- `tinydng_stb_image.c` - Vendored stb_image (baseline/progressive JPEG)
- `tiny_dng_ljpeg92_v2.c/h` - Standalone LJPEG92 decoder/encoder (lossless JPEG)
- `td_internal.h` - Internal structures and tag definitions

## Deprecated Files

v1 (C++) and v2 (C) APIs have been deprecated and moved to `attic/`:
- `attic/v1/` - `tiny_dng_loader.h`, `tiny_dng_writer.h`, test, examples
- `attic/v2/` - `tiny_dng_v2.h/c`, test
- `attic/fuzzer/` - v1 fuzz target
- `attic/examples/` - v1 C++ examples
- `attic/python/` - v1 Python bindings

The LJPEG codec (`tiny_dng_ljpeg92_v2.h/c`) is still used by v3 and is NOT deprecated.

## Architecture Notes

### V3 SubIFD Handling
SubIFDs are only processed when `TINYDNG_OPEN_PARSE_SUBIFDS` flag is set. Without it, only IFD0 is processed. Depth limited by `config.max_ifd_depth` (default 8).

### V3 Metadata
- Document-level: `tinydng_document_exif(doc)` returns `tinydng_exif*` from IFD0
- Per-image: `tinydng_image_get(doc, i)` returns `tinydng_image_info*` with `.exif`, `.cfa`, `.raw` sub-structs
- EXIF: make, model, software, datetime, image_description, orientation, shutter_speed, aperture_value, exposure_time, iso
- CFA: pattern_dim, pattern[16], plane_color[4], layout
- Raw: black/white levels, color/forward/camera calibration matrices, as_shot_neutral, active_area, profile_name, profile_tone_curve, noise_profile, gainmaps, opcodes, warps, vignettes, cr2_slices, semantic_name, linearization_table, profile_embed_policy
- Custom fields: all non-Standard TIFF tags captured into `custom_fields[]`

### V3 Quick Format Check
`tinydng_is_dng(path)` / `tinydng_is_dng_memory(data, size)` check TIFF/PSD magic bytes without full parse.

### V3 LightSource Constants
`TINYDNG_LIGHTSOURCE_DAYLIGHT`, `_FLUORESCENT`, `_TUNGSTEN`, `_CLOUDY`, `_SHADE`, `_D50`, `_D55`, `_D65`, `_D75` defined as macros in `tinydng.h`.

### LJPEG v2 Error Codes
- `TDNG_LJ92_ERROR_NONE = 0` - Success
- `TDNG_LJ92_ERROR_NOT_LOSSLESS = -5` - Stream is baseline/progressive JPEG (SOF0/1/2), not lossless. V3 codec gracefully returns this instead of CORRUPT.
- `TDNG_LJ92_ERROR_CORRUPT = -1` - Actual corruption
- `TDNG_LJ92_ERROR_IO = -6` - Short write during streaming encode

### LJPEG v2 Fast Decode + Streaming
- Huffman LUT entries pack `(ssss << 8) | (codelen + ssss)`; the entropy loop consumes code + residual with ONE accumulator shift (branchless, no ssss==0 special case). Decode ~1.5x vs pre-port.
- `tdng_lj92_open_streaming(lj, user, read_fn, size_fn, ...)` parses headers through a chunk-cached reader (64KB). Entropy payload is destuffed straight from the callback (no full-segment materialization).
- v3 codec (`tinydng_codec.c` `td_decode_block_ljpeg`) uses the streaming path automatically when `io->map == NULL` (stdio backend).
- SIMD prefix-sum kernels (SSE2/AVX2) are compiled into every GCC/Clang x86 build via `__attribute__((target))` and selected at runtime with `__builtin_cpu_supports` (`tdng_simd_level()`); no `-march` leaks into other code. Overrides: `TINY_DNG_LJPEG92_V2_USE_AVX2/_SSE2` (compile-time) and `TINY_DNG_LJPEG92_NO_SIMD` (scalar only). MSVC builds scalar.
- The v3 codec decodes LJPEG directly into the block/destination buffer (dims are proven equal just before), so there is no per-segment scratch + memcpy. KEEP_PACKED (bps 9..15) is the exception: it decodes to a u16 scratch then repacks MSB-first rows (this path previously overflowed the packed-size block).

### LJPEG v2 Streaming Encode
- `tdng_lj92_encode_open/scan/begin/rows/finish` emit the encoded stream to a `tdng_lj92_write_fn` sink (64KB staged chunks, never materialized). Two-pass: `scan` builds the SSSS histogram, `begin` emits SOI/SOF3/DHT/SOS, `rows` feeds the entropy pass incrementally.
- Bit writer: 64-bit left-aligned accumulator; drains 32 bits at a time, bulk-appending 4 bytes whenever none is 0xFF (`enc_word_has_ff`), else falls back to byte-wise stuffing. `enc_emit_diff` fuses Huffman code + residual into ONE accumulator update via `emitlut[s] = (code << 16) | len`; per-sample code-validity checks were hoisted to a one-time check in `encode_begin` (the histogram pass saw identical samples).
- Mono/skip-free/no-delinearize configuration (what the writer emits) uses streaming specializations in BOTH passes: diffs are computed from adjacent raw input elements (u16 wraparound identical to the generic path), eliminating row-cache traffic; 16-sample blocks go through `tdng_diff16_{sse2,avx2}` when the host supports it. Encoder output is bit-identical to the generic path.

### v3 Streaming / Tiled Writer
- `tinydng_write_io` (absolute-offset write/size/close/flush; file + memory backends). `flush` is optional; `tinydng_writer_finish` calls it on success so a final stdio-buffer failure reports E_IO instead of silent truncation.
- `tinydng_writer_create/write_tile/write_strip/finish`: tiled or multi-strip; none/LZW/PackBits/lossless-JPEG compression. Edge tiles padded to full tile dims.
- Lossless JPEG writer constraints (dims <= 65535, spp 1..4) are validated at create time (E_UNSUPPORTED).
- `tinydng_write_memory/write_file` are thin wrappers (single strip + memory/file sink)
- The byte-swap scratch (`scratch_swap`) and compressed-output staging (`scratch_enc`) are cached in the writer and grown on demand — do not reintroduce per-tile alloc/free.

### V3 Load Path
- stdio backend reads use POSIX `pread` on a dup'ed fd (positionless => decode workers read concurrently with no lock); other platforms serialize on the dedicated `ctx->io_lock`, which is separate from the allocator's `ctx->lock`.
- DNG metadata arrays (`td_read_reals/uints`, linearization table, gainmap pixels) bulk-view the whole span once (`td_bulk_span_view` + `td_*_from_buf` in tinydng_tiff.c) instead of one io_view per element.

### V3 Hardening Notes
- Security regression fixtures live in `tests/v3_test/test_v3_security.c`; each case is designed to fail under ASan+UBSan if its fix is reverted.
- Baseline-JPEG payloads (TIFF JPEGInterchangeFormat + PSD thumbnail/smart objects) pass an stbi_info budget check before decode; stb_image allocates outside the tracked allocator, so `STBI_MAX_DIMENSIONS` is pinned to 1<<15 in `tinydng_stb_image.c`.
- Vendored deps vs known CVEs: stb_image 2.28 is built with `STBI_ONLY_JPEG`(+`STBI_ONLY_PNG`) which compiles out every decoder named by public stb_image CVEs (GIF/TGA/HDR/PIC); do not widen the format set without re-checking. miniz 10.0.3 has no known open CVEs.
- LJPEG linearization/delinearization tables are indexed with strict `< length` bounds everywhere (decoder and encoder agree).
- PSD bitmap mode (depth 1): a stored set bit is BLACK, decoded to {0, 255} for both composites and layer channels (Photoshop semantics). KEEP_PACKED output stays raw stored bytes.
- PSD duplicate layer channel ids are deduped at decode task-build time so threaded decodes never race on one output slot.
- PSD layers may declare huge rects with little channel data (uniform-color/adjustment layers are legal). Channel planes report their filled prefix; scatter onto pre-zeroed destinations is bounded by that prefix, so decode cost tracks stored bytes, not declared pixels.
- LJPEG writer row predictors index the previous row with a SIGNED offset (`rp[-(ptrdiff_t)W]`). An unsigned negated index (`rp[-(size_t)W]`) wraps in size_t and trips UBSan pointer-overflow even when the final address is in-bounds (regression: fuzz-v3-write crash, hardening round 4).
- Allocation-failure injection (fixture Q): an allocator that fails after N allocations is swept over every ordinal for open+decode (uncompressed + LZW fixtures built by the writer) and write_memory. Every td_ctx_alloc call site must fail with a clean status and leave `tinydng_context_memory_used() == 0`. Keep this property when adding allocation sites.
- The fuzz-v3 harness exercises `tinydng_decode_segment` and `tinydng_decode_region` (in-bounds pseudo-random windows derived from decoded pixels), not just full-image decode — keep region/segment paths in harness coverage when adding new decode features.

### V3 Decode
- `tinydng_decode_image(ctx, doc, idx, opts, pixels, err)` - decode full image
- `tinydng_decode_region(ctx, doc, idx, x, y, w, h, opts, pixels, err)` - decode sub-region
- `tinydng_decode_segment(ctx, doc, idx, seg_idx, opts, pixels, err)` - decode single segment
- Multi-threaded decode via `opts->num_threads` (POSIX/Win32 threads)

### V3 Codec Performance Notes (tinydng_codec.c)
- LZW decoder (`td_lzw_decode`): stops as soon as the output byte count is satisfied (libtiff-compatible). Real-world encoders (Photoshop, NASA PDS) may omit EOI and pad trailing bits with garbage; consuming codes past the expected size failed otherwise-valid files (regression fixture P in test_v3_security.c). Emission writes the prefix-chain directly through a descending cursor instead of a push/pop stack; bit refill bulk-loads up to 5 bytes. Differential-tested against the old implementation on encoder-generated and random streams.
- Per-worker reusable scratches (`td_scratch in_sc/stored_sc` in `td_decode_worker_arg`): raw input bytes and decompressed bytes are grown-once buffers reused across segments, so threaded decodes do not hit the locked ctx allocator per segment. Do not reintroduce per-segment alloc/free in the block decoders.
- Direct-decode fast path in `td_decode_block_compressed`: when stored layout == output layout (bps 8, or 16/32 with matching endianness), LZW/PackBits/ZIP decompress straight into the destination block and skip both scratch + copy pass; PSD zip-with-prediction is excluded (its unpredict runs pre-conversion).
- Fast sub-byte row unpackers `td_unpack_row_{10,12,14}`: fixed sample groups with whole-byte boundaries (2s/3B, 4s/5B, 4s/7B); tails fall back to `td_unpack_tail`. These depths dominate RAW TIFF unpack time.
- Predictor 2 horizontal differencing is pointer-stepped per channel (no per-sample index multiplication); endian swap passes are byte-wise swaps.

## Test Files

- `pixel3.dng` - Main IFD=baseline JPEG thumbnail (672x504), SubIFD=lossless JPEG main (4032x3024), Make=Google, Model=Pixel 3 XL, Orientation=6
- `colorchart.dng` - Pure lossless JPEG (1888x1182, 14-bit), Make=Canon, Model=Canon EOS Kiss X4
- `IMG_*.CR2` - Canon RAW files: IFD0=main JPEG, IFD1=thumbnail, IFD2=uncompressed backup, Make=Canon, Model=Canon EOS 7D
- `proraw-48mp-02.dng` - Apple ProRAW, Make=Apple, Model=iPhone 14 Pro Max

## LJPEG Predictors

Predictor values 0-7 are valid for lossless JPEG. Predictor 0 means default (usually 1). The decoder handles all 8 predictors.

## EXIF Tag Parsing

V3 parses the following EXIF tags:
- `0x010F` (Make) - camera manufacturer
- `0x0110` (Model) - camera model
- `0x0131` (Software) - software name
- `0x0132` (DateTime) - date/time string
- `0x010E` (ImageDescription) - image description
- `0x0112` (Orientation) - 1-8, 0=not specified

## Extended Metadata

V3 parses the following DNG metadata tags:

### CFA Pattern Tags
- `0x828E` (CFARepeatPatternDim) - CFA pattern dimensions (rows, cols)
- `0x828F` (CFAPattern) - CFA pattern values
- `0xC616` (CFAPlaneColor) - CFA plane colors
- `0xC617` (CFALayout) - CFA layout (1=normal, 2=flipped, etc)

Access via `img->cfa` (`tinydng_cfa`).

### Raw Info Tags
- `0xC618` (BlackLevel) - Black level values (can be 1 or 4 values)
- `0xC61A` (WhiteLevel) - White level values (can be 1 or 4 values)
- `0x828D` (ColorMatrix1) - Color matrix for illuminant 1
- `0x828C` (ColorMatrix2) - Color matrix for illuminant 2
- `0x829A` (ForwardMatrix1) - Forward matrix for illuminant 1
- `0x829B` (ForwardMatrix2) - Forward matrix for illuminant 2
- `0xC612` (DNGVersion) - DNG version (4 bytes)

Access via `img->raw` (`tinydng_raw_info`).

### Additional Metadata
- `0xC61E` (AsShotNeutral) - As-shot neutral values (3 RATIONAL/SRATIONAL)
- `0xC760` (CalibrationIlluminant1) - Calibration illuminant 1 (SHORT)
- `0xC761` (CalibrationIlluminant2) - Calibration illuminant 2 (SHORT)
- `0xC705` (ActiveArea) - Active area (4 LONG values: top, left, bottom, right)
- `0xC709` (DefaultBlackRender) - Default black render (SHORT)
- `0xC718` (ProfileName) - Profile name (ASCII string)
- `0xC72C` (ProfileToneCurve) - Profile tone curve (up to 16 doubles)
- `0xC741` (NoiseProfile) - Noise profile (up to 8 doubles, type 12)
- `0xC6ED` (ProfileEmbedPolicy) - Profile embed policy (SHORT: 0=allow copy, 1=embed if used, 2=embed never)

### Type Handling
- Color matrices support both type 5 (RATIONAL) and type 10 (SRATIONAL)
- proraw-48mp-02.dng uses SRATIONAL (type 10) for color matrices
- colorchart.dng uses RATIONAL (type 5) for color matrices
- Black/White levels support both LONG (type 4) and SHORT (type 3)
