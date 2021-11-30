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

static void CreateGrayscale32bitFpTiff(tinydngwriter::DNGImage *dng_image) {
  unsigned int image_width = 512;
  unsigned int image_height = 512;
  dng_image->SetSubfileType(false, false, false);
  dng_image->SetImageWidth(image_width);
  dng_image->SetImageLength(image_height);
  dng_image->SetRowsPerStrip(image_height);
  dng_image->SetSamplesPerPixel(1);
  uint16_t bps =32;
  dng_image->SetBitsPerSample(1, &bps);
  dng_image->SetPlanarConfig(tinydngwriter::PLANARCONFIG_CONTIG);
  dng_image->SetCompression(tinydngwriter::COMPRESSION_NONE);
  dng_image->SetPhotometric(
      tinydngwriter::PHOTOMETRIC_BLACK_IS_ZERO);  // grayscale
  dng_image->SetXResolution(1.0);
  dng_image->SetYResolution(1.1);
  dng_image->SetResolutionUnit(tinydngwriter::RESUNIT_NONE);

  uint16_t format = tinydngwriter::SAMPLEFORMAT_IEEEFP;
  dng_image->SetSampleFormat(1, &format);

  std::vector<float> buf;
  buf.resize(image_width * image_height);

  for (size_t y = 0; y < image_height; y++) {
    for (size_t x = 0; x < image_width; x++) {
      buf[(y * image_width + x)] = float(y * image_width + x);
    }
  }

  dng_image->SetImageData(reinterpret_cast<unsigned char *>(buf.data()),
                          buf.size() * sizeof(float));
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

  // fp32 tiff test
  {
    tinydngwriter::DNGImage tiff;
    tiff.SetBigEndian(big_endian);

    CreateGrayscale32bitFpTiff(&tiff);

    tinydngwriter::DNGWriter dng_writer(big_endian);
    bool ret = dng_writer.AddImage(&tiff);
    assert(ret);

    std::string err;
    ret = dng_writer.WriteToFile("output-fp32-grayscale.tiff", &err);

    if (!err.empty()) {
      std::cerr << err;
    }

    if (!ret) {
      return EXIT_FAILURE;
    }

  }
  return EXIT_SUCCESS;
}
