#define TINYEXR_IMPLEMENTATION
#include "tinyexr.h"

#define TINY_DNG_LOADER_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#include "../../tiny_dng_loader.h"

#include <iostream>

#if defined(TINYEXR_USE_ZFP)

static int SaveEXRWithZfp(const float *data, int width, int height, int components, double tolerance, const char *outfilename, const char **err) {
  if ((components == 1) || components == 3 || components == 4) {
    // OK
  } else {
    std::stringstream ss;
    ss << "Unsupported component value : " << components << std::endl;

    tinyexr::SetErrorMessage(ss.str(), err);
    return TINYEXR_ERROR_INVALID_ARGUMENT;
  }

  EXRHeader header;
  InitEXRHeader(&header);

  if ((width < 16) && (height < 16)) {
    // No compression for small image.
    header.compression_type = TINYEXR_COMPRESSIONTYPE_NONE;
  } else {
    header.compression_type = TINYEXR_COMPRESSIONTYPE_ZFP;
  }

  // Write ZFP custom attributes.

  unsigned char zfp_compression_type = TINYEXR_ZFP_COMPRESSIONTYPE_ACCURACY;
  double zfp_compression_tolerance = tolerance;

  header.num_custom_attributes = 2;
  header.custom_attributes = reinterpret_cast<EXRAttribute *>(malloc(sizeof(EXRAttribute) * size_t(header.custom_attributes)));

  memcpy(header.custom_attributes[0].name, "zfpCompressionType", strlen("zfpCompressionType"));
  header.custom_attributes[0].name[strlen("zfpCompressionType")] = '\0';

  memcpy(header.custom_attributes[0].type, "uchar", strlen("uchar"));
  header.custom_attributes[0].type[strlen("uchar")] = '\0';

  header.custom_attributes[0].size = 1;
  header.custom_attributes[0].value = reinterpret_cast<unsigned char*>(malloc(1));
  memcpy(header.custom_attributes[0].value, &zfp_compression_type, 1);

  memcpy(header.custom_attributes[1].name, "zfpCompressionTolerance", strlen("zfpCompressionTolerance"));
  header.custom_attributes[1].name[strlen("zfpCompressionTolerance")] = '\0';

  memcpy(header.custom_attributes[1].type, "double", strlen("double"));
  header.custom_attributes[1].type[strlen("double")] = '\0';

  header.custom_attributes[1].size = sizeof(double);
  header.custom_attributes[1].value = reinterpret_cast<unsigned char*>(malloc(sizeof(double)));
  memcpy(header.custom_attributes[1].value, &zfp_compression_tolerance, sizeof(double));


  EXRImage image;
  InitEXRImage(&image);

  image.num_channels = components;

  std::vector<float> images[4];

  if (components == 1) {
    images[0].resize(static_cast<size_t>(width * height));
    memcpy(images[0].data(), data, sizeof(float) * size_t(width * height));
  } else {
    images[0].resize(static_cast<size_t>(width * height));
    images[1].resize(static_cast<size_t>(width * height));
    images[2].resize(static_cast<size_t>(width * height));
    images[3].resize(static_cast<size_t>(width * height));

    // Split RGB(A)RGB(A)RGB(A)... into R, G and B(and A) layers
    for (size_t i = 0; i < static_cast<size_t>(width * height); i++) {
      images[0][i] = data[static_cast<size_t>(components) * i + 0];
      images[1][i] = data[static_cast<size_t>(components) * i + 1];
      images[2][i] = data[static_cast<size_t>(components) * i + 2];
      if (components == 4) {
        images[3][i] = data[static_cast<size_t>(components) * i + 3];
      }
    }
  }

  float *image_ptr[4] = {nullptr, nullptr, nullptr, nullptr};
  if (components == 4) {
    image_ptr[0] = &(images[3].at(0));  // A
    image_ptr[1] = &(images[2].at(0));  // B
    image_ptr[2] = &(images[1].at(0));  // G
    image_ptr[3] = &(images[0].at(0));  // R
  } else if (components == 3) {
    image_ptr[0] = &(images[2].at(0));  // B
    image_ptr[1] = &(images[1].at(0));  // G
    image_ptr[2] = &(images[0].at(0));  // R
  } else if (components == 1) {
    image_ptr[0] = &(images[0].at(0));  // A
  }

  image.images = reinterpret_cast<unsigned char **>(image_ptr);
  image.width = width;
  image.height = height;

  header.num_channels = components;
  header.channels = static_cast<EXRChannelInfo *>(malloc(
      sizeof(EXRChannelInfo) * static_cast<size_t>(header.num_channels)));
  // Must be (A)BGR order, since most of EXR viewers expect this channel order.
  if (components == 4) {
#ifdef _MSC_VER
    strncpy_s(header.channels[0].name, "A", 255);
    strncpy_s(header.channels[1].name, "B", 255);
    strncpy_s(header.channels[2].name, "G", 255);
    strncpy_s(header.channels[3].name, "R", 255);
#else
    strncpy(header.channels[0].name, "A", 255);
    strncpy(header.channels[1].name, "B", 255);
    strncpy(header.channels[2].name, "G", 255);
    strncpy(header.channels[3].name, "R", 255);
#endif
    header.channels[0].name[strlen("A")] = '\0';
    header.channels[1].name[strlen("B")] = '\0';
    header.channels[2].name[strlen("G")] = '\0';
    header.channels[3].name[strlen("R")] = '\0';
  } else if (components == 3) {
#ifdef _MSC_VER
    strncpy_s(header.channels[0].name, "B", 255);
    strncpy_s(header.channels[1].name, "G", 255);
    strncpy_s(header.channels[2].name, "R", 255);
#else
    strncpy(header.channels[0].name, "B", 255);
    strncpy(header.channels[1].name, "G", 255);
    strncpy(header.channels[2].name, "R", 255);
#endif
    header.channels[0].name[strlen("B")] = '\0';
    header.channels[1].name[strlen("G")] = '\0';
    header.channels[2].name[strlen("R")] = '\0';
  } else {
#ifdef _MSC_VER
    strncpy_s(header.channels[0].name, "A", 255);
#else
    strncpy(header.channels[0].name, "A", 255);
#endif
    header.channels[0].name[strlen("A")] = '\0';
  }

  header.pixel_types = static_cast<int *>(
      malloc(sizeof(int) * static_cast<size_t>(header.num_channels)));
  header.requested_pixel_types = static_cast<int *>(
      malloc(sizeof(int) * static_cast<size_t>(header.num_channels)));
  for (int i = 0; i < header.num_channels; i++) {
    header.pixel_types[i] =
        TINYEXR_PIXELTYPE_FLOAT;  // pixel type of input image

    header.requested_pixel_types[i] =
        TINYEXR_PIXELTYPE_FLOAT;  // save with float(fp32) pixel format(i.e.
                                    // no precision reduction)
  }

  int ret = SaveEXRImageToFile(&image, &header, outfilename, err);
  if (ret != TINYEXR_SUCCESS) {
    return ret;
  }

  free(header.channels);
  free(header.pixel_types);
  free(header.requested_pixel_types);

  // TODO(syoyo): Fix memory leak of custom attributes.

  return ret;
}

#endif

int
main(int argc, char **argv)
{
  bool use_zfp = false;
  double zfp_tolerance = 0.0;

  size_t image_idx = static_cast<size_t>(-1); // -1 = use largest image.
  if (argc < 3) {
    std::cout << "tiff2exr input.tiff output.exr (image_idx) (use_zfp) (zfp_tolerance)" << std::endl;
    std::cout << "  options" << std::endl;
    std::cout << "    image_idx           : The index of image for conversion. Default = -1 = auto detect(choose the largest image in TIFF)." << std::endl;
    std::cout << "    use_zfp(when TINYEXR_USE_ZFP is defined) : 1 to use ZFP. Default 0" << std::endl;
    std::cout << "    zfp_tolerance(when TINYEXR_USE_ZFP is defined) : Tolerance value(double). Default 0.0" << std::endl;
    return EXIT_FAILURE;
  }

  std::string input_filename = std::string(argv[1]);
  std::string output_filename = std::string(argv[2]);

  if (argc > 3) {
    image_idx = static_cast<size_t>(atoi(argv[3]));
  }

  if (argc > 4) {
    use_zfp = bool(atoi(argv[4]));
  }

  if (argc > 5) {
    zfp_tolerance = atof(argv[5]);
  }

  std::vector<tinydng::DNGImage> images;
  {
    std::vector<tinydng::FieldInfo> custom_field_list;
    std::string warn, err;
    bool ret =
        tinydng::LoadDNG(input_filename.c_str(), custom_field_list, &images, &warn, &err);

    if (!warn.empty()) {
      std::cout << "WARN: " << warn << std::endl;
    }

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

  const char *exr_err;

#if defined(TINYEXR_USE_ZFP)
  int ret;

  if (use_zfp) {
    std::cout << "Use zfp. tolerance = " << zfp_tolerance << "\n";

    ret = SaveEXRWithZfp(image_data, images[image_idx].width, images[image_idx].height, images[image_idx].samples_per_pixel, zfp_tolerance, output_filename.c_str(), &exr_err);
  } else {
    ret = SaveEXR(image_data, images[image_idx].width, images[image_idx].height, images[image_idx].samples_per_pixel, /* fp16 */0, output_filename.c_str(), &exr_err);
  }

#else
  int ret = SaveEXR(image_data, images[image_idx].width, images[image_idx].height, images[image_idx].samples_per_pixel, /* fp16 */0, output_filename.c_str(), &exr_err);
#endif

  if (ret != TINYEXR_SUCCESS) {
    if (exr_err) {
      std::cerr << "Err: " << exr_err << std::endl;
      FreeEXRErrorMessage(exr_err);
    }
    std::cout << "Save EXR failure: err code = " << ret << std::endl;
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;

}
