//
// TinyDNGWriter, single header only DNG writer.
//

/*
The MIT License (MIT)

Copyright (c) 2016 Syoyo Fujita.

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


namespace tinydngwriter {

typedef enum {
  TIFFTAG_SUBFILETYPE = 254,
  TIFFTAG_IMAGEWIDTH = 256,
  TIFFTAG_IMAGELENGTH = 257,
  TIFFTAG_BITSPERSAMPLE= 258,
  TIFFTAG_COMPRESSION = 259,
  TIFFTAG_PHOTOMETRIC = 262,
	TIFFTAG_SAMPLESPERPIXEL = 277,
	TIFFTAG_ROWSPERSTRIP = 278,
  TIFFTAG_PLANARCONFIG = 284,
  TIFFTAG_ORIENTATION = 274,

  // DNG extension
	TIFFTAG_CFAREPEATPATTERNDIM = 33421,
	TIFFTAG_CFAPATTERN = 33422,

  TIFFTAG_DNGVERSION = 50706,
  TIFFTAG_DNGBACKWARDVERSION = 50707,
	TIFFTAG_CHRROMABLURRADIUS = 50703,
	TIFFTAG_BLACKLEVEL = 50714,
	TIFFTAG_WHITELEVEL = 50717,
	TIFFTAG_COLORMATRIX1 = 50721,
	TIFFTAG_COLORMATRIX2 = 50722,
	TIFFTAG_EXTRACAMERAPROFILES = 50933,
	TIFFTAG_PROFILENAME = 50936,
	TIFFTAG_ASSHOTPROFILENAME = 50934,
	TIFFTAG_DEFAULTBLACKRENDER = 51110,
	TIFFTAG_FORWARDMATRIX1 = 50964,
	TIFFTAG_FORWARDMATRIX2 = 50965
} Tag;

// SUBFILETYPE
static const int FILETYPE_REDUCEDIMAGE = 1;
static const int FILETYPE_PAGE = 2;
static const int FILETYPE_MASK = 4;

// PLANARCONFIG
static const int PLANARCONFIG_CONFIG= 1;
static const int PLANARCONFIG_SEPARATE = 2;

// COMPRESSION
// TODO(syoyo) more compressin types.
static const int COMPRESSION_NONE = 1;

// PHOTOMETRIC
// TODO(syoyo) more photometric types.
static const int PHOTOMETRIC_RGB = 2;
static const int PHOTOMETRIC_CFA = 32893; // DNG ext
static const int PHOTOMETRIC_LINEARRAW = 34892; // DNG ext



class
DNGWriter
{
 public:
  DNGWriter() : is_host_big_endian_(false), swap_endian_(true), num_fields_(0), samples_per_pixels_(0) {}
  ~DNGWriter() {}

  /// Explicitly specify endian swapness.
  bool SwapEndian(bool swap_endian);

  /// Set TIFF field.
  bool SetField(Tag tag, const unsigned char *data);

  /// Write DNG to a file.
  /// Return error string to `err` when Write() returns false.
  /// Returns true upon success.
  bool Write(const char *filename, std::string *err);

 private:
  std::ostringstream ifd_os_;
  std::ostringstream data_os_;
  std::ostringstream err_ss_;
  bool is_host_big_endian_;
  bool swap_endian_;
  unsigned short num_fields_;
  unsigned int samples_per_pixels_;

};

bool WriteDNG(std::ostringstream *out);

}  // namespace tinydng

#ifdef TINY_DNG_WRITER_IMPLEMENTATION

//
// TIFF format resources.
//
// http://c0de517e.blogspot.jp/2013/07/tiny-hdr-writer.html
// http://paulbourke.net/dataformats/tiff/ and
// http://partners.adobe.com/public/developer/en/tiff/TIFF6.pdf
//

#include <cassert>
#include <sstream>
#include <fstream>
#include <iostream>

namespace tinydngwriter {

//
// TinyDNGWriter stores IFD table in the end of file so that offset to
// image data can be easily computed.
//
// +--------------+
// |    header    |
// +--------------+
// |              |
// |  image data  |
// |              |
// +--------------+
// |              |
// |  IFD table   |
// |              |
// +--------------+
//
	

// From tiff.h
typedef enum {
	TIFF_NOTYPE = 0,      /* placeholder */
	TIFF_BYTE = 1,        /* 8-bit unsigned integer */
	TIFF_ASCII = 2,       /* 8-bit bytes w/ last byte null */
	TIFF_SHORT = 3,       /* 16-bit unsigned integer */
	TIFF_LONG = 4,        /* 32-bit unsigned integer */
	TIFF_RATIONAL = 5,    /* 64-bit unsigned fraction */
	TIFF_SBYTE = 6,       /* !8-bit signed integer */
	TIFF_UNDEFINED = 7,   /* !8-bit untyped data */
	TIFF_SSHORT = 8,      /* !16-bit signed integer */
	TIFF_SLONG = 9,       /* !32-bit signed integer */
	TIFF_SRATIONAL = 10,  /* !64-bit signed fraction */
	TIFF_FLOAT = 11,      /* !32-bit IEEE floating point */
	TIFF_DOUBLE = 12,     /* !64-bit IEEE floating point */
	TIFF_IFD = 13,        /* %32-bit unsigned integer (offset) */
	TIFF_LONG8 = 16,      /* BigTIFF 64-bit unsigned integer */
	TIFF_SLONG8 = 17,     /* BigTIFF 64-bit signed integer */
	TIFF_IFD8 = 18        /* BigTIFF 64-bit unsigned integer (offset) */
} DataType;

const static int kHeaderSize = 8; // TIFF header size.

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

static void Write1(const unsigned char c, std::ostringstream *out) 
{
  unsigned char value = c;
  out->write(reinterpret_cast<const char*>(&value), 1); 
}

static void Write2(const unsigned short c, std::ostringstream *out, const bool swap_endian = false) 
{
  unsigned short value = c;
  if (swap_endian) {
		swap2(&value);
  }
    
  out->write(reinterpret_cast<const char*>(&value), 2); 
}

static void Write4(const unsigned int c, std::ostringstream *out, const bool swap_endian = false) 
{
  unsigned int value = c;
  if (swap_endian) {
		swap4(&value);
  }
    
  out->write(reinterpret_cast<const char*>(&value), 4); 
}


static bool WriteTIFFTag(const unsigned short tag, const unsigned short type,
                       const unsigned int count, const unsigned char *data, const bool swap_endian, std::ostringstream *ifd_out, std::ostringstream *data_out) {

  Write2(tag, ifd_out, swap_endian);
  Write2(type, ifd_out, swap_endian);
  Write4(count, ifd_out, swap_endian);

  size_t typesize_table[] = {1, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8, 4};

	size_t len = count * (typesize_table[(type) < 14 ? (type) : 0]);
  if (len > 4) {
		// Store offset value.
		
		unsigned int offset = static_cast<unsigned int>(data_out->tellp()) + kHeaderSize;
    Write4(offset, ifd_out, swap_endian);

		data_out->write(reinterpret_cast<const char*>(data), static_cast<ssize_t>(len));
		
  } else {
		// less than 4 bytes = store data itself.
		
		unsigned int value = *(reinterpret_cast<const unsigned int *>(data));
    Write4(value, ifd_out, swap_endian);
	}

	return true;
}


static bool WriteTIFFHeader(std::ostringstream *out) {

	// TODO(syoyo): Support BigTIFF?
	
	// Use big endian format.
	
  // Header

	// 4d 4d = Big endian. 49 49 = Little endian.
	Write1(0x4d, out);
	Write1(0x4d, out);
  Write1(0x0, out);
  Write1(0x2a, out); // Tiff version ID

  return true;

}

bool DNGWriter::SetField(Tag tag, const unsigned char *data) {

  assert(data);

  unsigned int count = 0;
  DataType type = TIFF_NOTYPE;

  if (tag == TIFFTAG_IMAGEWIDTH) {
    type = TIFF_LONG;
    count = 1;
  } else if (tag == TIFFTAG_IMAGELENGTH) {
    type = TIFF_LONG;
    count = 1;
  } else if (tag == TIFFTAG_ROWSPERSTRIP) {
    type = TIFF_LONG;
    count = 1;
		// NOTE: must be > 0.
  } else if (tag == TIFFTAG_SAMPLESPERPIXEL) {
    type = TIFF_SHORT;
    count = 1;
		samples_per_pixels_ = *(reinterpret_cast<const unsigned short*>(data)); // Save value for later use.
		assert(samples_per_pixels_ <= 4);
  } else if (tag == TIFFTAG_BITSPERSAMPLE) {
    type = TIFF_SHORT;
    count = 1;
  } else if (tag == TIFFTAG_PLANARCONFIG) {
    type = TIFF_SHORT;
    count = 1;
  } else if (tag == TIFFTAG_COMPRESSION) {
    type = TIFF_SHORT;
    count = 1;
  } else if (tag == TIFFTAG_ORIENTATION) {
    type = TIFF_SHORT;
    count = 1;
  } else if (tag == TIFFTAG_PHOTOMETRIC) {
    type = TIFF_SHORT;
    count = 1;
  } else if (tag == TIFFTAG_PROFILENAME) {
    type = TIFF_ASCII;
    count = static_cast<unsigned int>(strlen(reinterpret_cast<const char*>(data)) + 1); // +1 for null
  } else if (tag == TIFFTAG_ASSHOTPROFILENAME) {
    type = TIFF_ASCII;
    count = static_cast<unsigned int>(strlen(reinterpret_cast<const char*>(data)) + 1); // +1 for null
  } else if (tag == TIFFTAG_BLACKLEVEL) {
    type = TIFF_RATIONAL;
    count = samples_per_pixels_;
		assert(0); // TODO
  } else if (tag == TIFFTAG_WHITELEVEL) {
    type = TIFF_RATIONAL;
    count = samples_per_pixels_;
		assert(0); // TODO
	}

  if (type == TIFF_NOTYPE) {
    // Unknown or unsupported tag.
    err_ss_ << "Unknown or unsupported TIFF TAG. Skip writing this TAG : " << tag << std::endl;
    return false;
  }

  WriteTIFFTag(static_cast<unsigned short>(tag), static_cast<unsigned short>(type), count, data, swap_endian_, &ifd_os_, &data_os_);

  num_fields_++;

  return true;
  
}

bool DNGWriter::Write(const char *filename, std::string *err) {

  if ((num_fields_ == 0) || (ifd_os_.tellp() < 12) ) {
    err_ss_ << "No TIFF tag.\n";
    if (err) {
      (*err) = err_ss_.str();
    }
    return false;
  }

  std::ostringstream header;
  bool ret = WriteTIFFHeader(&header);
  if (!ret) {
    return false;
  }

  std::ofstream ofs(filename, std::ostream::binary);

  if (!ofs) {
    err_ss_ << "Failed to open file.\n";
    if (err) {
      (*err) = err_ss_.str();
    }
      
    return false;
  }

	assert(header.str().length() == 4);

	// Write offset to ifd table.
	const unsigned int ifd_offset = kHeaderSize + static_cast<unsigned int>(data_os_.str().length());
	Write4(ifd_offset, &header);

	assert(header.str().length() == 8);

	std::cout << "data_len " << data_os_.str().length() << std::endl;
	std::cout << "ifd_len " << ifd_os_.str().length() << std::endl;

  ofs.write(header.str().c_str(), static_cast<ssize_t>(header.str().length()));
  ofs.write(data_os_.str().c_str(), static_cast<ssize_t>(data_os_.str().length()));

  assert(num_fields_ == (ifd_os_.str().length() / 12));

  // Write the number of IFD entries.
  unsigned short num_fields = num_fields_;
  if (swap_endian_) {
    swap2(&num_fields_);
  } 
  ofs.write(reinterpret_cast<const char*>(&num_fields), 2);

  // Write IFD entries;
  ofs.write(ifd_os_.str().c_str(), static_cast<ssize_t>(ifd_os_.str().length()));

  // Write zero as IFD offset(= end of data)
  unsigned int zero = 0;
  ofs.write(reinterpret_cast<const char*>(&zero), 4);

  err_ss_.clear();
  ifd_os_.clear();
  data_os_.clear();
  num_fields_ = 0;

  return true;
  
}

}  // namespace tinydng

#endif  // TINY_DNG_WRITER_IMPLEMENTATION
