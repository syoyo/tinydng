# TinyDNG WebAssembly demo

This directory contains a decode-only WebAssembly build of the v3 C API and a
small browser demo for DNG/TIFF files. The browser build includes TIFF/DNG
parsing, baseline JPEG, lossless JPEG, LZW, PackBits and ZIP decoding. It does
not include the writer, PSD support or worker threads.

## Build

Use an Emscripten environment with `emcc` on `PATH`:

```bash
emcmake cmake -S web -B web/build -DCMAKE_BUILD_TYPE=Release
cmake --build web/build
```

The generated `tinydng.js` and `tinydng.wasm` files are written to
`web/js/dist/` and are intentionally not source-controlled.

## Run the demo

Serve the repository over HTTP so the browser can load the ES module and WASM
binary:

```bash
python3 -m http.server 8000
```

Then open <http://localhost:8000/web/js/> and choose a DNG or TIFF file. The
demo selects the largest image/IFD by default, shows basic metadata, and uses
WebGL2 for a tiled raw-development preview. The GPU path performs a compact
3×3 CFA demosaic, exposure adjustment, shadow lift, highlight rolloff and
optional DNG ColorMatrix1/AsShotNeutral correction. The preview is capped at
2048 pixels on its longest edge; only one source tile plus a one-pixel halo is
uploaded at a time, so large files do not require a full-resolution GPU
texture. Browsers without WebGL2 get a normalized Canvas2D fallback.

## Smoke test

After building the WASM target:

```bash
node web/js/test.mjs .
```

The smoke test opens DNG and TIFF fixtures, checks metadata and decodes pixels,
including a DNG whose main image is stored in a SubIFD.
