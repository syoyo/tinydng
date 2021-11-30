#include <cstdio>
#include <cstdlib>
#include <iostream>

#define TINY_DNG_WRITER_IMPLEMENTATION
#include "tiny_dng_writer.h"

#define TINY_DNG_LOADER_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#include "tiny_dng_loader.h"

#define MY_TAG (21264)

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

static void CreateImage(tinydngwriter::DNGImage *dng_image, const unsigned short basecol, const int intvalue)
{
    unsigned int image_width = 512;
    unsigned int image_height = 512;
    dng_image->SetSubfileType(tinydngwriter::FILETYPE_REDUCEDIMAGE);
    dng_image->SetImageWidth(image_width);
    dng_image->SetImageLength(image_height);
    dng_image->SetRowsPerStrip(image_height);

    dng_image->SetSamplesPerPixel(3);
    unsigned short bps[3] = {16, 16, 16};
    dng_image->SetBitsPerSample(3, &bps[0]);

    dng_image->SetPlanarConfig(tinydngwriter::PLANARCONFIG_CONTIG);
    dng_image->SetCompression(tinydngwriter::COMPRESSION_NONE);
    dng_image->SetPhotometric(tinydngwriter::PHOTOMETRIC_RGB);

    dng_image->SetCustomFieldLong(MY_TAG, intvalue);

    std::vector<unsigned short> buf;
    buf.resize(image_width * image_height * 3);

    for (size_t y = 0; y < image_height; y++) {
      for (size_t x = 0; x < image_width; x++) {
        buf[3 * (y * image_width + x) + 0] = static_cast<unsigned short>(x % 512);
        buf[3 * (y * image_width + x) + 1] = static_cast<unsigned short>(y % 512);
        buf[3 * (y * image_width + x) + 2] = basecol;
      }
    }

    dng_image->SetImageData(reinterpret_cast<unsigned char *>(buf.data()), buf.size() * sizeof(unsigned short));
}

static bool WriteDNG(const std::string &filename, const int custom_value)
{
  bool big_endian = true;

  tinydngwriter::DNGImage dng_image0;
  dng_image0.SetBigEndian(big_endian);
  tinydngwriter::DNGImage dng_image1;
  dng_image1.SetBigEndian(big_endian);

  CreateImage(&dng_image0, 12000, custom_value);
  CreateImage(&dng_image1, 42000, 456);

  tinydngwriter::DNGWriter dng_writer(big_endian);
  assert(dng_writer.AddImage(&dng_image0));
  assert(dng_writer.AddImage(&dng_image1));

  std::string err;
  bool ret = dng_writer.WriteToFile(filename.c_str(), &err);

  if (!err.empty()) {
    std::cerr << err;
  }

  if (!ret) {
    return false;
  }

  std::cout << "Wrote : " << filename << std::endl;

  return true;
}

int main(int argc, char **argv) {

  std::string filename = "test.dng";
  int custom_value = 123;

  if (argc > 1) {
    custom_value = atoi(argv[1]);
  }

  if (!WriteDNG(filename, custom_value)) {
    std::cerr << "Failed to write DNG\n";
    return EXIT_FAILURE;
  }

  std::string warn, err;
  std::vector<tinydng::DNGImage> images;
  std::vector<tinydng::FieldInfo> custom_fields;

  // Setup custom field lists for reading.
  {
    tinydng::FieldInfo field;
    field.tag = MY_TAG;
    field.type = tinydng::TYPE_SLONG;
    field.name = "my_tag";

    custom_fields.push_back(field);
  }

  bool ret = tinydng::LoadDNG(filename.c_str(), custom_fields, &images, &warn, &err);

  if (!warn.empty()) {
    std::cout << "warn: " << warn << std::endl;
  }

  if (ret) {
    for (size_t i = 0; i < images.size(); i++) {
      const tinydng::DNGImage &image = images[i];

      std::cout << "width = " << image.width << std::endl;
      std::cout << "height = " << image.height << std::endl;
      std::cout << "bits per piexl = " << image.bits_per_sample << std::endl;
      std::cout << "bits per piexl(original) = " << image.bits_per_sample_original << std::endl;
      std::cout << "samples per piexl = " << image.samples_per_pixel << std::endl;

      std::cout << "version = " << image.version << std::endl;

      for (int s = 0; s < image.samples_per_pixel; s++) {
        std::cout << "white_level[" << s << "] = " << image.white_level[s] << std::endl;
        std::cout << "black_level[" << s << "] = " << image.black_level[s] << std::endl;
      }

      std::cout << "tile_width = " << image.tile_width << std::endl;
      std::cout << "tile_length = " << image.tile_length << std::endl;
      std::cout << "tile_offset = " << image.tile_offset << std::endl;

      std::cout << "tile_offset = " << image.tile_offset << std::endl;

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

      // Custom fields
      std::cout << "# of custo fields : " << image.custom_fields.size() << std::endl;
      for (size_t c = 0; c < image.custom_fields.size(); c++) {
        std::cout << "Custom field: name = " << image.custom_fields[c].name << std::endl;
        std::cout << "Custom field: type = " << image.custom_fields[c].type << std::endl;

        if (image.custom_fields[c].type == tinydng::TYPE_LONG) {
          const unsigned int value = *(reinterpret_cast<const unsigned int *>(image.custom_fields[c].data.data()));
          std::cout << "  uint value : " << value << std::endl;
        } else if (image.custom_fields[c].type == tinydng::TYPE_SLONG) {
          const int value = *(reinterpret_cast<const int *>(image.custom_fields[c].data.data()));
          std::cout << "  int value : " << value << std::endl;
        } else {
          // TODO(syoyo): Implement more data types.
        }
      }
    }

  } else {
    std::cout << "Fail to load DNG " << filename << std::endl;
    if (!err.empty()) {
      std::cout << "ERR: " << err;
    }
  }

  return EXIT_SUCCESS;
}
