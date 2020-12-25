#include <stdint.h>
#include <stddef.h>
#define STB_IMAGE_IMPLEMENTATION
#define TINY_DNG_LOADER_IMPLEMENTATION
#include "tiny_dng_loader.h"
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  std::vector<tinydng::FieldInfo> custom_fields;
  std::vector<tinydng::DNGImage> images;
  std::string warn;
  std::string err;

  bool ret = tinydng::LoadDNGFromMemory(reinterpret_cast<const char *>(data), size, custom_fields, &images, &warn, &err);

  if (!ret) return 0;

  return 0;
}

