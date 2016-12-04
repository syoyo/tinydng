#include <cstdio>
#include <cstdlib>
#include <iostream>

#define TINY_DNG_LOADER_IMPLEMENTATION
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

  int width, height, bits, components;
  std::string err;
  tinydng::DNGInfo dng_info;
  std::vector<unsigned char> data;
  size_t data_len;

  bool ret = tinydng::LoadDNG(&dng_info, &data, &data_len, &width, &height,
                              &bits, &components, &err, input_filename.c_str());

  if (ret) {
    std::cout << "width = " << width << std::endl;
    std::cout << "height = " << height << std::endl;
    std::cout << "bits per piexl = " << bits << std::endl;
    std::cout << "bits per piexl(original) = " << dng_info.bits_internal << std::endl;
    std::cout << "samples per piexl = " << dng_info.samples_per_pixel << std::endl;
    std::cout << "# of components = " << components << std::endl;

    std::cout << "version = " << dng_info.version << std::endl;

    for (int s = 0; s < dng_info.samples_per_pixel; s++) {
      std::cout << "white_level[" << s << "] = " << dng_info.white_level[s] << std::endl;
      std::cout << "black_level[" << s << "] = " << dng_info.black_level[s] << std::endl;
    }

    std::cout << "tile_width = " << dng_info.tile_width << std::endl;
    std::cout << "tile_length = " << dng_info.tile_length << std::endl;
    std::cout << "tile_offset = " << dng_info.tile_offset << std::endl;

    std::cout << "tile_offset = " << dng_info.tile_offset << std::endl;

    std::cout << "cfa_layout = " << dng_info.cfa_layout << std::endl;
    std::cout << "cfa_plane_color = "
              << get_colorname(dng_info.cfa_plane_color[0])
              << get_colorname(dng_info.cfa_plane_color[1])
              << get_colorname(dng_info.cfa_plane_color[2])
              << get_colorname(dng_info.cfa_plane_color[3]) << std::endl;
    std::cout << "cfa_pattern[2][2] = " << std::endl
              << dng_info.cfa_pattern[0][0] << ", "
              << dng_info.cfa_pattern[0][1] << std::endl
              << dng_info.cfa_pattern[1][0] << ", "
              << dng_info.cfa_pattern[1][1] << std::endl;

    std::cout << "active_area = " << dng_info.active_area[0] << ", "
              << dng_info.active_area[1] << ", " << dng_info.active_area[2]
              << ", " << dng_info.active_area[3] << std::endl;

    std::cout << "calibration_illuminant1 = "
              << dng_info.calibration_illuminant1 << std::endl;
    std::cout << "calibration_illuminant2 = "
              << dng_info.calibration_illuminant2 << std::endl;

    std::cout << "color_matrix1 = " << std::endl;
    for (size_t i = 0; i < 3; i++) {
      std::cout << dng_info.color_matrix1[i][0] << " , "
                << dng_info.color_matrix1[i][1] << " , "
                << dng_info.color_matrix1[i][2] << std::endl;
    }

    std::cout << "color_matrix2 = " << std::endl;
    for (size_t i = 0; i < 3; i++) {
      std::cout << dng_info.color_matrix2[i][0] << " , "
                << dng_info.color_matrix2[i][1] << " , "
                << dng_info.color_matrix2[i][2] << std::endl;
    }

    std::cout << "forward_matrix1 = " << std::endl;
    for (size_t i = 0; i < 3; i++) {
      std::cout << dng_info.forward_matrix1[i][0] << " , "
                << dng_info.forward_matrix1[i][1] << " , "
                << dng_info.forward_matrix1[i][2] << std::endl;
    }

    std::cout << "forward_matrix2 = " << std::endl;
    for (size_t i = 0; i < 3; i++) {
      std::cout << dng_info.forward_matrix2[i][0] << " , "
                << dng_info.forward_matrix2[i][1] << " , "
                << dng_info.forward_matrix2[i][2] << std::endl;
    }

    std::cout << "camera_calibration1 = " << std::endl;
    for (size_t i = 0; i < 3; i++) {
      std::cout << dng_info.camera_calibration1[i][0] << " , "
                << dng_info.camera_calibration1[i][1] << " , "
                << dng_info.camera_calibration1[i][2] << std::endl;
    }

    std::cout << "camera_calibration2 = " << std::endl;
    for (size_t i = 0; i < 3; i++) {
      std::cout << dng_info.camera_calibration2[i][0] << " , "
                << dng_info.camera_calibration2[i][1] << " , "
                << dng_info.camera_calibration2[i][2] << std::endl;
    }

    if (dng_info.has_analog_balance) {
      std::cout << "analog_balance = "
                << dng_info.analog_balance[0] << " , "
                << dng_info.analog_balance[1] << " , "
                << dng_info.analog_balance[2] << std::endl;
    }

    if (dng_info.has_as_shot_neutral) {
      std::cout << "as_shot_neutral = "
                << dng_info.as_shot_neutral[0] << " , "
                << dng_info.as_shot_neutral[1] << " , "
                << dng_info.as_shot_neutral[2] << std::endl;
    }

  } else {
    std::cout << "Fail to load DNG " << input_filename << std::endl;
    if (!err.empty()) {
      std::cout << "ERR: " << err;
    }
  }

  return EXIT_SUCCESS;
}
