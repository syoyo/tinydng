#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/numpy.h>
#include <cstring>

// define some helper functions for pybind11
#define TINY_DNG_LOADER_IMPLEMENTATION
#include "tiny_dng_loader.h"

namespace py = pybind11;

using namespace tinydng;

PYBIND11_MODULE(tinydng, tdng_module)
{
  tdng_module.doc() = "Python bindings for TinyDNG.";

  // TODO

}

