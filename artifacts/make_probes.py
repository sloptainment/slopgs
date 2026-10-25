#!/usr/bin/env python3
"""Generate MSGS reference-render probe MIDIs. Stdlib only.

Each file isolates ONE variable so a diff against the real synth's render
points at a specific DSP stage instead of "something is off somewhere".
Run: python3 make_probes.py   -> writes *.mid next to this script.
"""

import os
import struct

TPQ = 480          # ticks per quarter
TEMPO = 500_000    # us per quarter -> 120bpm -> 960 ticks/sec
TPS = TPQ * 1_000_000 // TEMPO


def t(seconds):
    return int(round(seconds * TPS))


def varlen(n):
    out = bytearray([n & 0x7F])
    n >>= 7
    while n:
        out.append((n & 0x7F) | 0x80)
        n >>= 7
    return bytes(reversed(out))


class Track:
    """Absolute-time event list, serialized to delta times at the end."""

    def __init__(self):
        self.events = []   # (abs_tick, order, raw_bytes)
        self._seq = 0

    def at(self, tick, data):
        self.events.append((tick, self._seq, bytes(data)))
        self._seq += 1
        return self

    def note(self, tick, dur, ch, key, vel=100, off_vel=0):
        self.at(tick, [0x90 | ch, key, vel])
        self.at(tick + dur, [0x80 | ch, key, off_vel])
        return self

    def cc(self, tick, ch, num, val):
        return self.at(tick, [0xB0 | ch, num, val])

    def prog(self, tick, ch, num):
        return self.at(tick, [0xC0 | ch, num])

    def bend(self, tick, ch, value14):
        return self.at(tick, [0xE0 | ch, value14 & 0x7F, (value14 >> 7) & 0x7F])

    def sysex(self, tick, payload):
        # payload excludes the leading F0, includes the trailing F7
        return self.at(tick, b"\xf0" + varlen(len(payload)) + bytes(payload))

    def rpn(self, tick, ch, rpn_msb, rpn_lsb, data_msb):
        self.cc(tick, ch, 101, rpn_msb)
        self.cc(tick, ch, 100, rpn_lsb)
        self.cc(tick, ch, 6, data_msb)
        return self

    def serialize(self, tempo=True):
        body = bytearray()
        if tempo:
            body += b"\x00\xff\x51\x03" + struct.pack(">I", TEMPO)[1:]
        prev = 0
        for tick, _, data in sorted(self.events, key=lambda e: (e[0], e[1])):
            body += varlen(tick - prev) + data
            prev = tick
        body += b"\x00\xff\x2f\x00"
        return b"MTrk" + struct.pack(">I", len(body)) + bytes(body)


# CLAUDE.adoc: "All MIDIs must be generated in probes/." Every .mid and every
# .manifest.tsv lands here; `here` stays the artifacts/ dir because the gm.dls
# lookup below is relative to that, not to probes/.
PROBE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "probes")


def write(name, track, raw_track=None, tracks=None):
    """`tracks` (a list of Track) writes a FORMAT 1 file, one MTrk each, tempo
    in the first only -- probe 42 needs it, everything else is format 0."""
    if tracks is not None:
        chunk = b"".join(tr.serialize(tempo=(i == 0)) for i, tr in enumerate(tracks))
        head = b"MThd" + struct.pack(">IHHH", 6, 1, len(tracks), TPQ)
    else:
        chunk = raw_track if raw_track is not None else track.serialize()
        head = b"MThd" + struct.pack(">IHHH", 6, 0, 1, TPQ)
    os.makedirs(PROBE_DIR, exist_ok=True)
    path = os.path.join(PROBE_DIR, name)
    with open(path, "wb") as f:
        f.write(head + chunk)
    return path


# --- probes ---------------------------------------------------------------

def p01_programs():
    """All 128 GM programs, identical note. Isolates per-patch region lookup."""
    tr = Track()
    for prog in range(128):
        base = t(prog * 2.5)
        tr.prog(base, 0, prog)
        tr.note(base + t(0.1), t(1.5), 0, 60, 100)
    return write("01_programs.mid", tr)


def p02_keyrange():
    """Every key on three patches. Isolates region splits and pitch scaling."""
    tr = Track()
    clock = 0
    for prog in (0, 48, 73):
        tr.prog(clock, 0, prog)
        clock += t(0.2)
        for key in range(128):
            tr.note(clock, t(0.35), 0, key, 100)
            clock += t(0.6)
        clock += t(1.0)
    return write("02_keyrange.mid", tr)


def p03_velocity():
    """Velocity sweep. Isolates velocity->amplitude/filter curves."""
    tr = Track()
    clock = 0
    for prog in (0, 48):
        tr.prog(clock, 0, prog)
        clock += t(0.2)
        for vel in range(1, 128, 2):
            tr.note(clock, t(0.5), 0, 60, vel)
            clock += t(0.9)
        clock += t(1.0)
    return write("03_velocity.mid", tr)


def p04_envelope():
    """Note-off at many points. Isolates attack/decay/sustain/release stages."""
    tr = Track()
    clock = 0
    for prog in (0, 48, 73):
        tr.prog(clock, 0, prog)
        clock += t(0.2)
        for hold in (0.01, 0.03, 0.08, 0.15, 0.3, 0.6, 1.2, 2.5, 5.0):
            tr.note(clock, t(hold), 0, 60, 100)
            clock += t(hold + 4.0)   # long gap: capture the full release tail
        clock += t(1.0)
    return write("04_envelope.mid", tr)


def p05_pitchbend():
    """Bend sweeps at three bend ranges. Isolates pitch math and RPN handling."""
    tr = Track()
    clock = 0
    for rng in (2, 12, 24):
        tr.rpn(clock, 0, 0, 0, rng)
        clock += t(0.2)
        tr.note(clock, t(6.0), 0, 60, 100)
        for i in range(121):
            tr.bend(clock + t(i * 0.05), 0, int(i * 16383 / 120))
        clock += t(7.0)
        tr.bend(clock, 0, 8192)
    return write("05_pitchbend.mid", tr)


def p06_modwheel():
    """CC1 depth steps on a held note. Isolates LFO rate/depth/shape."""
    tr = Track()
    clock = 0
    for prog in (48, 73):
        tr.prog(clock, 0, prog)
        for val in (0, 32, 64, 96, 127):
            tr.cc(clock, 0, 1, val)
            tr.note(clock + t(0.1), t(4.0), 0, 60, 100)
            clock += t(5.5)
        tr.cc(clock, 0, 1, 0)
    return write("06_modwheel.mid", tr)


def p07_pan_volume():
    """CC10/CC7/CC11 steps. Isolates pan law and the volume->gain curve."""
    tr = Track()
    clock = 0
    for cc_num in (10, 7, 11):
        for val in (0, 16, 32, 48, 64, 80, 96, 112, 127):
            tr.cc(clock, 0, 7, 100)
            tr.cc(clock, 0, 11, 127)
            tr.cc(clock, 0, 10, 64)
            tr.cc(clock, 0, cc_num, val)
            tr.note(clock + t(0.05), t(1.5), 0, 60, 100)
            clock += t(2.5)
        clock += t(1.0)
    return write("07_pan_volume.mid", tr)


def p08_reverb():
    """CC91 steps, short note, long tail. Isolates the reverb unit alone."""
    tr = Track()
    clock = 0
    tr.cc(clock, 0, 93, 0)   # chorus off, so only reverb moves
    for val in (0, 16, 32, 48, 64, 80, 96, 112, 127):
        tr.cc(clock, 0, 91, val)
        tr.note(clock + t(0.05), t(0.2), 0, 60, 100)
        clock += t(6.0)      # long gap: the whole tail must decay into silence
    return write("08_reverb.mid", tr)


def p09_chorus():
    """CC93 steps with reverb off. Isolates the chorus unit alone."""
    tr = Track()
    clock = 0
    tr.cc(clock, 0, 91, 0)
    tr.prog(clock, 0, 48)
    for val in (0, 16, 32, 48, 64, 80, 96, 112, 127):
        tr.cc(clock, 0, 93, val)
        tr.note(clock + t(0.05), t(3.0), 0, 60, 100)
        clock += t(5.0)
    return write("09_chorus.mid", tr)


def p10_polyphony():
    """Stack notes past the voice limit. Isolates the voice-stealing rule."""
    tr = Track()
    clock = t(0.2)
    # ramp on: one note every 60ms, 64 of them, all sustained
    for i in range(64):
        tr.at(clock + t(i * 0.06), [0x90, 24 + i, 100])
    end = clock + t(64 * 0.06) + t(4.0)
    for i in range(64):
        tr.at(end, [0x80, 24 + i, 0])
    # second pass across all 16 channels at once
    clock = end + t(3.0)
    for ch in range(16):
        if ch == 9:
            continue
        for n in range(6):
            tr.note(clock + t(ch * 0.05), t(3.0), ch, 48 + n * 4, 100)
    return write("10_polyphony.mid", tr)


def p11_drums():
    """Every drum note across seven kits on ch10. Isolates the drum key map."""
    tr = Track()
    clock = 0
    # all nine kits gm.dls actually defines — 48 (Orchestra) and 56 (SFX) were
    # missing from the first cut, and SFX has a different region count (46 vs 61)
    for kit in (0, 8, 16, 24, 25, 32, 40, 48, 56):
        tr.prog(clock, 9, kit)
        clock += t(0.2)
        for key in range(27, 88):
            tr.note(clock, t(0.3), 9, key, 100)
            clock += t(0.6)
        clock += t(1.0)
    return write("11_drums.mid", tr)


def p12_gs_sysex():
    """GS reset and part params. Isolates GS-specific state handling."""
    tr = Track()
    clock = 0
    tr.sysex(clock, [0x7E, 0x7F, 0x09, 0x01, 0xF7])                    # GM reset
    clock += t(1.0)
    tr.note(clock, t(1.0), 0, 60, 100)
    clock += t(2.0)
    tr.sysex(clock, [0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F,
                     0x00, 0x41, 0xF7])                                # GS reset
    clock += t(1.0)
    tr.note(clock, t(1.0), 0, 60, 100)
    clock += t(2.0)
    # master volume, then master pan, then ch1 -> drum part
    tr.sysex(clock, [0x7F, 0x7F, 0x04, 0x01, 0x00, 0x40, 0xF7])
    clock += t(0.5)
    tr.note(clock, t(1.0), 0, 60, 100)
    clock += t(2.0)
    tr.sysex(clock, [0x41, 0x10, 0x42, 0x12, 0x40, 0x11, 0x15,
                     0x02, 0x18, 0xF7])                                # ch2=drums
    clock += t(0.5)
    tr.note(clock, t(1.0), 1, 38, 100)
    return write("12_gs_sysex.mid", tr)


def p13_edge():
    """Parser and state edge cases. Isolates event handling, not the DSP."""
    tr = Track()
    clock = t(0.2)
    # note-on with velocity 0 must act as note-off
    tr.at(clock, [0x90, 60, 100])
    tr.at(clock + t(1.0), [0x90, 60, 0])
    clock += t(2.5)
    # sustain pedal holds through note-off
    tr.cc(clock, 0, 64, 127)
    tr.note(clock + t(0.1), t(0.3), 0, 62, 100)
    tr.cc(clock + t(2.0), 0, 64, 0)
    clock += t(4.0)
    # all-sound-off (120) vs all-notes-off (123): 120 must cut the tail dead
    tr.note(clock, t(4.0), 0, 64, 100)
    tr.cc(clock + t(1.0), 0, 120, 0)
    clock += t(3.0)
    tr.note(clock, t(4.0), 0, 65, 100)
    tr.cc(clock + t(1.0), 0, 123, 0)
    clock += t(3.0)
    # retrigger the same key without an intervening note-off
    tr.at(clock, [0x90, 67, 100])
    tr.at(clock + t(0.3), [0x90, 67, 100])
    tr.at(clock + t(0.6), [0x90, 67, 100])
    tr.at(clock + t(2.0), [0x80, 67, 0])
    clock += t(3.5)
    # reset-all-controllers must revert bend and modwheel
    tr.cc(clock, 0, 1, 127)
    tr.bend(clock, 0, 16383)
    tr.note(clock + t(0.1), t(1.0), 0, 60, 100)
    tr.cc(clock + t(1.5), 0, 121, 0)
    tr.note(clock + t(2.0), t(1.0), 0, 60, 100)
    return write("13_edge.mid", tr)


def p14_running_status():
    """Hand-built track using running status. Nothing else exercises it."""
    body = bytearray()
    body += b"\x00\xff\x51\x03" + struct.pack(">I", TEMPO)[1:]
    body += b"\x00\x90\x3c\x64"                      # note on, status byte given
    body += varlen(t(0.5)) + b"\x3e\x64"             # running status: note on
    body += varlen(t(0.5)) + b"\x40\x64"
    body += varlen(t(1.0)) + b"\x3c\x00"             # running status: vel-0 off
    body += varlen(t(0.2)) + b"\x3e\x00"
    body += varlen(t(0.2)) + b"\x40\x00"
    body += varlen(t(1.0)) + b"\xff\x2f\x00"
    return write("14_running_status.mid", None,
                 b"MTrk" + struct.pack(">I", len(body)) + bytes(body))


def gs(addr, data):
    """Roland GS SysEx payload with checksum. addr/data are byte lists."""
    body = list(addr) + list(data)
    csum = (128 - (sum(body) % 128)) % 128
    return [0x41, 0x10, 0x42, 0x12] + body + [csum, 0xF7]


GS_RESET = gs([0x40, 0x00, 0x7F], [0x00])


def read_dls_inventory(path):
    """Enumerate (bank_msb, bank_lsb, program, is_drum, name) from a DLS file.

    Guessing which banks exist wastes render time on silent fallbacks and, worse,
    misses the ones that do (the first cut of this probe tested banks 0/8/16/24/32
    and missed "Square" at bank 1 entirely). Read the collection instead.
    """
    d = open(path, "rb").read()
    assert d[:4] == b"RIFF" and d[8:12] == b"DLS ", "not a DLS file"

    def walk(off, end):
        out, i = [], off
        while i + 8 <= end:
            cid = d[i:i + 4]
            sz = struct.unpack("<I", d[i + 4:i + 8])[0]
            body = i + 8
            if cid in (b"LIST", b"RIFF"):
                out += walk(body + 4, body + sz)
            else:
                out.append((cid, body, sz))
            i = body + sz + (sz & 1)
        return out

    insts, cur = [], None
    for cid, body, sz in walk(12, len(d)):
        if cid == b"insh":
            _, bank, prog = struct.unpack("<III", d[body:body + 12])
            cur = ((bank >> 8) & 0x7F, bank & 0x7F, prog, bool(bank & 0x80000000))
        elif cid == b"INAM" and cur is not None:
            name = d[body:body + sz].split(b"\x00")[0].decode("latin-1").strip()
            insts.append(cur + (name,))
            cur = None
    return insts


def p15_banks():
    """Bank select: one representative tone per bank, plus the fallback rule.

    The full (bank, program) -> tone map is already recoverable from gm.dls, so
    rendering all 98 variation tones would mostly re-measure known data. What the
    collection does NOT tell us is what happens on a request for a bank that
    doesn't exist for that program: drop to bank 0, go silent, or keep the
    previous patch. That's what this probe spends its time on.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    inv = read_dls_inventory(os.path.join(here, os.pardir, "dist", "gm.dls"))
    melodic = [x for x in inv if not x[3]]
    present = {(x[0], x[1], x[2]) for x in melodic}

    tr = Track()
    tr.sysex(0, GS_RESET)
    clock = t(1.0)
    manifest = ["# 15_banks.mid — expected tone per note onset",
                "# onset_seconds\tCC0\tCC32\tprogram\texpected"]

    def play(msb, lsb, prog, expect):
        nonlocal clock
        tr.cc(clock, 0, 0, msb)
        tr.cc(clock, 0, 32, lsb)
        tr.prog(clock + t(0.02), 0, prog)
        tr.note(clock + t(0.1), t(1.0), 0, 60, 100)
        manifest.append(f"{clock / TPS + 0.1:.3f}\t{msb}\t{lsb}\t{prog}\t{expect}")
        clock += t(1.8)

    # A: one representative tone per non-zero bank, each against its bank-0 twin
    for msb in sorted({x[0] for x in melodic if x[0]}):
        cand = sorted([x for x in melodic if x[0] == msb], key=lambda x: x[2])[0]
        play(0, 0, cand[2], "<bank 0 reference>")
        play(msb, 0, cand[2], cand[4])

    # B: the lead variations, since these are the ones by ear
    for prog in (80, 81):
        for msb in (0, 1, 8):
            nm = next((x[4] for x in melodic if x[:3] == (msb, 0, prog)), "MISSING")
            play(msb, 0, prog, nm)

    # C: the actual unknown — fallback for combinations that don't exist
    for msb, prog in ((1, 40), (8, 40), (16, 0), (24, 0), (32, 40), (9, 80)):
        if (msb, 0, prog) not in present:
            play(msb, 0, prog, f"MISSING (bank {msb} undefined for prog {prog})")
    for msb in (10, 33, 64, 127):
        play(msb, 0, 0, f"MISSING (bank {msb} undefined everywhere)")
    for lsb in (1, 127):
        play(0, lsb, 0, f"LSB {lsb} (collection defines LSB=0 only)")
    # does a failed bank select leave the previous patch in place?
    play(0, 0, 19, "Church Organ (bank 0, establishes a known patch)")
    play(64, 0, 19, "MISSING bank on same prog — does Church Organ persist?")

    path = write("15_banks.mid", tr)
    with open(os.path.join(PROBE_DIR, "15_banks.manifest.tsv"), "w") as f:
        f.write("\n".join(manifest) + "\n")
    return path


def p16_drum_parts():
    """Arbitrary drum parts via GS "Use For Rhythm Part".

    The GS block number does NOT equal the MIDI channel: block 0 is channel 10,
    blocks 1-9 are channels 1-9, blocks A-F are channels 11-16. Probe 12 got
    this wrong (it set block 1 = channel 1, then played on channel 2), so its
    fourth note is a pitched fallback, not a drum. This probe pins the mapping
    down by playing the same key on both candidate channels after each switch.
    """
    tr = Track()
    tr.sysex(0, GS_RESET)
    clock = t(1.0)

    def probe_pair(label_ch_a, label_ch_b):
        nonlocal clock
        tr.note(clock, t(0.8), label_ch_a, 38, 100)
        clock += t(1.5)
        tr.note(clock, t(0.8), label_ch_b, 38, 100)
        clock += t(1.5)

    # baseline: neither ch1 nor ch2 is a drum part yet
    probe_pair(0, 1)
    # block 1 -> should make MIDI channel 1 (index 0) a drum part
    tr.sysex(clock, gs([0x40, 0x11, 0x15], [0x02]))
    clock += t(0.5)
    probe_pair(0, 1)
    # block 2 -> should make MIDI channel 2 (index 1) a drum part
    tr.sysex(clock, gs([0x40, 0x12, 0x15], [0x02]))
    clock += t(0.5)
    probe_pair(0, 1)
    # block 0 -> channel 10: turn the standard drum part MELODIC
    tr.note(clock, t(0.8), 9, 38, 100)        # drums, before
    clock += t(1.5)
    tr.sysex(clock, gs([0x40, 0x10, 0x15], [0x00]))
    clock += t(0.5)
    tr.note(clock, t(0.8), 9, 38, 100)        # should now be pitched
    clock += t(1.5)
    tr.prog(clock, 9, 80)                     # and should accept a program
    tr.note(clock + t(0.1), t(0.8), 9, 60, 100)
    clock += t(1.5)
    # drum kit variation on a re-assigned part
    tr.sysex(clock, gs([0x40, 0x12, 0x15], [0x02]))
    clock += t(0.5)
    for kit in (0, 8, 16, 24, 25, 32, 40):
        tr.prog(clock, 1, kit)
        tr.note(clock + t(0.05), t(0.6), 1, 38, 100)
        clock += t(1.2)
    return write("16_drum_parts.mid", tr)


def p17_master_volume():
    """GM master volume sweep. Probe 12 showed 50% -> 0.2529 amplitude, i.e.
    the value is applied SQUARED. This maps the whole curve to confirm it."""
    tr = Track()
    clock = t(0.5)
    for msb in (0x7F, 0x60, 0x40, 0x30, 0x20, 0x10, 0x08, 0x00):
        tr.sysex(clock, [0x7F, 0x7F, 0x04, 0x01, 0x00, msb, 0xF7])
        tr.note(clock + t(0.1), t(1.0), 0, 60, 100)
        clock += t(2.0)
    # then the same sweep on CC7, to see whether channel volume uses that curve
    tr.sysex(clock, [0x7F, 0x7F, 0x04, 0x01, 0x00, 0x7F, 0xF7])
    clock += t(0.3)
    for v in (127, 96, 64, 48, 32, 16, 8, 0):
        tr.cc(clock, 0, 7, v)
        tr.note(clock + t(0.1), t(1.0), 0, 60, 100)
        clock += t(2.0)
    return write("17_master_volume.mid", tr)


def p18_key_groups():
    """Exclusive key groups (hi-hat choke and friends).

    The Standard kit defines seven groups: 1 = closed/pedal/open hat, 2 = whistles,
    3 = guiros, 4 = cuicas, 5 = triangles, 6 = surdos, 7 = scratches. A note in a
    group must cut any sounding note in the same group. Open hat is LOOPED with a
    ~4s release, so an unchoked open hat rings for seconds and a choke is obvious.
    Probe 11 sweeps keys in ascending order, so it plays 42 before 46 and never
    triggers a single choke — none of this is otherwise tested.

    The gap between the two notes must be SHORT ENOUGH that A is still sounding.
    The first cut of this probe used a flat 2.0s gap and proved nothing: by then
    even the open hat had decayed ~51dB and there was nothing left to cut. The
    gaps below come from the region data — looped keys (46, 81, 87) ring for
    seconds, but the unlooped one-shots last only as long as their sample:
    long whistle 0.354s, long guiro 0.199s, open cuica 0.112s, scratch 0.083s.
    """
    tr = Track()
    tr.sysex(0, GS_RESET)
    clock = t(1.0)

    notes = ["# 18_key_groups.mid",
             "# first_onset\tgap_s\tkey_a\tkey_b\texpected"]

    def choke(first, second, gap, label):
        nonlocal clock
        t0 = clock
        tr.note(clock, t(0.05), 9, first, 100)
        tr.note(clock + t(gap), t(0.05), 9, second, 100)
        clock += t(3.0)
        notes.append(f"{t0 / TPS:.3f}\t{gap}\t{first}\t{second}\t{label}")

    # group 1, open hat rings ~4s — sweep the gap to see WHEN the cut applies
    for gap in (0.15, 0.30, 0.80):
        choke(46, 42, gap, f"gap {gap}s: OPEN HAT cut by closed hat (grp 1)")
    choke(46, 44, 0.30, "OPEN HAT cut by pedal hat (grp 1)")
    choke(46, 46, 0.30, "OPEN HAT retriggered by itself (grp 1)")
    # groups 5 and 6: looped, release 2.355s and 3.367s
    choke(81, 80, 0.30, "OPEN TRIANGLE cut by mute triangle (grp 5)")
    choke(87, 86, 0.30, "OPEN SURDO cut by mute surdo (grp 6)")
    # unlooped groups: gap must beat the sample length or there is nothing to cut
    choke(72, 71, 0.15, "LONG WHISTLE (0.354s sample) cut by short whistle (grp 2)")
    choke(74, 73, 0.08, "LONG GUIRO (0.199s sample) cut by short guiro (grp 3)")
    choke(79, 78, 0.05, "OPEN CUICA (0.112s sample) cut by mute cuica (grp 4)")
    choke(30, 29, 0.04, "SCRATCH PULL (0.083s sample) cut by push (grp 7)")
    # controls: no shared group, must NOT cut
    choke(46, 38, 0.30, "open hat + SNARE — different group, must NOT cut")
    choke(49, 57, 0.30, "crash 1 + crash 2 — no key group, must NOT cut")
    choke(51, 49, 0.30, "ride + crash — no key group, must NOT cut")
    # baselines: A alone, so the choked cases have something to be compared against
    for key in (46, 81, 87, 72):
        tr.note(clock, t(0.05), 9, key, 100)
        notes.append(f"{clock / TPS:.3f}\t-\t{key}\t-\tBASELINE: {key} alone, uncut")
        clock += t(3.0)

    path = write("18_key_groups.mid", tr)
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(PROBE_DIR, "18_key_groups.manifest.tsv"), "w") as f:
        f.write("\n".join(notes) + "\n")
    return path


def p19_prior_art():
    """Closes out the remaining MEASURABLE claims from `msgs research/`.

    Everything else in the prior art needs the disassembly (interpolation method,
    polyphony cap). These four are settleable by ear/measurement, and each one is
    a specific published assertion rather than a general mechanism.
    """
    tr = Track()
    tr.sysex(0, GS_RESET)
    clock = t(1.0)
    man = ["# 19_prior_art.mid", "# onset\tsection\texpected"]

    def mark(t0, sec, txt):
        man.append(f"{t0 / TPS:.3f}\t{sec}\t{txt}")

    # A. Prior analysis: "CC#120 ... still ignores hold pedal and releases notes",
    #    whereas CC#123 should NOT release notes held by sustain.
    for cc, claim in ((120, "should release DESPITE sustain held"),
                      (123, "should NOT release, sustain holds it")):
        tr.cc(clock, 0, 64, 127)                  # sustain down first
        tr.note(clock + t(0.1), t(0.3), 0, 60, 100)
        tr.cc(clock + t(1.5), 0, cc, 0)           # while sustain still held
        mark(clock, "A", f"CC{cc} at +1.5s with sustain HELD: {claim}")
        tr.cc(clock + t(4.0), 0, 64, 0)           # lift pedal at +4.0
        clock += t(6.0)

    # B. Prior analysis: "MSGS ignores portamento and soft pedal controls"
    tr.cc(clock, 0, 5, 64)                        # portamento time
    tr.cc(clock, 0, 65, 127)                      # portamento ON
    tr.note(clock + t(0.1), t(1.0), 0, 48, 100)
    tr.note(clock + t(1.3), t(1.5), 0, 72, 100)   # big leap: glide or jump?
    mark(clock, "B", "portamento ON, C3->C5 leap: glide would be audible")
    clock += t(4.0)
    tr.cc(clock, 0, 65, 0)
    tr.note(clock + t(0.1), t(1.0), 0, 48, 100)
    tr.note(clock + t(1.3), t(1.5), 0, 72, 100)
    mark(clock, "B", "portamento OFF, same leap: reference for the above")
    clock += t(4.0)
    for soft in (0, 127):
        tr.cc(clock, 0, 67, soft)                 # soft pedal
        tr.note(clock + t(0.1), t(1.5), 0, 60, 100)
        mark(clock, "B", f"soft pedal CC67={soft}: claim is that it is ignored")
        clock += t(3.0)
    tr.cc(clock, 0, 67, 0)

    # C. Prior analysis describes Capital Tone Fallback as "closest available tone,
    #    rounded down". Program 80 has banks 0, 1, 8 only. Requesting bank 9:
    #    round-down predicts bank 8 (Sine Wave), plain fallback predicts bank 0
    #    (Square Wave). Play the candidates ADJACENT so alignment drift between
    #    distant notes cannot confound the comparison (which is what made the
    #    first attempt at this inconclusive).
    for msb, note in ((0, "bank 0 = Square Wave"), (8, "bank 8 = Sine Wave"),
                      (9, "bank 9 UNDEFINED -> which one?"),
                      (0, "bank 0 again"), (9, "bank 9 again"),
                      (8, "bank 8 again"), (9, "bank 9 again")):
        tr.cc(clock, 0, 0, msb)
        tr.cc(clock, 0, 32, 0)
        tr.prog(clock + t(0.02), 0, 80)
        tr.note(clock + t(0.1), t(1.0), 0, 60, 100)
        mark(clock, "C", note)
        clock += t(1.6)

    # D. same, on a program whose only banks are 0 and 8, to separate
    #    "round down to nearest defined" from "always bank 0"
    for msb, note in ((0, "prog 0 bank 0 = Piano 1"), (8, "prog 0 bank 8"),
                      (16, "prog 0 bank 16"), (9, "prog 0 bank 9 UNDEFINED"),
                      (17, "prog 0 bank 17 UNDEFINED"),
                      (1, "prog 0 bank 1 UNDEFINED")):
        tr.cc(clock, 0, 0, msb)
        tr.cc(clock, 0, 32, 0)
        tr.prog(clock + t(0.02), 0, 0)
        tr.note(clock + t(0.1), t(1.0), 0, 60, 100)
        mark(clock, "D", note)
        clock += t(1.6)

    path = write("19_prior_art.mid", tr)
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(PROBE_DIR, "19_prior_art.manifest.tsv"), "w") as f:
        f.write("\n".join(man) + "\n")
    return path


def p20_voice_count():
    """Find the exact polyphony cap by energy plateau.

    Probe 10 could only establish >=64 because it used program 0: a decaying
    piano makes a stolen voice look like ordinary decay, and its second section
    reused the same six keys on every channel so voices could not be counted at
    all.

    This uses program 80 bank 8, the GS "Sine Wave" — measured at h2 -45dB and
    everything above -53dB, i.e. a genuinely pure tone, with a flat sustain
    (+-2% over a second) and an instant attack. One voice therefore contributes
    exactly ONE countable spectral peak, so voices are counted directly rather
    than inferred from an energy plateau.

    Keys 48-127 only: at 130Hz a semitone is ~7.8Hz, which a 32768-point FFT
    resolves comfortably. Going lower would pack peaks below the bin width and
    make them uncountable, which is exactly the register that gets stolen first.

    Velocity 40, not 100: 80 sines at full velocity would sum past full scale,
    and clipping would spray intermodulation products across the spectrum and
    destroy the peak count.
    """
    tr = Track()
    tr.sysex(0, GS_RESET)          # bank select needs GS mode
    man = ["# 20_voice_count.mid", "# onset\tsection\tnote_index\tkey\tchannel"]
    KEYS = list(range(48, 128))    # 80 distinct, all FFT-resolvable
    VEL = 40

    def sine_on(clock, ch):
        tr.cc(clock, ch, 0, 8)
        tr.cc(clock, ch, 32, 0)
        tr.prog(clock + t(0.02), ch, 80)

    # A: 80 distinct sustained sines on ONE channel, one every 100ms
    clock = t(1.0)
    sine_on(clock, 0)
    clock += t(0.2)
    for i, key in enumerate(KEYS):
        tr.at(clock + t(i * 0.1), [0x90, key, VEL])
        man.append(f"{(clock + t(i * 0.1)) / TPS:.3f}\tA\t{i + 1}\t{key}\t0")
    end_a = clock + t(len(KEYS) * 0.1) + t(3.0)
    for key in KEYS:
        tr.at(end_a, [0x80, key, 0])

    # B: the same 80 voices spread over 8 channels — global pool or per-channel?
    clock = end_a + t(4.0)
    for ch in range(8):
        sine_on(clock, ch)
    clock += t(0.2)
    for i, key in enumerate(KEYS):
        ch = i % 8
        tr.at(clock + t(i * 0.1), [0x90 | ch, key, VEL])
        man.append(f"{(clock + t(i * 0.1)) / TPS:.3f}\tB\t{i + 1}\t{key}\t{ch}")
    end_b = clock + t(len(KEYS) * 0.1) + t(3.0)
    for i, key in enumerate(KEYS):
        tr.at(end_b, [0x80 | (i % 8), key, 0])

    path = write("20_voice_count.mid", tr)
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(PROBE_DIR, "20_voice_count.manifest.tsv"), "w") as f:
        f.write("\n".join(man) + "\n")
    return path


def p21_steal_policy():
    """Which voice gets stolen — oldest, or lowest?

    Probe 20 established a 48-voice global cap, but it ramped UPWARD in pitch,
    so "the 32 oldest notes" and "the 32 lowest notes" were the same set and the
    policy was unidentifiable. This ramps DOWNWARD from key 127:

      oldest-first  -> the LAST 48 started survive, i.e. the LOW keys  (48-95)
      lowest-first  -> the HIGH keys survive, i.e. the same set as probe 20
      quietest-first-> survivors scattered by level, not contiguous

    Section B holds one long note through the whole ramp to see whether a voice
    that has been sounding longest is protected or is the first to go.
    """
    tr = Track()
    tr.sysex(0, GS_RESET)
    man = ["# 21_steal_policy.mid", "# onset\tsection\tnote_index\tkey"]
    KEYS_DOWN = list(range(127, 47, -1))     # 80 notes, high to low
    VEL = 40

    def sine_on(clock, ch):
        tr.cc(clock, ch, 0, 8)
        tr.cc(clock, ch, 32, 0)
        tr.prog(clock + t(0.02), ch, 80)

    # A: descending ramp
    clock = t(1.0)
    sine_on(clock, 0)
    clock += t(0.2)
    for i, key in enumerate(KEYS_DOWN):
        tr.at(clock + t(i * 0.1), [0x90, key, VEL])
        man.append(f"{(clock + t(i * 0.1)) / TPS:.3f}\tA\t{i + 1}\t{key}")
    end_a = clock + t(len(KEYS_DOWN) * 0.1) + t(3.0)
    for key in KEYS_DOWN:
        tr.at(end_a, [0x80, key, 0])

    # B: one sentinel note held from before the ramp — protected, or first out?
    clock = end_a + t(4.0)
    sine_on(clock, 0)
    clock += t(0.2)
    tr.at(clock, [0x90, 60, VEL])            # sentinel, oldest of all
    man.append(f"{clock / TPS:.3f}\tB\t0\t60 SENTINEL (started first)")
    clock += t(1.0)
    for i, key in enumerate(range(61, 128)):  # 67 more notes on top
        tr.at(clock + t(i * 0.1), [0x90, key, VEL])
        man.append(f"{(clock + t(i * 0.1)) / TPS:.3f}\tB\t{i + 1}\t{key}")
    end_b = clock + t(67 * 0.1) + t(3.0)
    for key in range(60, 128):
        tr.at(end_b, [0x80, key, 0])

    path = write("21_steal_policy.mid", tr)
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(PROBE_DIR, "21_steal_policy.manifest.tsv"), "w") as f:
        f.write("\n".join(man) + "\n")
    return path


def p22_no_gs_reset():
    """Is bank select gated on a prior GS Reset?

    Prior adjudication open question #2. Every existing bank probe (12, 15, 16, 18, 19)
    sends a GS Reset first, so the "bank select requires GS mode" clause has
    never been tested in isolation. The disassembly gives strong circumstantial
    support — CC0 (0x1341e) and CC32 (0x13492) are both gated on a flag at
    [synth+0xf54], written only inside the SysEx handler (0x136f5 / 0x137f4) —
    but the flag's meaning is unconfirmed; it could be a parse-position counter.

    Section A sends NO GS Reset at all. Section B repeats it verbatim after one.
    Same file, so the two are directly comparable; any tone that differs between
    A and B was gated.
    """
    tr = Track()
    man = ["# 22_no_gs_reset.mid", "# onset\tsection\tCC0\tprogram\tchannel\texpected"]
    clock = t(1.0)

    def trial(sec, msb, prog, ch, note, label):
        nonlocal clock
        tr.cc(clock, ch, 0, msb)
        tr.cc(clock, ch, 32, 0)
        tr.prog(clock + t(0.02), ch, prog)
        tr.note(clock + t(0.1), t(1.0), ch, note, 100)
        man.append(f"{clock / TPS + 0.1:.3f}\t{sec}\t{msb}\t{prog}\t{ch}\t{label}")
        clock += t(1.8)

    def battery(sec):
        # bank 0 first as the in-section reference
        trial(sec, 0, 80, 0, 60, "prog 80 bank 0 = Square Wave (reference)")
        trial(sec, 1, 80, 0, 60, "prog 80 bank 1 = Square — gated?")
        trial(sec, 8, 80, 0, 60, "prog 80 bank 8 = Sine Wave — gated?")
        trial(sec, 16, 0, 0, 60, "prog 0 bank 16 = Piano 1d — gated?")
        # Prior analysis claims the extra drum kits are ALWAYS available, GS or not
        trial(sec, 0, 0, 9, 38, "ch10 kit 0 Standard (reference)")
        trial(sec, 0, 8, 9, 38, "ch10 kit 8 Room — claimed available without GS")
        trial(sec, 0, 25, 9, 38, "ch10 kit 25 TR-808 — claimed available without GS")

    battery("A-no-reset")
    tr.sysex(clock, GS_RESET)
    man.append(f"{clock / TPS:.3f}\t--\t-\t-\t-\tGS RESET sent here")
    clock += t(1.5)
    battery("B-after-reset")

    path = write("22_no_gs_reset.mid", tr)
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(PROBE_DIR, "22_no_gs_reset.manifest.tsv"), "w") as f:
        f.write("\n".join(man) + "\n")
    return path


def p23_rpn_tune():
    """RPN master coarse (0,2) and fine (0,1) tune — the gap probe 05 left.

    Probe 05 covers RPN (0,0) bend range. Nothing tests master tune, which is
    exactly what GENERAL SERUM uses (126 coarse, 42 fine) and where the octave
    bug hid. Open questions this settles about the real synth: does MSGS honor
    coarse tune at all? What range does it clamp to? Does master tune reach an
    already-sounding note, or only notes started after it? All on channel 0,
    note 60 (C4, 261.6 Hz) as the fixed reference pitch.
    """
    tr = Track()
    clock = t(0.5)
    man = ["# 23_rpn_tune.mid", "# onset\tsection\tRPN\tdata\texpected"]

    def rpn(msb, lsb, data):
        tr.cc(clock, 0, 101, msb)
        tr.cc(clock, 0, 100, lsb)
        tr.cc(clock, 0, 6, data)

    def ref_note(sec, tag):
        nonlocal clock
        tr.note(clock + t(0.05), t(1.0), 0, 60, 100)
        man.append(f"{clock / TPS + 0.05:.3f}\t{sec}\t-\t-\t{tag}")
        clock += t(1.6)

    # A. coarse tune sweep: RPN(0,2) data = 64 + semitones. Play C4 each time;
    #    it should transpose by (data-64) semitones if honored.
    ref_note("A", "baseline C4, no tune")
    for data, semis in ((64 - 24, -24), (64 - 12, -12), (64 - 5, -5),
                        (64 + 7, +7), (64 + 12, +12), (64 + 24, +24), (64, 0)):
        rpn(0, 2, data)
        tr.note(clock + t(0.1), t(1.0), 0, 60, 100)
        man.append(f"{clock / TPS + 0.1:.3f}\tA\t0,2\t{data}\tC4 {semis:+d} semitones")
        clock += t(1.6)

    # B. fine tune sweep: RPN(0,1) MSB = data. 64 = center; each step ~ up to
    #    +-100 cents across the 14-bit range (LSB left 0). Small pitch shifts.
    for data in (64, 96, 127, 32, 0):
        rpn(0, 1, data)
        tr.note(clock + t(0.1), t(1.0), 0, 60, 100)
        man.append(f"{clock / TPS + 0.1:.3f}\tB\t0,1\t{data}\tfine tune, C4 shifted a few cents")
        clock += t(1.6)
    rpn(0, 1, 64)  # recenter fine

    # C. coarse + fine together, then RPN-null (7F,7F) followed by a stray
    #    data-entry that MUST be ignored (does MSGS honor the null select?).
    rpn(0, 2, 64 + 12)
    rpn(0, 1, 96)
    tr.note(clock + t(0.1), t(1.0), 0, 60, 100)
    man.append(f"{clock / TPS + 0.1:.3f}\tC\t0,2+0,1\t76,96\tC4 +12 semi + fine")
    clock += t(1.6)
    tr.cc(clock, 0, 101, 0x7F)
    tr.cc(clock, 0, 100, 0x7F)          # RPN null
    tr.cc(clock, 0, 6, 0x00)            # stray data entry — should be ignored
    tr.note(clock + t(0.1), t(1.0), 0, 60, 100)
    man.append(f"{clock / TPS + 0.1:.3f}\tC\tnull\t0\tC4 unchanged (null must ignore data entry)")
    clock += t(1.6)

    # D. does master tune reach a HELD note? Start C4, then apply coarse +12
    #    while it sounds. Real-time (like pitch bend) or note-on-only?
    rpn(0, 2, 64)                       # recenter first
    clock += t(0.2)
    tr.at(clock, [0x90, 60, 100])
    man.append(f"{clock / TPS:.3f}\tD\t-\t-\tC4 held; coarse +12 applied at +1.5s")
    tr.cc(clock + t(1.5), 0, 101, 0)
    tr.cc(clock + t(1.5), 0, 100, 2)
    tr.cc(clock + t(1.5), 0, 6, 64 + 12)
    tr.at(clock + t(3.0), [0x80, 60, 0])
    clock += t(4.0)

    path = write("23_rpn_tune.mid", tr)
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(PROBE_DIR, "23_rpn_tune.manifest.tsv"), "w") as f:
        f.write("\n".join(man) + "\n")
    return path


def p24_gain_staging():
    """The gain chain: master vol / CC7 / CC11 / velocity / sample attenuation,
    and — the untested part — whether they SUM in dB.

    The individual curves are covered (probe 03 velocity, 17 master+CC7). What
    no probe checks is the interaction: does velocity 64 + CC7 64 give the sum
    of their individual attenuations, or does something clamp/saturate/apply
    master differently? Each control at ~half is ~-12 dB (squared law); the
    minimal test is a few singles to anchor, then stacks to test additivity.

    Patch bank 1 program 80 ("Square") — a clean, steady, looped tone so RMS is
    a reliable gain proxy. Needs a GS Reset for the bank-1 select to take.
    """
    tr = Track()
    tr.sysex(0, GS_RESET)
    clock = t(0.5)
    man = ["# 24_gain_staging.mid",
           "# onset\tvel\tCC7\tCC11\tmasterMSB\texpected_dB_vs_baseline"]

    def master(msb):
        tr.sysex(clock, [0x7F, 0x7F, 0x04, 0x01, 0x00, msb, 0xF7])

    def note(vel, cc7, cc11, mmsb, exp):
        nonlocal clock
        tr.cc(clock, 0, 0, 1)              # bank 1
        tr.cc(clock, 0, 32, 0)
        tr.prog(clock + t(0.01), 0, 80)   # Square
        master(mmsb)
        tr.cc(clock, 0, 7, cc7)
        tr.cc(clock, 0, 11, cc11)
        tr.note(clock + t(0.05), t(0.5), 0, 60, vel)
        man.append(f"{clock / TPS + 0.05:.3f}\t{vel}\t{cc7}\t{cc11}\t{mmsb}\t{exp}")
        clock += t(0.9)

    # baseline: everything at max (0 dB attenuation reference)
    note(127, 127, 127, 127, "0.0 (baseline)")
    # singles at ~half — each should be ~-12 dB (squared law)
    note(64, 127, 127, 127, "-11.9 (velocity alone)")
    note(127, 64, 127, 127, "-11.9 (CC7 alone)")
    note(127, 127, 64, 127, "-11.9 (CC11 alone)")
    note(127, 127, 127, 64, "-12.0 (master alone)")
    # stacks — test additivity in dB
    note(64, 64, 127, 127, "-23.8 (vel+CC7, if additive)")
    note(64, 64, 64, 127, "-35.7 (vel+CC7+CC11)")
    note(64, 64, 64, 64, "-47.7 (full stack, all four)")
    # a milder stack to confirm additivity away from the half point
    note(96, 96, 127, 127, "-9.0 (vel96+CC7 96, ~-4.5 each)")
    # restore, then a plain full-scale note to bookend/confirm no drift
    note(127, 127, 127, 127, "0.0 (bookend, must match baseline)")

    path = write("24_gain_staging.mid", tr)
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(PROBE_DIR, "24_gain_staging.manifest.tsv"), "w") as f:
        f.write("\n".join(man) + "\n")
    return path


def p25_pan_law():
    """Pan (CC10) law — measure L and R separately across the full sweep.

    The finicky questions: is it constant-power (center = -3 dB per side) or
    linear (center = -6 dB per side) or something else? Does hard-left actually
    silence R? Is CC10=64 the exact center? Sine patch (bank 8 prog 80) so each
    channel's RMS is a clean gain proxy. Everything else pinned at max.
    """
    tr = Track()
    tr.sysex(0, GS_RESET)
    clock = t(0.5)
    man = ["# 25_pan_law.mid", "# onset\tCC10\texpected"]

    def note(cc10, tag):
        nonlocal clock
        tr.cc(clock, 0, 0, 8)             # bank 8 = Sine Wave
        tr.cc(clock, 0, 32, 0)
        tr.prog(clock + t(0.01), 0, 80)
        tr.cc(clock, 0, 7, 127)
        tr.cc(clock, 0, 11, 127)
        tr.cc(clock, 0, 10, cc10)
        tr.note(clock + t(0.05), t(0.6), 0, 60, 100)
        man.append(f"{clock / TPS + 0.05:.3f}\t{cc10}\t{tag}")
        clock += t(1.0)

    for cc10 in (0, 16, 32, 48, 64, 80, 96, 112, 127):
        note(cc10, f"CC10={cc10}: L/R balance")
    return _write_manifest("25_pan_law.mid", tr, man)


def p26_other_gains():
    """The gain terms probe 24 didn't cover: GS part-level PAN (SysEx, separate
    from CC10) and the per-patch sample attenuation (absolute level).

    - Section A: does GS 'part pan' (SysEx 41 10 42 12 40 1p 1C vv) exist and
      stack with CC10? Roland GS carries a part pan independent of CC10.
    - Section B: play several patches at IDENTICAL velocity/CC7/master and
      measure absolute level. Different patches have different wsmp/art1
      attenuations, so this tests whether the per-patch sample volume is
      applied at the right absolute scale (the units question from probe 24).
    """
    tr = Track()
    tr.sysex(0, GS_RESET)
    clock = t(0.5)
    man = ["# 26_other_gains.mid", "# onset\tsection\tdetail"]

    def sine():
        tr.cc(clock, 0, 0, 8); tr.cc(clock, 0, 32, 0); tr.prog(clock + t(0.01), 0, 80)
        tr.cc(clock, 0, 7, 127); tr.cc(clock, 0, 11, 127); tr.cc(clock, 0, 10, 64)

    # A: GS part pan (address 40 10 1C, part 1) — 0x00 left .. 0x40 center .. 0x7F right
    for pan, tag in ((0x40, "GS part pan CENTER (0x40)"), (0x00, "GS part pan LEFT (0x00)"),
                     (0x7F, "GS part pan RIGHT (0x7F)"), (0x40, "back to center")):
        sine()
        tr.sysex(clock, gs([0x40, 0x10, 0x1C], [pan]))
        tr.note(clock + t(0.1), t(0.6), 0, 60, 100)
        man.append(f"{clock / TPS + 0.1:.3f}\tA\t{tag}")
        clock += t(1.0)
    # combine GS part pan with CC10 — do they stack?
    sine()
    tr.sysex(clock, gs([0x40, 0x10, 0x1C], [0x00]))   # GS pan hard left
    tr.cc(clock, 0, 10, 127)                           # CC10 hard right
    tr.note(clock + t(0.1), t(0.6), 0, 60, 100)
    man.append(f"{clock / TPS + 0.1:.3f}\tA\tGS pan LEFT + CC10 RIGHT (cancel? stack?)")
    clock += t(1.0)

    # B: per-patch absolute level — same vel/vol, different patches.
    # bank 0: 0 Piano, 40 Violin, 48 Strings, 56 Trumpet, 80 SquareLead, 118 Melodic Tom
    tr.sysex(clock, GS_RESET); clock += t(0.3)
    for prog in (0, 40, 48, 56, 80, 118):
        tr.cc(clock, 0, 0, 0); tr.cc(clock, 0, 32, 0); tr.prog(clock + t(0.01), 0, prog)
        tr.cc(clock, 0, 7, 127); tr.cc(clock, 0, 11, 127); tr.cc(clock, 0, 10, 64)
        tr.note(clock + t(0.1), t(0.6), 0, 60, 100)
        man.append(f"{clock / TPS + 0.1:.3f}\tB\tprog {prog} at vel100/CC7-127 (absolute level)")
        clock += t(1.0)
    return _write_manifest("26_other_gains.mid", tr, man)


def _write_manifest(name, tr, man, tracks=None):
    path = write(name, tr, tracks=tracks)
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(PROBE_DIR, name.replace(".mid", ".manifest.tsv")), "w") as f:
        f.write("\n".join(man) + "\n")
    return path


def p27_gain_curves():
    """Rigorous version of probe 24: prove the four controls use the SAME curve
    and compose additively — WITHOUT the coincidence trap.

    Probe 24 tested every control at the same value (64), so four different
    curves that merely cross at 64 would look identical and additive. This fixes
    both holes:
      A. sweep each of vel / CC7 / CC11 / master over the SAME distinguishing
         values (8, 20, 40, 64, 90, 110) and compare the four curves point by
         point. These values maximally separate squared from linear or a table.
      B. combinations of UNEQUAL values (vel 110 + CC7 20, vel 20 + CC7 110,
         three-way asymmetric) so additivity is tested where each term differs.
    If curve_vel(a)+curve_cc7(b) predicts each combo from the section-A curves,
    same-curve + additive is proven; deviation exposes a per-control curve or a
    non-linear compose. Sine (bank 8 prog 80) for clean RMS.
    """
    tr = Track()
    tr.sysex(0, GS_RESET)
    clock = t(0.5)
    man = ["# 27_gain_curves.mid", "# onset\tcontrol\tvalue\tvel\tCC7\tCC11\tmasterMSB"]
    VALS = (127, 110, 90, 64, 40, 20, 8)

    def setup():
        tr.cc(clock, 0, 0, 8); tr.cc(clock, 0, 32, 0); tr.prog(clock + t(0.01), 0, 80)

    def play(vel, cc7, cc11, mmsb, ctrl, val):
        nonlocal clock
        setup()
        tr.sysex(clock, [0x7F, 0x7F, 0x04, 0x01, 0x00, mmsb, 0xF7])
        tr.cc(clock, 0, 7, cc7); tr.cc(clock, 0, 11, cc11); tr.cc(clock, 0, 10, 64)
        tr.note(clock + t(0.05), t(0.45), 0, 60, vel)
        man.append(f"{clock / TPS + 0.05:.3f}\t{ctrl}\t{val}\t{vel}\t{cc7}\t{cc11}\t{mmsb}")
        clock += t(0.8)

    # A: four curves over identical values
    for v in VALS: play(v, 127, 127, 127, "velocity", v)
    for v in VALS: play(127, v, 127, 127, "CC7", v)
    for v in VALS: play(127, 127, v, 127, "CC11", v)
    for v in VALS: play(127, 127, 127, v, "master", v)
    # B: asymmetric combinations
    for vel, cc7, cc11 in ((110, 20, 127), (20, 110, 127), (90, 40, 127),
                           (40, 90, 127), (110, 40, 20), (64, 90, 110)):
        play(vel, cc7, cc11, 127, "combo", f"v{vel}c{cc7}e{cc11}")
    return _write_manifest("27_gain_curves.mid", tr, man)



def p28_expression_gate():
    """CC11 square-wave gating at several rates. Isolates the expression->gain
    smoothing/attack-release behaviour AT SPEED, which no existing probe does.

    Entry 4's GAIN_SMOOTH_ALPHA (a 12 ms time constant) was fit against slow
    CC7/CC11 glides. GENERAL SERUM measure 226 gates CC11 127<->0 every 12-25
    ms, and the reference goes fully SILENT for 90 ms across a stretch where
    expression alternates 0/127 every 25 ms -- i.e. MSGS does not simply track
    a 50%-duty square. Whatever produces that (an asymmetric rise/fall, a
    slower attack than release, or expression being sampled on a slow control
    cadence) is unmeasured and unrecovered.

    Structure: one held note per section, expression square-waved at a fixed
    half-period. Sections descend from far slower than the smoothing constant
    to far faster, so the response can be read as a function of rate:
    half-period = 200, 100, 50, 25, 12, 6 ms. A final section holds CC11 at 0
    for 300 ms then restores 127, to read the rise and fall edges cleanly and
    separately at a rate where they cannot interact.
    """
    tr = Track()
    man = ["section\tstart_s\tinstrument\thalf_period_ms\tnote"]
    clock = 0
    tr.prog(clock, 0, 80)          # Lead 1 (square) -- steady, loop-sustained
    tr.cc(clock, 0, 7, 100)
    tr.cc(clock, 0, 10, 64)
    clock += t(0.5)
    for half_ms in (200, 100, 50, 25, 12, 6):
        start = clock
        man.append(f"gate\t{clock / TPS:.3f}\t80\t{half_ms}\t60")
        tr.cc(clock, 0, 11, 127)
        tr.note(clock, t(3.0), 0, 60, 100)
        # square-wave expression across the held note
        half = t(half_ms / 1000.0)
        if half < 1:
            half = 1
        k = 0
        pos = clock + t(0.25)
        while pos < clock + t(2.75):
            tr.cc(pos, 0, 11, 0 if k % 2 == 0 else 127)
            pos += half
            k += 1
        tr.cc(clock + t(2.9), 0, 11, 127)
        clock += t(4.0)
    # clean single fall/rise edge, well separated
    man.append(f"edge\t{clock / TPS:.3f}\t80\t-\t60")
    tr.cc(clock, 0, 11, 127)
    tr.note(clock, t(3.0), 0, 60, 100)
    tr.cc(clock + t(1.0), 0, 11, 0)
    tr.cc(clock + t(1.3), 0, 11, 127)
    clock += t(4.0)
    return _write_manifest("28_expression_gate.mid", tr, man)


def p29_all_sound_off_gap():
    """CC120 (All Sound Off) mid-note at decreasing intervals. Measures how
    fast the driver actually reaches silence, and whether a note-on landing
    shortly after a CC120 starts clean.

    GENERAL SERUM measure 226 issues note-off + CC120 at every group boundary
    and the reference shows hard 25-90 ms silences there; our render shows no
    gap at all, so either our post-CC120 release is far too slow or CC120's
    effect differs from ordinary note-off more than the driver suggests.

    Structure: hold a note, fire CC120, then re-attack after a gap of
    400/200/100/50/25/12 ms. Reading the silence width per section gives the
    real post-CC120 decay directly. A control section repeats the shortest
    gap with an ordinary note-off instead of CC120, so the two paths can be
    compared on identical material (the driver has two distinct routines,
    0x19a2c vs 0x19aa4).
    """
    tr = Track()
    man = ["section\tstart_s\tmode\tgap_ms\tinstrument\tnote"]
    clock = 0
    tr.cc(clock, 0, 7, 100)
    tr.cc(clock, 0, 11, 127)
    tr.cc(clock, 0, 10, 64)
    clock += t(0.5)
    for prog, pname in ((80, "square_lead"), (48, "strings")):
        tr.prog(clock, 0, prog)
        clock += t(0.1)
        for mode in ("cc120", "noteoff"):
            for gap_ms in (400, 200, 100, 50, 25, 12):
                man.append(f"{mode}\t{clock / TPS:.3f}\t{mode}\t{gap_ms}\t{prog}\t60")
                tr.at(clock, [0x90, 60, 100])
                cut = clock + t(1.0)
                if mode == "cc120":
                    tr.cc(cut, 0, 120, 0)
                else:
                    tr.at(cut, [0x80, 60, 0])
                # re-attack after the gap; silence width in the render is the answer
                re = cut + t(gap_ms / 1000.0)
                tr.note(re, t(1.0), 0, 60, 100)
                clock = re + t(2.0)
    return _write_manifest("29_all_sound_off_gap.mid", tr, man)


def p30_tune_clamp_bend():
    """RPN2 coarse tune + high keys + a pitch-bend sweep, straddling the
    CentsToRatio +-4800 clamp. Settles where latched tuning enters the pitch
    chain and whether live bend is subject to the same clamp.

    CentsToRatio clamps to +-4800 cents [A:0x18e2c], and the disassembled
    note-on sum is
    `fineTune + (key-unityNote)*100 + pitchBendCents` -- with NO RPN term.
    S4.4 lists the RPN1/RPN2 consumer as [O], never located. So the spec does
    not say whether latched tune lands inside or outside that clamp, and
    probe 23 only exercises RPN2 at keys nowhere near it. The distinction is
    inaudible below the clamp and drastic above it: GENERAL SERUM measure 226
    plays keys 105-127 on a patch whose unity note is 81/91, which with its
    +2400 cents of RPN2 crosses +4800 on every note, and the reference's
    pitch plainly still moves there while a summed-into-the-clamp
    implementation freezes dead.

    Structure, all on program 86 (5th Saw Wave, the same patch measure 226
    uses, unity note 81 below key 106 and 91 above it):
      A. RPN2 = 0, keys 60/81/98/105/119/127 held plain -- the unclamped
         baseline, gives each key's own pitch with no tuning applied.
      B. RPN2 = +24 semitones, same keys -- keys 105+ cross the clamp. If
         tune is inside the clamp these flatten to a common pitch; if outside,
         each stays distinct.
      C. RPN2 = +24 semitones, same keys, each with a full-range bend sweep.
         If bend shares the clamp, the sweep is silent on the clamped keys;
         if bend is applied outside it, every key sweeps.
      D. RPN2 = -24 semitones for the symmetric negative-side check.
    """
    tr = Track()
    man = ["section\tstart_s\trpn2_semitones\tkey\tbend\tinstrument"]
    KEYS = (60, 81, 98, 105, 119, 127)
    clock = 0
    tr.cc(clock, 0, 7, 100)
    tr.cc(clock, 0, 11, 127)
    tr.cc(clock, 0, 10, 64)
    tr.prog(clock, 0, 86)
    clock += t(0.5)

    def section(tag, semis, sweep):
        nonlocal clock
        tr.rpn(clock, 0, 0, 2, 64 + semis)   # RPN2 coarse tune
        tr.bend(clock, 0, 8192)
        clock += t(0.2)
        for key in KEYS:
            man.append(f"{tag}\t{clock / TPS:.3f}\t{semis}\t{key}\t{'sweep' if sweep else 'none'}\t86")
            tr.note(clock, t(2.0), 0, key, 100)
            if sweep:
                # full-range bend sweep across the held note, 20 ms steps
                steps = 100
                for i in range(steps + 1):
                    val = int(round(i * 16383 / steps))
                    tr.bend(clock + t(0.1) + i * t(0.018), 0, val)
                tr.bend(clock + t(2.0), 0, 8192)
            clock += t(2.6)
        clock += t(0.6)

    section("A_notune", 0, False)
    section("B_up24", 24, False)
    section("C_up24_bend", 24, True)
    section("D_down24", -24, False)
    return _write_manifest("30_tune_clamp_bend.mid", tr, man)



def p31_tune_clamp_bend_sine():
    """Probe 30 rerun on a single-partial patch: bank 8 program 80, Sine Wave.

    Probe 30 uses program 86 (5th Saw Wave) because that is what GENERAL SERUM
    measure 226 actually plays, which makes it the faithful reproduction of the
    real case -- but it is a poor *measuring instrument* at these pitches. Keys
    105-127 with +24 semitones of coarse tune put the fundamental far above
    Nyquist at the 22050 Hz render rate, so a saw's harmonic stack aliases and
    the strongest visible peak is some folded image rather than the fundamental.
    Probe 30 left keys 119/127 disagreeing with the reference by 5.6% and that
    number could not be attributed: real pitch error, or simply a different
    aliased partial winning the argmax.

    A sine has exactly one partial, so whatever appears IS the (folded)
    fundamental and the comparison is unambiguous. Same structure as probe 30
    -- sections A/B/C/D, same keys, same +-24 semitone RPN2, same bend sweep --
    so the two are directly comparable note for note.

    Also worth having independently: everything in gm.dls except this patch is
    multi-partial, so this is the project's only clean instrument for any future
    pitch question.
    """
    tr = Track()
    man = ["section\tstart_s\trpn2_semitones\tkey\tbend\tinstrument"]
    KEYS = (60, 81, 98, 105, 119, 127)
    tr.sysex(0, GS_RESET)          # bank select is GS-gated on some paths (probe 22)
    clock = t(0.5)
    tr.cc(clock, 0, 7, 100)
    tr.cc(clock, 0, 11, 127)
    tr.cc(clock, 0, 10, 64)
    tr.cc(clock, 0, 0, 8)          # bank 8 ...
    tr.cc(clock, 0, 32, 0)
    tr.prog(clock + t(0.01), 0, 80)  # ... program 80 = Sine Wave
    clock += t(0.5)

    def section(tag, semis, sweep):
        nonlocal clock
        tr.rpn(clock, 0, 0, 2, 64 + semis)   # RPN2 coarse tune
        tr.bend(clock, 0, 8192)
        clock += t(0.2)
        for key in KEYS:
            man.append(f"{tag}\t{clock / TPS:.3f}\t{semis}\t{key}\t{'sweep' if sweep else 'none'}\t8:80_sine")
            tr.note(clock, t(2.0), 0, key, 100)
            if sweep:
                steps = 100
                for i in range(steps + 1):
                    val = int(round(i * 16383 / steps))
                    tr.bend(clock + t(0.1) + i * t(0.018), 0, val)
                tr.bend(clock + t(2.0), 0, 8192)
            clock += t(2.6)
        clock += t(0.6)

    section("A_notune", 0, False)
    section("B_up24", 24, False)
    section("C_up24_bend", 24, True)
    section("D_down24", -24, False)
    return _write_manifest("31_tune_clamp_bend_sine.mid", tr, man)



def p32_ramp_shape():
    """Isolated control steps, each repeated 12x for ensemble averaging, on the
    single-partial Sine Wave patch. Settles the RAMP SHAPE that probes 28 and 31
    could only bound.

    What is already known and what is missing:
      - Probe 28 proved our 12 ms one-pole gain smoothing is far too slow (gate
        depth 4.3 dB where the reference holds 77.9 dB at a 6 ms half-period)
        AND that an instant per-sample gate is also wrong (it regressed the
        corpus). Reference edge durations came out ~21 ms fall / ~13 ms rise.
      - Probe 31 showed our pitch STEPS where the reference GLIDES (~9-22 ms).
      - What neither could give is the SHAPE: linear in amplitude, linear in dB,
        or exponential; and whether fall and rise genuinely differ. Fitting one
        edge on a harmonically rich patch gave R^2 of 0.014-0.65 -- useless.

    Three design changes make the shape recoverable:
      1. SINE patch (bank 8 program 80) -- one partial, so the RMS envelope IS
         the gain, with no harmonic beating to corrupt the edge.
      2. EVERY step repeated 12 times identically, each on its own fresh note so
         the note onset is a hard alignment reference. Averaging 12 aligned
         edges cuts the envelope noise ~3.5x, which is what the earlier fits
         lacked.
      3. SEVERAL step sizes per direction. A duration that scales with step size
         means a rate-limited (constant dB/s or amplitude/s) ramp; a duration
         that does not means a fixed-length ramp. One step size cannot tell
         those apart, which is the ambiguity left over from probe 28.

    Sections A-F are CC11 expression steps, G-J are pitch-bend steps, all with
    the same repeat-and-average structure so the gain and pitch ramps are
    measured on identical footing.
    """
    tr = Track()
    man = ["section\trep\tnote_onset_s\tstep_time_s\tkind\tfrom\tto"]
    REPS = 12
    tr.sysex(0, GS_RESET)
    clock = t(0.5)
    tr.cc(clock, 0, 7, 100)
    tr.cc(clock, 0, 10, 64)
    tr.cc(clock, 0, 0, 8)              # bank 8 ...
    tr.cc(clock, 0, 32, 0)
    tr.prog(clock + t(0.01), 0, 80)    # ... program 80 = Sine Wave
    clock += t(0.5)

    def steps(tag, kind, frm, to):
        """12 identical reps: fresh note, settle, one step, hold, release."""
        nonlocal clock
        for rep in range(REPS):
            onset = clock
            if kind == "cc11":
                tr.cc(onset - t(0.02), 0, 11, frm)
            else:
                tr.bend(onset - t(0.02), 0, frm)
            tr.note(onset, t(1.0), 0, 60, 100)
            step_at = onset + t(0.45)      # well clear of the attack transient
            if kind == "cc11":
                tr.cc(step_at, 0, 11, to)
            else:
                tr.bend(step_at, 0, to)
            man.append(f"{tag}\t{rep}\t{onset / TPS:.4f}\t{step_at / TPS:.4f}\t{kind}\t{frm}\t{to}")
            clock = onset + t(1.30)        # >250ms of silence between reps
        clock += t(0.4)

    # --- expression: both directions at three step sizes -------------------
    steps("A_fall_127_0",   "cc11", 127, 0)
    steps("B_rise_0_127",   "cc11", 0, 127)
    steps("C_fall_127_64",  "cc11", 127, 64)
    steps("D_rise_64_127",  "cc11", 64, 127)
    steps("E_fall_64_0",    "cc11", 64, 0)
    steps("F_rise_0_64",    "cc11", 0, 64)

    # --- pitch bend: both directions, large and small -----------------------
    # 8192 = centre. Full-range steps first, then a two-semitone-ish step, so a
    # rate-limited glide separates from a fixed-length one.
    steps("G_bend_dn_full", "bend", 8192, 0)
    steps("H_bend_up_full", "bend", 8192, 16383)
    steps("I_bend_dn_half", "bend", 8192, 4096)
    steps("J_bend_up_half", "bend", 8192, 12288)
    tr.bend(clock, 0, 8192)
    return _write_manifest("32_ramp_shape.mid", tr, man)



def p33_pitch_ramp():
    """Characterise the driver's pitch-bend RAMP: its shape, its duration law,
    its symmetry, and whether it aims at the clamped or the raw target.

    Why this probe exists. A hand-cut excerpt containing a +-60-semitone bend
    square wave at ~15 Hz (RPN0 set to 127 semitones) was analyzed to show how
    the real driver integrates into smooth arcs; we reproduce it as flat-topped
    squares, because we latch each bend value on receipt. Probe 31 measured the
    staircase (ours steps at ~23 ms, the reference does not) and probe 32 gave
    isolated-step glide times, but neither pins down the LAW. Four things are
    still unknown and each changes the implementation:

      1. SHAPE -- linear in cents, linear in frequency ratio, or a first-order
         exponential approach? Section A's large steps separate these: a linear
         ramp crosses the midpoint at half the duration, an exponential at 69%.
      2. DURATION LAW -- fixed time regardless of step size, or rate-limited
         (constant cents/second)? Section A sweeps step size over 12x at a
         fixed bend range; a fixed-time ramp gives the same 10-90% for all,
         a rate-limited one gives duration proportional to size.
      3. SYMMETRY -- up and down steps are separate rows throughout, because
         probe 28 and probe 32 disagreed about which direction is faster for
         expression and the same question is open for pitch.
      4. CLAMP INTERACTION -- `CentsToRatio` clamps at +-4800 cents (+-48
         semitones), confirmed. Does the ramp travel toward the RAW requested
         pitch and get clipped on arrival, or toward the already-clamped value?
         Section B straddles that boundary: a -40 st step never reaches it, a
         -70 st step is far past it. If the ramp aims at the raw target, the
         -70 case spends longer pinned and its approach rate is steeper.

    Every condition is repeated 8x on its own fresh note so onsets give a hard
    alignment reference and the edges can be ensemble-averaged -- single edges
    on probe 32 gave fits too noisy to read (R^2 0.014-0.65).

    Bank 8 program 80 (Sine Wave) throughout: one partial, so the instantaneous
    pitch is unambiguous. Keys are chosen per direction so the bent pitch stays
    well inside the band and never folds -- aliasing would make the tracking
    ambiguous, which is the whole problem with measuring this on real content.
    """
    tr = Track()
    man = ["section\trep\tnote_onset_s\tstep_time_s\tkind\tkey\trpn0_semis\tfrom_semis\tto_semis"]
    REPS = 8
    CENTRE = 8192

    tr.sysex(0, GS_RESET)
    clock = t(0.5)
    tr.cc(clock, 0, 7, 100)
    tr.cc(clock, 0, 11, 127)
    tr.cc(clock, 0, 10, 64)
    tr.cc(clock, 0, 0, 8)
    tr.cc(clock, 0, 32, 0)
    tr.prog(clock + t(0.01), 0, 80)
    clock += t(0.4)

    def set_range(semis):
        nonlocal clock
        tr.rpn(clock, 0, 0, 0, semis)      # RPN0 = pitch-bend sensitivity
        clock += t(0.15)

    def bend_val(semis, rng):
        v = int(round(CENTRE + (semis / float(rng)) * 8192))
        return max(0, min(16383, v))

    def steps(tag, key, rng, to_semis, hold=1.6, at=0.5):
        """8 reps: fresh note at centre bend, one step at `at`, then settle."""
        nonlocal clock
        for rep in range(REPS):
            onset = clock
            tr.bend(onset - t(0.02), 0, CENTRE)
            tr.note(onset, t(hold), 0, key, 100)
            st = onset + t(at)
            tr.bend(st, 0, bend_val(to_semis, rng))
            man.append(f"{tag}\t{rep}\t{onset / TPS:.4f}\t{st / TPS:.4f}\tbend\t{key}\t{rng}\t0\t{to_semis}")
            clock = onset + t(hold + 0.5)
        clock += t(0.3)

    # --- A: shape + duration law + symmetry, all inside the clamp -----------
    set_range(24)
    for semis in (2, -2, 6, -6, 24, -24):
        steps(f"A_step_{semis:+d}", 72, 24, semis)

    # --- B: does the ramp aim at the raw target or the clamped one? ---------
    # key 96 for downward (stays trackable at -70 st), key 36 for upward
    # (+70 st still lands under 4 kHz, no folding).
    set_range(72)
    for semis in (-40, -70):
        steps(f"B_down_{semis:+d}", 96, 72, semis)
    for semis in (40, 70):
        steps(f"B_up_{semis:+d}", 36, 72, semis)

    # --- C: the real gesture, controlled -------------------------------------
    # Alternating square wave like HueArme's, at four rates spanning its own
    # ~34 ms half-period. If the ramp model is right, the rendered excursion
    # should SHRINK as the alternation gets faster, because the ramp has less
    # time to travel before being re-aimed.
    set_range(64)
    for half_ms in (136, 68, 34, 17):
        onset = clock
        tr.bend(onset - t(0.02), 0, CENTRE)
        tr.note(onset, t(3.0), 0, 72, 100)
        man.append(f"C_square_{half_ms}ms\t0\t{onset / TPS:.4f}\t{onset / TPS:.4f}\tsquare\t72\t64\t-30\t+30")
        half = max(1, t(half_ms / 1000.0))
        pos = onset + t(0.3)
        k = 0
        while pos < onset + t(2.8):
            tr.bend(pos, 0, bend_val(30 if k % 2 == 0 else -30, 64))
            pos += half
            k += 1
        tr.bend(onset + t(2.9), 0, CENTRE)
        clock = onset + t(3.8)

    # --- D: is the EXPRESSION ramp the same law as the pitch ramp? -----------
    # Same 8-rep structure so the two are measured identically (probe 32 hinted
    # they differ: ours glides ~55-110 ms on CC11 but ~4-8 ms on bend).
    for to in (0, 64, 127):
        for rep in range(REPS):
            onset = clock
            tr.cc(onset - t(0.02), 0, 11, 127 if to != 127 else 0)
            tr.note(onset, t(1.6), 0, 72, 100)
            st = onset + t(0.5)
            tr.cc(st, 0, 11, to)
            man.append(f"D_cc11_to{to}\t{rep}\t{onset / TPS:.4f}\t{st / TPS:.4f}\tcc11\t72\t-\t{127 if to != 127 else 0}\t{to}")
            clock = onset + t(2.1)
        clock += t(0.3)

    tr.bend(clock, 0, CENTRE)
    return _write_manifest("33_pitch_ramp.mid", tr, man)



def p34_sfx_bank_identity():
    """Which sample does a bank-2 SFX program actually select, and is it pitched
    correctly? Settles a defect found by ear on real content.

    A hand-cut excerpt opens with ~14 s of bank 2 program 125 (`Car-Stop`),
    and our render of that passage tracks its
    fundamental **+693 to +960 cents above the reference** across 13 consecutive
    frames -- audible, and confirmed present with the pitch ramp disabled, so it
    is not the ramp. The driver rules out the obvious explanation: bank select is
    GS-gated at `device+0xf54` (`[A:0x1341e]`/`[A:0x13492]`) and that file sends
    a GS Reset 270 ms before its CC0=2, so the gate is open; and S3.1.2's
    three-tier fallback uses an exact-equality matcher with no masking or
    rounding, while bank 2 program 125 exists outright -- so no fallback is even
    invoked. Both engines should therefore play `Car-Stop`.

    Direct measurement on the real content could not settle it: `Car-Stop` is a
    tire-screech SFX where "the fundamental" is barely a well-defined quantity,
    and a spectral-shape correlation against our own bank-0 and bank-2 renders
    came back 0.475 vs 0.443 -- neither a match. Hence this probe, which
    replaces inference with a fingerprint.

    Sections:
      A. **No-GS-reset control, FIRST in the file** (GS mode is sticky once set,
         so this can only be tested before any GS Reset). Bank 2 + program 125
         with no GS Reset: if bank select is gated, this plays bank 0
         (`Helicopter`) in both engines.
      B. **Bank fingerprint sweep.** After a GS Reset, program 125 across ALL
         TEN banks it exists in (0 Helicopter, 1 Car-Engine, 2 Car-Stop,
         3 Car-Pass, 4 Car-Crash, 5 Siren, 6 Train, 7 Jetplane, 8 Starship,
         9 Burst Noise), one note each on the same key. This gives a reference
         fingerprint per patch, so whatever the isolate actually plays can be
         matched against all ten rather than guessed between two.
      C. **Key sweep on bank 2** at the three keys the isolate uses (55, 57, 69)
         plus 43/63/81 for span. `Car-Stop` is one region, keys 0-127,
         unityNote 63, fineTune 0 -- so if unity-note transposition is applied
         normally, key 63 plays at the sample's natural rate and each key steps
         one semitone. A flat response would mean SFX patches ignore key.
      D. **The same key sweep on bank 0** (`Helicopter`, one region, unityNote
         62, fineTune 22) so the two patches' pitch behaviour can be compared on
         identical keys.

    Deliberately plain: no pitch bend, no RPN, no expression, fixed velocity.
    The isolate confounds patch selection with bend and coarse tune; this
    separates them.
    """
    tr = Track()
    man = ["section\tonset_s\tCC0\tprogram\tkey\tgs_reset_before\texpected_patch"]
    clock = 0

    def note(tag, bank, prog, key, gs, expect, hold=2.0, gap=0.6):
        nonlocal clock
        tr.cc(clock, 0, 0, bank)
        tr.cc(clock, 0, 32, 0)
        tr.prog(clock + t(0.01), 0, prog)
        tr.cc(clock, 0, 7, 100)
        tr.cc(clock, 0, 11, 127)
        tr.cc(clock, 0, 10, 64)
        on = clock + t(0.05)
        tr.note(on, t(hold), 0, key, 100)
        man.append(f"{tag}\t{on / TPS:.4f}\t{bank}\t{prog}\t{key}\t{gs}\t{expect}")
        clock = on + t(hold + gap)

    # --- A: BEFORE any GS Reset, so the +0xf54 gate is closed -------------
    for key in (57, 69):
        note("A_nogs_bank2", 2, 125, key, "no", "Helicopter if gated, Car-Stop if not")
    clock += t(0.5)

    # --- everything below is after a GS Reset -----------------------------
    tr.sysex(clock, GS_RESET)
    clock += t(0.5)

    # --- B: fingerprint every bank program 125 exists in ------------------
    NAMES = {0: "Helicopter", 1: "Car-Engine", 2: "Car-Stop", 3: "Car-Pass",
             4: "Car-Crash", 5: "Siren", 6: "Train", 7: "Jetplane",
             8: "Starship", 9: "Burst-Noise"}
    for bank in range(10):
        note(f"B_bank{bank}", bank, 125, 57, "yes", NAMES[bank])
    clock += t(0.5)

    # --- C / D: key response on the two candidate patches -----------------
    for key in (43, 55, 57, 63, 69, 81):
        note("C_bank2_keys", 2, 125, key, "yes", "Car-Stop")
    clock += t(0.5)
    for key in (43, 55, 57, 63, 69, 81):
        note("D_bank0_keys", 0, 125, key, "yes", "Helicopter")
    return _write_manifest("34_sfx_bank_identity.mid", tr, man)


def p35_decay_keyfollow():
    """EG1 decay-time key-follow: how fast does a note decay as a function of
    KEY? Turns the one number this project inferred into a measurement.

169 of gm.dls's 235 instruments carry a usSource=3 (KEYNUMBER) ->
    usDestination=0x0207 (EG1 decay) connection [A:0x15868] -- and this project dropped all of them until 2026-07-26,
    so every acoustic patch decayed 3-5x too slowly (found on field/town.mid:
    a Steel-str.Gt chord that rings for the whole bar instead of fading).

    **Why not the Sine/Square carriers probes 24/25/27 use.** Those probes
    measure engine-wide laws (gain, pan) that apply to any patch, so they pick
    the cleanest tone available. This one measures a PER-INSTRUMENT gm.dls
    data row, so the instrument is not a free variable: `008:080` Sine Wave
    authors no EG1 decay at all (sentinel, no sustain row -- it holds flat
    until note-off, there is no decay segment to time), `001:080` Square and
    `000:081` Saw author 3.1 s but with sustain 968/1000, so their decay
    segment falls 0.28 dB and stops -- and none of the three carries the
    key-follow row in the first place. The usable set is the 92 instruments
    with the row AND a real decay AND sustain < 10%.

    Timbre matters less here than it would for a pitch probe: EG1 is a
    broadband multiplier and every region used below loops, so the sample
    contributes no amplitude change of its own past loop entry and total RMS
    in dB vs. time is a straight line whatever the harmonic content. Verified
    against our own render: r^2 0.993-1.000 on all 17 notes, fitted decay
    within 2-6% of the expected column below. Beating between partials adds
    ripple, not slope bias.

    What is now shipped but NOT measured is the NORMALIZATION. The driver
    stores the high word of lScale (the full-scale timecent offset) and its
    consumption code was then unrecovered, so the
    per-note offset `kf*key/128` is DLS-1's own convention taken on faith. The
    corpus prefers /128 over /127 by 0.13 dB, which is not a measurement.

    Sections, all one instrument per section, sustain=0, held long enough that
    the whole decay segment is visible, big gaps so each note decays into
    silence uncontaminated:

      A. **Piano 1** (program 0) at keys 24/36/48/60/72/84/96. Its own authored
         decay is 6386.5 tc (40.0 s) with kf = -3979, so under /128 the decay
         time should run 27.0 s (key 24) -> 6.1 s (key 96): a >4x span across
         the section, far larger than any plausible measurement error. Fit
         dB/s per note and the slope of log(decay_time) vs. key IS the
         normalization: /128 predicts -3979/128 = -31.1 tc per key, /127
         predicts -31.3, and a 60-relative reading predicts the same slope but
         a decay time that crosses 40.0 s at key 60 instead of key 0. The
         three differ in OFFSET, which is why the sweep needs low keys.
      B. **Steel-str.Gt** (program 25), the instrument the field defect was
         found on, at the same keys. Different base (5543.9 tc) and different
         kf (-4389) -- so it cross-checks that the model is per-instrument
         data and not a constant fitted to Piano 1.
      C. **Vibraphone** (program 11, kf = -4800, the most extreme value the
         file uses) at keys 48/72/96 -- 13.1 s down to 4.6 s. This is the
         closest thing to a sine carrier that carries the connection at all:
         near-sinusoidal, and the largest kf makes any normalization error
         the most visible. The section to read if A and B disagree.
         (`008:006` Coupled Hps. matches its kf on a 40.0 s base, a slightly
         wider absolute span, but needs a GS Reset for the bank-8 select and
         is a pluckier, less clean tone. Not used.)

    All three carry eg1_sustain = 0, so the decay segment runs all the way to
    silence and a straight-line dB/s fit measures it end to end.

    Deliberately plain: velocity fixed at 100, no CC, no bend, no pedal, no
    GS Reset (bank 0 programs need none). Nothing here should decay for the
    same reason twice.
    """
    tr = Track()
    clock = t(0.5)
    man = ["# 35_decay_keyfollow.mid -- expected EG1 decay per note onset",
           "# base_tc/kf are gm.dls art1 raw values; T = 2^((base_tc/65536 +"
           " kf*key/128)/1200) seconds",
           "# onset_seconds\tsection\tprogram\tkey\texpected_decay_s"]

    def section(tag, program, base_tc, kf, keys, hold, gap):
        nonlocal clock
        tr.prog(clock, 0, program)
        clock += t(0.05)
        for key in keys:
            secs = 2.0 ** ((base_tc / 65536.0 + kf * key / 128.0) / 1200.0)
            tr.note(clock, t(hold), 0, key, 100)
            man.append(f"{clock / TPS:.3f}\t{tag}\t{program}\t{key}\t{secs:.2f}")
            clock += t(hold + gap)
        clock += t(1.0)

    # hold 8s: long enough to see the whole decay on the fast (high) keys and
    # a clean straight-line dB/s fit on the slow (low) ones, without making
    # the probe minutes long. The note is still held at note-off on low keys
    # by design -- the decay segment is what is being measured, not release.
    section("A_piano", 0, 418578432, -3979, (24, 36, 48, 60, 72, 84, 96), 8.0, 1.5)
    section("B_steel_gt", 25, 363331584, -4389, (24, 36, 48, 60, 72, 84, 96), 8.0, 1.5)
    section("C_vibraphone", 11, 409796608, -4800, (48, 72, 96), 8.0, 1.5)
    return _write_manifest("35_decay_keyfollow.mid", tr, man)


def p36_kit_key_fallback():
    """What happens when a drum kit does not cover the key?

    `[A:0x147b7]`: FindInstrument itself walks the key ranges,
    so an instrument whose regions miss the note is rejected exactly like a
    locale mismatch and S3.1.2's NEXT fallback tier gets a turn. Only one
    instrument in gm.dls can exercise this: the SFX kit (drum, program 56)
    covers keys 39-84, while the other eight kits cover 27-87/88. So a drum
    note below 39 on an SFX-kit part should fall through to the Standard kit
    (locale 0x80000000), not go silent.

    This project dropped the note until 2026-07-26 (it matched the kit on
    locale alone, then found no region and bailed), which is audible on
    field/HueArme-Weekend.mid: channel 9 is an SFX-kit rhythm part playing
    key 35 in a velocity-ramped roll, and the whole roll was missing.

    Sections, all on channel 10 (the default rhythm part, no sysex needed):
      A. Standard kit (program 0), keys 35 and 40 -- the control: what the
         Standard kit's own regions for those keys sound like.
      B. SFX kit (program 56), key 40 -- in range, plays the SFX kit region.
      C. SFX kit, keys 27/35/38 -- all below the kit's 39 floor. Each must
         sound, and must be identical to the same key in section A.
      D. SFX kit, key 85 -- above the kit's 84 ceiling, the other side of the
         same gap; Standard kit covers up to 87.
      E. SFX kit, key 25 -- outside BOTH kits (Standard starts at 27, S2.11).
         Retry #2 fails, and locale &= 0x7f then asks for melodic program 56
         (Trumpet), which covers the full keyboard. So this one settles
         whether the third tier really is reached from a drum locale: silence
         means the fallback chain stops at the drum tier, a pitched trumpet
         note means it does not.

    RESOLVED by the reference render `[M]` (unit.c S3.1.2): C and D all
    sound (and C's key 35 is spectrally the Standard kit's own key-35 region,
    log-spectrum r = 0.988), while E is digital silence -- the chain does
    stop at the drum tier.
    """
    tr = Track()
    tr.sysex(0, GS_RESET)
    clock = t(1.0)

    def hit(prog, key):
        nonlocal clock
        tr.prog(clock, 9, prog)
        tr.note(clock + t(0.1), t(0.8), 9, key, 120)
        clock += t(2.0)

    for key in (35, 40):        # A
        hit(0, key)
    hit(56, 40)                 # B
    for key in (27, 35, 38):    # C
        hit(56, key)
    hit(56, 85)                 # D
    hit(56, 25)                 # E
    return write("36_kit_key_fallback.mid", tr)


def p37_rac_volume_order():
    """Does CC121 (Reset All Controllers) clobber a Channel Volume set at the
    SAME timestamp, and does it clobber one set at an EARLIER timestamp?

    The CC121 handler `[A:0x1351f]` re-schedules Volume=100 into the driver's
    timestamp-keyed controller queues, which this project does not have:
    dispatch_cc writes the
    reset straight over c->volume. tests/warm-echo.mid says that is wrong.
    Both its tracks send CC7 (76 on ch0, 52 on ch1) and then CC121 at tick 0,
    and the reference keeps 76/52: neutering the two CC121 events in the file
    moves this project's two-channel-vs-one-channel level from +4.84 dB to
    -2.12 dB against the reference's -2.03 dB `[M]`.

    That is one data point, and it only constrains the same-tick case. Two
    rules fit it and disagree elsewhere: "CC121 never touches volume" (one
    #define) versus "an explicit CC at the same timestamp wins, a later CC121
    still resets" (needs the event tick threaded into synth.c). Nothing in the
    corpus separates them -- every CC121 in field/ and tests/ sits at tick 0.

    Sine (bank 8 prog 80) for a clean RMS, one note per case, CC7 and CC11
    parked at 127 well before each case so the case's own events are the only
    ones at their tick:
      A. CC7=40, note                     -- control: what 40 sounds like.
      B. CC7=100, note                    -- control: what CC121's reset value
                                             sounds like.
      C. CC7=40 then CC121, same tick     -- warm-echo's shape. A or B?
      D. CC121 then CC7=40, same tick     -- reverse order, must be A.
      E. CC7=40, CC121 50 ms later        -- the discriminator.
      F. CC7=40, CC121 500 ms later       -- same, far past any one buffer, so
                                             a "later" that lands in the same
                                             flush cannot explain E.
      G. CC11=40 then CC121, same tick    -- does the rule generalize past
                                             CC7? Expression shares CC7's
                                             curve (S3.5), so A is its control.

    Read it as: each case's RMS equals A's or B's. C=A is expected (it is what
    warm-echo already measured). E/F=B means the timestamp rule; E/F=A means
    the blanket one. G=C means the fix belongs in the reset, not in CC7.
    """
    tr = Track()
    tr.sysex(0, GS_RESET)
    man = ["# 37_rac_volume_order.mid", "# onset\tcase\tevents"]
    clock = t(0.5)

    def case(name, events, build):
        nonlocal clock
        tr.cc(clock, 0, 0, 8); tr.cc(clock, 0, 32, 0); tr.prog(clock, 0, 80)
        tr.cc(clock, 0, 7, 127); tr.cc(clock, 0, 11, 127)
        base = clock + t(0.5)
        build(base)
        onset = clock + t(1.2)
        tr.note(onset, t(0.45), 0, 60, 100)
        man.append(f"{onset / TPS:.3f}\t{name}\t{events}")
        clock += t(2.2)

    case("A_cc7_40", "cc7=40", lambda b: tr.cc(b, 0, 7, 40))
    case("B_cc7_100", "cc7=100", lambda b: tr.cc(b, 0, 7, 100))
    case("C_same_tick", "cc7=40, cc121 @same tick",
         lambda b: (tr.cc(b, 0, 7, 40), tr.cc(b, 0, 121, 0)))
    case("D_same_tick_rev", "cc121, cc7=40 @same tick",
         lambda b: (tr.cc(b, 0, 121, 0), tr.cc(b, 0, 7, 40)))
    case("E_later_50ms", "cc7=40, cc121 +50ms",
         lambda b: (tr.cc(b, 0, 7, 40), tr.cc(b + t(0.05), 0, 121, 0)))
    case("F_later_500ms", "cc7=40, cc121 +500ms",
         lambda b: (tr.cc(b, 0, 7, 40), tr.cc(b + t(0.5), 0, 121, 0)))
    case("G_cc11_same_tick", "cc11=40, cc121 @same tick",
         lambda b: (tr.cc(b, 0, 11, 40), tr.cc(b, 0, 121, 0)))
    return _write_manifest("37_rac_volume_order.mid", tr, man)


def p38_same_tick_locale():
    """Which locale does a note-on take when several Bank Select + Program
    Change pairs land on ONE channel at ONE tick?

    field/serum_opening.mid does this 64 times, Kot_and_A64-GENERAL_SERUM 1603
    times, CrystalOscillator 947: three tracks share a channel, each sends its
    own CC0/CC32/PC and then its own note, all on the same tick, to layer three
    patches. No probe or test in the corpus sends more than one Program Change
    per channel per tick, so nothing measures what the driver does with it.

    Two rules fit everything measured so far and disagree here:
      stream order -- each note-on takes the locale latched by the most recent
                      Program Change BEFORE it in the byte stream. What
                      smf.c's stable (abs_tick, seq) sort produces today.
      look-ahead   -- 0x16c50: the Bank/Program queue is
                      timestamp-keyed and a read ahead of the periodic flush
                      sees the LAST value scheduled at that timestamp, so every
                      note-on at the tick takes the last PC of the group.

    On serum_opening's 10.100s section the two answers are 24 dB apart in the
    2-11kHz bands, and each is badly wrong where the other is right, so the
    file itself cannot settle it -- hence this probe.

    Patches are picked for maximum spectral distance: 001:080 (GS Square Wave,
    harmonics to Nyquist) vs 008:080 (GS Sine Wave, fundamental only) vs
    000:079 (Ocarina, dull and breathy). Roughly 40 dB apart above 2 kHz, so
    one band-energy read per case answers it. No bend and no tune: the pitch
    aliasing serum_opening also exercises is a separate question.

      A/B/C  one patch each, nothing else at the tick -- the three controls
             every other case is read against.
      D      both PC groups, THEN the note. Both rules say sine; if this reads
             square the second group is not landing at all and E/F/G/H mean
             nothing.
      E      square group, note, sine group -- all one tick.
             stream -> square, look-ahead -> sine.  THE DISCRIMINATOR.
      F      E mirrored (sine group, note, square group), so the answer cannot
             be "the brighter one wins" or a level artifact.
      G      serum_opening's actual shape: three groups, each with its own
             note, on three DIFFERENT keys so nothing chokes anything.
             stream -> square/sine/ocarina, look-ahead -> ocarina x3.
      H      G on ONE key. Two questions at once, deliberately: it is the exact
             shape of serum_opening's key-3 gesture. Under same-key retrigger
             choke only the last note survives; without it all three sum. Read
             H against G's total, then against C.
    """
    tr = Track()
    tr.sysex(0, GS_RESET)
    man = ["# 38_same_tick_locale.mid",
           "# onset\tcase\tkey\texpect_stream_order\texpect_look_ahead\tevents"]
    clock = t(0.5)
    SQ, SI, OC = (1, 80, "square"), (8, 80, "sine"), (0, 79, "ocarina")
    DUR, GAP = t(0.6), t(1.5)

    def sel(tick, bp):
        """One track's CC0/CC32/PC group, in the order a sequencer emits it."""
        tr.cc(tick, 0, 0, bp[0])
        tr.cc(tick, 0, 32, 0)
        tr.prog(tick, 0, bp[1])

    def case(name, events, build):
        """build(tick, note) -- note(key, stream, lookahead) records one onset.
        Every event a case emits sits on the single tick `clock`."""
        nonlocal clock
        def note(key, stream, lookahead):
            tr.note(clock, DUR, 0, key, 100)
            man.append(f"{clock / TPS:.3f}\t{name}\t{key}\t{stream}\t{lookahead}\t{events}")
        build(clock, note)
        clock += GAP

    case("A_ctl_square", "cc0=1 pc80, on60",
         lambda k, note: (sel(k, SQ), note(60, "square", "square")))
    case("B_ctl_sine", "cc0=8 pc80, on60",
         lambda k, note: (sel(k, SI), note(60, "sine", "sine")))
    case("C_ctl_ocarina", "cc0=0 pc79, on60",
         lambda k, note: (sel(k, OC), note(60, "ocarina", "ocarina")))
    case("D_note_after_both", "cc0=1 pc80, cc0=8 pc80, on60",
         lambda k, note: (sel(k, SQ), sel(k, SI), note(60, "sine", "sine")))
    case("E_note_between", "cc0=1 pc80, on60, cc0=8 pc80",
         lambda k, note: (sel(k, SQ), note(60, "square", "sine"), sel(k, SI)))
    case("F_note_between_rev", "cc0=8 pc80, on60, cc0=1 pc80",
         lambda k, note: (sel(k, SI), note(60, "sine", "square"), sel(k, SQ)))
    case("G_three_keys", "cc0=1 pc80 on48, cc0=8 pc80 on60, cc0=0 pc79 on72",
         lambda k, note: (sel(k, SQ), note(48, "square", "ocarina"),
                          sel(k, SI), note(60, "sine", "ocarina"),
                          sel(k, OC), note(72, "ocarina", "ocarina")))
    case("H_three_same_key", "cc0=1 pc80 on60, cc0=8 pc80 on60, cc0=0 pc79 on60",
         lambda k, note: (sel(k, SQ), note(60, "square+sine+ocarina if no choke, else ocarina", "ocarina"),
                          sel(k, SI), note(60, "(same onset)", "(same onset)"),
                          sel(k, OC), note(60, "(same onset)", "(same onset)")))
    return _write_manifest("38_same_tick_locale.mid", tr, man)


def p39_high_sustain_decay():
    """Does EG1 decay actually stop at a HIGH authored sustain level, or does
    the real driver keep decaying past it?

    field/onestop_extract.mid (a three-note "Atmosphere", program 99, chord)
    measures ~13.3 dB spectral residual against the reference, and a
    150ms-smoothed RMS envelope shows the reference decaying ~6 dB over
    roughly 700ms-1400ms while this project's render stays flat. That chord
    is a bad isolation target: notes 55/56 are a semitone apart and beat at
    ~11.5 Hz, and the alignment is only 50ms-hop coarse, so either could be
    manufacturing the appearance of a difference.

    This project's own EG1 decay model (`voice.c`'s `exp_coef_scaled` /
    `ENV_DECAY`, DLS-1's "decays toward the authored sustain level" reading)
    caps how much a note CAN decay at 20*log10(sustain_permille/1000) --
    for Atmosphere's 88.2% sustain that ceiling is 1.09 dB, reached (to
    within the 0.0005 snap threshold) at 3.80s per the current model. A
    6 dB drop is not reachable under this model at ANY sustain level this
    high, on ANY held note, regardless of chord/beating/alignment -- so if
    the reference shows one, on a SINGLE held note, in isolation, that is
    this project's decay model being wrong, not a measurement artifact.

    **First cut of this probe (sections A/B/C) confirmed exactly that**: on
    isolated single notes, Atmosphere and Soundtrack both showed a real
    ~5-6 dB drop landing well inside 1s, then a hard plateau, while this
    project's render stayed flat as the current model predicts. The
    surprising part: that ~5-6 dB / ~0.5-1s shape looked roughly the SAME
    for both instruments despite a 2.3x difference in authored decay_tc
    (7.74s vs 18.09s) and different sustain (88.2% vs 93.0%) -- i.e. the
    real decay did NOT visibly scale with the DLS data the way the model
    assumes it must.

    That "looks the same across two instruments" is exactly the shape of
    the trap this project already fell into once: a `DECAY_RATE_MULT=2.85`
    fit against ONE instrument (Piano note 60) looked like a universal
    constant and turned out to be key-follow specific to that single note,
    regressing every other instrument when shipped as a global multiplier
    (a 2.85 decay multiplier fit to probe 04 lost on the corpus). Two
    agreeing data points is not enough to rule out coincidence here either.
    This cut adds more instruments spanning a wider decay_tc/sustain range,
    plus a velocity sweep, specifically to separate "real, roughly-fixed
    driver behavior" from "these two just happened to land close."

    gm.dls authors this shape (high sustain, long decay_tc, i.e. "the decay
    segment is nearly a no-op" under the current model) on 51+ instruments,
    so this is a systemic question, not an Atmosphere-specific one. Seven
    sections, none carrying a usSource=3 (KEYNUMBER) decay key-follow row
    (probe 35 already owns that variable) and none carrying an LFO->
    ATTENUATION connection (ruled out separately for Atmosphere already):

      A. **Square Wave** (program 80): decay 3.10s -> 96.8% sustain, ceiling
         0.28 dB, model snap 1.16s. Synthetic, no natural sample dynamics --
         the cleanest RMS proxy, and the smallest ceiling of the set.
      B. **Atmosphere** (program 99): decay 7.74s -> 88.2% sustain, ceiling
         1.09 dB, model snap 3.80s. The instrument onestop_extract actually
         uses -- already measured showing the ~6 dB/~1s real decay.
      C. **Soundtrack** (program 97): decay 18.09s -> 93.0% sustain, ceiling
         0.63 dB, model snap 8.05s -- already measured showing a ~4.5-4.8 dB/
         ~1-1.5s real decay, i.e. a similar shape despite 2.3x the decay_tc.
      D. **Atmosphere velocity sweep**, key 60 fixed, velocity 20/50/80/110:
         does the ~6 dB drop's SIZE or TIMING track velocity (a velocity-
         triggered attack-overshoot-and-settle would), or is it velocity-
         independent like the rest of this data suggests?
      E. **Solo Vox** (program 85): decay 4.00s -> 98.2% sustain, ceiling
         0.16 dB, model snap 1.29s -- the smallest ceiling of the new
         instruments, closest in decay_tc to Atmosphere.
      F. **Synth Brass1** (program 62): decay 9.70s -> 94.6% sustain,
         ceiling 0.48 dB, model snap 4.09s -- between Atmosphere and
         Soundtrack on both axes.
      G. **Halo Pad** (program 94): decay 10.27s -> 91.4% sustain, ceiling
         0.78 dB, model snap 4.76s -- closest sustain% to Atmosphere at a
         decay_tc closer to Synth Brass1/Soundtrack, to help separate which
         axis (if either) the real decay actually tracks.

    Keys 48/60/72 per key-sweep section (no key-follow authored, so the
    model predicts identical curves across them); velocity fixed at 100
    except section D. No CC, no bend, no pedal, no GS Reset (all bank 0,
    none need it) -- nothing here should decay for any reason this probe
    isn't naming.
    """
    tr = Track()
    clock = t(0.5)
    man = ["# 39_high_sustain_decay.mid -- expected EG1 decay per note onset",
           "# current-model columns are this project's own exp_coef_scaled "
           "prediction, not a measurement",
           "# onset_seconds\tsection\tprogram\tkey\tvel\tdecay_s\tsustain_pct\t"
           "model_max_drop_db\tmodel_snap_s"]

    def section(tag, program, keys, decay_s, sustain_pct, max_drop_db, snap_s,
                hold, gap, vel=100):
        nonlocal clock
        tr.prog(clock, 0, program)
        clock += t(0.05)
        for key in keys:
            tr.note(clock, t(hold), 0, key, vel)
            man.append(f"{clock / TPS:.3f}\t{tag}\t{program}\t{key}\t{vel}\t{decay_s:.2f}\t"
                       f"{sustain_pct:.1f}\t{max_drop_db:.2f}\t{snap_s:.2f}")
            clock += t(hold + gap)
        clock += t(1.0)

    def velocity_section(tag, program, key, vels, decay_s, sustain_pct,
                          max_drop_db, snap_s, hold, gap):
        nonlocal clock
        tr.prog(clock, 0, program)
        clock += t(0.05)
        for vel in vels:
            tr.note(clock, t(hold), 0, key, vel)
            man.append(f"{clock / TPS:.3f}\t{tag}\t{program}\t{key}\t{vel}\t{decay_s:.2f}\t"
                       f"{sustain_pct:.1f}\t{max_drop_db:.2f}\t{snap_s:.2f}")
            clock += t(hold + gap)
        clock += t(1.0)

    section("A_square", 80, (48, 60, 72), 3.10, 96.8, -0.28, 1.16, 2.0, 2.0)
    section("B_atmosphere", 99, (48, 60, 72), 7.74, 88.2, -1.09, 3.80, 5.0, 2.0)
    section("C_soundtrack", 97, (48, 60, 72), 18.09, 93.0, -0.63, 8.05, 9.0, 2.0)
    velocity_section("D_atmosphere_velocity", 99, 60, (20, 50, 80, 110),
                      7.74, 88.2, -1.09, 3.80, 4.0, 2.0)
    section("E_solovox", 85, (48, 60, 72), 4.00, 98.2, -0.16, 1.29, 3.0, 2.0)
    section("F_synthbrass1", 62, (48, 60, 72), 9.70, 94.6, -0.48, 4.09, 6.0, 2.0)
    section("G_halopad", 94, (48, 60, 72), 10.27, 91.4, -0.78, 4.76, 6.0, 2.0)
    return _write_manifest("39_high_sustain_decay.mid", tr, man)


def p40_same_tick_bank():
    """Probe 38's leftover: when several Bank Select MSBs land on ONE channel at
    ONE tick, which one does the note-on's locale end up carrying?

    Probe 38 settled the PROGRAM half -- 7 of its 8 cases say a note-on takes
    the LAST Program Change at its tick, not the one before it in the stream
    (case G, serum_opening's own shape, collapses to ocarina x3 and sits 19 dB
    from what stream order renders). Its case F did not fit: F and E have the
    same program in both groups and differ only in bank order, `1->8` versus
    `8->1`, and the reference came out bank 8 BOTH times (3.75 dB from the
    008:080 control, 28.6 from 001:080). No order-sensitive rule produces that.
    Bank 1 is definitely honoured -- 38's case A matches 001:080 at 4.36 dB
    against 27.24 for 000:080 -- so it is not bank 1 being dropped.

    Everything here is program 80, so the program half is held fixed and only
    the bank moves. Three banks of program 80 exist in gm.dls and are mutually
    27 dB or more apart: 000:080 Square Lead, 001:080 Square Wave (both bright,
    but not each other), 008:080 Sine Wave (fundamental only). The manifest
    carries one column per candidate rule, so the reference render reads
    straight off as a truth table:

      stream    each note-on takes the locale latched by the most recent PC
                before it in the byte stream. What smf.c does today.
      last_pc   look-ahead: the note takes the last PC of the tick, each PC
                having latched whatever bank byte was live when it ran.
                Probe 38's 7-of-8 answer.
      high      order-independent: the highest bank MSB that reached a PC at
                that tick wins. The only simple rule that also fits 38's F.

      A/B/C  one bank each -- the three controls.
      D/E    both PC groups then the note, in both bank orders. E is the
             discriminator: stream and last_pc say 001:080, high says 008:080.
      F/G    note between the two groups, both orders. These re-measure 38's
             E and F in a fresh capture; F separates stream from last_pc.
      H/I    same as D/E but the second bank is 0, so "high" is separated from
             the other two in the other direction (bank 0 arriving last).
      J/K    two Bank Selects, ONE Program Change, note last. No PC queue tie
             at all -- this isolates the bank byte itself. K is the one that
             matters: stream and last_pc both say 001:080, high says 008:080.
      L/M    a Bank Select AFTER the note with NO program change following it.
             Every rule above says the note keeps the first group's bank. If
             the reference disagrees, the bank is being read live at voice
             render time and all three rules are wrong.
    """
    tr = Track()
    tr.sysex(0, GS_RESET)
    man = ["# 40_same_tick_bank.mid",
           "# onset\tcase\tevents\texpect_stream\texpect_last_pc\texpect_high"]
    clock = t(0.5)
    DUR, GAP = t(0.6), t(1.5)
    NAME = {0: "squarelead(000:080)", 1: "squarewave(001:080)", 8: "sine(008:080)"}

    def bank(tick, msb):
        tr.cc(tick, 0, 0, msb)
        tr.cc(tick, 0, 32, 0)

    def sel(tick, msb):
        """One track's CC0/CC32/PC group, in the order a sequencer emits it."""
        bank(tick, msb)
        tr.prog(tick, 0, 80)

    def case(name, events, build, stream, last_pc, high):
        nonlocal clock
        build(clock, lambda k: tr.note(k, DUR, 0, 60, 100))
        man.append(f"{clock / TPS:.3f}\t{name}\t{events}\t"
                   f"{NAME[stream]}\t{NAME[last_pc]}\t{NAME[high]}")
        clock += GAP

    case("A_ctl_b0", "b0 pc80, on60",
         lambda k, note: (sel(k, 0), note(k)), 0, 0, 0)
    case("B_ctl_b1", "b1 pc80, on60",
         lambda k, note: (sel(k, 1), note(k)), 1, 1, 1)
    case("C_ctl_b8", "b8 pc80, on60",
         lambda k, note: (sel(k, 8), note(k)), 8, 8, 8)
    case("D_18_note_last", "b1 pc80, b8 pc80, on60",
         lambda k, note: (sel(k, 1), sel(k, 8), note(k)), 8, 8, 8)
    case("E_81_note_last", "b8 pc80, b1 pc80, on60",
         lambda k, note: (sel(k, 8), sel(k, 1), note(k)), 1, 1, 8)
    case("F_18_note_mid", "b1 pc80, on60, b8 pc80",
         lambda k, note: (sel(k, 1), note(k), sel(k, 8)), 1, 8, 8)
    case("G_81_note_mid", "b8 pc80, on60, b1 pc80",
         lambda k, note: (sel(k, 8), note(k), sel(k, 1)), 8, 1, 8)
    case("H_10_note_last", "b1 pc80, b0 pc80, on60",
         lambda k, note: (sel(k, 1), sel(k, 0), note(k)), 0, 0, 1)
    case("I_80_note_last", "b8 pc80, b0 pc80, on60",
         lambda k, note: (sel(k, 8), sel(k, 0), note(k)), 0, 0, 8)
    case("J_18_one_pc", "b1, b8, pc80, on60",
         lambda k, note: (bank(k, 1), sel(k, 8), note(k)), 8, 8, 8)
    case("K_81_one_pc", "b8, b1, pc80, on60",
         lambda k, note: (bank(k, 8), sel(k, 1), note(k)), 1, 1, 8)
    case("L_1_then_b8_no_pc", "b1 pc80, on60, b8 (no pc)",
         lambda k, note: (sel(k, 1), note(k), bank(k, 8)), 1, 1, 1)
    case("M_8_then_b1_no_pc", "b8 pc80, on60, b1 (no pc)",
         lambda k, note: (sel(k, 8), note(k), bank(k, 1)), 8, 8, 8)
    return _write_manifest("40_same_tick_bank.mid", tr, man)


def p41_sustain_decay_curve():
    """Probe 39's leftover: what does the real EG1 "decay" against a HIGH
    authored sustain level actually correlate with?

    Probe 39 (sections A-G) measured a real, on-the-record decay this
    project's model does not predict -- on isolated single notes, no chord,
    no beating, velocity-independent (section D's 20/50/80/110 sweep on
    Atmosphere all showed the same ~7.4 dB drop), key-independent (identical
    across keys 48/60/72 on every clean instrument). But its magnitude did
    NOT track the thing the current model uses, authored decay_tc: Synth
    Brass1 (9.7s) showed only ~2 dB while Atmosphere (7.74s, a SHORTER
    authored decay) showed ~7 dB. What it DID track, monotonically, across
    all four clean instruments measured, was authored sustain%:

        Solo Vox      98.2% sustain, decay_tc  4.0s -> ~0 dB real decay
        Synth Brass1  94.6% sustain, decay_tc  9.7s -> ~2 dB
        Soundtrack    93.0% sustain, decay_tc 18.1s -> ~4.5-5.2 dB
        Atmosphere    88.2% sustain, decay_tc  7.7s -> ~7 dB

    decay_tc spans 4.0s-18.1s across those four with no corresponding trend;
    sustain% alone predicts the ordering exactly. This probe is a denser
    sweep across sustain% (98.6% down to 90.4%) specifically to see whether
    the real decay-dB is a function of sustain% alone (and if so, fit its
    shape -- squared? some other dB-domain transform of the linear reading
    the disassembly currently backs?) or whether it is confounded by
    something else once more instruments are added.

    Two instruments deliberately share a sustain% with a probe-39 instrument
    at a DIFFERENT decay_tc, to directly test decay_tc-independence rather
    than assume it from the trend so far:
      Syn.Strings2 (98.2% sustain, decay_tc 2.43s) vs. Solo Vox (98.2%,
        4.0s, probe 39E) -- same sustain, different decay_tc: matching
        results here confirms decay_tc really is irrelevant at this end of
        the curve too, not just coincidentally at the two points already in
        hand.
      5th Saw Wave (94.6% sustain, decay_tc 12.17s) vs. Synth Brass1 (94.6%,
        9.7s, probe 39F) -- same test at the ~2 dB point.

    Halo Pad (probe 39G) is dropped: several-dB oscillation throughout its
    hold in BOTH ref and slop, no clean settle -- almost certainly an
    inherent (non-CC1-gated) LFO->PITCH connection on that patch that this
    probe did not screen for. Every instrument below was checked for
    usSource=1/usDestination=0x0003/usControl=0 (inherent vibrato) and
    excluded if its depth exceeded a few cents, in addition to the existing
    screens (no usSource=3 key-follow, no usSource=1/usDestination=0x0001
    tremolo).

    One note per instrument, key 60, velocity 100 (key- and velocity-
    independence are probe 39's finding, not re-assumed -- if this cut
    disagrees on either axis for any instrument, that itself is new
    information). Held past each instrument's own real-decay settle window
    (probe 39: consistently under ~1.5s) with margin, sorted by sustain%
    descending so the manifest reads as a curve top to bottom:

      A. Syn.Calliope  (program 82): 98.6% sustain, decay_tc 2.30s
      B. Syn.Strings2  (program 51): 98.2% sustain, decay_tc 2.43s
      C. Star Theme    (program 103): 97.8% sustain, decay_tc 3.31s
      D. Charang       (program 84): 95.8% sustain, decay_tc 14.60s
      E. 5th Saw Wave  (program 86): 94.6% sustain, decay_tc 12.17s
      F. Polysynth     (program 90): 94.1% sustain, decay_tc 10.87s
      G. Bowed Glass   (program 92): 90.9% sustain, decay_tc 11.76s
      H. Echo Drops    (program 102): 90.4% sustain, decay_tc 2.10s

    No CC, no bend, no pedal, no GS Reset (all bank 0) -- nothing here
    should decay for any reason this probe isn't naming.
    """
    tr = Track()
    clock = t(0.5)
    man = ["# 41_sustain_decay_curve.mid -- expected EG1 decay per note onset",
           "# current-model columns are this project's own exp_coef_scaled "
           "prediction, not a measurement",
           "# onset_seconds\tsection\tprogram\tkey\tvel\tdecay_s\tsustain_pct\t"
           "model_max_drop_db\tmodel_snap_s"]

    def note(tag, program, decay_s, sustain_pct, max_drop_db, snap_s, hold, gap):
        nonlocal clock
        tr.prog(clock, 0, program)
        clock += t(0.05)
        tr.note(clock, t(hold), 0, 60, 100)
        man.append(f"{clock / TPS:.3f}\t{tag}\t{program}\t60\t100\t{decay_s:.2f}\t"
                   f"{sustain_pct:.1f}\t{max_drop_db:.2f}\t{snap_s:.2f}")
        clock += t(hold + gap + 1.0)

    note("A_syncalliope", 82, 2.30, 98.6, -0.12, 0.69, 3.0, 2.0)
    note("B_synstrings2", 51, 2.43, 98.2, -0.16, 0.78, 3.0, 2.0)
    note("C_startheme", 103, 3.31, 97.8, -0.19, 1.13, 3.0, 2.0)
    note("D_charang", 84, 14.60, 95.8, -0.37, 5.82, 3.5, 2.0)
    note("E_5thsawwave", 86, 12.17, 94.6, -0.48, 5.13, 3.5, 2.0)
    note("F_polysynth", 90, 10.87, 94.1, -0.53, 4.67, 3.5, 2.0)
    note("G_bowedglass", 92, 11.76, 90.9, -0.83, 5.51, 3.5, 2.0)
    note("H_echodrops", 102, 2.10, 90.4, -0.88, 0.99, 3.0, 2.0)
    return _write_manifest("41_sustain_decay_curve.mid", tr, man)


def p42_multitrack_same_tick():
    """Is the same-tick Bank/Program look-ahead scoped to the MERGED tick, or
    to each TRACK separately?

    Probe 40 settled the rule 13/13: a note-on takes the last Program Change at
    its tick. Every probe that measured it, though, is FORMAT 0 -- one track,
    one stream, no scoping question to ask. Every real file that exercises the
    rule is format 1 with the groups on SEPARATE tracks (serum_opening 64
    same-tick multi-PC groups, GENERAL_SERUM 1603, CrystalOscillator 947), and
    there the merged-tick reading is measurably wrong on half the gestures:
    serum_opening's alternating Type A gesture improves 28.3 -> 10.8 dB rms
    band error under it, while Type B goes 19.8 -> 25.2 and flips from ~20 dB
    too loud to ~25 dB too quiet. Type B's tick ends with `CC0=8, PC80`, so a
    merged look-ahead sends every note there to 008:080 sine and the reference
    plainly carries something brighter.

    Per-track scoping would explain that and no format-0 probe could see it.
    So: probe 40's discriminating cases restaged across tracks, plus
    serum_opening's Type B shape reproduced structurally.

    Format 1. Track 0 is tempo + GS Reset only; tracks 1-3 are the "voices",
    each sending its own CC0/CC32/PC and its own note, all on one tick, the way
    a sequencer lays out layered parts. Program 80 throughout except case D and
    J, so the bank is what moves; 000:080 / 001:080 / 008:080 / 001:081 are all
    mutually 28 dB or more apart.

      A/B/C/D  controls, one group on track 1. D (001:081 Saw Wave) exists for
               case J.
      E        t1 group, t2 group + note. Both readings say the note takes
               t2's bank -- the control proving that splitting across tracks
               does not by itself change anything.
      F        t1 group + NOTE, t2 group. merged -> t2's 008:080,
               per-track -> t1's own 001:080.  THE DISCRIMINATOR.
      G        F mirrored, so the answer cannot be "the duller one wins".
      H        three tracks, three groups, three DIFFERENT keys -- exactly
               serum_opening's Type A layering. merged -> all three notes on
               t3's 000:080, per-track -> 001:080/008:080/000:080 on
               48/60/72. Roughly 30 dB apart either way.
      I        H on one key: choke leaves the last note-on whichever way the
               scoping goes, so this is a control on the choke surviving a
               track split, not a discriminator.
      J        serum_opening's Type B shape: two tracks each playing both keys,
               a third playing only the upper one, the last group being the
               sine. merged -> sine on both keys (the too-quiet render),
               per-track -> 001:081 saw on key 60, 008:080 sine on key 72.

    How to read it: F and G are single notes and label straight off the A-D
    controls. H and J are multi-note cases, so a single-note control cannot
    label them -- compare those against the two candidate RENDERS instead,
    which is exact, because the two hypotheses are just this file rendered with
    SMF_BANKPROG_LOOKAHEAD 1 and 0:

        cc -std=c11 -O2 -Isrc -DSMF_BANKPROG_LOOKAHEAD=0 -o /tmp/off \\
           src/cli.c src/engine/*.c

    Under the shipped build F/G/H/J read 008:080 / 001:080 / 000:080 x3 /
    008:080; with the flag off they read 001:080 / 008:080 / a three-patch mix
    / 001:081-ish. Every other case is identical between the two, which is the
    check that the split itself changed nothing.
    """
    t0, t1, t2, t3 = Track(), Track(), Track(), Track()
    t0.sysex(0, GS_RESET)
    man = ["# 42_multitrack_same_tick.mid",
           "# onset\tcase\tkey\ttracks\texpect_merged_tick\texpect_per_track"]
    clock = t(0.5)
    DUR, GAP = t(0.6), t(1.5)
    NAME = {(0, 80): "squarelead(000:080)", (1, 80): "squarewave(001:080)",
            (8, 80): "sine(008:080)", (1, 81): "saw(001:081)"}

    def group(tr, tick, msb, prog=80):
        tr.cc(tick, 0, 0, msb)
        tr.cc(tick, 0, 32, 0)
        tr.prog(tick, 0, prog)

    def case(name, tracks_desc, build, notes):
        """notes: list of (key, merged, per_track), one row per note onset."""
        nonlocal clock
        build(clock)
        for key, merged, per_track in notes:
            man.append(f"{clock / TPS:.3f}\t{name}\t{key}\t{tracks_desc}\t"
                       f"{NAME[merged]}\t{NAME[per_track]}")
        clock += GAP

    case("A_ctl_b0", "t1: b0 pc80 on60",
         lambda k: (group(t1, k, 0), t1.note(k, DUR, 0, 60, 100)),
         [(60, (0, 80), (0, 80))])
    case("B_ctl_b1", "t1: b1 pc80 on60",
         lambda k: (group(t1, k, 1), t1.note(k, DUR, 0, 60, 100)),
         [(60, (1, 80), (1, 80))])
    case("C_ctl_b8", "t1: b8 pc80 on60",
         lambda k: (group(t1, k, 8), t1.note(k, DUR, 0, 60, 100)),
         [(60, (8, 80), (8, 80))])
    case("D_ctl_b1_p81", "t1: b1 pc81 on60",
         lambda k: (group(t1, k, 1, 81), t1.note(k, DUR, 0, 60, 100)),
         [(60, (1, 81), (1, 81))])
    case("E_note_on_t2", "t1: b1 pc80 | t2: b8 pc80 on60",
         lambda k: (group(t1, k, 1), group(t2, k, 8), t2.note(k, DUR, 0, 60, 100)),
         [(60, (8, 80), (8, 80))])
    case("F_note_on_t1", "t1: b1 pc80 on60 | t2: b8 pc80",
         lambda k: (group(t1, k, 1), t1.note(k, DUR, 0, 60, 100), group(t2, k, 8)),
         [(60, (8, 80), (1, 80))])
    case("G_note_on_t1_rev", "t1: b8 pc80 on60 | t2: b1 pc80",
         lambda k: (group(t1, k, 8), t1.note(k, DUR, 0, 60, 100), group(t2, k, 1)),
         [(60, (1, 80), (8, 80))])
    case("H_three_tracks_3keys", "t1: b1 pc80 on48 | t2: b8 pc80 on60 | t3: b0 pc80 on72",
         lambda k: (group(t1, k, 1), t1.note(k, DUR, 0, 48, 100),
                    group(t2, k, 8), t2.note(k, DUR, 0, 60, 100),
                    group(t3, k, 0), t3.note(k, DUR, 0, 72, 100)),
         [(48, (0, 80), (1, 80)), (60, (0, 80), (8, 80)), (72, (0, 80), (0, 80))])
    case("I_three_tracks_1key", "t1: b1 pc80 on60 | t2: b8 pc80 on60 | t3: b0 pc80 on60",
         lambda k: (group(t1, k, 1), t1.note(k, DUR, 0, 60, 100),
                    group(t2, k, 8), t2.note(k, DUR, 0, 60, 100),
                    group(t3, k, 0), t3.note(k, DUR, 0, 60, 100)),
         [(60, (0, 80), (0, 80))])
    case("J_typeB_shape", "t1: b1 pc80 on60 on72 | t2: b1 pc81 on60 on72 | t3: b8 pc80 on72",
         lambda k: (group(t1, k, 1), t1.note(k, DUR, 0, 60, 100), t1.note(k, DUR, 0, 72, 100),
                    group(t2, k, 1, 81), t2.note(k, DUR, 0, 60, 100), t2.note(k, DUR, 0, 72, 100),
                    group(t3, k, 8), t3.note(k, DUR, 0, 72, 100)),
         [(60, (8, 80), (1, 81)), (72, (8, 80), (8, 80))])
    return _write_manifest("42_multitrack_same_tick.mid", t0, man,
                           tracks=[t0, t1, t2, t3])


def p43_level_vs_pitch():
    """Does a patch's LEVEL depend on the key that was written, or on the pitch
    that comes out? Nothing in the corpus separates those.

    Every gain probe (24, 27, 28) holds the patch fixed at key 60 and varies a
    controller. Every patch-identity probe (15, 34) holds the pitch at key 60
    and varies the patch. The cell where both move at once is empty, and that
    is where serum_opening's Type B gesture lives: its notes are written at
    keys 3 and 15, pulled up 24 semitones by RPN2 and another 48 by a bend at
    full range, and this project renders them with roughly the right spectral
    SHAPE but about 16 dB too much level. Probes 30/31 come closest -- RPN2
    coarse plus bend -- but they deliberately attack the +-4800 cent clamp from
    ABOVE, keys 60 to 127. Nothing approaches it from below.

    Two axes, crossed. Four patches, chosen because they are the candidates for
    the Type B layer and are mutually 13 dB or more apart in timbre: 000:080
    Square Lead, 001:080 Square Wave, 001:081 Saw Wave, 008:080 Sine Wave.
    Four ways to arrive at two sounding pitches:

      nat75    key 75, no tune, no bend                 -- sounds 75, natively
      tuned75  key 51, RPN2 +24, no bend                -- sounds 75, one
                                                           region lower
      bent75   key 3, RPN2 +24, RPN0 48, bend at max    -- sounds 75, the
                                                           Type B route, from
                                                           the bottom region
      nat27    key 27, no tune, no bend                 -- sounds 27, natively
      tuned27  key 3, RPN2 +24, no bend                 -- sounds 27, isolating
                                                           the tune half

    The first three are the load-bearing ones: 622 Hz reached through three
    DIFFERENT regions. Keys 75, 51 and 3 fall in three separate regions of all
    four patches (000:080 splits at 38/52/63/77, 001:080 at 45/55/67/80,
    001:081 at 40/51/63/76, 008:080 at 52/64/82), so if per-region attenuation
    or key-follow is what sets level, these three separate on it while the
    sounding pitch is held fixed.

    nat27/tuned27 is a NULL control, deliberately: keys 3 and 27 land in the
    SAME bottom region of all four patches, so the only difference is how the
    pitch was reached. They must come out identical. If they don't, RPN2 itself
    is doing something to level and the whole reading is off.

    Read it two ways:
      DOWN a patch's column: nat75 / tuned75 / bent75 equal means level follows
      the sounding pitch and the Type B excess is not a pitch-dependent level
      law. Spread means the WRITTEN key drives it -- region selection and
      whatever per-region attenuation comes with it -- which is the shape of
      answer that would explain Type B, already half-flagged by the fact
      that 001:080 and 008:080 lack decay key-follow (probe 35 avoids
      them for that reason). This project already renders a 4.5 dB spread on
      000:080 between nat75 and bent75, so the axis is live either way; the
      reference says whether that spread is right.
      ACROSS the bent75 row: the relative level of the four candidate layers at
      exactly Type B's pitch, which is the number needed to decide whether the
      reference's Type B carries a quiet saw or a loud sine.

    Bend is held STATIC at the extreme, not swept -- probe 33 already
    characterises the ramp, and a static hold gives a clean RMS. RPN0 and RPN2
    are explicitly restored to 2 and 0 before each case, and the bend recentred,
    because both persist across a channel.
    """
    tr = Track()
    tr.sysex(0, GS_RESET)
    man = ["# 43_level_vs_pitch.mid",
           "# onset\tcase\tpatch\twritten_key\tcoarse\tbend_range\tbend\tsounding_key"]
    clock = t(0.5)
    DUR, GAP = t(0.6), t(1.5)
    CENTRE, MAXBEND = 8192, 16383
    PATCHES = [(0, 80, "squarelead(000:080)"), (1, 80, "squarewave(001:080)"),
               (1, 81, "saw(001:081)"), (8, 80, "sine(008:080)")]
    # (name, written key, RPN2 coarse semitones, RPN0 range, bend value)
    CONFIGS = [("nat75", 75, 0, 2, CENTRE),
               ("tuned75", 51, 24, 2, CENTRE),
               ("bent75", 3, 24, 48, MAXBEND),
               ("nat27", 27, 0, 2, CENTRE),
               ("tuned27", 3, 24, 2, CENTRE)]

    for msb, prog, pname in PATCHES:
        for cname, key, coarse, rng, bend in CONFIGS:
            # park every persistent register first, then set this case's
            tr.rpn(clock, 0, 0, 0, 2)          # RPN0 = default 2 semitones
            tr.rpn(clock, 0, 0, 2, 64)         # RPN2 = 0 semitones
            tr.bend(clock, 0, CENTRE)
            tr.cc(clock, 0, 7, 100)
            tr.cc(clock, 0, 11, 127)
            tr.cc(clock, 0, 10, 64)
            tr.cc(clock, 0, 0, msb)
            tr.cc(clock, 0, 32, 0)
            tr.prog(clock, 0, prog)
            setup = clock + t(0.2)
            if coarse: tr.rpn(setup, 0, 0, 2, 64 + coarse)
            if rng != 2: tr.rpn(setup, 0, 0, 0, rng)
            if bend != CENTRE: tr.bend(setup, 0, bend)
            onset = clock + t(0.5)
            tr.note(onset, DUR, 0, key, 100)
            man.append(f"{onset / TPS:.3f}\t{cname}\t{pname}\t{key}\t"
                       f"{coarse:+d}\t{rng}\t{bend}\t{key + coarse + (rng if bend == MAXBEND else 0)}")
            clock += GAP
    return _write_manifest("43_level_vs_pitch.mid", tr, man)


def p44_release_shape():
    """What SHAPE is the ordinary note-off release, and what sets its length?

    tests/sine-gate.mid answers this for exactly one patch and the answer
    contradicts the engine. Its reference releases are LINEAR IN AMPLITUDE and
    reach true digital zero: 15 consecutive envelope samples falling by a
    constant -0.0254 of the plateau every 2 ms, straight into silence, ~31 ms
    from plateau to zero, reproduced across two independent captures. The
    engine's ENV_RELEASE is geometric (-100 dB over RELEASE_FLOOR_S), which
    asymptotes and never lands, so it sits at -18 dB in every one of that
    file's 10.7 ms gaps and the notes never separate.

    One patch is not a law, and 008:080's own authored release is 6.25 ms (the
    rate the engine renders with RELEASE_FLOOR_S forced to 0), five times
    shorter than the 31 ms observed. Three readings fit that single data point
    identically and imply different code:

      1. linear to zero over eg1_release_tc x some constant factor
      2. linear to zero over a FIXED duration, patch-independent -- a
         voice-retire fade, with the 6.25 ms authored release finishing
         invisibly underneath it and eg1_release_tc never reaching the output
      3. a RATE rather than a duration (fraction of full scale per second), so
         what varies is the level at note-off, not the patch -- probe 45

    Nothing in the existing corpus separates them. Scanning all 43 references
    for plateau-to-silence falls finds plenty, but they are the wrong events:
    32/33/37/31 are CC7/CC11 controller ramps (all four implying the same
    ~20.9 ms full-to-zero, i.e. the mixer ramp period, not a release), 04/39/41
    are EG1 DECAY tails on sustain=0 patches, and 11/18 are drum key-group
    chokes on the fast-release path. sine-gate is the only item in the corpus
    where an ordinary note-off release is isolated against silence.

    Section A: 12 melodic patches chosen to span the authored-release range,
    one note each, held 600 ms then 900 ms of silence so the release completes
    in the clear whatever its length. Three reps per patch: the sine-gate
    captures disagreed on 2 of 34 gaps, so a single observation per patch is
    not enough to call one an outlier.

    If every patch ramps in the same ~31 ms, reading 2 is right and the fix is
    a constant. If ramp length tracks the patch, reading 1 is right and the
    factor falls out of the 12 points.

    Section B: keys 48/60/72 on three of them. The driver has key-follow on
    the DECAY time; whether the release has it too is unrecorded, and a probe
    that cannot see it would need re-recording if it does.
    """
    tr = Track()
    tr.sysex(0, GS_RESET)
    man = ["# 44_release_shape.mid",
           "# onset_s\toff_s\tsection\trep\tpatch\tbank\tprogram\tkey\tvel"]
    clock = t(0.5)
    DUR, GAP = t(0.6), t(0.9)
    PATCHES = [(0, 0, "piano1"), (0, 16, "organ1"), (0, 24, "nylon_gt"),
               (0, 47, "timpani"), (0, 48, "strings"), (0, 52, "choir_aahs"),
               (0, 61, "brass_section"), (0, 73, "flute"),
               (0, 80, "square_lead"), (0, 88, "pad_new_age"),
               (0, 122, "seashore"), (8, 80, "sine(008:080)")]

    def case(section, rep, bank, prog, key, vel=100):
        nonlocal clock
        tr.cc(clock, 0, 7, 100)
        tr.cc(clock, 0, 11, 127)
        tr.cc(clock, 0, 10, 64)
        tr.cc(clock, 0, 0, bank)
        tr.cc(clock, 0, 32, 0)
        tr.prog(clock, 0, prog)
        onset = clock + t(0.1)
        tr.note(onset, DUR, 0, key, vel)
        name = next(n for b, p, n in PATCHES if (b, p) == (bank, prog))
        man.append(f"{onset / TPS:.3f}\t{(onset + DUR) / TPS:.3f}\t{section}\t{rep}\t"
                   f"{name}\t{bank}\t{prog}\t{key}\t{vel}")
        clock = onset + DUR + GAP

    for rep in range(3):
        for bank, prog, _ in PATCHES:
            case("A_patches", rep, bank, prog, 60)
    for bank, prog, _ in ((0, 0, ""), (0, 48, ""), (8, 80, "")):
        for key in (48, 60, 72):
            case("B_keyfollow", 0, bank, prog, key)
    # A redundant CC11=127 (already its value, so silent) purely to push
    # end-of-track past the last note-off: the release tail IS the measurement
    # here, and a player that stops at EOT would clip the final case.
    tr.cc(clock, 0, 11, 127)
    return _write_manifest("44_release_shape.mid", tr, man)


def p45_release_from_level():
    """Is the release a fixed DURATION or a fixed RATE, and does it ride on the
    bare EG or on the audible level?

    Probe 44 varies the patch. This varies the level the release starts from,
    holding the patch fixed, which is the axis that separates reading 3 from
    readings 1 and 2. A linear ramp at a fixed rate (fraction of full scale per
    second) takes proportionally less time from a lower starting level; a fixed
    duration takes the same time from any level, so the two diverge by tens of
    milliseconds at the bottom of section A.

    Section A moves the EG level: two patches whose EG1 decays toward a low
    sustain, note-off taken at 50/150/300/600/1200/2400 ms after note-on so the
    envelope is at a different point of its decay each time. Piano 1 is probe
    04's own patch (sustain=0, so the decay spans the full range); Marimba is a
    second sustain=0 patch, because one patch behaving oddly here would
    otherwise be indistinguishable from a law.

    Sections B and C move the level WITHOUT moving the EG, which is the
    distinction voice_step_envelope's finish detection already had to make
    (+0x13c was then read as a gain-inclusive log-domain level, and this
    project's AUDIBLE_FLOOR check depended on that reading). Both use the
    flat-sustain sine so the EG is pinned at its plateau:

      B: velocity 127/96/64/32/16 -- moves the note's own attenuation
      C: CC7 at 127/96/64/32/16 at fixed velocity -- moves channel volume only,
         touching nothing in the voice's envelope setup at all

    If ramp length varies in A but not in B/C, the ramp is on the bare EG. If
    it varies in all three, it is on the audible level. If it varies in none,
    the release is a fixed duration and probe 44 alone settles the rest.
    """
    tr = Track()
    tr.sysex(0, GS_RESET)
    man = ["# 45_release_from_level.mid",
           "# onset_s\toff_s\tsection\tpatch\tbank\tprogram\tkey\tvel\tcc7\thold_ms"]
    clock = t(0.5)
    GAP = t(0.9)

    def case(section, bank, prog, pname, key, vel, cc7, hold_s):
        nonlocal clock
        tr.cc(clock, 0, 7, cc7)
        tr.cc(clock, 0, 11, 127)
        tr.cc(clock, 0, 10, 64)
        tr.cc(clock, 0, 0, bank)
        tr.cc(clock, 0, 32, 0)
        tr.prog(clock, 0, prog)
        onset, dur = clock + t(0.1), t(hold_s)
        tr.note(onset, dur, 0, key, vel)
        man.append(f"{onset / TPS:.3f}\t{(onset + dur) / TPS:.3f}\t{section}\t{pname}\t"
                   f"{bank}\t{prog}\t{key}\t{vel}\t{cc7}\t{hold_s * 1000:.0f}")
        clock = onset + dur + GAP

    for prog, pname in ((0, "piano1"), (12, "marimba")):
        for hold in (0.05, 0.15, 0.30, 0.60, 1.20, 2.40):
            case("A_eg_level", 0, prog, pname, 60, 100, 100, hold)
    for vel in (127, 96, 64, 32, 16):
        case("B_velocity", 8, 80, "sine(008:080)", 60, vel, 100, 0.60)
    for cc7 in (127, 96, 64, 32, 16):
        case("C_cc7", 8, 80, "sine(008:080)", 60, 100, cc7, 0.60)
    tr.cc(clock, 0, 11, 127)   # see probe 44: keep EOT clear of the last tail
    return _write_manifest("45_release_from_level.mid", tr, man)


def p46_noteoff_grid():
    """Why does the reference start its release BEFORE the MIDI note-off?

    Measured on tests/sine-gate.mid: the release ramp begins 5 to 27 ms ahead
    of the nominal note-off, varying note to note, while note-ONS in the same
    file land within ~1 ms of sample-exact. Two independent captures agree per
    note (r = 0.963, sd of the difference 1.79 ms, and 32 of 34 notes agree to
    ~0.5 ms), so it is deterministic -- not real-time scheduling jitter, and
    not anything the Windows-side sequencer could produce.

    That points at the driver's own drain: `[A:0x12bd6]`, called once per
    audio block from the render callback `[A:0x130af]` with now+nframes, pops
    every event due during the COMING block and acts on it immediately. A
    note-off would then take effect at the block start -- early by up to one
    block -- while a note-on still gets its offset within the block. But no
    fixed block size fits sine-gate's 34 points (2-D scan over period 8-32 ms
    and phase: best rms 4.55 ms against a 6.38 ms null, i.e. no fit), which
    would mean a VARIABLE nframes per callback.

    34 note-offs scattered over one song is too thin to resolve that. This
    sweeps the note-off across a fine grid instead: 64 cases at 500 ms spacing,
    each note-off one tick (1.042 ms) later within its case than the last, so
    the note-off's absolute time advances by 501.042 ms per case and walks
    densely across the phase of any grid in the plausible range. Sine patch,
    300 ms of silence after each note-off so the release always completes.

    Section B is the discriminator, and it removes the alignment error that
    limits the sine-gate measurement. Each case carries a MARKER note on
    channel 1 -- key 96, 2093 Hz, three octaves clear of the key-60 body --
    struck at exactly the same tick as the note-off. Note-ons are already
    established as sample-exact, so the interval from the marker's onset to the
    body's ramp start IS the earliness, read inside one file with no reference
    alignment and no clock-drift correction in the path. Section A repeats the
    same sweep without the marker, so if the marker's own voice allocation
    perturbs anything the two sections disagree and say so.

    If the earliness is -(t mod B) for some B, 64 dense phases will show it as
    a sawtooth outright. If it is not, that kills the block-quantisation
    reading and the answer is somewhere else in swmidi.sys.
    """
    tr = Track()
    tr.sysex(0, GS_RESET)
    man = ["# 46_noteoff_grid.mid",
           "# onset_s\toff_s\tsection\tcase\toff_offset_ticks\tmarker"]
    clock = t(0.5)
    SPACING, HOLD = t(0.5), t(0.2)
    for section, marker in (("A_plain", False), ("B_marked", True)):
        for i in range(32):
            for ch in (0, 1):
                tr.cc(clock, ch, 7, 100)
                tr.cc(clock, ch, 11, 127)
                tr.cc(clock, ch, 10, 64)
                tr.cc(clock, ch, 0, 8)
                tr.cc(clock, ch, 32, 0)
                tr.prog(clock, ch, 80)
            onset = clock + t(0.1)
            off = onset + HOLD + i          # +i ticks: 1.042 ms per case
            tr.at(onset, [0x90, 60, 100])
            tr.at(off, [0x80, 60, 0])
            if marker:
                tr.note(off, t(0.1), 1, 96, 100)
            # 6 decimals, not 3: the sweep step is 1.042 ms and millisecond
            # rounding would quantize away the very thing being measured.
            man.append(f"{onset / TPS:.6f}\t{off / TPS:.6f}\t{section}\t{i}\t{i}\t"
                       f"{'key96_ch1' if marker else '-'}")
            clock += SPACING
    tr.cc(clock, 0, 11, 127)   # see probe 44: keep EOT clear of the last tail
    return _write_manifest("46_noteoff_grid.mid", tr, man)


def p47_gm_off_state():
    """What GM System Off leaves behind: GS mode, and the instrument set.

    System Off calls the same device reset System On does (0x136e1), so it
    clears GS mode; it also releases the loaded collection (0x13700), which is
    `[I]` and is what this probe is really for. One case discriminates all
    three possibilities at once. After a GS Reset, bank 8 is selectable, so
    008:080 (Sine Wave) and 000:080 (Square Wave) are the two audible outcomes:

      silent      -> the collection really was released
      Sine Wave   -> GS mode survived System Off, and nothing was released
      Square Wave -> GS mode cleared, but nothing was released

    Cases 1 and 4 are positive controls: the same Sine Wave before the message
    and after a System On has re-acquired, so a silent case 2 cannot be blamed
    on the file.
    """
    tr = Track()
    tr.sysex(0, GS_RESET)
    clock = t(0.5)

    def sine_attempt():
        """Ask for 008:080. Sine Wave only if GS mode is on AND a kit is loaded."""
        nonlocal clock
        tr.cc(clock, 0, 0, 8)
        tr.cc(clock, 0, 32, 0)
        tr.prog(clock, 0, 80)
        tr.note(clock + t(0.1), t(1.0), 0, 60, 100)
        clock += t(2.0)

    sine_attempt()                                      # 1: control, Sine Wave
    tr.sysex(clock, [0x7E, 0x7F, 0x09, 0x02, 0xF7])     # GM System Off
    clock += t(0.5)
    sine_attempt()                                      # 2: the discriminator
    tr.sysex(clock, [0x7E, 0x7F, 0x09, 0x01, 0xF7])     # GM System On, re-acquires
    clock += t(0.5)
    sine_attempt()                                      # 3: Square Wave, GS is off
    tr.sysex(clock, GS_RESET)                           # GS back on
    clock += t(0.5)
    sine_attempt()                                      # 4: control, Sine Wave again
    return write("47_gm_off_state.mid", tr)


def p48_rhythm_part_reset():
    """USE RHYTHM PART falls into the shared reset tail (0x137b5).

    The message is aimed at block 5 -- a part nothing here plays on -- so any
    effect on channels 1 and 2 is the tail's device-wide controller reset, not
    the rhythm assignment. Two things to look for: the sustained notes cutting
    off where the SysEx lands, and the retriggered pair coming back loud and
    centred despite no controller being sent between them. Bank and Program are
    expected to survive, so all four notes should stay Square Wave.
    """
    tr = Track()
    tr.sysex(0, GS_RESET)
    clock = t(0.5)
    for ch in (0, 1):
        tr.prog(clock, ch, 80)                      # square wave: steady, easy to read
        tr.cc(clock, ch, 7, 40)                     # volume well under the 100 default
        tr.cc(clock, ch, 10, 0 if ch == 0 else 127) # hard left / hard right
        tr.cc(clock, ch, 11, 60)                    # expression under the 127 default
    clock += t(0.5)

    def pair(on):
        tr.at(clock, [(0x90 if on else 0x80), 60, 100 if on else 0])
        tr.at(clock, [(0x91 if on else 0x81), 64, 100 if on else 0])

    pair(True)
    clock += t(1.5)
    tr.sysex(clock, gs([0x40, 0x15, 0x15], [0x02]))  # block 5, an unrelated part
    clock += t(1.5)
    pair(False)                                      # may already be gone
    clock += t(0.5)
    pair(True)                                       # no controllers touched since
    clock += t(1.5)
    pair(False)
    clock += t(0.5)
    return write("48_rhythm_part_reset.mid", tr)


def p49_gm_off_during_note():
    """What happens to a voice already sounding when GM System Off arrives.

    One question is open (INVESTIGATE.adoc#i-gm-off-voices): whether a voice that is already
    sounding when the message lands holds a reference of its own to the
    instrument collection, and so survives the collection release. Probe 47
    could not speak to this -- nothing was sounding when its System Off
    arrived.

    What this probe CAN do: measure, for the first time, what actually
    happens to a sounding voice at GM System Off -- whether it is cut, how
    fast, and whether anything anomalous (a noise burst, a click, a
    truncation part-way through the sample) appears at the boundary. That is
    `[A]`-only today and worth an `[M]`.

    What it CANNOT do: cleanly settle the refcount question by itself. Traced
    from the binary: on the GM path the driver calls `ResetDevice`, then
    the collection release (`0x13700`), then falls into the shared reset tail,
    which runs `ResetAllChannelControllers` -- and that tail reclaims every
    sounding voice immediately (`0x123de`) regardless of
    whether the release actually unlinked the collection. So the voice is cut
    by the tail under BOTH hypotheses; the only window in which "voice holds
    a reference" and "voice does not" would differ audibly is between the
    release call and the tail, which is shorter than one render buffer. This
    probe bounds the question -- it does not settle it.

    Case 1 is the measurement: Sine Wave (008:080) held for 3.0s, with GM
    System Off landing 1.5s in -- squarely inside the sustain, not near
    either edge. Cases 2-4 are the same controls as probe 47: 2 is expected
    silent (collection released); 3 is a positive control after GM System On
    re-acquires -- GS mode is also cleared by System On, so 008:080 sounds as
    Square Wave (bank 0), not Sine Wave, and that is expected, not a defect;
    4 is a closing control after a GS Reset, Sine Wave again.
    """
    tr = Track()
    tr.sysex(0, GS_RESET)
    clock = t(0.5)

    # Case 1: the measurement. Note held well past the message so the cut
    # (or its absence) is unambiguous, and the note-off is still emitted at
    # its scheduled time even though the note is expected gone by then.
    tr.cc(clock, 0, 0, 8)
    tr.cc(clock, 0, 32, 0)
    tr.prog(clock, 0, 80)
    note_on = clock + t(0.1)
    note_dur = t(3.0)
    tr.note(note_on, note_dur, 0, 60, 100)
    tr.sysex(note_on + t(1.5), [0x7E, 0x7F, 0x09, 0x02, 0xF7])  # GM System Off, mid-note
    clock = note_on + note_dur + t(0.5)

    def sine_attempt(dur=1.0):
        """Ask for 008:080. Sine Wave only if GS mode is on AND a kit is loaded."""
        nonlocal clock
        tr.cc(clock, 0, 0, 8)
        tr.cc(clock, 0, 32, 0)
        tr.prog(clock, 0, 80)
        tr.note(clock + t(0.1), t(dur), 0, 60, 100)
        clock += t(dur) + t(0.6)

    sine_attempt(1.0)                                    # 2: control, expected silent
    tr.sysex(clock, [0x7E, 0x7F, 0x09, 0x01, 0xF7])     # GM System On, re-acquires
    clock += t(0.5)
    sine_attempt(1.0)                                    # 3: Square Wave, GS mode cleared
    tr.sysex(clock, GS_RESET)                           # GS back on
    clock += t(0.5)
    sine_attempt(1.0)                                    # 4: control, Sine Wave again
    return write("49_gm_off_during_note.mid", tr)


def p50_gain_ceiling():
    """The TR-808 hand clap defect (`field/HueArme-Weekend`, 77.5456s):
    gm.dls wave 193 `HCLAP60` is the CORRECT sample, correctly selected, but
    slopgs renders it at a burst prominence of +14.2 dB against the real
    MSGS reference's +3.0 dB -- about 11 dB too hot. A uniform global trim
    was already tried and failed, because score.py's metric is
    level-normalised and a constant offset is invisible to it.

    Mechanism under test, read from voice_update_gain() (voice.c:827-845):

        atten_hdb   = g_master_vol_hdb + chan_vol + expr + v->atten_const_hdb;
        gain_linear = 10^((atten_hdb/100 + GAIN_TRIM_DB) / 20);   // voice.c:835
        ... pan applied ...
        if (gain_l_target > GAIN_CEILING) gain_l_target = GAIN_CEILING; // :844
        if (gain_r_target > GAIN_CEILING) gain_r_target = GAIN_CEILING; // :845

    with GAIN_CEILING = 32767/65536 = 0.49999 (voice.c:816). The clamp is
    PER-VOICE and happens BEFORE any voices are summed (render_voice /
    render_frames in render.c accumulate already-clamped per-voice gains via
    a saturating add). Every gm.dls region carries a NEGATIVE wsmp
    lAttenuation, i.e. a gain BOOST, so at zero total attenuation
    gain_linear = 1.0, already more than double GAIN_CEILING: ordinary loud
    notes are pinned, not a rare edge case.

    HCLAP60's region has its own `lart` LIST but, walking gm.dls directly, no
    KEYONVELOCITY(2)->ATTENUATION(1) `art1` connection block inside it, and
    the instrument has no usable `lart` of its own either -- so it falls
    back to the engine's default -9600 full-depth velocity law: velocity
    modulates its attenuation at FULL depth.

    Because the clamp is per-voice/pre-sum and the corpus metric is
    level-normalised, a global offset cannot move any of the following
    relative to each other -- so they are what this probe measures instead:
    the velocity KNEE (where a voice stops being pinned and starts tracking),
    the FLAT-TOP width above it, and whether a real attenuation LADDER
    collapses (ceiling) or keeps its authored spacing (scale/no ceiling).

    A. Velocity knee sweep, GS Sine Wave 008:080, key 60, CC7=127/CC11=127
       (not the GM defaults -- those never engage the clamp at all, see A2).
       Predicted knee: atten_hdb = g_table_vel[vel] - 384 must exceed -602 to
       pin, i.e. vel > 127*10^(-218/4000) ~= 112.0. Dense from 104 up (4-unit
       steps) to bracket it; top (>=112) should render FLAT, below should
       TRACK the velocity law.
    A2. Unpinned control: same patch/key at the OLD GM-default CC7=100,
       CC11=127, vel 96/112/127 only. Must TRACK (no flat top): predicted
       -4.86 / -2.18 / 0.00 dB relative to its own top. Separates "the
       ceiling exists" (A) from "the velocity law is wrong" (A2).
    B. Region attenuation ladder at vel 127 (above section A's knee):
       SAW0_60 (000:081, authored 0.000 dB), BAGPP64A (000:109, -2.4965 dB),
       PIANO64 (000:000, -6.700 dB), HCLAP60 (TR-808 kit, prog 25, key 39,
       -4.300 dB). Raw gm.dls wsmp lAttenuation, re-verified directly against
       the collection with dB = lAttenuation/655360: 0, -1636067, -4390912,
       -2818048 respectively.
    C. The same ladder at vel 32 (below the knee) -- the control that makes
       B interpretable: if B's spread is collapsed and C's is not, the
       ceiling is confirmed and bracketed between the two velocities.
    D. Unequal-pair stacking: the per-voice pre-sum clamp only shows up when
       two DIFFERENT levels are summed (identical levels above the ceiling
       scale together and prove nothing either way).
    E. Identical-voice stacking, N = 1/2/4/8, to separate a per-voice clamp
       (predicts +6.02 dB per doubling, since the clamp does not prevent a
       linear sum) from any limiting/clipping applied after the sum.
    F. Single voice with total attenuation driven to zero (vel 127, CC7 127,
       CC11 127) -- pins the raw-register mapping on its own.
    G. The exact defect: TR-808 kit on MIDI channel 11 (0-based index 10),
       declared a rhythm part via GS "USE FOR RHYTHM PART" block 0x0A (addr
       byte 0x10+0x0A = 0x1A, block 0 = channel 10, blocks 1-9 = channels
       1-9, blocks A-F = channels 11-16 -- probe 16/48 convention), key 39,
       vel 126, then two velocity neighbours on the same voice to locate
       HCLAP60's own knee.
    """
    tr = Track()
    tr.sysex(0, GS_RESET)
    clock = t(0.5)
    man = ["# 50_gain_ceiling.mid",
           "# HCLAP60 (wave 193): region has its own lart but no",
           "# KEYONVELOCITY(2)->ATTENUATION(1) art1 connection block, and the",
           "# instrument-level lart is absent too -- confirmed by walking gm.dls",
           "# directly -- so it falls back to the engine's default -9600",
           "# full-depth velocity law (velocity modulates it at full depth).",
           "# onset\tsection\tch\tbank\tprog\tkey\tvel\tvoices\texpected"]

    def emit(section, voices, desc, dur, gap):
        """voices: list of (ch, bank_or_None, prog_or_None, key, vel,
        cc7_or_None, cc11_or_None). All voices in one call share one onset,
        i.e. they are one manifest case / one expected burst."""
        nonlocal clock
        onset = clock
        for ch, bank, prog, key, vel, cc7, cc11 in voices:
            if bank is not None:
                tr.cc(clock, ch, 0, bank)
                tr.cc(clock, ch, 32, 0)
            if prog is not None:
                tr.prog(clock + t(0.01), ch, prog)
            if cc7 is not None:
                tr.cc(clock, ch, 7, cc7)
            if cc11 is not None:
                tr.cc(clock, ch, 11, cc11)
        for ch, bank, prog, key, vel, cc7, cc11 in voices:
            tr.note(clock + t(0.05), dur, ch, key, vel)
        man.append(f"{onset / TPS + 0.05:.3f}\t{section}\t"
                   f"{','.join(str(v[0]) for v in voices)}\t"
                   f"{','.join('-' if v[1] is None else str(v[1]) for v in voices)}\t"
                   f"{','.join('-' if v[2] is None else str(v[2]) for v in voices)}\t"
                   f"{','.join(str(v[3]) for v in voices)}\t"
                   f"{','.join(str(v[4]) for v in voices)}\t"
                   f"{len(voices)}\t{desc}")
        clock += gap

    MELODIC_DUR, DRUM_DUR = t(0.5), t(0.4)
    GAP, DRUM_GAP, STACK_GAP, PAUSE = t(1.0), t(1.1), t(1.3), t(1.5)

    # -- A: velocity knee sweep, GS Sine Wave, key 60, CC7=127/CC11=127 (NOT
    # the GM defaults -- at 100/127 the top of the sweep never reaches the
    # clamp, which is the defect this revision fixes; see A2 for the old
    # settings kept as the unpinned control). Predicted knee: atten_hdb =
    # g_table_vel[vel] - 384 must exceed -602 to pin, i.e. vel > 112.0. Dense
    # from 104 up (4-unit steps) to bracket it.
    for vel in (8, 16, 32, 48, 64, 80, 96, 104, 108, 112, 116, 120, 124, 127):
        where = ("predicted ABOVE the knee (~vel112): must render FLAT, "
                 "same level as every other vel>=112 case" if vel >= 112 else
                 "predicted BELOW the knee (~vel112): must TRACK the "
                 "velocity law, below the flat top")
        emit("A", [(0, 8, 80, 60, vel, 127, 127)],
             f"vel {vel}, CC7=127 CC11=127: {where}",
             MELODIC_DUR, GAP)
    clock += PAUSE

    # -- A2: unpinned control -- same patch/key at the OLD GM-default
    # CC7=100/CC11=127. Must TRACK with no flat top: predicted -4.86 dB
    # (vel96), -2.18 dB (vel112), 0.00 dB (vel127), all relative to this
    # section's own top (vel127). Contrasting this against A's pinned top is
    # what separates "the ceiling exists" from "the velocity law is wrong".
    for vel, exp_db in ((96, -4.86), (112, -2.18), (127, 0.00)):
        emit("A2", [(0, 8, 80, 60, vel, 100, 127)],
             f"vel {vel}, CC7=100 CC11=127 (unpinned control): predicted "
             f"{exp_db:+.2f} dB vs this section's vel127 -- must TRACK, no "
             "flat top",
             MELODIC_DUR, GAP)
    clock += PAUSE

    # -- B/C: region attenuation ladder. Values re-verified directly against
    # gm.dls (dB = lAttenuation / 655360); see docstring for the raw ints.
    LADDER = [
        (0, 81, 60, 0.0000, "SAW0_60 (000:081)"),
        (0, 109, 60, -2.4965, "BAGPP64A (000:109)"),
        (0, 0, 60, -6.7000, "PIANO64 (000:000)"),
    ]
    for bank, prog, key, db, label in LADDER:
        emit("B", [(0, bank, prog, key, 127, 100, 127)],
             f"{label} authored {db:+.4f} dB at vel127 (above the knee): a "
             "ceiling collapses this ladder's spread to nothing; a scale "
             "error preserves it",
             MELODIC_DUR, GAP)
    emit("B", [(9, None, 25, 39, 127, 100, 127)],
         "HCLAP60 (TR-808 kit, prog25 key39) authored -4.3000 dB at vel127 "
         "(above the knee): the exact defect instrument, same ladder test",
         DRUM_DUR, DRUM_GAP)
    clock += PAUSE

    for bank, prog, key, db, label in LADDER:
        emit("C", [(0, bank, prog, key, 32, 100, 127)],
             f"{label} authored {db:+.4f} dB at vel32 (below the knee, "
             "control): the true authored spacing should appear here, "
             "unlike B -- this brackets the ceiling threshold",
             MELODIC_DUR, GAP)
    emit("C", [(9, None, 25, 39, 32, 100, 127)],
         "HCLAP60 at vel32 (below the knee, control): true -4.3dB spacing "
         "vs the other three should appear here, unlike B",
         DRUM_DUR, DRUM_GAP)
    clock += PAUSE

    # -- D: unequal-pair stacking. The per-voice pre-sum clamp only shows up
    # when two DIFFERENT levels are summed.
    emit("D", [(0, 8, 80, 60, 127, 100, 127)],
         "sine vel127 alone: anchor for the pair sum below", MELODIC_DUR, GAP)
    emit("D", [(0, 8, 80, 60, 48, 100, 127)],
         "sine vel48 alone: anchor for the pair sum below", MELODIC_DUR, GAP)
    emit("D", [(0, 8, 80, 60, 127, 100, 127), (1, 8, 80, 60, 48, 100, 127)],
         "sine vel127 + vel48 together: a 1:1 sum (both pinned) vs the "
         "authored-ratio sum (unpinned) separates the ceiling",
         MELODIC_DUR, STACK_GAP)
    emit("D", [(0, 0, 81, 60, 127, 100, 127)],
         "SAW0_60 (0.000 dB) alone: anchor for the unequal-attenuation pair "
         "sum below", MELODIC_DUR, GAP)
    emit("D", [(0, 0, 0, 60, 127, 100, 127)],
         "PIANO64 (-6.700 dB) alone: anchor for the unequal-attenuation "
         "pair sum below", MELODIC_DUR, GAP)
    emit("D", [(0, 0, 81, 60, 127, 100, 127), (1, 0, 0, 60, 127, 100, 127)],
         "SAW0_60 + PIANO64 together, both vel127: a 1:1 sum (both pinned) "
         "vs the authored 6.7dB-ratio sum (unpinned) separates the ceiling",
         MELODIC_DUR, STACK_GAP)
    clock += PAUSE

    # -- E: identical-voice stacking, N = 1/2/4/8. All GS Sine Wave, vel127,
    # on distinct keys within ONE region (key 65-82, wave SINE_76) so every
    # stacked voice shares the same authored attenuation, 2 semitones apart
    # so the voices neither phase-cancel nor alias and stay individually
    # resolvable in a spectrogram. A linear (unclamped-sum) mixer predicts
    # +6.02 dB per doubling; a per-voice clamp that still sums linearly
    # matches that, post-sum limiting/int16 clipping would not.
    STACK_KEYS = (65, 67, 69, 71, 73, 75, 77, 79)
    for n in (1, 2, 4, 8):
        voices = [(0, 8, 80, k, 127, 100, 127) for k in STACK_KEYS[:n]]
        emit("E", voices,
             f"{n} identical sine voices summed: +6.02 dB per doubling is "
             "the linear-mixer prediction (per-voice clamp still sums "
             "linearly); post-sum limiting/clipping would flatten it",
             MELODIC_DUR, STACK_GAP)
    clock += PAUSE

    # -- F: single voice with total attenuation driven to zero -- CC7 and
    # CC11 both pushed to 127 (above section A's 100/127 defaults), so
    # gain_linear -> 1.0, well above GAIN_CEILING. Pins the raw-register
    # mapping independent of any summing behavior.
    emit("F", [(0, 8, 80, 60, 127, 127, 127)],
         "full-scale single voice (vel127, CC7=127, CC11=127): pins the "
         "raw-register mapping on its own", MELODIC_DUR, GAP)
    clock += PAUSE

    # -- G: the exact defect. Block 0x0A -> MIDI channel 11 (0-based index
    # 10): block 0 is channel 10, blocks 1-9 are channels 1-9, blocks A-F
    # are channels 11-16 (probe 16/48 convention); addr byte = 0x10+block =
    # 0x1A. No CC7/CC11 override here, so the defect conditions are exactly
    # GS Reset + this one SysEx + program + key + velocity.
    tr.sysex(clock, gs([0x40, 0x1A, 0x15], [0x02]))
    clock += t(0.5)
    for vel, desc in (
        (126, "field defect velocity on MIDI channel 11 (0-based ch 10): "
              "reproduces the +11dB-hot clap exactly as it occurs in "
              "field/HueArme-Weekend"),
        (100, "neighbour below the field velocity: locates HCLAP60's own "
              "knee"),
        (64, "further below: brackets HCLAP60's knee together with vel100 "
             "and vel126"),
    ):
        emit("G", [(10, None, 25, 39, vel, None, None)], desc,
             DRUM_DUR, DRUM_GAP)

    return _write_manifest("50_gain_ceiling.mid", tr, man)


PROBES = [p01_programs, p02_keyrange, p03_velocity, p04_envelope, p05_pitchbend,
          p06_modwheel, p07_pan_volume, p08_reverb, p09_chorus, p10_polyphony,
          p11_drums, p12_gs_sysex, p13_edge, p14_running_status,
          p15_banks, p16_drum_parts, p17_master_volume, p18_key_groups,
          p19_prior_art, p20_voice_count, p21_steal_policy, p22_no_gs_reset,
          p23_rpn_tune, p24_gain_staging, p25_pan_law, p26_other_gains,
          p27_gain_curves, p28_expression_gate, p29_all_sound_off_gap,
          p30_tune_clamp_bend, p31_tune_clamp_bend_sine, p32_ramp_shape, p33_pitch_ramp, p34_sfx_bank_identity,
          p35_decay_keyfollow, p36_kit_key_fallback, p37_rac_volume_order,
          p38_same_tick_locale, p39_high_sustain_decay, p40_same_tick_bank,
          p41_sustain_decay_curve, p42_multitrack_same_tick, p43_level_vs_pitch,
          p44_release_shape, p45_release_from_level, p46_noteoff_grid,
          p47_gm_off_state, p48_rhythm_part_reset, p49_gm_off_during_note,
          p50_gain_ceiling]


def check(path):
    """Re-parse what we wrote: chunk framing, delta times, and event bytes."""
    with open(path, "rb") as f:
        data = f.read()
    assert data[:4] == b"MThd", path
    assert struct.unpack(">I", data[4:8])[0] == 6, path
    fmt, ntrk, div = struct.unpack(">HHH", data[8:14])
    assert div == TPQ and (fmt, ntrk) == (0, 1) or fmt == 1, (path, fmt, ntrk, div)
    pos = 14
    for _ in range(ntrk):
        assert data[pos:pos + 4] == b"MTrk", (path, pos)
        length = struct.unpack(">I", data[pos + 4:pos + 8])[0]
        assert pos + 8 + length <= len(data), (path, pos, length)
        _check_track(path, data, pos + 8, pos + 8 + length)
        pos += 8 + length
    assert pos == len(data), (path, pos, len(data))


def _check_track(path, data, start, end):
    i, status, saw_eot = start, None, False
    while i < end:
        while data[i] & 0x80:          # delta time
            i += 1
        i += 1
        b = data[i]
        if b == 0xFF:                  # meta: FF <type> <varlen len> <data>
            meta_type = data[i + 1]
            i += 2
            n = 0
            while data[i] & 0x80:
                n = (n << 7) | (data[i] & 0x7F)
                i += 1
            n = (n << 7) | data[i]
            i += 1 + n
            saw_eot = saw_eot or meta_type == 0x2F
        elif b in (0xF0, 0xF7):        # sysex
            i += 1
            n = 0
            while data[i] & 0x80:
                n = (n << 7) | (data[i] & 0x7F)
                i += 1
            n = (n << 7) | data[i]
            i += 1 + n
        else:
            if b & 0x80:
                status = b
                i += 1
            assert status is not None, f"{path}: running status with no status"
            i += 1 if (status & 0xF0) in (0xC0, 0xD0) else 2
    assert i == end, f"{path}: trailing garbage"
    assert saw_eot, f"{path}: no end-of-track"


if __name__ == "__main__":
    for probe in PROBES:
        path = probe()
        check(path)
        print(f"ok  {os.path.basename(path):28} {os.path.getsize(path):>7} bytes")
    print(f"\n{len(PROBES)} probes written and re-parsed clean.")
