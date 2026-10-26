# Makefile -- the swmidi.sys oracle: a freestanding i386 binary that maps the
# real driver and renders through it (oracle.c). Kept apart from the top-level
# Makefile: it is a measurement tool, not part of the clean-room engine, and
# needs only gcc's -m32 code generation (no 32-bit libc or libgcc).
#
#   make -C artifacts/oracle
#   MSGS_RENDER=artifacts/oracle/oracle .venv/bin/python artifacts/score.py

CC   ?= gcc
ROOT := $(abspath ../..)

# Frames per ControlLogic::Mix call (the driver's 0x1000-byte buffer)
MIX_FRAMES ?= 1024

CFLAGS := -m32 -march=i686 -O2 -ffreestanding -fno-pie -fno-stack-protector \
          -fno-tree-loop-distribute-patterns -Wall -Wextra -I$(ROOT)/src \
          -DSWMIDI_SYS='"$(ROOT)/artifacts/swmidi.sys"' \
          -DORACLE_MIX_FRAMES=$(MIX_FRAMES)u \
          -DSMF_BANKPROG_LOOKAHEAD=0 -DSERVICE_BLOCK_FRAMES=$(MIX_FRAMES)u
LDFLAGS := -m32 -nostdlib -static -no-pie

SRCS := oracle.c $(ROOT)/src/engine/smf.c

oracle: $(SRCS) $(ROOT)/src/engine/smf.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS)

clean:
	rm -f oracle

.PHONY: clean
