# Makefile for Bubble Chamber Simulation
# Supports native (Linux/macOS) and WebAssembly (Emscripten) builds.

# ── native build ────────────────────────────────────────────────────
CC       ?= gcc
CFLAGS   := -Wall -Wextra -O2 -std=c99
LDFLAGS  := -lraylib -lm

ifeq ($(shell uname),Linux)
    LDFLAGS += -lGL -lpthread -ldl -lrt -lX11
endif

TARGET   := bubble_chamber

.PHONY: all clean web

all: $(TARGET)

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# ── web / WASM build ───────────────────────────────────────────────
# Requires Emscripten SDK and a raylib WASM build.
#   export EMSDK=<path>
#   export RAYLIB_WEB=<path-to-raylib-web-install>
# Then: make web

EMCC         ?= emcc
RAYLIB_WEB   ?= $(HOME)/raylib-web
WEB_CFLAGS   := -Wall -Os -std=c99 -DPLATFORM_WEB
WEB_CFLAGS   += -I$(RAYLIB_WEB)/include
WEB_LDFLAGS  := -L$(RAYLIB_WEB)/lib -lraylib
WEB_LDFLAGS  += -s USE_GLFW=3 -s ASYNCIFY
WEB_LDFLAGS  += --shell-file index.html
WEB_OUT      := bubble_chamber.html

web: $(WEB_OUT)

$(WEB_OUT): main.c index.html
	$(EMCC) $(WEB_CFLAGS) -o $@ $< $(WEB_LDFLAGS)

clean:
	rm -f $(TARGET) $(WEB_OUT) bubble_chamber.js bubble_chamber.wasm
