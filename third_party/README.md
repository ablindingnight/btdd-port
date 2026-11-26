# third_party
Fetched by CMake (FetchContent), not vendored: SDL3 (zlib license) from M2 on.

## Shader algorithms (M10 upscalers, `shaders/`)
- `mmpx.frag.hlsl`: HLSL port of the MMPX reference rules (Morgan McGuire and Mara Gagiu, "MMPX Style-Preserving
  Pixel-Art Magnification", Journal of Computer Graphics Techniques 10(2), 2021). Reference code licence (MIT):

  > Copyright 2020 Morgan McGuire & Mara Gagiu.
  > Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
  > documentation files (the "Software"), to deal in the Software without restriction, including without limitation
  > the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and
  > to permit persons to whom the Software is furnished to do so, subject to the following conditions:
  > The above copyright notice and this permission notice shall be included in all copies or substantial portions of
  > the Software.
  > THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
  > THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  > AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
  > CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
  > IN THE SOFTWARE.
- `xbr.frag.hlsl`: our own implementation of the xBR edge rule (level 1 with the level-2 interpolation restriction)
  as published by Hyllian (xBR shaders, MIT licence, Copyright (C) 2011-2016 Hyllian). No code copied; credited here.
- `upscale2x.frag.hlsl`: the EPX / Scale2x rule (Eric Johnston, 1992; idea only, own code).
