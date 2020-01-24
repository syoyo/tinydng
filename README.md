# Tiny DNG Loader

[![Build Status](https://travis-ci.org/syoyo/tinydngloader.svg?branch=master)](https://travis-ci.org/syoyo/tinydngloader)

Header-only simple&limited DNG(Digital NeGative, TIFF format + extension) loader in C++03.

Currently TinyDNGLoader only supports lossless RAW DNG and limited lossless JPEG DNG.

TinyDNGLoader can also be used as an TIFF RGB image loader(8bit, 16bit and 32bit are supported).

![](images/tinydngloader_viewer.png)
(NOTE: TinyDNGLoader just loads DNG data as is, thus you'll need your own RAW processing code(e.g. debayer) to get a developed image as shown the above)

## Features

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
* Experimental
  * Decode Canon RAW(CR2)
    * [x] RAW
    * [ ] mRAW
    * [ ] sRAW
  * Decode Nikon RAW(NEF)
    * TODO
  * Reading custom TIFF tags.
* [x] Read DNG data from memory.

## Supported DNG files

Here is the list of supported DNG files.

* [x] Sigma sd Quattro H
  * Uncompressed RGB 12bit image.
* [x] iPhone DNG
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

## Usage

```
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

## Customizations

* `TINY_DNG_LOADER_USE_THREAD` : Enable threaded loading(requires C++11)
* `TINY_DNG_LOADER_ENABLE_ZIP` : Enable decoding AdobeDeflate image(Currently, tiled RGB image only).
  * `TINY_DNG_LOADER_USE_SYSTEM_ZLIB` : Use system's zlib library instead of miniz.
* `TINY_DNG_NO_EXCEPTION` : disable C++ exception(abort the program when got an assertion)
* `TINY_DNG_LOADER_DEBUG` : Enable debug printf(developer only!)

## Examples

* [examples/custom_fields](examples/viewer) Write a DNG with custom TIFF field.
* [examples/viewer](examples/viewer) Simple viewer example with simple debayering.
* [examples/tiff_viewer](examples/tiff_viewer) Simple TIFF viewer example(assume TIFF RGB image stored in DNG).
* [examples/fptiff2exr](examples/fptiff2exr) 32bit float(SAMPLEFORMAT_IEEEFP) grayscale or RGB image to EXR converter.
* [examples/dng2exr](examples/dng2exr) Simple DNG to OpenEXR converter.
* [examples/dngwriter](examples/dngwriter) Simple DNG writer example.


## Resource

Here is the list of great articles on how to decode RAW file and how to develop RAW image.

* Developing a RAW photo file 'by hand' - Part 1 http://www.odelama.com/photo/Developing-a-RAW-Photo-by-hand/
* Developing a RAW photo file 'by hand' - Part 2 http://www.odelama.com/photo/Developing-a-RAW-Photo-by-hand/Developing-a-RAW-Photo-by-hand_Part-2/
* Understanding What is stored in a Canon RAW .CR2 file, How and Why http://lclevy.free.fr/cr2/

## TODO

* [ ] Parse more DNG headers
* [ ] Parse more custom DNG(TIFF) tags
* [ ] lossy DNG
* [ ] Improve DNG writer
  * [ ] Support compression
* [ ] Support Big TIFF(4GB+)
* [ ] Decode Nikon RAW(NEF)
* [ ] Improve Canon RAW decoding
* [ ] Optimimze lossless JPEG decoding
* [ ] Delayed load of tiled image.

## License

TinyDNGLoader is licensed under MIT license.

TinyDNGLoader uses the following third party libraries.

* liblj92(Lossless JPEG library) : (c) Andrew Baldwin 2014. MIT license.  https://bitbucket.org/baldand/mlrawviewer.git
* stb_image : Public domain image loader.
* lzw.hpp : Author: Guilherme R. Lampert. Public domain LZW decoder.
* miniz : Copyright 2013-2014 RAD Game Tools and Valve Software. Copyright 2010-2014 Rich Geldreich and Tenacious Software LLC MIT license. See `miniz.LICENSE`

