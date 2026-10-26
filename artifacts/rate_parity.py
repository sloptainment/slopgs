#!/usr/bin/env python3
"""rate_parity.py -- CLAUDE.adoc's rate-parity bar.

    .venv/bin/python artifacts/rate_parity.py name ...

Renders each named item (score.py's names, or a .mid path) with
dist/msgs-render (x1) and dist/msgs-render-x2 (RESAMPLE_FACTOR=2), downsamples
the x2 render to 22050Hz with soxr, and checks it against the x1 render.
MSGS_RENDER / MSGS_RENDER_X2 override the two builds. Exits 1 on any FAIL.
"""
import os, subprocess, sys, tempfile

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from score import DIRS, ROOT, mono_s16

X1 = os.environ.get("MSGS_RENDER", os.path.join(ROOT, "dist/msgs-render"))
X2 = os.environ.get("MSGS_RENDER_X2", os.path.join(ROOT, "dist/msgs-render-x2"))
RATE = 22050

# env_r is a floor, the rest ceilings. Fit 2026-10-26 on 06, 51 and
# serum_opening: the LFO-fixed x2 build reads env_r 0.9977-0.9998, pitch_c
# <=0.37; the pre-fix build (LFO 2x fast) reads 0.9947/0.9901 and 8.94/5.03 c
# on 06/51, so pitch_c is the wide margin.
BAR = {"offset_ms": 2.0, "env_r": 0.995, "pitch_c": 2.0, "band_db": 0.5}


def render(exe, mid, out, down=False):
    subprocess.run([exe, os.path.join(ROOT, "dist/gm.dls"), mid, "0",
                    "999999999", out], check=True, stdout=subprocess.DEVNULL)
    if down:
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", out, "-af",
                        "aresample=resampler=soxr:precision=28", "-ar",
                        str(RATE), out + ".22k.wav"], check=True)
        out += ".22k.wav"
    return mono_s16(out)


def env_r(a, b, hop=RATE // 100):
    k = len(a) // hop
    ea = np.sqrt((a[:k * hop].reshape(k, hop) ** 2).mean(1))
    eb = np.sqrt((b[:k * hop].reshape(k, hop) ** 2).mean(1))
    return float(np.corrcoef(ea, eb)[0, 1])


def pitch_c(a, b, W=1024, N=16384):
    """Median |cents| between the two renders' dominant-peak tracks, 46ms
    frames, frames within 20dB of the loudest. Short enough to follow vibrato,
    so a wrong LFO rate or depth shows here as well as a detune."""
    win = np.hanning(W)
    f = np.fft.rfftfreq(N, 1 / RATE)
    band = (f > 40) & (f < 5000)
    idx = np.arange(0, len(a) - W, W // 2)
    loud = np.array([np.sqrt((a[i:i + W] ** 2).mean()) for i in idx])
    idx = idx[loud > loud.max() * 0.1]
    idx = [i for i in idx if np.any(b[i:i + W])]

    def peak(x):
        S = np.abs(np.fft.rfft(x * win, N))[band]
        k = int(np.clip(np.argmax(S), 1, len(S) - 2))
        p, q, r = np.log(S[k - 1:k + 2] + 1e-12)
        den = p - 2 * q + r
        d = np.clip(0.5 * (p - r) / den, -0.5, 0.5) if den else 0.0
        return f[band][k] + d * (f[1] - f[0])

    d = [1200 * np.log2(peak(b[i:i + W]) / peak(a[i:i + W])) for i in idx]
    return float(np.median(np.abs(d)))


def band_db(a, b, W=4096):
    win = np.hanning(W)
    f = np.fft.rfftfreq(W, 1 / RATE)

    def S(x):
        return np.mean([np.abs(np.fft.rfft(x[i:i + W] * win)) ** 2
                        for i in range(0, len(x) - W, W // 2)], 0)[f < 5000].sum()
    return float(10 * np.log10(S(b) / S(a)))


def check(mid, tmp):
    a = render(X1, mid, os.path.join(tmp, "x1.wav"))
    b = render(X2, mid, os.path.join(tmp, "x2.wav"), down=True)
    # soxr compensates its own delay, so the two are compared as rendered;
    # offset is the first-sound difference
    onset = [int(np.argmax(np.abs(x) > 1e-3)) for x in (a, b)]
    m = min(len(a), len(b))
    a, b = a[:m], b[:m]
    return {"offset_ms": abs(onset[0] - onset[1]) * 1000 / RATE,
            "env_r": env_r(a, b),
            "pitch_c": pitch_c(a, b), "band_db": abs(band_db(a, b))}


def main():
    names = set(sys.argv[1:])
    if not names:
        sys.exit(__doc__)
    # score.py's names, minus its need for a reference FLAC
    mids = [(os.path.basename(n)[:-4], n) for n in names if n.endswith(".mid")]
    for d, _ in DIRS:
        p = os.path.join(ROOT, d)
        for f in sorted(os.listdir(p)) if os.path.isdir(p) else []:
            if f.endswith(".mid") and names & {f[:-4], f.split("_")[0]}:
                mids.append((f[:-4], os.path.join(p, f)))
    print(f"{'item':32s} {'offset_ms':>9s} {'env_r':>7s} {'pitch_c':>7s} "
          f"{'band_db':>7s}")
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        for base, mid in mids:
            m = check(mid, tmp)
            bad = [k for k, lim in BAR.items()
                   if (m[k] < lim if k == "env_r" else m[k] > lim)]
            failed += bool(bad)
            print(f"{base:32s} {m['offset_ms']:9.2f} {m['env_r']:7.4f} "
                  f"{m['pitch_c']:7.2f} {m['band_db']:7.2f}  "
                  + ("FAIL " + ",".join(bad) if bad else "ok"))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
