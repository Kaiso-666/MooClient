# MooMoo Desktop Client — plan

Status: planning only. Nothing built yet.
Date: 2026-10-28
Target: Windows 10 1809+ / Windows 11, x64 (+ arm64 optional).
Distribution: public installer, per-user, no admin, no services, no drivers.

A native Windows shell around the real moomoo.io client: no browser chrome, instant
launch, persistent accounts, a local asset cache, a local database, and a hard rule
that only `moomoo.io` may ever render inside the app window. Everything else opens
in the user's default browser after a confirmation prompt.

---

## 0. Decision

**WebView2 host in C++20 (Win32), with a zero-overhead present path.**

"Most performant possible" has a hard ceiling: the game is Chromium + Canvas2D, and
any client that runs the real game runs that code. The host can only remove overhead
and guarantee the best scheduling/GPU path. That is what this design does, and it is
verifiable with PresentMon (§4.6) rather than asserted.

A native reimplementation would be faster in theory and is not viable in practice:

- the WebSocket protocol is sealed — obfuscated JS plus a WASM `mixKey` stream cipher
  (`/p/s16pcq.js`), no plain msgpack anymore;
- login goes through Cloudflare Turnstile; you cannot automate it or reimplement it;
- the API is FRVR-token/ticket based, undocumented and server-validated;
- the page counts untrusted events and reports them to the server;
- it would break on every game update and would need its own renderer, netcode,
  anti-cheat arms race, and months of work.

So the fastest *shippable, maintainable* architecture is: own the shell, own the
cache, own the data, and never touch a frame on the way to the screen.

Rejected: Electron/Tauri (heavy, slower start), CEF (200 MB+, no capability we need),
native WS client (above), Edge `--app=` wrapper (no cache control, no vault, no DB,
no navigation policy).

---

## 1. Verified facts about the game (live, 2026-10-28)

Re-check before building; the plan depends on these.

- Client **v1.9.4**, FRVR rewrite. Vite bundle `assets/index-993d072d.js` + `vendor-*`.
- Entry HTML `Cache-Control: no-store`; hashed assets `immutable, max-age=31536000`.
- Import map: `moomoo-protocol → /p/s16pcq.js`, `moomoo-seal → /p/e1ddf9ae….js`.
- **Sealed netcode**: `/p/s16pcq.js` builds WASM and XORs WebSocket frames with a
  `mixKey` keystream. No plain protocol to implement.
- **REST** `https://api.moomoo.io`: `/servers`, `/join`, `/profile`, `/name-check`,
  `/clan`, `/clan/mine`, `/top`, `/friends/*`, `/page/ticket`, `/discord/link`.
  Auth = FRVR SDK access tokens + server tickets; no public API.
- **Login**: email → 6-digit code, or password; signups use Cloudflare Turnstile.
  The real login UI must run in the embedded browser.
- **Event trust**: handlers are wrapped; untrusted event counts are sent to the server
  in the `"T"` packet. Synthetic input must be real OS input (`SendInput`), never CDP.
- No service worker, no IndexedDB, no Cache API. Session = cookies + `localStorage`.
- Server list endpoint 520s for plain HTTP clients; use the page context, not native.
- WebView2 runtime is **not installed on this machine** (Edge 156 is separate).

---

## 2. Architecture

```
┌──────────────────────────────────────────────────────────────────────┐
│ MooClient.exe  (C++20, Win32/COM, static CRT, single exe)            │
│                                                                      │
│  shell/      window, DPI, fullscreen, single instance, crash dump     │
│  webview/    env + controller, flags, nav policy, preload, bridge     │
│  cache/      L2 content-addressed store, capture/serve, LRU, builds  │
│  store/      SQLite, migrations, DAOs, DPAPI vault                   │
│  accounts/   login capture, session snapshot, switch, health check    │
│  ui/         Control Center (local https://app.local, WebMessage)     │
│  diag/       logs, metrics, minidumps, perf marks, PresentMon hooks   │
└───────────────┬──────────────────────────────────────────────────────┘
                │ CoreWebView2 (HWND controller — real window messages)
┌───────────────▼──────────────────────────────────────────────────────┐
│ Real moomoo.io v1.9.x (unmodified HTML/JS/WASM)                      │
│  assets served from L2 cache → no refetch, no network on relaunch     │
│  API + Turnstile + WebSocket exactly as in a browser                 │
└──────────────────────────────────────────────────────────────────────┘
```

Invariants:

1. **Never modify game code.** No response rewriting, no DOM surgery, no traffic MITM.
   Additive preload scripts only, opt-in.
2. **All game/API work happens in the page.** Native reads results; keeps FRVR tokens,
   Turnstile and server tickets valid by construction.
3. **One process, one active session.** Account switching is explicit.
4. **The host never paints, captures, or overlays the game surface.** See §4.2.
5. **No telemetry.** Ever.

---

## 3. Components

### 3.1 Shell

- Borderless window, custom title bar, per-monitor DPI v2, `asInvoker` manifest.
- Splash is a separate window, destroyed before the game is shown; never topmost over it.
- `Alt+Enter` / `F11` fullscreen; `Esc` exits fullscreen only when pointer lock is off.
- Single instance: named mutex + `WM_COPYDATA`; second launch focuses the first, and
  `--account <id>` requests a switch instead of opening a second game.
- Keep-awake during play (`SetThreadExecutionState`), opt-out.
- Crash handling: unhandled filter + `MiniDumpWriteDump`; ring-buffered log flushed on
  crash; WebView2 `ProcessFailed` and `BrowserProcessExited` recovery (recreate the
  controller, return to menu, never a dead window).
- `--portable` uses `.\data\` next to the exe.

### 3.2 WebView2 layer

- Evergreen runtime; `Microsoft.Web.WebView2` SDK, `WebView2LoaderStatic.lib` so the
  app ships as one exe with no loader DLL.
- Environment created **before** the window is shown, in parallel with splash drawing;
  navigate the moment the controller exists.
- Profile model: one shared user data folder + encrypted cookie/localStorage snapshots
  (fast switch). Optional "strict isolation" = one UDF per account.
- Use the **HWND controller**, not the CompositionController: real `WM_*` input,
  Chromium's normal present path, no manual pointer injection.
- Settings: devtools off in release, context menu off, autofill/password prompts off,
  built-in error page off (we render our own), downloads off.
- Flags live in one table (§4.3) and are A/B tested; nothing is hardcoded blind.

### 3.3 Navigation policy

Only the game renders; every other URL goes to the browser.

```
allowlist (in-app):   https://moomoo.io, https://www.moomoo.io  (any path)
external (prompt):    every other top-level navigation, new window,
                      target=_blank, window.open, external protocol
untouched:            iframes (Turnstile, ads, FRVR SDK), sub-resources
```

```
OnNavigationStarting:                    // top-level only
    if !isGameUrl(args.Uri): args.Cancel = true; PromptExternal(args.Uri)

OnNewWindowRequested:
    args.Handled = true; PromptExternal(args.Uri)     // never a second WebView2

PromptExternal(uri):
    reject non-{http,https,mailto}
    if link_rules[host] == browser: ShellExecute(uri); return
    TaskDialog: "This link leaves the game. Open in your browser?"
                [Open in browser] [Cancel]  [ ] Remember for <host>
    on confirm: ShellExecuteW(L"open", uri)
```

- Host match: exact `moomoo.io`, or dot-boundary `.moomoo.io`, after lowercasing and
  stripping a trailing dot. Never `EndsWith` without a boundary.
- Re-check when `args.IsRedirected` — redirects are the usual escape.
- Never shell-execute `blob:`, `data:`, `javascript:`, `file:`, `ms-*`.
- Discord link: the OAuth round-trip ends on `moomoo.io/?code=…`; sending it to the
  system browser completes the link server-side. Refresh the profile afterwards.
- Optional: deny `about:blank` popups used by the ad SDK.

### 3.4 Accounts and sessions

The game's login is the only login. The client manages sessions.

- **Add:** complete email/code/password + Turnstile in the real UI, then capture
  `moomoo.io` cookies (`CookieManager.GetCookies`), the `localStorage` dump, the FRVR
  profile, and the account identity from the page. One encrypted blob per account.
  Never store the emailed code. Password storage is opt-in and labelled.
- **Switch:** snapshot current → delete `moomoo.io` cookies → clear localStorage →
  restore target cookies → preload applies the storage snapshot at `document-start`
  → reload. Target ≤ 1.5 s.
- **Auto sign-in:** restore last account, verify with a page-context profile read, and
  if expired show the sign-in card with the email prefilled (real click to submit).
- **Health:** check on start and every 15 min idle; say "session expired" instead of
  silently dropping to guest.
- **Concurrency:** blocked by design (one active controller). Also avoids multi-boxing.

### 3.5 Resource cache — what was built

> Revised after measuring the live origin on 2026-10-28. The original three-tier
> design (cache every `/assets,/p,/libs,/img,/css` path) was wrong for this site;
> the evidence is in `M3-CACHE-RESULTS.md`.

| Tier | What | Where |
|---|---|---|
| L1 | Chromium HTTP cache | per-UDF, 32 MB |
| L2 | content store, keyed by URL | `%LOCALAPPDATA%\MooClient\cache` |
| L3 | build manifest + prefetch | not built (needs SQLite, M4) |

**Only content-addressed URLs enter L2.** The origin sends no `ETag`, and its
`Last-Modified` is the *edge node's* cache-fill time — two Cloudflare nodes return
different values for the same unchanged file, so it cannot prove a body is
current. The only trustworthy freshness signal is the URL itself.

| Path | Origin headers | Decision |
|---|---|---|
| `/assets/index-<hash>.js` | `max-age=31536000, immutable` | **L2, forever.** The filename is a content hash; a new build is a new URL. |
| `/p/<module>.js` | `max-age=7200, immutable` | **L2, but revalidated on the first request of every run.** Not hashed, and a stale protocol module breaks the game. |
| `/css/*`, `/libs/*?v=`, `/img/*` | `max-age=120` | **Not intercepted.** Chromium's cache revalidates them correctly; caching them in L2 would either serve them stale or re-download them every launch (WebView2 does not cache responses we synthesise). |
| `/`, `/docs/*`, `manifest.json` | `no-store` | Never cached. |
| `api.moomoo.io`, Turnstile, ads | — | Never intercepted (different hosts, outside the filter). |

- **Serve:** `WebResourceRequested` (filtered to the two game hosts) → disk
  lookup → `CreateWebResourceResponse(file stream, 200, stored headers)`. Stored
  headers are the origin's minus `Content-Encoding`/`Content-Length`/`Vary`/
  `Set-Cookie`/`Date`/`Age`/`Server`; `Content-Length` is recomputed from the
  decoded body. `Range` requests bypass.
- **Capture:** `WebResourceResponseReceived` → `GetContent()` (32 MB cap) →
  SHA-256 → atomic write (`.tmp` + `MoveFileEx`) → metadata file. A capture that
  started before the copy already on disk is discarded, so a slow older response
  cannot overwrite a newer body.
- **Eviction:** LRU by last-used, 512 MB cap, trimmed to 90% when exceeded.
- **Refresh:** `Ctrl+F5` drops every non-immutable entry and reloads.
- **No offline play** (WebSocket needed). Blobs stay local, unmodified,
  unredistributed.

### 3.6 Data layer — SQLite

`data.db`, WAL, `synchronous=NORMAL`, `foreign_keys=ON`, forwards-only migrations keyed
by `user_version`, each in a transaction, with a pre-migration backup.

```sql
accounts(id TEXT PK, label TEXT, email TEXT, display_name TEXT,
         password_enc BLOB, color INT, created_at TEXT, last_used_at TEXT, flags INT)
sessions(account_id TEXT PK REFERENCES accounts(id) ON DELETE CASCADE,
         cookies_enc BLOB NOT NULL, storage_enc BLOB, captured_at TEXT, expires_at TEXT)
profiles(name TEXT COLLATE NOCASE PK, json TEXT, fetched_at TEXT)
stats(account_id TEXT, captured_at TEXT, score INT, kills INT, deaths INT,
      age INT, rank INT, clan TEXT, raw TEXT, PRIMARY KEY(account_id, captured_at))
matches(id INTEGER PK, account_id TEXT, started_at TEXT, ended_at TEXT,
        kills INT, deaths INT, score INT, duration_s INT)
friends(account_id TEXT, name TEXT, online INT, last_seen TEXT, note TEXT,
        PRIMARY KEY(account_id, name))
settings(key TEXT PK, value TEXT, updated_at TEXT)
builds(build_id TEXT PK, first_seen TEXT, index_sha TEXT, asset_count INT, bytes INT, active INT)
assets(url_hash TEXT PK, url TEXT, build_id TEXT, mime TEXT, size INT,
       sha256 TEXT, captured_at TEXT, last_used_at TEXT, immutable INT)
link_rules(host TEXT PK, action TEXT, created_at TEXT)
scripts(id TEXT PK, name TEXT, source TEXT, run_at TEXT, match TEXT,
        enabled INT, priority INT, updated_at TEXT)
script_state(script_id TEXT, account_id TEXT, json TEXT, PRIMARY KEY(script_id, account_id))
servers(region TEXT, host TEXT, port INT, ping_ms INT, seen_at TEXT, PRIMARY KEY(region,host,port))
events(id INTEGER PK, ts TEXT, level INT, area TEXT, message TEXT, data TEXT)
frame_stats(id INTEGER PK, ts TEXT, account_id TEXT, p50_ms REAL, p95_ms REAL,
            p99_ms REAL, long_tasks INT, fps REAL)
```

DB management: table browser, row counts/sizes, vacuum, integrity check, online
backup/restore, JSON/CSV export, cache/stat wipes, retention. Nightly WAL checkpoint,
`events` capped at 20k, `stats` 90 days unless pinned. Corruption → rename
`data.corrupt-<ts>.db`, recreate, restore backup, log loudly, never crash-loop.

Secrets: DPAPI (`CryptProtectData`, per-user, entropy = app GUID + field name).
SQLCipher is a later option, not v1.

### 3.7 Control Center

Local UI mapped via `SetVirtualHostNameToFolderMapping("app.local", …)` in a second
window. C++ keeps the hot path; the UI is cheap to iterate. WebMessage JSON with
versioned schema + request ids; no SQL passthrough.

Pages: Accounts, Database, Cache, Links, Scripts, Diagnostics, Settings.
Dark, one accent, keyboard-first, bundled fonts (works offline).

### 3.8 Scripts (optional, opt-in)

`AddScriptToExecuteOnDocumentCreated` userscripts, per-account enable/order/match,
editor + save. Nothing is bundled; the existing Toolkit/HEX scripts stay external.
Not part of the performance path — they run in the page and are disabled by default.

### 3.9 Diagnostics

- Native marks: `process_start`, `env_ready`, `controller_ready`, `navigate`,
  `dom_loaded`, `menu_visible`, `first_frame`.
- Page marks: `performance.mark`, long-task observer (>50 ms), rAF sampler (§4.6).
- Asset hit/miss/bytes per build; WebView2 version; GPU renderer string via
  `WEBGL_debug_renderer_info` (warn on SwiftShader/Basic Render).
- 2 MB rotating log; "copy diagnostics" zip with no credentials.

---

## 4. Performance architecture

### 4.1 The contract

The host must be **invisible** to the frame: no copy, no composition, no capture, no
overlay, no extra window above the game. Success is measured externally (§4.6), not
by how many flags are set.

What the host can control: startup time, asset bytes, process priority/power state,
GPU selection, background behavior, resolution/quality guidance, crash recovery.
What it cannot control: the game's own JS cost per frame.

### 4.2 Present-path invariants (hard rules)

1. **No layered/transparent window.** No `WS_EX_LAYERED`, no
   `SetLayeredWindowAttributes`, no Mica/Acrylic/`DwmExtendFrameIntoClientArea` over
   the game area. These force DWM composition of the whole window and can break
   independent-flip presentation.
2. **No native overlays.** No crosshair/FPS/toast child windows above the WebView.
   In-page DOM/CSS only (composited by Chromium), or a separate diagnostics window.
3. **Splash dies before the game appears.** Destroy it, don't hide it.
4. **No per-frame capture.** No `PrintWindow`, `BitBlt`, WGC, or Desktop Duplication
   loop. Screenshots only on explicit user action (`CapturePreviewAsync`).
5. **No host painting.** Never `InvalidateRect`/`WM_PAINT` the game area; no
   `WS_EX_COMPOSITED`; no custom cursor drawing.
6. **Fullscreen = `WS_POPUP`** sized exactly to the monitor, no thick frame, no
   rounded corners, no topmost.
7. **No low-level input hooks.** No `WH_KEYBOARD_LL`/`WH_MOUSE_LL`; macros use
   `SendInput` only when the user triggers them.
8. **Do not set `WDA_EXCLUDEFROMCAPTURE`** by default; it can disable hardware overlays.
9. **One monitor.** Offer "lock to monitor" so mixed-refresh setups don't hitch when
   the window is moved.

### 4.3 Engine flags (A/B tested, one profile table)

| Flag | Effect | Default |
|---|---|---|
| `--use-angle=d3d11` | force D3D11 ANGLE backend; predictable GPU path | on |
| `--enable-gpu-rasterization` | GPU raster for canvas/compositing | on |
| `--enable-zero-copy` | avoid a compositor copy where supported | on |
| `--enable-accelerated-2d-canvas` | must never be off | on |
| `--disable-background-timer-throttling` | keep timers running when unfocused | on |
| `--disable-renderer-backgrounding` | don't deprioritize the renderer | on |
| `--disable-backgrounding-occluded-windows` | keep rAF when covered | on |
| `--disable-features=CalculateNativeWinOcclusion` | game keeps running when alt-tabbed | optional |
| `--disable-features=UseEcoQoSForBackgroundProcess` | keep renderer off E-cores | on |
| `--disable-features=msWebOOUI,msPdfOOUI` | no built-in UI | on |
| `--disk-cache-size=33554432` | small; L2 serves assets | on |
| `--disable-frame-rate-limit` + `--disable-gpu-vsync` | uncapped/tearing; latency experiment | **off** |
| `--force-device-scale-factor` | DPI correctness only, not a render-scale lever | unset |
| `--no-sandbox`, `--in-process-gpu`, `--single-process` | stability/security | **never** |

Note: DPI/scale factor does **not** reduce canvas pixels in fullscreen — the backing
store tracks physical pixels either way. The real render-scale lever is the game's own
"Use Native Resolution" setting, exposed as a one-click performance preset (§4.5).

### 4.4 OS-level tuning

- **Job object** with `JOB_OBJECT_LIMIT_PRIORITY_CLASS` = `ABOVE_NORMAL_PRIORITY_CLASS`
  for the whole process tree (host, GPU, renderer, network). Not `HIGH`, never
  `REALTIME` — starving input/audio makes frame pacing worse.
- **EcoQoS off** for the tree: `SetProcessInformation(ProcessPowerThrottling)` with
  `PROCESS_POWER_THROTTLING_EXECUTION_SPEED` cleared, plus the Chromium feature flag.
  Keeps the renderer on P-cores on hybrid CPUs.
- **GPU preference:** write `GpuPreference=2;` for the exe under
  `HKCU\Software\Microsoft\DirectX\UserGpuPreferences` (installer + Settings toggle).
  This is the same switch as Windows "High performance" graphics settings.
- **Display/keep-awake** during matches; warn on battery saver / power-saving plan.
- **CPU sets:** experimental, off by default; pin only if A/B proves a win and the GPU
  process is never starved.
- `SetThreadExecutionState` only; no timer-resolution hacks, no registry tweaks.

### 4.5 Quality guidance

- Detect 4K + weak GPU (renderer string) and surface a native hint: turn off the
  game's "Use Native Resolution", use fullscreen. One click applies it through the
  real checkbox (trusted click).
- Offer 60/120/144/uncapped guidance based on the monitor; never force a cap the game
  doesn't support.
- Warn if WebGL reports SwiftShader/Basic Render — hardware acceleration failed.

### 4.6 Frame-time instrumentation and gates

- Passive rAF sampler injected at document-start: ring of 6k frame intervals,
  computes p50/p95/p99, long-task count, effective fps; native pulls a 1-min aggregate
  into `frame_stats`. Zero allocation in the hot loop; sampler detaches on request.
- External validation with **PresentMon** on reference machines: require independent-
  flip present mode and record `msUntilDisplayed` p50/p95; compare against Edge on the
  same box, same scene, same monitor.
- **Release gate:** no build ships if p99 frame time regresses >10% vs the previous
  release, or if startup p95 exceeds budget, or if the host uses >3% CPU while idle
  in the menu.

### 4.7 Budgets

| Metric | Target |
|---|---|
| Process start → window visible | ≤ 250 ms |
| Window visible → menu interactive (warm) | ≤ 1.2 s |
| Menu interactive (cold, 20 Mbit) | ≤ 4 s |
| Relaunch asset bytes | ≤ 2% of build |
| Account switch | ≤ 1.5 s |
| Host CPU while playing | ≤ 3% of one core |
| Host idle RAM (excluding renderer) | ≤ 80 MB |
| Frame time | parity with Edge ±5%, p99 ≤ 1.5× refresh interval |
| Disk per account | ≤ 250 MB; L2 ≤ 512 MB |

---

## 5. Distribution

### 5.1 WebView2 runtime strategy

- **Fixed version is the shipping default for MooClient.** The installer places
  `Microsoft.WebView2.FixedVersionRuntime.<ver>.x64` under `<install>\runtime\`.
  The host detects `runtime\msedgewebview2.exe` and passes that folder to
  `CreateCoreWebView2EnvironmentWithOptions`, so the app does not depend on the
  machine's Edge/WebView2 install or its update schedule. The version is pinned by
  `cmake/fetch_webview2_runtime.ps1`; bumping it is an explicit release step.
- Evergreen remains the fallback for development builds and for a missing `runtime\`
  folder: detect via the EdgeUpdate client key for the WebView2 Runtime product GUID
  `{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}` (HKLM/HKCU, WOW6432Node) and
  `…\Microsoft\EdgeWebView\Application\<ver>\msedgewebview2.exe`.
- Fixed runtime payload: ~300 MB cab, ~670 MB extracted (Chromium DLLs); the
  installer compresses it. Servicing is our responsibility: rebuild when Chromium
  security fixes matter.
- Never require admin for the app itself; the runtime installer may elevate.

### 5.2 Packaging

- One exe: static CRT (`/MT`), static `WebView2Loader`, SQLite amalgamation,
  header-only JSON. Target ~6–10 MB.
- UI + preload + migrations embedded as resources; extracted to
  `%LOCALAPPDATA%\MooClient\ui\<build>\` on first run with per-file SHA-256 checks.
  No loose files to tamper with, no execution from `%TEMP%`.
- Installer: **Inno Setup 6** (per-user, `%LOCALAPPDATA%\Programs\MooClient`, no UAC),
  plus a portable ZIP. WiX/MSI only if enterprise deployment is ever needed.
- Shortcuts, uninstall entry, optional "launch at login" off by default.
- Manifest: `perMonitorV2`, `longPathAware`, `asInvoker`, no `uiAccess`.
- x64 first; arm64 build later (WebView2 supports it) — same code, different triple.
- No services, no drivers, no shell extensions, no scheduled tasks → AV-clean.

### 5.3 Signing

- Authenticode SHA-256 + RFC3161 timestamp on exe and installer.
- EV certificate if public distribution; SmartScreen reputation is the main install
  friction and only signing + downloads fix it.
- Pin the signer thumbprint in the updater and verify the downloaded file's signature,
  not just its hash.

### 5.4 Updates

- `update.json` (channel, version, url, sha256, min_version, notes) signed with an
  embedded Ed25519/minisign public key — a compromised CDN cannot push a build.
- Download → verify manifest signature → verify SHA-256 → verify Authenticode signer →
  run installer `/VERYSILENT /NORESTART /CLOSEAPPLICATIONS` → relaunch.
- Channels: stable / beta. Keep the previous installer for one-click rollback.
- Check on launch (non-blocking) and every 6 h; never interrupt a match.

### 5.5 Licensing, branding, legal

- Ship the WebView2 SDK/runtime notices, SQLite (public domain), JSON (MIT), your EULA.
- **Do not ship game assets, art, or fonts.** The client downloads everything from the
  origin at runtime. Exception, decided by the user: the official `moomoologo` artwork is
  embedded as the application icon and the startup splash (`res/logo.png` →
  `tools/make_assets.py` → `res/app.ico`, `res/splash.png`). It is never fetched at
  runtime and no other game art is bundled.
- **Trademark:** do not name it in a way that implies affiliation. Working name
  "MooClient" or similar neutral branding; "for moomoo.io" as a compatibility note only.
- FRVR's ToS governs accounts. A third-party client is a grey area at best and a
  takedown target at worst; distribution raises that risk. No automation bundled, no
  "ban-proof" claims, no monetization that implies endorsement.
- Privacy policy: local-only data, zero telemetry, opt-in diagnostics upload only.

### 5.6 Support

- "Copy diagnostics" produces a zip: versions, flags, GPU string, startup waterfall,
  frame stats, log tail, DB schema version — no cookies, no tokens, no email.
- Optional crash upload, off by default, explicit consent per upload.

---

## 6. Security and privacy

- DPAPI blobs only; reveal requires a click; cleared on account delete.
- Cache paths derive from hashes, never URL text (no traversal); verify SHA-256 on read
  and drop mismatches.
- Exact host matching, redirect re-check, scheme allowlist, no shell-exec of non-http.
- No TLS interception, no WS proxy, no DLL injection, no overlay injection.
- Backup/export warns that it contains secrets.
- Zero telemetry, zero analytics, no crash upload without consent.

---

## 7. Terms-of-service risk

- The untrusted-event counter is an anti-automation signal. Forwarding real input is
  fine; macro injection is detectable in principle.
- Wrapping the official site does not grant permission to modify or automate it.
  Scripts stay user-supplied, off by default, warned about, unbundled.
- Multi-account concurrent play is blocked (multi-boxing).
- No bypassing Turnstile, ads, consent, or paywalls; no asset redistribution.
- If the goal is competitive advantage rather than a better launcher, stop: bans and
  protocol drift outweigh the gains.

---

## 8. Milestones

| # | Scope | Exit criteria |
|---|---|---|
| M0 | Spike: WebView2 + game + cookie persistence + link prompt | menu loads, login survives restart, `example.com` opens in the default browser |
| M1 | Shell: window, DPI, fullscreen, single instance, crash dump, flags profile | no white flash; crash dump; second launch focuses first |
| M2 | Accounts: SQLite + migrations + DPAPI, capture/switch/autofill/health | 2 accounts, 50 switches, no session loss; survives kill -9 |
| M3 | Cache: L2 store, capture/serve, build pinning, LRU, prefetch, metrics | relaunch fetches <2% of assets; §4.7 met |
| M4 | Control Center: accounts, DB, cache, links, scripts, diagnostics | every page functional, no extra native dialogs |
| M5 | Performance: flags A/B, job/power/GPU tuning, PresentMon validation | parity with Edge ±5%, p99 gate, host CPU ≤3% |
| M6 | Distribution: Inno installer, runtime bootstrap, embedded UI, signing | clean VM installs and runs with no admin, no runtime, no VC++ redist |
| M7 | Update: signed manifest, silent installer, rollback, channels | update from N-1 works, bad signature refused |
| M8 | Optional R&D: native protocol spike (2 weeks, timeboxed) | go/no-go memo with measured latency delta; default **no-go** |

Estimate: M0–M7 ≈ 5–6 weeks part-time, ~3 weeks full-time.

---

## 9. Test plan

- Unit: host matching (`moomoo.io.evil.com`, IDN, trailing dot), cache policy, DPAPI
  round-trip, migration ladder, LRU, link rules, manifest signature parsing.
- Integration (local fake origin with a recorded manifest): cold → warm → version bump
  → rollback → offline → 404 → truncated blob → disk full → corrupt DB → WebView2
  process kill.
- Session: 100 switches, expired cookie, revoked session, two accounts with different
  localStorage.
- Nav: 40 hostile URLs (redirect chains, `javascript:`, `file:`, `ms-settings:`,
  userinfo tricks, punycode lookalikes) — prompt or refuse, never render.
- Perf: 20 cold + 50 warm starts, p50/p95 per mark; 10-minute play session with
  PresentMon; A/B against Edge on the same machine.
- Manual matrix: Win10 22H2 / Win11 24H2; 100/125/150/200% DPI; mixed-refresh dual
  monitor; Intel/NVIDIA/AMD; Intel UHD 620 8 GB; no runtime; proxy; VPN; battery.
- Distribution: clean VM, standard user, no admin, no runtime, Defender on, offline
  install path, uninstall leaves no services/tasks.

---

## 10. Risks

| Risk | Impact | Mitigation |
|---|---|---|
| Game update changes bundle layout | cache misses, stale pins | content-hash keys, passive capture, `index.html` never cached, no JS rewriting |
| Sealed protocol / anti-cheat change | script features break | scripts optional and page-scoped; core never depends on them |
| WebView2 missing on target | app won't start | detect → bootstrapper → retry; fixed-version offline variant |
| GPU blocklist / software rendering | low fps | detect renderer string, warn, offer `--ignore-gpu-blocklist` guidance |
| AV flags HTTP interception | false positive | signing, no injection, no proxy, no persistence tricks |
| Layered/overlay feature creep | real fps loss | §4.2 invariants enforced in review + PresentMon gate |
| Runtime auto-update regression | perf/compat drift | Evergreen default, but pin-test releases; fixed-version option |
| SmartScreen reputation | install friction | EV cert, signed installer, download volume |
| Trademark/ToS complaint | takedown | neutral branding, no assets, no bundled automation |
| Disk growth | GBs | 32 MB per-profile cache, 512 MB L2 cap, per-account purge |
| Credential loss / DB corruption | lockout | backups, corrupt-recovery, re-login is always possible |

---

## 11. Open decisions (defaults chosen)

1. **Scripts shipped?** No — launcher only; user files.
2. **Accounts at once?** No — one active session, switch only.
3. **Ad/tracker blocking?** Allow game-required frames; optional tracker block; ad SDK
   not blocked by default.
4. **Profile model?** Shared UDF + snapshots (fast); strict isolation optional.
5. **Runtime?** Evergreen default; fixed-version offline variant optional.
6. **UI?** Local web Control Center; native only for splash/prompts.
7. **Distribution scope?** Public release vs friends-only changes signing, EV cert,
   update hosting, and legal exposure. Public needs the branding/ToS work in §5.5.
8. **Minimum OS?** Win10 1809 x64.

---

## 12. Build prerequisites (as built)

The machine already had VS 18 Build Tools with MSVC 14.51 but **no Windows SDK
headers/libs** (the `Win11SDK_10.0.26100` payload was skipped during install). The
project therefore bootstraps everything except the compiler, without admin rights:

1. VS 18 Build Tools — "Desktop development with C++" (MSVC 14.51.36231).
2. `python -m pip install cmake ninja` (installs to the per-user Scripts dir).
3. `cmake/bootstrap_deps.ps1` downloads and extracts:
   - `Microsoft.Web.WebView2` 1.0.4314.35 → `third_party/webview2/`
   - `Microsoft.Windows.SDK.CPP` 10.0.28000.0 (headers) → `third_party/winsdk/base/`
   - `Microsoft.Windows.SDK.CPP.x64` (x64 libs) → `third_party/winsdk/x64/`
   - `Microsoft.Windows.SDK.BuildTools` (`rc.exe`, `mt.exe`) → `third_party/winsdk/buildtools/`
4. `cmake/build.ps1` imports vcvars64, prepends the SDK include/lib paths, configures
   Ninja, builds, and post-links the manifest with `mt.exe`.
5. SQLite amalgamation + `nlohmann/json` are still to be vendored (M2).
6. Inno Setup 6 for M6; `signtool` for signing.

```
moomoo-client/
  CMakeLists.txt
  cmake/            bootstrap_deps.ps1, version.cmake
  src/
    main.cpp
    shell/          window.cpp, splash.cpp, single_instance.cpp, crash.cpp, job.cpp
    webview/        env.cpp, controller.cpp, nav_policy.cpp, flags.cpp, preload.cpp
    cache/          store.cpp, capture.cpp, build_id.cpp, lru.cpp
    store/          db.cpp, migrations/, accounts.cpp, settings.cpp, dpapi.cpp
    accounts/       session.cpp, switch.cpp, autofill.cpp
    ui/             bridge.cpp, control_center.cpp
    diag/           log.cpp, marks.cpp, frame_stats.cpp
  ui/               index.html, app.css, app.js, fonts/
  res/              app.rc, icon.ico, manifest, ui.zip
  third_party/      webview2/, sqlite/, json/
  tests/            unit/, integration/
  installer/        moonsetup.iss, update_manifest.json
```

---

## 13. What "done" looks like

Launch from the taskbar → splash → menu in about a second, already signed in. Every
link that isn't the game opens in the default browser after one clear question. The
Control Center shows accounts, a 400 MB cache at 96% hit rate, a clean database with
one-click backup, and PresentMon numbers proving frame-time parity with Edge. No game
code modified, no traffic intercepted, nothing uploaded. Installs per-user from one
signed exe, on a clean Windows box, with no admin prompt beyond the WebView2 runtime.
