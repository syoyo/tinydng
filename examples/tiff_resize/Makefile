CXX=clang++
CXXFLAGS = -fsanitize=address -Weverything -Werror -Wno-variadic-macros -g -O3

all:
	$(CXX) $(CXXFLAGS) -I../dng2exr -o tiff_resize tiff_resize.cc
