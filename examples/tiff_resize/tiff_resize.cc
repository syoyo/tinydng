#define TINYEXR_IMPLEMENTATION
#include "tinyexr.h"

#define TINY_DNG_LOADER_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#include "../../tiny_dng_loader.h"

#define TINY_DNG_WRITER_IMPLEMENTATION
#include "../dngwriter/tiny_dng_writer.h"

#include <iostream>

// http://www.exploringbinary.com/ten-ways-to-check-if-an-integer-is-a-power-of-two-in-c/
static int isPowerOfTwo (unsigned int x)
{
 while (((x % 2) == 0) && x > 1) /* While x is even and > 1 */
   x /= 2;
 return (x == 1);
}

static const std::string GetFileExtension(const std::string &filename) {
  if (filename.find_last_of(".") != std::string::npos)
    return filename.substr(filename.find_last_of(".") + 1);
  return "";
}

// Save as 16bit TIFF image.
static bool SaveTIFFImage(const std::string &filename, const unsigned short *src, const int width, const int height, const int spp)
{
  tinydngwriter::DNGImage image;

  image.SetSubfileType(tinydngwriter::FILETYPE_REDUCEDIMAGE);
  image.SetImageWidth(static_cast<unsigned int>(width));
  image.SetImageLength(static_cast<unsigned int>(height));
  image.SetRowsPerStrip(static_cast<unsigned int>(height));
  image.SetBitsPerSample(16);
  image.SetPlanarConfig(tinydngwriter::PLANARCONFIG_CONTIG);
  image.SetCompression(tinydngwriter::COMPRESSION_NONE);
  if (spp == 1) {
    image.SetPhotometric(tinydngwriter::PHOTOMETRIC_MINISBLACK); // grayscale.
  } else {
    image.SetPhotometric(tinydngwriter::PHOTOMETRIC_RGB);
  }
  image.SetSamplesPerPixel(static_cast<unsigned short>(spp));

  image.SetImageData(reinterpret_cast<const unsigned char *>(src), size_t(width * height * spp) * sizeof(unsigned short));

  tinydngwriter::DNGWriter writer;
  writer.AddImage(&image);

  std::string err;
  bool ret = writer.WriteToFile(filename.c_str(), &err);

  if (!err.empty()) {
    std::cerr << "Failed to write a TIFF. err : " << err << std::endl;
  }

  if (!ret) {
    std::cerr << "Failed to write a TIFF : " << filename << std::endl;
    return false;
  }

  return true;


}


int
main(int argc, char **argv)
{
  size_t image_idx = static_cast<size_t>(-1); // -1 = use largest image.
  if (argc < 4) {
    std::cout << "tiff_resize input.tiff output.tiff resize_factor (bayer) (image_idx)" << std::endl;
    std::cout << "  options" << std::endl;
    std::cout << "    resize_factor       : Resize image to (1/resize_factor). e.g, 2: half size, 4: 1/4 size." << std::endl;
    std::cout << "    bayer               : 1:Keep 2x2 Bayer for resizing image. 0:Do not consider Bayer pattern for resizing." << std::endl;
    std::cout << "    image_idx           : The index of image for conversion. Default = auto detect(choose the largest image in TIFF)." << std::endl;
    return EXIT_FAILURE;
  }

  std::string input_filename = std::string(argv[1]);
  std::string output_filename = std::string(argv[2]);
  const std::string output_ext = GetFileExtension(output_filename);
  if (output_ext.compare("tiff") != 0) {
    std::cerr << "File extension must be .tiff" << std::endl;
    return EXIT_FAILURE;
  }

  int resize_factor = atoi(argv[3]);

  if (!isPowerOfTwo(static_cast<unsigned int>(resize_factor))) {
    std::cerr << "resize_factor value must be power of two." << std::endl;
    return EXIT_FAILURE;
  }

  bool bayer = false;
  if (argc > 4) {
    bayer = bool(atoi(argv[4]));
  }

  if (argc > 5) {
    image_idx = static_cast<size_t>(atoi(argv[5]));
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
    const size_t width = size_t(image.width);
    const size_t height = size_t(image.height);

    const unsigned short *image_data = reinterpret_cast<const unsigned short *>(image.data.data());

    size_t step = size_t(resize_factor);
    size_t dst_width = width / size_t(resize_factor);
    size_t dst_height = height / size_t(resize_factor);
    if ((dst_width < 2) || (dst_height < 2)) {
      std::cerr << "Resized image too small." << std::endl;
      return EXIT_FAILURE;
    }

    if (bayer) {
      // multiple of 2.
      dst_width = (2 * dst_width) / 2;
      dst_height = (2 * dst_height) / 2;
    }

    std::vector<unsigned short> buf;
    buf.resize(dst_width * dst_height * size_t(image.samples_per_pixel));
    memset(buf.data(), 0, sizeof(dst_width * dst_height * size_t(image.samples_per_pixel) * sizeof(unsigned short)));

    if (image.samples_per_pixel == 1) {
      if (bayer) {
        // Assume 2x2 Bayer
        step *= 2;
        for (size_t y = 0, v = 0; y < height; y+=step, v+=2) {
          for (size_t x = 0, u = 0; x < width; x+=step, u+=2) {
            for (size_t t = 0; t < 2; t++) {
              if ((v + t) >= dst_height) {
                continue;
              }
              for (size_t s = 0; s < 2; s++) {
                if ((u + s) >= dst_width) {
                  continue;
                }
                unsigned short val = 0;
                if (((y + t) < height) && ((x + s) < width)) {
                  val = image_data[(y + t) * width + (x + s)];
                }
                buf[(v + t) * dst_width + (u + s)] = val;
                //printf("[%d, %d] = %d, width %d\n", int(u), int(v), buf[v * dst_width + u], int(dst_width));
              }
            }
          }
        }
      } else {
        for (size_t y = 0, v = 0; y < height; y+=step, v++) {
          for (size_t x = 0, u = 0; x < width; x+=step, u++) {
            buf[v * dst_width + u] = image_data[y * width + x];
            //printf("[%d, %d] = %d, width %d\n", int(u), int(v), buf[v * dst_width + u], int(dst_width));
          }
        }
      }

    } else if (image.samples_per_pixel == 3) {
      if (bayer) {
        // Assume 2x2 Bayer
        step *= 2;
        for (size_t y = 0, v = 0; y < height; y+=step, v+=2) {
          for (size_t x = 0, u = 0; x < width; x+=step, u+=2) {
            for (size_t t = 0; t < 2; t++) {
              if ((v + t) >= dst_height) {
                continue;
              }
              for (size_t s = 0; s < 2; s++) {
                if ((u + s) >= dst_width) {
                  continue;
                }
                unsigned short val0 = 0;
                unsigned short val1 = 0;
                unsigned short val2 = 0;
                if (((y + t) < height) && ((x + s) < width)) {
                  val0 = image_data[3 * ((y + t) * width + (x + s)) + 0];
                  val1 = image_data[3 * ((y + t) * width + (x + s)) + 1];
                  val2 = image_data[3 * ((y + t) * width + (x + s)) + 2];
                }
                buf[3 * ((v + t) * dst_width + (u + s)) + 0] = val0;
                buf[3 * ((v + t) * dst_width + (u + s)) + 1] = val1;
                buf[3 * ((v + t) * dst_width + (u + s)) + 2] = val2;
              }
            }
          }
        }
      } else {
        for (size_t y = 0, v = 0; y < height; y+=step, v++) {
          for (size_t x = 0, u = 0; x < width; x+=step, u++) {
            buf[3 * (v * dst_width + u) + 0] = image_data[3 * (v * width + u) + 0];
            buf[3 * (v * dst_width + u) + 1] = image_data[3 * (v * width + u) + 1];
            buf[3 * (v * dst_width + u) + 2] = image_data[3 * (v * width + u) + 2];
          }
        }
      }

    } else {
      // ???
      return EXIT_FAILURE; 
    }

#if 0
    if (output_ext.compare("exr") == 0) {
      int ret = SaveEXR(buf.data(), int(dst_width), int(dst_height), image.samples_per_pixel, output_filename.c_str());
      if (ret != TINYEXR_SUCCESS) {
        std::cout << "Save EXR failure: err code = " << ret << std::endl;
        return EXIT_FAILURE;
      }
#endif
    { // assume tiff
      bool ret = SaveTIFFImage(output_filename, buf.data(), int(dst_width), int(dst_height), image.samples_per_pixel);
      if (!ret) {
        return EXIT_FAILURE;
      }
    }

  }


  return EXIT_SUCCESS;
  
}
