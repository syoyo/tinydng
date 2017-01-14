
#define TINY_DNG_LOADER_IMPLEMENTATION
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
  dng_writer.SetImageWidth(image_width);
  dng_writer.SetImageLength(image_height);
  dng_writer.SetPhotometric(tinydngwriter::PHOTOMETRIC_RGB);
  dng_writer.SetBitsPerSample(16);

  std::vector<unsigned char> buf;
  buf.resize(image_width * image_height * sizeof(short));

  dng_writer.SetImageData(buf.data(), buf.size());

  std::string err;
  bool ret = dng_writer.WriteToFile("output.dng", &err);

  if (!err.empty()) {
    std::cerr << err;
  }

  if (!ret) {
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
