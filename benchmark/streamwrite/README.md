# Streaming encode benchmark (tiled lossless-JPEG DNG writer)

Measures end-to-end streaming-encode throughput: the source image (largest
>8-bit lossless-JPEG image of a DNG) is decoded once, then re-encoded every
iteration.

```bash
make
./bench <lossless_16bit_dng> [iterations]
```

Reports:

- `stream writer (tiled LJPEG DNG)` — full `tinydng_writer_*` pipeline
  (512x512 tiles, per-tile streaming LJPEG straight to a memory sink, IFD
  assembly) in MPix/s and bytes per frame.
- `codec streaming encode` / `codec one-shot encode` — the
  `tdng_lj92_encode_*` streaming API vs `tdng_lj92_encode_ex` (both to
  buffers), per 512x512 tile.

Reference results (AMD Ryzen Threadripper 1950X, gcc -O2):

| workload | stream writer | codec streaming | codec one-shot |
|----------|--------------:|----------------:|---------------:|
| proraw-48mp-02.dng 8064x6048x3 b16 (48.8 MPix) | 25.1 MPix/s, 18.4 MB/frame | 24.7 MPix/s | 25.5 MPix/s |
| pixel3.dng SubIFD 4032x3024x1 b16 (12.2 MPix)  | 68.6 MPix/s, 1.5 MB/frame | 66.5 MPix/s | 69.7 MPix/s |

The streaming encoder tracks the one-shot path within noise (the 4KB staged
sink adds no measurable overhead); the writer adds per-tile IFD/segment
bookkeeping on top.
