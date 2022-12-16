# Emscripten experiment of TinyDNG


## Run on browser

Run http server. Recommended way is to use python

```
$ python -m http.server 8888
```

### Display P3 color space

Canvas is created with Display P3 color space support.

https://webkit.org/blog/12058/wide-gamut-2d-graphics-using-html-canvas/

You may see vivid color on Display P3 capable devices(e.g. iPhone7 or later)


## Run on nodejs

```
$ node test.js
```
