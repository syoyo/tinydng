## Requirements

clang with fuzzer support(`-fsanitize=fuzzer`. at least clang 8.0 should work)

## Setup

### Ubuntu 18.04

Add apt source: https://apt.llvm.org/

```
$ sudo apt install clang++-11
$ sudo apt install libfuzzer-11-dev
```

Optionally, if you didn't set `update-alternatives` you can set `clang++` to point to `clang++11`

```
$ sudo update-alternatives --install /usr/bin/clang clang /usr/bin/clang-11 10
$ sudo update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-11 10
```

## How to compile

Edit Makefile if required, then simply:

```
$ make
```

## How to run

Increase memory limit. e.g. `-rss_limit_mb=50000`

```
$ ./fuzzer -rss_limit_mb=20000 -jobs 4
```


## v3 (clean-room C11) fuzzers

The `fuzz-v3*` harnesses target the clean-room parser/writer and are built with
the bundled Makefile targets (clang + `-fsanitize=address,undefined,fuzzer`):

```
$ make v3            # all v3 harnesses
$ make v3-psd        # PSD/PSB reader
$ make v3-psd-write  # PSD writer round-trip (asserts byte-identical read-back)
$ make psd-seeds     # writer-generated PSD/PSB seed corpus -> seeds-psd/
```

Run the PSD reader with its token dictionary and seeds:

```
$ ./fuzz-v3-psd -dict=v3-psd.dict -max_len=65536 seeds-psd
$ ./fuzz-v3-psd-write            # no corpus needed; derives docs from input
```

`fuzz-v3-psd` walks composite decode, every layer (interleaved + per channel),
the thumbnail, and depth-capped smart-object open/decode. `fuzz-v3-psd-write`
writes a doc derived from the input, reopens it, and asserts the decoded
composite and layer bytes match what was written.

## TODO

* [ ] Fuzzer for DNG writer.
