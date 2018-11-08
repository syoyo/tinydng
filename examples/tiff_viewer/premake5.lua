sources = {
   "main.cc",
   "../viewer/imgui.cpp",
   "../viewer/imgui_draw.cpp",
   "../viewer/imgui_impl_btgui.cpp",
   }

-- premake4.lua
solution "TiffViewerSolution"
   configurations { "Release", "Debug" }

   if os.is("Windows") then
      platforms { "x64", "x32" }
   else
      platforms { "native", "x64", "x32" }
   end


   projectRootDir = os.getcwd() .. "/../viewer/"
   dofile ("../viewer/findOpenGLGlewGlut.lua")
   initOpenGL()
   initGlew()

   -- Use c++11
   flags { "c++11" }

   -- A project defines one build target
   project "viwewer"
      kind "ConsoleApp"
      language "C++"
      files { sources, "../../miniz.c" }

      includedirs { "./", "../../" }
      includedirs { "../viewer/" }

      if os.is("Windows") then
         defines { "NOMINMAX" }
         defines { "USE_NATIVEFILEDIALOG" }
         flags { "FatalCompileWarnings" }
         warnings "Extra" --  /W4
         files{
            "../viewer/OpenGLWindow/Win32OpenGLWindow.cpp",
            "../viewer/OpenGLWindow/Win32OpenGLWindow.h",
            "../viewer/OpenGLWindow/Win32Window.cpp",
            "../viewer/OpenGLWindow/Win32Window.h",
            }
      end
      if os.is("Linux") then
         defines { "TINY_DNG_LOADER_ENABLE_ZIP" }
         buildoptions { "-fsanitize=address" }
         linkoptions { "-fsanitize=address" }
         files {
            "../viewer/OpenGLWindow/X11OpenGLWindow.cpp",
            "../viewer/OpenGLWindow/X11OpenGLWindows.h"
            }
         links {"X11", "pthread", "dl"}
      end
      if os.is("MacOSX") then
         buildoptions { "-fsanitize=address" }
         linkoptions { "-fsanitize=address" }
         links {"Cocoa.framework"}
         files {
                "../viewer/OpenGLWindow/MacOpenGLWindow.h",
                "../viewer/OpenGLWindow/MacOpenGLWindow.mm",
               }
      end

      configuration "Debug"
         defines { "DEBUG" } -- -DDEBUG
         symbols "On"
         targetname "tiff_view_debug"

      configuration "Release"
         -- defines { "NDEBUG" } -- -NDEBUG
	 symbols "On"
	 optimize "On"
         targetname "tiff_view"
