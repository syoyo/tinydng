/*
The MIT License (MIT)

Copyright (c) 2016 Syoyo Fujita

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

#ifdef _MSC_VER
#pragma warning(disable : 4100)
#pragma warning(disable : 4101)
#pragma warning(disable : 4189)
#pragma warning(disable : 4244)
#pragma warning(disable : 4267)
#pragma warning(disable : 4305)
#include <Windows.h>
#endif

#define USE_OPENGL2
#include "OpenGLWindow/OpenGLInclude.h"
#ifdef _WIN32
#include "OpenGLWindow/Win32OpenGLWindow.h"
#elif defined __APPLE__
#include "OpenGLWindow/MacOpenGLWindow.h"
#else
// assume linux
#include "OpenGLWindow/X11OpenGLWindow.h"
#endif

#ifdef _WIN32
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#endif

#ifdef _WIN32
#ifdef __cplusplus
extern "C" {
#endif
#include <mmsystem.h>
#include <windows.h>
#ifdef __cplusplus
}
#endif
#pragma comment(lib, "winmm.lib")
#else
#if defined(__unix__) || defined(__APPLE__)
#include <sys/time.h>
#else
#include <ctime>
#endif
#endif

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_btgui.h"

#define TINY_DNG_LOADER_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#include "../../tiny_dng_loader.h"

b3gDefaultOpenGLWindow* window = 0;
int gWidth = 512;
int gHeight = 512;
int gMousePosX = -1, gMousePosY = -1;
bool gMouseLeftDown = false;

class timer {
 public:
#ifdef _WIN32
  typedef DWORD time_t;

  timer() { ::timeBeginPeriod(1); }
  ~timer() { ::timeEndPeriod(1); }

  void start() { t_[0] = ::timeGetTime(); }
  void end() { t_[1] = ::timeGetTime(); }

  time_t sec() { return (time_t)((t_[1] - t_[0]) / 1000); }
  time_t msec() { return (time_t)((t_[1] - t_[0])); }
  time_t usec() { return (time_t)((t_[1] - t_[0]) * 1000); }

#else
#if defined(__unix__) || defined(__APPLE__)
  typedef unsigned long int time_t;

  void start() { gettimeofday(tv + 0, &tz); }
  void end() { gettimeofday(tv + 1, &tz); }

  time_t sec() { return (time_t)(tv[1].tv_sec - tv[0].tv_sec); }
  time_t msec() {
    return this->sec() * 1000 +
           (time_t)((tv[1].tv_usec - tv[0].tv_usec) / 1000);
  }
  time_t usec() {
    return this->sec() * 1000000 + (time_t)(tv[1].tv_usec - tv[0].tv_usec);
  }

#else  // C timer
  // using namespace std;
  typedef clock_t time_t;

  void start() { t_[0] = clock(); }
  void end() { t_[1] = clock(); }

  time_t sec() { return (time_t)((t_[1] - t_[0]) / CLOCKS_PER_SEC); }
  time_t msec() { return (time_t)((t_[1] - t_[0]) * 1000 / CLOCKS_PER_SEC); }
  time_t usec() { return (time_t)((t_[1] - t_[0]) * 1000000 / CLOCKS_PER_SEC); }

#endif
#endif

 private:
#ifdef _WIN32
  DWORD t_[2];
#else
#if defined(__unix__) || defined(__APPLE__)
  struct timeval tv[2];
  struct timezone tz;
#else
  time_t t_[2];
#endif
#endif
};

typedef struct {
  int width;
  int height;
  int bits;
  int components;
  int largest_idx;

  // Decoded image.
  tinydng::DNGImage image;

  // HDR RAW data
  std::vector<float> hdr_image;

  // Developed image.
  std::vector<float> framebuffer;

} RAWImage;

typedef struct {
  float intensity;
  bool flip_y;

  float view_offset[2];
  int view_scale;  // percentage.
  float display_gamma;
  int cfa_offset[2];  // CFA offset.
  int cfa_pattern[4];

  float color_matrix[3][3];
  float inv_color_matrix[3][3];

  float raw_white_balance[3];

  bool use_as_shot_neutral;
  float as_shot_neutral[3];

  // Pixel inspection
  float inspect_raw_value;         // RAW censor value
  float inspect_after_debayer[3];  // RAW color value after debayer
  int inspect_pos[2];

  // Perf counter
  double color_correction_msec;
  double debayer_msec;
  double develop_msec;

  // Per channel black/white level.
  int black_level[4] = {0, 0, 0, 0};
  int white_level[4] = {-1, -1, -1, -1};

  float hue;
  float saturation;
  float sepiaAmount;
  float brightness;
  float contrast;
  float vibranceAmount;
} UIParam;

RAWImage gRAWImage;
UIParam gUIParam;

struct Vertex {
  GLfloat pos[2];
  GLfloat texcoord[2];
};
static Vertex QUAD[4] = {{{-1.0f, -1.0f}, {0.0, 0.0}},
                         {{1.0f, -1.0f}, {1.0, 0.0}},
                         {{-1.0f, 1.0f}, {0.0, 1.0}},
                         {{1.0f, 1.0f}, {1.0, 1.0}}};

static const char gVertexShaderStr[] =
    "#version 120\n"
    "attribute vec2 pos;\n"
    "attribute vec2 texcoord;\n"
    "varying vec2 vTexcoord;\n"
    "void main() {\n"
    "    gl_Position = vec4(pos, 0.0, 1.0);\n"
    "    vTexcoord = texcoord;\n"
    "}\n";

static const char gFragmentShaderStr[] =
    "#version 120\n"
    "varying vec2 vTexcoord;\n"
    "uniform float uGamma;\n"
    "uniform vec2  uOffset;\n"
    "uniform vec2  uTexsize;\n"
    "uniform float uScale;\n"
    "uniform float uIntensity;\n"
    "uniform sampler2D tex;\n"
    "void main() {\n"
    "    vec2 tcoord = (vTexcoord + uOffset) - vec2(0.5, 0.5);\n"
    "    tcoord = uScale * tcoord + vec2(0.5, 0.5);\n"
    "    vec3 col = uIntensity * texture2D(tex, tcoord).rgb;\n"
    "    col = clamp(pow(col, vec3(1.0f / uGamma)), 0.0, 1.0);\n"
    "    gl_FragColor = vec4(col, 1.0);\n"
    "}\n";

typedef struct {
  GLint program;

  GLuint vb;

  GLint pos_attrib;
  GLint texcoord_attrib;

  GLint gamma_loc;
  GLint intensity_loc;
  GLint uv_offset_loc;
  GLint uv_scale_loc;
  GLint texture_size_loc;
  GLint tex_loc;

  GLuint tex_id;
} GLContext;

GLContext gGLCtx;

static inline unsigned short swap2(unsigned short val) {
  unsigned short ret;

  char* buf = reinterpret_cast<char*>(&ret);

  short x = val;
  buf[1] = x;
  buf[0] = x >> 8;

  return ret;
}

// sRGB = XYZD65_to_sRGB * XYZD50_to_XYZD65 * ForwardMatrix(wbCCT) * rawWB *
// CameraRaw
static const double xyzD65_to_sRGB[3][3] = {{3.2406, -1.5372, -0.4986},
                                            {-0.9689, 1.8758, 0.0415},
                                            {0.0557, -0.2040, 1.0570}};

static const double xyzD50_to_xyzD65[3][3] = {
    {0.9555766, -0.0230393, 0.0631636},
    {-0.0282895, 1.0099416, 0.0210077},
    {0.0122982, -0.0204830, 1.3299098}};

// From the Halide camera_pipe's color_correct
void MakeColorMatrix(float color_matrix[], float temperature) {
  float alpha = (1.0 / temperature - 1.0 / 3200) / (1.0 / 7000 - 1.0 / 3200);

  color_matrix[0] = alpha * 1.6697f + (1 - alpha) * 2.2997f;
  color_matrix[1] = alpha * -0.2693f + (1 - alpha) * -0.4478f;
  color_matrix[2] = alpha * -0.4004f + (1 - alpha) * 0.1706f;
  color_matrix[3] = alpha * -42.4346f + (1 - alpha) * -39.0923f;

  color_matrix[4] = alpha * -0.3576f + (1 - alpha) * -0.3826f;
  color_matrix[5] = alpha * 1.0615f + (1 - alpha) * 1.5906f;
  color_matrix[6] = alpha * 1.5949f + (1 - alpha) * -0.2080f;
  color_matrix[7] = alpha * -37.1158f + (1 - alpha) * -25.4311f;

  color_matrix[8] = alpha * -0.2175f + (1 - alpha) * -0.0888f;
  color_matrix[9] = alpha * -1.8751f + (1 - alpha) * -0.7344f;
  color_matrix[10] = alpha * 6.9640f + (1 - alpha) * 2.2832f;
  color_matrix[11] = alpha * -26.6970f + (1 - alpha) * -20.0826f;
}

static inline void InverseMatrix33(double dst[3][3], double m[3][3]) {
  // computes the inverse of a matrix m
  double det = m[0][0] * (m[1][1] * m[2][2] - m[2][1] * m[1][2]) -
               m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
               m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);

  double invdet = 1 / det;

  dst[0][0] = (m[1][1] * m[2][2] - m[2][1] * m[1][2]) * invdet;
  dst[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * invdet;
  dst[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * invdet;
  dst[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * invdet;
  dst[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * invdet;
  dst[1][2] = (m[1][0] * m[0][2] - m[0][0] * m[1][2]) * invdet;
  dst[2][0] = (m[1][0] * m[2][1] - m[2][0] * m[1][1]) * invdet;
  dst[2][1] = (m[2][0] * m[0][1] - m[0][0] * m[2][1]) * invdet;
  dst[2][2] = (m[0][0] * m[1][1] - m[1][0] * m[0][1]) * invdet;
}

void MatrixMult(double dst[3][3], const double a[3][3], const double b[3][3]) {
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      dst[i][j] = 0.0;
      for (int c = 0; c < 3; c++) {
        dst[i][j] += a[i][c] * b[c][j];
      }
    }
  }
}

//
// Decode 12bit integer image into floating point HDR image
//
void decode12_hdr(std::vector<float>& image, unsigned char* data, int width,
                  int height, bool do_swap) {
  int offsets[2][2] = {{0, 1}, {1, 2}};

  int bit_shifts[2] = {4, 0};

  image.resize(width * height);

#pragma omp parallel for schedule(dynamic, 1)
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      unsigned char buf[3];

      // Calculate load addres for 12bit pixel(three 8 bit pixels)
      int n = int(y * width + x);

      // 24 = 12bit * 2 pixel, 8bit * 3 pixel
      int n2 = n % 2;           // used for offset & bitshifts
      int addr3 = (n / 2) * 3;  // 8bit pixel pos
      int odd = (addr3 % 2);

      int bit_shift;
      bit_shift = bit_shifts[n2];

      int offset[2];
      offset[0] = offsets[n2][0];
      offset[1] = offsets[n2][1];

      if (do_swap) {
        // load with short byte swap
        if (odd) {
          buf[0] = data[addr3 - 1];
          buf[1] = data[addr3 + 2];
          buf[2] = data[addr3 + 1];
        } else {
          buf[0] = data[addr3 + 1];
          buf[1] = data[addr3 + 0];
          buf[2] = data[addr3 + 3];
        }
      } else {
        buf[0] = data[addr3 + 0];
        buf[1] = data[addr3 + 1];
        buf[2] = data[addr3 + 2];
      }
      unsigned int b0 = (unsigned int)buf[offset[0]] & 0xff;
      unsigned int b1 = (unsigned int)buf[offset[1]] & 0xff;

      unsigned int val = (b0 << 8) | b1;
      val = 0xfff & (val >> bit_shift);

      image[y * width + x] = (float)val;
    }
  }
}

//
// Decode 14bit integer image into floating point HDR image
//
void decode14_hdr(std::vector<float>& image, unsigned char* data, int width,
                  int height, bool do_swap) {
  int offsets[4][3] = {{0, 0, 1}, {1, 2, 3}, {3, 4, 5}, {5, 5, 6}};

  int bit_shifts[4] = {2, 4, 6, 0};

  image.resize(width * height);

#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic, 1)
#endif
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      unsigned char buf[7];

      // Calculate load addres for 14bit pixel(three 8 bit pixels)
      int n = int(y * width + x);

      // 56 = 14bit * 4 pixel, 8bit * 7 pixel
      int n4 = n % 4;           // used for offset & bitshifts
      int addr7 = (n / 4) * 7;  // 8bit pixel pos
      int odd = (addr7 % 2);

      int offset[3];
      offset[0] = offsets[n4][0];
      offset[1] = offsets[n4][1];
      offset[2] = offsets[n4][2];

      int bit_shift;
      bit_shift = bit_shifts[n4];

      if (do_swap) {
        // load with short byte swap
        if (odd) {
          buf[0] = data[addr7 - 1];
          buf[1] = data[addr7 + 2];
          buf[2] = data[addr7 + 1];
          buf[3] = data[addr7 + 4];
          buf[4] = data[addr7 + 3];
          buf[5] = data[addr7 + 6];
          buf[6] = data[addr7 + 5];
        } else {
          buf[0] = data[addr7 + 1];
          buf[1] = data[addr7 + 0];
          buf[2] = data[addr7 + 3];
          buf[3] = data[addr7 + 2];
          buf[4] = data[addr7 + 5];
          buf[5] = data[addr7 + 4];
          buf[6] = data[addr7 + 7];
        }
      } else {
        memcpy(buf, &data[addr7], 7);
      }
      unsigned int b0 = (unsigned int)buf[offset[0]] & 0xff;
      unsigned int b1 = (unsigned int)buf[offset[1]] & 0xff;
      unsigned int b2 = (unsigned int)buf[offset[2]] & 0xff;

      // unsigned int val = (b0 << 16) | (b1 << 8) | b2;
      // unsigned int val = (b2 << 16) | (b0 << 8) | b0;
      unsigned int val = (b0 << 16) | (b1 << 8) | b2;
      // unsigned int val = b2;
      val = 0x3fff & (val >> bit_shift);

      image[y * width + x] = (float)val;
    }
  }
}

//
// Decode 16bit integer image into floating point HDR image
//
void decode16_hdr(std::vector<float>& image, unsigned char* data, int width,
                  int height, bool do_swap) {
  image.resize(width * height);
  unsigned short* ptr = reinterpret_cast<unsigned short*>(data);

#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic, 1)
#endif
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      unsigned short val = ptr[y * width + x];
      if (do_swap) {
        val = swap2(val);
      }

      // range will be [0, 65535]
      image[y * width + x] = (float)val;
    }
  }
}

static inline double fclamp(double x, double minx, double maxx) {
  if (x < minx) return minx;
  if (x > maxx) return maxx;
  return x;
}

static inline int clamp(int x, int minx, int maxx) {
  if (x < minx) return minx;
  if (x > maxx) return maxx;
  return x;
}

static inline float fetch(const std::vector<float>& in, int x, int y, int w,
                          int h) {
  int xx = clamp(x, 0, w - 1);
  int yy = clamp(y, 0, h - 1);
  return in[yy * w + xx];
}

// Simple debayer.
// debayerOffset = pixel offset to make CFA pattern RGGB.
static double debayer(std::vector<float>& out, const std::vector<float>& in,
                      int width, int height, const int debayerOffset[2]) {
  timer t_debayer;

  t_debayer.start();

  if (out.size() != width * height * 3) {
    out.resize(width * height * 3);
  }

  // printf("offset = %d, %d\n", debayerOffset[0], debayerOffset[1]);

#ifdef _OPENMP
#pragma omp parallel for
#endif
  for (int yy = 0; yy < height; yy++) {
    for (int xx = 0; xx < width; xx++) {
      int x = xx + debayerOffset[0];
      int y = yy + debayerOffset[1];

      int xCoord[4] = {x - 2, x - 1, x + 1, x + 2};
      int yCoord[4] = {y - 2, y - 1, y + 1, y + 2};

      float C = fetch(in, x, y, width, height);
      float kC[4] = {4.0 / 8.0, 6.0 / 8.0, 5.0 / 8.0, 5.0 / 8.0};

      int altername[2];
      altername[0] = xx % 2;
      altername[1] = yy % 2;
      // printf("x, y, alternate = %d, %d, %d, %d\n",
      //  x, y, altername[0], altername[1]);

      // int pattern = CFAPattern[y%2][x%2];

      float Dvec[4];
      Dvec[0] = fetch(in, xCoord[1], yCoord[1], width, height);
      Dvec[1] = fetch(in, xCoord[1], yCoord[2], width, height);
      Dvec[2] = fetch(in, xCoord[2], yCoord[1], width, height);
      Dvec[3] = fetch(in, xCoord[2], yCoord[2], width, height);

      // vec4 PATTERN = (kC.xyz * C).xyzz;
      float PATTERN[4];
      PATTERN[0] = kC[0] * C;
      PATTERN[1] = kC[1] * C;
      PATTERN[2] = kC[2] * C;
      PATTERN[3] = kC[2] * C;

      // D = Dvec[0] + Dvec[1] + Dvec[2] + Dvec[3]
      Dvec[0] += Dvec[1] + Dvec[2] + Dvec[3];

      float value[4];
      value[0] = fetch(in, x, yCoord[0], width, height);
      value[1] = fetch(in, x, yCoord[1], width, height);
      value[2] = fetch(in, xCoord[0], y, width, height);
      value[3] = fetch(in, xCoord[1], y, width, height);

      float temp[4];
      temp[0] = fetch(in, x, yCoord[3], width, height);
      temp[1] = fetch(in, x, yCoord[2], width, height);
      temp[2] = fetch(in, xCoord[3], y, width, height);
      temp[3] = fetch(in, xCoord[2], y, width, height);

      float kA[4] = {-1.0 / 8.0, -1.5 / 8.0, 0.5 / 8.0, -1.0 / 8.0};
      float kB[4] = {2.0 / 8.0, 0.0 / 8.0, 0.0 / 8.0, 4.0 / 8.0};
      float kD[4] = {0.0 / 8.0, 2.0 / 8.0, -1.0 / 8.0, -1.0 / 8.0};

      value[0] += temp[0];
      value[1] += temp[1];
      value[2] += temp[2];
      value[3] += temp[3];

      //#define kE (kA.xywz)
      //#define kF (kB.xywz)
      float kE[4];
      kE[0] = kA[0];
      kE[1] = kA[1];
      kE[2] = kA[3];
      kE[3] = kA[2];

      float kF[4];
      kF[0] = kB[0];
      kF[1] = kB[1];
      kF[2] = kB[3];
      kF[3] = kB[2];

      //#define A (value[0])
      //#define B (value[1])
      //#define D (Dvec.x)
      //#define E (value[2])
      //#define F (value[3])

      // PATTERN.yzw += (kD.yz * D).xyy;
      //
      // PATTERN += (kA.xyz * A).xyzx + (kE.xyw * E).xyxz;
      // PATTERN.xw  += kB.xw * B;
      // PATTERN.xz  += kF.xz * F;

      PATTERN[1] += kD[1] * Dvec[0];
      PATTERN[2] += kD[2] * Dvec[0];
      PATTERN[3] += kD[2] * Dvec[0];

      PATTERN[0] += kA[0] * value[0] + kE[0] * value[2];
      PATTERN[1] += kA[1] * value[0] + kE[1] * value[2];
      PATTERN[2] += kA[2] * value[0] + kE[0] * value[2];
      PATTERN[3] += kA[0] * value[0] + kE[3] * value[2];

      PATTERN[0] += kB[0] * value[1];
      PATTERN[3] += kB[3] * value[1];

      PATTERN[0] += kF[0] * value[3];
      PATTERN[2] += kF[2] * value[3];

      float rgb[3];
      if (altername[1] == 0) {
        if (altername[0] == 0) {
          rgb[0] = C;
          rgb[1] = PATTERN[0];
          rgb[2] = PATTERN[1];
        } else {
          rgb[0] = PATTERN[2];
          rgb[1] = C;
          rgb[2] = PATTERN[3];
        }
      } else {
        if (altername[0] == 0) {
          rgb[0] = PATTERN[3];
          rgb[1] = C;
          rgb[2] = PATTERN[2];
        } else {
          rgb[0] = PATTERN[1];
          rgb[1] = PATTERN[0];
          rgb[2] = C;
        }
      }

      out[3 * (yy * width + xx) + 0] = rgb[0];
      out[3 * (yy * width + xx) + 1] = rgb[1];
      out[3 * (yy * width + xx) + 2] = rgb[2];
    }
  }

  t_debayer.end();

  return t_debayer.msec();
}

static void compute_color_matrix(double dst[3][3],
                                 const double color_matrix[3][3],
                                 const float wb[3]) {
  //
  // See "Mapping Camera Color Space to CIE XYZ Space" section in DNG spec for
  // more details.
  // @todo { Consider AnalogBalance, ForwardMatrix, CameraCalibration,
  // ReudctionMatrix }
  //
  // CM = ColorMatrix
  // CC = CameraCalibration
  // AB = AnalogBalance
  // RM = ReductionMatrix
  // FM = ForwardMatrix
  //

  // CM(Color Matrix) is a matrix which converts XYZ value to camera space,
  // Thus we need to invert it to get XYZ value from camera's raw censor value
  // (Unless ForwardMatrix is not available)

  // XYZtoCamera = AB * (CC) * CM;
  double xyz_to_camera[3][3];

  const double analog_balance[3][3] = {
      {wb[0], 0, 0}, {0, wb[1], 0}, {0, 0, wb[2]}};

  MatrixMult(xyz_to_camera, analog_balance, color_matrix);

  // Assume ForwardMatrix not included.
  // CamraToXYZ = Inverse(XYZtoCamer);
  double camera_to_xyz[3][3];
  InverseMatrix33(camera_to_xyz, xyz_to_camera);

  // DBG
  for (int j = 0; j < 3; j++) {
    for (int i = 0; i < 3; i++) {
      gUIParam.inv_color_matrix[j][i] = camera_to_xyz[j][i];
    }
  }

  // @todo { CB }
  // XYZD65tosRGB * XYZD50toXYZD65 * (CB) * CameraToXYZ

  // Bradford.
  // @fixme { Is this correct matrix? }
  const double chromatic_abbrebiation[3][3] = {
      {0.8951000, 0.2664000, -0.1614000},
      {-0.7502000, 1.7135000, 0.0367000},
      {0.0389000, -0.0685000, 1.0296000}};

  double tmp0[3][3];
  double tmp1[3][3];
  // MatrixMult(tmp0, chromatic_abbrebiation, camera_to_xyz);
  // MatrixMult(tmp1, xyzD50_to_xyzD65, tmp0);
  MatrixMult(tmp1, xyzD50_to_xyzD65, camera_to_xyz);
  MatrixMult(dst, xyzD65_to_sRGB, tmp1);
  // MatrixMult(dst, chromatic_abbrebiation, camera_to_xyz);
  // MatrixMult(dst, camera_to_xyz, chromatic_abbrebiation);
  // MatrixMult(dst, chromatic_abbrebiation, camera_to_xyz);
}

static void pre_color_correction(std::vector<float>& out,
                                 const std::vector<float>& in,
                                 const int* black_levels,  // per channel black levels
                                 const int* white_levels,  // per channel white levels
                                 int width, int height, int channels,
                                 float scale) {
  assert(in.size() == (width * height * channels));

  if (out.size() != width * height * channels) {
    out.resize(width * height * channels);
  }

  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      for (int c = 0; c < channels; c++) {
        double val = in[channels * (y * width + x) + c];

        val -= black_levels[c];

        out[channels * (y * width + x)  + c] =
            fclamp(val, 0.0, std::min(double(white_levels[c]), 65535.0));
      }
    }
  }
}

// https://github.com/evanw/glfx.js/blob/master/src/filters/adjust/sepia.js
// amount: 0 to 1
void sepia(std::vector<float>& out, const std::vector<float>& in, int width,
           int height, float sepiaAmount, float maxValue) {
#ifdef _OPENMP
#pragma omp parallel for
#endif
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      float r = in[3 * (y * width + x) + 0] / maxValue;
      float g = in[3 * (y * width + x) + 1] / maxValue;
      float b = in[3 * (y * width + x) + 2] / maxValue;

      float outColor[3];
      outColor[0] = std::min(1.0, (r * (1.0 - (0.607 * sepiaAmount))) +
                                      (g * (0.769 * sepiaAmount)) +
                                      (b * (0.189 * sepiaAmount)));
      outColor[1] = std::min(1.0, (r * 0.349 * sepiaAmount) +
                                      (g * (1.0 - (0.314 * sepiaAmount))) +
                                      (b * 0.168 * sepiaAmount));
      outColor[2] =
          std::min(1.0, (r * 0.272 * sepiaAmount) + (g * 0.534 * sepiaAmount) +
                            (b * (1.0 - (0.869 * sepiaAmount))));

      out[3 * (y * width + x) + 0] = outColor[0] * maxValue;
      out[3 * (y * width + x) + 1] = outColor[1] * maxValue;
      out[3 * (y * width + x) + 2] = outColor[2] * maxValue;
    }
  }
}

// hue & saturation filter
// https://github.com/evanw/glfx.js/blob/master/src/filters/adjust/huesaturation.js
// hue -1 to 1
// saturation -1 to 1
void hue_saturation(std::vector<float>& out, const std::vector<float>& in,
                    int width, int height, float hue, float saturation,
                    float maxValue) {
#ifdef _OPENMP
#pragma omp parallel for
#endif
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      float r = in[3 * (y * width + x) + 0] / maxValue;
      float g = in[3 * (y * width + x) + 1] / maxValue;
      float b = in[3 * (y * width + x) + 2] / maxValue;

      float angle = hue * 3.14159265;
      float s = sin(angle), c = cos(angle);

      float weights[3];
      weights[0] = (2.0 * c + 1.) / 3.;
      weights[1] = (-sqrt(3.0) * s - c + 1) / 3.;
      weights[2] = (sqrt(3.0) * s - c + 1) / 3.;

      float outColor[3];
      outColor[0] = r * weights[0] + g * weights[1] + b * weights[2];
      outColor[1] = r * weights[2] + g * weights[0] + b * weights[1];
      outColor[2] = r * weights[1] + g * weights[2] + b * weights[0];

      /* saturation adjustment */
      float average = (outColor[0] + outColor[1] + outColor[2]) / 3.0;
      if (saturation > 0.0) {
        // color.rgb += (average - color.rgb) * (1.0 - 1.0 / (1.001 -
        // saturation));
        outColor[0] +=
            (average - outColor[0]) * (1.0 - 1.0 / (1.001 - saturation));
        outColor[1] +=
            (average - outColor[1]) * (1.0 - 1.0 / (1.001 - saturation));
        outColor[2] +=
            (average - outColor[2]) * (1.0 - 1.0 / (1.001 - saturation));
      } else {
        // color.rgb += (average - color.rgb) * (-saturation);
        outColor[0] += (average - outColor[0]) * (-saturation);
        outColor[1] += (average - outColor[1]) * (-saturation);
        outColor[2] += (average - outColor[2]) * (-saturation);
      }

      out[3 * (y * width + x) + 0] = outColor[0] * maxValue;
      out[3 * (y * width + x) + 1] = outColor[1] * maxValue;
      out[3 * (y * width + x) + 2] = outColor[2] * maxValue;
    }
  }
}

// https://github.com/evanw/glfx.js/blob/master/src/filters/adjust/brightnesscontrast.js
// brightness -1 to 1
// contrast -1 to 1
void brightness_contrast(std::vector<float>& out, const std::vector<float>& in,
                         int width, int height, float brightness,
                         float contrast, float maxValue) {
#ifdef _OPENMP
#pragma omp parallel for
#endif
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      float r = in[3 * (y * width + x) + 0] / maxValue;
      float g = in[3 * (y * width + x) + 1] / maxValue;
      float b = in[3 * (y * width + x) + 2] / maxValue;
      r += brightness;
      g += brightness;
      b += brightness;

      float outColor[3];
      if (contrast > 0.0) {
        outColor[0] = (r - 0.5) / (1.0 - contrast) + 0.5;
        outColor[1] = (g - 0.5) / (1.0 - contrast) + 0.5;
        outColor[2] = (b - 0.5) / (1.0 - contrast) + 0.5;
      } else {
        outColor[0] = (r - 0.5) * (1.0 + contrast) + 0.5;
        outColor[1] = (g - 0.5) * (1.0 + contrast) + 0.5;
        outColor[2] = (b - 0.5) * (1.0 + contrast) + 0.5;
      }

      out[3 * (y * width + x) + 0] = outColor[0] * maxValue;
      out[3 * (y * width + x) + 1] = outColor[1] * maxValue;
      out[3 * (y * width + x) + 2] = outColor[2] * maxValue;
    }
  }
}

// https://github.com/evanw/glfx.js/blob/master/src/filters/adjust/vibrance.js
// vibranceAmount -1 to 1
void vibrance(std::vector<float>& out, const std::vector<float>& in, int width,
              int height, float vibranceAmount, float maxValue) {
#ifdef _OPENMP
#pragma omp parallel for
#endif
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      float r = in[3 * (y * width + x) + 0] / maxValue;
      float g = in[3 * (y * width + x) + 1] / maxValue;
      float b = in[3 * (y * width + x) + 2] / maxValue;
      //      printf("col r  %f \n", r);
      float average = (r + g + b) / 3.0;
      float mx = std::max(r, std::max(g, b));
      float amt = (mx - average) * (-vibranceAmount * 3.0);

      out[3 * (y * width + x) + 0] = (r * (1. - amt) + mx * (amt)) * maxValue;
      out[3 * (y * width + x) + 1] = (g * (1. - amt) + mx * (amt)) * maxValue;
      out[3 * (y * width + x) + 2] = (b * (1. - amt) + mx * (amt)) * maxValue;
    }
  }
}

// Simple color correctionr.
double color_correction(std::vector<float>& out, const std::vector<float>& in,
                        int width, int height,
                        const double color_matrix[3][3]) {
  out.resize(in.size());

#ifdef _OPENMP
#pragma omp parallel for
#endif
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      float r = in[3 * (y * width + x) + 0];
      float g = in[3 * (y * width + x) + 1];
      float b = in[3 * (y * width + x) + 2];

      float R = color_matrix[0][0] * r + color_matrix[1][0] * g +
                color_matrix[2][0] * b;
      float G = color_matrix[0][1] * r + color_matrix[1][1] * g +
                color_matrix[2][1] * b;
      float B = color_matrix[0][2] * r + color_matrix[1][2] * g +
                color_matrix[2][2] * b;

      out[3 * (y * width + x) + 0] = R;
      out[3 * (y * width + x) + 1] = G;
      out[3 * (y * width + x) + 2] = B;
    }
  }

  // TODO(syoyo): report processing time.
  return 0.0;
}

void DecodeToHDR(RAWImage* raw, bool swap_endian) {
  raw->hdr_image.resize(raw->width * raw->height * raw->components);

  if (raw->bits == 12) {
    decode12_hdr(raw->hdr_image, raw->image.data.data(), raw->width * raw->components,
                 raw->height, swap_endian);
  } else if (raw->bits == 14) {
    decode14_hdr(raw->hdr_image, raw->image.data.data(), raw->width * raw->components,
                 raw->height, swap_endian);
  } else if (raw->bits == 16) {
    decode16_hdr(raw->hdr_image, raw->image.data.data(), raw->width * raw->components,
                 raw->height, swap_endian);
  } else {
    assert(0);
    exit(-1);
  }
}

// @todo { debayer, color correction, etc. }
void Develop(RAWImage* raw, const UIParam& param) {
  timer t_develop;

  t_develop.start();

  if (raw->framebuffer.size() != (raw->width * raw->height * 3)) {
    raw->framebuffer.resize(raw->width * raw->height * 3);
  }

  const float inv_scale = 1.0f / (param.white_level[0] - param.black_level[0]);

  std::vector<float> pre_color_corrected;
  pre_color_correction(pre_color_corrected, raw->hdr_image,
                       param.black_level, param.white_level,
                       raw->width, raw->height, raw->components,
                       /* scale */ 1.0f);

  std::vector<float> demosaiced;
  if (raw->components == 3) {
    // RAW contains demosaiced image.
    demosaiced = pre_color_corrected;
  } else {
    gUIParam.debayer_msec = debayer(demosaiced, pre_color_corrected, raw->width,
                                    raw->height, param.cfa_offset);
  }

#if 0
  double srgb_color_matrix[3][3];
  compute_color_matrix(srgb_color_matrix, raw->image.color_matrix1,
                       param.raw_white_balance);

  std::vector<float> color_corrected;
  gUIParam.color_correction_msec = color_correction(
      color_corrected, demosaiced, raw->width, raw->height, srgb_color_matrix);

  // filter
  float maxValue =
      fclamp(param.white_level[0] - param.black_level[0], 0.0, 65535.0);
  sepia(color_corrected, color_corrected, raw->width, raw->height,
        param.sepiaAmount, maxValue);
  hue_saturation(color_corrected, color_corrected, raw->width, raw->height,
                 param.hue, param.saturation, maxValue);
  brightness_contrast(color_corrected, color_corrected, raw->width, raw->height,
                      param.brightness, param.contrast, maxValue);
  vibrance(color_corrected, color_corrected, raw->width, raw->height,
           param.vibranceAmount, maxValue);

#else
  std::vector<float> color_corrected = demosaiced;
#endif

  for (size_t y = 0; y < raw->height; y++) {
    for (size_t x = 0; x < raw->width; x++) {
      int Y = (param.flip_y) ? (raw->height - y - 1) : y;

      raw->framebuffer[3 * (Y * raw->width + x) + 0] =
          param.intensity * color_corrected[3 * (y * raw->width + x) + 0] *
          inv_scale;
      raw->framebuffer[3 * (Y * raw->width + x) + 1] =
          param.intensity * color_corrected[3 * (y * raw->width + x) + 1] *
          inv_scale;
      raw->framebuffer[3 * (Y * raw->width + x) + 2] =
          param.intensity * color_corrected[3 * (y * raw->width + x) + 2] *
          inv_scale;
    }
  }

  t_develop.end();

  gUIParam.develop_msec = t_develop.msec();

  // Upload to GL texture.
  if (gGLCtx.tex_id > 0) {
    glBindTexture(GL_TEXTURE_2D, gGLCtx.tex_id);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, raw->width, raw->height, GL_RGB,
                    GL_FLOAT, raw->framebuffer.data());
    glBindTexture(GL_TEXTURE_2D, 0);
  }
}

void inspect_pixel(int x, int y) {
  if (x >= gRAWImage.width) x = gRAWImage.width - 1;
  if (x < 0) x = 0;
  if (y >= gRAWImage.height) y = gRAWImage.height - 1;
  if (y < 0) y = 0;

  float value = gRAWImage.image.data[y * gRAWImage.width + x];

  gUIParam.inspect_raw_value = value;
  gUIParam.inspect_pos[0] = x;
  gUIParam.inspect_pos[1] = y;
}

void CheckGLError(std::string desc) {
  GLenum e = glGetError();
  if (e != GL_NO_ERROR) {
    fprintf(stderr, "OpenGL error in \"%s\": %d (%d)\n", desc.c_str(), e, e);
    exit(20);
  }
}

bool BindUniform(GLint& loc, GLuint progId, const char* name) {
  loc = glGetUniformLocation(progId, name);
  if (loc < 0) {
    fprintf(stderr, "Cannot find uniform: %s\n", name);
    return false;
  }
  return true;
}

GLuint CreateShader(GLenum shaderType, const char* src) {
  GLuint shader = glCreateShader(shaderType);
  if (!shader) {
    CheckGLError("glCreateShader");
    return 0;
  }
  glShaderSource(shader, 1, &src, NULL);

  GLint compiled = GL_FALSE;
  glCompileShader(shader);
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (!compiled) {
    GLint infoLogLen = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLogLen);
    if (infoLogLen > 0) {
      GLchar* infoLog = (GLchar*)malloc(infoLogLen);
      if (infoLog) {
        glGetShaderInfoLog(shader, infoLogLen, NULL, infoLog);
        fprintf(stderr, "Could not compile %s shader:\n%s\n",
                shaderType == GL_VERTEX_SHADER ? "vertex" : "fragment",
                infoLog);
        free(infoLog);
      }
    }
    glDeleteShader(shader);
    return 0;
  }

  return shader;
}

GLuint CreateProgram(const char* vtxSrc, const char* fragSrc) {
  GLuint vtxShader = 0;
  GLuint fragShader = 0;
  GLuint program = 0;
  GLint linked = GL_FALSE;

  vtxShader = CreateShader(GL_VERTEX_SHADER, vtxSrc);
  if (!vtxShader) goto exit;

  fragShader = CreateShader(GL_FRAGMENT_SHADER, fragSrc);
  if (!fragShader) goto exit;

  program = glCreateProgram();
  if (!program) {
    CheckGLError("glCreateProgram");
    goto exit;
  }
  glAttachShader(program, vtxShader);
  glAttachShader(program, fragShader);

  glLinkProgram(program);
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (!linked) {
    fprintf(stderr, "Could not link program");
    GLint infoLogLen = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &infoLogLen);
    if (infoLogLen) {
      GLchar* infoLog = (GLchar*)malloc(infoLogLen);
      if (infoLog) {
        glGetProgramInfoLog(program, infoLogLen, NULL, infoLog);
        fprintf(stderr, "Could not link program:\n%s\n", infoLog);
        free(infoLog);
      }
    }
    glDeleteProgram(program);
    program = 0;
  }

exit:
  glDeleteShader(vtxShader);
  glDeleteShader(fragShader);
  return program;
}

void InitGLDisplay(GLContext* ctx, int width, int height) {
  ctx->program = CreateProgram(gVertexShaderStr, gFragmentShaderStr);
  if (!ctx->program) {
    fprintf(stderr, "Failed to create shader program.");
    exit(-1);
  }

  // Attrib
  ctx->pos_attrib = glGetAttribLocation(ctx->program, "pos");
  ctx->texcoord_attrib = glGetAttribLocation(ctx->program, "texcoord");

  // uniform
  BindUniform(ctx->gamma_loc, ctx->program, "uGamma");
  BindUniform(ctx->uv_offset_loc, ctx->program, "uOffset");
  BindUniform(ctx->uv_scale_loc, ctx->program, "uScale");
  BindUniform(ctx->intensity_loc, ctx->program, "uIntensity");
  // BindUniform(ctx->texture_size_loc, ctx->program, "uTexsize");

  // Init texture for display.
  {
    glGenTextures(1, &ctx->tex_id);

    glBindTexture(GL_TEXTURE_2D, ctx->tex_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_FLOAT,
                 0);
    CheckGLError("glTexImage2D");

    glBindTexture(GL_TEXTURE_2D, 0);

    if (!BindUniform(ctx->tex_loc, ctx->program, "tex")) {
      fprintf(stderr, "failed to bind texture.");
    }
  }

  // Vertex buffer.
  glGenBuffers(1, &ctx->vb);
  glBindBuffer(GL_ARRAY_BUFFER, ctx->vb);
  glBufferData(GL_ARRAY_BUFFER, sizeof(QUAD), &QUAD[0], GL_STATIC_DRAW);
  CheckGLError("genBuffer");
}

void keyboardCallback(int keycode, int state) {
  printf("hello key %d, state %d(ctrl %d)\n", keycode, state,
         window->isModifierKeyPressed(B3G_CONTROL));
  // if (keycode == 'q' && window && window->isModifierKeyPressed(B3G_SHIFT))
  // {
  if (keycode == 27) {  // ESC
    if (window) window->setRequestExit();
  } else if (keycode == 32) {  // Space
    // @todo { check key is pressed outside of ImGui window. }
    gUIParam.view_offset[0] = 0;
    gUIParam.view_offset[1] = 0;
  }

  ImGui_ImplBtGui_SetKeyState(keycode, (state == 1));

  if (keycode >= 32 && keycode <= 126) {
    if (state == 1) {
      ImGui_ImplBtGui_SetChar(keycode);
    }
  }
}

void mouseMoveCallback(float x, float y) {
  if (gMouseLeftDown) {
    int dx = (int)x - gMousePosX;
    int dy = (int)y - gMousePosY;

    gUIParam.view_offset[0] -= float(dx) / float(gWidth);
    gUIParam.view_offset[1] += float(dy) / float(gHeight);

    std::cout << "view_offt " << gUIParam.view_offset[0] << std::endl;
  }

  int px = clamp(x + gUIParam.view_offset[0], 0, gRAWImage.width - 1);
  int py = clamp(y + gUIParam.view_offset[1], 0, gRAWImage.height - 1);

  inspect_pixel(px, py);

  gMousePosX = (int)x;
  gMousePosY = (int)y;
}

void mouseButtonCallback(int button, int state, float x, float y) {
  ImGui_ImplBtGui_SetMouseButtonState(button, (state == 1));

  ImGuiIO& io = ImGui::GetIO();
  if (io.WantCaptureMouse || io.WantCaptureKeyboard) {
    return;
  }

  // left button
  if (button == 0) {
    if (state) {
      gMouseLeftDown = true;
    } else
      gMouseLeftDown = false;
  }
}

void resizeCallback(float width, float height) {
  GLfloat h = (GLfloat)height / (GLfloat)width;
  GLfloat xmax, znear, zfar;

  znear = 1.0f;
  zfar = 1000.0f;
  xmax = znear * 0.5f;

  gWidth = width;
  gHeight = height;
}

inline float pesudoColor(float v, int ch) {
  if (ch == 0) {  // red
    if (v <= 0.5f)
      return 0.f;
    else if (v < 0.75f)
      return (v - 0.5f) / 0.25f;
    else
      return 1.f;
  } else if (ch == 1) {  // green
    if (v <= 0.25f)
      return v / 0.25f;
    else if (v < 0.75f)
      return 1.f;
    else
      return 1.f - (v - 0.75f) / 0.25f;
  } else if (ch == 2) {  // blue
    if (v <= 0.25f)
      return 1.f;
    else if (v < 0.5f)
      return 1.f - (v - 0.25f) / 0.25f;
    else
      return 0.f;
  } else {  // alpha
    return 1.f;
  }
}

void Display(const GLContext& ctx, const UIParam& param) {
  // glRasterPos2i(pos_x, pos_y);
  // glDrawPixels(width, height, GL_RGB, GL_FLOAT,
  //             static_cast<const GLvoid*>(rgb));

  glUseProgram(ctx.program);
  CheckGLError("use_program");

  glUniform2f(ctx.uv_offset_loc, param.view_offset[0], param.view_offset[1]);
  glUniform1f(ctx.uv_scale_loc, (float)(100.0f) / (float)param.view_scale);
  glUniform1f(ctx.gamma_loc, param.display_gamma);
  glUniform1f(ctx.intensity_loc, param.intensity);
  // glUniform2f(ctx.texture_size_loc, gRAWImage.width, gRAWImage.height);
  CheckGLError("uniform");

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, ctx.tex_id);
  glUniform1i(ctx.tex_loc, 0);  // texture slot 0.

  glBindBuffer(GL_ARRAY_BUFFER, ctx.vb);
  glVertexAttribPointer(ctx.pos_attrib, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                        (const GLvoid*)offsetof(Vertex, pos));
  glVertexAttribPointer(ctx.texcoord_attrib, 2, GL_FLOAT, GL_FALSE,
                        sizeof(Vertex),
                        (const GLvoid*)offsetof(Vertex, texcoord));
  glEnableVertexAttribArray(ctx.pos_attrib);
  glEnableVertexAttribArray(ctx.texcoord_attrib);

  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  CheckGLError("draw");

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindTexture(GL_TEXTURE_2D, 0);
  glDisableVertexAttribArray(ctx.pos_attrib);
  glDisableVertexAttribArray(ctx.texcoord_attrib);
  glUseProgram(0);

  glFlush();
}

int main(int argc, char** argv) {
  std::string input_filename = "../../colorchart.dng";

  if (argc < 2) {
    std::cout << "Needs input.dng" << std::endl;
    // return EXIT_FAILURE;
  } else {
    input_filename = std::string(argv[1]);
  }

  int layer_id = -1;  // -1 = use largest image.
  if (argc > 2) {
    layer_id = atoi(argv[2]);
  }

  {
    std::string warn, err;
    std::vector<tinydng::DNGImage> images;
    std::vector<tinydng::FieldInfo> custom_fields;

    bool ret = tinydng::LoadDNG(input_filename.c_str(), custom_fields, &images,
                                &warn, &err);

    if (!warn.empty()) {
      std::cout << "WARN: " << warn << std::endl;
    }

    if (!err.empty()) {
      std::cout << err << std::endl;
    }

    if (ret == false) {
      std::cout << "failed to load DNG" << std::endl;
      return EXIT_FAILURE;
    }

    assert(images.size() > 0);

    std::cout << "Layers in DNG: " << images.size() << std::endl;

    // print info
    for (size_t i = 0; i < images.size(); i++) {
      std::cout << "image[" << i << "]\n";
      std::cout << "  width : " << images[i].width << "\n";
      std::cout << "  height: " << images[i].height << "\n";
      std::cout << "  bits per sample : " << images[i].bits_per_sample << "\n";
      std::cout << "  bits per sample(in stored image) : " << images[i].bits_per_sample_original << "\n";
      std::cout << "  black_level[0] " << images[i].black_level[0] << std::endl;
      std::cout << "  white_level[1] " << images[i].white_level[0] << std::endl;
    }

    size_t largest = 0;

    {
      if (layer_id < 0) {
        // Find largest image(based on width pixels and bit depth).
        int largest_width = images[0].width;
        int largest_bits = images[0].bits_per_sample;

        for (size_t i = 1; i < images.size(); i++) {
          if (largest_width < images[i].width) {
            largest = i;
            largest_width = images[i].width;
          } else if (largest_width == images[i].width) {
            if (largest_bits < images[i].bits_per_sample) {
              largest = i;
              largest_bits = images[i].bits_per_sample;
            }
          }
        }
      } else {
        largest = layer_id;
        if (largest >= images.size()) {
          largest = images.size() - 1;
        }
      }
    }

    std::cout << "Use layer id = " << largest << std::endl;

    gRAWImage.largest_idx = largest;
    gRAWImage.width = images[largest].width;
    gRAWImage.height = images[largest].height;
    gRAWImage.bits = images[largest].bits_per_sample;
    gRAWImage.components = images[largest].samples_per_pixel;

    // gRAWImage.data = images[largest].data;

    gRAWImage.image = images[largest];

    std::cout << "width " << gRAWImage.width << std::endl;
    std::cout << "height " << gRAWImage.height << std::endl;
    std::cout << "bits " << gRAWImage.bits << std::endl;
    std::cout << "samples_per_pixel " << gRAWImage.components << std::endl;


    // gRAWImage.dng_info = dng_info;

    DecodeToHDR(&gRAWImage, /* endian*/ false);  // @fixme { detect endian }
  }

  // Init UI param
  {
    gUIParam.intensity = 1.0f;
    gUIParam.flip_y = true;
    gUIParam.view_offset[0] = 0;
    gUIParam.view_offset[1] = 0;
    gUIParam.display_gamma = 2.2f;
    gUIParam.view_scale = 100;  // 100%
    gUIParam.cfa_offset[0] = 0;
    gUIParam.cfa_offset[1] = 0;

    assert(gRAWImage.largest_idx >= 0);

    gUIParam.cfa_pattern[0] = gRAWImage.image.cfa_pattern[0][0];
    gUIParam.cfa_pattern[1] = gRAWImage.image.cfa_pattern[0][1];
    gUIParam.cfa_pattern[2] = gRAWImage.image.cfa_pattern[1][0];
    gUIParam.cfa_pattern[3] = gRAWImage.image.cfa_pattern[1][1];

    gUIParam.raw_white_balance[0] = 1.0;
    gUIParam.raw_white_balance[1] = 1.0;
    gUIParam.raw_white_balance[2] = 1.0;

    gUIParam.as_shot_neutral[0] = gRAWImage.image.as_shot_neutral[0];
    gUIParam.as_shot_neutral[1] = gRAWImage.image.as_shot_neutral[1];
    gUIParam.as_shot_neutral[2] = gRAWImage.image.as_shot_neutral[2];

    gUIParam.use_as_shot_neutral =
        (gRAWImage.image.has_as_shot_neutral) ? true : false;

    for (int j = 0; j < 3; j++) {
      for (int i = 0; i < 3; i++) {
        gUIParam.color_matrix[j][i] = gRAWImage.image.color_matrix1[j][i];
      }
    }

    for (int j = 0; j < 3; j++) {
      for (int i = 0; i < 3; i++) {
        gUIParam.inv_color_matrix[j][i] = 0.0f;
      }
    }

    gUIParam.debayer_msec = 0.0;
    gUIParam.develop_msec = 0.0;
    gUIParam.color_correction_msec = 0.0;

    for (int s = 0; s < gRAWImage.image.samples_per_pixel; s++) {
      gUIParam.black_level[s] = gRAWImage.image.black_level[s];
      gUIParam.white_level[s] = gRAWImage.image.white_level[s];
      std::cout << "channel[" << s << "] black level " << gRAWImage.image.black_level[s] << ", white level " << gRAWImage.image.white_level[s] << "\n";
    }

    gUIParam.hue = 0.0;
    gUIParam.saturation = 0.0;
    gUIParam.sepiaAmount = 0.0;
    gUIParam.brightness = 0.0;
    gUIParam.contrast = 0.0;
    gUIParam.vibranceAmount = 0.0;
  }

  window = new b3gDefaultOpenGLWindow;
  b3gWindowConstructionInfo ci;
#ifdef USE_OPENGL2
  ci.m_openglVersion = 2;
#endif
  ci.m_width = gRAWImage.width;
  ci.m_height = gRAWImage.height;

  gWidth = gRAWImage.width;
  gHeight = gRAWImage.height;
  window->createWindow(ci);

  window->setWindowTitle("view");

#ifndef __APPLE__
#ifndef _WIN32
  // some Linux implementations need the 'glewExperimental' to be true
  glewExperimental = GL_TRUE;
#endif
  if (glewInit() != GLEW_OK) {
    fprintf(stderr, "Failed to initialize GLEW\n");
    exit(-1);
  }

  if (!GLEW_VERSION_2_1) {
    fprintf(stderr, "OpenGL 2.1 is not available\n");
    exit(-1);
  }
#endif

  CheckGLError("init");

  InitGLDisplay(&gGLCtx, gRAWImage.width, gRAWImage.height);
  CheckGLError("initDisplay");

  window->setMouseButtonCallback(mouseButtonCallback);
  window->setMouseMoveCallback(mouseMoveCallback);
  window->setKeyboardCallback(keyboardCallback);
  window->setResizeCallback(resizeCallback);

  ImGui_ImplBtGui_Init(window);

  ImGuiIO& io = ImGui::GetIO();
  io.Fonts->AddFontDefault();

  Develop(&gRAWImage, gUIParam);

  while (!window->requestedExit()) {
    window->startRendering();

    CheckGLError("begin frame");

    ImGui_ImplBtGui_NewFrame(gMousePosX, gMousePosY);
    ImGui::Begin("UI");
    {
      ImGui::Text("develop            : %f [msecs]", gUIParam.develop_msec);
      ImGui::Text("  debayer          : %f [msecs]", gUIParam.debayer_msec);
      ImGui::Text("  color correction : %f [msecs]",
                  gUIParam.color_correction_msec);

      for (int s = 0; s < gRAWImage.image.samples_per_pixel; s++) {
        ImGui::Text("Image [%d]", s);
        std::string tag = "  black level##" + std::to_string(s);
        if (ImGui::InputInt(tag.c_str(), &(gUIParam.black_level[s]))) {
          Develop(&gRAWImage, gUIParam);
        }
        tag = "  while level##" + std::to_string(s);
        if (ImGui::InputInt(tag.c_str(), &(gUIParam.white_level[s]))) {
          Develop(&gRAWImage, gUIParam);
        }
      }

      ImGui::Text("(%d x %d) RAW value = : %f", gUIParam.inspect_pos[0],
                  gUIParam.inspect_pos[1], gUIParam.inspect_raw_value);

      if (ImGui::SliderFloat("display gamma", &gUIParam.display_gamma, 0.0f,
                             8.0f)) {
        Develop(&gRAWImage, gUIParam);
      }

      if (ImGui::SliderFloat("intensity", &gUIParam.intensity, 0.0f, 32.0f)) {
        // Develop(&gRAWImage, gUIParam);
      }
      if (ImGui::Checkbox("flip Y", &gUIParam.flip_y)) {
        Develop(&gRAWImage, gUIParam);
      }
      if (ImGui::SliderInt2("CFA offset(xy)", gUIParam.cfa_offset, 0, 1)) {
        Develop(&gRAWImage, gUIParam);
      }
      if (ImGui::SliderInt("zoom(%%)", &gUIParam.view_scale, 1, 1600)) {
        // Develop(&gRAWImage, gUIParam);
      }
      if (ImGui::InputFloat3("color mat 0", gUIParam.color_matrix[0])) {
        Develop(&gRAWImage, gUIParam);
      }
      if (ImGui::InputFloat3("color mat 1", gUIParam.color_matrix[1])) {
        Develop(&gRAWImage, gUIParam);
      }
      if (ImGui::InputFloat3("color mat 2", gUIParam.color_matrix[2])) {
        Develop(&gRAWImage, gUIParam);
      }

      if (ImGui::InputFloat3("inv color mat 0", gUIParam.inv_color_matrix[0])) {
      }
      if (ImGui::InputFloat3("inv color mat 1", gUIParam.inv_color_matrix[1])) {
      }
      if (ImGui::InputFloat3("inv color mat 2", gUIParam.inv_color_matrix[2])) {
      }

      if (ImGui::InputInt4("CFA pattern", gUIParam.cfa_pattern)) {
        Develop(&gRAWImage, gUIParam);
      }
      if (ImGui::SliderFloat3("RAW white balance", gUIParam.raw_white_balance,
                              0.0, 5.0)) {
        Develop(&gRAWImage, gUIParam);
      }
      if (gRAWImage.image.has_as_shot_neutral) {
        if (ImGui::Checkbox("Use as shot neutral",
                            &gUIParam.use_as_shot_neutral)) {
          Develop(&gRAWImage, gUIParam);
        }

        if (ImGui::InputFloat3("As shot neutral", gUIParam.as_shot_neutral)) {
          Develop(&gRAWImage, gUIParam);
        }
      }

      ImGui::Text("Filter");
      if (ImGui::SliderFloat("sepia", &gUIParam.sepiaAmount, 0.0f, 1.0f)) {
        Develop(&gRAWImage, gUIParam);
      }
      if (ImGui::SliderFloat("hue", &gUIParam.hue, -1.0f, 1.0f)) {
        Develop(&gRAWImage, gUIParam);
      }
      if (ImGui::SliderFloat("saturation", &gUIParam.saturation, -1.0f, 1.0f)) {
        Develop(&gRAWImage, gUIParam);
      }
      if (ImGui::SliderFloat("brightness", &gUIParam.brightness, -1.0f, 1.0f)) {
        Develop(&gRAWImage, gUIParam);
      }
      if (ImGui::SliderFloat("contrast", &gUIParam.contrast, -1.0f, 1.0f)) {
        Develop(&gRAWImage, gUIParam);
      }
      if (ImGui::SliderFloat("vibrance", &gUIParam.vibranceAmount, -1.0f,
                             1.0f)) {
        Develop(&gRAWImage, gUIParam);
      }
    }

    ImGui::End();

    // Draw image.
    {
      glViewport(0, 0, window->getWidth(), window->getHeight());
      glClearColor(0, 0.1, 0.2f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
              GL_STENCIL_BUFFER_BIT);

      glMatrixMode(GL_PROJECTION);
      glPushMatrix();
      glLoadIdentity();
      glOrtho(0, window->getWidth(), 0, window->getHeight(), 0, 1);
      glMatrixMode(GL_MODELVIEW);

      CheckGLError("clear");

      Display(gGLCtx, gUIParam);

      glMatrixMode(GL_PROJECTION);
      glPopMatrix();
    }

    // Draw ImGui
    ImGui::Render();

    CheckGLError("im render");

    window->endRendering();

    // Give some cycles to this thread.
    // std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }

  ImGui_ImplBtGui_Shutdown();
  delete window;

  return EXIT_SUCCESS;
}
