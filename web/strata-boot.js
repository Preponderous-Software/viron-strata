/* strata-boot.js: starts this page's Emscripten build behind the Strata web
   kit's loading shell (strata-kit/strata-loader.js, vendored at a pinned
   Strata commit).

   The loader expects a MODULARIZE build (a factory it calls with wasmBinary).
   This build is a classic one: a global Module and the glue script that
   emcc writes for {{{ SCRIPT }}}, which the shell keeps inside
   <template id="strata-glue"> so that it does not run by itself. This file
   bridges the two:

   - the glue starts at once, as before, so a --preload-file data package
     downloads alongside the engine; it waits in Module.instantiateWasm;
   - the loader fetches the .wasm as a stream, shows its bytes, and hands
     them to the factory below, which instantiates them: nothing downloads
     twice;
   - an abort, a lost WebGL context that does not come back, and (outside
     automated browsers and ?bench) a device drawing under 12 frames a
     second all end in the loader's fallback: the trailer or the poster,
     with the reason.

     StrataBoot.start({
       wasm: "index.wasm", title: "...",        // the loader's options, plus:
       canvas: document.getElementById("canvas"),
     });
     // when the first frame is up (the page's own "ready"):
     StrataBoot.ready();

   `bytes` defaults to data-bytes on the template, which the build stamps
   (web/strata_page.cmake): the .wasm plus any data package, since the arcade
   compresses on the fly and sends no Content-Length.

   MIT licensed, as this repository. */
(function () {
  "use strict";

  const FACTORY = "__strataBootFactory";
  let opts = {}, running = false, settled = false, failNow = null;
  let wasmLen = null, data = null;

  // A data package downloads alongside the .wasm. The loader counts the .wasm;
  // once it has, the bar goes on with the package's bytes on top, against the
  // page's stated size (both together). This writes into the loader's own
  // overlay (.sk-bar, .sk-fill, .sk-size, as strata-loader.js at PIN builds it).
  function mb(n) { return (n / 1048576).toFixed(n < 10485760 ? 1 : 0); }
  function showData() {
    if (wasmLen == null || !data || StrataLoader.state !== "loading") return;
    const bar = document.querySelector(".sk-loader .sk-bar");
    if (!bar) return;
    const stated = opts.bytes || 0;
    const sane = data.got <= data.total;  // a compressed Content-Length is smaller than the bytes read
    const total = sane ? Math.max(stated, wasmLen + data.total) : 0;
    const got = wasmLen + data.got;
    const size = document.querySelector(".sk-loader .sk-size");
    if (total) {
      const pct = Math.min(100, Math.round(got / total * 100));
      bar.classList.remove("sk-indeterminate");
      bar.querySelector(".sk-fill").style.width = pct + "%";
      bar.setAttribute("aria-valuenow", String(pct));
      if (size) size.textContent = `${mb(Math.min(got, total))} of ${mb(total)} MB`;
    } else if (size) {
      size.textContent = `${mb(got)} MB`;
    }
  }

  // The glue's src, from the inert copy of emcc's script tag.
  function glueSrc() {
    const t = document.getElementById("strata-glue");
    const s = t && t.content && t.content.querySelector("script[src]");
    return s ? s.getAttribute("src") : null;
  }

  function engineFailed(message) {
    if (settled) return;
    if (failNow) { failNow(new Error(message)); return; }  // still loading: the loader shows it
    settled = true;
    StrataLoader.fallback("engine", message ? `The live view's renderer stopped. (${message})` : undefined);
  }

  // Links on the fallback leave a framed page (the arcade embeds games) as a whole.
  function topLinks() {
    document.querySelectorAll(".sk-loader a[href]").forEach((a) => { a.target = "_top"; a.rel = "noopener"; });
  }

  function watchContext(canvas) {
    if (!canvas) return;
    let timer = null;
    canvas.addEventListener("webglcontextlost", () => {
      clearTimeout(timer);
      timer = setTimeout(() => { if (!settled) { settled = true; StrataLoader.fallback("context-lost"); } }, 3000);
    });
    canvas.addEventListener("webglcontextrestored", () => clearTimeout(timer));
  }

  // Under 12 frames a second over 8 s of visible time, 3 s after the first
  // frame: offer the clip without taking the live view away. Automated
  // browsers (software GL) and ?bench runs are left alone.
  function watchSpeed() {
    if (navigator.webdriver || /[?&]bench\b/.test(location.search)) return;
    let frames = 0, visibleMs = 0, last = 0;
    document.addEventListener("visibilitychange", () => { last = 0; });
    function step(now) {
      if (settled) return;
      if (last && !document.hidden) { const dt = now - last; if (dt < 1000) { visibleMs += dt; frames++; } }
      last = now;
      if (visibleMs < 8000) { requestAnimationFrame(step); return; }
      if (frames * 1000 / visibleMs < 12) StrataLoader.slow();
    }
    setTimeout(() => requestAnimationFrame(step), 3000);
  }

  function start(o) {
    opts = Object.assign({}, o);
    const src = glueSrc();
    if (!src) throw new Error("strata-boot: put <template id=\"strata-glue\"> with emcc's script tag before this call");
    const M = window.Module = window.Module || {};
    const userAbort = M.onAbort;
    M.onAbort = (what) => { if (userAbort) userAbort(what); engineFailed(String(what || "aborted")); };
    const userFallback = opts.onFallback;
    opts.onFallback = (code) => { settled = true; topLinks(); if (userFallback) userFallback(code); };

    // The glue asks for its instance here; the factory below answers once
    // the loader has the bytes. The factory settles with the instantiation.
    let give;
    const instance = new Promise((r) => { give = r; });
    M.instantiateWasm = (imports, ok) => {
      instance.then((b) => WebAssembly.instantiate(b, imports))
        .then((r) => { failNow = null; ok(r.instance, r.module); if (resolveFactory) resolveFactory(M); },
              (e) => engineFailed(String(e && e.message || e)));
      return {};
    };
    // A --preload-file package reports through setStatus("... (got/total)").
    const userStatus = M.setStatus;
    M.setStatus = (text) => {
      if (userStatus) userStatus(text);
      const m = /\((\d+)\/(\d+)\)/.exec(text || "");
      if (m) { data = {got: +m[1], total: +m[2]}; showData(); }
    };

    let resolveFactory = null;
    window[FACTORY] = (args) => new Promise((resolve, reject) => {
      failNow = reject;
      resolveFactory = resolve;
      wasmLen = args.wasmBinary.byteLength;
      showData();
      give(args.wasmBinary);
    });

    // No WebGL 2: the loader shows the fallback before anything downloads.
    if (StrataLoader.hasWebGL2()) {
      const s = document.createElement("script");
      s.src = src;
      s.async = true;
      s.onerror = () => {
        if (settled) return;
        settled = true;
        StrataLoader.fallback("network", `The live view didn't download. (${src})`);
      };
      document.body.appendChild(s);
    }
    running = true;
    watchContext(opts.canvas);
    if (!opts.bytes) {
      const t = document.getElementById("strata-glue");
      const n = t ? parseInt(t.getAttribute("data-bytes") || "", 10) : 0;
      if (n > 0) opts.bytes = n;
    }
    const loadOpts = Object.assign({}, opts, {script: src, factory: FACTORY});
    delete loadOpts.canvas;
    return StrataLoader.load(loadOpts).then((mod) => mod, (e) => { settled = true; throw e; });
  }

  // The first frame is up: fade the shell out and start watching the speed.
  function ready() {
    if (!running || settled) return;
    StrataLoader.done();
    watchSpeed();
  }

  window.StrataBoot = {start, ready};
})();
