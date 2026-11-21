/*
 * compare.js -- shared implementation for the three dist/compare-*.html pages
 * (probes, tests, field). Renders slopgs live via msgs.wasm + gm.dls (same ABI
 * sequence as bg-sound2.js, read but not modified) and compares each render
 * against a reference FLAC: spectrograms, analytical stats, and playback.
 *
 * Playback is driven from the spectrogram rather than from its own player.
 * Click anywhere in either canvas to drop a red playhead; "play reference" /
 * "play slopgs" / "play mixed" all start from that column, on the aligned,
 * nudged signals the spectrograms are drawn from. "Mixed" puts the reference
 * in the left ear and slopgs in the right, so a divergence reads as the
 * stereo image pulling to one side.
 *
 * Plain classic <script>, no build step, no imports, no dependencies.
 *
 * Handles the three traps named in CLAUDE.adoc's "Measuring" section:
 *   - rate:      decodeReferenceAtSynthRate() forces decodeAudioData onto a
 *                SYNTH_RATE-Hz context so the platform resamples for us.
 *   - alignment: alignByEnvelope() does RMS-envelope cross-correlation over
 *                a coarse hop, searching a few seconds of lag.
 *   - dither/level: normalizeRMS() scales both signals to a common RMS
 *                before the spectral residual is computed.
 */
"use strict";

const SlopgsCompare = (() => {
  // BASE_RATE (22050) * RESAMPLE_FACTOR, the rate dist/msgs.wasm was built at
  // (voice.h). The analysis and grading constants below bake it in, so it stays
  // a declared constant here -- but loadSynth() now checks it against the
  // module's own msgs_sample_rate() rather than trusting this copy. Default
  // build, RESAMPLE_FACTOR=1.
  const SYNTH_RATE = 22050;
  const RENDER_CHUNK_FRAMES = SYNTH_RATE; // 1s per chunk
  const MAX_RENDER_SECONDS = 240; // cap so a runaway file can't hang the page
  const ENV_HOP_MS = 50; // envelope hop for alignment cross-correlation
  const MAX_LAG_SECONDS = 5; // alignment search window
  const FFT_SIZE = 2048; // for averaged spectrum / spectral residual
  const FFT_HOP = 1024;
  // On-screen spectrogram: the analysis window is chosen per view (see
  // pickFftSize), between these bounds -- small when zoomed in for time
  // resolution, large when zoomed out for frequency resolution.
  const SPEC_MIN_FFT = 256;
  const SPEC_MAX_FFT = 2048;
  const SPEC_SURVEY_FFT = 1024; // fixed size for the one-off dB-scale survey
  const SPEC_DYNAMIC_RANGE_DB = 80; // default shading floor below peak; slider-driven
  const SPEC_CANVAS_HEIGHT = 300;
  const NYQUIST = SYNTH_RATE / 2; // whole visible spectrum, half of SYNTH_RATE
  const MIN_FREQ_SPAN = 100; // narrowest frequency window, in Hz
  // Vertical axis warp. Linear by default; "log" is log(f + LOG_KNEE) rather
  // than log(f) so 0 Hz stays on the axis instead of running to -infinity --
  // the band that matters here starts at DC, not at some arbitrary cutoff.
  // The knee is where the axis stops being logarithmic and goes linear-ish;
  // 30Hz puts the whole audible bass range on the log part.
  const LOG_KNEE = 30;
  function warpHz(f, log) { return log ? Math.log(Math.max(0, f) + LOG_KNEE) : f; }
  function unwarpHz(w, log) { return log ? Math.exp(w) - LOG_KNEE : w; }
  const ZOOM_STEP = 2; // per +/- press and per ctrl+wheel notch

  // -----------------------------------------------------------------------
  // wasm driver -- same ABI call sequence as bg-sound2.js's loadSynth/_pump,
  // but single-shot render-to-completion instead of a playback pump loop.
  // -----------------------------------------------------------------------
  let synthPromise = null;

  // Points synth.e at an instance with room for an SMF of smfLen bytes. A
  // bigger song gets a bigger instance; msgs_alloc never frees, so the old
  // block stays behind.
  function ensureCapacity(synth, smfLen) {
    if (smfLen <= synth.cap) return;
    const { exp } = synth;
    const size = exp.msgs_size(smfLen) >>> 0;
    const e = exp.msgs_create(exp.msgs_alloc(size), size) >>> 0;
    if (!e) throw new Error(`msgs_create failed (${size} bytes)`);
    const initRet = exp.msgs_init(e, synth.dlsPtr, synth.dlsLen) | 0;
    if (initRet !== 0) throw new Error(`msgs_init failed (code ${initRet})`);
    synth.e = e;
    synth.cap = smfLen;
  }

  function loadSynth() {
    if (synthPromise) return synthPromise;
    synthPromise = (async () => {
      // cache: "reload" -- msgs.wasm changes identity on every rebuild but never
      // its URL, and a stale cached copy is exactly the silent wrong-speed bug
      // the msgs_sample_rate() check below exists to catch.
      const resp = await fetch("msgs.wasm", { cache: "reload" });
      if (!resp.ok) throw new Error(`HTTP ${resp.status} fetching msgs.wasm`);
      const wasmBytes = await resp.arrayBuffer();
      const { instance } = await WebAssembly.instantiate(wasmBytes, {});
      const exp = instance.exports;
      if (typeof exp.__wasm_call_ctors === "function") exp.__wasm_call_ctors();

      // The one thing this file cannot infer: which RESAMPLE_FACTOR the module
      // was built at. Mismatch is not subtle -- it plays every file at the wrong
      // speed -- but it is silent, so refuse to run instead.
      const wasmRate = exp.msgs_sample_rate ? exp.msgs_sample_rate() >>> 0 : 0;
      if (wasmRate !== SYNTH_RATE) {
        throw new Error(
          `msgs.wasm renders at ${wasmRate || "an unreported rate"}Hz but compare.js `
          + `is configured for ${SYNTH_RATE}Hz -- audio would play at `
          + `${wasmRate ? (SYNTH_RATE / wasmRate).toFixed(2) : "?"}x speed. Rebuild the wasm at a `
          + `matching RESAMPLE_FACTOR (voice.h, -D'd at compile time) or set SYNTH_RATE to ${wasmRate}.`
        );
      }

      const dlsResp = await fetch("gm.dls");
      if (!dlsResp.ok) throw new Error(`HTTP ${dlsResp.status} fetching gm.dls`);
      const dlsBytes = new Uint8Array(await dlsResp.arrayBuffer());

      const dlsPtr = exp.msgs_alloc(dlsBytes.length);
      new Uint8Array(exp.memory.buffer, dlsPtr, dlsBytes.length).set(dlsBytes);

      // Reused scratch buffer for every render() call across every item --
      // msgs_alloc never frees, so allocating fresh per item would grow
      // wasm memory forever across a page session (see rt_alloc's ponytail
      // note in src/wasm.c).
      const outPtr = exp.msgs_alloc(RENDER_CHUNK_FRAMES * 4); // stereo int16
      const synth = { exp, outPtr, dlsPtr, dlsLen: dlsBytes.length, e: 0, cap: -1 };
      ensureCapacity(synth, 0);
      return synth;
    })();
    return synthPromise;
  }

  // Renders one MIDI file to completion. Every item shares one instance, so
  // items must be rendered one at a time -- callers must not call this
  // concurrently for two items.
  async function renderMidi(midiUrl) {
    const resp = await fetch(midiUrl);
    if (!resp.ok) throw new Error(`HTTP ${resp.status} fetching ${midiUrl}`);
    const smfBytes = new Uint8Array(await resp.arrayBuffer());
    return renderMidiBytes(smfBytes);
  }

  // Same render, from bytes already in hand (a File, not a fetch) -- what
  // play2.html uses for a locally-picked .mid.
  async function renderMidiBytes(smfBytes) {
    const synth = await loadSynth();
    ensureCapacity(synth, smfBytes.length);
    const { exp, outPtr, e } = synth;
    exp.msgs_reset(e);
    const ptr = exp.msgs_alloc(smfBytes.length);
    new Uint8Array(exp.memory.buffer, ptr, smfBytes.length).set(smfBytes);
    const loadRet = exp.msgs_load_smf(e, ptr, smfBytes.length) | 0;
    if (loadRet !== 0) throw new Error(`msgs_load_smf failed (code ${loadRet})`);
    exp.msgs_set_loop(e, 0);

    const maxFrames = MAX_RENDER_SECONDS * SYNTH_RATE;
    const chunks = [];
    let total = 0;
    while (total < maxFrames) {
      const n = exp.msgs_render(e, outPtr, RENDER_CHUNK_FRAMES) >>> 0;
      if (n > 0) {
        // Re-view memory.buffer every time: memory.grow() (possible inside
        // msgs_render, e.g. voice/event growth) detaches prior views.
        const pcm = new Int16Array(exp.memory.buffer, outPtr, n * 2);
        chunks.push(pcm.slice()); // copy out before the next render() call reuses outPtr
        total += n;
      }
      if (n === 0 || exp.msgs_is_finished(e)) break;
    }

    const left = new Float32Array(total);
    const right = new Float32Array(total);
    let off = 0;
    for (const c of chunks) {
      const n = c.length / 2;
      for (let i = 0; i < n; i++) {
        left[off + i] = c[2 * i] / 32768;
        right[off + i] = c[2 * i + 1] / 32768;
      }
      off += n;
    }
    return { left, right, sampleRate: SYNTH_RATE };
  }

  // -----------------------------------------------------------------------
  // reference decode -- trap #1 (rate): force decodeAudioData onto a
  // SYNTH_RATE context so the platform resamples the 44.1kHz reference for
  // us. This buffer is reused for both playback and analysis.
  // ponytail: shared SYNTH_RATE buffer for playback too (slightly lower
  // fidelity than a native-rate decode); add a second native-rate decode
  // for listening if that quality loss ever matters.
  // -----------------------------------------------------------------------
  async function decodeReferenceAtSynthRate(flacUrl) {
    const resp = await fetch(flacUrl);
    if (!resp.ok) throw new Error(`HTTP ${resp.status} fetching ${flacUrl}`);
    const bytes = await resp.arrayBuffer();
    const Ctx = window.OfflineAudioContext || window.webkitOfflineAudioContext;
    const octx = new Ctx(2, 1, SYNTH_RATE);
    let buf;
    try {
      buf = await octx.decodeAudioData(bytes);
    } catch (err) {
      throw new Error(`could not decode ${flacUrl} as audio (browser FLAC support required): ${err.message || err}`);
    }
    if (buf.sampleRate !== SYNTH_RATE) {
      throw new Error(`decodeAudioData did not resample ${flacUrl} to ${SYNTH_RATE}Hz (got ${buf.sampleRate}Hz)`);
    }
    const left = buf.getChannelData(0).slice();
    const right = buf.numberOfChannels > 1 ? buf.getChannelData(1).slice() : left.slice();
    return { left, right, sampleRate: SYNTH_RATE };
  }

  function toMono(left, right) {
    const n = left.length;
    const m = new Float32Array(n);
    for (let i = 0; i < n; i++) m[i] = (left[i] + right[i]) * 0.5;
    return m;
  }

  // -----------------------------------------------------------------------
  // analysis: envelope, alignment (trap #2), level normalization (trap #3),
  // FFT / spectral residual, spectrogram.
  // -----------------------------------------------------------------------
  function computeEnvelope(mono, sampleRate, hopMs) {
    const hop = Math.max(1, Math.round((sampleRate * hopMs) / 1000));
    const n = Math.floor(mono.length / hop);
    const env = new Float32Array(n);
    for (let i = 0; i < n; i++) {
      let sum = 0;
      const start = i * hop;
      for (let j = 0; j < hop; j++) { const v = mono[start + j]; sum += v * v; }
      env[i] = Math.sqrt(sum / hop);
    }
    return env;
  }

  function pearsonAt(a, aStart, b, bStart, len) {
    let sa = 0, sb = 0;
    for (let i = 0; i < len; i++) { sa += a[aStart + i]; sb += b[bStart + i]; }
    const ma = sa / len, mb = sb / len;
    let cov = 0, va = 0, vb = 0;
    for (let i = 0; i < len; i++) {
      const da = a[aStart + i] - ma, db = b[bStart + i] - mb;
      cov += da * db; va += da * da; vb += db * db;
    }
    const denom = Math.sqrt(va * vb);
    return denom > 1e-12 ? cov / denom : 0;
  }

  // RMS-envelope cross-correlation over a coarse hop, searching
  // +/-MAX_LAG_SECONDS of lag -- the lazy sufficient alignment per
  // CLAUDE.adoc's tip (named alongside sample-domain FFT cross-cor).
  // lag > 0 means the slopgs envelope is delayed relative to the reference.
  function alignByEnvelope(envRef, envSlop, hopMs, maxLagSeconds) {
    const maxLagHops = Math.round((maxLagSeconds * 1000) / hopMs);
    let best = { lag: 0, r: -Infinity, len: 0 };
    for (let lag = -maxLagHops; lag <= maxLagHops; lag++) {
      const aStart = Math.max(0, lag), bStart = Math.max(0, -lag);
      const len = Math.min(envRef.length - aStart, envSlop.length - bStart);
      if (len < 10) continue;
      const r = pearsonAt(envRef, aStart, envSlop, bStart, len);
      if (r > best.r) best = { lag, r, len };
    }
    return { lagHops: best.lag, lagMs: best.lag * hopMs, r: best.r };
  }

  // Crops two full-resolution mono signals to their overlapping region at
  // the given sample-domain lag (same convention as alignByEnvelope: lag>0
  // means `slop` starts later than `ref`).
  function cropToLag(ref, slop, lagSamples) {
    const aStart = Math.max(0, lagSamples), bStart = Math.max(0, -lagSamples);
    const len = Math.min(ref.length - aStart, slop.length - bStart);
    return { ref: ref.subarray(aStart, aStart + len), slop: slop.subarray(bStart, bStart + len) };
  }

  function rms(x) {
    let sum = 0;
    for (let i = 0; i < x.length; i++) sum += x[i] * x[i];
    return Math.sqrt(sum / Math.max(1, x.length));
  }
  function peakAbs(x) {
    let m = 0;
    for (let i = 0; i < x.length; i++) { const v = Math.abs(x[i]); if (v > m) m = v; }
    return m;
  }
  function toDb(v) { return 20 * Math.log10(Math.max(v, 1e-12)); }

  // Trap #3: normalize both signals to a common RMS before the residual is
  // computed -- neutralizes the reference's dither noise floor / any gain
  // difference. Peak/RMS dB reported on the page are measured BEFORE this
  // normalization (they're meant to show the raw levels); only the spectral
  // residual uses the normalized pair.
  function normalizeRMS(ref, slop) {
    const rRms = rms(ref), sRms = rms(slop);
    const target = (rRms + sRms) / 2 || 1;
    const refN = new Float32Array(ref.length);
    const slopN = new Float32Array(slop.length);
    const rScale = target / Math.max(rRms, 1e-9), sScale = target / Math.max(sRms, 1e-9);
    for (let i = 0; i < ref.length; i++) refN[i] = ref[i] * rScale;
    for (let i = 0; i < slop.length; i++) slopN[i] = slop[i] * sScale;
    return { refN, slopN };
  }

  // In-place iterative radix-2 Cooley-Tukey. n must be a power of 2.
  function fft(re, im) {
    const n = re.length;
    for (let i = 1, j = 0; i < n; i++) {
      let bit = n >> 1;
      for (; j & bit; bit >>= 1) j ^= bit;
      j ^= bit;
      if (i < j) {
        let t = re[i]; re[i] = re[j]; re[j] = t;
        t = im[i]; im[i] = im[j]; im[j] = t;
      }
    }
    for (let len = 2; len <= n; len <<= 1) {
      const ang = (-2 * Math.PI) / len;
      const wr = Math.cos(ang), wi = Math.sin(ang);
      for (let i = 0; i < n; i += len) {
        let curWr = 1, curWi = 0;
        for (let k = 0; k < len / 2; k++) {
          const ur = re[i + k], ui = im[i + k];
          const xr = re[i + k + len / 2], xi = im[i + k + len / 2];
          const vr = xr * curWr - xi * curWi, vi = xr * curWi + xi * curWr;
          re[i + k] = ur + vr; im[i + k] = ui + vi;
          re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
          const nwr = curWr * wr - curWi * wi, nwi = curWr * wi + curWi * wr;
          curWr = nwr; curWi = nwi;
        }
      }
    }
  }

  function hannWindow(n) {
    const w = new Float32Array(n);
    for (let i = 0; i < n; i++) w[i] = 0.5 - 0.5 * Math.cos((2 * Math.PI * i) / (n - 1));
    return w;
  }

  // Short-time magnitude spectra of `mono`, one Float32Array (length
  // fftSize/2) per frame.
  function stftMagnitudes(mono, fftSize, hop) {
    const win = hannWindow(fftSize);
    const frames = [];
    const re = new Float32Array(fftSize), im = new Float32Array(fftSize);
    for (let start = 0; start + fftSize <= mono.length; start += hop) {
      for (let i = 0; i < fftSize; i++) { re[i] = mono[start + i] * win[i]; im[i] = 0; }
      fft(re, im);
      const mag = new Float32Array(fftSize / 2);
      for (let i = 0; i < fftSize / 2; i++) mag[i] = Math.hypot(re[i], im[i]);
      frames.push(mag);
    }
    return frames;
  }

  function averagedSpectrum(frames, bins) {
    const avg = new Float32Array(bins);
    if (frames.length === 0) return avg;
    for (const f of frames) for (let i = 0; i < bins; i++) avg[i] += f[i];
    for (let i = 0; i < bins; i++) avg[i] /= frames.length;
    return avg;
  }

  // Spectral residual dB: L2 distance between the two (normalized-input)
  // averaged magnitude spectra, relative to the reference spectrum's norm.
  // Worse = less negative, per CLAUDE.adoc.
  function spectralResidualDb(monoRefN, monoSlopN, sampleRate) {
    const framesRef = stftMagnitudes(monoRefN, FFT_SIZE, FFT_HOP);
    const framesSlop = stftMagnitudes(monoSlopN, FFT_SIZE, FFT_HOP);
    const bins = FFT_SIZE / 2;
    const specRef = averagedSpectrum(framesRef, bins);
    const specSlop = averagedSpectrum(framesSlop, bins);
    let diffSq = 0, refSq = 0;
    for (let i = 0; i < bins; i++) {
      const d = specRef[i] - specSlop[i];
      diffSq += d * d; refSq += specRef[i] * specRef[i];
    }
    return toDb(Math.sqrt(diffSq) / Math.max(Math.sqrt(refSq), 1e-9));
  }

  // One FFT per on-screen pixel column, over the visible sample window only.
  // This is what makes zoom mean something: the transform is recomputed for
  // whatever slice of the signal is on screen, so zooming in resolves detail
  // that a fixed pre-rendered image can only interpolate.
  //
  // The analysis window shrinks as you zoom in (pickFftSize) -- otherwise a
  // fixed 1024-sample window would smear ~46ms across the whole viewport and
  // zooming past that point would buy nothing but bigger blur.
  function pickFftSize(visibleSamples, width) {
    const targetWindow = (visibleSamples / Math.max(1, width)) * 4; // ~4 columns wide
    let n = SPEC_MIN_FFT;
    while (n < targetWindow && n < SPEC_MAX_FFT) n <<= 1;
    return n;
  }

  // Fixed dB scale shared by both canvases and by every zoom level. Computed
  // once from the full signals: if each view auto-gained itself, brightness
  // would shift as you scrolled and the reference/slopgs pair would be
  // shaded on two different scales -- which is precisely the comparison the
  // page exists to support.
  function computeSharedScale(signals) {
    let maxDb = -Infinity;
    for (const mono of signals) {
      const hop = Math.max(SPEC_MIN_FFT, Math.floor(mono.length / 400)); // coarse survey
      const frames = stftMagnitudes(mono, SPEC_SURVEY_FFT, hop);
      for (const f of frames) for (let i = 0; i < f.length; i++) {
        const d = toDb(f[i]);
        if (d > maxDb) maxDb = d;
      }
    }
    if (!isFinite(maxDb)) maxDb = 0;
    return { maxDb }; // the shading floor comes from the contrast slider
  }

  // Renders mono[start .. start+count) into `canvas` at one column per pixel.
  // ponytail: width FFTs per redraw, recomputed from scratch on every scroll
  // step -- fine at ~900px and a SYNTH_RATE mono signal, and it keeps the code a
  // single pass with no cache to invalidate. Add a column cache keyed by
  // (start, count) if a very long field recording ever feels sluggish.
  // Returns the band statistics it measured while drawing; the caller needs
  // both signals' numbers before it can print either one's delta, so text is
  // overlaid afterwards by drawOverlay rather than here.
  function drawSpectrogramWindow(canvas, mono, start, count, scale, meta) {
    const width = canvas.width, height = canvas.height;
    const fftSize = pickFftSize(count, width);
    const bins = fftSize / 2;
    const ctx = canvas.getContext("2d");
    const img = ctx.createImageData(width, height);
    const win = hannWindow(fftSize);
    const re = new Float32Array(fftSize), im = new Float32Array(fftSize);
    const mag = new Float32Array(bins);
    // Contrast: how far below the pair's shared peak the shading bottoms out.
    // Narrow it to pull faint detail out of the floor, widen it to keep only
    // the loud structure.
    const range = meta && meta.dynRangeDb ? meta.dynRangeDb : SPEC_DYNAMIC_RANGE_DB;
    const floorDb = scale.maxDb - range;
    const span = range || 1;
    // Visible frequency window, shared by both canvases (see buildSpectrograms).
    const fLo = meta && isFinite(meta.fLo) ? meta.fLo : 0;
    const fHi = meta && isFinite(meta.fHi) ? meta.fHi : NYQUIST;
    // Rows are laid out on the warped axis; only the row<->Hz conversion
    // changes between linear and log, everything downstream still works in Hz.
    const log = !!(meta && meta.logFreq);
    const wLo = warpHz(fLo, log), wHi = warpHz(fHi, log);
    const wSpan = Math.max(1e-9, wHi - wLo);
    // Band-pass meter over exactly the visible band and the visible time span,
    // accumulated from the transforms we are computing anyway.
    const bLo = Math.max(0, Math.min(bins - 1, Math.floor((fLo / NYQUIST) * bins)));
    const bHi = Math.max(bLo + 1, Math.min(bins, Math.ceil((fHi / NYQUIST) * bins)));
    let bandSumSq = 0, bandN = 0, bandPeak = 0;

    for (let c = 0; c < width; c++) {
      // Centre each column's window on the time that column represents, so
      // the image lines up with the waveform instead of lagging by a window.
      const centre = start + Math.floor(((c + 0.5) * count) / width);
      const from = centre - (fftSize >> 1);
      for (let i = 0; i < fftSize; i++) {
        const s = from + i;
        re[i] = (s >= 0 && s < mono.length ? mono[s] : 0) * win[i]; // zero-pad the edges
        im[i] = 0;
      }
      fft(re, im);
      for (let i = 0; i < bins; i++) mag[i] = Math.hypot(re[i], im[i]);
      for (let b = bLo; b < bHi; b++) {
        const m = mag[b];
        bandSumSq += m * m; bandN++;
        if (m > bandPeak) bandPeak = m;
      }
      for (let row = 0; row < height; row++) {
        // Canvas rows are top-down and high frequency is at the top, so row 0
        // is fHi. Each row covers a frequency band; take the max over that
        // band's bins rather than sampling one -- when a 2048-point window
        // gives 1024 bins for 300 rows, picking every ~3rd bin drops narrow
        // partials entirely, which is exactly what this page exists to show.
        const fTop = unwarpHz(wHi - (row * wSpan) / height, log);
        const fBot = unwarpHz(wHi - ((row + 1) * wSpan) / height, log);
        let b0 = Math.floor((fBot / NYQUIST) * bins);
        let b1 = Math.ceil((fTop / NYQUIST) * bins);
        b0 = Math.max(0, Math.min(bins - 1, b0));
        b1 = Math.max(b0 + 1, Math.min(bins, b1));
        let m = 0;
        for (let b = b0; b < b1; b++) if (mag[b] > m) m = mag[b];
        const db = toDb(m);
        const v = Math.max(0, Math.min(1, (db - floorDb) / span));
        const gray = Math.round(v * 255);
        const idx = (row * width + c) * 4;
        img.data[idx] = gray; img.data[idx + 1] = gray; img.data[idx + 2] = gray; img.data[idx + 3] = 255;
      }
    }
    ctx.putImageData(img, 0, 0);

    // Frequency gridlines, labelled in Hz. Without an axis a zoomed-in band is
    // unreadable -- "the bright streak is too loud" means nothing until you can
    // see it sits at 3.2kHz, and a screenshot has to carry that on its face.
    ctx.font = "11px monospace";
    ctx.textBaseline = "middle";
    let lastY = -Infinity;
    for (const f of log ? logTicks(fLo, fHi) : niceTicks(fLo, fHi, 6)) {
      const y = Math.round(((wHi - warpHz(f, log)) * height) / wSpan) + 0.5;
      if (y < 12 || y > height - 4) continue;
      // A log axis bunches its low decades; drop labels that would overlap.
      if (y - lastY < 14) continue;
      lastY = y;
      ctx.strokeStyle = "rgba(120,190,255,0.28)";
      ctx.lineWidth = 1;
      ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(width, y); ctx.stroke();
      const t = fmtHz(f);
      const tw = ctx.measureText(t).width;
      ctx.fillStyle = "rgba(0,0,0,0.66)";
      ctx.fillRect(width - tw - 8, y - 7, tw + 6, 14);
      ctx.fillStyle = "#9ecbff";
      ctx.fillText(t, width - tw - 5, y);
    }
    ctx.textBaseline = "alphabetic";

    // Band levels are reported relative to the pair's shared peak, so 0 dB is
    // the loudest point in either signal. Absolute FFT magnitudes would depend
    // on window size and scale with zoom; this does not, and the ref/slopgs
    // difference stays directly comparable across every view.
    const bandRmsDb = toDb(Math.sqrt(bandSumSq / Math.max(1, bandN))) - scale.maxDb;
    const bandPeakDb = toDb(bandPeak) - scale.maxDb;
    return { fftSize, bandRmsDb, bandPeakDb, fLo, fHi, log };
  }

  // Same viewport draw contract as drawSpectrogramWindow (canvas, mono,
  // start, count, scale, meta) -> stats, so paint() can swap the two without
  // touching the shared zoom/scroll/playhead machinery built around them. No
  // frequency axis here -- min/max sample per pixel column instead of an FFT
  // per column.
  function drawWaveformWindow(canvas, mono, start, count, scale) {
    const width = canvas.width, height = canvas.height;
    const ctx = canvas.getContext("2d");
    ctx.fillStyle = "#000";
    ctx.fillRect(0, 0, width, height);
    const mid = height / 2;
    let sumSq = 0, n = 0, peak = 0;
    ctx.strokeStyle = "#7db8ff";
    ctx.beginPath();
    for (let c = 0; c < width; c++) {
      const from = start + Math.floor((c * count) / width);
      const to = Math.max(from + 1, start + Math.floor(((c + 1) * count) / width));
      let mn = 0, mx = 0;
      for (let s = Math.max(0, from); s < Math.min(mono.length, to); s++) {
        const v = mono[s];
        if (v < mn) mn = v; if (v > mx) mx = v;
        sumSq += v * v; n++;
        const av = Math.abs(v); if (av > peak) peak = av;
      }
      const y0 = mid - mx * mid, y1 = mid - mn * mid;
      ctx.moveTo(c + 0.5, y0); ctx.lineTo(c + 0.5, Math.max(y1, y0 + 1));
    }
    ctx.stroke();
    ctx.strokeStyle = "rgba(255,255,255,0.15)";
    ctx.beginPath(); ctx.moveTo(0, mid + 0.5); ctx.lineTo(width, mid + 0.5); ctx.stroke();
    // Same "relative to the pair's shared peak" convention as the
    // spectrogram's band stats (scale.maxDb comes from the same
    // computeSharedScale call) -- the absolute number reads oddly since
    // that peak is an FFT-bin magnitude, not a sample amplitude, but the
    // ref/slop delta the overlay prints is unaffected: both canvases share
    // the one `scale`, so the offset cancels out of the difference.
    const bandRmsDb = toDb(Math.sqrt(sumSq / Math.max(1, n))) - scale.maxDb;
    const bandPeakDb = toDb(peak) - scale.maxDb;
    return { fftSize: 0, bandRmsDb, bandPeakDb, fLo: 0, fHi: NYQUIST, log: false };
  }

  // Text block, drawn after both canvases are analysed so each can quote the
  // other. It goes ON the canvas, not in the DOM around it: a screenshot of
  // the image is what actually gets shared, and it is only useful if it says
  // which signal it is, what time and frequency range it covers, what manual
  // offset produced it, and what the band measured.
  function drawOverlay(canvas, label, lines) {
    const ctx = canvas.getContext("2d");
    ctx.font = "bold 14px sans-serif";
    let w = ctx.measureText(label).width;
    ctx.font = "12px monospace";
    for (const l of lines) w = Math.max(w, ctx.measureText(l).width);
    ctx.fillStyle = "rgba(0,0,0,0.72)";
    ctx.fillRect(2, 2, w + 10, 20 + lines.length * 16);
    ctx.font = "bold 14px sans-serif";
    ctx.fillStyle = label.startsWith("SLOPGS") ? "#7CFC9A" : "#FFD27C";
    ctx.fillText(label, 7, 16);
    ctx.font = "12px monospace";
    ctx.fillStyle = "#ddd";
    lines.forEach((l, i) => ctx.fillText(l, 7, 32 + i * 16));
  }

  function fmtZoom(z) { return (z < 10 ? z.toFixed(1) : Math.round(z)) + "×"; }
  function msToSamples(ms) { return Math.max(1, Math.round((ms / 1000) * SYNTH_RATE)); }
  function fmtHz(f) {
    return f >= 1000 ? `${(f / 1000).toFixed(f < 10000 ? 2 : 1)}kHz` : `${Math.round(f)}Hz`;
  }

  // Round tick values (1/2/5 x 10^n) inside [lo, hi] -- so a zoomed band is
  // labelled at readable numbers instead of arbitrary fractions of the view.
  function niceTicks(lo, hi, target) {
    const raw = (hi - lo) / Math.max(1, target);
    const mag = Math.pow(10, Math.floor(Math.log10(Math.max(raw, 1e-6))));
    const norm = raw / mag;
    const stepMul = norm <= 1 ? 1 : norm <= 2 ? 2 : norm <= 5 ? 5 : 10;
    const step = stepMul * mag;
    const out = [];
    for (let f = Math.ceil(lo / step) * step; f <= hi; f += step) out.push(f);
    return out;
  }

  // 1/2/5-per-decade ticks for the log axis -- linear ticks over a log axis
  // would crowd every label into the top third and leave the low end, which
  // is the reason to be in log mode at all, unlabelled.
  function logTicks(lo, hi) {
    const out = [];
    for (let d = 1; d <= 100000; d *= 10) {
      for (const m of [1, 2, 5]) {
        const f = d * m;
        if (f >= lo && f <= hi) out.push(f);
      }
    }
    return out;
  }

  // -----------------------------------------------------------------------
  // playback
  //
  // Both sides go through WebAudio from the same decoded/rendered Float32
  // buffers the spectrograms are drawn from -- no media element anywhere.
  // That is the point: the transport is the spectrogram's own playhead, so
  // what you hear starts at exactly the column you clicked, on either signal
  // or on both at once ("mixed": reference left, slopgs right).
  // -----------------------------------------------------------------------
  let playCtx = null;
  function getPlayCtx() {
    if (!playCtx) playCtx = new (window.AudioContext || window.webkitAudioContext)();
    return playCtx;
  }

  // One thing sounds at a time, page-wide: starting a second stops the first
  // and fires its onStop so the card it belonged to can drop its "playing"
  // marker.
  let currentPlay = null;
  function stopCurrent() {
    if (!currentPlay) return;
    const { src, onStop } = currentPlay;
    currentPlay = null;
    src.onended = null;
    try { src.stop(); } catch (_) { /* already ended */ }
    if (onStop) onStop();
  }
  // opts.startSample: sample to start at (default 0). opts.loop: when true,
  // uses the AudioBufferSourceNode's native loop (loopStart = startSample,
  // loopEnd = end of buffer) instead of looping back to sample 0 -- exactly
  // "loop from wherever the playhead is", for free, from the platform.
  function playStereo(left, right, sampleRate, onStop, opts) {
    stopCurrent();
    const ctx = getPlayCtx();
    ctx.resume().catch(() => {});
    const buf = ctx.createBuffer(2, left.length, sampleRate);
    buf.getChannelData(0).set(left);
    buf.getChannelData(1).set(right);
    const src = ctx.createBufferSource();
    src.buffer = buf;
    const startSample = (opts && opts.startSample) || 0;
    if (opts && opts.loop) {
      src.loop = true;
      src.loopStart = startSample / sampleRate;
      src.loopEnd = left.length / sampleRate;
    }
    src.connect(ctx.destination);
    src.onended = () => {
      if (currentPlay && currentPlay.src === src) { currentPlay = null; if (onStop) onStop(); }
    };
    currentPlay = { src, onStop };
    src.start(0, startSample / sampleRate);
    return src;
  }

  // Plays two channels that start at independent sample offsets in their own
  // signals -- which is what "from this point" means once the two signals have
  // been nudged apart: the playhead is one column on screen, but that column
  // is a different sample index in each signal. Clamped and length-matched so
  // an offset past either end just yields a shorter (or empty) buffer.
  function playFromOffsets(chL, atL, chR, atR, onStop) {
    const l = Math.max(0, Math.min(chL.length, Math.round(atL)));
    const r = Math.max(0, Math.min(chR.length, Math.round(atR)));
    const n = Math.min(chL.length - l, chR.length - r);
    if (n <= 0) return null;
    return playStereo(chL.subarray(l, l + n), chR.subarray(r, r + n), SYNTH_RATE, onStop);
  }

  // The ABI has one global synth state (no session handle), so two renders
  // must never overlap -- every render goes through this chain. Each item's
  // result is then cached on its own state object so pressing Play twice, or
  // Play then Load & compare, renders once.
  let renderChain = Promise.resolve();
  function getRender(item, state) {
    if (!state.slopPromise) {
      const run = () => renderMidi(item.midiUrl);
      const next = renderChain.then(run, run);
      renderChain = next.catch(() => {});
      state.slopPromise = next.catch((err) => { state.slopPromise = null; throw err; });
    }
    return state.slopPromise;
  }

  // -----------------------------------------------------------------------
  // per-item pipeline + UI
  // -----------------------------------------------------------------------
  function el(tag, attrs, children) {
    const e = document.createElement(tag);
    if (attrs) for (const k in attrs) {
      if (k === "text") e.textContent = attrs[k];
      else if (k === "html") e.innerHTML = attrs[k];
      else e.setAttribute(k, attrs[k]);
    }
    if (children) for (const c of children) e.appendChild(c);
    return e;
  }

  function buildInfoTable(extraRows) {
    // extraRows: [[label, value], ...] -- used by the tests page to surface
    // tests/README.adoc's per-item table (what it's derived from, what it
    // tests, regression example, what to expect).
    const table = el("table", { class: "info" });
    for (const [k, v] of extraRows) {
      const tr = el("tr", null, [el("th", { text: k }), el("td", { text: v })]);
      table.appendChild(tr);
    }
    return table;
  }

  function missingMsg(kind, url) {
    return `Missing ${kind}: expected at ${url}. This reference is not checked into the repo `
      + `(see CLAUDE.adoc); generate it and reload this page to compare against it. `
      + `The rest of the corpus is unaffected.`;
  }

  async function runItem(item, state, statusEl, resultsEl) {
    resultsEl.innerHTML = "";
    statusEl.textContent = "loading synth + gm.dls...";
    try {
      await loadSynth();
    } catch (err) {
      statusEl.textContent = `slopgs engine unavailable: ${err.message || err}`;
      return;
    }

    // Fetch/decode/render each side independently so a missing reference or
    // missing MIDI degrades just this item, not the page.
    let ref, slop;
    statusEl.textContent = "decoding reference...";
    try {
      ref = await decodeReferenceAtSynthRate(item.flacUrl);
    } catch (err) {
      statusEl.textContent = missingMsg("reference FLAC", item.flacUrl) + ` (${err.message || err})`;
      return;
    }
    statusEl.textContent = "rendering slopgs...";
    try {
      slop = await getRender(item, state);
    } catch (err) {
      statusEl.textContent = missingMsg("MIDI or slopgs render failed for", item.midiUrl) + ` (${err.message || err})`;
      return;
    }

    statusEl.textContent = "aligning + analyzing...";
    const monoRefFull = toMono(ref.left, ref.right);
    const monoSlopFull = toMono(slop.left, slop.right);
    const envRef = computeEnvelope(monoRefFull, SYNTH_RATE, ENV_HOP_MS);
    const envSlop = computeEnvelope(monoSlopFull, SYNTH_RATE, ENV_HOP_MS);
    const align = alignByEnvelope(envRef, envSlop, ENV_HOP_MS, MAX_LAG_SECONDS);
    const lagSamples = Math.round((align.lagMs / 1000) * SYNTH_RATE);
    const { ref: monoRef, slop: monoSlop } = cropToLag(monoRefFull, monoSlopFull, lagSamples);
    // Same crop on the stereo channels, so the spectrogram's playhead indexes
    // the listenable signal too. Kept stereo rather than played back from the
    // mono the spectrogram analyses: the pan probes (25_pan_law, 07_pan_volume)
    // exist precisely to be heard in stereo, and a downmix would silence what
    // they test.
    const L = cropToLag(ref.left, slop.left, lagSamples);
    const R = cropToLag(ref.right, slop.right, lagSamples);
    const chan = { refL: L.ref, refR: R.ref, slopL: L.slop, slopR: R.slop,
                   refM: monoRef, slopM: monoSlop };

    const peakRefDb = toDb(peakAbs(monoRef));
    const peakSlopDb = toDb(peakAbs(monoSlop));
    const rmsRefDb = toDb(rms(monoRef));
    const rmsSlopDb = toDb(rms(monoSlop));
    const { refN, slopN } = normalizeRMS(monoRef, monoSlop);
    const residualDb = spectralResidualDb(refN, slopN, SYNTH_RATE);

    statusEl.textContent = "";

    // One row of chips. Peak/RMS used to be a table, residual/r/lag a
    // paragraph, and lag/r again the status line -- three blocks stacked above
    // the spectrograms saying five numbers between them.
    resultsEl.innerHTML = "";
    const pair = (a, b) => `${a.toFixed(1)} / ${b.toFixed(1)} dB`;
    resultsEl.appendChild(el("div", { class: "summary" }, [
      // The two headline numbers from CLAUDE.adoc's corpus gate, lit up: the
      // rest of the row is context for these.
      stat("residual", `${residualDb.toFixed(1)} dB`, "level-normalized spectral residual; worse = less negative", "key-residual"),
      stat("env r", align.r.toFixed(3), "envelope correlation at the detected lag", "key-r"),
      stat("lag", `${align.lagMs} ms`, "start-delay removed by envelope cross-correlation"),
      stat("peak", pair(peakRefDb, peakSlopDb), "before level normalization"),
      stat("rms", pair(rmsRefDb, rmsSlopDb), "before level normalization"),
      el("span", { class: "note", text: "pairs are reference / slopgs" }),
    ]));

    buildSpectrograms(resultsEl, monoRef, monoSlop, align.lagMs, chan);
  }

  // Reference on top, slopgs below. Neither canvas scrolls: they are a fixed
  // viewport onto the signal, and scrolling/zooming moves the *window* being
  // transformed and repaints both. One scrollbar and one zoom factor feed both
  // canvases from the same (start, count), so the two views cannot drift apart
  // and every repaint shows real analysis rather than stretched pixels.
  function buildSpectrograms(parent, monoRef, monoSlop, autoLagMs, chan) {
    const total = Math.min(monoRef.length, monoSlop.length);
    const scale = computeSharedScale([monoRef, monoSlop]);

    const specWrap = el("div", { class: "specwrap" });
    const cRef = el("canvas", { class: "spectrogram" });
    const cSlop = el("canvas", { class: "spectrogram" });
    const cursor = el("div", { class: "spec-cursor" });
    const viewport = el("div", { class: "spec-viewport" }, [cRef, cSlop, cursor]);
    // A plain overflow strip whose inner spacer is `zoom` viewports wide. The
    // browser gives us a real scrollbar with real momentum/keyboard/trackpad
    // behaviour, and we read scrollLeft off it -- no scrollbar to reimplement.
    const spacer = el("div", { class: "spec-spacer" });
    const scrollbar = el("div", { class: "spec-scrollbar" }, [spacer]);

    const zoomVal = el("span", { class: "zoomval" });
    const rangeVal = el("span", { class: "rangeval" });
    const nudgeVal = el("span", { class: "rangeval" });
    let dynRange = SPEC_DYNAMIC_RANGE_DB;
    let zoom = 1; // 1 = whole item across the viewport
    let pending = false;
    // Manual alignment: a per-signal sample offset on top of the automatic
    // envelope-correlation lag. alignByEnvelope resolves to a 50ms hop and
    // assumes one constant offset, so it cannot fix a sub-hop error or a
    // reference that drifts; dragging a canvas nudges that signal's window.
    let refOff = 0, slopOff = 0;
    // Playhead, in shared-timeline samples (i.e. before either signal's own
    // nudge). One column on screen is one `playhead` value but a different
    // sample index per signal once refOff/slopOff differ, which is exactly
    // how playback has to treat it -- see playFromOffsets. null = unset.
    let playhead = null;
    // Visible frequency window, in Hz. Shared by both canvases -- unlike the
    // time nudge, which is deliberately per-signal, comparing two spectra only
    // means anything if they are on the same frequency axis.
    let fLo = 0, fHi = NYQUIST;
    // Linear or log-ish vertical axis. Shared by both canvases for the same
    // reason the band is: two spectra on different axes compare to nothing.
    let logFreq = false;
    // "spectrogram" or "waveform". Same zoom/scroll/playhead/drag machinery
    // drives both -- only the per-column draw call and the overlay text
    // change; see drawWaveformWindow's contract note.
    let mode = "spectrogram";

    function viewWidth() { return cRef.width || 1; }
    function visibleSamples() { return Math.max(viewWidth(), Math.round(total / zoom)); }

    function paint() {
      pending = false;
      const count = visibleSamples();
      const start = viewStart();
      const toMs = (s) => (s / SYNTH_RATE) * 1000;
      const view = { zoom, fLo, fHi, logFreq, dynRangeDb: dynRange };
      const draw = mode === "waveform" ? drawWaveformWindow : drawSpectrogramWindow;
      const sRef = draw(cRef, monoRef, start + refOff, count, scale, view);
      const sSlop = draw(cSlop, monoSlop, start + slopOff, count, scale, view);

      const band = mode === "waveform" ? "waveform" : `${fmtHz(fLo)}–${fmtHz(fHi)}${logFreq ? " log" : ""}`;
      const viewLine = (off, st) =>
        `${((start + off) / SYNTH_RATE).toFixed(3)}s–${((start + off + count) / SYNTH_RATE).toFixed(3)}s`
        + `   ${band}   offset ${off >= 0 ? "+" : ""}${toMs(off).toFixed(1)}ms`
        + (mode === "waveform"
          ? `   zoom ${fmtZoom(zoom)}`
          : `   window ${st.fftSize}   zoom ${fmtZoom(zoom)}   contrast ${dynRange}dB`);
      const lvl = (st) => `${mode === "waveform" ? "" : "band "}rms ${st.bandRmsDb.toFixed(1)}dB`
        + `  peak ${st.bandPeakDb.toFixed(1)}dB (rel. pair peak)`;
      const dRms = sSlop.bandRmsDb - sRef.bandRmsDb;
      const dPeak = sSlop.bandPeakDb - sRef.bandPeakDb;
      const sign = (v) => (v >= 0 ? "+" : "") + v.toFixed(1);

      drawOverlay(cRef, `REFERENCE (44.1kHz source, resampled to ${SYNTH_RATE}Hz)`,
        [viewLine(refOff, sRef), lvl(sRef)]);
      drawOverlay(cSlop, `SLOPGS (native ${SYNTH_RATE}Hz render)`,
        [viewLine(slopOff, sSlop), `${lvl(sSlop)}   Δ vs ref: rms ${sign(dRms)}dB  peak ${sign(dPeak)}dB`]);

      // The playhead sits at the same x on both canvases by construction: it
      // is a shared-timeline sample and both canvases show `count` samples
      // starting at `start` (plus each one's own nudge, which cancels here).
      if (playhead !== null) drawPlayhead(((playhead - start) / count) * viewWidth());
      playheadVal.textContent = playhead === null
        ? "click a spectrogram to set the start point"
        : `from ${(playhead / SYNTH_RATE).toFixed(3)}s`
          + (refOff || slopOff
            ? `  (ref ${((playhead + refOff) / SYNTH_RATE).toFixed(3)}s,`
              + ` slopgs ${((playhead + slopOff) / SYNTH_RATE).toFixed(3)}s)`
            : "");

      const t0 = start / SYNTH_RATE, t1 = (start + count) / SYNTH_RATE;
      rangeVal.textContent = `${t0.toFixed(2)}s – ${t1.toFixed(2)}s of ${(total / SYNTH_RATE).toFixed(2)}s`;
      zoomVal.textContent = fmtZoom(zoom);
      // The number that matters for alignment is slopgs relative to reference;
      // it is what you would feed back as a corrected lag.
      const rel = toMs(slopOff - refOff);
      nudgeVal.textContent = `${rel >= 0 ? "+" : ""}${rel.toFixed(1)} ms`
        + ` → total ${(autoLagMs + rel).toFixed(1)} ms`;
    }

    // 2px red rule down both canvases. Drawn last, after the overlays, so it
    // stays visible over the label block.
    function drawPlayhead(x) {
      if (x < -2 || x > viewWidth() + 2) return;
      for (const c of [cRef, cSlop]) {
        const ctx = c.getContext("2d");
        ctx.fillStyle = "#f00";
        ctx.fillRect(Math.round(x) - 1, 0, 2, c.height);
      }
    }

    // Frequency zoom about the centre of the visible band, clamped to the
    // real spectrum: there is nothing above Nyquist to look at.
    // Zoom about the centre of the axis as drawn: on a log axis the visual
    // centre of 0–11kHz is a few hundred Hz, not 5.5kHz, and zooming towards
    // the linear midpoint would walk straight past the band you switched to
    // log mode to look at.
    function setFreqZoom(factor) {
      const wMid = (warpHz(fLo, logFreq) + warpHz(fHi, logFreq)) / 2;
      const wHalf = ((warpHz(fHi, logFreq) - warpHz(fLo, logFreq)) / 2) * factor;
      let lo = Math.max(0, unwarpHz(wMid - wHalf, logFreq));
      let hi = Math.min(NYQUIST, unwarpHz(wMid + wHalf, logFreq));
      if (hi - lo < MIN_FREQ_SPAN) {
        const mid = (lo + hi) / 2;
        lo = mid - MIN_FREQ_SPAN / 2; hi = mid + MIN_FREQ_SPAN / 2;
      }
      panFreq(lo, hi);
    }
    function panFreq(lo, hi) {
      const s = hi - lo;
      if (lo < 0) { lo = 0; hi = s; }
      if (hi > NYQUIST) { hi = NYQUIST; lo = NYQUIST - s; }
      fLo = Math.max(0, lo); fHi = Math.min(NYQUIST, hi);
      schedulePaint();
    }
    // Coalesce to one repaint per frame: a scroll gesture fires far more
    // events than there are frames, and each repaint is width-many FFTs.
    function schedulePaint() {
      if (pending) return;
      pending = true;
      requestAnimationFrame(paint);
    }

    function setZoom(z) {
      const prev = zoom;
      zoom = Math.max(1, Math.min(maxZoom(), z));
      // Hold the centre of the current view steady across the zoom change.
      const denomPrev = Math.max(1, spacer.offsetWidth - scrollbar.clientWidth);
      const fracPrev = Math.min(1, Math.max(0, scrollbar.scrollLeft / denomPrev));
      // Zoom about the red playhead once one is set -- that is the point you
      // are inspecting, and holding it still is what makes zooming in on a
      // suspect moment usable. Falls back to holding the view centre steady
      // when no playhead has been dropped yet.
      const centreFrac = playhead !== null
        ? playhead / Math.max(1, total)
        : prev <= 1 ? 0.5 : fracPrev * (1 - 1 / prev) + 0.5 / prev;
      spacer.style.width = `${(zoom * 100).toFixed(4)}%`;
      const denomNext = Math.max(1, spacer.offsetWidth - scrollbar.clientWidth);
      const targetFrac = zoom <= 1 ? 0 : (centreFrac - 0.5 / zoom) / (1 - 1 / zoom);
      scrollbar.scrollLeft = Math.min(1, Math.max(0, targetFrac)) * denomNext;
      schedulePaint();
    }
    // Never zoom past one sample per pixel -- beyond that there is no more
    // signal to resolve, only interpolation.
    function maxZoom() { return Math.max(1, total / viewWidth()); }

    // Current visible window start, in shared-timeline samples. The single
    // pixel<->sample origin: paint(), the click handler and the playback
    // cursor all read it, so they cannot disagree about which column is when.
    function viewStart() {
      const count = visibleSamples();
      const maxStart = Math.max(0, total - count);
      const denom = Math.max(1, spacer.offsetWidth - scrollbar.clientWidth);
      const frac = denom > 0 ? Math.min(1, Math.max(0, scrollbar.scrollLeft / denom)) : 0;
      return Math.round(frac * maxStart);
    }

    // Drag a canvas left/right to slide that signal against the other. Both
    // are draggable: you nudge whichever one you are looking at. A pointerup
    // that never travelled more than DRAG_SLOP px is a click, not a drag, and
    // sets the playhead instead -- one gesture handler, so the two can't fight
    // over the same pointer sequence.
    const DRAG_SLOP = 4;
    function attachDrag(canvas, get, set) {
      let from = 0, fromY = 0, base = 0, baseLo = 0, baseHi = 0, active = false;
      canvas.addEventListener("pointerdown", (ev) => {
        active = true; from = ev.clientX; fromY = ev.clientY; base = get();
        baseLo = fLo; baseHi = fHi;
        try { canvas.setPointerCapture(ev.pointerId); } catch (_) { /* no capture */ }
        ev.preventDefault();
      });
      canvas.addEventListener("pointermove", (ev) => {
        if (!active) return;
        // Horizontal: convert CSS pixels of travel into samples at the current
        // zoom, so a drag moves the image exactly as far as the pointer went.
        // This one is per-signal -- it IS the manual alignment.
        const perPx = visibleSamples() / Math.max(1, canvas.clientWidth);
        set(Math.round(base - (ev.clientX - from) * perPx));
        // Vertical: pan the frequency window, and do it for both canvases.
        // Dragging down reveals higher frequencies, since high is at the top.
        // In warped units, so a drag moves the image by exactly the pointer's
        // travel on a log axis too.
        const wLo0 = warpHz(baseLo, logFreq), wHi0 = warpHz(baseHi, logFreq);
        const dW = ((ev.clientY - fromY) * (wHi0 - wLo0)) / Math.max(1, canvas.clientHeight);
        panFreq(unwarpHz(wLo0 + dW, logFreq), unwarpHz(wHi0 + dW, logFreq));
        schedulePaint();
      });
      const end = (ev) => {
        if (!active) return;
        active = false;
        try { canvas.releasePointerCapture(ev.pointerId); } catch (_) { /* fine */ }
        if (ev.type !== "pointerup") return;
        if (Math.abs(ev.clientX - from) > DRAG_SLOP || Math.abs(ev.clientY - fromY) > DRAG_SLOP) return;
        const x = ev.clientX - canvas.getBoundingClientRect().left;
        const perPx = visibleSamples() / Math.max(1, canvas.clientWidth);
        playhead = Math.max(0, Math.min(total, Math.round(viewStart() + x * perPx)));
        schedulePaint();
      };
      canvas.addEventListener("pointerup", end);
      canvas.addEventListener("pointercancel", end);
    }
    attachDrag(cRef, () => refOff, (v) => { refOff = v; });
    attachDrag(cSlop, () => slopOff, (v) => { slopOff = v; });

    const contrast = el("input", {
      type: "range", min: "20", max: "120", step: "5",
      value: String(SPEC_DYNAMIC_RANGE_DB), class: "contrast",
    });
    contrast.oninput = () => { dynRange = Number(contrast.value); schedulePaint(); };
    const scaleBtn = mkBtn("linear", () => {
      logFreq = !logFreq;
      scaleBtn.textContent = logFreq ? "log" : "linear";
      schedulePaint();
    });
    const freqOutBtn = mkBtn("−", () => setFreqZoom(2)); // wider band = zoomed out
    const freqInBtn = mkBtn("+", () => setFreqZoom(0.5));
    const freqFullBtn = mkBtn("Full", () => panFreq(0, NYQUIST));
    // Frequency axis and contrast are spectrogram-only; the waveform draw
    // ignores fLo/fHi/dynRange entirely, so grey those controls out rather
    // than leave them live and pointing at nothing.
    const viewBtn = mkBtn(mode, () => {
      mode = mode === "waveform" ? "spectrogram" : "waveform";
      viewBtn.textContent = mode;
      const disabled = mode === "waveform";
      contrast.disabled = disabled;
      scaleBtn.disabled = disabled;
      freqOutBtn.disabled = disabled;
      freqInBtn.disabled = disabled;
      freqFullBtn.disabled = disabled;
      schedulePaint();
    });
    // Row one is what you are looking at, row two is what you do to it. The
    // per-group readouts the canvas overlay already prints (band, contrast,
    // window) are not repeated here.
    const viewRow = el("div", { class: "ctlrow" }, [
      grp("zoom", [
        mkBtn("−", () => setZoom(zoom / ZOOM_STEP)),
        mkBtn("+", () => setZoom(zoom * ZOOM_STEP)),
        mkBtn("Fit", () => setZoom(1)),
        zoomVal,
      ]),
      grp("freq", [freqOutBtn, freqInBtn, freqFullBtn, scaleBtn]),
      grp("contrast", [contrast]),
      grp("view", [viewBtn]),
      rangeVal,
    ]);

    // Transport. There is no separate reference/slopgs player on the card any
    // more: the spectrogram IS the transport, and every one of these starts at
    // the red playhead (or at 0 if nothing has been clicked yet).
    const playheadVal = el("span", { class: "rangeval" });
    const playingVal = el("span", { class: "playing-indicator" });
    const followBox = el("input", { type: "checkbox" });
    const playRow = el("div", { class: "ctlrow" }, [
      grp("play", [
        mkBtn("▶ reference", () => playAt("ref")),
        mkBtn("▶ slopgs", () => playAt("slop")),
        mkBtn("▶ mixed", () => playAt("mixed")),
        mkBtn("■ stop", () => stopCurrent()),
        el("label", null, [followBox, document.createTextNode(" follow")]),
        playingVal,
        playheadVal,
      ]),
      grp("align", [
        mkBtn("◀ 1ms", () => { slopOff -= msToSamples(1); schedulePaint(); }),
        mkBtn("1ms ▶", () => { slopOff += msToSamples(1); schedulePaint(); }),
        mkBtn("Reset", () => { refOff = 0; slopOff = 0; schedulePaint(); }),
        nudgeVal,
      ]),
    ]);

    // Live playback cursor: a white bar tracking where the audio actually is,
    // driven off the AudioContext clock (the only clock that knows) and
    // positioned from the same (start, count) window paint() uses -- so it
    // stays put over the right column while you scroll or zoom mid-playback.
    // Hidden whenever nothing is sounding, or the position is off-screen.
    let cursorRaf = 0, cursorFrom = 0, cursorT0 = 0;
    function hideCursor() {
      if (cursorRaf) cancelAnimationFrame(cursorRaf);
      cursorRaf = 0;
      cursor.style.display = "none";
    }
    function onPlaybackStop() {
      hideCursor();
      playingVal.textContent = "";
    }
    // Scrolls the view so `sampleAt` sits at its centre -- same scrollLeft
    // math as viewStart()'s inverse, and shared across both canvases since
    // they share one scrollbar. See buildSingleViewer's centerOn for the
    // rationale (firing the scrollbar's own "scroll" listener repaints).
    function centerOn(sampleAt) {
      const maxStart = Math.max(0, total - visibleSamples());
      if (maxStart <= 0) return; // whole signal already fits on screen
      const desiredStart = Math.max(0, Math.min(maxStart, Math.round(sampleAt - visibleSamples() / 2)));
      const denom = Math.max(1, spacer.offsetWidth - scrollbar.clientWidth);
      const newScrollLeft = (desiredStart / maxStart) * denom;
      if (Math.abs(scrollbar.scrollLeft - newScrollLeft) > 0.5) scrollbar.scrollLeft = newScrollLeft;
    }
    function trackCursor() {
      cursorRaf = requestAnimationFrame(trackCursor);
      const at = cursorFrom + (getPlayCtx().currentTime - cursorT0) * SYNTH_RATE;
      if (followBox.checked) centerOn(at);
      const x = ((at - viewStart()) / visibleSamples()) * viewport.clientWidth;
      if (x < 0 || x > viewport.clientWidth) { cursor.style.display = "none"; return; }
      cursor.style.display = "block";
      cursor.style.left = `${x}px`;
    }

    // "mixed" is reference-left / slopgs-right on purpose: with the two in
    // opposite ears a difference reads as the image pulling to one side, which
    // is far easier to hear than the same difference summed to mono.
    function playAt(which) {
      const at = playhead === null ? 0 : playhead;
      const go = (l, lAt, r, rAt) => playFromOffsets(l, lAt, r, rAt, onPlaybackStop);
      const src = which === "ref"
        ? go(chan.refL, at + refOff, chan.refR, at + refOff)
        : which === "slop"
          ? go(chan.slopL, at + slopOff, chan.slopR, at + slopOff)
          : go(chan.refM, at + refOff, chan.slopM, at + slopOff);
      // Nothing started (playhead past the end of one signal) -- and nothing
      // was stopped either, so leave any running cursor alone.
      if (!src) return;
      playingVal.textContent = which === "ref" ? "▶ playing reference"
        : which === "slop" ? "▶ playing slopgs" : "▶ playing mixed (ref L / slopgs R)";
      if (cursorRaf) cancelAnimationFrame(cursorRaf);
      // Shared-timeline sample the cursor starts from -- `at`, not at+refOff:
      // the cursor is drawn in view coordinates, which are shared-timeline.
      cursorFrom = at;
      cursorT0 = getPlayCtx().currentTime;
      trackCursor();
    }

    specWrap.appendChild(viewRow);
    specWrap.appendChild(playRow);
    specWrap.appendChild(viewport);
    specWrap.appendChild(scrollbar);

    scrollbar.addEventListener("scroll", schedulePaint);
    // Ctrl/Cmd+wheel zooms time, the usual image-viewer gesture; Shift+wheel
    // zooms frequency. A plain wheel is left alone so the page still scrolls.
    viewport.addEventListener("wheel", (ev) => {
      if (ev.shiftKey) {
        ev.preventDefault();
        setFreqZoom(ev.deltaY < 0 ? 0.5 : 2);
        return;
      }
      if (!ev.ctrlKey && !ev.metaKey) return;
      ev.preventDefault();
      setZoom(ev.deltaY < 0 ? zoom * ZOOM_STEP : zoom / ZOOM_STEP);
    }, { passive: false });

    // Insert before sizing: the canvas backing store is set from the laid-out
    // viewport width, which is 0 until the element is in the document.
    parent.appendChild(specWrap);
    const w = Math.max(320, Math.round(viewport.clientWidth || 900));
    for (const c of [cRef, cSlop]) {
      c.width = w;
      c.height = SPEC_CANVAS_HEIGHT;
      c.style.width = "100%";
      c.style.height = SPEC_CANVAS_HEIGHT + "px";
    }
    spacer.style.width = "100%";
    paint();
    return specWrap;
  }

  // Single-signal viewer for play2.html: one canvas (spectrogram/waveform),
  // click to set the playhead, play from there with native buffer looping
  // (loopStart = playhead). Shares the draw/zoom/scroll machinery with
  // buildSpectrograms but there's only one signal, so no alignment nudge,
  // no drag-to-realign, no mixed playback.
  function buildSingleViewer(parent, mono, left, right, sampleRate) {
    const total = mono.length;
    const scale = computeSharedScale([mono]);

    const specWrap = el("div", { class: "specwrap" });
    const cSig = el("canvas", { class: "spectrogram" });
    const cursor = el("div", { class: "spec-cursor" });
    const viewport = el("div", { class: "spec-viewport" }, [cSig, cursor]);
    const spacer = el("div", { class: "spec-spacer" });
    const scrollbar = el("div", { class: "spec-scrollbar" }, [spacer]);

    const zoomVal = el("span", { class: "zoomval" });
    const rangeVal = el("span", { class: "rangeval" });
    let dynRange = SPEC_DYNAMIC_RANGE_DB;
    let zoom = 1;
    let pending = false;
    let playhead = 0;
    let fLo = 0, fHi = NYQUIST;
    let logFreq = false;
    let mode = "spectrogram";

    function viewWidth() { return cSig.width || 1; }
    function visibleSamples() { return Math.max(viewWidth(), Math.round(total / zoom)); }
    function maxZoom() { return Math.max(1, total / viewWidth()); }

    function viewStart() {
      const count = visibleSamples();
      const maxStart = Math.max(0, total - count);
      const denom = Math.max(1, spacer.offsetWidth - scrollbar.clientWidth);
      const frac = denom > 0 ? Math.min(1, Math.max(0, scrollbar.scrollLeft / denom)) : 0;
      return Math.round(frac * maxStart);
    }

    function drawPlayhead(x) {
      if (x < -2 || x > viewWidth() + 2) return;
      const ctx = cSig.getContext("2d");
      ctx.fillStyle = "#f00";
      ctx.fillRect(Math.round(x) - 1, 0, 2, cSig.height);
    }

    function paint() {
      pending = false;
      const count = visibleSamples();
      const start = viewStart();
      const view = { zoom, fLo, fHi, logFreq, dynRangeDb: dynRange };
      const draw = mode === "waveform" ? drawWaveformWindow : drawSpectrogramWindow;
      const st = draw(cSig, mono, start, count, scale, view);

      const band = mode === "waveform" ? "waveform" : `${fmtHz(fLo)}–${fmtHz(fHi)}${logFreq ? " log" : ""}`;
      const viewLine = `${(start / sampleRate).toFixed(3)}s–${((start + count) / sampleRate).toFixed(3)}s`
        + `   ${band}`
        + (mode === "waveform" ? `   zoom ${fmtZoom(zoom)}`
          : `   window ${st.fftSize}   zoom ${fmtZoom(zoom)}   contrast ${dynRange}dB`);
      const lvl = `${mode === "waveform" ? "" : "band "}rms ${st.bandRmsDb.toFixed(1)}dB  peak ${st.bandPeakDb.toFixed(1)}dB`;
      drawOverlay(cSig, `MIDI RENDER (native ${sampleRate}Hz)`, [viewLine, lvl]);

      drawPlayhead(((playhead - start) / count) * viewWidth());
      playheadVal.textContent = `from ${(playhead / sampleRate).toFixed(3)}s`;

      const t0 = start / sampleRate, t1 = (start + count) / sampleRate;
      rangeVal.textContent = `${t0.toFixed(2)}s – ${t1.toFixed(2)}s of ${(total / sampleRate).toFixed(2)}s`;
      zoomVal.textContent = fmtZoom(zoom);
    }

    function schedulePaint() {
      if (pending) return;
      pending = true;
      requestAnimationFrame(paint);
    }

    function setFreqZoom(factor) {
      const wMid = (warpHz(fLo, logFreq) + warpHz(fHi, logFreq)) / 2;
      const wHalf = ((warpHz(fHi, logFreq) - warpHz(fLo, logFreq)) / 2) * factor;
      let lo = Math.max(0, unwarpHz(wMid - wHalf, logFreq));
      let hi = Math.min(NYQUIST, unwarpHz(wMid + wHalf, logFreq));
      if (hi - lo < MIN_FREQ_SPAN) {
        const mid = (lo + hi) / 2;
        lo = mid - MIN_FREQ_SPAN / 2; hi = mid + MIN_FREQ_SPAN / 2;
      }
      panFreq(lo, hi);
    }
    function panFreq(lo, hi) {
      const s = hi - lo;
      if (lo < 0) { lo = 0; hi = s; }
      if (hi > NYQUIST) { hi = NYQUIST; lo = NYQUIST - s; }
      fLo = Math.max(0, lo); fHi = Math.min(NYQUIST, hi);
      schedulePaint();
    }

    function setZoom(z) {
      zoom = Math.max(1, Math.min(maxZoom(), z));
      // Zoom about the playhead, same as buildSpectrograms.
      const centreFrac = playhead / Math.max(1, total);
      spacer.style.width = `${(zoom * 100).toFixed(4)}%`;
      const denomNext = Math.max(1, spacer.offsetWidth - scrollbar.clientWidth);
      const targetFrac = zoom <= 1 ? 0 : (centreFrac - 0.5 / zoom) / (1 - 1 / zoom);
      scrollbar.scrollLeft = Math.min(1, Math.max(0, targetFrac)) * denomNext;
      schedulePaint();
    }

    // Click sets the playhead; vertical drag pans the frequency window (same
    // gesture split as buildSpectrograms' attachDrag). No horizontal-drag
    // realignment here -- there's nothing to align against.
    const DRAG_SLOP = 4;
    let dragFrom = 0, dragFromY = 0, dragBaseLo = 0, dragBaseHi = 0, dragActive = false;
    cSig.addEventListener("pointerdown", (ev) => {
      dragActive = true; dragFrom = ev.clientX; dragFromY = ev.clientY;
      dragBaseLo = fLo; dragBaseHi = fHi;
      try { cSig.setPointerCapture(ev.pointerId); } catch (_) { /* no capture */ }
      ev.preventDefault();
    });
    cSig.addEventListener("pointermove", (ev) => {
      if (!dragActive) return;
      const wLo0 = warpHz(dragBaseLo, logFreq), wHi0 = warpHz(dragBaseHi, logFreq);
      const dW = ((ev.clientY - dragFromY) * (wHi0 - wLo0)) / Math.max(1, cSig.clientHeight);
      panFreq(unwarpHz(wLo0 + dW, logFreq), unwarpHz(wHi0 + dW, logFreq));
    });
    const dragEnd = (ev) => {
      if (!dragActive) return;
      dragActive = false;
      try { cSig.releasePointerCapture(ev.pointerId); } catch (_) { /* fine */ }
      if (ev.type !== "pointerup") return;
      if (Math.abs(ev.clientX - dragFrom) > DRAG_SLOP || Math.abs(ev.clientY - dragFromY) > DRAG_SLOP) return;
      const x = ev.clientX - cSig.getBoundingClientRect().left;
      const perPx = visibleSamples() / Math.max(1, cSig.clientWidth);
      playhead = Math.max(0, Math.min(total - 1, Math.round(viewStart() + x * perPx)));
      schedulePaint();
    };
    cSig.addEventListener("pointerup", dragEnd);
    cSig.addEventListener("pointercancel", dragEnd);

    const contrast = el("input", {
      type: "range", min: "20", max: "120", step: "5",
      value: String(SPEC_DYNAMIC_RANGE_DB), class: "contrast",
    });
    contrast.oninput = () => { dynRange = Number(contrast.value); schedulePaint(); };
    const scaleBtn = mkBtn("linear", () => {
      logFreq = !logFreq;
      scaleBtn.textContent = logFreq ? "log" : "linear";
      schedulePaint();
    });
    const freqOutBtn = mkBtn("−", () => setFreqZoom(2));
    const freqInBtn = mkBtn("+", () => setFreqZoom(0.5));
    const freqFullBtn = mkBtn("Full", () => panFreq(0, NYQUIST));
    const viewBtn = mkBtn(mode, () => {
      mode = mode === "waveform" ? "spectrogram" : "waveform";
      viewBtn.textContent = mode;
      const disabled = mode === "waveform";
      contrast.disabled = disabled;
      scaleBtn.disabled = disabled;
      freqOutBtn.disabled = disabled;
      freqInBtn.disabled = disabled;
      freqFullBtn.disabled = disabled;
      schedulePaint();
    });
    const viewRow = el("div", { class: "ctlrow" }, [
      grp("zoom", [
        mkBtn("−", () => setZoom(zoom / ZOOM_STEP)),
        mkBtn("+", () => setZoom(zoom * ZOOM_STEP)),
        mkBtn("Fit", () => setZoom(1)),
        zoomVal,
      ]),
      grp("freq", [freqOutBtn, freqInBtn, freqFullBtn, scaleBtn]),
      grp("contrast", [contrast]),
      grp("view", [viewBtn]),
      rangeVal,
    ]);

    const playheadVal = el("span", { class: "rangeval" });
    const playingVal = el("span", { class: "playing-indicator" });
    const loopBox = el("input", { type: "checkbox", checked: "checked" });
    const followBox = el("input", { type: "checkbox" });
    const playRow = el("div", { class: "ctlrow" }, [
      grp("play", [
        mkBtn("▶ play", () => playFromHere()),
        mkBtn("■ stop", () => stopCurrent()),
        el("label", null, [loopBox, document.createTextNode(" loop")]),
        el("label", null, [followBox, document.createTextNode(" follow")]),
        playingVal,
        playheadVal,
      ]),
    ]);

    let cursorRaf = 0, cursorFrom = 0, cursorT0 = 0;
    function hideCursor() {
      if (cursorRaf) cancelAnimationFrame(cursorRaf);
      cursorRaf = 0;
      cursor.style.display = "none";
    }
    function onPlaybackStop() {
      hideCursor();
      playingVal.textContent = "";
    }
    // Scrolls the view so `sampleAt` sits at its centre -- same scrollLeft
    // math as viewStart()'s inverse. Firing the scrollbar's own "scroll"
    // listener repaints the canvas, so following just means "move the
    // scrollbar every frame"; no separate repaint path needed.
    function centerOn(sampleAt) {
      const maxStart = Math.max(0, total - visibleSamples());
      if (maxStart <= 0) return; // whole signal already fits on screen
      const desiredStart = Math.max(0, Math.min(maxStart, Math.round(sampleAt - visibleSamples() / 2)));
      const denom = Math.max(1, spacer.offsetWidth - scrollbar.clientWidth);
      const newScrollLeft = (desiredStart / maxStart) * denom;
      if (Math.abs(scrollbar.scrollLeft - newScrollLeft) > 0.5) scrollbar.scrollLeft = newScrollLeft;
    }
    function trackCursor() {
      cursorRaf = requestAnimationFrame(trackCursor);
      let at = cursorFrom + (getPlayCtx().currentTime - cursorT0) * sampleRate;
      // Native loop wraps the underlying buffer at loopStart/loopEnd; mirror
      // that here so the cursor doesn't run off the right edge forever.
      if (loopBox.checked && total > cursorFrom) {
        at = cursorFrom + ((at - cursorFrom) % (total - cursorFrom));
      }
      if (followBox.checked) centerOn(at);
      const x = ((at - viewStart()) / visibleSamples()) * viewport.clientWidth;
      if (x < 0 || x > viewport.clientWidth) { cursor.style.display = "none"; return; }
      cursor.style.display = "block";
      cursor.style.left = `${x}px`;
    }

    function playFromHere() {
      const src = playStereo(left, right, sampleRate, onPlaybackStop,
        { startSample: playhead, loop: loopBox.checked });
      if (!src) return;
      playingVal.textContent = loopBox.checked ? "▶ playing (looped)" : "▶ playing";
      if (cursorRaf) cancelAnimationFrame(cursorRaf);
      cursorFrom = playhead;
      cursorT0 = getPlayCtx().currentTime;
      trackCursor();
    }

    specWrap.appendChild(viewRow);
    specWrap.appendChild(playRow);
    specWrap.appendChild(viewport);
    specWrap.appendChild(scrollbar);

    scrollbar.addEventListener("scroll", schedulePaint);
    viewport.addEventListener("wheel", (ev) => {
      if (ev.shiftKey) {
        ev.preventDefault();
        setFreqZoom(ev.deltaY < 0 ? 0.5 : 2);
        return;
      }
      if (!ev.ctrlKey && !ev.metaKey) return;
      ev.preventDefault();
      setZoom(ev.deltaY < 0 ? zoom * ZOOM_STEP : zoom / ZOOM_STEP);
    }, { passive: false });

    parent.appendChild(specWrap);
    cSig.style.width = "100%";
    cSig.style.height = SPEC_CANVAS_HEIGHT + "px";
    spacer.style.width = "100%";
    // The canvas's backing store (a pixel count) doesn't track its CSS size
    // (a percentage) on its own -- a rotation or window resize changes
    // viewport.clientWidth without touching cSig.width, which would leave
    // the image stretched instead of redrawn at the new resolution.
    let lastW = 0;
    function resizeCanvas() {
      const w = Math.max(320, Math.round(viewport.clientWidth || 900));
      if (w === lastW) return false;
      lastW = w;
      cSig.width = w;
      cSig.height = SPEC_CANVAS_HEIGHT;
      return true;
    }
    resizeCanvas();
    new ResizeObserver(() => { if (resizeCanvas()) schedulePaint(); }).observe(viewport);
    paint();
    return specWrap;
  }

  function mkBtn(text, onclick) {
    const b = el("button", { text });
    b.onclick = onclick;
    return b;
  }

  function stat(label, value, title, key) {
    return el("span", { class: key ? `stat ${key}` : "stat", title },
      [el("i", { text: label }), el("b", { text: value })]);
  }

  // A labelled cluster of controls, ruled off from its neighbours by CSS.
  function grp(label, children) {
    return el("span", { class: "grp" }, [el("span", { class: "zoomlabel", text: label })].concat(children));
  }

  function renderPage(config) {
    const root = document.getElementById("items");
    const notice = document.getElementById("notice");
    // One term per trap from CLAUDE.adoc's "Measuring" section, plus the one
    // platform requirement -- four separate facts, so four separate rows.
    if (notice) {
      notice.innerHTML = `<dl>
        <dt>rate</dt><dd>references are decoded via <code>decodeAudioData</code> on a ${SYNTH_RATE}Hz
          <code>OfflineAudioContext</code>, so the platform resamples them to the synth's native
          ${SYNTH_RATE}Hz (verified against <code>decodedBuffer.sampleRate</code>).</dd>
        <dt>alignment</dt><dd>start delay is removed by RMS-envelope cross-correlation over a 50ms
          hop, searching ±5s of lag. The detected lag is shown per item.</dd>
        <dt>level</dt><dd>both signals are normalized to a common RMS before the spectral residual
          is computed, to neutralize reference dithering/gain.</dd>
        <dt>requires</dt><dd>browser-native FLAC decoding (Chrome/Firefox); references will not
          decode without it.</dd>
      </dl>`;
    }
    for (const item of config.items) root.appendChild(makeCard(item));
  }

  function makeCard(item) {
    const state = {}; // holds the memoized slopgs render for this item
    const card = el("div", { class: "card" });
    card.appendChild(el("h3", { text: item.title }));
    if (item.extraRows) card.appendChild(buildInfoTable(item.extraRows));
    const paths = el("p", { class: "paths" });
    paths.textContent = `midi: ${item.midiUrl}   flac: ${item.flacUrl}`;
    card.appendChild(paths);

    const status = el("p", { class: "status" });
    const results = el("div", { class: "results" });

    // No players here: nothing is playable until both signals exist and have
    // been aligned, and once they have, the transport lives under the
    // spectrogram (buildSpectrograms) where the playhead is.
    const cmpBtn = el("button", { text: "Load & compare" });
    // Unload drops the card's canvases AND the memoized render. A field item is
    // up to 240s of stereo float either side; with nine cards on the page,
    // keeping every one of them loaded is how the tab ends up in the gigabytes.
    const unloadBtn = el("button", { text: "Unload" });
    unloadBtn.disabled = true;
    unloadBtn.onclick = () => {
      stopCurrent();
      results.innerHTML = "";
      status.textContent = "";
      state.slopPromise = null;
      unloadBtn.disabled = true;
    };
    cmpBtn.onclick = () => {
      cmpBtn.disabled = true;
      runItem(item, state, status, results).catch((err) => {
        status.textContent = `unexpected error: ${err.message || err}`;
      }).finally(() => { cmpBtn.disabled = false; unloadBtn.disabled = false; });
    };
    card.appendChild(cmpBtn);
    card.appendChild(unloadBtn);
    card.appendChild(status);
    card.appendChild(results);
    return card;
  }

  return { renderPage, renderMidiBytes, buildSingleViewer, toMono };
})();
