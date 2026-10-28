// Runs the network installer page's packing and installing code
// (ghostldr/data/netinstall.html, between GFS-BEGIN and GFS-END) in node
// for ghostldr/tests/test_netinstall.py.  stdin: one JSON request; stdout:
// one JSON answer.
//
//   node netinstall_node.js PAGE < request.json
//
// ops: "pack" (a file list, as gfs_pack.pack lays it out), "install" and
// "remove" (against a fake console following gfs_receiver.lua's rules, as
// test_gfs_install.py's FakeConsole does), "zip" (a .zip read as a source:
// its files, and every container's bytes), "http" (a whole install against
// a real server over HTTP, as the page does it).
"use strict";
const fs = require("fs");
const html = fs.readFileSync(process.argv[2], "utf8");
const core = html.match(/\/\*GFS-BEGIN[^\n]*\n([\s\S]*?)const GFS = makeGFS\(\);\n\/\*GFS-END\*\//)[1];
const prof = html.match(/\/\*PROFILES-BEGIN[^\n]*\*\/\n([\s\S]*?)\/\*PROFILES-END\*\//)[1];
const makeGFS = new Function(core + "\nreturn makeGFS;")();
const PROFILES = new Function(prof + "\nreturn PROFILES;")();
const req = JSON.parse(fs.readFileSync(0, "utf8"));
const G = makeGFS({cap: req.cap});
const hex = b => Buffer.from(b).toString("hex");
const unhex = s => new Uint8Array(Buffer.from(s, "hex"));
const NO_SPACE = 0x809F000A, REFUSED = 0xFFFFFFFD;

class FakeConsole {
  constructor(c) {
    this.saves = {};
    for (const [d, files] of Object.entries(c.saves || {})) {
      this.saves[d] = {};
      for (const [f, h] of Object.entries(files)) this.saves[d][f] = unhex(h);
    }
    this.blocks = Object.assign({}, c.blocks || {});
    this.capacity = c.capacity || 2 ** 40;
    this.failPut = c.fail_put || null;
    this.ops = [];
  }
  protected(d) { return d === "SLUS-20268" || d === "SLES-50366" || d.endsWith("SAVE"); }
  used() { let n = 0; for (const d of Object.keys(this.saves)) n += this.blocks[d] || 0; return n; }
  async readFile(d, f, len, off) {
    if (!(d in this.saves)) return null;
    const data = this.saves[d][f];
    return data ? data.slice(off || 0, (off || 0) + len) : null;
  }
  async probe(names) {
    return names.map(d => ({exists: d in this.saves, tag: d in this.saves && this.saves[d]["gfs.tag"] ? this.saves[d]["gfs.tag"].slice(0, 16) : null}));
  }
  async write(d, f, bytes) {
    this.ops.push(["write", d, f]);
    if (this.protected(d)) return REFUSED;
    if (!(d in this.saves)) return G.NOT_FOUND;
    this.saves[d][f] = new Uint8Array(bytes);
    return 0;
  }
  async del(d) { this.ops.push(["delete", d]); if (this.protected(d)) return REFUSED; delete this.saves[d]; return 0; }
  async put(d, f, blocks, bytes) {
    if (this.protected(d)) throw new Error("put " + d);
    this.ops.push(["put", d]);
    if (d === this.failPut) throw new G.InstallError(d + ": connection lost");
    delete this.saves[d];
    if (this.used() + blocks > this.capacity) return NO_SPACE;
    this.saves[d] = {[f]: new Uint8Array(bytes)};
    this.blocks[d] = blocks;
    return 0;
  }
  async free() { return {status: 0, have: (this.capacity - this.used()) * 32768}; }
  async data(job, src, progress) {
    const blob = await src.range(job, 0, job.size);
    const bytes = new Uint8Array(await blob.arrayBuffer());
    if (bytes.length !== job.size) throw new Error("short container");
    return {status: await this.put(job.dir, "data.bin", job.blocks, bytes)};
  }
  dump() {
    const saves = {};
    for (const [d, files] of Object.entries(this.saves)) { saves[d] = {}; for (const [f, b] of Object.entries(files)) saves[d][f] = hex(b); }
    return {saves, blocks: this.blocks};
  }
}
function memSource(files) {
  const items = Object.entries(files).map(([rel, h]) => ({path: rel, size: h.length / 2, file: new Blob([unhex(h)])}));
  return G.makeSource("folder", "", items);
}
const knownPrefixes = () => Object.fromEntries(Object.entries(PROFILES).filter(([, p]) => p.pack && p.pack.prefix).map(([t, p]) => [p.pack.prefix, t]));
const packOf = (title, prefix, pk) => pk === undefined ? ((PROFILES[title] || {}).pack || {}) : pk;

async function main() {
  const out = {};
  if (req.op === "pack") {
    const p = G.pack(new Map(req.files), new Set(req.dirs || []), req.title, req.prefix, packOf(req.title, req.prefix, req.pk));
    Object.assign(out, {layout_id: p.layoutId.toString(16).padStart(16, "0"), index: hex(p.index), total: p.total,
                        entries: p.entries, index_blocks: p.indexBlocks, traced: p.traced,
                        containers: p.containers.map(c => ({dir: c.dir, blocks: c.blocks, size: c.size, pieces: c.pieces}))});
  } else if (req.op === "install" || req.op === "remove") {
    const con = new FakeConsole(req.console);
    const logs = [];
    try {
      if (req.op === "remove") await G.remove(con, req.title, knownPrefixes(), m => logs.push(m));
      else {
        const src = memSource(req.files);
        const prefix = req.prefix || (packOf(req.title).prefix) || G.defaultPrefix(req.title);
        const p = G.pack(src.files, src.dirs, req.title, prefix, packOf(req.title, prefix, req.pk));
        const st = await G.survey(con, p, req.info || {}, !!req.force, knownPrefixes(), m => logs.push(m));
        const steps = G.plan(st, p, req.info || {}, !!req.force);
        out.steps = steps.map(s => s[0] === "catalog" ? ["catalog", s[1].title, s[1].flags] : s);
        const need = G.spaceNeeded(st, steps, p);
        if (need) {
          const f = await con.free(need);
          if (f.have < need) throw new G.InstallError("not enough space");
        }
        await G.execute(con, steps, st, p, src, {log: m => logs.push(m), step() {}, progress() {}}, {stopped: false});
      }
    } catch (e) {
      if (!(e instanceof G.InstallError)) throw e;
      out.error = e.message;
    }
    Object.assign(out, con.dump(), {ops: con.ops, logs});
  } else if (req.op === "zip") {
    const bytes = fs.readFileSync(req.zip);
    const src = await G.fromZip(new File([bytes], "game.zip"));
    const p = G.pack(src.files, src.dirs, req.title, req.prefix, {});
    out.files = Object.fromEntries(src.files);
    out.dirs = [...src.dirs].sort();
    out.junk = src.junk; out.skipped = src.skipped;
    out.containers = [];
    for (const job of p.containers) {
      // whole, and in pieces from odd offsets (as a resume asks)
      const whole = new Uint8Array(await (await src.range(job, 0, job.size)).arrayBuffer());
      const parts = [];
      for (let off = 0; off < job.size; off += req.step) parts.push(new Uint8Array(await (await src.range(job, off, Math.min(req.step, job.size - off))).arrayBuffer()));
      const back = new Uint8Array(await (await src.range(job, Math.floor(job.size / 2), job.size - Math.floor(job.size / 2))).arrayBuffer());
      out.containers.push({dir: job.dir, whole: hex(whole), parts: hex(Buffer.concat(parts)), back: hex(back)});
    }
  } else if (req.op === "http") {
    Object.assign(out, await http(req));
  } else if (req.op === "live") {
    Object.assign(out, await live(req));
  }
  process.stdout.write(JSON.stringify(out));
}

// The page's own HTTP client (makeRx) against manager.lua in manager_fake.lua's live mode.
async function live(req) {
  const {spawn} = require("child_process");
  const lua = spawn(req.lua, [req.fake, req.manager, req.scenario, req.outdir], {stdio: ["pipe", "pipe", "inherit"]});
  const exited = new Promise(res => lua.on("close", res));
  let buf = Buffer.alloc(0), wake = null;
  lua.stdout.on("data", d => { buf = Buffer.concat([buf, d]); if (wake) { const w = wake; wake = null; w(); } });
  lua.stdout.on("end", () => { if (wake) { const w = wake; wake = null; w(); } });
  async function need(pred) { while (!pred()) { await new Promise(r => { wake = r; }); if (lua.exitCode !== null && !pred()) throw new Error("the fake console exited"); } }
  async function line() { await need(() => buf.indexOf(10) >= 0); const i = buf.indexOf(10), l = buf.subarray(0, i).toString(); buf = buf.subarray(i + 1); return l; }
  async function take(n) { await need(() => buf.length >= n); const b = buf.subarray(0, n); buf = buf.subarray(n); return b; }
  const token = (await line()).replace(/^TOKEN /, "");
  const drops = (req.drops || []).slice();       // [[container, body bytes that arrive]]
  const requests = [];
  async function raw(method, url, body, headers) {
    body = body ? Buffer.from(body instanceof Blob ? await body.arrayBuffer() : body) : Buffer.alloc(0);
    const head = [`${method} ${url} HTTP/1.1`, "Host: 192.0.2.10:8090"];
    for (const [k, v] of Object.entries(headers || {})) head.push(`${k}: ${v}`);
    if (method === "POST") head.push(`Content-Length: ${body.length}`);
    const h = Buffer.from(head.join("\r\n") + "\r\n\r\n");
    let deliver = -1;
    const m = url.match(/\/data\?c=([A-Z0-9]+)/);
    if (m && drops.length && drops[0][0] === m[1]) deliver = h.length + drops.shift()[1];
    requests.push(url + (deliver >= 0 ? " (dropped)" : ""));
    lua.stdin.write(`REQ ${h.length + body.length} ${deliver}\n`);
    lua.stdin.write(Buffer.concat([h, body]));
    const n = +(await line()).replace(/^RESP /, "");
    const r = await take(n);
    if (!n) return null;
    const sep = r.indexOf("\r\n\r\n");
    const lines = r.subarray(0, sep).toString().split("\r\n");
    const hdrs = new Headers();
    for (const l of lines.slice(1)) { const i = l.indexOf(":"); hdrs.append(l.slice(0, i).trim(), l.slice(i + 1).trim()); }
    return new Response(r.subarray(sep + 4), {status: +lines[0].split(" ")[1], headers: hdrs});
  }
  const env = {
    async fetch(url, o) { const r = await raw(o.method, url, o.body, o.headers); if (!r) throw new TypeError("connection lost"); return r; },
    async post(url, blob, progress) {
      const r = await raw("POST", url, blob, {"X-Ghostldr-Token": token});
      if (!r) return {status: 0, json: null};
      progress(blob.size);
      let j = null; try { j = await r.json(); } catch (e) { /* none */ }
      return {status: r.status, json: j};
    },
    token: () => token, sleep: async () => {}, log: m => logs.push(m), status: m => logs.push(m), fmtSize: n => n + " B",
  };
  const logs = [], out = {};
  const rx = G.makeRx(env);
  try {
    const info = await rx.api("GET", "info");
    out.info = info;
    await rx.api("POST", "auth");
    const src = memSource(req.files);
    const prefix = req.prefix || (packOf(req.title).prefix) || G.defaultPrefix(req.title);
    const p = G.pack(src.files, src.dirs, req.title, prefix, packOf(req.title, prefix, req.pk));
    for (const c of p.containers) c.lid = p.layoutId;
    await rx.api("POST", "begin", {title: p.title, name: "", total: p.total});
    const st = await G.survey(rx, p, {}, !!req.force, knownPrefixes(), m => logs.push(m));
    for (const c of p.containers) c.lid = p.layoutId;
    const steps = G.plan(st, p, {}, !!req.force);
    out.steps = steps.map(s => s[0] === "catalog" ? ["catalog", s[1].title, s[1].flags] : s);
    const need = G.spaceNeeded(st, steps, p);
    if (need) {
      const f = await rx.free(need);
      if (f.status || f.have < need) throw new G.InstallError("not enough space");
    }
    await G.execute(rx, steps, st, p, src, {log: m => logs.push(m), step() {}, progress() {}}, {stopped: false});
    await rx.api("POST", "end", {title: p.title, ok: 1});
    if (req.remove) await G.remove(rx, req.remove, knownPrefixes(), m => logs.push(m));
  } catch (e) {
    if (!(e instanceof G.InstallError || e instanceof G.HttpError)) { lua.kill(); throw e; }
    out.error = e.message;
  }
  try { await raw("POST", "/api/exit", null, {}); } catch (e) { /* gone */ }
  lua.stdin.end();
  await exited;
  return Object.assign(out, {logs, requests, report: JSON.parse(fs.readFileSync(req.outdir + "/report.json", "utf8"))});
}
main().catch(e => { process.stdout.write(JSON.stringify({crash: String(e && e.stack || e)})); process.exitCode = 1; });

// A whole install against a real console, as the page does it (a developer's
// check on hardware): {"op": "http", "base": "http://CONSOLE:8090", "token":
// "...", "dir": "path/to/app0" or "zip": "game.zip", "title"?, "force"?, "remove"?}
async function http(req) {
  const logs = [], say = m => { logs.push(m); if (!req.quiet) process.stderr.write(m + "\n"); };
  let last = 0;
  const env = {
    fetch: (u, o) => fetch(req.base + u, o),
    async post(url, blob, progress) {
      try {
        const r = await fetch(req.base + url, {method: "POST", body: blob, headers: {"X-Ghostldr-Token": req.token}});
        progress(blob.size);
        let j = null; try { j = await r.json(); } catch (e) { /* none */ }
        return {status: r.status, json: j};
      } catch (e) { say("post: " + e.message); return {status: 0, json: null}; }
    },
    token: () => req.token, sleep: ms => new Promise(r => setTimeout(r, ms)), log: say, status: say,
    fmtSize: n => (n / 1048576).toFixed(1) + " MiB",
  };
  const rx = G.makeRx(env);
  let src;
  if (req.zip) src = await G.fromZip(new File([await fs.openAsBlob(req.zip)], require("path").basename(req.zip)));
  else {
    const items = [], path = require("path");
    const walk = d => { for (const e of fs.readdirSync(d, {withFileTypes: true})) {
      const full = path.join(d, e.name), rel = path.relative(req.dir, full).split(path.sep).join("/");
      if (e.isDirectory()) { items.push({path: rel + "/", dir: true, size: 0}); walk(full); }
      else if (e.isFile()) items.push({path: rel, size: fs.statSync(full).size, file: fs.openAsBlob(full)});
    } };
    walk(req.dir);
    for (const x of items) if (x.file) x.file = await x.file;
    src = G.makeSource("folder", path.basename(req.dir), items);
  }
  const out = {};
  const t0 = Date.now();
  try {
    out.info = await rx.api("GET", "info");
    say("console: " + JSON.stringify(out.info));
    await rx.api("POST", "auth");
    if (req.remove) { await G.remove(rx, req.remove, knownPrefixes(), say); return out; }
    const sfo = G.parseSfo(await src.bytes("sce_sys/param.sfo"));
    const title = req.title || G.normalizeTitle(sfo.TITLE_ID);
    const pr = PROFILES[title] || {}, pk = pr.pack || {};
    const prefix = pk.prefix || G.defaultPrefix(title);
    const info = {name: pr.name || sfo.TITLE || "", version: pr.version || sfo.APP_VER || ""};
    const p = G.pack(src.files, src.dirs, title, prefix, pk);
    for (const c of p.containers) c.lid = p.layoutId;
    say(`${title} ${info.name} ${info.version}: ${p.files} files, ${p.total} bytes, ${p.containers.length} containers, layout ${p.layoutId.toString(16)}`);
    await rx.api("POST", "begin", {title, name: info.name, total: p.total});
    const st = await G.survey(rx, p, info, !!req.force, knownPrefixes(), say);
    for (const c of p.containers) c.lid = p.layoutId;
    const steps = G.plan(st, p, info, !!req.force);
    out.steps = steps.map(s => s[0] === "catalog" ? ["catalog", s[1].title, s[1].flags] : s);
    say("steps: " + JSON.stringify(out.steps));
    const need = G.spaceNeeded(st, steps, p);
    if (need) { const f = await rx.free(need); say(`free: ${JSON.stringify(f)} for ${need}`); if (f.status || f.have < need) throw new G.InstallError("not enough space"); }
    await G.execute(rx, steps, st, p, src, {log: say, step: say, progress(done, total) {
      if (Date.now() - last > 5000) { last = Date.now(); say(`${(done / 1048576).toFixed(0)} / ${(total / 1048576).toFixed(0)} MiB, ${(done / 1e3 / Math.max(1, Date.now() - t0)).toFixed(1)} MB/s`); }
    }}, {stopped: false});
    await rx.api("POST", "end", {title, ok: 1});
    out.seconds = (Date.now() - t0) / 1000;
    say(`installed in ${out.seconds} s`);
  } catch (e) {
    out.error = e.message;
    say("error: " + e.message);
  }
  out.logs = logs;
  return out;
}
