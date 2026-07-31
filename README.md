# Tiny DNG Loader and Writer library

Header-only simple&limited DNG(Digital NeGative, TIFF format + extension) loader and writer in C++11.

Currently TinyDNG only supports lossless RAW DNG and limited lossless JPEG DNG(no lossy compression support).

TinyDNG can also be used as an TIFF RGB image loader(8bit, 16bit and 32bit are supported).

TinyDNG v3 also includes a pure C11 Adobe PSD/PSB reader and writer for composite images, layers, tagged blocks and embedded smart objects.

TinyDNG loader module is being fuzz tested using LLVMFuzzer, and is enoughly secure(and no C++ exception and assert/abort code exists).

![](images/tinydngloader_viewer.png)
(NOTE: TinyDNG just loads DNG data as is, thus you'll need your own RAW processing code(e.g. debayer) to get a developed image as shown the above)

## Features

### Loading

* [x] RAW DNG data
* [x] Lossless JPEG
  * Lossless JPEG decoding is supported based on liblj92 lib: https://bitbucket.org/baldand/mlrawviewer.git
* [x] ZIP-compressed DNG
  * Use miniz or zlib
* [x] JPEG
  * Support JPEG image(e.g. thumbnail) through `stb_image.h`.
* [x] TIFF
  * [x] 8bit uncompressed
  * [x] 8bit LZW compressed(no preditor, horizontal diff predictor)
* [x] PSD / PSB (Adobe Photoshop, v3 C API)
  * [x] Composite image decode through the normal `tinydng_decode_*` APIs
  * [x] Layers, layer channels, masks, blend modes, groups and tagged blocks
  * [x] RAW, RLE(PackBits), ZIP and ZIP-with-prediction channel data
  * [x] 1, 8, 16 and 32 bit depths
  * [x] Image resources and JPEG thumbnail resources
  * [x] Embedded smart objects(PSD/PSB/TIFF/DNG/JPEG/PNG payloads)
* Experimental
  * Apple ProRAW(Lossless JPEG 12bit)
    * [x] Lossless JPEG 12bit
    * [x] Semantic map(8bit Standard JPEG)
  * Decode Canon RAW(CR2)
    * [x] RAW
    * [ ] mRAW
    * [ ] sRAW
  * Decode Nikon RAW(NEF)
    * TODO
  * Reading custom TIFF tags.
* [x] Read DNG data from memory.

### Writing

* [x] DNG and TIFF
  * [x] LosslessJPEG compression
* [x] PSD / PSB (v3 C API)
  * [x] Composite image + layers
  * [x] RAW or RLE(PackBits) output
  * [x] 8, 16 and 32 bit depths
  * [x] Optional ICC profile resource

## Supported DNG files

Here is the list of supported DNG files.

* [x] Sigma sd Quattro H
  * Uncompressed RGB 12bit image.
* [x] iPhone DNG
* [x] Apple ProRAW
  * [x] Lossless JPEG 12bit
  * [x] Semantic map
* [x] Black magic DNG
  * CinemaDNG(lossy compression) is **not supported**.
* [x] Canon CR2(experimental)
  * Since CR2 format is also based on TIFF format : http://lclevy.free.fr/cr2/
  * RAW only(mRAW and sRAW are not supported)
* [x] Magic lantern DNG
  * [x] Uncompressed
  * [x] lossless JPEG(http://www.magiclantern.fm/forum/index.php?topic=18443.0)
* [ ] 8-bit TIFF image
  * [x] LZW compressed 8-bit image.
* [x] 16-bit uncompressed TIFF image
* [x] 32-bit uncompressed TIFF image
* OpCodeList
  * [x] GainMap

## Usage

### Loading DNG

```c++
#include <cstdio>
#include <cstdlib>
#include <iostream>

// Define TINY_DNG_LOADER_IMPLEMENTATION and STB_IMAGE_IMPLEMENTATION in only one *.cc
#define TINY_DNG_LOADER_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION

// Enable ZIP compression(through miniz library)
// Please don't forget copying&adding `miniz.c` and `miniz.h` to your project.
// #define TINY_DNG_LOADER_ENABLE_ZIP

// Uncomment these two lines if you want to use system provided zlib library, not miniz
// #define TINY_DNG_LOADER_USE_SYSTEM_ZLIB
// #include <zlib.h>
#include "tiny_dng_loader.h"

int main(int argc, char **argv) {
  std::string input_filename = "colorchart.dng";

  if (argc > 1) {
    input_filename = std::string(argv[1]);
  }

  std::string warn, err;
  std::vector<tinydng::DNGImage> images;

  // List of custom field infos. This is optional and can be empty.
  std::vector<tinydng::FieldInfo> custom_field_lists;

  // Loads all images(IFD) in the DNG file to `images` array.
  // You can use `LoadDNGFromMemory` API to load DNG image from a memory.
  bool ret = tinydng::LoadDNG(input_filename.c_str(), custom_field_lists, &images, &warn, &err);


  if (!warn.empty()) {
    std::cout << "Warn: " << warn << std::endl;
  }

  if (!err.empty()) {
    std::cerr << "Err: " << err << std::endl;
  }

  if (ret) {
    for (size_t i = 0; i < images.size(); i++) {
      const tinydng::DNGImage &image = images[i];
;
      std::cout << "width = " << image.width << std::endl;
      std::cout << "height = " << image.height << std::endl;
      std::cout << "bits per piexl = " << image.bits_per_sample << std::endl;
      std::cout << "bits per piexl(original) = " << image.bits_per_sample_original << std::endl;
      std::cout << "samples per pixel = " << image.samples_per_pixel << std::endl;

    }
  }

  return EXIT_SUCCESS;
}

```

### Writing DNG(and TIFF)

See [examples/dngwriter](examples/dngwriter) and https://github.com/storyboardcreativity/zraw-decoder for more details.

```c++
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <iostream>

#define TINY_DNG_WRITER_IMPLEMENTATION
#include "tiny_dng_writer.h"

static void CreateRGBImage(tinydngwriter::DNGImage *dng_image,
                        const unsigned short basecol) {
  unsigned int image_width = 512;
  unsigned int image_height = 512;
  dng_image->SetSubfileType(false, false, false);
  dng_image->SetImageWidth(image_width);
  dng_image->SetImageLength(image_height);
  dng_image->SetRowsPerStrip(image_height);

  // SetSamplesPerPixel must be called before SetBitsPerSample()
  dng_image->SetSamplesPerPixel(3);
  uint16_t bps[3] = {16, 16, 16};
  dng_image->SetBitsPerSample(3, bps);
  dng_image->SetPlanarConfig(tinydngwriter::PLANARCONFIG_CONTIG);
  dng_image->SetCompression(tinydngwriter::COMPRESSION_NONE);
  dng_image->SetPhotometric(tinydngwriter::PHOTOMETRIC_RGB);
  dng_image->SetXResolution(1.0);
  dng_image->SetYResolution(1.2); // fractioal test
  dng_image->SetResolutionUnit(tinydngwriter::RESUNIT_NONE);
  dng_image->SetImageDescription("bora");

  std::vector<unsigned short> buf;
  buf.resize(image_width * image_height * 3);

  for (size_t y = 0; y < image_height; y++) {
    for (size_t x = 0; x < image_width; x++) {
      buf[3 * (y * image_width + x) + 0] = static_cast<unsigned short>(x % 512);
      buf[3 * (y * image_width + x) + 1] = static_cast<unsigned short>(y % 512);
      buf[3 * (y * image_width + x) + 2] = basecol;
    }
  }

  dng_image->SetImageData(reinterpret_cast<unsigned char *>(buf.data()),
                          buf.size() * sizeof(unsigned short));
}

int main(int argc, char **argv) {
  std::string output_filename = "output.dng";

  if (argc < 1) {
    std::cout << argv[0] << " <output.dng>" << std::endl;
  }

  if (argc > 1) {
    output_filename = std::string(argv[1]);
  }

  // TinyDNGWriter supports both BigEndian and LittleEndian TIFF.
  // Default = BigEndian.
  bool big_endian = false;

  if (argc > 2) {
    big_endian = bool(atoi(argv[2]));
  }

  {
    // DNGWriter supports multiple DNG images.
    // First create DNG image data, then pass it to DNGWriter with AddImage API.
    tinydngwriter::DNGImage dng_image0;
    dng_image0.SetBigEndian(big_endian);
    tinydngwriter::DNGImage dng_image1;
    dng_image1.SetBigEndian(big_endian);

    CreateRGBImage(&dng_image0, 12000);
    CreateRGBImage(&dng_image1, 42000);

    tinydngwriter::DNGWriter dng_writer(big_endian);
    bool ret = dng_writer.AddImage(&dng_image0);
    assert(ret);

    ret = dng_writer.AddImage(&dng_image1);
    assert(ret);

    std::string err;
    ret = dng_writer.WriteToFile(output_filename.c_str(), &err);

    if (!err.empty()) {
      std::cerr << err;
    }

    if (!ret) {
      return EXIT_FAILURE;
    }

    std::cout << "Wrote : " << output_filename << std::endl;
  }

  return EXIT_SUCCESS;
}
```

### Loading PSD/PSB (v3 C API)

PSD/PSB support lives in the v3 C API (`tinydng.h`). Open a Photoshop file with the normal `tinydng_open_*` entry points. The merged composite image is exposed as document image 0, so it can be decoded with `tinydng_decode_image`, `tinydng_decode_region` or `tinydng_decode_segment`. PSD-specific metadata is available through `tinydng_document_psd()`.

```c
#include <stdio.h>
#include "tinydng.h"

int main(int argc, char **argv) {
  tinydng_context *ctx = NULL;
  tinydng_document *doc = NULL;
  tinydng_error err;
  const tinydng_psd_info *psd = NULL;
  tinydng_pixels composite;

  if (argc < 2) {
    return 1;
  }

  ctx = tinydng_context_create(NULL, &err);
  if (!ctx) {
    fprintf(stderr, "%s\n", err.message);
    return 1;
  }

  if (tinydng_open_file(ctx, argv[1], NULL, &doc, &err) != TINYDNG_OK) {
    fprintf(stderr, "%s\n", err.message);
    tinydng_context_destroy(ctx);
    return 1;
  }

  psd = tinydng_document_psd(doc);
  if (psd) {
    printf("%s %ux%u depth=%u layers=%zu resources=%zu\n",
           psd->is_psb ? "PSB" : "PSD", psd->width, psd->height,
           psd->depth, psd->layer_count, psd->resource_count);
  }

  if (tinydng_decode_image(ctx, doc, 0, NULL, &composite, &err) == TINYDNG_OK) {
    printf("composite: %ux%u channels=%u bytes=%zu\n",
           composite.width, composite.height,
           composite.samples_per_pixel, composite.size);
    tinydng_pixels_free(ctx, &composite);
  }

  tinydng_document_destroy(ctx, doc);
  tinydng_context_destroy(ctx);
  return 0;
}
```

Useful PSD-specific APIs:

* `tinydng_document_psd(doc)` returns `tinydng_psd_info` for layers, image resources, global tagged blocks and embedded smart objects.
* `tinydng_psd_decode_layer()` decodes a layer to interleaved pixels.
* `tinydng_psd_decode_layer_channel()` decodes one layer channel or mask channel.
* `tinydng_psd_read_block()` copies raw image-resource or tagged-block bytes.
* `tinydng_psd_decode_thumbnail()` decodes Photoshop JPEG thumbnail resources 1036 or 1033.
* `tinydng_psd_smart_object_open()` opens an embedded PSD/PSB/TIFF/DNG smart object as a nested `tinydng_document`.
* `tinydng_psd_smart_object_decode()` decodes embedded PSD/PSB/TIFF/DNG/JPEG/PNG smart-object payloads to pixels.

Define `TINYDNG_NO_PSD` to compile PSD/PSB support out.

### Writing PSD/PSB (v3 C API)

Use `tinydng_psd_write_memory()` or `tinydng_psd_write_file()` with a `tinydng_psd_write_doc`. The writer accepts interleaved composite pixels in host byte order and optional layer planes. Set `tinydng_psd_write_options::as_psb` to write PSB, otherwise PSD is emitted.

```c
tinydng_psd_write_doc doc;
tinydng_psd_write_options opts;

memset(&doc, 0, sizeof(doc));
doc.width = width;
doc.height = height;
doc.depth = 16;
doc.color_mode = TINYDNG_PSD_RGB;
doc.channel_count = 3;
doc.composite = rgb16_interleaved;
doc.composite_size = (size_t)width * height * 3 * 2;

memset(&opts, 0, sizeof(opts));
opts.compression = TINYDNG_PSD_COMP_RLE; /* or TINYDNG_PSD_COMP_RAW */

if (tinydng_psd_write_file(ctx, "out.psd", &doc, &opts, &err) != TINYDNG_OK) {
  fprintf(stderr, "%s\n", err.message);
}
```

The PSD writer currently supports 8/16/32-bit composite and layer data, RGB/grayscale/indexed and other Photoshop color-mode headers, RAW or RLE(PackBits) output compression, UTF-8 layer names, group section markers, alpha layer channels and optional ICC profile resource 1039. Mask-channel writing is not currently supported.

### PSD/PSB tests

The CMake test target `test_v3_psd` covers PSD/PSB writer-reader round trips, RAW/RLE, 8/16/32-bit depths, ZIP and ZIP-with-prediction reader paths, 1-bit composites, group trees, multi-threaded decode determinism, truncation checks and smart-object nesting. With an external corpus directory, run `test_v3_psd corpus <dir>` to open and fully decode every `*.psd` and `*.psb` in that directory.

## Customizations

* `TINY_DNG_LOADER_USE_THREAD` : Enable threaded loading(requires C++11)
* `TINY_DNG_LOADER_ENABLE_ZIP` : Enable decoding AdobeDeflate image(Currently, tiled RGB image only).
  * `TINY_DNG_LOADER_USE_SYSTEM_ZLIB` : Use system's zlib library instead of miniz.
* `TINY_DNG_LOADER_DEBUG` : Enable debug printf(developer only!)
* `TINY_DNG_LOADER_NO_STB_IMAGE_INCLUDE` : Do not include `stb_image.h` inside of `tiny_dng_loader.h`.
* `TINY_DNG_LOADER_NO_STDIO` : Disable printf, cout/cerr.

## Examples

* [examples/custom_fields](examples/viewer) Write a DNG with custom TIFF field.
* [examples/viewer](examples/viewer) Simple viewer example with simple debayering.
* [examples/tiff_viewer](examples/tiff_viewer) Simple TIFF viewer example(assume TIFF RGB image stored in DNG).
* [examples/fptiff2exr](examples/fptiff2exr) 32bit float(SAMPLEFORMAT_IEEEFP) grayscale or RGB image to EXR converter.
* [examples/dng2exr](examples/dng2exr) Simple DNG to OpenEXR converter.
* [examples/dngwriter](examples/dngwriter) Simple DNG writer example.

* https://github.com/storyboardcreativity/zraw-decoder

## Python binding(Experimental)

```
$ python -m pip install tinydng
```

Windows(including ARM), Linux(including aarch64) and macOS are supported.

See [experimental/python](experimental/python) for exsample python code.

### Command line tools(Experimental)

When TinyDNG is installed from pip, CLI command `tinydng` is available.

```
$ tinydng input.dng
```

### WebAssembly demo

The maintained browser build lives in [web](web/). It uses the v3 pure C API
to decode DNG/TIFF files in WebAssembly and includes a metadata/raw-development
demo with a WebGL2 tiled GPU path. Build it with an Emscripten environment:

```bash
emcmake cmake -S web -B web/build -DCMAKE_BUILD_TYPE=Release
cmake --build web/build
python3 -m http.server 8000
```

Open <http://localhost:8000/web/js/>. The former
[experimental/emscripten](experimental/emscripten) project is deprecated.

## Fuzzing test

* [fuzzer](fuzzer/) Fuzzing test.


## Resource

Here is the list of great articles on how to decode RAW file and how to develop RAW image.

* Developing a RAW photo file 'by hand' - Part 1 http://www.odelama.com/photo/Developing-a-RAW-Photo-by-hand/
* Developing a RAW photo file 'by hand' - Part 2 http://www.odelama.com/photo/Developing-a-RAW-Photo-by-hand/Developing-a-RAW-Photo-by-hand_Part-2/
* Understanding What is stored in a Canon RAW .CR2 file, How and Why http://lclevy.free.fr/cr2/

## TODO

* [ ] Move to C++11.
  * [x] Drop C++03 support.
* [x] Parse semantic map tags in Apple ProRAW.
* [ ] Add DNG header load only mode
* [ ] Parse more DNG headers
* [ ] Parse more custom DNG(TIFF) tags
* [ ] lossy DNG
* [ ] Improve DNG writer
  * [x] Support compression(LJPEG)
* [ ] Support Big TIFF(4GB+)
* [ ] Decode Nikon RAW(NEF)
* [ ] Improve Canon RAW decoding
* [ ] Optimimze lossless JPEG decoding
* [ ] Delayed load of tiled image.

## License

TinyDNG is licensed under MIT license.

TinyDNG uses the following third party libraries.

* liblj92(Lossless JPEG library) : (c) Andrew Baldwin 2014. MIT license.  https://bitbucket.org/baldand/mlrawviewer.git
* stb_image : Public domain image loader.
* lzw.hpp : Author: Guilherme R. Lampert. Public domain LZW decoder.
* miniz : Copyright 2013-2014 RAD Game Tools and Valve Software. Copyright 2010-2014 Rich Geldreich and Tenacious Software LLC MIT license. See `miniz.LICENSE`
