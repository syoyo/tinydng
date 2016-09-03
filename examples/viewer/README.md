# Simple DNG viewer example with bullet3's OpenGLWindow + ImGui.

## Requirements

* premake5
* OpenGL 2.x

## Build on Linux/MacOSX

    $ premake5 gmake
    $ make

## Build on MinGW(Experimental)

    $ premake5 gmake
    $ make

## Build on Windows

    > premake5 vs2013

Or 

    > premake5 vs2015

## Usage

    $ ./bin/native/Release/view /path/to/file.dng


## Licenses

This example uses following third party libraries.

* ImGui : MIT
* bt3gui : zlib like license
* glew : BSD or MIT
* nativefiledialog : See `nativefiledialog/LICENSE` for details.
