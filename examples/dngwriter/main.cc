
#define TINY_DNG_LOADER_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#include "../../tiny_dng_loader.h"

#define TINY_DNG_WRITER_IMPLEMENTATION
#include "tiny_dng_writer.h"

#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <iostream>

int
main(int argc, char **argv)
{
  std::string output_filename = "output.dng";

  if (argc < 1) {
    std::cout << argv[0] << " <output.dng>" << std::endl;
  }

  if (argc > 1) {
    output_filename = std::string(argv[1]);
  }

  tinydngwriter::DNGImage dng_image;
  unsigned int image_width = 512;
  unsigned int image_height = 512;
  dng_image.SetSubfileType(tinydngwriter::FILETYPE_REDUCEDIMAGE);
  dng_image.SetImageWidth(image_width);
  dng_image.SetImageLength(image_height);
  dng_image.SetRowsPerStrip(image_height);
  dng_image.SetBitsPerSample(16);
  dng_image.SetPlanarConfig(tinydngwriter::PLANARCONFIG_CONTIG);
  dng_image.SetCompression(tinydngwriter::COMPRESSION_NONE);
  dng_image.SetPhotometric(tinydngwriter::PHOTOMETRIC_RGB);
  dng_image.SetSamplesPerPixel(3);

  std::vector<unsigned short> buf;
  buf.resize(image_width * image_height * 3);

  for (size_t y = 0; y < image_height; y++) {
    for (size_t x = 0; x < image_width; x++) {
      buf[3 * (y * image_width + x) + 0] = static_cast<unsigned short>(x % 512);
      buf[3 * (y * image_width + x) + 1] = static_cast<unsigned short>(y % 512);
      buf[3 * (y * image_width + x) + 2] = static_cast<unsigned short>(12384);
    }
  }

  dng_image.SetImageData(reinterpret_cast<unsigned char *>(buf.data()), buf.size() * sizeof(unsigned short));


  //std::cout << "datalen = " << dng_image.GetDataSize() << std::endl;
  
  tinydngwriter::DNGWriter dng_writer;
  assert(dng_writer.AddImage(&dng_image));

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
