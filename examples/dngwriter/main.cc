
#define TINY_DNG_LOADER_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#include "../../tiny_dng_loader.h"

#define TINY_DNG_WRITER_IMPLEMENTATION
#include "tiny_dng_writer.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>

int
main(int argc, char **argv)
{
  std::string output_filename = "output.dng";

  if (argc < 2) {
    std::cout << argv[0] << " input.dng <output.dng>" << std::endl;
    return EXIT_FAILURE;
  }

  if (argc > 2) {
    output_filename = std::string(argv[2]);
  }

  tinydngwriter::DNGWriter dng_writer;
  unsigned int image_width = 512;
  unsigned int image_height = 512;
  dng_writer.SetSubfileType(tinydngwriter::FILETYPE_REDUCEDIMAGE);
  dng_writer.SetImageWidth(image_width);
  dng_writer.SetImageLength(image_height);
  dng_writer.SetRowsPerStrip(image_height);
  dng_writer.SetBitsPerSample(16);
  dng_writer.SetPlanarConfig(tinydngwriter::PLANARCONFIG_CONTIG);
  dng_writer.SetCompression(tinydngwriter::COMPRESSION_NONE);
  dng_writer.SetPhotometric(tinydngwriter::PHOTOMETRIC_RGB);
  dng_writer.SetSamplesPerPixel(1);

  std::vector<unsigned short> buf;
  buf.resize(image_width * image_height * 3);

  for (size_t y = 0; y < image_height; y++) {
    for (size_t x = 0; x < image_width; x++) {
      buf[3 * (y * image_width + x) + 0] = static_cast<unsigned short>(x % 512);
      buf[3 * (y * image_width + x) + 1] = static_cast<unsigned short>(y % 512);
      buf[3 * (y * image_width + x) + 2] = static_cast<unsigned short>(12384);
    }
  }

  dng_writer.SetImageData(reinterpret_cast<unsigned char *>(buf.data()), buf.size() * sizeof(unsigned short));

  std::string err;
  bool ret = dng_writer.WriteToFile(output_filename.c_str(), &err);

  if (!err.empty()) {
    std::cerr << err;
  }

  if (!ret) {
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
