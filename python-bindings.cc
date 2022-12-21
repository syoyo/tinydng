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

namespace py = pybind11;

using namespace tinydng;

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

  return images;
}


PYBIND11_MODULE(tinydng, tdng_module)
{
  tdng_module.doc() = "Python bindings for TinyObjLoader.";

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
