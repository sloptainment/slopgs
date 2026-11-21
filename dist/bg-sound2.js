/*
 * bg-sound2.js -- replacement for the browserify'd asset/bg-sound.js.
 *
 * Drives msgs.wasm (a freestanding wasm32 module, no imports, no libc) instead
 * of the old bundled libtimidity.js/.wasm decoder. Registers the same custom
 * element name, "bg-sound", with the same src/loop property-over-attribute
 * contract that midi/index.html already uses:
 *
 *   bg_sound = document.createElement("bg-sound");
 *   bg_sound.src  = absoluteUrl;   // property setter -> setAttribute("src", ...)
 *   bg_sound.loop = "-1";          // property setter -> setAttribute("loop", "-1")
 *   document.body.appendChild(bg_sound);        // starts playback
 *   ...
 *   bg_sound.parentElement.removeChild(bg_sound); // stops playback
 *
 * Loop-continuation semantics are copied verbatim from the original
 * asset/bg-sound.js (`_onEnded`):
 *
 *   const e = (this.loop + "").toLowerCase();
 *   ("infinite" === e || "true" === e || "-1" === e || +this.loop > this.playCount)
 *     && this._initPlayer();
 *
 * i.e. playCount increments *after* each full playthrough, then the element
 * decides whether to play again by string-comparing the (lowercased) loop
 * attribute against "infinite"/"true"/"-1", or by numeric comparison against
 * the running play count. This is JS-level bookkeeping, independent of
 * whatever msgs_set_loop() means inside the wasm -- see NOTE(loop) below.
 *
 * Plain classic <script>, no build step, no bundler, no imports.
 */
"use strict";

(() => {
  // This must run synchronously at parse time: document.currentScript is
  // only valid while this classic script is the one being evaluated.
  const SCRIPT_URL = document.currentScript ? document.currentScript.src : location.href;
  const ASSET_BASE = new URL(".", SCRIPT_URL);
  const WASM_URL = new URL("msgs.wasm", ASSET_BASE).href;
  const DLS_URL = new URL("gm.dls", ASSET_BASE).href;

  // Native output format is fixed by the ABI: stereo int16 @ 22050 Hz.
  const SYNTH_RATE = 22050;
  // 0.5s chunks: coarse enough to keep overhead low, fine enough that the
  // top-up timer (below) has plenty of chances to keep the lookahead full.
  const RENDER_CHUNK_FRAMES = Math.floor(SYNTH_RATE * 0.5);
  const LOOKAHEAD_SECONDS = 1.5; // within the briefed 0.5-2s range
  const TOPUP_INTERVAL_MS = 200;

  const REQUIRED_EXPORTS = [
    "msgs_abi_version", "msgs_mem_size", "msgs_alloc", "msgs_size",
    "msgs_create", "msgs_init",
    "msgs_reset", "msgs_load_smf", "msgs_set_loop", "msgs_render",
    "msgs_is_finished", "msgs_midi", "memory",
  ];

  // Cache Storage persists across page loads (unlike synthPromise below,
  // which only dedupes fetches within one page's lifetime). Bump this name
  // whenever msgs.wasm/gm.dls change content at the same URL, since nothing
  // else here invalidates a stale cached entry.
  const ASSET_CACHE_NAME = "slopgs-20261121";

  async function cachedFetch(url) {
    if (typeof caches !== "object") return fetch(url); // no Cache Storage (e.g. insecure context)
    const cache = await caches.open(ASSET_CACHE_NAME);
    const hit = await cache.match(url);
    if (hit) return hit;
    const resp = await fetch(url);
    if (resp.ok) cache.put(url, resp.clone()).catch(() => {});
    return resp;
  }

  // ---------------------------------------------------------------------
  // Singleton synth: one wasm instance, one gm.dls load, shared by every
  // bg-sound element for the lifetime of the page. Every element drives the
  // same engine instance, so only one bg-sound element is ever meant to be
  // actively driving it -- which matches how index.html always removes the
  // previous element before appending a new one.
  // ---------------------------------------------------------------------
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
      if (typeof WebAssembly !== "object") {
        throw new Error("WebAssembly is not supported in this browser");
      }

      let wasmBytes;
      try {
        const resp = await cachedFetch(WASM_URL);
        if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
        wasmBytes = await resp.arrayBuffer();
      } catch (err) {
        throw new Error(`could not fetch msgs.wasm from ${WASM_URL}: ${err.message || err}`);
      }

      let instance;
      try {
        // msgs.wasm is freestanding and imports nothing -- an empty import
        // object is correct. If instantiation ever needs an import object,
        // that is a defect in the wasm module, not something to patch here.
        ({ instance } = await WebAssembly.instantiate(wasmBytes, {}));
      } catch (err) {
        throw new Error(`msgs.wasm failed to instantiate: ${err.message || err}`);
      }

      const exp = instance.exports;
      if (typeof exp.__wasm_call_ctors === "function") exp.__wasm_call_ctors();

      for (const name of REQUIRED_EXPORTS) {
        if (!(name in exp)) throw new Error(`msgs.wasm is missing required export "${name}"`);
      }

      const abiVersion = exp.msgs_abi_version() >>> 0;
      if (abiVersion !== 2) {
        throw new Error(`msgs.wasm reports ABI version ${abiVersion}, expected 2`);
      }

      // The one thing this file cannot infer: which RESAMPLE_FACTOR (voice.h)
      // the fetched module was built at. A mismatch is silent otherwise --
      // every song plays at the wrong speed -- so refuse to run instead. See
      // compare.js's loadSynth() for the sibling fix. The staleness vector
      // here is this file's own Cache Storage layer (cachedFetch), not the
      // HTTP cache, so the fix is bumping ASSET_CACHE_NAME above, not a
      // fetch-level cache option.
      const wasmRate = exp.msgs_sample_rate ? exp.msgs_sample_rate() >>> 0 : 0;
      if (wasmRate !== SYNTH_RATE) {
        throw new Error(
          `msgs.wasm renders at ${wasmRate || "an unreported rate"}Hz but bg-sound2.js `
          + `is configured for ${SYNTH_RATE}Hz -- audio would play at `
          + `${wasmRate ? (SYNTH_RATE / wasmRate).toFixed(2) : "?"}x speed. Rebuild the wasm at a `
          + `matching RESAMPLE_FACTOR, set SYNTH_RATE to ${wasmRate}, or bump ASSET_CACHE_NAME if `
          + `a stale cached module is the cause.`
        );
      }

      let dlsBytes;
      const dlsStart = performance.now();
      try {
        const resp = await cachedFetch(DLS_URL);
        if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
        dlsBytes = new Uint8Array(await resp.arrayBuffer());
      } catch (err) {
        throw new Error(`could not fetch gm.dls from ${DLS_URL}: ${err.message || err}`);
      }
      const dlsLoadMs = performance.now() - dlsStart;

      // msgs_alloc is a bump allocator that grows the module's own linear
      // memory as needed (see msgs_mem_size/memory.grow) -- the host does
      // not need to grow memory itself, just ask for space and write into
      // whatever offset comes back. Re-view the buffer after every alloc:
      // memory.grow() detaches any prior ArrayBuffer view.
      const dlsPtr = exp.msgs_alloc(dlsBytes.length);
      new Uint8Array(exp.memory.buffer, dlsPtr, dlsBytes.length).set(dlsBytes);

      // One reusable output scratch buffer for every render() call, for
      // every song, for the rest of the page's life. Allocated once here
      // (not per chunk, not per song) so the bump allocator never grows
      // memory again for ordinary playback -- see NOTE(alloc) in _pump.
      const outPtr = exp.msgs_alloc(RENDER_CHUNK_FRAMES * 4); // stereo int16 = 4 bytes/frame

      const synth = { exp, outPtr, dlsLoadMs, dlsBytes: dlsBytes.length,
                      dlsPtr, dlsLen: dlsBytes.length, e: 0, cap: -1 };
      try {
        ensureCapacity(synth, 0);
      } catch (err) {
        throw new Error(`${err.message}; is ${DLS_URL} a valid gm.dls?`);
      }
      return synth;
    })();
    return synthPromise;
  }

  // ---------------------------------------------------------------------
  // <bg-sound>
  // ---------------------------------------------------------------------
  class BgSound extends HTMLElement {
    static get observedAttributes() {
      return ["src", "baseUrl", "loop"];
    }

    constructor() {
      super();
      this.playCount = 0;
      this._token = 0; // bumped on every teardown; invalidates in-flight async work
      this._ctx = null;
      this._timer = null;
      this._sources = [];
      this._synth = null;
      this._smfBytes = null;
    }

    // src/loop/baseUrl are properties backed by attributes, exactly like the
    // original: index.html does `bg_sound.src = url` and `bg_sound.loop =
    // "-1"` as plain property assignment, so these must be real accessors.
    get src() { return this.getAttribute("src"); }
    set src(v) { this.setAttribute("src", v); }

    get loop() { return this.getAttribute("loop"); }
    set loop(v) { this.setAttribute("loop", v); }

    // baseUrl is part of the original contract (observedAttributes lists
    // it) but this glue has no use for it: msgs.wasm/gm.dls are located
    // relative to this script's own URL, not a configurable base. Kept as
    // an inert accessor only so existing callers that set/read it don't
    // break.
    get baseUrl() { return this.getAttribute("baseUrl"); }
    set baseUrl(v) { this.setAttribute("baseUrl", v); }

    attributeChangedCallback() {
      // Intentionally inert (matches the original, whose callback body was
      // dead code past a debug log once minified).
    }

    connectedCallback() {
      if (!this.hasAttribute("loop")) this.setAttribute("loop", "1");
      this.playCount = 0;
      const token = ++this._token;

      // Audio playback requires a user gesture. index.html creates and
      // appends this element synchronously inside a click handler, so this
      // is that gesture: create the AudioContext and call resume() here,
      // synchronously, before any `await` -- not after loadSynth() below,
      // which would run past the end of the gesture's call stack.
      const Ctx = window.AudioContext || window.webkitAudioContext;
      if (!Ctx) { this._fail(new Error("AudioContext is not supported in this browser")); return; }
      const ctx = new Ctx();
      this._ctx = ctx;
      ctx.resume().catch((err) => {
        console.warn("bg-sound: AudioContext.resume() rejected, will retry implicitly on playback:", err);
      });

      this._start(token, ctx).catch((err) => this._fail(err));
    }

    disconnectedCallback() {
      this._teardown();
    }

    async _start(token, ctx) {
      const synth = await loadSynth();
      if (token !== this._token) return; // disconnected while loading synth/gm.dls
      this._synth = synth;

      const resp = await fetch(this.src);
      if (!resp.ok) throw new Error(`HTTP ${resp.status} fetching ${this.src}`);
      const smfBytes = new Uint8Array(await resp.arrayBuffer());
      if (token !== this._token) return; // disconnected while loading the MIDI file
      this._smfBytes = smfBytes;

      this._loadSong(smfBytes);

      this._nextStartTime = ctx.currentTime;
      this._pump(token);
      this._timer = setInterval(() => this._pump(token), TOPUP_INTERVAL_MS);
    }

    _loadSong(smfBytes) {
      ensureCapacity(this._synth, smfBytes.length);
      const { exp, e } = this._synth;
      exp.msgs_reset(e);
      const ptr = exp.msgs_alloc(smfBytes.length);
      new Uint8Array(exp.memory.buffer, ptr, smfBytes.length).set(smfBytes);
      const ret = exp.msgs_load_smf(e, ptr, smfBytes.length) | 0;
      if (ret !== 0) throw new Error(`msgs_load_smf failed (code ${ret}) for ${this.src}`);
      // NOTE(loop): msgs_set_loop's own -1/0 semantics are a separate,
      // wasm-internal concept from this element's loop attribute. Looping
      // here is driven entirely at the JS level (see _shouldLoopAgain),
      // exactly like the original bg-sound's _onEnded/_initPlayer, so every
      // song is loaded as "play once" from the wasm's point of view.
      exp.msgs_set_loop(e, 0);
    }

    _shouldLoopAgain() {
      const e = (this.loop + "").toLowerCase();
      return e === "infinite" || e === "true" || e === "-1" || +this.loop > this.playCount;
    }

    _pump(token) {
      if (token !== this._token) return;
      const ctx = this._ctx;
      const synth = this._synth;
      if (!ctx || !synth) return;
      const { exp, outPtr, e } = synth;

      // If a throttled/backgrounded timer let us fall behind the context
      // clock, resync instead of trying to catch up all at once.
      if (this._nextStartTime < ctx.currentTime) this._nextStartTime = ctx.currentTime;

      while (this._nextStartTime - ctx.currentTime < LOOKAHEAD_SECONDS) {
        // NOTE(alloc): outPtr is allocated once per process (in loadSynth)
        // and reused for every chunk of every song -- msgs_alloc is a bump
        // allocator with no free, so allocating fresh here would grow wasm
        // memory forever on an infinite loop (loop="-1" is exactly what
        // index.html always sets). Looping a song reuses the same buffer
        // and calls msgs_reset() rather than reallocating/reloading.
        const n = exp.msgs_render(e, outPtr, RENDER_CHUNK_FRAMES) >>> 0;
        if (n > 0) {
          const pcm = new Int16Array(exp.memory.buffer, outPtr, n * 2);
          const buf = ctx.createBuffer(2, n, SYNTH_RATE);
          const l = buf.getChannelData(0);
          const r = buf.getChannelData(1);
          for (let i = 0; i < n; i++) {
            l[i] = pcm[2 * i] / 32768;
            r[i] = pcm[2 * i + 1] / 32768;
          }
          const node = ctx.createBufferSource();
          node.buffer = buf;
          node.connect(ctx.destination);
          node.start(this._nextStartTime);
          node.onended = () => {
            const idx = this._sources.indexOf(node);
            if (idx !== -1) this._sources.splice(idx, 1);
          };
          this._sources.push(node);
          this._nextStartTime += n / SYNTH_RATE;
        }

        if (n === 0 || exp.msgs_is_finished(e)) {
          this.playCount += 1;
          if (this._shouldLoopAgain()) {
            exp.msgs_reset(e); // rewind the already-loaded song, no realloc
          } else {
            clearInterval(this._timer);
            this._timer = null;
            return; // done; existing scheduled buffers finish playing out
          }
        }
      }
    }

    _fail(err) {
      console.error("bg-sound:", err && err.message ? err.message : err);
      this.dispatchEvent(new CustomEvent("bg-sound-error", {
        detail: String(err && err.message ? err.message : err),
        bubbles: true,
      }));
      this._teardown();
    }

    _teardown() {
      this._token++; // invalidate any in-flight fetch/instantiate continuations
      if (this._timer !== null) {
        clearInterval(this._timer);
        this._timer = null;
      }
      for (const src of this._sources) {
        try { src.stop(); } catch (_) { /* already stopped/ended */ }
        try { src.disconnect(); } catch (_) { /* already disconnected */ }
      }
      this._sources.length = 0;
      if (this._ctx) {
        const ctx = this._ctx;
        this._ctx = null;
        // Closing the context stops all output immediately and releases
        // the audio hardware/thread -- this is what actually guarantees
        // disconnectedCallback fully stops and releases audio, not just
        // the individual node.stop() calls above.
        ctx.close().catch(() => {});
      }
    }
  }

  window.customElements.define("bg-sound", BgSound);
})();
