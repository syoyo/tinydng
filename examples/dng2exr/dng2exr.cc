#define TINYEXR_IMPLEMENTATION
#include "tinyexr.h"

#define TINY_DNG_LOADER_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#include "../../tiny_dng_loader.h"

#include <iostream>

static inline unsigned short swap2(unsigned short val) {
  unsigned short ret;

  unsigned char* buf = reinterpret_cast<unsigned char*>(&ret);

  unsigned short x = val;
  buf[1] = static_cast<unsigned char>(x);
  buf[0] = static_cast<unsigned char>(x >> 8);

  return ret;
}

//
// Decode 12bit integer image into floating point HDR image
//
static void decode12_hdr(std::vector<float>& image, unsigned char* data, int width,
                  int height, bool do_swap) {
  int offsets[2][2] = {{0, 1}, {1, 2}};

  int bit_shifts[2] = {4, 0};

  image.resize(static_cast<size_t>(width * height));

#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic, 1)
#endif
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      unsigned char buf[3];

      // Calculate load addres for 12bit pixel(three 8 bit pixels)
      int n = int(y * width + x);

      // 24 = 12bit * 2 pixel, 8bit * 3 pixel
      int n2 = n % 2;           // used for offset & bitshifts
      int addr3 = (n / 2) * 3;  // 8bit pixel pos
      int odd = (addr3 % 2);

      int bit_shift;
      bit_shift = bit_shifts[n2];

      int offset[2];
      offset[0] = offsets[n2][0];
      offset[1] = offsets[n2][1];

      if (do_swap) {
        // load with short byte swap
        if (odd) {
          buf[0] = data[addr3 - 1];
          buf[1] = data[addr3 + 2];
          buf[2] = data[addr3 + 1];
        } else {
          buf[0] = data[addr3 + 1];
          buf[1] = data[addr3 + 0];
          buf[2] = data[addr3 + 3];
        }
      } else {
        buf[0] = data[addr3 + 0];
        buf[1] = data[addr3 + 1];
        buf[2] = data[addr3 + 2];
      }
      unsigned int b0 = static_cast<unsigned int>(buf[offset[0]] & 0xff);
      unsigned int b1 = static_cast<unsigned int>(buf[offset[1]] & 0xff);

      unsigned int val = (b0 << 8) | b1;
      val = 0xfff & (val >> bit_shift);

      image[static_cast<size_t>(y * width + x)] = static_cast<float>(val);
    }
  }
}

//
// Decode 14bit integer image into floating point HDR image
//
static void decode14_hdr(std::vector<float>& image, unsigned char* data, int width,
                  int height, bool do_swap) {
  int offsets[4][3] = {{0, 0, 1}, {1, 2, 3}, {3, 4, 5}, {5, 5, 6}};

  int bit_shifts[4] = {2, 4, 6, 0};

  image.resize(static_cast<size_t>(width * height));

#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic, 1)
#endif
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      unsigned char buf[7];

      // Calculate load addres for 14bit pixel(three 8 bit pixels)
      int n = int(y * width + x);

      // 56 = 14bit * 4 pixel, 8bit * 7 pixel
      int n4 = n % 4;           // used for offset & bitshifts
      int addr7 = (n / 4) * 7;  // 8bit pixel pos
      int odd = (addr7 % 2);

      int offset[3];
      offset[0] = offsets[n4][0];
      offset[1] = offsets[n4][1];
      offset[2] = offsets[n4][2];

      int bit_shift;
      bit_shift = bit_shifts[n4];

      if (do_swap) {
        // load with short byte swap
        if (odd) {
          buf[0] = data[addr7 - 1];
          buf[1] = data[addr7 + 2];
          buf[2] = data[addr7 + 1];
          buf[3] = data[addr7 + 4];
          buf[4] = data[addr7 + 3];
          buf[5] = data[addr7 + 6];
          buf[6] = data[addr7 + 5];
        } else {
          buf[0] = data[addr7 + 1];
          buf[1] = data[addr7 + 0];
          buf[2] = data[addr7 + 3];
          buf[3] = data[addr7 + 2];
          buf[4] = data[addr7 + 5];
          buf[5] = data[addr7 + 4];
          buf[6] = data[addr7 + 7];
        }
      } else {
        memcpy(buf, &data[addr7], 7);
      }
      unsigned int b0 = static_cast<unsigned int>(buf[offset[0]] & 0xff);
      unsigned int b1 = static_cast<unsigned int>(buf[offset[1]] & 0xff);
      unsigned int b2 = static_cast<unsigned int>(buf[offset[2]] & 0xff);

      // unsigned int val = (b0 << 16) | (b1 << 8) | b2;
      // unsigned int val = (b2 << 16) | (b0 << 8) | b0;
      unsigned int val = (b0 << 16) | (b1 << 8) | b2;
      // unsigned int val = b2;
      val = 0x3fff & (val >> bit_shift);

      image[static_cast<size_t>(y * width + x)] = static_cast<float>(val);
    }
  }
}

//
// Decode 16bit integer image into floating point HDR image
//
static void decode16_hdr(std::vector<float>& image, unsigned char* data, int width,
                  int height, bool do_swap) {
  image.resize(static_cast<size_t>(width * height));
  unsigned short* ptr = reinterpret_cast<unsigned short*>(data);

#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic, 1)
#endif
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      unsigned short val = ptr[y * width + x];
      if (do_swap) {
        val = swap2(val);
      }

      // range will be [0, 65535]
      image[static_cast<size_t>(y * width + x)] = static_cast<float>(val);
    }
  }
}

int
main(int argc, char **argv)
{
  size_t image_idx = static_cast<size_t>(-1); // -1 = use largest image.
  if (argc < 3) {
    std::cout << "dng2exr input.dng output.exr (normalize_intensity) (image_idx)" << std::endl;
    std::cout << "  options" << std::endl;
    std::cout << "    normalize_intensity : Normalize RAW pixel value to [0.0, 1.0] range. `0` or `1`. Default = `1`(true)" << std::endl;
    std::cout << "    image_idx           : The index of image to use(For DNG containing multiple images). Default = auto detect(choose the largest image)." << std::endl;
    return EXIT_FAILURE;
  }

  std::string input_filename = std::string(argv[1]);
  std::string output_filename = std::string(argv[2]);

  bool do_normalize = true;

  if (argc > 3) {
    do_normalize = static_cast<bool>(atoi(argv[3]));
  }

  if (argc > 4) {
    image_idx = static_cast<size_t>(atoi(argv[4]));
  }

  std::vector<tinydng::DNGImage> images;
  {
    std::string warn, err;
    std::vector<tinydng::FieldInfo> custom_field_list;
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

  // Convert to float.
  std::vector<float> hdr;
  bool do_swap = false;

  int spp = images[image_idx].samples_per_pixel;
  if (images[image_idx].bits_per_sample == 12) {
    decode12_hdr(hdr, &(images[image_idx].data.at(0)), images[image_idx].width, images[image_idx].height * spp, do_swap);
  } else if (images[image_idx].bits_per_sample == 14) {
    decode14_hdr(hdr, &(images[image_idx].data.at(0)), images[image_idx].width, images[image_idx].height * spp, do_swap);
  } else if (images[image_idx].bits_per_sample == 16) {
    decode16_hdr(hdr, &(images[image_idx].data.at(0)), images[image_idx].width, images[image_idx].height * spp, do_swap);
  } else {
    std::cerr << "Unsupported bits_per_sample : " << images[image_idx].samples_per_pixel << std::endl;
    exit(-1);
  }

  if (spp == 3) {

    if (do_normalize) {
      float inv_scale = 1.0f / static_cast<float>((1 << images[image_idx].bits_per_sample));
      for (size_t i = 0; i < hdr.size(); i++) {
        hdr[i] *= inv_scale;
      }
    }

    const char *exr_err;
    int ret = SaveEXR(&(hdr.at(0)), images[image_idx].width, images[image_idx].height, spp, /* fp16 */0, output_filename.c_str(), &exr_err);
    if (ret != TINYEXR_SUCCESS) {
      if (exr_err) {
        std::cout << "ERR: " << exr_err << std::endl;
        FreeEXRErrorMessage(exr_err);
      }
      std::cout << "Save EXR failure: err code = " << ret << std::endl;
      return EXIT_FAILURE;
    }
    std::cout << "Saved to " << output_filename << std::endl;

  } else if (spp == 1) {

    // Create grayscale image & normalize intensity.
    std::vector<float> tmp;
    tmp.resize(static_cast<size_t>(images[image_idx].width * images[image_idx].height * 3));

    float inv_scale = 1.0f;
    if (do_normalize) {
      inv_scale = 1.0f / static_cast<float>((1 << images[image_idx].bits_per_sample));
    }
    for (size_t i = 0; i < static_cast<size_t>(images[image_idx].width * images[image_idx].height); i++) {
      tmp[3 * i + 0] = hdr[i] * inv_scale;
      tmp[3 * i + 1] = hdr[i] * inv_scale;
      tmp[3 * i + 2] = hdr[i] * inv_scale;
    }

    const char *exr_err;
    int ret = SaveEXR(&(tmp.at(0)), images[image_idx].width, images[image_idx].height, 3, /* fp16 */0, output_filename.c_str(), &exr_err);
    if (ret != TINYEXR_SUCCESS) {
      if (exr_err) {
        std::cout << "ERR: " << exr_err << std::endl;
        FreeEXRErrorMessage(exr_err);
      }
      std::cout << "Save EXR failure: err code = " << ret << std::endl;
      return EXIT_FAILURE;
    }
    std::cout << "Saved to " << output_filename << std::endl;

  } else {
    std::cerr << "Unsupported samples per pixel: " << spp << std::endl;
    return EXIT_FAILURE;
  }
    
  return EXIT_SUCCESS;
  
}
