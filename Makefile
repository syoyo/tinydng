CXX=clang++

CXXFLAGS = -O1 -g

# Enable address sanitizer
CXXFLAGS += -fsanitize=address

# Enable ZSTD support
# Please do not forget checkout zstd(or add inc path and lib settings if you use system-installed zstd)
# If you use git submoule'd zstd, recommended way is enter `external/zstd` and do `make` to build libzstd.
CXXFLAGS += -DTINY_DNG_LOADER_ENABLE_ZSTD -I./external/zstd/lib
LDFLAGS += -L./external/zstd/lib -lzstd 

all:
	$(CXX) -o test_loader $(CXXFLAGS) test_loader.cc $(LDFLAGS)
