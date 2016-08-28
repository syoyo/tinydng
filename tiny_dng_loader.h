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
             const char* filename);

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
  int samples;
  int bytes;
  int dummy;
  int active_area[4];  // top, left, bottom, right
} TIFFInfo;

static unsigned int ReadUInt(FILE* fp) {
  unsigned int val;
  size_t ret = fread(&val, 1, 4, fp);
  assert(ret == 4);
  return val;
}

static int ReadInt(int type, FILE* fp) {
  if (type == 3) {
    unsigned short val;
    size_t ret = fread(&val, 1, 2, fp);
    assert(ret == 2);
    return static_cast<int>(val);
  } else {
    unsigned int val;
    size_t ret = fread(&val, 1, 4, fp);
    assert(ret == 4);
    return static_cast<int>(val);
  }
}

static double ReadReal(int type, FILE* fp) {
  assert(type == 10);  // @todo { Support more types. }
  int num = ReadInt(type, fp);
  int denom = ReadInt(type, fp);

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

static bool ParseTIFFIFD(tinydng::DNGInfo* dngInfo, TIFFInfo infos[16],
                         int* numIFDs,  // initial value must be 0
                         FILE* fp) {
  int idx = (*numIFDs)++;  // increament numIFDs.

  unsigned short numEntries;
  size_t ret = fread(&numEntries, 1, 2, fp);
  assert(ret == 2);
  if (numEntries > 512) {
    assert(0);
    return false;  // @fixme
  }

  while (numEntries--) {
    unsigned short tag, type;
    unsigned int len;
    unsigned int saved_offt;
    GetTIFFTag(&tag, &type, &len, &saved_offt, fp);

    switch (tag) {
      case 2:
      case 256:
      case 61441:  // ImageWidth
        infos[idx].width = ReadInt(type, fp);
        break;

      case 3:
      case 257:
      case 61442:  // ImageHeight
        infos[idx].height = ReadInt(type, fp);
        break;

      case 258:
      case 61443:  // BitsPerSample
        infos[idx].samples = len & 7;
        infos[idx].bps = ReadInt(type, fp);
        break;

      case 259:  // Compression
        infos[idx].compression = ReadInt(type, fp);
        break;

      case 273:              // StripOffset
      case 513:              // JpegIFOffset
        assert(tag == 273);  // @todo { jpeg data }
        infos[idx].offset = ReadUInt(fp);
        break;

      case 330:  // SubIFDs

      {
        while (len--) {
          unsigned int i = static_cast<unsigned int>(ftell(fp));
          unsigned int offt = ReadUInt(fp);
          unsigned int base = 0;  // @fixme
          fseek(fp, offt + base, SEEK_SET);

          ParseTIFFIFD(dngInfo, infos, numIFDs, fp);  // recursive call
          fseek(fp, i + 4, SEEK_SET);                 // rewind
        }
      }

      break;

      case 50714:  // BlackLevel
        dngInfo->black_level = ReadInt(type, fp);
        break;

      case 50717:  // WhiteLevel
        dngInfo->white_level = ReadInt(type, fp);
        break;

      case 50721:  // ColorMatrix1
      case 50722:  // ColorMatrix2
      {
        for (int c = 0; c < 3; c++) {
          for (int k = 0; k < 3; k++) {
            double val = ReadReal(type, fp);
            dngInfo->color_matrix[c][k] = val;
          }
        }
      }

      break;

      case 50829:  // ActiveArea
        infos[idx].active_area[0] = ReadInt(type, fp);
        infos[idx].active_area[1] = ReadInt(type, fp);
        infos[idx].active_area[2] = ReadInt(type, fp);
        infos[idx].active_area[3] = ReadInt(type, fp);

        break;

      default:
        break;
    }

    fseek(fp, saved_offt, SEEK_SET);
  }

  return true;
}

static bool ParseDNG(tinydng::DNGInfo* dngInfo, TIFFInfo infos[16],
                     int* numIFDs, FILE* fp) {
  int offt;
  size_t ret = fread(&offt, 1, 4, fp);
  assert(ret == 4);

  // Init
  dngInfo->color_matrix[0][0] = 1.0;
  dngInfo->color_matrix[0][1] = 0.0;
  dngInfo->color_matrix[0][2] = 0.0;
  dngInfo->color_matrix[1][0] = 0.0;
  dngInfo->color_matrix[1][1] = 1.0;
  dngInfo->color_matrix[1][2] = 0.0;
  dngInfo->color_matrix[2][0] = 0.0;
  dngInfo->color_matrix[2][1] = 0.0;
  dngInfo->color_matrix[2][2] = 1.0;
  dngInfo->white_level = 16383;  // 2^14-1
  dngInfo->black_level = 0;

  (*numIFDs) = 0;  // initial value = 0
  while (offt) {
    fseek(fp, offt, SEEK_SET);
    if (ParseTIFFIFD(dngInfo, infos, numIFDs, fp)) {
      break;
    }
    ret = fread(&offt, 1, 4, fp);
    assert(ret == 4);
  }

  return true;
}

bool LoadDNG(DNGInfo* info, std::vector<unsigned char>* data, size_t* len,
             int* width, int* height, int* bits, int* compos, std::string* err,
             const char* filename) {
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

  // size_t fsize = static_cast<size_t>(ftell(fp));

  if (magic == 0x4949 || magic == 0x4d4d) {
    // might be TIFF(DNG).
  } else {
    ss << "Seems the file is not DNG format." << std::endl;
    if (err) {
      (*err) = ss.str();
    }

    fclose(fp);
    return false;
  }

  TIFFInfo infos[16];
  int numIFDs = 0;

  // skip magic header
  fseek(fp, 4, SEEK_SET);
  ret = ParseDNG(info, infos, &numIFDs, fp);

  if (ret) {
    // Find largest image
    int max_idx = 0;
    int max_width = 0;
    for (int i = 0; i < numIFDs; i++) {
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
