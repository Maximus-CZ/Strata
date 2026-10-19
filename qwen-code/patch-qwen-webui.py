"""patch-qwen-webui.py - Strata's live status in Qwen Code's web UI bar (the user 2026-10-16).

The web UI's bar shows only a joke phrase, an arrow, a characters/4 count and the time; the user wants to see what the
model is doing. This adds a small script to Qwen Code's web-shell/index.html: while the bar is shown it fetches
/strata-status twice a second and draws the status ten times a second in place of the phrase - between fetches the
counters move on at the measured speed; fixed-width slots and equal-width digits, so nothing shifts. The status is
drawn by the page only: it is never part of the model's stream, so it counts in no token number.

/strata-status: NPM's host the public web UI host (id 37) proxies it, behind Authentik, to Strata's
GET /status/line (http://192.168.0.2:8081, no key: phases and numbers only). Opened on the LAN (:4170) the path is
not there and the bar stays as it was.

Also (2026-10-18): the choice between a conversation's uncompacted and compacted branch (qwen-compactor.py makes the
compacted one while the user is away), through the daemon route /lp-compact/* that this patches in.

And (2026-10-18): side tasks / branches / forks in every trusted workspace, not only the daemon's primary one.
And (2026-10-18, core): propose_goal always declared - a Goal starting or ending no longer changes the tool list
(which made Strata re-read the whole conversation).

Lives in C:/Strata/qwen-code (branch patched, beside the Strata patches); C:/Users/user/bin/patch-qwen-webui.py runs it.
A Qwen Code update replaces index.html and the daemon: run this again (idempotent), then restart the web UI.
python patch-qwen-webui.py [--remove]
"""
import os
import sys

INDEX = os.path.join(os.environ["APPDATA"], r"npm\node_modules\@qwen-code\qwen-code\web-shell\index.html")
BEGIN, END = "<!-- strata-status: begin (C:\\Users\\user\\bin\\patch-qwen-webui.py) -->", "<!-- strata-status: end -->"

# 2026-10-18: a conversation qwen-compactor.py compacted while the user was away. Opening it (the address is
# /session/<id>) shows two big buttons over the message box - uncompacted / compacted, each with a pie of the context
# it fills - and the box takes nothing until one is picked; the other branch is then thrown away (compacted: the page
# reloads the conversation from its new start). The page's own requests carry the daemon token; it is reused here.
COMPACT_UI = r"""
<style>
  /* sits exactly on the message box (locked meanwhile) - the conversation above stays fully visible */
  #lp-compact { --pc-bg: #fff; --pc-fg: #1b1b1f; --pc-btn: #f3f3f6; --pc-line: #d4d4dc; --pc-pie: #e2e2ea; --pc-acc: #2f7de1;
    position: fixed; z-index: 2147483000; box-sizing: border-box; padding: 6px 8px; border-radius: 12px;
    display: flex; flex-direction: column; justify-content: center; gap: 5px;
    background: var(--pc-bg); color: var(--pc-fg); border: 1px solid var(--pc-line); font: 13px/1.3 system-ui, sans-serif; }
  @media (prefers-color-scheme: dark) {
    #lp-compact { --pc-bg: #24242b; --pc-fg: #ececf1; --pc-btn: #2f2f38; --pc-line: #474752; --pc-pie: #3b3b46; --pc-acc: #5aa2ff; } }
  #lp-compact .pc-title { font-size: 12px; opacity: .8; margin: 0 2px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  #lp-compact .pc-row { display: flex; gap: 8px; }
  #lp-compact button.pc { flex: 1 1 0; min-width: 0; display: flex; align-items: center; gap: 10px; padding: 6px 10px;
    border: 2px solid var(--pc-line); border-radius: 9px; background: var(--pc-btn); color: inherit; font: inherit;
    text-align: left; cursor: pointer; }
  #lp-compact button.pc:hover:not(:disabled) { border-color: var(--pc-acc); }
  #lp-compact button.pc:disabled { opacity: .6; cursor: progress; }
  #lp-compact button.pc b { font-size: 15px; }
  #lp-compact button.pc span { min-width: 0; overflow: hidden; white-space: nowrap; text-overflow: ellipsis; }
  #lp-compact button.pc small { opacity: .75; }
  #lp-compact svg { flex: none; }
</style>
<script>
(() => {
  let auth = null, panel = null, input = null, prev = null, shownFor = null, note = null;
  const asked = {};
  const nativeFetch = window.fetch;
  // 2026-10-18: "model idle" while a shell command runs is true but says too little. The conversation's event
  // stream (the page's own /session/<id>/events) tells which tool runs since when: a copy of it is read here and the
  // running tools are kept in window.__lpTools for the status bar (toolCallId -> {sid, name, started}).
  const tools = window.__lpTools = new Map();
  const done = new Set();                              // finished tool calls (a transcript page may list them first)
  function onEvent(sid, d) {
    if (d.type === "pending_prompt_completed" || d.type === "pending_prompt_added") {   // a turn ends / begins
      for (const [id, x] of tools) if (x.sid === sid) tools.delete(id);
      return;
    }
    const u = (d.type === "session_update" || d.sessionUpdate) && ((d.data && d.data.update) || d.data || d);
    if (!u || (u.sessionUpdate !== "tool_call" && u.sessionUpdate !== "tool_call_update") || !u.toolCallId) return;
    const m = u._meta || {}, had = tools.get(u.toolCallId);
    if (u.status === "completed" || u.status === "failed") { tools.delete(u.toolCallId); done.add(u.toolCallId); return; }
    if (done.has(u.toolCallId)) return;
    const name = m.toolName && m.toolName !== "tool_call" ? m.toolName : (u.rawInput && u.rawInput.name) || m.toolName;
    tools.set(u.toolCallId, { sid, name: name || (had && had.name) || u.kind || "tool",
                              started: (had && had.started) || m.startedAt || m.timestamp || u.timestamp || Date.now() });
  }
  // after a page load the page reads the conversation back (GET /session/<id>/transcript) and subscribes to new
  // events only: a tool already running is found here - a tool_call with no completed/failed update
  function onTranscript(sid, d) {
    const evs = (d && d.events) || [];
    for (const e of evs) {                             // finished ones first, whatever the page's order
      const u = e && e.data && (e.data.update || e.data);
      if (u && (u.status === "completed" || u.status === "failed") && u.toolCallId) { done.add(u.toolCallId); tools.delete(u.toolCallId); }
    }
    for (const e of evs) {
      const u = e && e.data && (e.data.update || e.data);
      if (u && u.sessionUpdate === "tool_call" && Date.now() - (u.timestamp || (u._meta || {}).timestamp || 0) < 6 * 3600e3)
        onEvent(sid, { type: "session_update", data: { update: u } });
    }
  }
  async function readEvents(sid, body) {
    const rd = body.pipeThrough(new TextDecoderStream()).getReader();
    let buf = "";
    try {
      for (;;) {
        const { value, done } = await rd.read();
        if (done) break;
        buf += value;
        let i;
        while ((i = buf.indexOf("\n")) >= 0) {
          const line = buf.slice(0, i).replace(/\r$/, "");
          buf = buf.slice(i + 1);
          if (line.startsWith("data:")) try { onEvent(sid, JSON.parse(line.slice(5))); } catch (e) {}
        }
      }
    } catch (e) {}
  }
  window.fetch = function (r, o) {                    // the page's own requests carry the daemon token: keep it
    try {
      const h = (o && o.headers) || (r instanceof Request ? r.headers : null);
      const a = h && (h instanceof Headers ? h.get("Authorization") : h.Authorization || h.authorization);
      if (a) auth = a;
    } catch (e) {}
    const p = nativeFetch.apply(this, arguments);
    const url = typeof r === "string" ? r : (r && (r.url || r.href)) || "";
    const tr = url.match(/\/session\/([0-9a-f-]{36})\/transcript(\?|$)/);
    if (tr) return p.then(res => {
      if (res.ok) res.clone().json().then(d => onTranscript(tr[1], d)).catch(() => {});
      return res;
    });
    const ev = url.match(/\/session\/([0-9a-f-]{36})\/events(\?|$)/);
    if (!ev) return p;
    return p.then(res => {
      if (!res.ok || !res.body) return res;
      try {
        const [mine, theirs] = res.body.tee();
        readEvents(ev[1], mine);
        const out = new Response(theirs, { status: res.status, statusText: res.statusText, headers: res.headers });
        Object.defineProperty(out, "url", { value: res.url });
        return out;
      } catch (e) { return res; }
    });
  };
  const hdr = () => Object.assign({ "Content-Type": "application/json" }, auth ? { Authorization: auth } : {});
  const sessionId = () => { const m = location.pathname.match(/\/session\/([0-9a-f-]{36})\/?$/); return m ? m[1] : null; };
  const fmt = n => Math.round(n).toLocaleString("en-US");
  function composer() {                                // the lowest big text box on the page
    if (input && input.isConnected) return input;
    let best = null, low = -1;
    for (const el of document.querySelectorAll('textarea, [contenteditable="true"], [contenteditable=""]')) {
      if (panel && panel.contains(el)) continue;
      const r = el.getBoundingClientRect();
      if (r.width >= 120 && r.height >= 12 && r.bottom > low) { low = r.bottom; best = el; }
    }
    return best;
  }
  function lock(on) {
    const el = composer();
    if (on && el && el !== input) {
      lock(false);
      input = el;
      prev = el.tagName === "TEXTAREA" ? el.disabled : el.getAttribute("contenteditable");
      if (el.tagName === "TEXTAREA") el.disabled = true; else el.setAttribute("contenteditable", "false");
    } else if (!on && input) {
      if (input.tagName === "TEXTAREA") input.disabled = !!prev; else input.setAttribute("contenteditable", prev || "true");
      input = null;
    }
  }
  for (const ev of ["keydown", "beforeinput", "paste", "drop"])
    document.addEventListener(ev, e => {
      if (panel && input && input.contains(e.target)) { e.preventDefault(); e.stopPropagation(); }
    }, true);
  function pie(frac) {
    const f = Math.max(0, Math.min(1, frac || 0)), R = 15.9155;      // circumference 100
    const col = f > 0.8 ? "#e5484d" : f > 0.5 ? "#e8a33d" : "var(--pc-acc)";
    return '<svg viewBox="0 0 36 36" width="44" height="44" aria-hidden="true">' +
      `<circle cx="18" cy="18" r="${R}" fill="none" stroke="var(--pc-pie)" stroke-width="6.5"/>` +
      `<circle cx="18" cy="18" r="${R}" fill="none" stroke="${col}" stroke-width="6.5" stroke-dasharray="${(f * 100).toFixed(2)} 100" transform="rotate(-90 18 18)"/>` +
      `<text x="18" y="20.6" text-anchor="middle" font-size="7.5" font-weight="600" fill="currentColor">${Math.round(f * 100)}%</text></svg>`;
  }
  function close() {
    if (panel) panel.remove();
    panel = note = null;
    lock(false);
    if (shownFor) delete asked[shownFor];             // not picked: asked again when it is opened again
    shownFor = null;
  }
  function place() {
    if (!panel) return;
    lock(true);
    const el = composer();
    let r = el ? el.getBoundingClientRect() : null;
    for (let b = el, i = 0; b && b.parentElement && i < 4; i++) {   // the message box's card, not only its text area
      const p = b.parentElement.getBoundingClientRect();
      if (p.height > 280 || p.width > innerWidth) break;
      b = b.parentElement; r = p;
    }
    // exactly over the message box's card (it takes nothing meanwhile); only a card too low for the buttons makes
    // the panel reach up a little - bottom edges aligned
    const w = Math.min(innerWidth - 16, Math.max(300, r ? r.width : innerWidth - 32));
    const left = Math.max(8, Math.min(innerWidth - w - 8, r ? r.left : 16));
    panel.style.width = w + "px";
    panel.style.left = left + "px";
    panel.style.bottom = (r ? Math.max(0, innerHeight - r.bottom) : 16) + "px";
    panel.style.minHeight = (r ? Math.max(r.height, 84) : 84) + "px";
  }
  function open(sid, o) {
    close();
    shownFor = sid;
    const win = o.window || 262144, made = new Date(o.made * 1000);
    panel = document.createElement("div");
    panel.id = "lp-compact";
    panel.innerHTML =
      `<div class="pc-title" title="The other branch is thrown away. Compacted ${made.toLocaleString()} ` +
      `(took ${Math.round(o.took_s / 60)} min).">Compacted while you were away - continue with ` +
      `(the other one is thrown away):</div><div class="pc-row">` +
      `<button class="pc" data-c="uncompacted">${pie(o.uncompacted.tokens / win)}<span><b>Uncompacted</b><br>` +
      `<small>${fmt(o.uncompacted.tokens)} tokens, word for word</small></span></button>` +
      `<button class="pc" data-c="compacted">${pie(o.compacted.tokens / win)}<span><b>Compacted</b><br>` +
      `<small>~${fmt(o.compacted.tokens)} tokens, summary + latest turns</small></span></button></div>`;
    note = panel.querySelector(".pc-title");
    document.body.append(panel);
    const btns = [...panel.querySelectorAll("button.pc")];
    btns.forEach(b => b.addEventListener("click", () => choose(sid, b.dataset.c, btns)));
    place();
  }
  async function choose(sid, c, btns) {
    btns.forEach(b => b.disabled = true);
    note.textContent = c === "compacted" ? "Switching to the compacted branch…" : "Keeping it uncompacted…";
    try {
      const r = await nativeFetch("/lp-compact/choose", { method: "POST", headers: hdr(), body: JSON.stringify({ session: sid, choice: c }) });
      const d = await r.json().catch(() => ({}));
      if (r.status === 404 || r.status === 409) { shownFor = null; close(); asked[sid] = Date.now(); return; }
      if (!r.ok) throw new Error(d.error || "HTTP " + r.status);
      if (c === "compacted") { location.reload(); return; }
      shownFor = null; close(); asked[sid] = Date.now();
    } catch (e) {
      note.textContent = "That failed (" + e.message + "); nothing was changed - try again.";
      btns.forEach(b => b.disabled = false);
    }
  }
  async function check() {
    const sid = sessionId();
    if (shownFor && sid !== shownFor) close();
    if (!sid || shownFor || Date.now() - (asked[sid] || 0) < 30000) return;
    asked[sid] = Date.now();
    try {
      const r = await nativeFetch("/lp-compact/offer?session=" + sid, { headers: hdr(), cache: "no-store" });
      const o = r.ok ? await r.json() : null;
      if (o && o.offer && sessionId() === sid) open(sid, o);
    } catch (e) {}
  }
  setInterval(check, 500);
  setInterval(place, 250);
  // 2026-10-18: Windows idle-slept the PC (19:59) while he used this page remotely - no input at the PC itself.
  // While the page is visible and was touched in the last 10 min it says so once a minute; the compactor then keeps
  // the PC awake 5 min more. A tab left open (hidden, or untouched) says nothing, so the PC still sleeps.
  let touched = Date.now(), told = 0;
  function awake() {
    if (document.visibilityState !== "visible" || Date.now() - touched > 600000) return;
    told = Date.now();
    nativeFetch("/lp-compact/awake", { method: "POST", headers: hdr(), body: "{}" }).catch(() => {});
  }
  for (const ev of ["pointerdown", "keydown", "wheel", "touchstart"])
    addEventListener(ev, () => { touched = Date.now(); if (touched - told > 60000) awake(); }, { capture: true, passive: true });
  setInterval(awake, 60000);
})();
</script>
"""

SCRIPT = BEGIN + r"""
<style>
  span[data-strata] { font-variant-numeric: tabular-nums; white-space: nowrap; margin-right: 0.6em; }
  span[data-strata] > i { font-style: normal; display: inline-block; overflow: hidden; vertical-align: bottom; }
  span[data-strata] > i.n { text-align: right; }
</style>
<script>
(() => {
  let st = null, got = 0;
  const fmt = n => Math.round(n).toLocaleString("en-US");
  async function poll() {
    try {
      const r = await fetch("/strata-status", { cache: "no-store" });
      st = r.ok ? await r.json() : null;
    } catch (e) { st = null; }
    got = performance.now();
  }
  // slot widths in ch: phase, count, speed, extra, time, queue
  const W = [17, 17, 10, 11, 7, 18];
  function slots(el) {
    if (el.children.length === W.length) return el.children;
    el.textContent = "";
    W.forEach((w, i) => {
      const x = document.createElement("i");
      x.style.width = w + "ch";
      if (i) x.className = "n";
      el.appendChild(x);
    });
    return el.children;
  }
  // the bar: the meta span that says "(... esc to cancel)" or "Press Esc again to stop", and its parent - many other
  // elements of the page have classes starting with _status_ / _label_ / _meta_ (tool cards, badges)
  function findBar() {
    for (const m of document.querySelectorAll('span[class^="_meta_"]'))
      if (/esc/i.test(m.textContent) && m.parentElement && m.parentElement.querySelector('[class^="_spinner_"]'))
        return m.parentElement;
    return null;
  }
  function draw() {
    const bar = findBar();
    if (!bar) return;
    let mine = bar.querySelector("span[data-strata]");
    const labels = [...bar.children].filter(e => /^_label_/.test(e.className) && !e.dataset.strata);
    if (!st) {                                        // no status (opened on the LAN, or Strata unreachable)
      if (mine) mine.remove();
      labels.forEach(e => e.style.display = "");
      return;
    }
    if (!mine) {
      mine = document.createElement("span");
      mine.dataset.strata = "1";
      if (labels[0]) mine.className = labels[0].className;
      bar.insertBefore(mine, [...bar.children].find(e => /^_meta_/.test(e.className)) || null);
    }
    labels.forEach(e => e.style.display = "none");
    const dt = (performance.now() - got) / 1000, x = slots(mine), t = ["", "", "", "", "", ""];
    const pre = st.kind ? "compress: " : "";
    if (st.phase === "encoding pictures") {             // the request's pictures, before its prompt is read
      t[0] = "pictures";
      t[1] = st.pictures[0] + " / " + st.pictures[1];
    } else if (!st.busy) {                             // the model does nothing: Qwen runs a tool, or waits
      // which tool, since when (the compaction script reads them from the conversation's event stream)
      const sm = location.pathname.match(/\/session\/([0-9a-f-]{36})\/?$/);
      const run = [...(window.__lpTools || new Map()).values()]
        .filter(x => sm && x.sid === sm[1] && Date.now() - x.started < 6 * 3600e3).sort((a, b) => a.started - b.started);
      if (run.length) {
        const nm = { run_shell_command: "shell", web_fetch: "web fetch", web_search: "web search", agent: "subagent",
                     read_file: "reading a file", write_file: "writing a file", edit: "editing", grep_search: "searching",
                     glob: "listing files" }[run[0].name] || run[0].name;
        const s = Math.max(0, Math.floor((Date.now() - run[0].started) / 1000));
        t[0] = "waiting: " + nm;
        t[1] = (s >= 3600 ? Math.floor(s / 3600) + ":" + String(Math.floor(s / 60) % 60).padStart(2, "0")
                          : Math.floor(s / 60)) + ":" + String(s % 60).padStart(2, "0");
        t[2] = run.length > 1 ? "+" + (run.length - 1) + " more" : "";
        t[3] = t[4] = "";
      } else {
        t[0] = "model idle"; t[1] = t[2] = t[3] = t[4] = "";
      }
    } else if (st.generated == null) {                 // reading the prompt
      const read = st.read == null ? null
        : Math.min(st.prompt, st.read + (st.read_rate || 0) * ((st.read_age || 0) + dt));
      t[0] = pre + "reading";
      t[1] = read == null ? fmt(st.prompt) + " tok" : fmt(read) + " / " + fmt(st.prompt);
      t[2] = st.read_rate ? fmt(st.read_rate) + " t/s" : "";
      t[3] = st.read_rate && read != null ? "~" + fmt((st.prompt - read) / st.read_rate) + " s left" : "";
    } else {                                           // thinking / answering / a tool call
      const ph = (st.phase || "").startsWith("writing a tool call") ? "tool call" : st.phase;
      t[0] = pre + ph;
      t[1] = fmt(st.generated + (st.gen_rate || 0) * dt) + " tok";
      t[2] = st.gen_rate ? fmt(st.gen_rate) + " t/s" : "";
      t[3] = "of " + fmt(st.prompt);
    }
    if (st.busy) t[4] = fmt(st.elapsed + dt) + " s";
    // a miss, or why the last big read happened (a minute each); new content needs no note
    const why = st.reread && st.reread.why || "";
    const short = why.startsWith("system prompt") ? "↻ new sys prompt" : why.startsWith("compressing") ? "↻ compression"
      : why.startsWith("first request") ? "↻ after restart" : "";
    t[5] = st.miss ? "⚠ cache miss" : st.queued ? st.queued + " waiting" : short;
    for (let i = 0; i < W.length; i++) if (x[i].textContent !== t[i]) x[i].textContent = t[i];
  }
  setInterval(() => { if (findBar()) poll(); else st = null; }, 500);
  setInterval(draw, 100);
})();
</script>
""" + COMPACT_UI + END + "\n"


# 2026-10-17: adding a workspace from the remote web UI failed for a folder that does not exist yet ("Path does
# not exist or is not accessible") - at the PC the Windows folder picker can create it. The daemon now creates it when
# its parent exists and it is under C:\Users\user (one level only: a typo deep in a path creates nothing).
DAEMON_DIR = os.path.join(os.environ["APPDATA"], r"npm\node_modules\@qwen-code\qwen-code\chunks")
D_OLD = 'canonical=realpathSync3.native(resolve7(sandboxCwd))}catch{res.status(400).json({error:"Path does not exist or is not accessible",code:"invalid_path"});return}'
D_NEW = ('canonical=realpathSync3.native(resolve7(sandboxCwd))}catch{/*lp-mkdir*/try{const p=resolve7(sandboxCwd);'
         r'if(/^[a-z]:\\users\\user\\[^\\]/i.test(p)&&existsSync4(resolve7(p,".."))&&!existsSync4(p)){mkdirSync2(p);'
         'canonical=realpathSync3.native(p)}}catch{}if(!canonical){res.status(400).json({error:"Path does not exist or is '
         'not accessible",code:"invalid_path"});return}}')


# 2026-10-18: compacted branches (qwen-compactor.py on 127.0.0.1:8085). The page asks /lp-compact/offer and
# /lp-compact/choose; the daemon passes them on after its own token check, so they work on the LAN and remotely alike.
C_OLD = 'healthRoutes.register(app)}installJsonBodyParser(app);const hostedHarness='
C_NEW = ('healthRoutes.register(app)}installJsonBodyParser(app);/*lp-compact*/app.use("/lp-compact",async(req,res)=>{'
         'try{const r=await globalThis.fetch("http://127.0.0.1:8085"+req.url,{method:req.method,headers:{"Content-Type":'
         '"application/json"},body:req.method==="POST"?JSON.stringify(req.body??{}):void 0,signal:AbortSignal.timeout(6e4)});'
         'res.status(r.status).type("application/json").send(await r.text())}catch(e){res.status(502).json({error:'
         '"the compactor (qwen-compactor.py) is not running"})}});const hostedHarness=')
# 2026-10-18: "Side task" in another workspace failed ('Route "POST /session/:id/side-task" is only available for
# primary workspace sessions'): side-task / branch / fork were allowed only in the daemon's primary workspace (Desktop)
# and internal ones. every workspace of his is as trusted as Desktop - a trusted registered workspace may use
# them too (untrusted ones are refused before this); they run on that workspace's own runtime either way. `cd` stays
# primary-only.
S_OLD = 'if(!runtime.primary&&(primaryOnly||!isInternalWorkspaceRuntime(runtime))){'
S_NEW = 'if(!runtime.primary&&(primaryOnly||!(isInternalWorkspaceRuntime(runtime)||runtime.trusted/*lp-side-task*/))){'
DAEMON_PATCHES = [("create a new workspace folder", D_OLD, D_NEW), ("compacted branches", C_OLD, C_NEW),
                  ("side tasks in every trusted workspace", S_OLD, S_NEW)]


def patch_daemon(remove=False):
    for name in os.listdir(DAEMON_DIR):
        if not (name.startswith("server-") and name.endswith(".js")):
            continue
        path = os.path.join(DAEMON_DIR, name)
        with open(path, encoding="utf-8") as f:
            js = f.read()
        if D_OLD not in js and D_NEW not in js:
            continue
        done = []
        for what, old, new in DAEMON_PATCHES:
            if remove and new in js:
                js = js.replace(new, old)
            elif not remove and old in js:
                js = js.replace(old, new)
            elif (new if not remove else old) not in js:
                print(f"daemon: {what}: the code moved (check by hand)")
                continue
            else:
                continue
            done.append(what)
        if done:
            with open(path, "w", encoding="utf-8", newline="") as f:
                f.write(js)
            print(("daemon unpatched (" if remove else "daemon patched (") + ", ".join(done) + "): " + path
                  + " - restart qwen serve (tray: Qwen Code > Restart web UI)")
        else:
            print("daemon: " + ("already patched" if not remove else "nothing to remove"))
        return bool(done)
    print("daemon: no server-*.js with the known code (check by hand)")
    return False


# Patches of Qwen Code's core (the session processes, `cli.js --acp`; a chunk found by its code, as names change).
# 2026-10-18: a side task re-read 224K tokens because its tool list changed - propose_goal is declared to the model
# only while the turn may propose a Goal, so it vanished when the Goal started (20:11, 20 -> 19 tools) and came back
# when it ended (21:00, 19 -> 20); the tool definitions sit at the start of the prompt, so either way Strata had to read
# the whole conversation again. Now it is declared always (when registered at all - goals.modelProposed "disabled"
# still removes it): a call from a turn that may not propose is refused by Qwen Code itself ("The Goal was not set:
# this turn cannot own a Goal approval ...").
CORE_PATCHES = [("propose_goal always declared (a stable tool list for the prompt cache)",
                 'return name!==ToolNames.PROPOSE_GOAL||this.config.isGoalProposalAvailable()}',
                 'return true/*lp-stable-tools: name!==ToolNames.PROPOSE_GOAL||this.config.isGoalProposalAvailable()*/}')]


def patch_core(remove=False):
    for what, old, new in CORE_PATCHES:
        hit = False
        for name in os.listdir(DAEMON_DIR):
            if not name.endswith(".js") or name.startswith("server-"):
                continue
            path = os.path.join(DAEMON_DIR, name)
            with open(path, encoding="utf-8") as f:
                js = f.read()
            a, b = (new, old) if remove else (old, new)
            if b in js and a not in js:
                print(f"core: {what}: already " + ("removed" if remove else "patched"))
                hit = True
                break
            if a in js:
                with open(path, "w", encoding="utf-8", newline="") as f:
                    f.write(js.replace(a, b))
                print(f"core {'unpatched' if remove else 'patched'} ({what}): {path} - new session processes load it "
                      "(restart qwen serve when Qwen is idle)")
                hit = True
                break
        if not hit:
            print(f"core: {what}: the code moved (check by hand)")


def main():
    patch_daemon("--remove" in sys.argv)
    patch_core("--remove" in sys.argv)
    with open(INDEX, encoding="utf-8") as f:
        html = f.read()
    if BEGIN in html:
        html = html[:html.index(BEGIN)] + html[html.index(END) + len(END):].lstrip("\n")
    if "--remove" not in sys.argv:
        i = html.rindex("</body>")
        html = html[:i] + SCRIPT + html[i:]
    with open(INDEX, "w", encoding="utf-8", newline="") as f:
        f.write(html)
    print(("removed from " if "--remove" in sys.argv else "patched ") + INDEX)


if __name__ == "__main__":
    main()
