#define PY_SSIZE_T_CLEAN
#ifndef Py_GIL_DISABLED
#define Py_LIMITED_API 0x030A0000
#endif
#include <Python.h>

#include "tinydng.h"

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  PyObject_HEAD
  uint32_t width, height;
  uint16_t bits_per_sample, bits_per_sample_in_file;
  uint16_t samples_per_pixel, compression, sample_format, orientation;
  PyObject *make, *model, *software, *profile_name;
  PyObject *profile_tone_curve, *profile_embed_policy, *noise_profile;
  PyObject *data, *shape, *dtype;
} DNGImageObject;

typedef struct {
  uint32_t width, height;
  uint16_t bits_per_sample, bits_per_sample_in_file;
  uint16_t samples_per_pixel, compression, sample_format, orientation;
  char *make, *model, *software, *profile_name;
  double profile_tone_curve[64];
  uint16_t profile_tone_curve_count;
  double noise_profile[8];
  uint16_t noise_profile_count;
  int profile_embed_policy;
  unsigned char *data;
  size_t data_size;
  const char *dtype;
  size_t element_size;
} LoadedImage;

static char *copy_string(const char *s) {
  size_t n;
  char *copy;
  if (!s) return NULL;
  n = strlen(s);
  copy = (char *)malloc(n + 1u);
  if (copy) memcpy(copy, s, n + 1u);
  return copy;
}

static void loaded_image_clear(LoadedImage *image) {
  if (!image) return;
  free(image->make);
  free(image->model);
  free(image->software);
  free(image->profile_name);
  free(image->data);
  memset(image, 0, sizeof(*image));
}

static void loaded_images_clear(LoadedImage *images, size_t count) {
  size_t i;
  for (i = 0; i < count; ++i) loaded_image_clear(&images[i]);
  free(images);
}

static int load_file(const char *filename, LoadedImage **out_images,
                     size_t *out_count, tinydng_error *error) {
  tinydng_context *ctx = NULL;
  tinydng_document *doc = NULL;
  tinydng_open_options options;
  LoadedImage *images = NULL;
  size_t count = 0, i;
  tinydng_status status;

  memset(&options, 0, sizeof(options));
  options.flags = TINYDNG_OPEN_PARSE_SUBIFDS | TINYDNG_OPEN_PREFER_MMAP;
  ctx = tinydng_context_create(NULL, error);
  if (!ctx) return 0;
  status = tinydng_open_file(ctx, filename, &options, &doc, error);
  if (status != TINYDNG_OK) goto fail;

  count = tinydng_image_count(doc);
  images = (LoadedImage *)calloc(count ? count : 1u, sizeof(*images));
  if (!images) {
    tinydng_error_clear(error);
    error->status = TINYDNG_E_OOM;
    strncpy(error->message, "out of memory allocating image list",
            sizeof(error->message) - 1u);
    goto fail;
  }

  for (i = 0; i < count; ++i) {
    const tinydng_image_info *source = tinydng_image_get(doc, i);
    tinydng_pixels pixels;
    size_t element_count, expected_size;
    LoadedImage *image = &images[i];
    if (!source) {
      error->status = TINYDNG_E_INTERNAL;
      strncpy(error->message, "invalid image index", sizeof(error->message) - 1u);
      goto fail;
    }

    memset(&pixels, 0, sizeof(pixels));
    status = tinydng_decode_image(ctx, doc, i, NULL, &pixels, error);
    if (status != TINYDNG_OK) goto fail;
    image->width = source->width;
    image->height = source->height;
    image->bits_per_sample = pixels.bits_per_sample;
    image->bits_per_sample_in_file = source->bits_per_sample;
    image->samples_per_pixel = pixels.samples_per_pixel;
    image->compression = source->compression;
    image->sample_format = pixels.sample_format;
    image->orientation = source->exif.orientation;
    image->dtype = "uint8";
    image->element_size = 1u;
    if (pixels.sample_format == TINYDNG_SAMPLEFORMAT_IEEEFP &&
        pixels.bits_per_sample == 32) {
      image->dtype = "float32";
      image->element_size = 4u;
    } else if (pixels.sample_format == TINYDNG_SAMPLEFORMAT_IEEEFP &&
               pixels.bits_per_sample == 64) {
      image->dtype = "float64";
      image->element_size = 8u;
    } else if (pixels.bits_per_sample > 8 && pixels.bits_per_sample <= 16) {
      image->dtype = "uint16";
      image->element_size = 2u;
    } else if (pixels.bits_per_sample > 16 && pixels.bits_per_sample <= 32) {
      image->dtype = "uint32";
      image->element_size = 4u;
    } else if (pixels.bits_per_sample > 32) {
      image->dtype = "uint64";
      image->element_size = 8u;
    }
    if ((size_t)image->width > SIZE_MAX / image->height ||
        (size_t)image->width * image->height >
            SIZE_MAX / image->samples_per_pixel ||
        (size_t)image->width * image->height * image->samples_per_pixel >
            SIZE_MAX / image->element_size) {
      tinydng_pixels_free(ctx, &pixels);
      error->status = TINYDNG_E_BOUNDS;
      strncpy(error->message, "image dimensions overflow",
              sizeof(error->message) - 1u);
      goto fail;
    }
    element_count = (size_t)image->width * image->height *
                    image->samples_per_pixel;
    expected_size = element_count * image->element_size;
    if (pixels.size < expected_size) {
      tinydng_pixels_free(ctx, &pixels);
      error->status = TINYDNG_E_DECODE;
      strncpy(error->message, "decoder returned a short pixel buffer",
              sizeof(error->message) - 1u);
      goto fail;
    }
    image->data = (unsigned char *)malloc(expected_size ? expected_size : 1u);
    if (!image->data) {
      tinydng_pixels_free(ctx, &pixels);
      error->status = TINYDNG_E_OOM;
      strncpy(error->message, "out of memory allocating pixels",
              sizeof(error->message) - 1u);
      goto fail;
    }
    memcpy(image->data, pixels.data, expected_size);
    image->data_size = expected_size;
    tinydng_pixels_free(ctx, &pixels);
    image->make = copy_string(source->exif.make);
    image->model = copy_string(source->exif.model);
    image->software = copy_string(source->exif.software);
    image->profile_name = copy_string(source->raw.profile_name);
    image->profile_tone_curve_count = source->raw.profile_tone_curve_count;
    memcpy(image->profile_tone_curve, source->raw.profile_tone_curve,
           sizeof(image->profile_tone_curve));
    image->noise_profile_count = source->raw.noise_profile_count;
    memcpy(image->noise_profile, source->raw.noise_profile,
           sizeof(image->noise_profile));
    image->profile_embed_policy = source->raw.has_profile_embed_policy
                                       ? source->raw.profile_embed_policy
                                       : -1;
  }
  tinydng_document_destroy(ctx, doc);
  tinydng_context_destroy(ctx);
  *out_images = images;
  *out_count = count;
  return 1;

fail:
  if (doc) tinydng_document_destroy(ctx, doc);
  tinydng_context_destroy(ctx);
  loaded_images_clear(images, count);
  return 0;
}

static PyObject *raise_tinydng_error(const char *operation,
                                     const tinydng_error *error) {
  char message[512];
  const char *detail = error->message[0] ? error->message :
                       tinydng_status_string(error->status);
  PyOS_snprintf(message, sizeof(message), "TinyDNG %s: %s", operation, detail);
  PyErr_SetString(PyExc_RuntimeError, message);
  return NULL;
}

static PyObject *image_get_uint16(PyObject *object, void *closure) {
  DNGImageObject *self = (DNGImageObject *)object;
  uint16_t value = *(uint16_t *)((char *)self + (size_t)closure);
  return PyLong_FromUnsignedLong((unsigned long)value);
}

static PyObject *image_get_uint32(PyObject *object, void *closure) {
  DNGImageObject *self = (DNGImageObject *)object;
  uint32_t value = *(uint32_t *)((char *)self + (size_t)closure);
  return PyLong_FromUnsignedLong((unsigned long)value);
}

static PyObject *image_get_object(PyObject *object, void *closure) {
  DNGImageObject *self = (DNGImageObject *)object;
  PyObject *value = *(PyObject **)((char *)self + (size_t)closure);
  if (!value) Py_RETURN_NONE;
  Py_INCREF(value);
  return value;
}

static PyGetSetDef image_getset[] = {
    {"width", image_get_uint32, NULL, NULL, (void *)offsetof(DNGImageObject, width)},
    {"height", image_get_uint32, NULL, NULL, (void *)offsetof(DNGImageObject, height)},
    {"bits_per_sample", image_get_uint16, NULL, NULL, (void *)offsetof(DNGImageObject, bits_per_sample)},
    {"bits_per_sample_in_file", image_get_uint16, NULL, NULL, (void *)offsetof(DNGImageObject, bits_per_sample_in_file)},
    {"samples_per_pixel", image_get_uint16, NULL, NULL, (void *)offsetof(DNGImageObject, samples_per_pixel)},
    {"compression", image_get_uint16, NULL, NULL, (void *)offsetof(DNGImageObject, compression)},
    {"sample_format", image_get_uint16, NULL, NULL, (void *)offsetof(DNGImageObject, sample_format)},
    {"orientation", image_get_uint16, NULL, NULL, (void *)offsetof(DNGImageObject, orientation)},
    {"make", image_get_object, NULL, NULL, (void *)offsetof(DNGImageObject, make)},
    {"model", image_get_object, NULL, NULL, (void *)offsetof(DNGImageObject, model)},
    {"software", image_get_object, NULL, NULL, (void *)offsetof(DNGImageObject, software)},
    {"profile_name", image_get_object, NULL, NULL, (void *)offsetof(DNGImageObject, profile_name)},
    {"profile_tone_curve", image_get_object, NULL, NULL, (void *)offsetof(DNGImageObject, profile_tone_curve)},
    {"profile_embed_policy", image_get_object, NULL, NULL, (void *)offsetof(DNGImageObject, profile_embed_policy)},
    {"noise_profile", image_get_object, NULL, NULL, (void *)offsetof(DNGImageObject, noise_profile)},
    {"data", image_get_object, NULL, NULL, (void *)offsetof(DNGImageObject, data)},
    {"shape", image_get_object, NULL, NULL, (void *)offsetof(DNGImageObject, shape)},
    {"dtype", image_get_object, NULL, NULL, (void *)offsetof(DNGImageObject, dtype)},
    {NULL, NULL, NULL, NULL, NULL}
};

static void image_dealloc(DNGImageObject *self) {
  Py_XDECREF(self->make);
  Py_XDECREF(self->model);
  Py_XDECREF(self->software);
  Py_XDECREF(self->profile_name);
  Py_XDECREF(self->profile_tone_curve);
  Py_XDECREF(self->noise_profile);
  Py_XDECREF(self->data);
  Py_XDECREF(self->shape);
  Py_XDECREF(self->dtype);
  PyObject_Free((void *)self);
}

static PyType_Slot image_slots[] = {
    {Py_tp_dealloc, (void *)image_dealloc},
    {Py_tp_getset, image_getset},
    {0, NULL}
};

static PyType_Spec image_spec = {
    "tinydng_ext.DNGImage", sizeof(DNGImageObject), 0,
    Py_TPFLAGS_DEFAULT, image_slots
};

static PyObject *make_image(PyObject *type, const LoadedImage *source) {
  DNGImageObject *image;
  if (!type) return NULL;
  image = (DNGImageObject *)PyObject_CallNoArgs(type);
  Py_DECREF(type);
  if (!image) return NULL;
  image->width = source->width;
  image->height = source->height;
  image->bits_per_sample = source->bits_per_sample;
  image->bits_per_sample_in_file = source->bits_per_sample_in_file;
  image->samples_per_pixel = source->samples_per_pixel;
  image->compression = source->compression;
  image->sample_format = source->sample_format;
  image->orientation = source->orientation;
  image->make = PyUnicode_FromString(source->make ? source->make : "");
  image->model = PyUnicode_FromString(source->model ? source->model : "");
  image->software = PyUnicode_FromString(source->software ? source->software : "");
  image->profile_name = PyUnicode_FromString(source->profile_name ? source->profile_name : "");
  image->profile_tone_curve = PyList_New(source->profile_tone_curve_count);
  image->noise_profile = PyList_New(source->noise_profile_count);
  image->profile_embed_policy = PyLong_FromLong(source->profile_embed_policy);
  image->data = PyBytes_FromStringAndSize((const char *)source->data,
                                          (Py_ssize_t)source->data_size);
  image->dtype = PyUnicode_FromString(source->dtype);
  image->shape = source->samples_per_pixel > 1
      ? Py_BuildValue("(IIH)", source->height, source->width, source->samples_per_pixel)
      : Py_BuildValue("(II)", source->height, source->width);
  if (!image->make || !image->model || !image->software || !image->profile_name ||
      !image->profile_tone_curve || !image->noise_profile ||
      !image->profile_embed_policy || !image->data || !image->dtype || !image->shape) {
    Py_DECREF((PyObject *)image);
    return NULL;
  }
  {
    uint16_t i;
    for (i = 0; i < source->profile_tone_curve_count; ++i) {
      PyObject *value = PyFloat_FromDouble(source->profile_tone_curve[i]);
      if (!value || PyList_SetItem(image->profile_tone_curve, i, value) < 0) {
        Py_XDECREF(value); Py_DECREF((PyObject *)image); return NULL;
      }
    }
    for (i = 0; i < source->noise_profile_count; ++i) {
      PyObject *value = PyFloat_FromDouble(source->noise_profile[i]);
      if (!value || PyList_SetItem(image->noise_profile, i, value) < 0) {
        Py_XDECREF(value); Py_DECREF((PyObject *)image); return NULL;
      }
    }
  }
  return (PyObject *)image;
}

static PyObject *module_loaddng(PyObject *module, PyObject *args) {
  const char *filename;
  LoadedImage *images = NULL;
  size_t count = 0, i;
  PyObject *result = NULL;
  PyObject *type = NULL;
  tinydng_error error;
  if (!PyArg_ParseTuple(args, "s:loaddng", &filename)) return NULL;
  memset(&error, 0, sizeof(error));
  Py_BEGIN_ALLOW_THREADS
  if (!load_file(filename, &images, &count, &error)) {
    /* The error is copied into `error`; no Python API is called here. */
  }
  Py_END_ALLOW_THREADS
  if (!images && error.status != TINYDNG_OK) {
    return raise_tinydng_error("load", &error);
  }
  result = PyList_New((Py_ssize_t)count);
  if (!result) {
    loaded_images_clear(images, count);
    return NULL;
  }
  type = PyObject_GetAttrString(module, "DNGImage");
  if (!type) {
    loaded_images_clear(images, count);
    Py_DECREF(result);
    return NULL;
  }
  for (i = 0; i < count; ++i) {
    PyObject *image = make_image(type, &images[i]);
    if (!image) {
      loaded_images_clear(images, count);
      Py_DECREF(type);
      Py_DECREF(result);
      return NULL;
    }
    PyList_SetItem(result, (Py_ssize_t)i, image);
  }
  Py_DECREF(type);
  loaded_images_clear(images, count);
  return result;
}

static PyMethodDef module_methods[] = {
    {"loaddng", module_loaddng, METH_VARARGS,
     "Load and decode a DNG file, returning DNGImage objects."},
    {NULL, NULL, 0, NULL}
};

static int module_exec(PyObject *module) {
  PyObject *type = PyType_FromSpec(&image_spec);
  if (!type) return -1;
  if (PyModule_AddObject(module, "DNGImage", type) < 0) {
    Py_DECREF(type);
    return -1;
  }
  return 0;
}

static PyModuleDef_Slot module_slots[] = {
    {Py_mod_exec, module_exec},
#if PY_VERSION_HEX >= 0x030D0000
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
#endif
    {0, NULL}
};

static struct PyModuleDef module_def = {
    PyModuleDef_HEAD_INIT, "tinydng_ext", "Python bindings for TinyDNG v3.",
    0, module_methods, module_slots, NULL, NULL, NULL
};

PyMODINIT_FUNC PyInit_tinydng_ext(void) {
  return PyModuleDef_Init(&module_def);
}
