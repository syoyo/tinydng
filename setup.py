import sysconfig

from setuptools import Extension, setup

dev_mode = False

tinydng_compile_args = []
free_threaded = str(sysconfig.get_config_var("Py_GIL_DISABLED")) == "1"
python_defines = [("Py_GIL_DISABLED", "1")] if free_threaded else []

if dev_mode:
  tinydng_compile_args.append('-O0')
  tinydng_compile_args.append('-g')
  tinydng_compile_args.append('-fsanitize=address')

ext_modules = [
    Extension("tinydng_ext",
        sorted([
            "python/python-bindings.c",
            "tinydng_api.c",
            "tinydng_io.c",
            "tinydng_tiff.c",
            "tinydng_dng.c",
            "tinydng_codec.c",
            "tinydng_write.c",
            "tinydng_psd.c",
            "tinydng_psd_write.c",
            "tinydng_miniz.c",
            "tinydng_stb_image.c",
            "tiny_dng_ljpeg92_v2.c",
        ]),
        include_dirs=['.'],
        define_macros=python_defines,
        py_limited_api=not free_threaded,
        extra_compile_args=tinydng_compile_args
        ),
]

setup(
    name="tinydng",
    package_dir={'': 'python'},
    packages=['tinydng'],
    url="https://github.com/syoyo/tinydng",
    description="Tiny DNG loader/saver",
    long_description=open("./README.md", 'r', encoding='utf8').read(),
    long_description_content_type='text/markdown',
    ext_modules=ext_modules,
    #extras_require={"test": "pytest"},
    ## Currently, build_ext only provides an optional "highest supported C++
    ## level" feature, but in the future it may provide more features.
    ## cmdclass={"build_ext": build_ext},
    #zip_safe=False,
    #python_requires=">=3.6",
)
