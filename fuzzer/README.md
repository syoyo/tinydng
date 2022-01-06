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


## TODO

* [ ] Fuzzer for DNG writer.
