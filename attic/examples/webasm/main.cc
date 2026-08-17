#include <emscripten/bind.h>

#define TINY_DNG_LOADER_IMPLEMENTATION
#include "tiny_dng_loader.h"

using namespace emscripten;

///
/// Simple C++ wrapper class for Emscripten
///
class DNGLoader {
 public:
  ///
  /// `binary` is the buffer for DNG binary(e.g. buffer read by fs.readFileSync)
  /// std::string can be used as UInt8Array in JS layer.
  ///
  DNGLoader(const std::string &binary) {
    const float *ptr = reinterpret_cast<const float *>(binary.data());

    error_.clear();
    tinydng::DNGLoaderOption option;

    load_ok_ = tinydng::LoadDNGFromMemory(
        reinterpret_cast<const char *>(binary.data()), static_cast<unsigned int>(binary.size()),
        option,
        &images_,
        &warn_,
        &error_);

  }
  ~DNGLoader() {}

  //
  // Use the first image.
  //
  // TODO: Support multiple images
  //

  // Return as memory views
  emscripten::val getBytes() const {
    return emscripten::val(
        emscripten::typed_memory_view(images_[0].data.size(), images_[0].data.data()));
  }

  bool ok() const { return load_ok_; }

  const std::string error() const { return error_; }

  int width() const { return images_[0].width; }

  int height() const { return images_[0].height; }

 private:
  bool load_ok_;
  std::vector<tinydng::DNGImage> images_;
  std::string warn_;
  std::string error_;
};

// Register STL
EMSCRIPTEN_BINDINGS(stl_wrappters) { register_vector<float>("VectorFloat"); }

EMSCRIPTEN_BINDINGS(tinyexr_module) {
  class_<DNGLoader>("DNGLoader")
      .constructor<const std::string &>()
      .function("getBytes", &DNGLoader::getBytes)
      .function("ok", &DNGLoader::ok)
      .function("error", &DNGLoader::error)
      .function("width", &DNGLoader::width)
      .function("height", &DNGLoader::height);
}
