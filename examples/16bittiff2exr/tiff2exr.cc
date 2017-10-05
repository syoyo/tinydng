#define TINYEXR_IMPLEMENTATION
#include "tinyexr.h"

#define TINY_DNG_LOADER_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#include "../../tiny_dng_loader.h"

#include <iostream>

int
main(int argc, char **argv)
{
  size_t image_idx = static_cast<size_t>(-1); // -1 = use largest image.
  if (argc < 3) {
    std::cout << "tiff2exr input.tiff output.exr (image_idx) (dont_normalize)" << std::endl;
    std::cout << "  options" << std::endl;
    std::cout << "    image_idx           : The index of image for conversion. Default = auto detect(choose the largest image in TIFF)." << std::endl;
    std::cout << "    dont_normalize      : Do not normalize pixel value. Output pixel value in the range [0, 65535] when this flag is set(`1'). Default: normalize pixel value [0, 65535] to [0.0, 1.0]" << std::endl;
    return EXIT_FAILURE;
  }

  std::string input_filename = std::string(argv[1]);
  std::string output_filename = std::string(argv[2]);
  bool normalize = true;

  if (argc > 3) {
    image_idx = static_cast<size_t>(atoi(argv[3]));
  }

  if (argc > 4) {
    normalize = static_cast<bool>(atoi(argv[4]));
  }

  std::vector<tinydng::DNGImage> images;
  {
    std::vector<tinydng::FieldInfo> custom_field_list;
    std::string err;
    bool ret =
        tinydng::LoadDNG(input_filename.c_str(), custom_field_list, &images, &err);

    if (!err.empty()) {
      std::cout << err << std::endl;
    }

    if (ret == false) {
      std::cout << "failed to load DNG" << std::endl;
      return EXIT_FAILURE;
    }

  }
  assert(images.size() > 0);

  for (size_t i = 0; i < images.size(); i++) {
      std::cout << "Image [ " << i << " ] size = " << images[i].width << " x " << images[i].height << std::endl;
  }

  if (image_idx == static_cast<size_t>(-1)) {
    // Find largest image based on width.
    size_t largest = 0;
    int largest_width = images[0].width;
    for (size_t i = 1; i < images.size(); i++) {
      if (largest_width < images[i].width) {
        largest = i;
        largest_width = images[i].width;
      } 
    }

    image_idx = static_cast<size_t>(largest);
  }

  std::cout << "Use image [ " << image_idx << " ] " << std::endl;

  if (images[image_idx].bits_per_sample != 16) {
    std::cerr << "Currently the converter supports only 16bit image." << std::endl;
    return EXIT_FAILURE;
  }

  if ((images[image_idx].samples_per_pixel == 1) ||
      (images[image_idx].samples_per_pixel == 3)) {
    // ok
  } else {
    std::cerr << "Samples per pixel must be 1 or 3, but get " << images[image_idx].samples_per_pixel << std::endl;
    return EXIT_FAILURE;
  }

  const tinydng::DNGImage &image = images[image_idx];
  {
    std::vector<float> buf;
    buf.resize(size_t(image.width * image.height * image.samples_per_pixel));

    const size_t width = size_t(image.width);
    const size_t height = size_t(image.height);

    const float scale = normalize ? (1.0f / 65535.0f) : 1.0f; 
    const unsigned short *image_data = reinterpret_cast<const unsigned short *>(image.data.data());

    if (image.samples_per_pixel == 1) {

      for (size_t y = 0; y < height; y++) {
        for (size_t x = 0; x < width; x++) {
          buf[y * width + x] = scale * image_data[y * width + x];
        }
      }

    } else if (image.samples_per_pixel == 3) {

      for (size_t y = 0; y < height; y++) {
        for (size_t x = 0; x < width; x++) {
          buf[3 * (y * width + x) + 0] = scale * image_data[3 * (y * width + x) + 0];
          buf[3 * (y * width + x) + 1] = scale * image_data[3 * (y * width + x) + 1];
          buf[3 * (y * width + x) + 2] = scale * image_data[3 * (y * width + x) + 2];
        }
      }

    } else {
      // ???
      return EXIT_FAILURE; 
    }

    int ret = SaveEXR(buf.data(), image.width, image.height, image.samples_per_pixel, output_filename.c_str());
    if (ret != TINYEXR_SUCCESS) {
      std::cout << "Save EXR failure: err code = " << ret << std::endl;
      return EXIT_FAILURE;
    }

  }


  return EXIT_SUCCESS;
  
}
