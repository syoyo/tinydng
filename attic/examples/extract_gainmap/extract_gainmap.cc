#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <fstream>

#define TINY_DNG_LOADER_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define TINY_DNG_NO_EXCEPTION
#include "tiny_dng_loader.h"

static bool IsBigEndian() {
  uint32_t i = 0x01020304;
  char c[4];
  memcpy(c, &i, 4);
  return (c[0] == 1);
}

///
/// Simple PFM(grayscale) image saver.
///
static bool SaveAsPFM(
  const std::string &filename, const std::vector<float> &image, size_t width, size_t height, size_t channels)
{
  std::string tag;
  if (channels == 1) {
    tag = "Pf";
  } else if (channels == 3) {
    tag = "PF";
  } else {
    // Unsupported.
    return false;
  }

  std::ofstream ofs(filename.c_str(), std::ios::binary);
  if (!ofs) {
    return false;
  }

  ofs << tag << "\n";
  ofs << width << " " << height << "\n";
  if (IsBigEndian()) {
    ofs << "1.0\n";
  } else {
    ofs << "-1.0\n";
  }

  size_t num = width * height * sizeof(float);

  ofs.write(reinterpret_cast<const char *>(image.data()), ssize_t(num));

  return true;
}

static void DumpGainMap(int layer_idx, int tag_idx, const std::vector<tinydng::GainMap> &gmaps)
{
  printf("OpCodeList%d : The number of GainMaps = %d\n", tag_idx, int(gmaps.size()));

  for (size_t i = 0; i < gmaps.size(); i++) {
    const tinydng::GainMap &gmap = gmaps[i];

    printf("  GainMap[%d]\n", int(i));
    printf("    top %d, left %d, bottom %d, right %d\n", gmap.top, gmap.left, gmap.bottom, gmap.right);
    printf("    plane %d, planes %d\n", gmap.plane, gmap.planes);
    printf("    row_pitch %d, col_pitch %d\n", gmap.row_pitch, gmap.col_pitch);
    printf("    map_points_v %d, map_points_h %d\n", gmap.map_points_v, gmap.map_points_h);
    printf("    map_spacing_v %f, map_spacing_h %f\n", gmap.map_spacing_v, gmap.map_spacing_h);
    printf("    map_origin_v %f, map_origin_h %f\n", gmap.map_origin_v, gmap.map_origin_h);
    printf("    map_planes %d\n", gmap.map_planes);

    std::stringstream ss;

    ss << "layer" << layer_idx << "-opcodelist" << tag_idx << "-gainmap" << i << ".pfm";

    const std::string filename = ss.str();

    if (gmap.map_planes != 1) {
      std::cerr << "Grayscale(map_planes = 1) GainMap expected but got map_planes " << gmap.map_planes << "\n";
    } else {

      if (!SaveAsPFM(filename, gmap.pixels, gmap.map_points_h, gmap.map_points_v, /* channels */1)) {
        std::cerr << "Failed to save: " << filename << "\n";

      } else {
        std::cout << "Saved GainMap image as: " << filename << "\n";
      }
    }
  }
}

int main(int argc, char **argv) {
  std::string input_filename;

  if (argc < 2) {
    std::cerr << "Needs input.dng\n";
    return -1;
  }

  input_filename = std::string(argv[1]);

  std::string warn, err;
  std::vector<tinydng::DNGImage> images;
  std::vector<tinydng::FieldInfo> custom_fields;

  bool ret = tinydng::LoadDNG(input_filename.c_str(), custom_fields, &images, &warn, &err);

  if (!warn.empty()) {
    std::cout << "WARN: " << warn << std::endl;
  }

  if (!err.empty()) {
    std::cout << "ERR: " << err;
  }

  if (!ret) {
    std::cerr << "Failed to load DNG\n";
    return -1;
  }

  for (size_t i = 0; i < images.size(); i++) {
    const tinydng::DNGImage &image = images[i];
    std::cout << "width = " << image.width << std::endl;
    std::cout << "height = " << image.height << std::endl;
    std::cout << "bits per pixel = " << image.bits_per_sample << std::endl;
    std::cout << "bits per pixel(original) = " << image.bits_per_sample_original << std::endl;
    std::cout << "samples per piexl = " << image.samples_per_pixel << std::endl;
    std::cout << "sample format = " << image.sample_format << std::endl;

    std::cout << "version = " << image.version << std::endl;

    if (image.opcodelist1_gainmap.size()) {
      DumpGainMap(int(i), 1, image.opcodelist1_gainmap);
    }
    if (image.opcodelist2_gainmap.size()) {
      DumpGainMap(int(i), 2, image.opcodelist2_gainmap);
    }
    if (image.opcodelist3_gainmap.size()) {
      DumpGainMap(int(i), 3, image.opcodelist3_gainmap);
    }
  }

  return EXIT_SUCCESS;
}
