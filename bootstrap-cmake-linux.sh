curdir=`pwd`

builddir=${curdir}/build_cmake

rm -rf ${builddir}
mkdir ${builddir}

CC=clang CXX=clang++ cmake \
  -B${builddir} \
  -DSANITIZE_ADDRESS=0 \
  -DCMAKE_VERBOSE_MAKEFILE=1 
