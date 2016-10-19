# Simple DNG viewer example with bullet3's OpenGLWindow + ImGui.

## Requirements

* premake5
* OpenGL 2.x

## Building

### Build on Linux/MacOSX

    $ premake5 gmake
    $ make

### Build on Windows

Confirmed build with Visual Studio 2015. Visual Studio 2013 may work.

    > premake5 vs2015

Then, build generated .sln with Visual Studio.

## Usage

    $ ./bin/native/Release/view /path/to/file.dng

## TODO

* [ ] Clamp hight(otherwise Pinkish or Greenish pixel appear in saturated region)
* [ ] Denoise(wavelet denoise, etc)
* [ ] More advanced debayer

## Licenses

This example uses following third party libraries.

* ImGui : MIT
* bt3gui : zlib like license
* glew : BSD or MIT
* nativefiledialog : See `nativefiledialog/LICENSE` for details.
