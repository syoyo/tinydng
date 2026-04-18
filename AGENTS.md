# TinyDNG Agent Notes

## Build & Test

```bash
cd build && cmake .. && make -j4
./test_dng_v2 <file.dng>        # V2 loader (pure C, no stb_image dependency)
./test_dng_v2 <file.dng> asis   # Store raw JPEG data instead of decoding
./test_dng_loader <file.dng>    # V1 loader (C++, uses stb_image for baseline JPEG)
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
