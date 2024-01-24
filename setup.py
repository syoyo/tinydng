# Adapted from https://github.com/pybind/python_example/blob/master/setup.py
import sys

#from pybind11 import get_cmake_dir
# Available at setup time due to pyproject.toml
from pybind11.setup_helpers import Pybind11Extension#, build_ext
from setuptools import setup

dev_mode = False

tinydng_compile_args = []

if dev_mode:
  tinydng_compile_args.append('-O0')
  tinydng_compile_args.append('-g')
  tinydng_compile_args.append('-fsanitize=address')

ext_modules = [
    Pybind11Extension("tinydng_ext",
        sorted(["python/python-bindings.cc"]),
        include_dirs=['.'],
        cxx_std=11,
        extra_compile_args=tinydng_compile_args
        ),
]

setup(
    name="tinydng",
    packages=['python/tinydng'],
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
