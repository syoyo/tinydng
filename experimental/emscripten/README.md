# Deprecated Emscripten experiment

This directory is retained only as a historical marker. The old C++/Embind
experiment is stale and should not be extended or used for new builds.

Use the maintained decode-only WebAssembly build and browser demo instead:

```bash
emcmake cmake -S web -B web/build -DCMAKE_BUILD_TYPE=Release
cmake --build web/build
python3 -m http.server 8000
```

Open <http://localhost:8000/web/js/> after building. See [web/README.md](../../web/README.md)
for the supported browser decode path.
