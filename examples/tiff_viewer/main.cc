/*
The MIT License (MIT)

Copyright (c) 2017 Syoyo Fujita

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
#include <windows.h>
#include <mmsystem.h>
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
  int curr_idx;

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

  // Pixel inspection
  float inspect_raw_value;         // RAW censor value
  float inspect_after_debayer[3];  // RAW color value after debayer
  int inspect_pos[2];

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
    "uniform int uFlipY;\n"
    "uniform sampler2D tex;\n"
    "void main() {\n"
    "    vec2 tcoord = vTexcoord;\n"
    "    tcoord.y = (uFlipY > 0) ? (1.0f - tcoord.y) : tcoord.y;\n"
    "    tcoord = (tcoord + uOffset) - vec2(0.5, 0.5);\n"
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
  GLint flip_y_loc;

  GLuint tex_id;
} GLContext;

GLContext gGLCtx;

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

static double gamma_correct(const unsigned char x)
{
  double v = fclamp(std::pow(double(x) / 255.0, 1.0 / 2.2), 0.0, 1.0);
  return v;
}

static double gamma_correct(const unsigned short x)
{
  double v = fclamp(std::pow(double(x) / 65536.0, 1.0 / 2.2), 0.0, 1.0);
  return v;
}

static inline float fetch(const std::vector<float>& in, int x, int y, int w,
                          int h) {
  int xx = clamp(x, 0, w - 1);
  int yy = clamp(y, 0, h - 1);
  return in[yy * w + xx];
}

void Update(RAWImage* raw, const UIParam& param) {

  if (raw->framebuffer.size() != (size_t(raw->width) * size_t(raw->height) * 3)) {
    raw->framebuffer.resize(size_t(raw->width) * size_t(raw->height) * 3);
  }

  std::vector<float> buf(size_t(raw->width) * size_t(raw->height) * 3);
  if (raw->bits == 8) {
    for (size_t i = 0; i < size_t(raw->width) * size_t(raw->height); i++) {
      if (raw->image.samples_per_pixel == 1) {
        buf[3 * i + 0] = float(gamma_correct(raw->image.data[i]));
        buf[3 * i + 1] = float(gamma_correct(raw->image.data[i]));
        buf[3 * i + 2] = float(gamma_correct(raw->image.data[i]));
      } else if (raw->image.samples_per_pixel == 3) {
        buf[3 * i + 0] = float(gamma_correct(raw->image.data[3 * i + 0]));
        buf[3 * i + 1] = float(gamma_correct(raw->image.data[3 * i + 1]));
        buf[3 * i + 2] = float(gamma_correct(raw->image.data[3 * i + 2]));
      } else if (raw->image.samples_per_pixel == 4) {
        buf[3 * i + 0] = float(gamma_correct(raw->image.data[4 * i + 0]));
        buf[3 * i + 1] = float(gamma_correct(raw->image.data[4 * i + 1]));
        buf[3 * i + 2] = float(gamma_correct(raw->image.data[4 * i + 2]));
      } else {
        assert(0);
      }
    }
  } else if (raw->bits == 16) {
    //std::cout << "width " << raw->width << std::endl;
    //std::cout << "width " << raw->height << std::endl;
    //std::cout << "size " << raw->image.data.size() << std::endl;
    const unsigned short *ptr = reinterpret_cast<const unsigned short *>(raw->image.data.data());
    for (size_t i = 0; i < size_t(raw->width * raw->height); i++) {
      if (raw->image.samples_per_pixel == 1) {
        buf[3 * i + 0] = float(gamma_correct(ptr[i]));
        buf[3 * i + 1] = float(gamma_correct(ptr[i]));
        buf[3 * i + 2] = float(gamma_correct(ptr[i]));
      } else if (raw->image.samples_per_pixel == 3) {
        buf[3 * i + 0] = float(gamma_correct(ptr[3 * i + 0]));
        buf[3 * i + 1] = float(gamma_correct(ptr[3 * i + 1]));
        buf[3 * i + 2] = float(gamma_correct(ptr[3 * i + 2]));
      } else if (raw->image.samples_per_pixel == 4) {
        buf[3 * i + 0] = float(gamma_correct(ptr[4 * i + 0]));
        buf[3 * i + 1] = float(gamma_correct(ptr[4 * i + 1]));
        buf[3 * i + 2] = float(gamma_correct(ptr[4 * i + 2]));
      } else {
        assert(0);
      }
    }
  } else if (raw->bits == 32) {
    if (raw->image.sample_format == tinydng::SAMPLEFORMAT_IEEEFP) {
      // No gamma correction.
      const float *ptr = reinterpret_cast<const float *>(raw->image.data.data());
      for (size_t i = 0; i < size_t(raw->width) * size_t(raw->height); i++) {
        if (raw->image.samples_per_pixel == 1) {
          buf[3 * i + 0] = ptr[i];
          buf[3 * i + 1] = ptr[i];
          buf[3 * i + 2] = ptr[i];
        } else if (raw->image.samples_per_pixel == 3) {
          buf[3 * i + 0] = ptr[3 * i + 0];
          buf[3 * i + 1] = ptr[3 * i + 1];
          buf[3 * i + 2] = ptr[3 * i + 2];
        } else if (raw->image.samples_per_pixel == 4) {
          buf[3 * i + 0] = ptr[4 * i + 0];
          buf[3 * i + 1] = ptr[4 * i + 1];
          buf[3 * i + 2] = ptr[4 * i + 2];
        } else {
          assert(0);
        }
      }
    } else {
      std::cerr << "32bit image with INT or UINT format are not supported.\n";
      assert(0); // @TODO
    }
  } else {
    std::cerr << "Unsupported bits " << raw->bits << std::endl;
    assert(0); // @TODO
  }

  // Upload to GL texture.
  if (gGLCtx.tex_id > 0) {
    glBindTexture(GL_TEXTURE_2D, gGLCtx.tex_id);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, raw->width, raw->height, GL_RGB,
                    GL_FLOAT, buf.data());
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
  //BindUniform(ctx->texture_size_loc, ctx->program, "uTexsize");
  BindUniform(ctx->flip_y_loc, ctx->program, "uFlipY");

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
  // if (keycode == 'q' && window && window->isModifierKeyPressed(B3G_SHIFT)) {
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

  glUniform2f(ctx.uv_offset_loc,
              param.view_offset[0],
              param.view_offset[1]);
  glUniform1f(ctx.uv_scale_loc, (float)(100.0f) / (float)param.view_scale);
  glUniform1f(ctx.gamma_loc, param.display_gamma);
  glUniform1f(ctx.intensity_loc, param.intensity);
  //glUniform2f(ctx.texture_size_loc, gRAWImage.width, gRAWImage.height);
  glUniform1i(ctx.flip_y_loc, param.flip_y);
  CheckGLError("uniform set");

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
  std::string input_filename = "../../images/lzw-no-predictor.tiff";

  if (argc < 2) {
    std::cout << "Needs input.tiff" << std::endl;
    // return EXIT_FAILURE;
  } else {
    input_filename = std::string(argv[1]);
  }

  {
    std::string warn, err;
    std::vector<tinydng::DNGImage> images;
    std::vector<tinydng::FieldInfo> custom_fields;

    bool ret =
        tinydng::LoadDNG(input_filename.c_str(), custom_fields, &images, &warn, &err);

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

    // Find largest image(based on width pixels).
    size_t largest = 0;
    int largest_width = images[0].width;
    for (size_t i = 1; i < images.size(); i++) {
      std::cout << largest_width << ", " << images[i].width << std::endl;
      if (largest_width < images[i].width) {
        largest = i;
        largest_width = images[i].width;
      }
    }

    std::cout << "largest = " << largest << std::endl;

    gRAWImage.curr_idx = largest;
    gRAWImage.width = images[largest].width;
    gRAWImage.height = images[largest].height;
    gRAWImage.bits = images[largest].bits_per_sample;
    //gRAWImage.components = images[largest].samples_per_pixel;
    //gRAWImage.data = images[largest].data;

    gRAWImage.image = images[largest];

    std::cout << "width " << gRAWImage.width << std::endl;
    std::cout << "height " << gRAWImage.height << std::endl;
    std::cout << "bits " << gRAWImage.bits << std::endl;
    //std::cout << "components " << gRAWImage.components << std::endl;

    //gRAWImage.dng_info = dng_info;

    //DecodeToHDR(&gRAWImage, /* endian*/ false);  // @fixme { detect endian }
  }

  // Init UI param
  {
    gUIParam.intensity = 1.0f;
    gUIParam.flip_y = true;
    gUIParam.view_offset[0] = 0;
    gUIParam.view_offset[1] = 0;
    gUIParam.display_gamma = 0.454545f;
    gUIParam.view_scale = 100;  // 100%

    assert(gRAWImage.curr_idx >= 0);
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

  Update(&gRAWImage, gUIParam);

  while (!window->requestedExit()) {
    window->startRendering();

    CheckGLError("begin frame");

    ImGui_ImplBtGui_NewFrame(gMousePosX, gMousePosY);
    ImGui::Begin("UI");
    {
      //ImGui::Text("(%d x %d) RAW value = : %f", gUIParam.inspect_pos[0],
      //            gUIParam.inspect_pos[1], gUIParam.inspect_raw_value);

      if (ImGui::SliderFloat("display gamma", &gUIParam.display_gamma, 0.0f,
                             8.0f)) {
        //Develop(&gRAWImage, gUIParam);
      }

      if (ImGui::SliderFloat("intensity", &gUIParam.intensity, 0.0f, 32.0f)) {
        //Develop(&gRAWImage, gUIParam);
      }
      if (ImGui::Checkbox("flip Y", &gUIParam.flip_y)) {
        //Develop(&gRAWImage, gUIParam);
      }
      if (ImGui::SliderInt("zoom(%%)", &gUIParam.view_scale, 1, 1600)) {
        //Develop(&gRAWImage, gUIParam);
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
