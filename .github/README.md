# Changes to upstream main

Oldest first. **commit**: what it does. *Why:* reason. `keywords`.

**Base (upstream PR #960)**

1. **4339695**: the prompt prefix is saved to disk, so a new chat restores its system prompt instead of reading it
   again. *Why:* a new chat starts in seconds instead of re-reading 30K+ tokens. `prefix-cache, disk, PR#960`

**Engine: cache, end points, VRAM**

2. **98a5d08**: `--prompt-cache 1` works together with `--prefix-cache-dir`. *Why:* the two options couldn't be
   combined. `prompt-cache, flags, compat`
3. **fb86c51**: the vision encoder loads only when a picture arrives, borrowing VRAM from the expert cache. *Why:* image
   support without permanently losing 10 GB card space. `vision, lazy-load, VRAM`
4. **c08cbe0**: each conversation's state ("end point") is saved to disk, pictures are keyed by content, and the vision
   encoder stays warm. *Why:* resuming a conversation after a restart or switch doesn't re-read it.
   `end-points, snapshots, picture-hash`
5. **ba7353a**: a saved state is stored only as the part beyond an older saved state of the same conversation (a
   "delta"). *Why:* stops full multi-GB copies per turn. `delta, disk-space, STRPFX03`
6. **fb38554**: if the closest base doesn't match byte for byte, the next-closest base is tried. *Why:* a mismatched
   base no longer forces a full read. `delta, fallback, robustness`
7. **50c5090**: first turns and tails under 2048 tokens get no file, and a delta's base must be a clean prefix.
   *Why:* fewer pointless small writes, and no broken bases. `thresholds, SSD-wear, correctness`
8. **0f54d0d**: the vision encoder's VRAM baseline is measured only from the loaded engine and reset on every start.
   *Why:* a stale baseline mis-sized the expert cache. `VRAM, baseline, vision`
9. **58dd19d**: VRAM is regrown until the expert cache is full, picture encoding shows as a status, and every big read
   is logged with its reason. *Why:* get speed back after vision use, and make slow reads explainable.
   `VRAM-regrow, expert-cache, logging`
10. **c4d44ee**: the VRAM regrow is retried before a request while the expert cache is short. *Why:* the first regrow
    often failed while memory was still held. `regrow, retry, VRAM`
11. **d71edd6**: the retry grows straight to the floor instead of shrinking first. *Why:* it shrank to a stale figure
    and lost cache. `regrow, bugfix`
12. **e199e53**: the regrow floor is lowered from 384 to 256 MiB. *Why:* fits more experts on the 10 GB card.
    `VRAM, tuning`
13. **2ffd5da**: the prefix store evicts the largest family first, and encoded pictures survive restarts. *Why:* keeps
    many small conversations instead of one huge one, and pictures aren't re-encoded.
    `eviction, prefix-store, pictures`
14. **edeb50c**: a request flag (`endpoint=0`) skips saving to disk, and the config's `endpoint_clients` decides which
    clients save. *Why:* background jobs (SubSync, compactions) shouldn't wear the SSD.
    `endpoint=0, per-client, SSD-wear`
15. **f471a7f** *(superseded by 17)*: states were written only after 16K tokens of growth or on a flush request.
    *Why:* two agents taking turns were writing about 750 MB per turn. `STRATA_ENDPOINT_GAP, SSD-wear`
16. **a6389d6** *(superseded by 17)*: writing was deferred only while the outgoing state stays in RAM. *Why:* two big
    conversations evicted each other and re-read their gaps. `deferral, RAM, regression-fix`
17. **dd2a9ed**: a conversation's state is written to disk only when RAM evicts it, on a flush, or when it isn't kept
    in RAM, not on every switch. *Why:* "save on RAM eviction", which cuts SSD wear without extra re-reads.
    `on_evict, capture_image, SSD-wear`

**Server: status, diagnostics, control**

18. **222defd**: live status while a request waits for the model or its prompt is being read. *Why:* the UI showed
    nothing during long reads. `status, UX`
19. **d1349b8**: `/status/line` works without the key and shows prompt-read progress, with no status lines leaking
    into the thinking text. *Why:* a status bar for the tray and web UI, with clean model output.
    `status-line, progress`
20. **8393206**: `/status/line` returns its numbers as separate fields. *Why:* a steady display that doesn't parse
    text. `API, status-fields`
21. **56d058a**: a request that reuses 2048+ fewer tokens than one cached before is logged. *Why:* cache misses become
    visible and debuggable. `cache-miss, diagnostics`
22. **0532746**: a changed system prompt or tool list is named as the cause before "cache miss". *Why:* the cause shows
    instead of just the symptom. `cache-miss, root-cause`
23. **4a8b3d3**: `POST /strata/cancel {client}` cancels that client's running and queued requests. *Why:* SubSync can
    be stopped at once when Qwen needs the model. `cancel, priority, SubSync`
24. **b96671b**: a compression requested in the last user message is shown as "compressing", not "answering".
    *Why:* the status was wrong during compaction. `compaction, status, request_kind`

**Qwen Code web UI** (`qwen-code/patch-qwen-webui.py`)

25. **1f3945e**: local Qwen patches tracked in the repo, and side tasks, branches and forks allowed in every trusted
    workspace. *Why:* reproducible patches, and side tasks beyond one folder. `patch-tracking, side-tasks, trusted`
26. **8b99be0**: the page reports "in use" once a minute while visible and touched. *Why:* the PC was sleeping during
    remote use. `keep-awake, compactor`
27. **a2b5297**: the status bar names the tool Qwen is waiting for and for how long. *Why:* "model idle" was
    misleading. `status, tool-wait, timer`
28. **5d921a9**: tools still running are also found in the transcript read after a page reload. *Why:* the waiting
    status was lost on reload. `reload, tool-tracking`
29. **0f0e7ed**: `propose_goal` is always declared. *Why:* a Goal starting or ending changed the tool list, which
    invalidated Strata's cache. `stable-tools, cache`
30. **7a6f8a9**: the compacted/uncompacted choice sits on the message box only. *Why:* it covered the end of the
    agent's reply. `compaction-UI, layout`
31. **5ee4717**: the chosen branch opens in place, without a page reload, and Qwen's context pie updates at once.
    *Why:* smoother switching with a correct context number. `no-reload, context-pie`
32. **533cf3d**: a banner appears when the Authentik sign-in has expired. *Why:* requests otherwise failed as an
    unexplained NetworkError. `auth, banner`
33. **5d73dbb**: the mkdir patch was silently dead in Qwen 0.25.0 and is fixed; projects can be created at any depth
    under `C:\ai_cache` on the local SSD. *Why:* creating a project folder from the UI didn't work.
    `mkdir, bugfix, ai_cache`
34. **70f00b5**: the mkdir scope is limited to `C:\ai_cache`. *Why:* the whole home folder was too wide a boundary.
    `scope, safety`
35. **1dca3a1**: the Add Workspace panel opens at `C:\ai_cache\projects`. *Why:* it opened at all of `C:\Users\user`,
    which doesn't match the mkdir scope. `workspace-picker, UX`
36. **fa54526**: the web shell's files are revalidated on each load instead of cached as immutable for a year.
    *Why:* a patched file now takes effect on a plain reload. `cache-headers, deploy`
37. **e9e28cd**: the service worker fetches from the network first, with the cache only as offline fallback, and the
    cache name is bumped. *Why:* stale patched files survived even a hard reload. `service-worker, cache-busting`

The list has 37 entries because entry 1 is the upstream PR the branch is built on and isn't counted among the 36.
When you add a patch, add its line here too.

## The patches in detail (the first ones; backups in `patch-backups\*.patch`)

1. **Upstream PR #960** (konijiwa110, open, not merged as of 2026-10-05): `--prefix-cache-dir DIR`.
   The state at the end of a request's system message (system text + tool definitions, the "root") is saved
   once per distinct system message to a file and restored by any later prompt that starts with it
   (~75 ms from disk for a 3.5K-token root; ~170 MB per file). Survives interleaving and engine restarts.
   Files are bound to the engine build: every rebuild deletes them at start-up (they are recreated on first use).
2. **local patch** (commit "serve: --prompt-cache 1 works with --prefix-cache-dir"): with `--prompt-cache 1`
   upstream dropped the root while the prompt was still being read, so #960 never saved it. Now the root is
   kept beside the newest point for the request, saved after DONE, then the chain goes back to one point.
   Result: every parked conversation holds only its tail checkpoint (113 MiB less each), system prompts come
   from disk. Worth proposing upstream on PR #960.

3. **local lazy GPU vision encoder** (commit "serve: a lazy GPU vision encoder ...", `serve/server.py` only, no
   rebuild): config `"vision": {"gpu": true, "lazy": true, "vram_mib": 3072, "max_tokens": 16384, ...}` +
   `"vram_elastic": true` + args `--vision --vram-segment-mib 64`. The encoder is NOT resident: for a picture not
   cached yet the engine's elastic expert cache gives VRAM back (shrinks to its floor, ~2.1 GB free), strata-vision
   starts on the GPU (5 s), encodes at full resolution (BF16 mmproj, up to 16,384 tokens = no downscaling of normal
   screenshots), stops, and the engine regrows to the free VRAM measured at the first picture less 128 MiB.
   Measured 2026-10-07: +5 s per request with a new picture; cache back to 97 % (89 % after a 3,600-token picture:
   the engine keeps ~150 MiB of image buffers). Same picture again = cached, no encoder start.
   Encoder file: `C:\Strata-data\models\mmproj-Qwen3.8-Flash-Next-BF16.gguf` (0.9 GB, approved the download).
   Qwen Code knows the model takes images: `"modalities": {"image": true}` in `~/.qwen/settings.json`.

4. **Conversation end points on disk + pictures keyed by content** (commit 6a0df09, engine): when a conversation
   leaves the GPU session (park), its deepest checkpoint ("end point") is saved to the prefix store too, if it is
   >= 1024 tokens past the system prompt; the older end points of that conversation are dropped (the root stays).
   So N parallel Qwen subagents no longer thrash the 4 RAM slots: an evicted one comes back from disk
   ("restored 51251 tokens (disk) in 187 ms"). Prefix files key every picture by start + content hash (not size), so
   two same-size pictures never share a cache line (red/blue test passes). The log says why an end point was not
   saved ("end point not saved: ..."). Vision `idle_s` (default 60) keeps the encoder warm; the live config uses 0
   (a warm encoder shrank the expert cache, prefill fell to 173 tok/s).
5. **Delta end points** (commits 2a1dcee + 2dd50e3, engine, file format STRPFX03; 2026-10-15): an end point is saved as
   only the bytes past an older snapshot of the same tokens (usually that conversation's previous end point, else
   the system-prompt root) plus a link to it; restore reads the chain ("restored N tokens (disk, 3 files)"). Before
   linking, sampled bytes of every K/V payload are compared with the session - the same tokens computed in another
   session differ in the last bits and are saved whole instead (the next-closest base is tried, up to 3).
   Chains over 24 are saved whole again; a base is kept while anything is stored on it; deleting a file deletes
   what is stored on it. Purpose: SSD wear and save time - a turn that adds 3K tokens writes ~100 MB, not 1-2 GB.
   Tested: `prefix_store_test` (399 checks; configure with -DSTRATA_BUILD_TESTS=ON -DSTRATA_BUILD_CONVERSATION_TESTS=ON).

6. **Which end points get a file** (commit 0b03feb, 2026-10-16): only a conversation that already came back once (>= 4
   turns) and is 2048+ tokens past its system prompt. First turns are mostly one-offs (SubSync verifier and
   whole-file questions, Qwen's web_fetch summaries) - live, 13 whole-file questions wrote 855 MB each in 3 min and
   flushed the 20 GB store. Delta bases that are wholly a prefix of the new snapshot (root, own end points) are tried
   before siblings (siblings share a few more tokens but other sessions' bytes).

7. **Live status** (commits 725625c + cb307d9, serve only): GET `/status/line` (no key - phases and numbers, never
   conversation text) and `/status` fields kind/read/read_tok_s/read_left_s. Shown in Qwen Code's web UI bar via
   NPM `the public web UI host/strata-status` and `qwen-code\patch-qwen-webui.py` (below).

## Qwen Code patches (`qwen-code\patch-qwen-webui.py`, since 2026-10-18 tracked here)

Qwen Code (npm `@qwen-code/qwen-code`, its `qwen serve` web UI on :4170) is patched in place; **every Qwen Code
update overwrites the patches: run `python C:\Users\user\bin\patch-qwen-webui.py` again (a wrapper for this file,
idempotent, `--remove` undoes), then restart the web UI (tray: Qwen Code > Restart web UI) when Qwen is idle.**
A patch whose code moved upstream is reported ("the code moved (check by hand)"), never applied blindly.
- `web-shell/index.html`: Strata's live status in the bar; the uncompacted / compacted choice (two buttons with
  context pies, the message box locked until one is picked) for branches made by `C:\Users\user\bin\qwen-compactor.py`.
- daemon `chunks/server-*.js`:
  1. a missing workspace folder is created (one level, under `C:\Users\user`) - remote "Add workspace";
  2. route `/lp-compact/*` (behind the daemon's token) -> the compactor on 127.0.0.1:8085;
  3. side tasks / branches / forks in every trusted workspace, not only the daemon's primary one (Desktop) - local
     workspaces are all as trusted as Desktop; `cd` stays primary-only.

Development worktree: `C:\Strata-build` (branch `spill`, = patched), build script `build-delta.bat` there (engine +
test into `C:\Strata-build\build`); engines tried live are copied to `C:\Strata-build\engine-*`.
The Qwen proxy can hold requests while Strata restarts: create `C:\Users\user\bin\proxy.hold`, wait for
`"upstream": 0` in `coder.json`, restart (flag file `C:\Users\user\bin\tray.restart-model`), delete `proxy.hold` -
nothing is cut.

## Live config (`strata-iq3_xxs.json`, started by the tray `C:\Users\user\bin\tray.ps1` on :8081)

Extra engine args: `--conversation-cache-mib 4096 --conversation-cache-slots 4 --prompt-cache 1
--prefix-cache-dir C:\Strata-data\prefix-cache --prefix-cache-ram-mib 0 --prefix-cache-disk-mib 20480`
(plus `--max-context 262144 --kv-resident 32768`). UPDATE-PATCHED.ps1 re-adds them if setup drops them.

Measured A/B (8 real subsync requests): release 31.9 s, #960 29.9 s, #960 + patch 29.9 s with every parked
conversation 113 MiB smaller. Details: Claude memory `strata-local-llm.md` (Desktop project).

## Updating: `UPDATE-PATCHED.bat`, NOT `UPDATE.bat`

`UPDATE.bat` does `git pull --ff-only`, which refuses on a branch with our own commits. `UPDATE-PATCHED.bat`:
refuses while Strata is busy (`-Force` overrides) -> `git fetch` + `git rebase origin/main` (a conflict aborts and
changes nothing) -> stops the tray + model -> `START-HERE.bat --update` (Python packages, configs, and a recompile
of engine + vision because the engine is "local"; minutes) -> if the compile failed, `git reset --keep` back to the
old source so server and engine match -> re-checks our config args -> restarts the tray through Explorer and loads.
Log: `update-patched.log`. If upstream merges #960, the rebase drops our copy of it by itself.

## When an update conflicts (exit code 2)

Upstream changed lines our patches touch (mostly `src/program/generate.cpp`). Fix by hand:
`git rebase origin/main`, resolve, keep the behaviour described above, `git rebase --continue`, then run
UPDATE-PATCHED.bat again. Rebuild by hand: `STRATA_NVCC=<CUDA 13.1 nvcc>` then
`.venv\Scripts\python -c "import setup as s; s.build_engine(s.gpu_info(),'gpu',True,s.get_llama_cpp())"`
(Strata must be stopped: Windows locks a running strata.exe). CUDA 13.0's folder has no nvcc on this PC.

## Going back to the release engine

`engine.release-0.1.39\` is the untouched ready-made engine. Copy it over `engine\` (model stopped), then
`git switch main` and use UPDATE.bat again; remove the `--prefix-cache-*` args and set `--prompt-cache 2`.

## Other local (untracked, excluded in .git/info/exclude) things

`bench-ctx\` (configs + bench scripts: strata-960.json / strata-960p.json point at `C:\Strata-build\engine-960*`,
a separate worktree used for the first builds), `lan-api-key.txt`, `strata-*.json.bak-*`.
