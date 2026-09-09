# raylib-particle-toy

A toy bubble chamber particle simulation written in minimal C code with Raylib.
Compiles natively or to WebAssembly.

Dependencies are managed with [pixi](https://pixi.sh); raylib and emsdk are
vendored as git submodules.

## Setup

```sh
git submodule update --init        # raylib + emsdk
pixi install                       # toolchain: gcc, cmake, ninja, python
pixi run setup-web                 # install & activate Emscripten (emsdk) once
```

## Build

```sh
pixi run make build                # native binary: ./bubble_chamber
pixi run make web                  # WebAssembly: bubble_chamber.html/.js/.wasm
pixi run make clean                # remove build trees and outputs
```

Or via pixi tasks: `pixi run build`, `pixi run web`.

To run the web build locally:

```sh
./serve.sh [port]        # default 8000
```

then open `http://localhost:8000/bubble_chamber.html`.

## Notes

- The native build on macOS uses `clang` (conda-forge `gcc` cannot compile
  Apple's block-based system headers); on Linux it uses `gcc`.
- `pixi.toml` is configured for both `osx-arm64` and `linux-64`.  On Linux the
  conda-forge env additionally provides the X11/OpenGL development headers
  (`libgl-devel` ships `<GL/gl.h>`, which the plain `libgl` runtime package does
  not) so raylib's bundled GLFW compiles and the app links.
- `raylib` is compiled from the submodule on first use (CMake + Ninja),
  separately for native and web targets, into `build-native/` and `build-web/`.
- Emscripten is provided by the `emsdk` submodule, not by conda-forge; the
  `setup-web` task installs and activates the latest SDK into `emsdk/upstream/`.
- After updating a submodule, run `pixi run make clean` first.
