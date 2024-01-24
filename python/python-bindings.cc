#define TINY_DNG_LOADER_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#include "tiny_dng_loader.h"

// TODO: writer
//#define TINY_DNG_WRITER_IMPLEMENTATION
//#include "tiny_dng_writer.h"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/numpy.h>
#include <cstring>
#include <cstdint>

namespace py = pybind11;

using namespace tinydng;

namespace {

// helper function to convert 10, 12 and 14 bit to uint16
// TODO: optimize code

// TODO: 10bit
#if 0
static void decode12_to_16(
  std::vector<uint16_t> &dst,
  uint8_t *src,
  size_t width,
  size_t height
  bool do_swap = false) {

  size_t offsets[4][3] = {
  size_t bit_shifts[4] = {

  // 40 = 10bit * 4 pixel, 8bit * 5 pixel
  //
  size_t n = size_t(y * wi)

  size_t n4 = n % 4;
  size_t addr5 = (n / 4) * 5; // 8bit pixel pos
  size_t odd = (addt5 % 2);

  size_t offset[3];
  offset[0] = offsets[n4][0];
  offset[1] = offsets[n4][1];
  offset[2] = offsets[n4][2];

  if (do_swap) {
    // load with short byte swap
    if (odd) {
      buf[0] = data[addr5 - 1];
      buf[1] = data[addr5 + 2];
      buf[2] = data[addr5 + 1];
      buf[3] = data[addr5 + 4];
      buf[4] = data[addr5 + 3];
      buf[5] = data[addr5 + 6];
      buf[6] = data[addr5 + 5];
    } else {
      buf[0] = data[addr5 + 1];
      buf[1] = data[addr5 + 0];
      buf[2] = data[addr5 + 3];
      buf[3] = data[addr5 + 2];
      buf[4] = data[addr5 + 5];
      buf[5] = data[addr5 + 4];
      buf[6] = data[addr5 + 7];
    }
  } else {
    memcpy(buf, &data[addr5], 5);
  }

 }

#endif

void decode12_to_u16(std::vector<uint16_t>& image, uint8_t* data, size_t width,
                  size_t height, bool do_swap) {
  size_t offsets[2][2] = {{0, 1}, {1, 2}};

  size_t bit_shifts[2] = {4, 0};

  image.resize(width * height);

  for (size_t y = 0; y < height; y++) {
    for (size_t x = 0; x < width; x++) {
      uint8_t buf[3];

      // Calculate load addres for 12bit pixel(three 8 bit pixels)
      size_t n = y * width + x;

      // 24 = 12bit * 2 pixel, 8bit * 3 pixel
      size_t n2 = n % 2;           // used for offset & bitshifts
      size_t addr3 = (n / 2) * 3;  // 8bit pixel pos
      size_t odd = (addr3 % 2);

      size_t bit_shift;
      bit_shift = bit_shifts[n2];

      size_t offset[2];
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
      unsigned int b0 = (unsigned int)buf[offset[0]] & 0xff;
      unsigned int b1 = (unsigned int)buf[offset[1]] & 0xff;

      unsigned int val = (b0 << 8) | b1;
      val = 0xfff & (val >> bit_shift);

      image[y * width + x] = uint16_t(val);
    }
  }
}

//
// Decode 14bit integer image into floating point HDR image
//
void decode14_to_u16(std::vector<uint16_t>& image, uint8_t* data, size_t width,
                  size_t height, bool do_swap) {
  size_t offsets[4][3] = {{0, 0, 1}, {1, 2, 3}, {3, 4, 5}, {5, 5, 6}};

  size_t bit_shifts[4] = {2, 4, 6, 0};

  image.resize(width * height);

  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      uint8_t buf[7];

      // Calculate load addres for 14bit pixel(three 8 bit pixels)
      size_t n = size_t(y * width + x);

      // 56 = 14bit * 4 pixel, 8bit * 7 pixel
      size_t n4 = n % 4;           // used for offset & bitshifts
      size_t addr7 = (n / 4) * 7;  // 8bit pixel pos
      size_t odd = (addr7 % 2);

      size_t offset[3];
      offset[0] = offsets[n4][0];
      offset[1] = offsets[n4][1];
      offset[2] = offsets[n4][2];

      size_t bit_shift;
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
      unsigned int b0 = (unsigned int)buf[offset[0]] & 0xff;
      unsigned int b1 = (unsigned int)buf[offset[1]] & 0xff;
      unsigned int b2 = (unsigned int)buf[offset[2]] & 0xff;

      // unsigned int val = (b0 << 16) | (b1 << 8) | b2;
      // unsigned int val = (b2 << 16) | (b0 << 8) | b0;
      unsigned int val = (b0 << 16) | (b1 << 8) | b2;
      // unsigned int val = b2;
      val = 0x3fff & (val >> bit_shift);

      image[y * width + x] = uint16_t(val);
    }
  }
}

std::vector<tinydng::DNGImage> load_dng(const std::string &filename)
{
  std::string warn, err;
  std::vector<tinydng::DNGImage> images;
  std::vector<tinydng::FieldInfo> custom_fields; // not used.

  bool ret = tinydng::LoadDNG(filename.c_str(), custom_fields, &images, &warn, &err);

  if (warn.size()) {
    py::print("TinyDNG LoadDNG Warninng: " + warn);
  }

  if (!ret) {
    throw "Failed to load DNG: " + err;
  }

  // decode pixel to uint16 for easy RAW data manipulation.
  for (auto &image : images) {
    if (image.bits_per_sample == 8) {
      // ok
    } else if (image.bits_per_sample == 12) {
      std::vector<uint16_t> img16;
      decode12_to_u16(img16, image.data.data(), image.width, image.height, /* byteswap */false);
      image.data.resize(img16.size() * sizeof(uint16_t));
      memcpy(image.data.data(), img16.data(), image.data.size());
      image.bits_per_sample = 16; // overwrite bps
    } else if (image.bits_per_sample == 14) {
      std::vector<uint16_t> img16;
      decode14_to_u16(img16, image.data.data(), image.width, image.height, /* byteswap */false);
      image.data.resize(img16.size() * sizeof(uint16_t));
      memcpy(image.data.data(), img16.data(), image.data.size());
      image.bits_per_sample = 16; // overwrite bps
    } else if (image.bits_per_sample == 32) {
      // ok. int or fp32 image
    } else if (image.bits_per_sample == 64) {
      // ok. int64 or fp64 image
    } else {
      py::print("TinyDNG: Unsupported bit depth: " + std::to_string(image.bits_per_sample));
      throw "Unsupported bit depth: " + std::to_string(image.bits_per_sample);
    }
  }

  return images;
}

}

PYBIND11_MODULE(tinydng, tdng_module)
{
  tdng_module.doc() = "Python bindings for TinyDNG.";

  // register struct
  py::class_<DNGImage>(tdng_module, "DNGImage")
    .def(py::init<>())
    .def_readwrite("width", &DNGImage::width)
    .def_readwrite("height", &DNGImage::height)
    .def_readwrite("bits_per_sample", &DNGImage::bits_per_sample)
    ;

  tdng_module.def("loaddng", &load_dng);

  // TODO
  // register struct
  // py::class_<tinydngwriter::DNGImage>(tdng_module, "WDNGImage")
  //tdng_module.def("savedng", &save_dng);


}
