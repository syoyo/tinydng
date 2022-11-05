#include <emscripten/bind.h>

#include <string>
#include <vector>

#define TINY_DNG_LOADER_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define TINY_DNG_NO_EXCEPTION
#include "tiny_dng_loader.h"

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

    warn_.clear();
    error_.clear();

    result_ =
        tinydng::LoadDNGFromMemory(binary.data(), binary.size(), custom_fields_,
                                   &images_, &warn_, &error_);
  }
  ~DNGLoader() {}

  bool ok() const { return result_; }

  const std::string warning() const { return warn_; }
  const std::string error() const { return error_; }

  int num_images() const { return images_.size(); }

  // Return largest image info
  int width() const {
    if (largest_idx_ < 0) {
      find_largest();
    }
    return images_[largest_idx_].width;
  }

  int height() const {
    if (largest_idx_ < 0) {
      find_largest();
    }
    return images_[largest_idx_].height;
  }
  int channels() const {
    if (largest_idx_ < 0) {
      find_largest();
    }

    return images_[largest_idx_].samples_per_pixel;
  }
  int bits() const {
    if (largest_idx_ < 0) {
      find_largest();
    }
    return images_[largest_idx_].bits_per_sample;
  }

  int largest_idx() const {
    find_largest();
    return largest_idx_;
  }

  // Return as memory views
  emscripten::val getImageBytes() const {
    return emscripten::val(emscripten::typed_memory_view(
        images_[largest_idx_].data.size(), images_[largest_idx_].data.data()));
  }

 private:
  bool result_{false};
  std::string warn_;
  std::string error_;

  std::vector<tinydng::DNGImage> images_;
  std::vector<tinydng::FieldInfo> custom_fields_;

  void find_largest() const {
    if (images_.empty()) {
      return;
    }

    // Find largest image(based on width pixels and bit depth).
    largest_idx_ = 0;
    int largest_width = images_[0].width;
    int largest_bits = images_[0].bits_per_sample;

    for (uint32_t i = 1; i < images_.size(); i++) {
      if (largest_width < images_[i].width) {
        largest_idx_ = i;
        largest_width = images_[i].width;
      } else if (largest_width == images_[i].width) {
        if (largest_bits < images_[i].bits_per_sample) {
          largest_idx_ = i;
          largest_bits = images_[i].bits_per_sample;
        }
      }
    }
  }

  mutable int largest_idx_ = -1;
};

// EMSCRIPTEN_BINDINGS(stl_wrappters) {
// emscripten::register_vector<float>("VectorFloat"); }

EMSCRIPTEN_BINDINGS(tinydng_module) {
  emscripten::class_<DNGLoader>("DNGLoader")
      .constructor<const std::string &>()
      .function("getImageBytes", &DNGLoader::getImageBytes)
      .function("ok", &DNGLoader::ok)
      .function("error", &DNGLoader::error)
      .function("largest_idx", &DNGLoader::largest_idx)
      .function("width", &DNGLoader::width)
      .function("height", &DNGLoader::height)
      .function("channels", &DNGLoader::channels)
      .function("bits", &DNGLoader::bits);
}
