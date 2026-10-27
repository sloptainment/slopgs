#!/usr/bin/env python3
"""score.py -- CLI port of dist/compare.js's two headline numbers.

    .venv/bin/python artifacts/score.py [name ...]

The reference is the real driver via artifacts/oracle/oracle (built on
demand) when this host can run i386 binaries; otherwise the recorded FLACs.

For every MIDI under field/, tests/ and artifacts/probes/ (oracle) or every
one with a reference FLAC (otherwise), or just the named ones -- a probe answers to either
"20" or "20_voice_count"), renders with dist/msgs-render and prints the same
envelope-correlation r and level-normalized spectral residual dB that the
browser compare pages show, plus a "dead" column: milliseconds of digital
silence in the render that the reference does not have (the dropout metric).

Same constants as compare.js: 50ms envelope hop, +/-5s lag search, 2048/1024
FFT, mean-RMS normalization before the residual. The reference is resampled
to 22050Hz by ffmpeg instead of by the browser's OfflineAudioContext, so
residuals track compare.js closely but are not bit-identical to it.

Needs numpy (requirements.txt) and ffmpeg.
"""
import os, platform, subprocess, sys, tempfile, wave

import numpy as np

RATE, ENV_HOP_MS, MAX_LAG_S, FFT_SIZE, FFT_HOP = 22050, 50, 5, 2048, 1024
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RENDERER = os.environ.get("MSGS_RENDER", os.path.join(ROOT, "dist/msgs-render"))
ORACLE = os.path.join(ROOT, "artifacts/oracle/oracle")


def oracle_ok():
    """x86 host whose kernel runs the i386 oracle (IA32 emulation can be
    off on x86_64, so exec it: usage exit = runs, OSError = can't)."""
    if platform.machine().lower() not in {"x86_64", "amd64", "i386", "i486",
                                          "i586", "i686"}:
        return False
    if subprocess.run(["make", "-s", "-C", os.path.dirname(ORACLE)]).returncode:
        return False
    try:
        subprocess.run([ORACLE], capture_output=True)
    except OSError:
        return False
    return True


def mono_s16(path):
    with wave.open(path, "rb") as w:
        raw = w.readframes(w.getnframes())
    x = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
    return x.reshape(-1, 2).mean(axis=1)


def decode_ref(flac, tmp):
    out = os.path.join(tmp, "ref.wav")
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", flac, "-ar", str(RATE),
                    "-ac", "2", "-f", "wav", out], check=True)
    return mono_s16(out)


def render(mid, tmp, renderer=RENDERER, name="slop.wav"):
    out = os.path.join(tmp, name)
    subprocess.run([renderer, os.path.join(ROOT, "dist/gm.dls"), mid,
                    "0", "999999999", out], check=True, stdout=subprocess.DEVNULL)
    return mono_s16(out)


def envelope(x, hop_ms):
    hop = int(RATE * hop_ms / 1000)
    n = (len(x) - hop) // hop
    return np.sqrt((x[:n * hop].reshape(n, hop) ** 2).mean(axis=1))


def align(er, es):
    """Same result as compare.js's alignByEnvelope: the lag maximizing the
    Pearson r of the overlapping region. Done as an explicit loop because
    each lag has a different overlap length, so the means/variances are
    per-lag and this is not a plain cross-correlation."""
    max_hops = round(MAX_LAG_S * 1000 / ENV_HOP_MS)
    best = (-2.0, 0)
    for lag in range(-max_hops, max_hops + 1):
        ai, bi = max(0, lag), max(0, -lag)
        n = min(len(er) - ai, len(es) - bi)
        if n < 10:
            continue
        a, b = er[ai:ai + n], es[bi:bi + n]
        sa, sb = a.std(), b.std()
        if sa < 1e-12 or sb < 1e-12:
            continue
        r = float(((a - a.mean()) * (b - b.mean())).mean() / (sa * sb))
        if r > best[0]:
            best = (r, lag)
    return best[0], best[1] * ENV_HOP_MS


def avg_spectrum(x):
    n = (len(x) - FFT_SIZE) // FFT_HOP + 1
    if n < 1:
        return np.zeros(FFT_SIZE // 2)
    idx = np.arange(FFT_SIZE) + np.arange(n)[:, None] * FFT_HOP
    frames = x[idx] * np.hanning(FFT_SIZE)  # == compare.js's 0.5-0.5cos(2pi i/(n-1))
    return np.abs(np.fft.rfft(frames, axis=1)[:, :FFT_SIZE // 2]).mean(axis=0)


def residual_db(ref, slop):
    rr, sr = np.sqrt((ref ** 2).mean()), np.sqrt((slop ** 2).mean())
    tgt = (rr + sr) / 2 or 1.0
    a = avg_spectrum(ref * (tgt / max(rr, 1e-9)))
    b = avg_spectrum(slop * (tgt / max(sr, 1e-9)))
    ratio = np.linalg.norm(a - b) / max(float(np.linalg.norm(a)), 1e-9)
    return 20 * np.log10(max(ratio, 1e-12))


def dead_ms(ref, slop):
    """Milliseconds where the render is >=60dB below its own RMS while the
    reference at the same spot is not -- i.e. dropouts we invented.

    Only meaningful on dense material. On a sparse probe the reference's own
    dither noise never drops below the threshold, so every genuine gap
    between test cases counts here; read it on field/ items, not probes."""
    er, es = envelope(ref, 5), envelope(slop, 5)
    n = min(len(er), len(es))
    thr_r = np.sqrt((ref ** 2).mean()) * 0.001
    thr_s = np.sqrt((slop ** 2).mean()) * 0.001
    return int(np.count_nonzero((es[:n] < thr_s) & (er[:n] >= thr_r)) * 5)


# (midi dir, reference-flac dir). Probes keep their references in a sibling
# dir and name them by the leading probe number only (20_voice_count.mid ->
# probe-results/20.flac), so try the number as well as the full basename.
DIRS = [("field", "field"), ("tests", "tests"),
        ("artifacts/probes", "artifacts/probe-results")]


def items(names, oracle):
    for mid_dir, flac_dir in DIRS:
        p = os.path.join(ROOT, mid_dir)
        if not os.path.isdir(p):
            continue
        for f in sorted(os.listdir(p)):
            if not f.endswith(".mid"):
                continue
            base = f[:-4]
            num = base.split("_")[0]
            if names and not (names & {base, num}):
                continue
            if oracle:
                yield base, os.path.join(p, f), None
                continue
            for stem in (base, num):
                flac = os.path.join(ROOT, flac_dir, stem + ".flac")
                if os.path.exists(flac):
                    yield base, os.path.join(p, f), flac
                    break


def main():
    names = set(sys.argv[1:])
    oracle = oracle_ok()
    print(f"{'item':32s} {'r':>7s} {'residual':>9s} {'dead':>8s}"
          f"   vs {'oracle' if oracle else 'FLAC'}")
    rs, res = [], []
    for base, mid, flac in items(names, oracle):
        with tempfile.TemporaryDirectory() as tmp:
            ref = (render(mid, tmp, ORACLE, "ref.wav") if oracle
                   else decode_ref(flac, tmp))
            slop = render(mid, tmp)
        r, lag_ms = align(envelope(ref, ENV_HOP_MS), envelope(slop, ENV_HOP_MS))
        lag = round(lag_ms / 1000 * RATE)
        ai, bi = max(0, lag), max(0, -lag)
        n = min(len(ref) - ai, len(slop) - bi)
        ref, slop = ref[ai:ai + n], slop[bi:bi + n]
        d = residual_db(ref, slop)
        rs.append(r); res.append(d)
        print(f"{base:32s} {r:7.3f} {d:9.2f} {dead_ms(ref, slop):6d}ms", flush=True)
    if rs:
        print(f"{'MEAN':32s} {sum(rs)/len(rs):7.3f} {sum(res)/len(res):9.2f}")


if __name__ == "__main__":
    main()
