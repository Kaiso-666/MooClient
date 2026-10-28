# M0 results — 2026-10-28

## Environment

- Compiler: MSVC 19.51.36260 (VS 18 Build Tools, `14.51.36231`).
- Windows SDK: `10.0.26100.0` installed by the VS installer after the skipped payload was
  repaired; the build also works from the NuGet fallback (`10.0.28000.0`).
- WebView2 SDK: `1.0.4314.35`; runtime `155.0.4283.62` (per-user).
- CMake 4.4.4 + Ninja from `pip`, no admin.

## Build

- `build/bin/MooClient.exe`, 2.1 MB, `/MT`, static WebView2 loader, no VC++ redist.
- Embedded manifest verified with `mt.exe`: PerMonitorV2, comctl32 v6, `asInvoker`.
- `cmake/bootstrap_deps.ps1` reproduces the SDK setup from nuget.org.
- `cmake/build.ps1` imports vcvars, prepends the SDK paths, builds and post-links the
  manifest.

## Verified

| Check | Result |
|---|---|
| Game loads | moomoo.io v1.9.4 menu, canvas and region/server picker render |
| Warm start (controller) | 759 ms (12:50 run), 855 ms (12:59 run) |
| Warm `DOMContentLoaded` | 2.47 s (12:59 run) |
| Warm page `load` | 7.88 s (ads/consent/FRVR; menu is interactive earlier) |
| External URL policy | `Ctrl+Alt+E` → prompt → default browser, logged at 12:51:43 |
| Log while running | readable after switching to `_SH_DENYNO` |
| Profile persistence | `%LOCALAPPDATA%\MooClient\profiles\default\EBWebView` created and reused |
| Manifest | PerMonitorV2 + comctl v6 + asInvoker |

Screenshots: `docs/m0-menu.png`, `docs/m0-nav-prompt.png`.

Raw log: `%LOCALAPPDATA%\MooClient\logs\client.log`.

## Issues

- First launch shows a Windows Defender Firewall prompt for `msedgewebview2.exe`.
  Outbound HTTP/WS works without allowing it; suppressing it needs admin/firewall rules.
- Startup varies with machine load (1.9 s controller while the VS installer was finishing).
- The `NewWindowRequested` path (game links) was not exercised by a real click because
  the desktop was busy with Snipping Tool focus; `NavigationStarting` is verified.
- `DOMContentLoaded` at ~2.5 s is above the 1.2 s target; M3's L2 cache/prefetch is the
  intended fix, not a host change.

## Next

- M1: fullscreen toggle (implemented, needs an idle-desktop test), crash dumps
  (installed, not yet exercised), flags profile.
- M2: SQLite + DPAPI, account capture/switch, session health.
- Re-test the real game-link popup with the desktop idle.

## M1 progress (13:05 build)

- Job object at `ABOVE_NORMAL` + EcoQoS opt-out applied; log line confirmed.
- Crash handler installed (`MiniDumpWriteDump` to `%LOCALAPPDATA%\MooClient\crashes\`).
- `F11` / `Alt+Enter` fullscreen wired through `ICoreWebView2Controller::
  add_AcceleratorKeyPressed`; not verified because the desktop was in use and the
  keypress went to Firefox. No more input automation until the machine is idle.

## M1 additions (14:15 build)

- **Fixed-version runtime**: `third_party/webview2-fixed/156.0.4314.50`; detected via
  `runtime\msedgewebview2.exe` next to the exe or `--runtime <dir>`. Log confirms
  `webview: runtime 156.0.4314.50` and the child process path. `cmake/fetch_webview2_runtime.ps1`
  downloads/extracts it; `-Deploy` copies it to `build\bin\runtime`.
- **Memory tuning**: background services off, spare renderer off, 32 MB disk cache,
  512 MB V8 heap cap, GPU raster/zero-copy/ANGLE D3D11; `WM_SIZE` minimized sets
  `ICoreWebView2_19::put_MemoryUsageTargetLevel(LOW)`. Active ~798 MB, minimized
  ~576 MB, restored ~647 MB (vs ~1.03 GB original, same page state).
- **Ping work**: `--no-proxy-server`, background networking off, whole process tree in
  an `ABOVE_NORMAL` job with EcoQoS off. No physical-RTT change is possible client-side.
- **Splash**: layered, click-through, borderless `MooMoo.IO` logo + drop shadow centered
  on the primary work area; shown at 14:02:58.163, closed at `DOMContentLoaded`
  (14:03:01.266), main window shown 14:03:01.629. 20 s fallback timer.
- **Document view**: injected script detects `document.contentType === 'text/plain'` on
  moomoo.io and re-renders it as version cards with a Back button (`Esc`, `Alt+Left`).
  Verified on `/docs/versions.txt`: 106 versions parsed, Back navigates to
  `https://moomoo.io/`. Background `moomoobg.PNG` is embedded, served from
  `mooclient.local`, `position: fixed` + `background-size: cover` (crop, not stretch);
  scrolling verified: cards move, background stays.
- **Title/icon**: `MooMoo.IO` title verified via `GetWindowText`; embedded icon
  (`res/app.ico`) verified via `WM_GETICON` and `ExtractAssociatedIcon`.

## Brand asset refresh (19:05 build)

- The official logo was replaced (source file is a 256x256 WebP despite the `.png`
  extension). It is normalised to `res/logo.png` and is now the single master asset.
- `tools/make_assets.py` regenerates both embedded assets from that master:
  `res/app.ico` with 10 PNG-compressed entries (16, 20, 24, 32, 40, 48, 64, 96, 128,
  256) and `res/splash.png` (800 px logo on the 1220x1000 canvas, three stacked
  shadow passes derived from the logo alpha).
- Verified: splash renders the new mark centered with its shadow (window 560x459 at
  520,200); `WM_GETICON` returns big and small handles; `ExtractAssociatedIcon` on the
  exe returns the new 32 px mark.
