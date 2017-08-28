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
    std::cout << "tiff2exr input.tiff output.exr (image_idx)" << std::endl;
    std::cout << "  options" << std::endl;
    std::cout << "    image_idx           : The index of image for conversion. Default = auto detect(choose the largest image in TIFF)." << std::endl;
    return EXIT_FAILURE;
  }

  std::string input_filename = std::string(argv[1]);
  std::string output_filename = std::string(argv[2]);

  if (argc > 3) {
    image_idx = static_cast<size_t>(atoi(argv[3]));
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

  if (images[image_idx].bits_per_sample != 32) {
    std::cerr << "Currently the converter supports only 32bit image." << std::endl;
    return EXIT_FAILURE;
  }

  if (images[image_idx].sample_format != tinydng::SAMPLEFORMAT_IEEEFP) {
    std::cerr << "Currently the converter supports only 32bit floating point image." << std::endl;
    return EXIT_FAILURE;
  }  

  if ((images[image_idx].samples_per_pixel == 1) ||
      (images[image_idx].samples_per_pixel == 3)) {
    // ok
  } else {
    std::cerr << "Samples per pixel must be 1 or 3, but get " << images[image_idx].samples_per_pixel << std::endl;
    return EXIT_FAILURE;
  }

  const float *image_data = reinterpret_cast<const float *>(images[image_idx].data.data());

  int ret = SaveEXR(image_data, images[image_idx].width, images[image_idx].height, images[image_idx].samples_per_pixel, output_filename.c_str());
  if (ret != TINYEXR_SUCCESS) {
    std::cout << "Save EXR failure: err code = " << ret << std::endl;
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
  
}
