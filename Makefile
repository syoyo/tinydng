# for clang
CXX=clang++
CXXFLAGS=-g -O2 -Weverything -Werror -Wno-c++98-compat
EXTRA_CXXFLAGS=-fsanitize=address

# for gcc
#CXX=g++
#CXXFLAGS=-g -O2

all:
	$(CXX) $(CXXFLAGS) $(EXTRA_CXXFLAGS) -o test_loader test_loader.cc
