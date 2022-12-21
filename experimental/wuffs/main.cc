#include <vector>
#include <cstdint>
#include <iostream>

#include "wuffs-v0.3.c"

static void swap2(unsigned short* val) {
  unsigned short tmp = *val;
  unsigned char* dst = reinterpret_cast<unsigned char*>(val);
  unsigned char* src = reinterpret_cast<unsigned char*>(&tmp);

  dst[0] = src[1];
  dst[1] = src[0];
}

static void swap4(unsigned int* val) {
  unsigned int tmp = *val;
  unsigned char* dst = reinterpret_cast<unsigned char*>(val);
  unsigned char* src = reinterpret_cast<unsigned char*>(&tmp);

  dst[0] = src[3];
  dst[1] = src[2];
  dst[2] = src[1];
  dst[3] = src[0];
}

static inline void swap4(int* val) {
  int tmp = *val;
  unsigned char* dst = reinterpret_cast<unsigned char*>(val);
  unsigned char* src = reinterpret_cast<unsigned char*>(&tmp);

  dst[0] = src[3];
  dst[1] = src[2];
  dst[2] = src[1];
  dst[3] = src[0];
}

static inline void swap8(uint64_t* val) {
  uint64_t tmp = (*val);
  unsigned char* dst = reinterpret_cast<unsigned char*>(val);
  unsigned char* src = reinterpret_cast<unsigned char*>(&tmp);

  dst[0] = src[7];
  dst[1] = src[6];
  dst[2] = src[5];
  dst[3] = src[4];
  dst[4] = src[3];
  dst[5] = src[2];
  dst[6] = src[1];
  dst[7] = src[0];
}

static inline void swap8(int64_t* val) {
  int64_t tmp = (*val);
  unsigned char* dst = reinterpret_cast<unsigned char*>(val);
  unsigned char* src = reinterpret_cast<unsigned char*>(&tmp);

  dst[0] = src[7];
  dst[1] = src[6];
  dst[2] = src[5];
  dst[3] = src[4];
  dst[4] = src[3];
  dst[5] = src[2];
  dst[6] = src[1];
  dst[7] = src[0];
}

static void cpy2(unsigned short* dst_val, const unsigned short* src_val) {
  unsigned char* dst = reinterpret_cast<unsigned char*>(dst_val);
  const unsigned char* src = reinterpret_cast<const unsigned char*>(src_val);

  dst[0] = src[0];
  dst[1] = src[1];
}

static void cpy2(short* dst_val, const short* src_val) {
  unsigned char* dst = reinterpret_cast<unsigned char*>(dst_val);
  const unsigned char* src = reinterpret_cast<const unsigned char*>(src_val);

  dst[0] = src[0];
  dst[1] = src[1];
}

static void cpy4(unsigned int* dst_val, const unsigned int* src_val) {
  unsigned char* dst = reinterpret_cast<unsigned char*>(dst_val);
  const unsigned char* src = reinterpret_cast<const unsigned char*>(src_val);

  dst[0] = src[0];
  dst[1] = src[1];
  dst[2] = src[2];
  dst[3] = src[3];
}

static void cpy4(int* dst_val, const int* src_val) {
  unsigned char* dst = reinterpret_cast<unsigned char*>(dst_val);
  const unsigned char* src = reinterpret_cast<const unsigned char*>(src_val);

  dst[0] = src[0];
  dst[1] = src[1];
  dst[2] = src[2];
  dst[3] = src[3];
}

static void cpy8(uint64_t* dst_val, const uint64_t* src_val) {
  unsigned char* dst = reinterpret_cast<unsigned char*>(dst_val);
  const unsigned char* src = reinterpret_cast<const unsigned char*>(src_val);

  dst[0] = src[0];
  dst[1] = src[1];
  dst[2] = src[2];
  dst[3] = src[3];
  dst[4] = src[4];
  dst[5] = src[5];
  dst[6] = src[6];
  dst[7] = src[7];
}

static void cpy8(int64_t* dst_val, const int64_t* src_val) {
  unsigned char* dst = reinterpret_cast<unsigned char*>(dst_val);
  const unsigned char* src = reinterpret_cast<const unsigned char*>(src_val);

  dst[0] = src[0];
  dst[1] = src[1];
  dst[2] = src[2];
  dst[3] = src[3];
  dst[4] = src[4];
  dst[5] = src[5];
  dst[6] = src[6];
  dst[7] = src[7];
}

///
/// Simple stream reader
///
class StreamReader {
 public:
  explicit StreamReader(const uint8_t* binary, const size_t length,
                        const bool swap_endian)
      : binary_(binary), length_(length), swap_endian_(swap_endian), idx_(0) {
    (void)pad_;
  }

  bool seek_set(const uint64_t offset) const {
    if (offset > length_) {
      return false;
    }

    idx_ = offset;
    return true;
  }

  bool seek_from_currect(const int64_t offset) const {
    if ((int64_t(idx_) + offset) < 0) {
      return false;
    }

    if (size_t((int64_t(idx_) + offset)) > length_) {
      return false;
    }

    idx_ = size_t(int64_t(idx_) + offset);
    return true;
  }

  size_t read(const size_t n, const uint64_t dst_len,
              unsigned char* dst) const {
    size_t len = n;
    if ((idx_ + len) > length_) {
      len = length_ - idx_;
    }

    if (len > 0) {
      if (dst_len < len) {
        // dst does not have enough space. return 0 for a while.
        return 0;
      }

      memcpy(dst, &binary_[idx_], len);
      idx_ += len;
      return len;

    } else {
      return 0;
    }
  }

  bool read1(unsigned char* ret) const {
    if ((idx_ + 1) > length_) {
      return false;
    }

    const unsigned char val = binary_[idx_];

    (*ret) = val;
    idx_ += 1;

    return true;
  }

  bool read1(char* ret) const {
    if ((idx_ + 1) > length_) {
      return false;
    }

    const char val = static_cast<const char>(binary_[idx_]);

    (*ret) = val;
    idx_ += 1;

    return true;
  }

  bool read2(unsigned short* ret) const {
    if ((idx_ + 2) > length_) {
      return false;
    }

    unsigned short val = 0;
    cpy2(&val, reinterpret_cast<const unsigned short*>(&binary_[idx_]));

    if (swap_endian_) {
      swap2(&val);
    }

    (*ret) = val;
    idx_ += 2;

    return true;
  }

  bool read2(short* ret) const {
    if ((idx_ + 2) > length_) {
      return false;
    }

    short val = 0;
    cpy2(&val, reinterpret_cast<const short*>(&binary_[idx_]));

    if (swap_endian_) {
      swap2(reinterpret_cast<unsigned short*>(&val));
    }

    (*ret) = val;
    idx_ += 2;

    return true;
  }

  bool read4(unsigned int* ret) const {
    if ((idx_ + 4) > length_) {
      return false;
    }

    unsigned int val = 0;
    cpy4(&val, reinterpret_cast<const unsigned int*>(&binary_[idx_]));

    if (swap_endian_) {
      swap4(&val);
    }

    (*ret) = val;
    idx_ += 4;

    return true;
  }

  bool read4(int* ret) const {
    if ((idx_ + 4) > length_) {
      return false;
    }

    int val = 0;
    cpy4(&val, reinterpret_cast<const int*>(&binary_[idx_]));

    if (swap_endian_) {
      swap4(&val);
    }

    (*ret) = val;
    idx_ += 4;

    return true;
  }

  bool read8(uint64_t* ret) const {
    if ((idx_ + 8) > length_) {
      return false;
    }

    uint64_t val = 0;
    cpy8(&val, reinterpret_cast<const uint64_t*>(&binary_[idx_]));

    if (swap_endian_) {
      swap8(&val);
    }

    (*ret) = val;
    idx_ += 8;

    return true;
  }

  bool read8(int64_t* ret) const {
    if ((idx_ + 8) > length_) {
      return false;
    }

    int64_t val = 0;
    cpy8(&val, reinterpret_cast<const int64_t*>(&binary_[idx_]));

    if (swap_endian_) {
      swap8(&val);
    }

    (*ret) = val;
    idx_ += 8;

    return true;
  }

  //
  // Returns a memory address. The begining of address is computed in
  // relative(based on current seek pos). This function is useful when you just
  // want to access the content in read-only mode.
  //
  // Note that the function does not change seek position after the call.
  // This function does the bound check.
  //
  // @param[in] offset Extra byte offset. 0 = use current seek position.
  // @param[in] length Byte length to map.
  //
  // @return nullptr when failed to map address.
  //
  const uint8_t* map_addr(size_t offset, const size_t length) {
    if (length == 0) {
      return NULL;
    }

    if ((idx_ + offset) > length_) {
      return NULL;
    }

    if ((idx_ + offset + length) > length_) {
      return NULL;
    }

    return &binary_[idx_ + offset];
  }

  //
  // Returns a memory address. The begining of address is specified by
  // absolute(ignores current seek pos). This function is useful when you just
  // want to access the content in read-only mode.
  //
  // Note that the function does not change seek position after the call.
  // This function does the bound check.
  //
  // @param[in] pos Absolute position in bytes.
  // @param[in] length Byte length to map.
  //
  // @return nullptr when failed to map address.
  //
  const uint8_t* map_abs_addr(size_t pos, const size_t length) {
    if (length == 0) {
      return NULL;
    }

    if (pos > length_) {
      return NULL;
    }

    if ((pos + length) > length_) {
      return NULL;
    }

    return &binary_[pos];
  }

  size_t tell() const { return idx_; }

  const uint8_t* data() const { return binary_; }

  bool swap_endian() const { return swap_endian_; }

  size_t size() const { return length_; }

 private:
  const uint8_t* binary_;
  const size_t length_;
  bool swap_endian_;
  char pad_[7];
  mutable uint64_t idx_;
};

// Based on lj9.2c
/*
lj92.c
(c) Andrew Baldwin 2014

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

constexpr size_t kMaxComponents = 16;

struct LosslessJpeg {
  size_t datalen;
  int scanstart;
  int ix;
  int x;           // Width
  int y;           // Height
  int bits;        // Bit depth
  int components;  // Components(Nf)
  int writelen;    // Write rows this long
  int skiplen;     // Skip this many values after each row
  std::vector<uint16_t> linearize;  // Linearization table
  int linlen;
  int sssshist[16];

  std::array<std::vector<uint16_t>, kMaxComponents> huffman_luts;
  std::array<int, kMaxComponents> huffman_bits;
  int num_huffmans; // <= kMaxComponents
};

enum class LJ92Error
{
  Success,
  Corrupt,
  NoMemory,
  BadHandle,
  TooLarge,
};

class LosslessJpegDecoder
{
 public:

  bool open(const uint8_t *data, const size_t data_length, bool swap_endian=true);

  // valid after 'open'
  int width();
  int height();
  int bitdepth();


  ///
  /// Write data with `dest_length` bytes, then skip `skip_length` bytes, then continue writing data.
  ///
  /// @param[in] dest_length
  /// @param[in] skip_length Skip bytes
  /// @param[in] linearize Optional. Linearlize table
  ///
  LJ92Error decode(uint16_t *dest, size_t dest_length, size_t skip_length, const std::vector<uint16_t> *linearize = nullptr);


 private:
  // Find SoI(Start of Image) marker.
  LJ92Error find_SoI();

  int find_marker();

  LosslessJpeg _lj;
  StreamReader *_sr{nullptr};
};

LJ92Error LosslessJpegDecoder::find_SoI()
{
  // SoI marker = 0xd8
  // TODO
  return LJ92Error::Corrupt;
}

bool LosslessJpegDecoder::open(const uint8_t *data, const size_t data_length, bool swap_endian)
{
  if (_sr) {
    delete _sr;
  }

  _sr = new StreamReader(data, data_length, swap_endian);

  // Check header.


  return true;
}

int main(int argc, char **argv)
{
  LosslessJpegDecoder decoder;
  std::vector<uint8_t> data;

  if (!decoder.open(data.data(), data.size())) {
    std::cerr << "Failed to open LosslessJpeg data.\n";
  }

  return 0;
}
