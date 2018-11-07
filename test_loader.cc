#include <cstdio>
#include <cstdlib>
#include <iostream>

#define TINY_DNG_LOADER_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define TINY_DNG_NO_EXCEPTION
#include "tiny_dng_loader.h"

static char get_colorname(int c) {
  switch (c) {
    case 0:
      return 'R';
    case 1:
      return 'G';
    case 2:
      return 'B';
    case 3:
      return 'C';
    case 4:
      return 'M';
    case 5:
      return 'Y';
    case 6:
      return 'W';
    default:
      return '?';
  }
}

int main(int argc, char **argv) {
  std::string input_filename = "colorchart.dng";

  if (argc > 1) {
    input_filename = std::string(argv[1]);
  }

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

  if (ret) {
    for (size_t i = 0; i < images.size(); i++) {
      const tinydng::DNGImage &image = images[i];
;
      std::cout << "width = " << image.width << std::endl;
      std::cout << "height = " << image.height << std::endl;
      std::cout << "bits per pixel = " << image.bits_per_sample << std::endl;
      std::cout << "bits per pixel(original) = " << image.bits_per_sample_original << std::endl;
      std::cout << "samples per piexl = " << image.samples_per_pixel << std::endl;
      std::cout << "sample format = " << image.sample_format << std::endl;

      std::cout << "version = " << image.version << std::endl;

      for (int s = 0; s < image.samples_per_pixel; s++) {
        std::cout << "white_level[" << s << "] = " << image.white_level[s] << std::endl;
        std::cout << "black_level[" << s << "] = " << image.black_level[s] << std::endl;
      }

      std::cout << "tile_width = " << image.tile_width << std::endl;
      std::cout << "tile_length = " << image.tile_length << std::endl;

      std::cout << "cfa_layout = " << image.cfa_layout << std::endl;
      std::cout << "cfa_plane_color = "
                << get_colorname(image.cfa_plane_color[0])
                << get_colorname(image.cfa_plane_color[1])
                << get_colorname(image.cfa_plane_color[2])
                << get_colorname(image.cfa_plane_color[3]) << std::endl;
      std::cout << "cfa_pattern[2][2] = " << std::endl
                << image.cfa_pattern[0][0] << ", "
                << image.cfa_pattern[0][1] << std::endl
                << image.cfa_pattern[1][0] << ", "
                << image.cfa_pattern[1][1] << std::endl;

      std::cout << "active_area = " << image.active_area[0] << ", "
                << image.active_area[1] << ", " << image.active_area[2]
                << ", " << image.active_area[3] << std::endl;

      std::cout << "calibration_illuminant1 = "
                << image.calibration_illuminant1 << std::endl;
      std::cout << "calibration_illuminant2 = "
                << image.calibration_illuminant2 << std::endl;

      std::cout << "color_matrix1 = " << std::endl;
      for (size_t k = 0; k < 3; k++) {
        std::cout << image.color_matrix1[k][0] << " , "
                  << image.color_matrix1[k][1] << " , "
                  << image.color_matrix1[k][2] << std::endl;
      }

      std::cout << "color_matrix2 = " << std::endl;
      for (size_t k = 0; k < 3; k++) {
        std::cout << image.color_matrix2[k][0] << " , "
                  << image.color_matrix2[k][1] << " , "
                  << image.color_matrix2[k][2] << std::endl;
      }

      std::cout << "forward_matrix1 = " << std::endl;
      for (size_t k = 0; k < 3; k++) {
        std::cout << image.forward_matrix1[k][0] << " , "
                  << image.forward_matrix1[k][1] << " , "
                  << image.forward_matrix1[k][2] << std::endl;
      }

      std::cout << "forward_matrix2 = " << std::endl;
      for (size_t k = 0; k < 3; k++) {
        std::cout << image.forward_matrix2[k][0] << " , "
                  << image.forward_matrix2[k][1] << " , "
                  << image.forward_matrix2[k][2] << std::endl;
      }

      std::cout << "camera_calibration1 = " << std::endl;
      for (size_t k = 0; k < 3; k++) {
        std::cout << image.camera_calibration1[k][0] << " , "
                  << image.camera_calibration1[k][1] << " , "
                  << image.camera_calibration1[k][2] << std::endl;
      }

      std::cout << "camera_calibration2 = " << std::endl;
      for (size_t k = 0; k < 3; k++) {
        std::cout << image.camera_calibration2[k][0] << " , "
                  << image.camera_calibration2[k][1] << " , "
                  << image.camera_calibration2[k][2] << std::endl;
      }

      if (image.has_analog_balance) {
        std::cout << "analog_balance = "
                  << image.analog_balance[0] << " , "
                  << image.analog_balance[1] << " , "
                  << image.analog_balance[2] << std::endl;
      }

      if (image.has_as_shot_neutral) {
        std::cout << "as_shot_neutral = "
                  << image.as_shot_neutral[0] << " , "
                  << image.as_shot_neutral[1] << " , "
                  << image.as_shot_neutral[2] << std::endl;
      }
    }

  } else {
    std::cout << "Fail to load DNG " << input_filename << std::endl;
  }

  return EXIT_SUCCESS;
}
