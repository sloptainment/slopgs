# Makefile -- two targets, two platforms, one engine (src/engine/*.c).
#
#   dist/msgs.wasm     freestanding wasm32 module (src/wasm.c + engine)
#   dist/msgs-render    native CLI renderer         (src/cli.c  + engine)
#
# No object files: the engine is small enough (a dozen .c files) that a
# single whole-program invocation per target recompiles everything in well
# under a second, so there is nothing to track dependencies for and nothing
# for `clean` to sweep up but the two binaries themselves.
#
# ponytail: no incremental .o/.d build -- add one if per-file compiles ever
# become slow enough to notice.

CC    ?= gcc
CLANG ?= clang
DIST  := dist

ENGINE_SRCS := $(wildcard src/engine/*.c)
ENGINE_HDRS := $(wildcard src/engine/*.h)
WASM_SRCS   := src/wasm.c $(ENGINE_SRCS)
CLI_SRCS    := src/cli.c  $(ENGINE_SRCS)

# -fvisibility=hidden + --export-dynamic: only symbols wasm.c marks
# __attribute__((visibility("default"))) end up exported.
CFLAGS_WASM  := --target=wasm32 -O2 -nostdlib -fvisibility=hidden -Wall -Wextra -Isrc
LDFLAGS_WASM := -Wl,--no-entry -Wl,--export-dynamic

CFLAGS_NATIVE := -std=c11 -O2 -Wall -Wextra -Isrc

UNIT_SRCS := src/unit.c src/unit_tap.c $(ENGINE_SRCS)

# VST2 instrument: GUI-less, cross-compiled 32-bit DLL (not built by `all`,
# needs a mingw cross-compiler not every contributor has) plus a native test
# harness.
VSTI_SRCS      := src/vsti/vsti.c src/vsti/gmdls.c $(ENGINE_SRCS)
VSTI_TEST_SRCS := src/vsti/test.c src/vsti/vsti.c src/vsti/gmdls.c \
                  src/unit_tap.c $(ENGINE_SRCS)
CC_X86  ?= i686-w64-mingw32-gcc
# 1 = drain MIDI at the service block boundary, as the driver does (the
# default, see vsti.c); 0 = give note-ons their sub-block offset.
VSTI_TIMING ?= 1
# Gain-segment/service-block length for the plugin only. 512 frames (23.2 ms)
# is what direct measurement of the reference captures shows; the engine's own
# 256 is a corpus-sweep choice, and the corpus is scored through msgs-render,
# not through a host. artifacts/INVESTIGATE.adoc#i-segment-length.
VSTI_SEGMENT ?= 512
CFLAGS_VSTI := -std=c11 -O2 -Wall -Wextra -Isrc \
               -DVSTI_IMPRECISE_TIMING=$(VSTI_TIMING) \
               -DSERVICE_BLOCK_FRAMES=$(VSTI_SEGMENT)

.PHONY: all clean probes test unit vsti vsti-test

all: $(DIST)/msgs.wasm $(DIST)/msgs-render

$(DIST)/msgs.wasm: $(WASM_SRCS) $(ENGINE_HDRS) | $(DIST)
	$(CLANG) $(CFLAGS_WASM) $(LDFLAGS_WASM) -o $@ $(WASM_SRCS)

$(DIST)/msgs-render: $(CLI_SRCS) $(ENGINE_HDRS) | $(DIST)
	$(CC) $(CFLAGS_NATIVE) -o $@ $(CLI_SRCS) -lm

# RESAMPLE_FACTOR=2 build for artifacts/rate_parity.py
$(DIST)/msgs-render-x2: $(CLI_SRCS) $(ENGINE_HDRS) | $(DIST)
	$(CC) $(CFLAGS_NATIVE) -DRESAMPLE_FACTOR=2 -o $@ $(CLI_SRCS) -lm

$(DIST)/msgs-unit: $(UNIT_SRCS) $(ENGINE_HDRS) src/unit_tap.h | $(DIST)
	$(CC) $(CFLAGS_NATIVE) -o $@ $(UNIT_SRCS) -lm

$(DIST)/msgs-vsti-x86.dll: $(VSTI_SRCS) $(ENGINE_HDRS) src/vsti/vst2.h $(DIST)/gm.dls | $(DIST)
	$(CC_X86) $(CFLAGS_VSTI) -shared -static-libgcc -static -o $@ $(VSTI_SRCS)

$(DIST)/vsti-test: $(VSTI_TEST_SRCS) $(ENGINE_HDRS) src/vsti/vst2.h src/unit_tap.h $(DIST)/gm.dls | $(DIST)
	$(CC) $(CFLAGS_VSTI) -DVSTI_TEST_BUILD -o $@ $(VSTI_TEST_SRCS) -lm

$(DIST):
	mkdir -p $(DIST)

clean:
	rm -f $(DIST)/msgs.wasm $(DIST)/msgs-render $(DIST)/msgs-render-x2 $(DIST)/msgs-unit
	rm -f $(DIST)/msgs-vsti-x86.dll $(DIST)/vsti-test

probes:
	python3 artifacts/make_probes.py

test: $(DIST)/msgs-render
	./$(DIST)/msgs-render --selftest $(DIST)/gm.dls artifacts/probes/01_programs.mid

unit: $(DIST)/msgs-unit
	./$(DIST)/msgs-unit

vsti: $(DIST)/msgs-vsti-x86.dll

vsti-test: $(DIST)/vsti-test
	./$(DIST)/vsti-test
