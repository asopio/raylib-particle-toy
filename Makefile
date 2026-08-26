# Makefile for Bubble Chamber Simulation
# Supports native (Linux/macOS) and WebAssembly (Emscripten) builds.
#
# raylib is the vendored git submodule (raylib/), compiled on demand with
# CMake + Ninja.  Emscripten comes from the emsdk submodule; run
#
#   pixi run task setup-web
#
# once to install and activate the toolchain into emsdk/upstream.
#
# Then, from a pixi environment:
#   pixi run make build   # native binary
#   pixi run make web     # bubble_chamber.html/.js/.wasm
#   pixi run make clean   # remove build trees and outputs
#
# After updating the raylib or emsdk submodules, run 'make clean' first so
# both raylib builds are regenerated.

# ── toolchain ────────────────────────────────────────────────────────
# conda-forge gcc cannot compile Apple's block-based system headers (it has
# no -fblocks support), so on macOS the native build uses clang.
CC       ?= gcc
ifeq ($(shell uname),Darwin)
    CC       := clang
endif

CMAKE    ?= cmake
NINJA    ?= ninja

CFLAGS   := -Wall -Wextra -O2 -std=c99
WEB_CFLAGS := -Wall -Os -std=c99 -DPLATFORM_WEB

BUILD_NATIVE := build-native
BUILD_WEB    := build-web
RAYLIB_NATIVE := $(BUILD_NATIVE)/raylib
RAYLIB_WEB    := $(BUILD_WEB)/raylib

# Emscripten SDK (emsdk/ submodule), activated into emsdk/upstream
EMSDK_DIR  := $(CURDIR)/emsdk
EMSDK_BIN  := $(EMSDK_DIR)/upstream/emscripten
EMCC       := $(EMSDK_BIN)/emcc
EMCMAKE    := $(EMSDK_BIN)/emcmake

TARGET   := bubble_chamber
WEB_OUT  := bubble_chamber.html

# macOS: raylib's static lib drags in GLFW's Cocoa backend frameworks
ifeq ($(shell uname),Darwin)
    PLATFORM_LIBS := -framework Cocoa -framework IOKit -framework QuartzCore
else
    PLATFORM_LIBS := -lGL -lpthread -ldl -lrt -lX11
endif

EMSDK_CHECK = @[ -x $(EMCC) ] || { echo "error: emsdk toolchain not found -" \
    "run 'pixi run task setup-web' first"; exit 1; }

.PHONY: all build web native clean raylib-native raylib-web

all: build

# ── native build ─────────────────────────────────────────────────────
build: $(TARGET)
native: $(TARGET)

$(TARGET): main.c
	@[ -f $(RAYLIB_NATIVE)/libraylib.a ] || $(MAKE) raylib-native
	$(CC) $(CFLAGS) -o $@ $< -I$(RAYLIB_NATIVE)/include \
	    -L$(RAYLIB_NATIVE) -lraylib -lm $(PLATFORM_LIBS)

raylib-native: $(RAYLIB_NATIVE)/libraylib.a

$(RAYLIB_NATIVE)/libraylib.a:
	$(CMAKE) -B $(BUILD_NATIVE) -S raylib -G Ninja \
	    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=$(CC) \
	    -DBUILD_EXAMPLES=OFF
	$(NINJA) -C $(BUILD_NATIVE)

# ── web / WASM build ─────────────────────────────────────────────────
web: $(WEB_OUT)

$(WEB_OUT): main.c index.html
	$(EMSDK_CHECK)
	@[ -f $(RAYLIB_WEB)/libraylib.a ] || $(MAKE) raylib-web
	PATH="$(EMSDK_BIN):$(PATH)" $(EMCC) $(WEB_CFLAGS) -o $@ $< \
	    -I$(RAYLIB_WEB)/include -L$(RAYLIB_WEB) -lraylib \
	    -s USE_GLFW=3 -s ASYNCIFY --shell-file index.html

raylib-web: $(RAYLIB_WEB)/libraylib.a

$(RAYLIB_WEB)/libraylib.a:
	$(EMSDK_CHECK)
	PATH="$(EMSDK_BIN):$(PATH)" $(EMCMAKE) $(CMAKE) -B $(BUILD_WEB) -S raylib \
	    -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_EXAMPLES=OFF
	PATH="$(EMSDK_BIN):$(PATH)" $(NINJA) -C $(BUILD_WEB)

# ── cleanup ──────────────────────────────────────────────────────────
clean:
	rm -rf $(BUILD_NATIVE) $(BUILD_WEB)
	rm -f $(TARGET) $(WEB_OUT) bubble_chamber.js bubble_chamber.wasm
