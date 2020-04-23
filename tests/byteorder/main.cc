#include <cstdio>
#include <cassert>
#include <cstdlib>
#include <iostream>

#include <tiffio.h>

int main(int argc, char **argv)
{
  if (argc < 2) {
    std::cout << "Needs input.tif\n";
    return EXIT_FAILURE;
  }

  TIFF* tif = TIFFOpen(argv[1], "r");
  if (tif) {
    tdata_t buf;
    tstrip_t strip;

    // TODO: count is N(SamplesPerPixel)
    uint16_t sampleformat;
    if (!TIFFGetField(tif, TIFFTAG_SAMPLEFORMAT, &sampleformat)) {
      sampleformat = 1;
    }

    uint16_t bitspersample;
    if (!TIFFGetField(tif, TIFFTAG_BITSPERSAMPLE, &bitspersample)) {
      std::cout << "TIFFTAG_BITSPERSAMPLE not found\n";
      return -1;
    }

    std::cout << "bitspersample = " << bitspersample << "\n";
    std::cout << "sampleformat = " << sampleformat << "\n";

    std::cout << "strip size: " << TIFFStripSize(tif) << "\n";

    buf = _TIFFmalloc(TIFFStripSize(tif));
  	for (strip = 0; strip < TIFFNumberOfStrips(tif); strip++) {
		  TIFFReadEncodedStrip(tif, strip, buf, (tsize_t) -1);
      if (sampleformat == 3) { // IEEEFP
        assert(bitspersample == 32); // TODO: 64bit float
        const float *pixels = reinterpret_cast<const float *>(buf);
        const size_t num_pixels = TIFFStripSize(tif) / sizeof(float);
        for (size_t i = 0; i < num_pixels; i++) {
          std::cout << "val[" << i << "] = " << pixels[i] << "\n";
        }
      } else if (sampleformat == 1) { // uint
        if (bitspersample == 16) {
          const uint16_t *pixels = reinterpret_cast<const uint16_t *>(buf);
          const size_t num_pixels = TIFFStripSize(tif) / sizeof(uint16_t);
          for (size_t i = 0; i < num_pixels; i++) {
            std::cout << "val[" << i << "] = " << pixels[i] << "\n";
          }
        }
      }
    }
	  _TIFFfree(buf);
	  TIFFClose(tif);
  }

  return EXIT_SUCCESS;
}
