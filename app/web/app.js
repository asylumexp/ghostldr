// ghostldr app: the page.  Plain JavaScript, no build step, no libraries.
// It talks to the local server (app/ghostapp/server.py) with the per-launch
// token from the address the launcher opened (#t=...).
"use strict";

(() => {
  // ---------- small helpers ----------
  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));

  function h(tag, attrs, ...kids) {
    const el = document.createElement(tag);
    for (const [k, v] of Object.entries(attrs || {})) {
      if (v === undefined || v === null || v === false) continue;
      if (k === "class") el.className = v;
      else if (k === "text") el.textContent = v;
      else if (k.startsWith("on")) el.addEventListener(k.slice(2), v);
      else if (k === "dataset") Object.assign(el.dataset, v);
      else if (k === "style") el.style.cssText = v;      // CSSOM: allowed by the page's CSP
      else el.setAttribute(k, v === true ? "" : v);
    }
    for (const kid of kids.flat(Infinity)) {
      if (kid === null || kid === undefined || kid === false) continue;
      el.append(kid instanceof Node ? kid : document.createTextNode(String(kid)));
    }
    return el;
  }

  function fill(el, ...kids) { el.replaceChildren(...kids.flat(Infinity).filter((k) => k !== null && k !== undefined && k !== false)); }

  // Rebuild `el` only when `sig` changed (keeps focus, scroll and hover steady).
  function once(el, sig, build) {
    if (el._sig === sig) return;
    el._sig = sig;
    fill(el, build());
  }

  const now = () => Date.now() / 1000;
  const plural = (n, one, many) => `${n} ${n === 1 ? one : (many || one + "s")}`;
  const gib = (bytes) => (bytes / 2 ** 30).toFixed(1) + " GiB";
  function fmtSize(bytes) {
    if (!bytes && bytes !== 0) return "";
    if (bytes >= 2 ** 30) return gib(bytes);
    if (bytes >= 2 ** 20) return (bytes / 2 ** 20).toFixed(0) + " MiB";
    return Math.max(1, Math.round(bytes / 1024)) + " KiB";
  }
  function fmtMinutes(min) {
    if (min === null || min === undefined) return "";
    if (min < 1) return "less than a minute";
    if (min < 60) return plural(Math.round(min), "minute");
    const h_ = Math.floor(min / 60), m = Math.round(min % 60);
    return `${h_} h ${m} min`;
  }
  function fmtAgo(ts) {
    if (!ts) return "";
    const d = now() - ts;
    if (d < 45) return "just now";
    if (d < 3600) return plural(Math.round(d / 60), "minute") + " ago";
    if (d < 86400) return plural(Math.round(d / 3600), "hour") + " ago";
    return new Date(ts * 1000).toLocaleDateString();
  }
  const fmtTime = (ts) => new Date(ts * 1000).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
  function hue(text) {
    let x = 0;
    for (const c of String(text)) x = (x * 31 + c.charCodeAt(0)) >>> 0;
    return x % 360;
  }
  const spinner = () => h("span", { class: "spin", "aria-hidden": "true" });

  // ---------- talking to the app ----------
  let TOKEN = "";
  function readToken() {
    const m = /[#&]t=([A-Za-z0-9_-]+)/.exec(location.hash);
    if (m) {
      TOKEN = m[1];
      try {
        sessionStorage.setItem("ghostldr-token", TOKEN);
        history.replaceState(null, "", location.pathname + location.search);
      } catch (e) { /* keep it in the address then */ }
    } else {
      try { TOKEN = sessionStorage.getItem("ghostldr-token") || ""; } catch (e) { TOKEN = ""; }
    }
  }

  class ApiError extends Error {
    constructor(message, status, data) { super(message); this.status = status; this.data = data || {}; }
  }

  let quitting = false;
  function setOffline(off) {
    $("#offline").hidden = !off;
    if (off && quitting) $("#offline").textContent = "The app has stopped. You can close this window.";
  }

  async function api(path, body, opts = {}) {
    const init = { method: body === undefined ? "GET" : "POST", cache: "no-store",
                   headers: { "X-Ghostldr-Token": TOKEN } };
    if (body !== undefined) {
      init.headers["Content-Type"] = "application/json";
      init.body = JSON.stringify(body);
    }
    let r;
    try {
      r = await fetch(path, init);
    } catch (e) {
      setOffline(true);
      throw new ApiError("The app isn't answering. Is it still running?", 0);
    }
    setOffline(false);
    if (r.status === 401) $("#notoken").hidden = false;
    if (opts.blob) {
      if (!r.ok) throw new ApiError("not found", r.status);
      return r.blob();
    }
    let data = {};
    try { data = await r.json(); } catch (e) { /* empty */ }
    if (!r.ok) throw new ApiError(data.error || `The app answered ${r.status}.`, r.status, data);
    return data;
  }

  // A button's action: run it, show errors as toasts, keep it disabled meanwhile.
  async function act(btn, fn) {
    if (btn && btn.disabled) return;
    const old = btn ? btn.disabled : false;
    if (btn) btn.disabled = true;
    try {
      return await fn();
    } catch (e) {
      handleError(e);
    } finally {
      if (btn) btn.disabled = old;
      render();
    }
  }

  function handleError(e) {
    if (!(e instanceof ApiError)) { console.error(e); toast(String(e), true); return; }
    const d = e.data || {};
    if (d.busy) {
      toast(e.message, true, "Show", () => showJob(d.busy));
    } else if (d.action === "goto_console") {
      toast(e.message, true, "Go there", () => go("home", "console"));
    } else if (d.action === "goto_game") {
      toast(e.message, true, "Go there", () => go("home", "game"));
    } else if (d.action === "goto_usb") {
      toast(e.message, true, "Go there", () => go("home", "usb"));
    } else if (d.action === "goto_manager") {
      toast(e.message, true, "Go there", () => go("library"));
    } else if (e.status !== 0) {        // 0: the offline banner says it
      toast(e.message, true);
    }
  }

  function toast(text, error = false, label = "", onClick = null) {
    const t = h("div", { class: "toast" + (error ? " error" : ""), role: error ? "alert" : "status" }, text);
    if (label && onClick) {
      t.append(" ", h("button", { class: "btn sm secondary", type: "button", style: "margin-left:8px",
                                  onclick: () => { t.remove(); onClick(); } }, label));
    }
    $("#toasts").append(t);
    setTimeout(() => t.remove(), error ? 9000 : 4500);
  }

  // ---------- state ----------
  let S = null;                        // the last /api/state
  let view = "home";
  let step = null;                     // console | game | install | play (null: pick one)
  let helpPane = "log";
  const ui = {
    dump: null,                        // the inspected game folder
    dumpPath: "",
    installTitle: "",
    playChoice: "",                    // a title ID, or "*select*"
    activity: "",
    trouble: null,
    runlog: null,
    startedDiscovery: false,
    drives: null,                      // drives mounted on this computer (USB path)
    drivesLoading: false,
    usbStatus: null,                   // whether the PS5's FTP server answers
    listed: {},                        // drives read once already this session
  };

  // ---------- jobs: one slot per kind of card, each following its latest job ----------
  const KIND_SLOT = { discover: "discover", doctor: "doctor", pack: "pack", report: "report", install: "install",
                      library: "library", remove: "libjob", meta: "libjob", boot: "boot", manager: "manager",
                      stubs: "stubs", usbcopy: "usbcopy", usblist: "usblist", usbremove: "libjob" };
  const slots = {};
  for (const s of new Set(Object.values(KIND_SLOT))) slots[s] = { id: "", job: null, lines: [], seen: 0 };
  const MAX_KEEP = { boot: 6000 };

  function attach(slotName, job) {
    const slot = slots[slotName];
    if (!job || slot.id === job.id) return;
    slot.id = job.id;
    slot.job = job;
    slot.lines = [];
    slot.seen = 0;
    const raw = rawFor(slotName);
    if (raw) { raw.textContent = ""; raw._count = 0; }
    if (slotName === "boot") { $("#boot-log").textContent = ""; $("#boot-log")._count = 0; }
    follow(slotName, job.id);
  }

  async function follow(slotName, id) {
    const slot = slots[slotName];
    let errors = 0;
    while (slot.id === id) {
      const running = !slot.job || ["starting", "running"].includes(slot.job.state);
      let res;
      try {
        res = await api(`/api/jobs/${id}?since=${slot.seen}&wait=${running ? 12 : 0}`);
        errors = 0;
      } catch (e) {
        if (e.status === 404) return;
        errors += 1;
        await new Promise((r) => setTimeout(r, Math.min(10000, 1000 * errors)));
        continue;
      }
      if (slot.id !== id) return;
      slot.job = res;
      slot.seen = res.next;
      if (res.output.length) {
        slot.lines.push(...res.output);
        const keep = MAX_KEEP[slotName] || 3000;
        if (slot.lines.length > keep) slot.lines.splice(0, slot.lines.length - keep);
        appendRaw(slotName, res.output);
      }
      render();
      if (!["starting", "running"].includes(res.state)) {
        refresh();
        return;
      }
    }
  }

  function rawFor(slotName) {
    if (slotName === "report") return null;
    const d = $(`details.raw[data-raw="${slotName}"] pre`);
    return d || null;
  }

  function appendLines(pre, lines, limit, classify) {
    if (!pre || !lines.length) return;
    const atEnd = pre.scrollTop + pre.clientHeight >= pre.scrollHeight - 30;
    const frag = document.createDocumentFragment();
    for (const line of lines) {
      const cls = classify ? classify(line) : "";
      frag.append(cls ? h("span", { class: cls }, line + "\n") : document.createTextNode(line + "\n"));
    }
    pre.append(frag);
    pre._count = (pre._count || 0) + lines.length;
    while (pre._count > limit && pre.firstChild) { pre.firstChild.remove(); pre._count -= 1; }
    return atEnd;
  }

  function classifyLog(line) {
    if (/^\s+\[hint\]/.test(line)) return "hint-line";
    if (/CRASH|Traceback|Error|error:|FAIL|failed/.test(line)) return "err-line";
    if (/^(preflight|sending|serving|listening|extension|selector):?/.test(line)) return "dim";
    return "";
  }

  function appendRaw(slotName, lines) {
    if (slotName === "boot") {
      const pre = $("#boot-log");
      appendLines(pre, lines, 4000, classifyLog);
      if ($("#chk-follow").checked) pre.scrollTop = pre.scrollHeight;
      return;
    }
    const pre = rawFor(slotName);
    if (pre) appendLines(pre, lines, 3000, null);
  }

  const isRunning = (job) => !!job && ["starting", "running"].includes(job.state);
  const slotRunning = (name) => isRunning(slots[name].job);
  const progressOf = (name) => (slots[name].job && slots[name].job.progress) || null;

  async function startJob(btn, path, body, slotName) {
    return act(btn, async () => {
      const res = await api(path, body);
      if (res.job) attach(slotName || KIND_SLOT[res.job.kind], res.job);
      await refresh();
      return res.job;
    });
  }

  async function cancelJob(name, ask) {
    const job = slots[name].job;
    if (!job || !isRunning(job)) return;
    if (ask) {
      const ok = await confirmBox(ask);
      if (!ok) return;
    }
    try { await api(`/api/jobs/${job.id}/cancel`, {}); } catch (e) { handleError(e); }
    refresh();
  }

  function showJob(id) {
    ui.activity = id;
    helpPane = "activity";
    go("help");
    loadActivity(id);
  }

  // ---------- refreshing ----------
  let refreshing = null;
  async function refresh() {
    if (refreshing) return refreshing;
    refreshing = (async () => {
      try {
        S = await api("/api/state");
        for (const [kind, id] of Object.entries(S.latest || {})) {
          const slotName = KIND_SLOT[kind];
          if (!id || !slotName) continue;
          const job = S.jobs.find((j) => j.id === id);
          if (!job) continue;
          // remove and meta share a card: follow the newer one
          const cur = slots[slotName].job;
          if (cur && cur.id !== id && cur.started > job.started) continue;
          attach(slotName, job);
        }
        firstRun();
        render();
      } catch (e) {
        if (!(e instanceof ApiError) || e.status !== 0) console.error(e);
      } finally {
        refreshing = null;
      }
    })();
    return refreshing;
  }

  function pollLoop() {
    const busy = S && S.running && S.running.length;
    const delay = document.hidden ? 10000 : busy ? 2000 : 3000;
    setTimeout(async () => { await refresh(); pollLoop(); }, delay);
  }

  // Whether Luac0re's loader listens, while a screen that needs it is open.
  function loaderLoop() {
    setTimeout(async () => {
      const wants = !document.hidden && S && S.settings.console.ip && view === "home" &&
        ["console", "install", "play"].includes(currentStep()) && !S.console_busy;
      if (wants) {
        try {
          S.loader = await api("/api/loader");
          renderLoader();
          renderChip();
          renderStepper();
        } catch (e) { /* the state poll reports it */ }
      }
      loaderLoop();
    }, 3000);
  }

  // Whether the PS5's FTP server answers, while the USB step (drive in the PS5) is open.
  function usbLoop() {
    setTimeout(async () => {
      const wants = !document.hidden && S && view === "home" && currentStep() === "usb" &&
        S.settings.usb_where === "console" && S.usb.target;
      if (wants) {
        try {
          const st = await api("/api/usb/status");
          const was = ui.usbStatus && ui.usbStatus.ready;
          ui.usbStatus = st;
          if (st.ready && !was) autoList();
          render();
        } catch (e) { /* the state poll reports it */ }
      }
      usbLoop();
    }, 3000);
  }

  let didFirst = false;
  function firstRun() {
    if (didFirst || !S) return;
    didFirst = true;
    if (!S.consoles.length && !S.latest.discover && !ui.startedDiscovery) {
      ui.startedDiscovery = true;
      startJob(null, "/api/discover", {}, "discover");
    }
    if (S.settings.dump && !ui.dump) inspect(S.settings.dump, true);
    ui.playChoice = S.settings.title || "";
    $("#chk-art").checked = !!S.settings.fetch_art;
    step = currentStep();          // from here on the step only changes when the person moves
  }

  // ---------- navigation ----------
  function go(v, st, pane) {
    view = v;
    if (st) step = st;
    if (pane) helpPane = pane;
    render();
    window.scrollTo({ top: 0 });
    if (v === "help") showHelpPane(helpPane);
  }

  // ---------- the two paths: saved data or a USB drive ----------
  const mode = () => (S && S.settings.mode) || "saves";
  const usbMode = () => mode() === "usb";
  const STEP_ORDER = { saves: ["console", "game", "install", "play"], usb: ["console", "game", "usb", "play"] };
  const thirdStep = () => (usbMode() ? "usb" : "install");
  function fixStep(st) {                 // the third step follows the path
    if (st === "install" || st === "usb") return thirdStep();
    return st;
  }
  // {title: entry} on the chosen USB drive as last read, or null
  function usbMap() {
    if (!S || !S.usb) return null;
    const lib = S.usb.library;
    if (!lib || !lib.ok || !S.usb.target || lib.target !== S.usb.target) return null;
    const m = {};
    for (const e of lib.entries) if (e.title) m[e.title] = e;
    return m;
  }
  const dumpTitle = () => (ui.dump && ui.dump.eboot && ui.dump.detect && ui.dump.detect.title) || "";
  function onDrive(title) {
    if (!title || !S) return false;
    const m = usbMap();
    if (m) return !!m[title];
    const c = S.usb.copies[title];
    return !!(c && c.target === S.usb.target);
  }
  function autoList(force) {
    const t = S && S.usb.target;
    if (!t || slotRunning("usblist") || (!force && (ui.listed[t] || (S.usb.library.ok && S.usb.library.target === t)))) return;
    ui.listed[t] = true;
    startJob(null, "/api/usb/list", {}, "usblist");
  }
  async function setMode(m, btn) {
    await act(btn, async () => {
      await api("/api/mode", { mode: m });
      await refresh();
      if (step) step = fixStep(step);
    });
  }
  async function chooseUsb(btn, body) {
    await act(btn, async () => {
      await api("/api/usb/choose", body);
      ui.usbStatus = null;
      await refresh();
      if (body.where === "computer" && body.drive) autoList();
    });
  }
  async function loadDrives() {
    if (ui.drivesLoading) return;
    ui.drivesLoading = true;
    render();
    try {
      ui.drives = (await api("/api/usb/drives", {})).drives;
    } catch (e) {
      handleError(e);
      ui.drives = ui.drives || [];
    } finally {
      ui.drivesLoading = false;
      render();
    }
  }

  function currentStep() {
    if (step) return fixStep(step);
    if (!S) return "console";
    if (!S.settings.console.ip) return "console";
    if (slotRunning("boot")) return "play";
    if (usbMode()) {
      if (slotRunning("usbcopy")) return "usb";
      const t = dumpTitle();
      if (t && onDrive(t)) return "play";
      return ui.dump || S.settings.dump ? "usb" : "game";
    }
    if (slotRunning("install")) return "install";
    if (!S.packed.length) return "game";
    const lib = installedMap();
    const t = S.settings.title || (S.packed[0] && S.packed[0].title);
    if (lib && lib[t] && lib[t].complete) return "play";
    return "install";
  }

  // {title: entry} of what the PS5 has, or null when unknown
  function installedMap() {
    if (!S || !S.library || !S.library.ok) return null;
    const m = {};
    for (const e of S.library.entries) if (e.title) m[e.title] = e;
    return m;
  }

  // ---------- rendering ----------
  function render() {
    if (!S) return;
    document.body.classList.remove("loading");
    for (const tab of $$(".tab")) {
      if (tab.dataset.view === view) tab.setAttribute("aria-current", "page");
      else tab.removeAttribute("aria-current");
    }
    $("#view-home").hidden = view !== "home";
    $("#view-library").hidden = view !== "library";
    $("#view-help").hidden = view !== "help";
    renderChip();
    const missing = Object.entries(S.prebuilt).filter(([n, ok]) => !ok && n !== "selector.bin").map(([n]) => n);
    const warn = $("#prebuilt-warn");
    warn.hidden = !missing.length;
    if (missing.length) {
      once(warn, missing.join(), () => [h("b", {}, "This copy can't start games"),
        `The prebuilt loader files are missing (build/ghostldr/${missing.join(", ")}). Unpack the release zip again and start the app from inside it.`]);
    }
    if (view === "home") {
      renderPaths();
      renderStepper();
      const cur = currentStep();
      for (const p of $$(".panel")) p.hidden = p.dataset.step !== cur;
      if (cur === "console") renderConsole();
      if (cur === "game") renderGame();
      if (cur === "install") renderInstall();
      if (cur === "usb") renderUsb();
      if (cur === "play") renderPlay();
      renderLoader();
    } else if (view === "library") {
      renderLibrary();
    } else {
      renderHelp();
    }
  }

  function consoleState() {
    const c = S.settings.console;
    if (!c.ip) return { cls: "", text: "No console chosen" };
    const name = c.name || c.kind || c.ip;
    if (S.console_busy) return { cls: "busy", text: `${name} · busy`, long: S.console_busy.label };
    if (S.manager.running) return { cls: "warn", text: `${name} · save manager`, long: "The save manager is running" };
    const l = S.loader;
    if (l.ip === c.ip && l.listening === true) return { cls: "ok", text: `${name} · Luac0re ready`, long: "Luac0re's loader is listening" };
    if (l.ip === c.ip && l.listening === false) return { cls: "warn", text: `${name} · start Luac0re`, long: "Luac0re's loader isn't listening" };
    return { cls: "", text: name };
  }

  function renderChip() {
    const st = consoleState();
    const chip = $("#console-chip");
    $(".dot", chip).className = "dot " + st.cls;
    $(".chip-text", chip).textContent = st.text;
    chip.title = st.long || st.text;
  }

  function renderPaths() {
    const chosen = !!S.settings.mode;
    $("#paths").classList.toggle("chosen", chosen);
    $("#paths-hint").textContent = chosen ? "You can switch any time." : "Pick one to start. You can switch any time.";
    for (const b of $$(".path")) b.setAttribute("aria-checked", String(chosen && b.dataset.mode === mode()));
  }

  function usbStepState() {
    const cp = progressOf("usbcopy");
    if (slotRunning("usbcopy")) {
      return { cls: "running", text: cp && cp.phase === "copying" && cp.need_gib ? `${cp.done_gib.toFixed(1)} of ${cp.need_gib.toFixed(1)} GiB` : "Copying…" };
    }
    const t = dumpTitle();
    if (t && onDrive(t)) return { cls: "done", text: "On the drive" };
    if (!S.usb.target) return { cls: "attention", text: "No drive chosen" };
    return { cls: "", text: "Not copied" };
  }

  function renderStepper() {
    const third = $$(".step")[2];
    if (third.dataset.step !== thirdStep()) {
      third.dataset.step = thirdStep();
      $(".lbl", third).textContent = usbMode() ? "USB drive" : "Install";
    }
    const order = STEP_ORDER[mode()];
    const lib = installedMap();
    const c = S.settings.console;
    const title = selectedInstallTitle();
    const packed = S.packed.find((p) => p.title === title);
    const pr = progressOf("install");
    const states = {
      console: c.ip ? { cls: "done", text: c.name || c.ip } : { cls: "attention", text: "Not chosen" },
      game: slotRunning("pack") ? { cls: "running", text: "Preparing…" } :
        packed ? { cls: "done", text: packed.name || packed.title } :
        S.packed.length ? { cls: "done", text: plural(S.packed.length, "game") + " ready" } : { cls: "", text: "No game yet" },
      install: slotRunning("install") ? { cls: "running",
        text: pr && pr.phase === "sending" && pr.send_containers ? `${pr.done}/${pr.send_containers} · ${fmtMinutes(pr.eta_min)} left` : "Installing…" } :
        lib && title && lib[title] && lib[title].complete ? { cls: "done", text: "Installed" } :
        lib && title && lib[title] ? { cls: "attention", text: "Not finished" } :
        { cls: "", text: lib ? "Not installed" : "Not checked" },
      usb: usbStepState(),
      play: slotRunning("boot") ? { cls: "running", text: "Playing" } : { cls: "", text: "" },
    };
    const cur = currentStep();
    for (const b of $$(".step")) {
      const name = b.dataset.step;
      const s = states[name];
      b.className = "step " + s.cls + (name === cur ? " current" : "");
      $(".st", b).textContent = s.text;
      const num = $(".num", b);
      const want = s.cls === "done" && name !== cur ? "check" : String(order.indexOf(name) + 1);
      if (num._v !== want) {
        num._v = want;
        if (want === "check") {
          num.innerHTML = '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M5 12.5l4.5 4.5L19 7.5"/></svg>';
        } else num.textContent = want;
      }
      if (name === cur) b.setAttribute("aria-current", "step"); else b.removeAttribute("aria-current");
    }
  }

  // The live "loader listens" item in each checklist.
  function renderLoader() {
    const c = S.settings.console;
    const l = S.loader;
    let cls = "", text = "", li = "";
    if (!c.ip) { text = "Choose your PS5 first."; }
    else if (S.console_busy) { text = `The app is using it: ${S.console_busy.label}.`; }
    else if (S.manager.running) { text = "The save manager holds it. Stop the save manager first (Library)."; cls = "warn"; }
    else if (l.ip === c.ip && l.listening) { text = "Listening. Ready."; cls = "ok"; li = "ok"; }
    else if (l.ip === c.ip && l.listening === false) { text = "Not listening yet. Checking every few seconds…"; li = "wait"; }
    else { text = "Checking…"; li = "wait"; }
    for (const item of $$("li[data-loader]")) {
      item.className = "auto " + li;
      const s = $(".loader-state", item);
      s.className = "loader-state " + cls;
      s.textContent = text;
    }
  }

  // ---------- problems (a failed job) ----------
  const ACTIONS = {
    wait_loader: ["Check again", () => { S.loader.checked = 0; go("home", "console"); }],
    discover: ["Search again", () => startJob(null, "/api/discover", {}, "discover")],
    pick_console: ["Choose a console", () => go("home", "console")],
    goto_game: ["Go to Game", () => go("home", "game")],
    goto_usb: ["Go to USB drive", () => go("home", "usb")],
    goto_play: ["Go to Play", () => go("home", "play")],
    goto_console: ["Go to Console", () => go("home", "console")],
    firewall: ["Firewall help", () => go("help", null, "firewall")],
    refresh_library: ["Refresh the Library", () => { go("library"); startJob($("#btn-lib-refresh"), "/api/library/refresh", {}, "library"); }],
    install_anyway: ["Install anyway", () => { $("#chk-nofit").checked = true; $("#chk-nofit").closest("details").open = true; go("home", "install"); }],
    force_preflight: ["Play anyway", () => play(true)],
  };

  function renderProblem(el, job, extra) {
    if (!job || isRunning(job)) { fill(el); el._sig = ""; return; }
    const sig = job.id + job.state;
    if (job.state === "done") { fill(el); el._sig = sig; return; }
    once(el, sig, () => {
      el.classList.toggle("warn", job.state === "cancelled");
      if (job.state === "cancelled") return [h("p", {}, extra && extra.cancelled || "Stopped.")];
      const p = job.problem;
      const out = [];
      if (p) {
        out.push(h("h4", {}, p.title), h("p", {}, p.fix));
        const a = ACTIONS[p.action];
        if (a) out.push(h("div", { class: "row" }, h("button", { class: "btn secondary sm", type: "button", onclick: a[1] }, a[0])));
      } else {
        out.push(h("h4", {}, "Something went wrong"));
        if (job.error) out.push(h("p", {}, h("code", {}, job.error)));
        out.push(h("p", { class: "hint" }, "Open Details below for everything it printed, or see Logs & Help → Troubleshooting."));
      }
      return out;
    });
  }

  function pill(el, job, texts) {
    texts = texts || {};
    let cls = "", text = "";
    if (!job) { /* nothing */ }
    else if (isRunning(job)) { cls = "run"; text = job.cancel_requested ? "Stopping…" : (texts.running || "Running"); }
    else if (job.state === "done") { cls = "ok"; text = texts.done || "Done"; }
    else if (job.state === "cancelled") { cls = "warn"; text = texts.cancelled || "Stopped"; }
    else { cls = "danger"; text = "Failed"; }
    el.className = "pill " + cls;
    el.textContent = text;
  }

  // ---------- 1. console ----------
  function consoleItem(c, selected) {
    const badges = [];
    if (c.loader) badges.push(h("span", { class: "pill ok" }, "Luac0re ready"));
    else if (c.swrr) badges.push(h("span", { class: "pill" }, "SWRR running"));
    if (!c.on) badges.push(h("span", { class: "pill warn" }, "Rest mode"));
    if (c.kind === "PS4") badges.push(h("span", { class: "pill" }, "PS4"));
    const meta = [c.kind === "PS5" ? "PlayStation 5" : c.kind === "PS4" ? "PlayStation 4" : "Console", c.ip,
                  c.firmware ? "firmware " + c.firmware : ""].filter(Boolean).join(" · ");
    return h("button", { class: "item" + (selected ? " selected" : ""), type: "button", role: "radio",
                         "aria-checked": selected ? "true" : "false",
                         onclick: (ev) => act(ev.currentTarget, async () => {
                           await api("/api/console/select", { ip: c.ip, host_id: c.host_id || "" });
                           await refresh();
                         }) },
      h("span", { class: "radio" }),
      h("span", { class: "grow" }, h("span", { class: "name" }, c.name || (c.kind === "PS4" ? "PS4" : "PS5")),
        h("span", { class: "meta" }, meta)),
      h("span", { class: "badges" }, badges));
  }

  function renderConsole() {
    const sel = S.settings.console;
    // the chosen console's loader as last checked, else as discovery saw it
    const live = (c) => (c.ip === sel.ip && S.loader.ip === c.ip && S.loader.checked > S.consoles_at &&
                         S.loader.listening !== null ? { ...c, loader: S.loader.listening } : c);
    const list = S.consoles.map(live);
    if (sel.ip && !list.some((c) => c.ip === sel.ip)) {
      list.push({ ip: sel.ip, name: sel.name, kind: sel.kind, host_id: sel.host_id, firmware: "", on: true,
                  loader: S.loader.ip === sel.ip && !!S.loader.listening, remembered: true });
    }
    const searching = slotRunning("discover");
    $("#btn-discover").disabled = searching;
    $("#btn-discover").textContent = searching ? "Searching…" : "Search again";
    const dj = slots.discover.job;
    const sig = JSON.stringify([list, sel.ip, searching, dj && dj.state]);
    once($("#console-list"), sig, () => {
      if (!list.length && searching) return h("div", { class: "searching" }, h("span", { class: "spin" }), "Looking for your PS5 on the network…");
      if (!list.length) {
        return h("div", { class: "empty" },
          h("p", {}, h("b", {}, "No PlayStation answered.")),
          h("p", { class: "hint" }, "Turn the PS5 on (not rest mode) and make sure it's on the same network as this computer. Or enter its IP address below."));
      }
      return list.map((c) => consoleItem(c, c.ip === sel.ip));
    });
    if (!list.length && S.consoles_at && !searching && !ui.ipOpened) { ui.ipOpened = true; $("#ip-details").open = true; }

    // doctor
    const dr = progressOf("doctor");
    const doc = slots.doctor.job;
    const running = slotRunning("doctor");
    $("#btn-doctor").disabled = running || !sel.ip || !!S.console_busy;
    fill($("#btn-doctor"), running ? [spinner(), "Checking…"] : doc ? "Check again" : "Run checks");
    once($("#doctor-rows"), JSON.stringify(dr && dr.rows), () => ((dr && dr.rows) || []).map(doctorRow));
    $("#doctor-foot").hidden = !(dr && dr.rows && dr.rows.length);

    const next = $("#btn-console-next");
    next.disabled = !sel.ip;
    $("#console-next-hint").textContent = sel.ip ? "" : "Choose your PS5 first.";
  }

  const DOCTOR_NAMES = { python: "Python", core: "Loader files", payload: "Loader size", log_port: "Game log port (UDP 18194)",
                         ext_port: "Extension port (TCP 18195)", loader: "Luac0re on the PS5", dump: "Game files" };
  function doctorRow(r) {
    let why = r.detail;
    if (r.key === "python") why = r.status === "PASS" ? r.label.replace("python", "Python") : r.detail;
    if (r.key === "core" && r.status === "PASS") why = "Present and intact.";
    if (r.key === "payload" && r.status === "PASS") {
      const m = /^(\d+) of (\d+) bytes/.exec(r.detail);
      why = m ? `Fits Luac0re's limit (${Math.round(m[1] / 1024)} of ${Math.round(m[2] / 1024)} KB).` : r.detail;
    }
    if (r.key === "loader" && r.status === "FAIL") why = "Not listening. Open OPTIONS → HALL OF FAME in SWRR, then check again.";
    if (r.key === "loader" && r.status === "PASS") why = "Listening on port 9026.";
    if (r.key === "dump") {
      if (r.functions !== undefined) {
        why = r.unresolved ? `${r.title}: ${r.functions} functions, ${r.unresolved} not implemented yet (it may still play).`
          : `${r.title}: all ${r.functions} functions covered.`;
      } else if (/no name table/.test(r.detail)) {
        why = "Normal for a release: there's no name table to check the game against.";
      } else if (/no local dump/.test(r.detail)) {
        why = "No game prepared on this computer yet.";
      }
    }
    if ((r.key === "log_port" || r.key === "ext_port") && r.status === "FAIL") {
      why = "Another program uses this port (maybe another ghostldr). Close it. " + r.detail;
    }
    if ((r.key === "log_port" || r.key === "ext_port") && r.status === "PASS") why = "Free.";
    const icon = { PASS: "✓", WARN: "!", FAIL: "✕" }[r.status];
    return h("li", {}, h("span", { class: "ico " + r.status, "aria-label": r.status }, icon),
      h("div", {}, h("div", { class: "what" }, DOCTOR_NAMES[r.key] || r.label), h("div", { class: "why" }, why)));
  }

  // ---------- 2. game ----------
  async function inspect(path, quiet) {
    path = String(path || "").trim();
    if (!path) return;
    ui.dumpPath = path;
    ui.inspecting = true;
    render();
    try {
      const info = await api("/api/dump/inspect", { path });
      ui.dump = info;
      if (info.eboot) await api("/api/settings", { dump: info.path });
    } catch (e) {
      if (!quiet) handleError(e);
      ui.dump = quiet ? null : { path, error: e.message };
    } finally {
      ui.inspecting = false;
      render();
    }
  }

  function packedFor(src) {
    return S.packed.find((p) => p.src === src) || null;
  }

  function renderGame() {
    const d = ui.dump;
    $("#folder-empty").hidden = !!(d || ui.inspecting);
    $("#folder-chosen").hidden = !(d || ui.inspecting);
    if (ui.inspecting && !d) {
      $("#folder-path").textContent = ui.dumpPath;
      fill($("#folder-title"), h("span", { class: "hint" }, spinner(), " Looking at the folder…"));
    }
    if (d) {
      $("#folder-path").textContent = d.path;
      const det = d.detect || {};
      const packed = d.eboot ? packedFor(d.path) : null;
      const name = d.profile_name || d.sfo_name || (packed && packed.name) || "";
      once($("#folder-title"), JSON.stringify([d.path, det, name, ui.inspecting]), () => {
        if (ui.inspecting) return h("span", { class: "hint" }, spinner(), " Looking at the folder…");
        if (d.error || !d.eboot) return null;
        const sub = [name ? det.title : "", det.version ? "v" + det.version : ""].filter(Boolean).join(" · ");
        return [h("span", {}, name || det.title || "Unrecognised game"), sub ? h("span", { class: "tid" }, sub) : null];
      });
      once($("#folder-facts"), JSON.stringify(d), () => {
        if (!d.eboot) return null;
        const f = [];
        if (d.files !== undefined) f.push(h("li", {}, h("b", {}, d.files.toLocaleString()), " files"));
        if (d.bytes !== undefined) f.push(h("li", {}, h("b", {}, fmtSize(d.bytes)), d.partial ? "+ (still counting)" : ""));
        f.push(h("li", { class: d.sce_module ? "ok" : "" }, d.sce_module ? "✓ sce_module" : "no sce_module"));
        if (d.param_sfo) f.push(h("li", { class: "ok" }, "✓ param.sfo"));
        if (det.homebrew) f.push(h("li", {}, "Homebrew"));
        return f;
      });
      once($("#folder-warn"), JSON.stringify([d.error, d.eboot, d.suggestions, det.title, d.detect === null]), () => {
        if (d.error) return h("div", { class: "callout danger" }, d.error);
        if (!d.eboot) {
          const out = [h("div", { class: "callout warn" }, h("b", {}, "This folder isn't a game dump"),
            "It has no eboot.bin. Choose the folder that contains eboot.bin, often called app0.")];
          if (d.suggestions && d.suggestions.length) {
            out.push(h("div", { class: "suggest" }, h("span", { class: "hint" }, "Found inside it:"),
              d.suggestions.slice(0, 5).map((s) => h("button", { class: "btn secondary sm", type: "button", onclick: () => inspect(s) }, "Use " + s))));
          }
          return out;
        }
        if (!det.title) {
          return h("div", { class: "callout warn" }, h("b", {}, "Couldn't tell which game this is"),
            "Enter its title ID (like CUSA12345) under More options before preparing it.");
        }
        return null;
      });
      if (!det.title && d.eboot) $("#game-more").open = true;
    }

    // packing
    const pj = slots.pack.job;
    const pr = progressOf("pack");
    const packing = isRunning(pj);
    $("#pack-card").hidden = !pj;
    if (pj) {
      pill($("#pack-pill"), pj, { running: "Working", done: "Ready" });
      const bar = $("#pack-progress");
      const phaseText = { scanning: "Reading the game's files…", art: "Downloading the name and pictures…",
                          layout: "Writing the layout…", fit: "Checking the game's code…", done: "Prepared." };
      let pct = 0;
      if (pj.state === "done") pct = 100;
      else if (packing && pr) pct = Math.min(92, (now() - pr.started) / Math.max(1, pr.estimate_s) * 85);
      bar.className = "progress" + (pj.state === "done" ? " done" : pj.state === "failed" ? " failed" : "");
      $(".bar", bar).style.width = (pj.state === "failed" || pj.state === "cancelled" ? 100 : pct) + "%";
      $("#pack-phase").textContent = packing ? (phaseText[pr && pr.phase] || "Working…") :
        pj.state === "done" ? "Prepared on this computer. Nothing was sent to the PS5." :
        pj.state === "cancelled" ? "Stopped." : "Didn't finish.";
      $("#pack-eta").textContent = packing && pr && pr.estimate_s > 20 ? `about ${fmtMinutes(Math.max(0, pr.estimate_s - (now() - pr.started)) / 60)} left` : "";
      once($("#pack-result"), JSON.stringify([pj.state, pr && pr.result, pr && pr.layout]), () => packResult(pj, pr));
      renderProblem($("#pack-problem"), pj);
    }
    // report
    const rj = slots.report.job;
    $("#report-card").hidden = !rj;
    if (rj) {
      pill($("#report-pill"), rj, { running: "Checking" });
      const rp = progressOf("report");
      once($("#report-body"), JSON.stringify([rj.state, rp && rp.result]), () => reportBody(rj, rp));
      const md = $("#report-md");
      const text = rp && rp.markdown ? rp.markdown : slots.report.lines.join("\n");
      if (md._text !== text) { md._text = text; md.textContent = text; }
      renderProblem($("#report-problem"), rj);
    }

    const ok = d && d.eboot && !d.error;
    const doneHere = ok && pj && pj.state === "done" && pj.meta && pj.meta.src === d.path;
    const already = ok && packedFor(d.path);
    const usb = usbMode();
    once($("#game-sub"), String(usb), () => usb
      ? ["Pick the folder of your own game dump — the one with ", h("code", {}, "eboot.bin"), " and ", h("code", {}, "sce_module"), ". The next step copies it to the USB drive."]
      : ["Pick the folder of your own game dump — the one with ", h("code", {}, "eboot.bin"), " and ", h("code", {}, "sce_module"), ". Preparing it only writes a small layout on this computer; nothing is sent yet."]);
    const btnPack = $("#btn-pack");
    btnPack.hidden = usb;
    btnPack.disabled = !ok || packing || !!ui.inspecting;
    btnPack.className = "btn " + (already ? "secondary" : "primary");
    fill(btnPack, packing ? [spinner(), "Preparing…"] : already ? "Prepare again" : "Prepare game");
    $("#btn-game-next").hidden = usb || !(doneHere || already) || packing;
    $("#btn-game-usb").hidden = !usb;
    $("#btn-game-usb").disabled = !ok || !!ui.inspecting;
    $("#btn-report").disabled = !ok || slotRunning("report");
    $("#btn-pick").disabled = !!ui.inspecting;
  }

  function fitBox(fit, installed) {
    if (!fit) return null;
    const what = {
      fits: ["ok", "Its code fits", "ghostldr can load this game's code inside SWRR."],
      multi: ["info", "Its code needs the multi-buffer placement", "It is big, but ghostldr spreads it over several buffers. It should load."],
      nofit: ["danger", "This game's code is too big for ghostldr", installed ? "It won't start." : "The installer will refuse it, because it couldn't start."],
      unknown: ["warn", "Couldn't check whether its code fits", ""],
    }[fit.kind] || ["warn", "Code check", ""];
    return h("div", { class: "callout " + what[0] }, h("b", {}, what[1]), what[2] ? h("span", {}, what[2] + " ") : null,
      h("span", { class: "hint" }, "(" + fit.text + ")"));
  }

  function packResult(job, pr) {
    if (!pr || job.state !== "done") return null;
    const r = pr.result || {};
    const lay = pr.layout || {};
    const out = [];
    out.push(h("div", { class: "stats" },
      stat("Game", lay.name || r.title || "?"), stat("Title ID", r.title || lay.title || "?"),
      stat("Size", r.gib !== null && r.gib !== undefined ? r.gib.toFixed(1) + " GiB" : fmtSize(lay.total)),
      stat("Files", r.files !== null && r.files !== undefined ? r.files.toLocaleString() : "?"),
      stat("Parts to send", r.containers || lay.containers || "?")));
    const res = h("div", { class: "result" }, fitBox(r.fit, false));
    if (r.notes && r.notes.length) res.append(h("div", { class: "callout warn" }, r.notes.join(" ")));
    if (r.selector) res.append(h("p", { class: "hint" }, "In the game selector: " + r.selector));
    out.push(res);
    return out;
  }

  function stat(k, v) { return h("div", { class: "stat" }, h("div", { class: "k" }, k), h("div", { class: "v" }, String(v))); }

  function reportBody(job, rp) {
    if (isRunning(job)) return h("p", { class: "hint" }, spinner(), " Reading the game's imports… (a minute or two for a big game)");
    if (!rp || !rp.result) return null;
    const r = rp.result;
    const by = r.by_class || {};
    const unres = by.UNRESOLVED || 0;
    // A release has no function-name table: most functions can't be matched then, so they count as unresolved.
    const noNames = r.name_table === false || (rp.notes || []).some((n) => /no names loaded/.test(n));
    const out = [];
    if (noNames) {
      out.push(h("div", { class: "callout warn" }, h("b", {}, "Rough numbers only"),
        "This copy has no function-name table, so most functions can't be matched and are counted as not implemented. A game can play fine with a high count here. ",
        h("span", { class: "hint" }, "(For exact numbers: run tools/fetch_deps.sh once, then check again.)")));
    }
    out.push(h("div", { class: "stats" }, stat("Functions it uses", r.functions ?? "?"),
      stat(noNames ? "Not matched" : "Not implemented yet", unres), stat("Call sites", r.sites ?? "?")));
    out.push(h("ul", { class: "facts" }, Object.entries(by).map(([k, v]) =>
      h("li", { class: k === "UNRESOLVED" && v && !noNames ? "warn" : "" }, h("b", {}, v), " " + k.toLowerCase().replace(/_/g, " ")))));
    const fitKind = /^fits/.test(r.fit_verdict) ? "fits" : /^needs multi/.test(r.fit_verdict) ? "multi" : /^won't fit/.test(r.fit_verdict) ? "nofit" : "unknown";
    if (r.fit_verdict || r.fit_error) out.push(h("div", { class: "result" }, fitBox({ kind: r.fit_verdict ? fitKind : "unknown", text: r.fit_verdict || r.fit_error })));
    if (unres && !noNames) {
      out.push(h("p", {}, "Functions ghostldr doesn't implement yet answer as if they worked. The game may still play, or misbehave where it uses them."));
      if (r.unresolved_libraries && r.unresolved_libraries.length) {
        out.push(h("ul", { class: "facts" }, r.unresolved_libraries.map((l) => h("li", {}, h("b", {}, l.unresolved), " " + l.library))));
      }
    } else if (r.functions && !unres) {
      out.push(h("div", { class: "callout ok" }, "Every function it imports is covered."));
    }
    return out;
  }

  // ---------- 3. install ----------
  function selectedInstallTitle() {
    if (!S) return "";
    const titles = S.packed.map((p) => p.title);
    if (ui.installTitle && titles.includes(ui.installTitle)) return ui.installTitle;
    if (S.settings.title && titles.includes(S.settings.title)) return S.settings.title;
    return titles[0] || "";
  }

  function renderInstall() {
    const sel = $("#sel-install");
    const title = selectedInstallTitle();
    once(sel, JSON.stringify(S.packed.map((p) => [p.title, p.name])), () =>
      S.packed.length ? S.packed.map((p) => h("option", { value: p.title }, (p.name ? p.name + " — " : "") + p.title))
        : h("option", { value: "" }, "No game prepared yet"));
    if (sel.value !== title) sel.value = title;
    sel.disabled = !S.packed.length || slotRunning("install");
    const p = S.packed.find((x) => x.title === title);
    const lib = installedMap();
    const entry = lib && lib[title];
    once($("#install-facts"), JSON.stringify([p, entry, !!lib]), () => {
      if (!p) return null;
      return [h("li", {}, h("b", {}, gib(p.total)), " to send"), h("li", {}, h("b", {}, p.containers), " parts"),
        p.version ? h("li", {}, "v" + p.version) : null,
        h("li", {}, "prepared " + fmtAgo(p.packed)),
        !lib ? h("li", {}, "PS5: not checked") : entry ? h("li", { class: entry.complete ? "ok" : "warn" },
          entry.complete ? "✓ Installed on the PS5" : "Partly installed") : h("li", {}, "Not on the PS5 yet")];
    });
    const installing = slotRunning("install") && slots.install.job.meta.title === title;
    once($("#install-state"), JSON.stringify([!!p, p && p.src_ok, entry, !!lib, installing]), () => {
      if (!p) {
        return h("div", { class: "callout info" }, h("b", {}, "Prepare a game first"), "Choose its folder on the Game step. ",
          h("button", { class: "btn secondary sm", type: "button", onclick: () => go("home", "game") }, "Go to Game"));
      }
      const out = [];
      if (!p.src_ok) out.push(h("div", { class: "callout warn" }, h("b", {}, "The game folder isn't there"),
        "It was prepared from " + (p.src || "a folder") + ", which can't be found now (a disconnected drive?). Connect it again before installing."));
      if (installing) return out;
      if (entry && entry.complete) out.push(h("div", { class: "callout ok" }, "Already installed. Install again only to repair it: it only sends what's missing."));
      else if (entry) out.push(h("div", { class: "callout warn" }, "An earlier install didn't finish. Press Install to continue it."));
      return out;
    });

    const ij = slots.install.job;
    const pr = progressOf("install");
    const running = isRunning(ij);
    $("#install-card").hidden = !ij;
    if (ij) {
      const dry = ij.meta && ij.meta.dry_run;
      const jp = S.packed.find((x) => x.title === ij.meta.title);
      const jname = (jp && jp.name) || ij.meta.title || "";
      $("#install-head").textContent = dry ? (running ? `Checking ${jname}` : `Checked ${jname}`) :
        ij.state === "done" ? `Installed ${jname}` : ij.state === "cancelled" ? `${jname}: install paused` : `Installing ${jname}`;
      pill($("#install-pill"), ij, { running: dry ? "Checking" : "Installing", done: dry ? "Checked" : "Installed", cancelled: "Paused" });
      const bar = $("#install-progress");
      let pct = 0, indet = false;
      if (pr) {
        const total = pr.send_gib * 1024;
        if (pr.phase === "sending" || pr.phase === "finishing") pct = total ? Math.min(100, pr.sent_mib / total * 100) : pr.done / Math.max(1, pr.send_containers) * 100;
        else if (running) indet = true;
        if (ij.state === "done") pct = 100;
      }
      bar.className = "progress" + (indet ? " indet" : "") + (ij.state === "done" ? " done" : ij.state === "failed" ? " failed" : "");
      $(".bar", bar).style.width = pct + "%";
      const phase = { checking: "Checking the game's code…", connecting: "Starting the receiver on the PS5…",
                      planning: "Comparing with what the PS5 has…", sending: "", finishing: "Writing the index…", done: "" };
      let left = "", right = "";
      if (pr && (pr.phase === "sending" || pr.phase === "finishing")) {
        left = `${pr.done} of ${pr.send_containers} parts · ${(pr.sent_mib / 1024).toFixed(1)} of ${pr.send_gib.toFixed(1)} GiB`;
        right = running ? `${pr.rate_mbs.toFixed(1)} MB/s · about ${fmtMinutes(pr.eta_min)} left` : "";
        if (pr.phase === "finishing" && running) right = phase.finishing;
      } else if (pr && running) {
        left = phase[pr.phase] || "Working…";
      }
      if (!running && ij.state === "done") left = dry ? "Nothing was sent." : "Done.";
      if (!running && ij.state === "cancelled") left = "";        // the problem box says it
      $("#install-left").textContent = left;
      $("#install-right").textContent = right;
      once($("#install-msg"), JSON.stringify([ij.state, pr && [pr.complete, pr.nothing, pr.space, pr.send_containers, pr.send_gib, pr.fit, pr.phase, pr.resume,
        running && pr && pr.phase === "sending" && stalledMinutes(pr)]]), () => installMsg(ij, pr, running));
      renderProblem($("#install-problem"), ij, { cancelled: "Paused. What was already sent stays on the PS5: Install continues from there." });
      const cancel = $("#btn-install-cancel");
      cancel.hidden = !running;
      cancel.disabled = !!ij.cancel_requested;
      cancel.textContent = dry ? "Stop" : "Pause install";
    }
    const busyOther = S.console_busy && (!ij || S.console_busy.id !== ij.id);
    const done = ij && ij.state === "done" && !(ij.meta && ij.meta.dry_run) && ij.meta.title === title;
    const btn = $("#btn-install");
    btn.disabled = !p || running || !S.settings.console.ip || !!busyOther;
    fill(btn, running && !(ij.meta && ij.meta.dry_run) ? [spinner(), "Installing…"] :
      entry && !entry.complete || (ij && ij.state === "cancelled") ? "Continue install" : "Install to PS5");
    btn.className = "btn " + (!running && (done || (entry && entry.complete)) ? "secondary" : "primary");
    $("#btn-install-next").hidden = running || !(done || (entry && entry.complete));
    $("#btn-dryrun").disabled = btn.disabled;
  }

  function stalledMinutes(pr) {
    const quiet = now() - pr.updated;
    const per = pr.done ? (pr.updated - pr.started) / pr.done : 120;
    return quiet > Math.max(240, per * 3) ? Math.floor(quiet / 60) : 0;
  }

  function installMsg(job, pr, running) {
    if (!pr) return null;
    const out = [];
    const dry = job.meta && job.meta.dry_run;
    if (running && pr.phase === "sending") {
      const stall = stalledMinutes(pr);
      if (stall) out.push(h("div", { class: "callout warn" }, h("b", {}, `No progress for ${stall} minutes`),
        "Is the PS5 still on, with SWRR open? If it went to sleep or SWRR closed, pause, restart SWRR and Luac0re, and install again: it continues where it stopped."));
      out.push(h("p", { class: "hint" }, "Keep the PS5 awake and SWRR open. You can use this computer meanwhile; pausing is safe."));
    }
    if (pr.space) {
      out.push(h("p", { class: "hint" }, `Space on the PS5: ${pr.space.need_gib.toFixed(1)} GiB needed, ${pr.space.at_least ? "at least " : ""}${pr.space.free_gib.toFixed(1)} GiB free.`));
    }
    if (job.state === "done") {
      if (dry) {
        if (pr.nothing) out.push(h("div", { class: "callout ok" }, "Already installed: there is nothing to send."));
        else if (!pr.send_containers) out.push(h("div", { class: "callout ok" }, h("b", {}, "Its data is already on the PS5"),
          "Installing would send nothing" + (pr.steps.length ? ", only " + pr.steps.join("; ") + "." : ".") + " Nothing was changed."));
        else out.push(h("div", { class: "callout info" }, h("b", {}, "Ready to install"),
          `It would send ${plural(pr.send_containers, "part")} (${pr.send_gib.toFixed(1)} GiB). Nothing was changed.`));
      } else if (pr.nothing) {
        out.push(h("div", { class: "callout ok" }, h("b", {}, "Already installed"), "Nothing needed sending."));
      } else {
        out.push(h("div", { class: "callout ok" }, h("b", {}, "Installed!"),
          "Before playing, restart SWRR: close it, start it again and open HALL OF FAME."));
      }
    }
    if (pr.fit && (pr.fit.kind === "nofit" || job.state === "done" && dry)) out.push(fitBox(pr.fit, false));
    return out;
  }

  // ---------- 3. the USB drive ----------
  const FS_NAMES = { exfat: "exFAT", msdos: "FAT32", apfs: "APFS", hfs: "Mac OS Extended", ntfs: "NTFS" };

  function drivesBody() {
    const st = S.settings;
    const list = ui.drives || [];
    const out = [];
    if (ui.drivesLoading && !list.length) {
      out.push(h("div", { class: "searching" }, h("span", { class: "spin" }), "Looking for drives…"));
    } else if (!list.length) {
      out.push(h("div", { class: "empty" }, "No drive found. Plug the USB drive into this computer, then search again."));
    } else {
      out.push(h("h4", { class: "sub-label" }, "Which drive?"));
      out.push(h("div", { class: "list", role: "radiogroup", "aria-label": "Drives on this computer" }, list.map((d) => {
        const sel = d.path === st.usb_drive;
        const meta = [FS_NAMES[d.fs] || d.fs || "unknown format", `${fmtSize(d.free)} free of ${fmtSize(d.total)}`,
                      d.titles.length ? plural(d.titles.length, "game") + " for ghostldr" : ""].filter(Boolean).join(" · ");
        const badge = !d.fs_ok ? h("span", { class: "pill warn" }, "The PS5 can't read this format")
          : d.fs === "msdos" ? h("span", { class: "pill warn" }, "FAT32: no file over 4 GiB") : h("span", { class: "pill ok" }, "Ready");
        return h("button", { class: "item" + (sel ? " selected" : ""), type: "button", role: "radio", "aria-checked": String(sel),
                             onclick: (ev) => chooseUsb(ev.currentTarget, { where: "computer", drive: d.path }) },
          h("span", { class: "radio" }), h("span", { class: "grow" }, h("span", { class: "name" }, d.name), h("span", { class: "meta" }, meta)),
          h("span", { class: "badges" }, badge));
      })));
    }
    const chosen = list.find((d) => d.path === st.usb_drive);
    if (st.usb_drive && !chosen && ui.drives && !ui.drivesLoading) {
      out.push(h("div", { class: "callout warn" }, h("b", {}, "The chosen drive isn't connected"),
        `${st.usb_drive} can't be found. Plug it in again, or choose another drive.`));
    }
    if (chosen && !chosen.fs_ok) {
      out.push(h("div", { class: "callout warn" }, h("b", {}, "Format this drive exFAT first"),
        "The PS5 reads exFAT and FAT32 drives only. Formatting erases the drive: copy off anything you want to keep first."));
    }
    out.push(h("div", { class: "row" }, h("button", { class: "btn ghost sm", type: "button", disabled: ui.drivesLoading, onclick: () => loadDrives() },
      ui.drivesLoading ? "Searching…" : "Search again")));
    return out;
  }

  function ftpBody(ready) {
    const st = S.settings;
    if (!S.usb.target) {
      return h("div", { class: "callout info" }, h("b", {}, "Choose your PS5 first"), "The copy goes to its address. ",
        h("button", { class: "btn secondary sm", type: "button", onclick: () => go("home", "console") }, "Go to Console"));
    }
    const us = ui.usbStatus;
    const checked = us && us.target === S.usb.target;
    return [
      h("ol", { class: "checklist" },
        h("li", {}, "Plug the drive into the PS5."),
        h("li", {}, "Play any game with ghostldr (Play step). It can stay at its menu: while it runs, its FTP server carries the copy."),
        h("li", { class: "auto " + (ready ? "ok" : "wait") }, st.usb_url ? "The FTP server answers." : "ghostldr's FTP server answers (port 1212).",
          h("span", { class: "loader-state " + (ready ? "ok" : "") }, ready ? "Answering. Ready." : checked ? "Not answering yet. Checking every few seconds…" : "Checking…"))),
      h("p", { class: "hint" }, "Copying to ", h("code", {}, S.usb.target)),
      h("details", { class: "inline-form", open: st.usb_url ? true : null },
        h("summary", {}, "Use another FTP address (a jailbroken PS5's FTP server)"),
        h("form", { class: "row", onsubmit: (e) => { e.preventDefault(); chooseUsb($("button", e.currentTarget), { where: "console", url: $("input", e.currentTarget).value.trim() }); } },
          h("input", { type: "text", placeholder: "ftp://192.168.1.23:2121/mnt/usb0", value: st.usb_url, autocomplete: "off", spellcheck: "false", "aria-label": "FTP address" }),
          h("button", { class: "btn secondary sm", type: "submit" }, "Use"),
          st.usb_url ? h("button", { class: "btn ghost sm", type: "button", onclick: (e) => chooseUsb(e.currentTarget, { where: "console", url: "" }) }, "Use ghostldr's") : null)),
    ];
  }

  function usbGameBody() {
    const d = ui.dump;
    if (ui.inspecting && !d) return h("p", { class: "hint" }, spinner(), " Looking at the folder…");
    if (!d || !d.eboot) {
      return h("div", { class: "callout info" }, h("b", {}, "Choose a game first"), "Pick its folder on the Game step. ",
        h("button", { class: "btn secondary sm", type: "button", onclick: () => go("home", "game") }, "Go to Game"));
    }
    const det = d.detect || {};
    const name = d.profile_name || d.sfo_name || det.title || "Unrecognised game";
    const sub = [det.title && name !== det.title ? det.title : "", det.version ? "v" + det.version : ""].filter(Boolean).join(" · ");
    return [
      h("div", { class: "title-line" }, h("span", {}, name), sub ? h("span", { class: "tid" }, sub) : null),
      h("ul", { class: "facts" },
        d.bytes !== undefined ? h("li", {}, h("b", {}, fmtSize(d.bytes)), " to copy") : null,
        d.files !== undefined ? h("li", {}, h("b", {}, d.files.toLocaleString()), " files") : null,
        det.title && onDrive(det.title) ? h("li", { class: "ok" }, "✓ On the drive") : null),
      h("p", { class: "hint" }, h("code", {}, d.path), " · ", h("a", { href: "#", onclick: (e) => { e.preventDefault(); go("home", "game"); } }, "Change")),
    ];
  }

  function usbCopyResult(job, cp) {
    if (!cp || job.state !== "done" || !cp.summary) return null;
    const s = cp.summary;
    const out = [h("div", { class: "stats" },
      stat("Game", cp.name || s.title), stat("Title ID", s.title), stat("Size", fmtSize(s.gib * 2 ** 30)),
      stat("Copied", s.copied.toLocaleString() + " files"), stat("Already there", s.kept.toLocaleString()))];
    const res = h("div", { class: "result" }, fitBox(cp.fit, true));
    res.append(h("div", { class: "callout ok" }, h("b", {}, "On the drive"), S.settings.usb_where === "computer"
      ? "Eject the drive from this computer and plug it into the PS5, then go to Play."
      : "Before playing it, restart SWRR: close it, start it again and open HALL OF FAME."));
    if (cp.selector) res.append(h("p", { class: "hint" }, "In the game selector: " + cp.selector));
    out.push(res);
    return out;
  }

  function renderUsb() {
    const st = S.settings;
    const where = st.usb_where;
    for (const b of $$("#usb-where .item")) {
      const on = b.dataset.where === where;
      b.classList.toggle("selected", on);
      b.setAttribute("aria-checked", String(on));
    }
    if (where === "computer" && !ui.drives && !ui.drivesLoading) loadDrives();
    if (where === "computer" && S.usb.target) autoList();
    const ready = where === "console" && !!(ui.usbStatus && ui.usbStatus.target === S.usb.target && ui.usbStatus.ready);
    once($("#usb-where-body"), JSON.stringify(where === "computer" ? [where, ui.drives, ui.drivesLoading, st.usb_drive]
      : [where, S.usb.target, st.usb_url, ready, !!ui.usbStatus]), () => (where === "computer" ? drivesBody() : ftpBody(ready)));
    once($("#usb-game"), JSON.stringify([ui.dump, ui.inspecting, onDrive(dumpTitle())]), usbGameBody);

    // what the drive holds
    const lj = slots.usblist.job;
    const reading = isRunning(lj);
    const m = usbMap();
    const lb = $("#btn-usb-list");
    lb.disabled = reading || !S.usb.target;
    fill(lb, reading ? [spinner(), "Reading…"] : "Read the drive");
    once($("#usb-ondrive"), JSON.stringify([m && S.usb.library.entries, reading, S.usb.target, artSig()]), () => {
      if (!S.usb.target) return h("p", { class: "hint" }, "Choose the drive first.");
      if (!m) return h("p", { class: "hint" }, reading ? "Reading the drive…" : where === "console" ? "Not read yet. The PS5's FTP server must be answering." : "Not read yet.");
      if (!S.usb.library.entries.length) return h("p", { class: "hint" }, "No games for ghostldr on it yet.");
      return h("div", { class: "list" }, S.usb.library.entries.map((e) => h("div", { class: "item static" }, artThumb(e.title, e.name),
        h("span", { class: "grow" }, h("span", { class: "name" }, e.name || e.title),
          h("span", { class: "meta" }, [e.title, e.version ? "v" + e.version : ""].filter(Boolean).join(" · "))))));
    });
    renderProblem($("#usb-list-problem"), lj);

    // the copy
    const cj = slots.usbcopy.job;
    const cp = progressOf("usbcopy");
    const copying = isRunning(cj);
    const shown = cj && (copying || cj.meta.target === S.usb.target);   // a finished copy to another drive: not this one's
    $("#usbcopy-card").hidden = !shown;
    if (shown) {
      const jname = (cp && cp.name) || cj.meta.name || cj.meta.title || "the game";
      $("#usbcopy-head").textContent = copying ? `Copying ${jname}` : cj.state === "done" ? `Copied ${jname}` :
        cj.state === "cancelled" ? `${jname}: copy stopped` : `Copying ${jname}`;
      pill($("#usbcopy-pill"), cj, { running: "Copying", done: "On the drive", cancelled: "Stopped" });
      let pct = 0, indet = false, left = "", right = "";
      if (cp && cp.phase === "copying") {
        pct = cp.need_gib > 0 ? cp.done_gib / cp.need_gib * 100 : cp.todo ? cp.count / cp.todo * 100 : 0;
        left = `${cp.done_gib.toFixed(1)} of ${cp.need_gib.toFixed(1)} GiB · ${cp.count.toLocaleString()} of ${cp.todo.toLocaleString()} files`;
        if (copying && cp.rate) right = `${cp.rate} MiB/s · about ${fmtMinutes((cp.need_gib - cp.done_gib) * 1024 / cp.rate / 60)} left`;
      } else if (cp && cp.phase === "finishing") {
        pct = 99;
        left = copying ? "Writing the name and art…" : "";
      } else if (copying) {
        indet = true;
        left = "Looking at the drive…";
      }
      if (cj.state === "done") { pct = 100; left = cp && cp.summary && !cp.summary.copied ? "Everything was on the drive already." : "Done."; right = ""; }
      if (!copying && cj.state !== "done") { left = ""; right = ""; }
      const bar = $("#usbcopy-progress");
      bar.className = "progress" + (indet ? " indet" : "") + (cj.state === "done" ? " done" : cj.state === "failed" ? " failed" : "");
      $(".bar", bar).style.width = Math.min(100, pct) + "%";
      $("#usbcopy-left").textContent = left;
      $("#usbcopy-right").textContent = right;
      once($("#usbcopy-result"), JSON.stringify([cj.state, cp && [cp.summary, cp.fit, cp.selector, cp.name]]), () => usbCopyResult(cj, cp));
      renderProblem($("#usbcopy-problem"), cj, { cancelled: "Stopped. What was copied stays on the drive: Copy continues from there." });
      const cancel = $("#btn-usbcopy-cancel");
      cancel.hidden = !copying;
      cancel.disabled = !!cj.cancel_requested;
    }

    // the buttons
    const d = ui.dump;
    const ok = d && d.eboot && !d.error;
    const chosen = where === "computer" ? (ui.drives || []).find((x) => x.path === st.usb_drive) : null;
    const fsBad = !!(chosen && !chosen.fs_ok);
    const t = dumpTitle();
    const there = t && onDrive(t);
    const btn = $("#btn-usb-copy");
    btn.disabled = !ok || copying || !S.usb.target || fsBad || (where === "console" && !ready);
    btn.className = "btn " + (there && !copying ? "secondary" : "primary");
    fill(btn, copying ? [spinner(), "Copying…"] : there ? "Copy again" : shown && cj.state === "cancelled" ? "Continue copying" : "Copy to USB drive");
    $("#btn-usb-next").hidden = copying || !there;
    $("#usb-copy-hint").textContent = copying ? "You can use this computer meanwhile; stopping is safe." : !ok ? "Choose a game first." :
      !S.usb.target ? (where === "computer" ? "Choose the drive first." : "Choose your PS5 first.") : fsBad ? "Format the drive exFAT first." :
      where === "console" && !ready ? "Waiting for the PS5's FTP server: play a game with ghostldr first." : there ? "Copying again only sends what changed." : "";
  }

  // ---------- 4. play ----------
  function playChoices() {
    if (usbMode()) {
      const m = usbMap();
      if (m) return S.usb.library.entries.map((e) => ({ title: e.title, name: e.name, version: e.version, complete: true }));
      return Object.entries(S.usb.copies).filter(([, c]) => c.target === S.usb.target)
        .map(([t, c]) => ({ title: t, name: c.name, gib: c.gib, complete: true, guess: true }));
    }
    const lib = installedMap();
    const list = [];
    if (lib) {
      for (const e of S.library.entries) list.push({ title: e.title, name: e.name, version: e.version, gib: e.gib, complete: e.complete });
    } else {
      for (const p of S.packed) list.push({ title: p.title, name: p.name, version: p.version, gib: p.total / 2 ** 30, complete: true, guess: true });
    }
    return list;
  }

  function renderPlay() {
    const list = playChoices();
    const usb = usbMode();
    const lib = usb ? usbMap() : installedMap();
    $("#play-usb-plug").hidden = !(usb && S.settings.usb_where === "computer");
    const canSelect = S.prebuilt["selector.bin"];
    const usable = list.filter((c) => c.complete && c.title);
    if (!ui.playChoice || (ui.playChoice !== "*select*" && !usable.some((c) => c.title === ui.playChoice))) {
      // the last game played or prepared, else the most recently prepared one that is installed
      const recent = S.packed.map((p) => p.title);
      const pickIt = usable.find((c) => c.title === S.settings.title) ||
        recent.map((t) => usable.find((c) => c.title === t)).find(Boolean) || usable[0];
      ui.playChoice = pickIt ? pickIt.title : canSelect ? "*select*" : "";
    }
    once($("#play-choices"), JSON.stringify([list, ui.playChoice, !!lib, canSelect, artSig(), usb]), () => {
      const out = [];
      if (!list.length && usb) {
        out.push(h("div", { class: "empty" }, "No game on the USB drive yet.",
          h("br"), h("button", { class: "btn secondary sm", type: "button", onclick: () => go("home", "usb") }, "Go to USB drive")));
      } else if (!list.length) {
        out.push(h("div", { class: "empty" }, lib ? "Nothing is installed on the PS5 yet." : "No game prepared yet.",
          h("br"), h("button", { class: "btn secondary sm", type: "button", onclick: () => go("home", lib ? "install" : "game") }, lib ? "Go to Install" : "Go to Game")));
      }
      for (const c of list) {
        const selected = ui.playChoice === c.title;
        out.push(h("button", { class: "item" + (selected ? " selected" : ""), type: "button", role: "radio", "aria-checked": String(selected),
                               disabled: !c.complete || !c.title, onclick: () => { ui.playChoice = c.title; api("/api/settings", { title: c.title }).catch(() => {}); render(); } },
          h("span", { class: "radio" }), artThumb(c.title, c.name),
          h("span", { class: "grow" }, h("span", { class: "name" }, c.name || c.title || "Untitled"),
            h("span", { class: "meta" }, [c.title, c.version ? "v" + c.version : "", c.gib ? c.gib.toFixed(1) + " GiB" : ""].filter(Boolean).join(" · "))),
          h("span", { class: "badges" }, !c.complete ? h("span", { class: "pill warn" }, "Install not finished") : null)));
      }
      if (canSelect && (list.length > 1 || !lib)) {
        const selected = ui.playChoice === "*select*";
        out.push(h("button", { class: "item" + (selected ? " selected" : ""), type: "button", role: "radio", "aria-checked": String(selected),
                               onclick: () => { ui.playChoice = "*select*"; render(); } },
          h("span", { class: "radio" }), h("span", { class: "art-sm", style: "background:var(--accent)" }, "▦"),
          h("span", { class: "grow" }, h("span", { class: "name" }, "Pick on the PS5"),
            h("span", { class: "meta" }, "Shows the game selector on the TV"))));
      }
      if (!lib && list.length && usb) out.push(h("p", { class: "hint" }, "Showing the games this app copied to the drive. ",
        h("a", { href: "#", onclick: (e) => { e.preventDefault(); go("library"); } }, "Read the drive"), " in the Library to see everything on it."));
      else if (!lib && list.length) out.push(h("p", { class: "hint" }, "Showing the games prepared on this computer. ",
        h("a", { href: "#", onclick: (e) => { e.preventDefault(); go("library"); } }, "Refresh the Library"), " to see what's on the PS5."));
      return out;
    });

    const bj = slots.boot.job;
    const bp = progressOf("boot");
    const running = isRunning(bj);
    $("#boot-card").hidden = !bj;
    if (bj) {
      $("#boot-head").textContent = bj.label;
      pill($("#boot-pill"), bj, { running: "Live", done: "Ended", cancelled: "Log stopped" });
      const stages = [["sending", "Sending"], ["core", "Loader started"], ["extension", "Extension"], ["title", "Game data"], ["starting", "Game started"]];
      const reached = bp && bp.stage ? stages.findIndex((s) => s[0] === bp.stage) : -1;
      once($("#boot-timeline"), JSON.stringify([reached, running]), () => stages.map((s, i) =>
        h("li", { class: i <= reached ? "done" : i === reached + 1 && running ? "now" : "" }, s[1])));
      once($("#boot-notes"), JSON.stringify([bp, bj.state]), () => bootNotes(bj, bp, running));
      renderProblem($("#boot-problem"), bj, { cancelled: "Stopped watching the log. The game keeps running on the PS5 until you close SWRR." });
      $("#btn-boot-stop").hidden = !running;
      $("#btn-boot-stop").disabled = !!bj.cancel_requested;
      $("#boot-stop-hint").hidden = !running;
      $("#btn-boot-stubs").hidden = running || !S.runlog;
    }
    const busyOther = S.console_busy && (!bj || S.console_busy.id !== bj.id);
    const btn = $("#btn-play");
    btn.disabled = running || !ui.playChoice || !S.settings.console.ip || !!busyOther;
    const playSvg = '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M8 5v14l11-7z"/></svg>';
    const label = running ? "Playing…" : "Play";
    if (btn._l !== label) { btn._l = label; btn.innerHTML = playSvg; btn.append(label); }
    $("#play-hint").textContent = !S.settings.console.ip ? "Choose your PS5 first." : busyOther ? `Wait for “${S.console_busy.label}”.` :
      !ui.playChoice ? (usb ? "Copy a game to the USB drive first." : "Install a game first.") : running ? "" :
      usb ? "Make sure the drive is in the PS5, SWRR was restarted and the loader listens." : "Make sure SWRR was restarted and the loader listens.";
  }

  function bootNotes(job, bp, running) {
    if (!bp) return null;
    const out = [];
    const pf = bp.preflight || {};
    if (pf.state === "unresolved" || pf.state === "stopped") {
      out.push(h("div", { class: "callout warn" }, h("b", {}, "Some functions this game uses aren't implemented yet"),
        pf.state === "unresolved" ? pf.text : "It may still play, or misbehave where it uses them. ",
        !running ? h("div", { class: "row", style: "margin-top:8px" }, h("button", { class: "btn secondary sm", type: "button", onclick: () => play(true) }, "Play anyway")) : null));
    }
    if (bp.crash) out.push(h("div", { class: "callout danger" }, h("b", {}, "The game crashed"), h("code", {}, bp.crash),
      h("div", { class: "hint" }, "The full report is in the log below and in build/ghostldr/run.log.")));
    for (const p of bp.problems || []) {
      out.push(h("div", { class: "callout danger" }, h("b", {}, "The PS5 couldn't fetch the extension from this computer"),
        "Allow Python through this computer's firewall (TCP 18195), restart SWRR, open HALL OF FAME and press Play again. ",
        h("a", { href: "#", onclick: (e) => { e.preventDefault(); go("help", null, "firewall"); } }, "How")));
    }
    for (const hint of bp.hints || []) out.push(h("div", { class: "callout warn" }, h("b", {}, "Hint"), hint));
    if (running && bp.stage === "sending" && !bp.log_lines && now() - bp.last_log > 25) {
      out.push(h("div", { class: "callout warn" }, h("b", {}, "No log from the PS5 yet"),
        "If the TV stays black, this computer's firewall may block UDP 18194. ",
        h("a", { href: "#", onclick: (e) => { e.preventDefault(); go("help", null, "firewall"); } }, "Firewall help")));
    }
    if (running && bp.stage === "starting") out.push(h("p", { class: "hint" }, "The game is starting on your TV. The first start can take a minute or two."));
    return out;
  }

  async function play(force) {
    const choice = ui.playChoice;
    const select = choice === "*select*";
    if (!select && choice) api("/api/settings", { title: choice }).catch(() => {});
    go("home", "play");
    await startJob($("#btn-play"), "/api/boot", { title: select ? "" : choice, select, force_preflight: !!force, source: mode() }, "boot");
  }

  // ---------- art ----------
  const art = new Map();               // title -> object URL, null (none) or a pending promise
  function loadArt(title) {
    if (!title || art.has(title)) return;
    art.set(title, api(`/api/art/${encodeURIComponent(title)}`, undefined, { blob: true })
      .then((b) => { art.set(title, URL.createObjectURL(b)); render(); })
      .catch(() => { art.set(title, null); }));
  }
  function artFor(title) {
    loadArt(title);
    const a = art.get(title);
    return typeof a === "string" ? a : null;
  }
  const artSig = () => [...art.entries()].filter(([, v]) => typeof v === "string").map(([k]) => k).join();
  function genBg(title) {
    const hh = hue(title || "?");
    return `linear-gradient(135deg, hsl(${hh} 55% 46%), hsl(${(hh + 40) % 360} 60% 30%))`;
  }
  function artThumb(title, name) {
    const src = artFor(title);
    if (src) return h("span", { class: "art-sm" }, h("img", { src, alt: "" }));
    return h("span", { class: "art-sm", style: `background:${genBg(title)}` }, (name || title || "?").slice(0, 2).toUpperCase());
  }
  function tileArt(title, name, badge) {
    const src = artFor(title);
    return h("div", { class: "art", style: src ? null : `background:${genBg(title)}` },
      src ? h("img", { src, alt: "" }) : h("div", { class: "gen" }, name || title || "Untitled", h("small", {}, title)),
      badge ? h("span", { class: "badge" }, badge) : null);
  }

  // ---------- library ----------
  function renderLibrary() {
    const lj = slots.library.job;
    const lib = S.library;
    const running = slotRunning("library");
    const b = $("#btn-lib-refresh");
    b.disabled = running || !S.settings.console.ip || !!S.console_busy;
    fill(b, running ? [spinner(), "Reading…"] : "Refresh from PS5");
    $("#lib-status").textContent = !S.settings.console.ip ? "Choose your PS5 on the Console step first." :
      running ? "Reading the list from the PS5…" :
      lib.ok ? `${plural(lib.entries.length, "game")} installed · checked ${fmtAgo(lib.refreshed)}` +
        (lib.stale ? " · something changed since: refresh" : "") :
      "Not read yet. Start Luac0re on the PS5 (HALL OF FAME), then press Refresh.";
    renderProblem($("#lib-problem"), lj);
    const artKeys = artSig();
    once($("#lib-tiles"), JSON.stringify([lib.entries, lib.ok, artKeys, S.console_busy && S.console_busy.id]), () => {
      if (lib.ok && !lib.entries.length) return h("div", { class: "empty lib-empty" }, "Nothing installed on the PS5 yet.");
      return lib.entries.map((e) => h("div", { class: "tile" },
        tileArt(e.title, e.name, !e.complete ? h("span", { class: "pill warn" }, "Install not finished") : null),
        h("div", { class: "body" }, h("div", { class: "name" }, e.name || e.title || "Untitled"),
          h("div", { class: "meta" }, [e.title, e.version ? "v" + e.version : "", e.gib ? e.gib.toFixed(1) + " GiB" : ""].filter(Boolean).join(" · ")),
          h("div", { class: "acts" },
            e.complete ? h("button", { class: "btn ghost sm", type: "button", onclick: () => { ui.playChoice = e.title; go("home", "play"); } }, "Play") : null,
            h("button", { class: "btn ghost sm", type: "button", disabled: !!S.console_busy, onclick: (ev) => updateMeta(ev.currentTarget, e) }, "Name & art"),
            h("button", { class: "btn ghost sm danger-text", type: "button", disabled: !!S.console_busy, onclick: (ev) => removeTitle(ev.currentTarget, e) }, "Remove")))));
    });
    once($("#packed-tiles"), JSON.stringify([S.packed, artKeys]), () => {
      if (!S.packed.length) return h("div", { class: "empty lib-empty" }, "No game prepared on this computer. ",
        h("button", { class: "btn secondary sm", type: "button", onclick: () => go("home", "game") }, "Choose a game"));
      return S.packed.map((p) => h("div", { class: "tile" },
        tileArt(p.title, p.name, !p.src_ok ? h("span", { class: "pill warn" }, "Folder missing") : null),
        h("div", { class: "body" }, h("div", { class: "name" }, p.name || p.title),
          h("div", { class: "meta" }, [p.title, p.version ? "v" + p.version : "", gib(p.total)].filter(Boolean).join(" · ")),
          h("div", { class: "meta" }, "prepared " + fmtAgo(p.packed)),
          h("div", { class: "acts" }, h("button", { class: "btn ghost sm", type: "button", onclick: () => { ui.installTitle = p.title; go("home", "install"); } }, "Install")))));
    });

    // the USB drive
    const ulj = slots.usblist.job;
    const ureading = isRunning(ulj);
    const um = usbMap();
    const ub = $("#btn-usb-lib-refresh");
    ub.disabled = ureading || !S.usb.target;
    fill(ub, ureading ? [spinner(), "Reading…"] : "Read the drive");
    $("#usb-lib-status").textContent = !S.usb.target ? "No USB drive chosen yet: choose one on the USB drive step (USB drive path)." :
      ureading ? "Reading the drive…" :
      um ? `${plural(S.usb.library.entries.length, "game")} on ${S.usb.target} · read ${fmtAgo(S.usb.library.refreshed)}` :
      S.settings.usb_where === "console" ? `Not read yet. A drive in the PS5 is read through ghostldr's FTP server, while a game runs.` : "Not read yet.";
    renderProblem($("#usb-lib-problem"), ulj);
    once($("#usb-tiles"), JSON.stringify([um && S.usb.library.entries, artKeys, S.usb.target, S.console_busy && S.console_busy.id]), () => {
      if (!um) return null;
      if (!S.usb.library.entries.length) return h("div", { class: "empty lib-empty" }, "No games for ghostldr on this drive yet.");
      return S.usb.library.entries.map((e) => h("div", { class: "tile" },
        tileArt(e.title, e.name, null),
        h("div", { class: "body" }, h("div", { class: "name" }, e.name || e.title),
          h("div", { class: "meta" }, [e.title, e.version ? "v" + e.version : ""].filter(Boolean).join(" · ")),
          h("div", { class: "acts" },
            h("button", { class: "btn ghost sm", type: "button", onclick: async () => { ui.playChoice = e.title; if (!usbMode()) await setMode("usb"); go("home", "play"); } }, "Play"),
            h("button", { class: "btn ghost sm danger-text", type: "button", onclick: (ev) => removeUsbTitle(ev.currentTarget, e) }, "Remove")))));
    });

    // library jobs (remove, name & art)
    const xj = slots.libjob.job;
    $("#libjob-card").hidden = !xj;
    if (xj) {
      $("#libjob-head").textContent = xj.label;
      pill($("#libjob-pill"), xj, { running: "Working" });
      $("#libjob-msg").textContent = isRunning(xj) ? "Working with the PS5…" :
        xj.state === "done" ? (xj.kind === "remove" ? "Removed. Its progress save was kept." :
          xj.kind === "usbremove" ? "Removed from the USB drive. Its progress save on the PS5 was kept." : "Updated. Restart SWRR before playing.") : "";
      renderProblem($("#libjob-problem"), xj);
    }

    // save manager
    const m = S.manager;
    const mj = slots.manager.job;
    const mrun = isRunning(mj);
    once($("#manager-info"), JSON.stringify([m, mrun]), () => {
      if (!m.running) return null;
      return h("div", { class: "callout ok" }, h("b", {}, "The save manager is running"),
        h("div", {}, "Address: ", h("code", {}, m.url)), m.token ? h("div", {}, "Token (for changes): ", h("code", {}, m.token)) : null,
        h("div", { class: "hint" }, "The PS5 shows the token too."));
    });
    renderProblem($("#manager-problem"), mj);
    $("#btn-manager-start").hidden = m.running;
    $("#btn-manager-start").disabled = mrun || !S.settings.console.ip || !!S.console_busy;
    fill($("#btn-manager-start"), mrun && !m.running ? [spinner(), "Starting…"] : "Open save manager");
    $("#btn-manager-open").hidden = !m.running;
    $("#btn-manager-stop").hidden = !m.running;
    $("#btn-manager-stop").disabled = mrun;
  }

  async function updateMeta(btn, e) {
    const ok = await confirmBox({ title: `Update the name and art of ${e.name || e.title}?`,
      text: "This downloads the game's official picture and background from the PlayStation Store and writes one small entry to the PS5. The game, its progress and SWRR's own save aren't touched. SWRR must be at HALL OF FAME.",
      ok: "Update" });
    if (ok) startJob(btn, "/api/library/meta", { title: e.title, fetch_art: true }, "libjob");
  }

  async function removeUsbTitle(btn, e) {
    const typed = await confirmBox({ title: `Remove ${e.name || e.title} from the USB drive?`,
      text: "This deletes the game's folder from the drive's ghostldr folder. Its progress (save) on the PS5 is kept, and nothing else on the drive is touched. You'd need to copy it again to play.",
      ok: "Remove", danger: true, field: { label: `Type ${e.title} to confirm`, expect: e.title } });
    if (typed) startJob(btn, "/api/usb/remove", { title: e.title, confirm: typed }, "libjob");
  }

  async function removeTitle(btn, e) {
    const typed = await confirmBox({ title: `Remove ${e.name || e.title} from the PS5?`,
      text: "This deletes the game's installed data from SWRR's saved data. Its progress (save) is kept, and other games and SWRR's own save aren't touched. You'd need to install it again to play.",
      ok: "Remove", danger: true, field: { label: `Type ${e.title} to confirm`, expect: e.title } });
    if (typed) startJob(btn, "/api/library/remove", { title: e.title, confirm: typed }, "libjob");
  }

  // ---------- help ----------
  function showHelpPane(pane) {
    helpPane = pane;
    for (const t of $$(".subtab")) t.setAttribute("aria-selected", String(t.dataset.help === pane));
    for (const p of $$(".helppane")) p.hidden = p.dataset.pane !== pane;
    if (pane === "log") loadRunlog();
    if (pane === "trouble") loadTrouble();
    if (pane === "activity" && ui.activity) loadActivity(ui.activity);
    renderHelp();
  }

  function renderHelp() {
    for (const t of $$(".subtab")) t.setAttribute("aria-selected", String(t.dataset.help === helpPane));
    for (const p of $$(".helppane")) p.hidden = p.dataset.pane !== helpPane;
    if (helpPane === "activity") {
      once($("#activity-list"), JSON.stringify([S.jobs.map((j) => [j.id, j.state]), ui.activity]), () => S.jobs.map((j) =>
        h("li", {}, h("button", { class: "item" + (ui.activity === j.id ? " selected" : ""), type: "button", onclick: () => { ui.activity = j.id; loadActivity(j.id); render(); } },
          h("span", { class: "grow" }, h("span", { class: "name" }, j.label), h("span", { class: "meta" }, fmtTime(j.started) + (j.ended ? " · " + Math.max(1, Math.round(j.ended - j.started)) + " s" : ""))),
          h("span", { class: "pill " + (isRunning(j) ? "run" : j.state === "done" ? "ok" : j.state === "cancelled" ? "warn" : "danger") },
            isRunning(j) ? "running" : j.state)))));
    }
    if (helpPane === "about") {
      once($("#about-kv"), JSON.stringify([S.version, S.webview, S.prebuilt]), () => {
        const rows = [["App", S.version], ["Python", S.python], ["System", S.platform], ["Window", S.webview ? "native (pywebview)" : "browser"],
          ["ghostldr folder", S.root], ["Settings", S.config],
          ["Loader files", Object.entries(S.prebuilt).map(([n, ok]) => (ok ? "✓ " : "✕ ") + n).join("  ")]];
        return rows.flatMap(([k, v]) => [h("dt", {}, k), h("dd", {}, v)]);
      });
    }
    if (helpPane === "log") {
      const sj = slots.stubs.job;
      $("#stubs-card").hidden = !sj;
      if (sj) {
        pill($("#stubs-pill"), sj);
        const sp = progressOf("stubs");
        once($("#stubs-body"), JSON.stringify([sj.state, sp && sp.result]), () => stubsBody(sj, sp));
        renderProblem($("#stubs-problem"), sj);
      }
      $("#btn-stubs").disabled = !S.runlog || slotRunning("stubs");
      if (slotRunning("boot") && (!ui.runlogAt || now() - ui.runlogAt > 4)) loadRunlog();
    }
  }

  function stubsBody(job, sp) {
    if (!sp || !sp.result) return null;
    const r = sp.result;
    if (!r.stub_lines) return h("div", { class: "callout ok" }, "The game didn't call anything ghostldr is missing in this log.");
    return [h("div", { class: "stats" }, stat("Calls to missing functions", r.stub_lines), stat("Different functions", r.distinct_stubs),
      stat("Refused calls", r.denied_hits)),
      h("ul", { class: "rows", style: "margin-top:12px" }, (r.top || []).slice(0, 25).map((x) =>
        h("li", {}, h("span", { class: "ico WARN" }, "!"), h("div", {}, h("div", { class: "what" }, `${x.name || x.nid}`),
          h("div", { class: "why" }, `${x.lib} · called ${x.count} times${x.kind === "DENIED" ? " · refused" : ""}`)))))];
  }

  async function loadRunlog() {
    ui.runlogAt = now();
    try {
      const r = await api("/api/runlog?tail=1500");
      const pre = $("#runlog");
      const text = r.lines.join("\n");
      $("#runlog-meta").textContent = r.exists ? `${fmtSize(r.size)} · updated ${fmtAgo(r.modified)} · ${r.path}` : "No game log yet: it appears after the first Play.";
      if (pre._text !== text) {
        const atEnd = pre.scrollTop + pre.clientHeight >= pre.scrollHeight - 30;
        pre._text = text;
        fill(pre, r.lines.map((l) => { const c = classifyLog(l); return c ? h("span", { class: c }, l + "\n") : l + "\n"; }));
        if (atEnd || !pre._scrolled) { pre.scrollTop = pre.scrollHeight; pre._scrolled = true; }
      }
    } catch (e) { /* offline banner */ }
  }

  async function loadTrouble() {
    if (ui.trouble) return;
    try {
      ui.trouble = (await api("/api/trouble")).problems;
      fill($("#trouble-list"), ui.trouble.flatMap((p) => [h("dt", {}, p.title), h("dd", {}, p.fix)]));
    } catch (e) { /* later */ }
  }

  async function loadActivity(id) {
    $("#activity-label").textContent = "Loading…";
    try {
      const r = await api(`/api/jobs/${id}?since=0`);
      $("#activity-label").textContent = `${r.label} · ${r.state}${r.rc !== null ? " (exit " + r.rc + ")" : ""}${r.truncated ? " · older lines dropped" : ""}`;
      $("#activity-out").textContent = r.output.join("\n");
    } catch (e) {
      $("#activity-label").textContent = "That run is no longer kept.";
      $("#activity-out").textContent = "";
    }
  }

  // ---------- dialogs ----------
  function confirmBox(o) {
    const dlg = $("#dlg-confirm");
    $("#confirm-title").textContent = o.title || "Are you sure?";
    $("#confirm-text").textContent = o.text || "";
    const okBtn = $("#confirm-ok");
    okBtn.textContent = o.ok || "OK";
    okBtn.className = "btn " + (o.danger ? "danger" : "primary");
    const field = $("#confirm-field"), input = $("#confirm-input");
    field.hidden = !o.field;
    input.value = "";
    if (o.field) {
      $("#confirm-field-label").textContent = o.field.label;
      okBtn.disabled = true;
      input.oninput = () => { okBtn.disabled = input.value.trim().toUpperCase() !== o.field.expect.toUpperCase(); };
      input.onkeydown = (e) => {
        if (e.key !== "Enter") return;
        e.preventDefault();
        if (!okBtn.disabled) dlg.close("ok");
      };
    } else okBtn.disabled = false;
    return new Promise((resolve) => {
      dlg.onclose = () => resolve(dlg.returnValue === "ok" ? (o.field ? input.value.trim() : true) : false);
      dlg.returnValue = "";
      dlg.showModal();
      (o.field ? input : okBtn).focus();
    });
  }

  let browsePath = "";
  async function browse(path) {
    try {
      const r = await api("/api/folder/list", { path: path || "" });
      browsePath = r.path;
      $("#browse-path").textContent = r.path;
      $("#browse-up").disabled = !r.parent;
      $("#browse-up").onclick = () => browse(r.parent);
      fill($("#browse-roots"), r.roots.map((x) => h("button", { class: "btn sm", type: "button", onclick: () => browse(x.path) }, x.name)));
      const folder = '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M3 6.5A1.5 1.5 0 0 1 4.5 5h4.6l2 2.2h8.4A1.5 1.5 0 0 1 21 8.7v9.8a1.5 1.5 0 0 1-1.5 1.5h-15A1.5 1.5 0 0 1 3 18.5z"/></svg>';
      fill($("#browse-list"), r.dirs.length ? r.dirs.map((d) => {
        const b = h("button", { type: "button", ondblclick: () => browse(d.path), onclick: () => browse(d.path) });
        b.innerHTML = folder;
        b.append(h("span", { class: "nm" }, d.name), d.game ? h("span", { class: "pill ok" }, "game") : "");
        return h("li", {}, b);
      }) : h("li", { class: "hint", style: "padding:12px" }, "No folders here."));
      $("#browse-hint").textContent = r.game ? "This folder has eboot.bin: use it." : "Folders with a game are marked “game”.";
      $("#browse-use").className = "btn " + (r.game ? "primary" : "secondary");
    } catch (e) { handleError(e); }
  }

  async function pickFolder(btn) {
    await act(btn, async () => {
      const r = await api("/api/folder/pick", {});
      if (r.available) { if (r.path) await inspect(r.path); return; }
      openBrowser();
    });
  }

  function openBrowser() {
    const start = (ui.dump && ui.dump.path) || (S && S.settings.dump) || "";
    $("#dlg-browse").showModal();
    browse(start);
  }

  // ---------- wiring ----------
  function wire() {
    for (const tab of $$(".tab")) tab.addEventListener("click", () => go(tab.dataset.view));
    for (const s of $$(".step")) s.addEventListener("click", () => go("home", s.dataset.step));
    $("#console-chip").addEventListener("click", () => go("home", "console"));
    for (const t of $$(".subtab")) t.addEventListener("click", () => showHelpPane(t.dataset.help));
    document.addEventListener("click", (e) => {
      const a = e.target.closest("[data-goto]");
      if (!a) return;
      e.preventDefault();
      const [v, pane] = a.dataset.goto.split(":");
      go(v, null, pane);
    });

    // console
    $("#btn-discover").addEventListener("click", (e) => startJob(e.currentTarget, "/api/discover", {}, "discover"));
    $("#form-ip").addEventListener("submit", (e) => {
      e.preventDefault();
      const btn = $("button", e.currentTarget);
      act(btn, async () => {
        const c = (await api("/api/console/select", { ip: $("#inp-ip").value })).console;
        toast(c && c.name ? `Using ${c.name}.` : "Using that address.");
        $("#inp-ip").value = "";
        await refresh();
      });
    });
    $("#btn-doctor").addEventListener("click", (e) => startJob(e.currentTarget, "/api/doctor", { title: selectedInstallTitle() }, "doctor"));
    $("#btn-console-next").addEventListener("click", () => {
      if (usbMode()) go("home", !ui.dump ? "game" : onDrive(dumpTitle()) ? "play" : "usb");
      else go("home", S.packed.length ? (installedMap() ? "play" : "install") : "game");
    });
    for (const b of $$(".path")) b.addEventListener("click", (e) => setMode(e.currentTarget.dataset.mode, e.currentTarget));

    // game
    $("#btn-pick").addEventListener("click", (e) => pickFolder(e.currentTarget));
    $("#btn-browse").addEventListener("click", openBrowser);
    $("#form-path").addEventListener("submit", (e) => {
      e.preventDefault();
      const v = $("#inp-path").value.trim().replace(/^["']|["']$/g, "");
      if (v) { $("#inp-path").value = ""; inspect(v); }
    });
    $("#browse-use").addEventListener("click", () => { $("#dlg-browse").close(); inspect(browsePath); });
    $("#chk-art").addEventListener("change", (e) => api("/api/settings", { fetch_art: e.currentTarget.checked }).catch(() => {}));
    $("#btn-pack").addEventListener("click", (e) => {
      if (!ui.dump) return;
      startJob(e.currentTarget, "/api/pack", { src: ui.dump.path, title: $("#inp-title").value.trim(), fetch_art: $("#chk-art").checked }, "pack")
        .then((job) => { if (job) ui.installTitle = ""; });
    });
    $("#btn-report").addEventListener("click", (e) => { if (ui.dump) startJob(e.currentTarget, "/api/report", { src: ui.dump.path }, "report"); });
    $("#btn-game-next").addEventListener("click", () => {
      const p = ui.dump && packedFor(ui.dump.path);
      if (p) ui.installTitle = p.title;
      go("home", "install");
    });

    // the USB drive
    $("#btn-game-usb").addEventListener("click", () => go("home", "usb"));
    for (const b of $$("#usb-where .item")) b.addEventListener("click", (e) => chooseUsb(e.currentTarget, { where: e.currentTarget.dataset.where }));
    $("#btn-usb-copy").addEventListener("click", (e) => {
      if (!ui.dump) return;
      startJob(e.currentTarget, "/api/usb/copy", { src: ui.dump.path, title: $("#inp-title").value.trim(), fetch_art: $("#chk-art").checked }, "usbcopy");
    });
    $("#btn-usbcopy-cancel").addEventListener("click", () => cancelJob("usbcopy", {
      title: "Stop copying?", text: "What was already copied stays on the drive. Press Copy later to continue from there.", ok: "Stop" }));
    $("#btn-usb-next").addEventListener("click", () => { const t = dumpTitle(); if (t) ui.playChoice = t; go("home", "play"); });
    $("#btn-usb-list").addEventListener("click", (e) => startJob(e.currentTarget, "/api/usb/list", {}, "usblist"));
    $("#btn-usb-lib-refresh").addEventListener("click", (e) => startJob(e.currentTarget, "/api/usb/list", {}, "usblist"));

    // install
    $("#sel-install").addEventListener("change", (e) => { ui.installTitle = e.currentTarget.value; render(); });
    const install = (btn, dry) => startJob(btn, "/api/install", { title: selectedInstallTitle(), dry_run: dry, no_fit_check: $("#chk-nofit").checked }, "install");
    $("#btn-install").addEventListener("click", (e) => install(e.currentTarget, false));
    $("#btn-dryrun").addEventListener("click", (e) => install(e.currentTarget, true));
    $("#btn-install-cancel").addEventListener("click", () => cancelJob("install", {
      title: "Pause the install?", text: "What was already sent stays on the PS5. Press Install later to continue from there.", ok: "Pause" }));
    $("#btn-install-next").addEventListener("click", () => { ui.playChoice = selectedInstallTitle(); go("home", "play"); });

    // play
    $("#btn-play").addEventListener("click", () => play(false));
    $("#btn-boot-stop").addEventListener("click", () => cancelJob("boot", {
      title: "Stop watching the log?", text: "The game keeps running on the PS5. To quit the game, close SWRR on the PS5.", ok: "Stop watching" }));
    $("#btn-boot-stubs").addEventListener("click", (e) => { startJob(e.currentTarget, "/api/stubs", {}, "stubs"); go("help", null, "log"); });
    $("#chk-follow").addEventListener("change", (e) => { if (e.currentTarget.checked) $("#boot-log").scrollTop = 1e9; });

    // library
    $("#btn-lib-refresh").addEventListener("click", (e) => startJob(e.currentTarget, "/api/library/refresh", {}, "library"));
    $("#btn-manager-start").addEventListener("click", (e) => startJob(e.currentTarget, "/api/manager/start", {}, "manager"));
    $("#btn-manager-stop").addEventListener("click", (e) => startJob(e.currentTarget, "/api/manager/stop", {}, "manager"));
    $("#btn-manager-open").addEventListener("click", (e) => act(e.currentTarget, () => api("/api/open/manager", { url: S.manager.url })));

    // help
    $("#btn-runlog").addEventListener("click", loadRunlog);
    $("#btn-stubs").addEventListener("click", (e) => startJob(e.currentTarget, "/api/stubs", {}, "stubs"));
    $("#btn-open-build").addEventListener("click", (e) => act(e.currentTarget, () => api("/api/open/folder", { which: "build" })));
    $("#lnk-open-logs").addEventListener("click", (e) => { e.preventDefault(); act(null, () => api("/api/open/folder", { which: "logs" })); });
    $("#btn-open-app").addEventListener("click", (e) => act(e.currentTarget, () => api("/api/open/folder", { which: "app" })));
    $("#btn-quit").addEventListener("click", async (e) => {
      const ok = await confirmBox({ title: "Quit the app?", text: "Anything still running (an install, the game log) is stopped. A game already on the TV keeps running.", ok: "Quit", danger: true });
      if (!ok) return;
      quitting = true;
      try { await api("/api/quit", { force: true }); } catch (err) { /* it may close before answering */ }
      setOffline(true);
    });
    document.addEventListener("visibilitychange", () => { if (!document.hidden) refresh(); });
  }

  // ---------- start ----------
  readToken();
  if (!TOKEN) { $("#notoken").hidden = false; return; }
  wire();
  refresh().then(() => { pollLoop(); loaderLoop(); usbLoop(); });
})();
