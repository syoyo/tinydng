curdir=`pwd`

builddir=${curdir}/build_cmake_with_python

rm -rf ${builddir}
mkdir ${builddir}

# set path to pybind11
# If you install pybind11 through pip, its usually installed to <site-package path>/pybind11.
pybind11_path=`python -c "import site; print (site.getsitepackages()[0])"`
echo ${pybind11_path}

CC=clang CXX=clang++ \
  pybind11_DIR=${pybind11_path}/pybind11 \
  cmake \
  -B${builddir} \
  -DCMAKE_VERBOSE_MAKEFILE=1 \
  -DTINYDNG_WITH_PYTHON=1 \
