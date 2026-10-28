// Smoke test for the browser build in headless Chromium (Playwright).
//
//   node web/smoke/smoke.cjs <page-url> <out-dir> [--phone [--dpr <n>]]
//        [--live <viron-url> --environment <id> --token-file <file> [--expect-rejected]]
//        [--public | --expect-fallback] [--site-dir <dir>]
//
// Demo mode (the default here; the page is opened with #demo): the page starts with no page errors and plays the
// built-in recording with no server: the poll loop runs, entities are placed,
// the recording advances, and the canvas draws (not one flat colour). A real
// click (or tap) on the canvas goes through SDL and strata_raycast_meshes and
// picks the same cell as the page's own pick call at that point (CSS pixels,
// no SDL), so SDL's pointer units are scaled right at the profile's device
// pixel ratio (1 on the desktop, 3 on the iPhone 13; --dpr overrides it).
//
// Live mode (--live): the page is opened with #viron=...&environment=...&token=...
// against a real Viron whose CORS allow-list names the page's origin, and must
// reach "live" with the environment's entities on screen; the token must be
// gone from the address bar. With --expect-rejected the token is a bad one and
// the page must say "token rejected" instead.
//
// Public world (--public): the page is opened with no fragment, as a visitor
// opens it, and must go live on the public Viron's "public-world" environment
// with entities moving, using only GET requests and never putting the token
// in the address bar. --expect-fallback opens it the same way where that
// Viron cannot be reached (or refuses this origin) and expects the demo with
// a note saying the public world is unavailable.
//
// --site-dir serves <dir> AS <page-url>'s origin (Playwright routing), so the
// page can be tested under its real origin, e.g. the arcade one a public
// Viron's CORS allow-list names, before it is deployed.
'use strict';
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');
const { chromium, devices } = require('playwright');

const argv = process.argv.slice(2);
const [url, outDir] = argv;
const flag = (name) => argv.includes(name);
const opt = (name) => { const i = argv.indexOf(name); return i > 0 ? argv[i + 1] : undefined; };
const phone = flag('--phone');
const dpr = opt('--dpr') ? Number(opt('--dpr')) : 0;
const live = opt('--live');
const liveEnv = opt('--environment');
const tokenFile = opt('--token-file');
const expectRejected = flag('--expect-rejected');
const publicWorld = flag('--public');
const expectFallback = flag('--expect-fallback');
const siteDir = opt('--site-dir');
if (!url || !outDir || (opt('--dpr') && !(dpr > 0)) || (live && (!liveEnv || !tokenFile))) {
  console.error('usage: smoke.cjs <url> <out-dir> [--phone [--dpr <n>]] [--live <viron-url> --environment <id> --token-file <f> [--expect-rejected]]');
  process.exit(2);
}
fs.mkdirSync(outDir, { recursive: true });

// Minimal PNG decoder (8-bit RGB/RGBA, non-interlaced: what Chromium writes).
function decodePng(buf) {
  let pos = 8, width = 0, height = 0, channels = 0;
  const idat = [];
  while (pos < buf.length) {
    const len = buf.readUInt32BE(pos);
    const type = buf.toString('ascii', pos + 4, pos + 8);
    const data = buf.subarray(pos + 8, pos + 8 + len);
    if (type === 'IHDR') {
      width = data.readUInt32BE(0);
      height = data.readUInt32BE(4);
      channels = { 2: 3, 6: 4 }[data[9]];
      if (data[8] !== 8 || !channels || data[12] !== 0) throw new Error('unsupported PNG');
    } else if (type === 'IDAT') idat.push(data);
    pos += 12 + len;
  }
  const raw = zlib.inflateSync(Buffer.concat(idat));
  const stride = width * channels;
  const out = Buffer.alloc(height * stride);
  for (let y = 0; y < height; y++) {
    const f = raw[y * (stride + 1)];
    for (let x = 0; x < stride; x++) {
      const v = raw[y * (stride + 1) + 1 + x];
      const a = x >= channels ? out[y * stride + x - channels] : 0;
      const b = y > 0 ? out[(y - 1) * stride + x] : 0;
      const c = x >= channels && y > 0 ? out[(y - 1) * stride + x - channels] : 0;
      let p;
      if (f === 0) p = 0;
      else if (f === 1) p = a;
      else if (f === 2) p = b;
      else if (f === 3) p = (a + b) >> 1;
      else {
        const pa = Math.abs(b - c), pb = Math.abs(a - c), pc = Math.abs(a + b - 2 * c);
        p = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
      }
      out[y * stride + x] = (v + p) & 255;
    }
  }
  return { width, height, channels, data: out };
}

// Distinct colours (quantised to 4 bits per channel) in a PNG.
function colourCount(png) {
  const img = decodePng(png);
  const seen = new Set();
  for (let i = 0; i < img.data.length; i += img.channels)
    seen.add(((img.data[i] >> 4) << 8) | ((img.data[i + 1] >> 4) << 4) | (img.data[i + 2] >> 4));
  return seen.size;
}

(async () => {
  const browser = await chromium.launch({ args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader'] });
  const base = phone ? { ...devices['iPhone 13'], ...(dpr ? { deviceScaleFactor: dpr } : {}) } : { viewport: { width: 1280, height: 800 } };
  const context = await browser.newContext({ ...base, userAgent: (base.userAgent || 'Mozilla/5.0') + ' viron-strata-smoke headless' });
  if (siteDir) {
    const origin = new URL(url).origin;
    const types = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.json': 'application/json' };
    await context.route(origin + '/**', (route) => {
      let p = new URL(route.request().url()).pathname;
      if (p.endsWith('/')) p += 'index.html';
      const file = path.join(siteDir, path.normalize(p).replace(/^([/\\])+/, ''));
      if (!fs.existsSync(file)) return route.fulfill({ status: 404, body: 'not found' });
      route.fulfill({ status: 200, body: fs.readFileSync(file), contentType: types[path.extname(file)] || 'application/octet-stream' });
    });
  }
  const page = await context.newPage();
  const errors = [];
  const methods = new Set();
  page.on('request', (r) => { if (!/^(data|blob):/.test(r.url())) methods.add(r.method()); });
  // A live world changes between requests: an entity seen in one poll can be
  // deleted by its driver before the client asks for its name, and Viron
  // answers that GET /entities/{id} with 404 (the client copes). Those are the
  // only 404s the public world may produce.
  const other404 = [];
  page.on('response', (r) => {
    if (r.status() === 404 && !(publicWorld && /\/api\/v1\/entities\/\d+$/.test(new URL(r.url()).pathname))) other404.push(r.url());
  });
  page.on('pageerror', (e) => errors.push(String(e)));
  page.on('console', (m) => {
    // A rejected token is answered 401, which Chromium logs as a failed resource load: expected there.
    // Where the public Viron is unreachable or refuses this origin, Chromium logs the failed fetch: expected there.
    const expected = (expectRejected && /401/.test(m.text())) ||
      (publicWorld && /status of 404/.test(m.text())) ||
      (expectFallback && /CORS|Failed to fetch|ERR_|net::/.test(m.text()));
    if (m.type() === 'error' && !expected) errors.push('console: ' + m.text());
  });
  const fail = async (why) => {
    console.error('FAIL: ' + why);
    await page.screenshot({ path: path.join(outDir, 'failure.png') }).catch(() => {});
    await browser.close();
    process.exit(1);
  };
  const status = () => page.evaluate(() => window.vironStrata.status());
  const until = async (what, pred, ms) => {
    const end = Date.now() + ms;
    let s;
    while (Date.now() < end) {
      s = await status();
      if (pred(s)) return s;
      await page.waitForTimeout(250);
    }
    await fail(`${what} (last status: ${JSON.stringify(s)})`);
  };
  const canvas = page.locator('#canvas');

  let target = publicWorld || expectFallback ? url : url + '#demo';
  if (live) {
    const token = fs.readFileSync(tokenFile, 'utf8').trim();
    target = `${url}#viron=${encodeURIComponent(live)}&environment=${encodeURIComponent(liveEnv)}&token=${encodeURIComponent(token)}`;
  }
  // The main loop keeps the load event from ever firing: wait for commit only.
  await page.goto(target, { waitUntil: 'commit' });
  try {
    await page.waitForFunction(() => window.vironStrata, null, { timeout: 90000 });
  } catch (e) {
    await fail('the viewer never became ready: ' + errors.join(' | '));
  }

  if (publicWorld || expectFallback) {
    if (expectFallback) {
      const s = await until('with the public world unreachable, the page should fall back to the demo', (s) => s.mode === 'demo' && s.state === 'live' && s.entities > 0, 40000);
      const noteText = await page.evaluate(() => window.vironStrata.note());
      if (!/Public world unavailable/.test(noteText)) await fail('the fallback should say why: note is ' + JSON.stringify(noteText));
      console.log(`fallback: demo, ${s.entities} entities; note: ${noteText}`);
    } else {
      const s1 = await until('the page should go live on the public world', (s) => s.mode === 'live' && s.state === 'live' && s.entities > 0 && s.environment === 'public-world', 60000);
      if (await page.evaluate(() => window.vironStrata.source()) !== 'public') await fail('the source should be the public world');
      const before = await page.evaluate(() => window.vironStrata.status());
      const s2 = await until('polls should keep coming', (s) => s.poll >= s1.poll + 8, 30000);
      console.log(`public: ${s2.environment} #${s2.environmentId}, ${s2.entities} entities on ${s2.grids} grids, poll ${s2.poll}, ${s2.requests} requests: ${s2.line}`);
      const shot = await canvas.screenshot({ path: path.join(outDir, 'public.png'), timeout: 180000 });
      const colours = colourCount(shot);
      console.log(`public: ${colours} colours (state line ${JSON.stringify(before.line)})`);
      if (colours < 30) await fail(`the public view drew only ${colours} colours`);
      const href = await page.evaluate(() => location.href);
      if (/eyJ/.test(href)) await fail('a token is in the address bar');
      const writes = [...methods].filter((m) => m !== 'GET');
      if (writes.length) await fail('the page sent ' + writes.join(', '));
    }
    if (other404.length) await fail('unexpected 404s: ' + other404.join(' '));
    if (errors.length) await fail('page errors: ' + errors.join(' | '));
    await page.screenshot({ path: path.join(outDir, publicWorld ? 'page-public.png' : 'page-fallback.png') });
    await browser.close();
    console.log('ok');
    return;
  }

  if (live) {
    const href = await page.evaluate(() => location.href);
    if (/token=/.test(href)) await fail('the token is still in the address bar: ' + href.replace(/token=[^&]*/, 'token=…'));
    if (expectRejected) {
      const s = await until('a bad token should stop the page with "token rejected"', (s) => s.mode === 'live' && s.state === 'token rejected', 30000);
      console.log(`live (bad token): ${s.state}: ${s.line}`);
    } else {
      const s1 = await until('the page should go live against the Viron', (s) => s.mode === 'live' && s.state === 'live' && s.entities > 0, 60000);
      const s2 = await until('polls should keep coming', (s) => s.poll >= s1.poll + 4, 30000);
      console.log(`live: ${s2.environment} #${s2.environmentId}, ${s2.entities} entities on ${s2.grids} grids, poll ${s2.poll}, ${s2.requests} requests: ${s2.line}`);
      const shot = await canvas.screenshot({ path: path.join(outDir, 'live.png'), timeout: 180000 });
      const colours = colourCount(shot);
      console.log(`live: ${colours} colours`);
      if (colours < 30) await fail(`the live view drew only ${colours} colours`);
    }
    if (errors.length) await fail('page errors: ' + errors.join(' | '));
    await page.screenshot({ path: path.join(outDir, 'page-live.png') });
    await browser.close();
    console.log('ok');
    return;
  }

  // ---- demo mode: no server, no token
  const s1 = await until('demo mode should start and go live on the recording', (s) => s.mode === 'demo' && s.state === 'live' && s.entities >= 8 && s.grids >= 2, 30000);
  const s2 = await until('the recording should advance', (s) => s.frame !== s1.frame && s.poll > s1.poll + 2, 15000);
  console.log(`demo: ${s2.environment}, ${s2.entities} entities on ${s2.grids} grids, frame ${s2.frame + 1}/${s2.frames}, poll ${s2.poll}`);
  const shot = await canvas.screenshot({ path: path.join(outDir, 'demo.png'), timeout: 180000 });
  const colours = colourCount(shot);
  console.log(`demo: ${colours} colours`);
  if (colours < 30) await fail(`demo drew only ${colours} colours`);

  // Page buttons go through to the view without errors.
  for (const sel of ['#zoomIn', '#zoomOut', '[data-key="20"]', '[data-key="8"]', '[data-key="9"]']) await page.locator(sel).click();
  await page.waitForTimeout(300);

  // A click or tap picks the cell the page's own pick call finds at that
  // point. SDL reports the pointer in its window's units, which on a phone
  // with a capped device pixel ratio (3, capped at 2) are not CSS pixels: a
  // wrong scale picks a cell up and to the left, or nothing (asn1-viz#9).
  // Only points well inside a cell are used: the direct pick must agree 3 CSS
  // pixels either way, so touch rounding at a cell edge cannot fail the check.
  const devicePixelRatio = await page.evaluate(() => window.devicePixelRatio);
  await page.locator('[data-key="9"]').click();  // frame every grid again
  await page.waitForTimeout(300);
  const box = await canvas.boundingBox();
  const key = (p) => (p ? `${p.grid}:${p.x},${p.y}` : '(nothing)');
  const pickDirect = (x, y) => page.evaluate(([px, py]) => {
    window.vironStrata.click(px, py);
    return window.vironStrata.status().pick;
  }, [x, y]);
  let probed = 0;
  for (const fy of [0.45, 0.55, 0.65]) {
    for (let fx = 0.15; fx < 0.9 && probed < 4; fx += 0.14) {
      const cx = Math.round(box.width * fx), cy = Math.round(box.height * fy);
      const want = key(await pickDirect(cx, cy));
      if (want === '(nothing)') continue;
      let solid = true;
      for (const [dx, dy] of [[-3, 0], [3, 0], [0, -3], [0, 3]]) {
        if (key(await pickDirect(cx + dx, cy + dy)) !== want) { solid = false; break; }
      }
      if (!solid) continue;
      await pickDirect(1, 1);  // clear (the corner is off the grids)
      const x = box.x + cx, y = box.y + cy;
      if (phone) await page.touchscreen.tap(x, y);
      else await page.mouse.click(x, y);
      await page.waitForTimeout(500);
      const got = key((await status()).pick);
      console.log(`pointer at DPR ${devicePixelRatio}: (${cx}, ${cy}) direct ${want}, ${phone ? 'tap' : 'click'} ${got}`);
      if (got !== want) await fail(`a ${phone ? 'tap' : 'click'} at (${cx}, ${cy}) picked ${got}, but that point is cell ${want} (DPR ${devicePixelRatio})`);
      probed++;
    }
  }
  if (probed < 2) await fail(`only ${probed} points well inside a cell were found to check the pointer with`);

  // The picked cell is listed (on a phone, in the sheet under the canvas).
  const picked = await status();
  if (!picked.pick) await fail('nothing is picked after the pointer checks');
  if (phone && !(await page.locator('#sheet').isVisible())) await fail('on a phone the picked cell should be listed under the canvas');

  // The loading shell finished; and without WebGL 2 it shows the trailer (or
  // its still) and says why, and the engine never downloads.
  {
    await page.waitForFunction(() => { const r = document.querySelector('.sk-loader'); return r && r.hidden; }, null, { timeout: 10000 }).catch(() => {});
    const shell = await page.evaluate(() => {
      const r = document.querySelector('.sk-loader');
      return { hidden: !!(r && r.hidden), state: window.StrataLoader && StrataLoader.state };
    });
    if (!shell.hidden || shell.state !== 'done') await fail('the loading shell did not finish: ' + JSON.stringify(shell));
    console.log('ok: the loading shell showed and is gone');
    const bare = await browser.newContext({ ...base, userAgent: (base.userAgent || 'Mozilla/5.0') + ' viron-strata-smoke headless' });
    await bare.addInitScript(() => {
      const get = HTMLCanvasElement.prototype.getContext;
      HTMLCanvasElement.prototype.getContext = function (t, o) { return t === 'webgl2' ? null : get.call(this, t, o); };
    });
    const p2 = await bare.newPage();
    const engine = [];
    p2.on('request', (r) => { if (/viron-strata\.(wasm|js)$/.test(r.url())) engine.push(r.url()); });
    await p2.goto(url, { waitUntil: 'commit' });
    await p2.waitForFunction(() => window.StrataLoader && StrataLoader.state === 'fallback', null, { timeout: 30000 });
    const fb = await p2.evaluate(() => {
      const r = document.querySelector('.sk-loader');
      return { text: r.innerText, media: !!r.querySelector('video, img'), wide: document.documentElement.scrollWidth > innerWidth };
    });
    await p2.screenshot({ path: path.join(outDir, 'no-webgl2.png') });
    if (!/WebGL 2/.test(fb.text) || fb.wide) await fail('without WebGL 2 the shell did not say why: ' + JSON.stringify(fb));
    if (engine.length) await fail('without WebGL 2 the engine still downloaded: ' + engine.join(', '));
    console.log('ok: without WebGL 2 the shell ' + (fb.media ? 'shows the trailer and ' : '') + 'says why, and nothing of the engine downloads');
    await bare.close();
  }

  if (errors.length) await fail('page errors: ' + errors.join(' | '));
  await page.screenshot({ path: path.join(outDir, 'page.png') });
  await browser.close();
  console.log('ok');
})().catch((e) => { console.error(e); process.exit(1); });
