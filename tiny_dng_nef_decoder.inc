// NEF decoding code fragment is based on dcraw.

/*
   dcraw.c -- Dave Coffin's raw photo decoder
   Copyright 1997-2016 by Dave Coffin, dcoffin a cybercom o net

   This is a command-line ANSI C program to convert raw photos from
   any digital camera on any computer running any operating system.

   No license is required to download and use dcraw.c.  However,
   to lawfully redistribute dcraw, you must either (a) offer, at
   no extra charge, full source code* for all executable files
   containing RESTRICTED functions, (b) distribute this code under
   the GPL Version 2 or later, (c) remove all RESTRICTED functions,
   re-implement them, or copy them from an earlier, unrestricted
   Revision of dcraw.c, or (d) purchase a license from the author.

   The functions that process Foveon images have been RESTRICTED
   since Revision 1.237.  All other code remains free for all uses.

   *If you have not modified dcraw.c in any way, a link to my
   homepage qualifies as "full source code".

   $Revision: 1.477 $
   $Date: 2016/05/10 21:30:43 $
 */

// from dcraw

#define CLASS
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define LIM(x,min,max) MAX(min,MIN(x,max))

#define RAW(row,col) \
        raw_image[(row)*raw_width+(col)]


#if !defined(uchar)
#define uchar unsigned char
#endif
#if !defined(ushort)
#define ushort unsigned short
#endif



// FIXME(syoyo)
static const uchar *g_input_ptr = NULL;
static int g_input_offset = 0;
static unsigned int zero_after_ff = 0;

// mimics fseek(fp, 0, SEEK_SET)
static void set_stream(const uchar *ptr, int offset)
{
  g_input_ptr = ptr;
  g_input_offset = offset;
}

// mimics fgetc()
static int get_byte() {
  int c = g_input_ptr[g_input_offset];
  g_input_offset++;
  return c;
}

static void CLASS merror (void *ptr, const char *where)
{
  if (ptr) return;
  TINY_DNG_ABORT(where);
}


static void CLASS derror()
{
  TINY_DNG_ABORT("Data error");
}

static unsigned CLASS getbithuff (int nbits, ushort *huff)
{
  static unsigned bitbuf=0;
  static int vbits=0, reset=0;
  unsigned c;

  if (nbits > 25) return 0;
  if (nbits < 0)
    return bitbuf = vbits = reset = 0;
  if (nbits == 0 || vbits < 0) return 0; 
  while (!reset && vbits < nbits && (c = get_byte()) != EOF &&
    !(reset = zero_after_ff && c == 0xff && get_byte())) {
    bitbuf = (bitbuf << 8) + (uchar) c;
    vbits += 8;
  } 
  c = bitbuf << (32-vbits) >> (32-nbits);
  if (huff) {
    vbits -= huff[c] >> 8;
    c = (uchar) huff[c];
  } else
    vbits -= nbits;
  if (vbits < 0) derror();
  return c;
}


#define getbits(n) getbithuff(n,0)
#define gethuff(h) getbithuff(*h,h+1)

/*
   Construct a decode tree according the specification in *source.
   The first 16 bytes specify how many codes should be 1-bit, 2-bit
   3-bit, etc.  Bytes after that are the leaf values.

   For example, if the source is

    { 0,1,4,2,3,1,2,0,0,0,0,0,0,0,0,0,
      0x04,0x03,0x05,0x06,0x02,0x07,0x01,0x08,0x09,0x00,0x0a,0x0b,0xff  },

   then the code is

        00              0x04
        010             0x03
        011             0x05
        100             0x06
        101             0x02
        1100            0x07
        1101            0x01
        11100           0x08
        11101           0x09
        11110           0x00
        111110          0x0a
        1111110         0x0b
        1111111         0xff
 */


static ushort * CLASS make_decoder_ref (const uchar **source)
{
  int max, len, h, i, j;
  const uchar *count;
  ushort *huff;

  count = (*source += 16) - 17;
  for (max=16; max && !count[max]; max--);
  huff = (ushort *) calloc (1 + (1 << max), sizeof *huff);
  merror (huff, "make_decoder()");
  huff[0] = max;
  for (h=len=1; len <= max; len++)
    for (i=0; i < count[len]; i++, ++*source)
      for (j=0; j < 1 << (max-len); j++)
        if (h <= 1 << max)
          huff[h++] = len << 8 | **source;
  return huff;
}

static ushort * CLASS make_decoder (const uchar *source)
{
  return make_decoder_ref (&source);
}

// curve[0x10000]
static int nikon_decode_raw(int tree, int split, int max_v, int min_v, const ushort *curve, ushort vpred[2][2], const uchar *src, int raw_width, int height, ushort *raw_image)
{
  static const unsigned char nikon_tree[][32] = {
    { 0,1,5,1,1,1,1,1,1,2,0,0,0,0,0,0,  /* 12-bit lossy */
      5,4,3,6,2,7,1,0,8,9,11,10,12 },
    { 0,1,5,1,1,1,1,1,1,2,0,0,0,0,0,0,  /* 12-bit lossy after split */
      0x39,0x5a,0x38,0x27,0x16,5,4,3,2,1,0,11,12,12 },
    { 0,1,4,2,3,1,2,0,0,0,0,0,0,0,0,0,  /* 12-bit lossless */
      5,4,6,3,7,2,8,1,9,0,10,11,12 },
    { 0,1,4,3,1,1,1,1,1,2,0,0,0,0,0,0,  /* 14-bit lossy */
      5,6,4,7,8,3,9,2,1,0,10,11,12,13,14 },
    { 0,1,5,1,1,1,1,1,1,1,2,0,0,0,0,0,  /* 14-bit lossy after split */
      8,0x5c,0x4b,0x3a,0x29,7,6,5,4,3,2,1,0,13,14 },
    { 0,1,4,2,2,3,1,2,0,0,0,0,0,0,0,0,  /* 14-bit lossless */
      7,6,8,5,9,4,10,3,11,12,2,0,1,13,14 } };

  int i;
  int row, col;
  int len, shl, diff;
  ushort *huff, hpred[2];
  huff = make_decoder (nikon_tree[tree]);

  //fseek (ifp, data_offset, SEEK_SET);
  // Assume `src` is the beginning of the address of compressed data.
  set_stream(src, 0);

  getbits(-1);
  for (min_v=row=0; row < height; row++) {
    if (split && row == split) {
      free (huff);
      huff = make_decoder (nikon_tree[tree+1]);
      max_v += (min_v = 16) << 1;
    }
    for (col=0; col < raw_width; col++) {
      i = gethuff(huff);
      len = i & 15;
      shl = i >> 4;
      diff = ((getbits(len-shl) << 1) + 1) << shl >> 1;
      if ((diff & (1 << (len-1))) == 0)
        diff -= (1 << len) - !shl;
      if (col < 2) hpred[col] = vpred[row & 1][col] += diff;
      else         hpred[col & 1] += diff;
      if ((ushort)(hpred[col & 1] + min_v) >= max_v) derror();
      RAW(row,col) = curve[LIM((short)hpred[col & 1],0,0x3fff)];
    }
  }
  free (huff);

  return 1; // OK
}
