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

### LJPEG v2 Streaming Encode
- `tdng_lj92_encode_open/scan/begin/rows/finish` emit the encoded stream to a `tdng_lj92_write_fn` sink (4KB staged chunks, never materialized). Two-pass: `scan` builds the SSSS histogram, `begin` emits SOI/SOF3/DHT/SOS, `rows` feeds the entropy pass incrementally.

### v3 Streaming / Tiled Writer
- `tinydng_write_io` (absolute-offset write/size/close; file + memory backends)
- `tinydng_writer_create/write_tile/write_strip/finish`: tiled or multi-strip; none/LZW/PackBits/lossless-JPEG compression. Edge tiles padded to full tile dims.
- `tinydng_write_memory/write_file` are thin wrappers (single strip + memory/file sink)

### V3 Decode
- `tinydng_decode_image(ctx, doc, idx, opts, pixels, err)` - decode full image
- `tinydng_decode_region(ctx, doc, idx, x, y, w, h, opts, pixels, err)` - decode sub-region
- `tinydng_decode_segment(ctx, doc, idx, seg_idx, opts, pixels, err)` - decode single segment
- Multi-threaded decode via `opts->num_threads` (POSIX/Win32 threads)

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
