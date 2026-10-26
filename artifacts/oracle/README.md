`oracle` is a native Linux program: "ELF 32-bit LSB executable, Intel i386, statically linked". It doesn't even use a C library; it talks to the Linux kernel directly.

## Usage

```console
$ make -C artifacts/oracle
$ artifacts/oracle/oracle <dls> <smf> <loops> <max_frames> <out_wav>
$ MSGS_RENDER=artifacts/oracle/oracle .venv/bin/python artifacts/score.py   # A/B against the references
```

- The arguments are the same as `msgs-render`'s; the output is a 22050 Hz stereo 16-bit WAV.
- The driver is read from `artifacts/swmidi.sys`, a path fixed at build time. Only the XP SP3 build (5.1.2600.5512) is accepted.
- `MIX_FRAMES` (default 1024) is the one build knob: frames per call to the driver's `Mix`.
- `--jitter=MS` (before the other arguments, as in `msgs-render`) delays each tick's events by a uniform random 0–MS ms, in order and reproducibly. It's for realism: the reference captures show about 22 ms of such jitter; the driver adds none.
- The entry-point addresses were taken from Microsoft's public symbols (`swmidi.pdb`) and are hardcoded, so the PDB is needed neither to build nor to run.

## Why that's possible

`swmidi.sys` is plain 32-bit x86 machine code, the same instruction set Linux runs. Windows only gets involved in two ways:

1. **Loading.** The file is in Windows' PE format, so something has to put it into memory correctly.
2. **The functions it calls into Windows.** The driver lists 61 of them, from `ntoskrnl.exe` and `ks.sys`.

The synthesis itself (voices, envelopes, mixing) is pure arithmetic. It never touches hardware or kernel-only instructions, so it runs the same in a normal user-space process. Replace those two Windows roles and the code doesn't know the difference.

## How it's done, step by step

1. **Load the file** (`load_driver`):
   - Read `swmidi.sys`, refuse it unless it's the exact XP SP3 build, and ask Linux for memory that can hold executable code.
   - Copy each section to where the file says it belongs.
   - If that memory didn't land at the driver's preferred address, patch every absolute address in its code.

2. **Supply its Windows functions** (`k_imports`):
   - The driver keeps a table of slots for the Windows functions it calls. The loader fills each slot with a small C function.
   - Memory allocation becomes a simple allocator. Locks become no-ops, since only one thread exists.
   - `ZwCreateFile`/`ZwReadFile` return the `<dls>` argument whatever path the driver asks for. Registry lookups just fail, so the driver keeps its default `gm.dls` path.
   - The Windows compiler's 64-bit math helpers are rewritten by hand, partly in assembly (`allshl`/`allshr`).
   - Each stub uses the calling convention Windows would (`stdcall`, `fastcall`).
   - About 24 functions are actually needed. Every other slot points to a `trap` that prints the function's name and exits, so a render that strays off the modelled path fails loudly instead of producing wrong audio.

3. **Find what to call.** The driver normally gets instructions from Windows' audio system through kernel-streaming requests. The oracle skips that layer entirely:
   - Microsoft's public symbol file gave the name and address of each function.
   - Disassembling the setup code showed the order the driver initializes itself in: table setup, then creating the synth object, which loads `gm.dls` on its own.
   - That showed the synth has a small internal interface: "queue a MIDI message at sample time T" and "mix N samples into this buffer".

4. **Drive it** (`driver_start`, then `synth_midi`, `synth_sysex`, `render_frames`, `voice_any_active`):
   - Run the same setup the driver would, then let slopgs's own MIDI file parser (`src/engine/smf.c`) feed it.
   - Each note or controller becomes a call to the driver's `RecordMIDI` with an exact sample time.
   - The audio comes from repeated calls to the driver's `Mix`, in 1024-frame chunks, the same buffer size the driver uses on Windows.
   - The samples are written straight to a WAV file.

5. **Build without a 32-bit C library** (`_start`). Only `gcc -m32` code generation is needed; no 32-bit libc or libgcc. So the program has its own startup code and calls the Linux kernel directly for opening files, reading, writing and memory mapping.

## What it is and isn't

The driver's own code computes the samples, with no Windows code and no emulator. That it matches the driver's output buffer on XP bit for bit is not shown: the reference captures are loopback recordings and can't show it. What is checked:

- Renders are deterministic.
- x87 control words `0x27F` and `0x37F` render all probes and `tests/` items byte-identically.
- The driver picks its MMX mixer whenever the CPU reports MMX, which every x86-64 host does.

What it doesn't reproduce is the real-time path: how a Windows MIDI player timed and ordered events on their way to the driver. That's why it's exact for synthesis but not necessarily for event delivery during the reference captures. It also builds `smf.c` with `SMF_BANKPROG_LOOKAHEAD=0`, so same-tick Bank Select/Program Change events stay in file order instead of `msgs-render`'s fitted reorder. Keep that in mind when diffing the two on same-tick items such as `42_multitrack_same_tick`.
