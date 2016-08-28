//
// TinyDNGLoadr, single header only DNG loader.
//

/*
The MIT License (MIT)

Copyright (c) 2016 Syoyo Fujita.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/
#ifndef TINY_DNG_LOADER_H_
#define TINY_DNG_LOADER_H_

// @note {
// https://www.adobe.com/content/dam/Adobe/en/products/photoshop/pdfs/dng_spec_1.4.0.0.pdf
// }

#include <string>
#include <vector>

namespace tinydng {

struct DNGInfo {
  int black_level;
  int white_level;
  int version;

  char cfa_plane_color[4];  // 0:red, 1:green, 2:blue, 3:cyan, 4:magenta,
                            // 5:yellow, 6:white
  int cfa_pattern[2][2];    // @fixme { Support non 2x2 CFA pattern. }
  int cfa_layout;
  int active_area[4];  // top, left, bottom, right

  int tile_width;
  int tile_length;
  unsigned int tile_offset;

  double color_matrix[3][3];
};

// Load DNG image.
// Returns true upon success.
// Returns false upon failure and store error message into `err`.
bool LoadDNG(DNGInfo* info,                     // [out] DNG meta information.
             std::vector<unsigned char>* data,  // [out] DNG image data
             size_t* data_len,                  // [out] DNG image data size
             int* width,                        // [out] DNG image width
             int* height,                       // [out] DNG image height
             int* bits,                         // [out] DNG pixel bits
             int* num_components,               // [out] DNG # of components
             std::string* err,                  // [out] error message.
             const char* filename,
             bool is_system_big_endian =
                 false);  // Set true if you are running on Big Endian machine.

}  // namespace tinydng

#ifdef TINY_DNG_LOADER_IMPLEMENTATION

#include <cassert>
#include <sstream>

//
// @todo { Support big endian }
//

namespace tinydng {

typedef struct {
  int width;
  int height;
  int bps;
  int compression;
  unsigned int offset;
  int orientation;
  int samples;
  int bytes;
  int active_area[4];  // top, left, bottom, right
  bool has_active_area;
  unsigned char pad[3];

  char cfa_plane_color[4];  // 0:red, 1:green, 2:blue, 3:cyan, 4:magenta,
                            // 5:yellow, 6:white
  int cfa_pattern[2][2];    // @fixme { Support non 2x2 CFA pattern. }
  int cfa_layout;

  int tile_width;
  int tile_length;
  unsigned int tile_offset;
} TIFFInfo;

static void swap2(unsigned short* val) {
  unsigned short tmp = *val;
  unsigned char* dst = reinterpret_cast<unsigned char*>(val);
  unsigned char* src = reinterpret_cast<unsigned char*>(&tmp);

  dst[0] = src[1];
  dst[1] = src[0];
}

static void swap4(unsigned int* val) {
  unsigned int tmp = *val;
  unsigned char* dst = reinterpret_cast<unsigned char*>(val);
  unsigned char* src = reinterpret_cast<unsigned char*>(&tmp);

  dst[0] = src[3];
  dst[1] = src[2];
  dst[2] = src[1];
  dst[3] = src[0];
}

static unsigned short Read2(FILE* fp, bool swap) {
  unsigned short val;
  size_t ret = fread(&val, 1, 2, fp);
  assert(ret == 2);
  if (swap) {
    swap2(&val);
  }
  return val;
}

static unsigned int Read4(FILE* fp, bool swap) {
  unsigned int val;
  size_t ret = fread(&val, 1, 4, fp);
  assert(ret == 4);
  if (swap) {
    swap4(&val);
  }
  return val;
}

static unsigned int ReadUInt(int type, FILE* fp, bool swap) {
  // @todo {8, 9, 10, 11, 12}
  if (type == 3) {
    unsigned short val;
    size_t ret = fread(&val, 1, 2, fp);
    assert(ret == 2);
    if (swap) {
      swap2(&val);
    }
    return static_cast<unsigned int>(val);
  } else if (type == 5) {
    unsigned int val0;
    size_t ret = fread(&val0, 1, 4, fp);
    assert(ret == 4);
    if (swap) {
      swap4(&val0);
    }

    unsigned int val1;
    ret = fread(&val1, 1, 4, fp);
    assert(ret == 4);
    if (swap) {
      swap4(&val1);
    }

    return static_cast<unsigned int>(val0 / val1);

  } else {  // guess 4.
    unsigned int val;
    size_t ret = fread(&val, 1, 4, fp);
    assert(ret == 4);
    if (swap) {
      swap4(&val);
    }
    return static_cast<unsigned int>(val);
  }
}

static double ReadReal(int type, FILE* fp, bool swap) {
  assert(type == 10);  // @todo { Support more types. }
  int num = static_cast<int>(Read4(fp, swap));
  int denom = static_cast<int>(Read4(fp, swap));

  return static_cast<double>(num) / static_cast<double>(denom);
}

static void GetTIFFTag(unsigned short* tag, unsigned short* type,
                       unsigned int* len, unsigned int* saved_offt, FILE* fp) {
  size_t ret;
  ret = fread(tag, 1, 2, fp);
  assert(ret == 2);
  ret = fread(type, 1, 2, fp);
  assert(ret == 2);
  ret = fread(len, 1, 4, fp);
  assert(ret == 4);

  (*saved_offt) = static_cast<unsigned int>(ftell(fp)) + 4;

  size_t typesize_table[] = {1, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8, 8};

  if ((*len) * (typesize_table[(*type) < 14 ? (*type) : 0]) > 4) {
    unsigned int base = 0;  // fixme
    unsigned int offt;
    ret = fread(&offt, 1, 4, fp);
    assert(ret == 4);
    fseek(fp, offt + base, SEEK_SET);
  }
}

static bool ParseTIFFIFD(tinydng::DNGInfo* dng_info, TIFFInfo infos[16],
                         int* num_ifds,  // initial value must be 0
                         FILE* fp, bool swap_endian) {
  int idx = (*num_ifds)++;  // increament num_ifds.

  unsigned short num_entries;
  size_t ret = fread(&num_entries, 1, 2, fp);
  assert(ret == 2);
  if (num_entries > 512) {
    assert(0);
    return false;  // @fixme
  }

  infos[idx].has_active_area = false;
  infos[idx].cfa_plane_color[0] = 0;
  infos[idx].cfa_plane_color[1] = 1;
  infos[idx].cfa_plane_color[2] = 2;
  infos[idx].cfa_plane_color[3] = 0;  // optional?

  // The spec says default is None, thus fill with -1(=invalid).
  infos[idx].cfa_pattern[0][0] = -1;
  infos[idx].cfa_pattern[0][1] = -1;
  infos[idx].cfa_pattern[1][0] = -1;
  infos[idx].cfa_pattern[1][1] = -1;

  infos[idx].cfa_layout = 1;

  while (num_entries--) {
    unsigned short tag, type;
    unsigned int len;
    unsigned int saved_offt;
    GetTIFFTag(&tag, &type, &len, &saved_offt, fp);

    switch (tag) {
      case 2:
      case 256:
      case 61441:  // ImageWidth
        infos[idx].width = static_cast<int>(ReadUInt(type, fp, swap_endian));
        break;

      case 3:
      case 257:
      case 61442:  // ImageHeight
        infos[idx].height = static_cast<int>(ReadUInt(type, fp, swap_endian));
        break;

      case 258:
      case 61443:  // BitsPerSample
        infos[idx].samples = len & 7;
        infos[idx].bps = static_cast<int>(ReadUInt(type, fp, swap_endian));
        break;

      case 259:  // Compression
        infos[idx].compression =
            static_cast<int>(ReadUInt(type, fp, swap_endian));
        break;

      case 273:              // StripOffset
      case 513:              // JpegIFOffset
        assert(tag == 273);  // @todo { jpeg data }
        infos[idx].offset = Read4(fp, swap_endian);
        break;

      case 274:  // Orientation
        infos[idx].orientation = Read2(fp, swap_endian);
        break;

      case 330:  // SubIFDs

      {
        while (len--) {
          unsigned int i = static_cast<unsigned int>(ftell(fp));
          unsigned int offt = Read4(fp, swap_endian);
          unsigned int base = 0;  // @fixme
          fseek(fp, offt + base, SEEK_SET);

          ParseTIFFIFD(dng_info, infos, num_ifds, fp,
                       swap_endian);   // recursive call
          fseek(fp, i + 4, SEEK_SET);  // rewind
        }
      }

      break;

      case 50714:  // BlackLevel
        dng_info->black_level =
            static_cast<int>(ReadUInt(type, fp, swap_endian));
        break;

      case 50717:  // WhiteLevel
        dng_info->white_level =
            static_cast<int>(ReadUInt(type, fp, swap_endian));
        break;

      case 50721:  // ColorMatrix1
      case 50722:  // ColorMatrix2
      {
        for (int c = 0; c < 3; c++) {
          for (int k = 0; k < 3; k++) {
            double val = ReadReal(type, fp, swap_endian);
            dng_info->color_matrix[c][k] = val;
          }
        }
      }

      break;

      case 50829:  // ActiveArea
        infos[idx].has_active_area = true;
        infos[idx].active_area[0] =
            static_cast<int>(ReadUInt(type, fp, swap_endian));
        infos[idx].active_area[1] =
            static_cast<int>(ReadUInt(type, fp, swap_endian));
        infos[idx].active_area[2] =
            static_cast<int>(ReadUInt(type, fp, swap_endian));
        infos[idx].active_area[3] =
            static_cast<int>(ReadUInt(type, fp, swap_endian));
        break;

      case 322:  // TileWidth
        infos[idx].tile_width =
            static_cast<int>(ReadUInt(type, fp, swap_endian));
        break;

      case 323:  // TileLength
        infos[idx].tile_length =
            static_cast<int>(ReadUInt(type, fp, swap_endian));
        break;

      case 324:  // TileOffsets
        infos[idx].tile_offset = len > 1 ? static_cast<unsigned int>(ftell(fp))
                                         : Read4(fp, swap_endian);
        break;

      case 33422:  // CFAPattern
      {
        char buf[16];
        size_t readLen = len;
        if (readLen > 16) readLen = 16;
        // Assume 2x2 CFAPattern.
        assert(readLen == 4);
        fread(buf, 1, readLen, fp);
        infos[idx].cfa_pattern[0][0] = buf[0];
        infos[idx].cfa_pattern[0][1] = buf[1];
        infos[idx].cfa_pattern[1][0] = buf[2];
        infos[idx].cfa_pattern[1][1] = buf[3];
      } break;

      case 50706:  // DNGVersion
      {
        char data[4];
        data[0] = static_cast<char>(fgetc(fp));
        data[1] = static_cast<char>(fgetc(fp));
        data[2] = static_cast<char>(fgetc(fp));
        data[3] = static_cast<char>(fgetc(fp));

        dng_info->version =
            (data[3] << 24) | (data[2] << 16) | (data[1] << 8) | data[0];
      } break;

      case 50710:  // CFAPlaneColor
      {
        char buf[4];
        size_t readLen = len;
        if (readLen > 4) readLen = 4;
        fread(buf, 1, readLen, fp);
        for (size_t i = 0; i < readLen; i++) {
          infos[idx].cfa_plane_color[i] = buf[i];
        }
      } break;

      case 50711:  // CFALayout
      {
        int layout = Read2(fp, swap_endian);
        infos[idx].cfa_layout = layout;
      } break;

      default:
        break;
    }

    fseek(fp, saved_offt, SEEK_SET);
  }

  return true;
}

static bool ParseDNG(tinydng::DNGInfo* dng_info, TIFFInfo infos[16],
                     int* num_ifds, FILE* fp, bool swap_endian) {
  int offt;
  size_t ret = fread(&offt, 1, 4, fp);
  assert(ret == 4);

  // Init
  dng_info->color_matrix[0][0] = 1.0;
  dng_info->color_matrix[0][1] = 0.0;
  dng_info->color_matrix[0][2] = 0.0;
  dng_info->color_matrix[1][0] = 0.0;
  dng_info->color_matrix[1][1] = 1.0;
  dng_info->color_matrix[1][2] = 0.0;
  dng_info->color_matrix[2][0] = 0.0;
  dng_info->color_matrix[2][1] = 0.0;
  dng_info->color_matrix[2][2] = 1.0;
  dng_info->white_level = 16383;  // 2^14-1 @fixme { The spec says: The default
                                  // value for this tag is (2 ** BitsPerSample)
                                  // -1 for unsigned integer images, and 1.0 for
                                  // floating point images. }
  dng_info->black_level = 0;

  (*num_ifds) = 0;  // initial value = 0
  while (offt) {
    fseek(fp, offt, SEEK_SET);
    if (ParseTIFFIFD(dng_info, infos, num_ifds, fp, swap_endian)) {
      break;
    }
    ret = fread(&offt, 1, 4, fp);
    assert(ret == 4);
  }

  return true;
}

bool LoadDNG(DNGInfo* info, std::vector<unsigned char>* data, size_t* len,
             int* width, int* height, int* bits, int* compos, std::string* err,
             const char* filename, bool is_system_big_endian) {
  std::stringstream ss;

  assert(info);
  assert(data);
  assert(len);
  assert(width);
  assert(height);
  assert(bits);
  assert(compos);

  FILE* fp = fopen(filename, "rb");
  if (!fp) {
    ss << "File not found or cannot open file " << filename << std::endl;
    if (err) {
      (*err) = ss.str();
    }
    return false;
  }

  unsigned short magic;
  char header[32];

  size_t ret;
  ret = fread(&magic, 1, 2, fp);
  assert(ret == 2);

  int seek_ret = fseek(fp, 0, SEEK_SET);
  assert(seek_ret == 0);

  ret = fread(header, 1, 32, fp);
  assert(ret == 32);
  seek_ret = fseek(fp, 0, SEEK_END);
  assert(seek_ret == 0);

  bool is_dng_big_endian = false;

  if (magic == 0x4949) {
    // might be TIFF(DNG).
  } else if (magic == 0x4d4d) {
    // might be TIFF(DNG, bigendian).
    is_dng_big_endian = true;
  } else {
    ss << "Seems the file is not DNG format." << std::endl;
    if (err) {
      (*err) = ss.str();
    }

    fclose(fp);
    return false;
  }

  TIFFInfo infos[16];
  int num_ifds = 0;

  // skip magic header
  fseek(fp, 4, SEEK_SET);
  const bool swap_endian = (is_dng_big_endian && (!is_system_big_endian));
  ret = ParseDNG(info, infos, &num_ifds, fp, swap_endian);

  if (ret) {
    // Choose the largest image
    int max_idx = 0;
    int max_width = 0;
    for (int i = 0; i < num_ifds; i++) {
      if (infos[i].width > max_width) {
        max_idx = i;
        max_width = infos[i].width;
      }
    }

    assert(max_idx < 16);

    int idx = max_idx;

    // @note { Compression: 1 : RAW DNG, 7 : RLE-encoded lossless DNG? 8 :
    // defalate(ZIP), 34892: lossy JPEG)
    assert(infos[idx].compression == 1);
    assert(((infos[idx].width * infos[idx].height * infos[idx].bps) % 8) == 0);
    (*len) = static_cast<size_t>(
        (infos[idx].width * infos[idx].height * infos[idx].bps) / 8);
    assert((*len) > 0);
    data->resize((*len));
    fseek(fp, infos[idx].offset, SEEK_SET);
    ret = fread(&data->at(0), 1, (*len), fp);
    assert(ret == (*len));

    if (infos[idx].has_active_area) {
      info->active_area[0] = infos[idx].active_area[0];
      info->active_area[1] = infos[idx].active_area[1];
      info->active_area[2] = infos[idx].active_area[2];
      info->active_area[3] = infos[idx].active_area[3];
    } else {
      info->active_area[0] = 0;
      info->active_area[1] = 0;
      info->active_area[2] = infos[idx].width;
      info->active_area[3] = infos[idx].height;
    }

    info->cfa_plane_color[0] = infos[idx].cfa_plane_color[0];
    info->cfa_plane_color[1] = infos[idx].cfa_plane_color[1];
    info->cfa_plane_color[2] = infos[idx].cfa_plane_color[2];
    info->cfa_plane_color[3] = infos[idx].cfa_plane_color[3];
    info->cfa_pattern[0][0] = infos[idx].cfa_pattern[0][0];
    info->cfa_pattern[0][1] = infos[idx].cfa_pattern[0][1];
    info->cfa_pattern[1][0] = infos[idx].cfa_pattern[1][0];
    info->cfa_pattern[1][1] = infos[idx].cfa_pattern[1][1];
    info->cfa_layout = infos[idx].cfa_layout;

    info->tile_width = infos[idx].tile_width;
    info->tile_length = infos[idx].tile_length;
    info->tile_offset = infos[idx].tile_offset;

    (*width) = infos[idx].width;
    (*height) = infos[idx].height;
    (*bits) = infos[idx].bps;
    (*compos) = infos[idx].samples;
  }

  fclose(fp);

  return ret;
}

}  // namespace tinydng

#endif

#endif  // TINY_DNG_LOADER_H_
