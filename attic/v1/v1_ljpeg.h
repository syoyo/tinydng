#ifndef V1_LJPEG_H_
#define V1_LJPEG_H_

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum LJ92_ERRORS {
  LJ92_ERROR_NONE = 0,
  LJ92_ERROR_CORRUPT = -1,
  LJ92_ERROR_NO_MEMORY = -2,
  LJ92_ERROR_BAD_HANDLE = -3,
  LJ92_ERROR_TOO_WIDE = -4
};

typedef struct _ljp* lj92;

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

#define LJ92_MAX_COMPONENTS (16)

typedef struct _ljp {
  u8* data;
  u8* dataend;
  int datalen;
  int scanstart;
  int ix;
  int x;           // Width
  int y;           // Height
  int bits;        // Bit depth
  int components;  // Components(Nf)
  int writelen;    // Write rows this long
  int skiplen;     // Skip this many values after each row
  u16* linearize;  // Linearization table
  int linlen;
  int sssshist[16];
  u16* hufflut[LJ92_MAX_COMPONENTS];
  int huffbits[LJ92_MAX_COMPONENTS];
  int num_huff_idx;
  int cnt;
  u32 b;
  u16* image;
  u16* rowcache;
  u16* outrow[2];
} ljp;

static int find(ljp* self) {
  int ix = self->ix;
  u8* data = self->data;
  while (data[ix] != 0xFF && ix < (self->datalen - 1)) {
    ix += 1;
  }
  ix += 2;
  if (ix >= self->datalen) return -1;
  self->ix = ix;
  return data[ix - 1];
}

#define BEH(ptr) ((((int)(*&ptr)) << 8) | (*(&ptr + 1)))

static int parseHuff(ljp* self) {
  int ret = LJ92_ERROR_CORRUPT;
  u8* huffhead = &self->data[self->ix];
  u8* bits = &huffhead[2];
  // bits[0] = 0; // Don't modify input data if possible
  int hufflen = BEH(huffhead[0]);
  if ((self->ix + hufflen) >= self->datalen) return ret;
  u8* huffvals = &self->data[self->ix + 19];
  int maxbits = 16;
  while (maxbits > 0) {
    if (bits[maxbits]) break;
    maxbits--;
  }
  self->huffbits[self->num_huff_idx] = maxbits;
  u16* hufflut = (u16*)malloc((1 << maxbits) * sizeof(u16));
  if (hufflut == NULL) return LJ92_ERROR_NO_MEMORY;
  self->hufflut[self->num_huff_idx] = hufflut;
  int i = 0, hv = 0, rv = 0, vl = 0, hcode, bitsused = 1;
  while (i < 1 << maxbits) {
    if (bitsused > maxbits) break;
    if (vl >= bits[bitsused]) { bitsused++; vl = 0; continue; }
    if (rv == 1 << (maxbits - bitsused)) { rv = 0; vl++; hv++; continue; }
    hcode = huffvals[hv];
    hufflut[i] = hcode << 8 | bitsused;
    i++; rv++;
  }
  self->num_huff_idx++;
  return LJ92_ERROR_NONE;
}

static int parseSof3(ljp* self) {
  if (self->ix + 6 >= self->datalen) return LJ92_ERROR_CORRUPT;
  self->y = BEH(self->data[self->ix + 3]);
  self->x = BEH(self->data[self->ix + 5]);
  self->bits = self->data[self->ix + 2];
  self->components = self->data[self->ix + 7];
  self->ix += BEH(self->data[self->ix]);
  if ((self->components >= 1) && (self->components < 6)) return LJ92_ERROR_NONE;
  return LJ92_ERROR_CORRUPT;
}

static int parseBlock(ljp* self, int marker) {
  (void)marker;
  self->ix += BEH(self->data[self->ix]);
  if (self->ix >= self->datalen) return LJ92_ERROR_CORRUPT;
  return LJ92_ERROR_NONE;
}

static inline int nextdiff(ljp* self, int component_idx, int Px, int *errcode) {
  (void)Px;
  if (component_idx >= self->num_huff_idx) { if (errcode) *errcode = LJ92_ERROR_CORRUPT; return 0; }
  u32 b = self->b;
  int cnt = self->cnt;
  int huffbits = self->huffbits[component_idx];
  int ix = self->ix;
  while (cnt < huffbits) {
    if ((ix + 1) >= self->datalen) { if (errcode) *errcode = LJ92_ERROR_CORRUPT; return 0; }
    int one = self->data[ix];
    int two = self->data[ix + 1];
    b = (b << 16) | (one << 8) | two;
    cnt += 16; ix += 2;
    if (one == 0xFF) { b >>= 8; cnt -= 8; } else if (two == 0xFF) ix++;
  }
  int index = b >> (cnt - huffbits);
  u16 ssssused = self->hufflut[component_idx][index];
  int usedbits = ssssused & 0xFF;
  int t = ssssused >> 8;
  cnt -= usedbits;
  u32 keepbitsmask = (1u << cnt) - 1;
  b &= keepbitsmask;
  while (cnt < t) {
    if ((ix + 1) >= self->datalen) { if (errcode) *errcode = LJ92_ERROR_CORRUPT; return 0; }
    int one = self->data[ix];
    int two = self->data[ix + 1];
    b = (b << 16) | (one << 8) | two;
    cnt += 16; ix += 2;
    if (one == 0xFF) { b >>= 8; cnt -= 8; } else if (two == 0xFF) ix++;
  }
  cnt -= t;
  int diff = b >> cnt;
  int vt = 1 << (t - 1);
  if (diff < vt) { vt = (-1 << t) + 1; diff += vt; }
  keepbitsmask = (1u << cnt) - 1;
  self->b = b & keepbitsmask;
  self->cnt = cnt;
  self->ix = ix;
  return diff;
}

#define TINY_DNG_CHECK_AND_RETURN_C(cond, retcode) do { if (!(cond)) return (retcode); } while (0)

static int parseScan(ljp* self) {
  self->ix = self->scanstart;
  int compcount = self->data[self->ix + 2];
  int pred = self->data[self->ix + 3 + 2 * compcount];
  if (pred < 0 || pred > 7) return LJ92_ERROR_CORRUPT;
  self->ix += BEH(self->data[self->ix]);
  self->cnt = 0; self->b = 0;
  u16* out = self->image;
  u16* thisrow = self->outrow[0];
  u16* lastrow = self->outrow[1];
  int Px = 0, left = 0;
  for (int row = 0; row < self->y; row++) {
    for (int col = 0; col < self->x; col++) {
      int colx = col * self->components;
      for (int c = 0; c < self->components; c++) {
        if ((col == 0) && (row == 0)) Px = 1 << (self->bits - 1);
        else if (row == 0) Px = thisrow[(col - 1) * self->components + c];
        else if (col == 0) Px = lastrow[c];
        else {
          int prev_colx = (col - 1) * self->components;
          left = thisrow[prev_colx + c];
          switch (pred) {
            case 0: Px = 0; break;
            case 1: Px = thisrow[prev_colx + c]; break;
            case 2: Px = lastrow[colx + c]; break;
            case 3: Px = lastrow[prev_colx + c]; break;
            case 4: Px = left + lastrow[colx + c] - lastrow[prev_colx + c]; break;
            case 5: Px = left + ((lastrow[colx + c] - lastrow[prev_colx + c]) >> 1); break;
            case 6: Px = lastrow[colx + c] + ((left - lastrow[prev_colx + c]) >> 1); break;
            case 7: Px = (left + lastrow[colx + c]) >> 1; break;
          }
        }
        int huff_idx = c;
        if (c >= self->num_huff_idx) {
          TINY_DNG_CHECK_AND_RETURN_C(self->num_huff_idx == 1, LJ92_ERROR_CORRUPT);
          huff_idx = 0;
        }
        int errcode = LJ92_ERROR_NONE;
        int diff = nextdiff(self, huff_idx, Px, &errcode);
        if (errcode != LJ92_ERROR_NONE) return errcode;
        left = (u16)((Px + diff) % 65536);
        thisrow[colx + c] = (u16)left;
        if (self->linearize) out[colx + c] = self->linearize[(u16)left];
        else out[colx + c] = (u16)left;
      }
    }
    u16* temprow = lastrow; lastrow = thisrow; thisrow = temprow;
    out += self->x * self->components + self->skiplen;
  }
  return LJ92_ERROR_NONE;
}

static int findSoI(ljp* self) {
  if (find(self) == 0xd8) {
    int ret = LJ92_ERROR_NONE;
    while (1) {
      int nextMarker = find(self);
      if (nextMarker == 0xc4) ret = parseHuff(self);
      else if (nextMarker == 0xc3) ret = parseSof3(self);
      else if (nextMarker == 0xda) { self->scanstart = self->ix; ret = LJ92_ERROR_NONE; break; }
      else if (nextMarker == -1) { ret = LJ92_ERROR_CORRUPT; break; }
      else ret = parseBlock(self, nextMarker);
      if (ret != LJ92_ERROR_NONE) break;
    }
    return ret;
  }
  return LJ92_ERROR_CORRUPT;
}

static void free_memory(ljp* self) {
  for (int i = 0; i < self->num_huff_idx; i++) free(self->hufflut[i]);
  free(self->rowcache);
}

int v1_lj92_open(lj92* lj, const uint8_t* data, int datalen, int* width, int* height, int* bitdepth, int* components) {
  ljp* self = (ljp*)calloc(sizeof(ljp), 1);
  if (self == NULL) return LJ92_ERROR_NO_MEMORY;
  self->data = (u8*)data; self->datalen = datalen;
  int ret = findSoI(self);
  if (ret == LJ92_ERROR_NONE) {
    u16* rowcache = (u16*)calloc(self->x * self->components * 2, sizeof(u16));
    if (rowcache == NULL) ret = LJ92_ERROR_NO_MEMORY;
    else {
      self->rowcache = rowcache;
      self->outrow[0] = rowcache;
      self->outrow[1] = &rowcache[self->x * self->components];
    }
  }
  if (ret != LJ92_ERROR_NONE) { *lj = NULL; free_memory(self); free(self); }
  else { *width = self->x; *height = self->y; *bitdepth = self->bits; *components = self->components; *lj = self; }
  return ret;
}

int v1_lj92_decode(lj92 lj, uint16_t* target, int writeLength, int skipLength, uint16_t* linearize, int linearizeLength) {
  ljp* self = lj;
  self->image = target; self->writelen = writeLength; self->skiplen = skipLength;
  self->linearize = linearize; self->linlen = linearizeLength;
  return parseScan(self);
}

void v1_lj92_close(lj92 lj) {
  if (lj) { free_memory(lj); free(lj); }
}

#endif
