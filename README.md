# MooClient

A native Windows shell around the real moomoo.io client: no browser chrome, persistent
accounts, a local asset cache, and a hard rule that only `moomoo.io` renders inside the
window. Every other link opens in the default browser after a confirmation prompt.

This repository currently implements **M0** of `00-PLAN.md` plus the M1 basics.

## What works so far

- Win32 host window; the game loads in a persistent WebView2 profile (login survives
  restart).
- **Asset cache.** `assets/*` bundles (hashed filenames) are stored decoded on disk
  and served from there on every later launch; the sealed `/p/` protocol modules are
  re-checked once per launch and then served locally. Nothing else is intercepted —
  Chromium's own HTTP cache revalidates the rest correctly. `Ctrl+F5` drops the
  cached copies and reloads. Cache: `%LOCALAPPDATA%\MooClient\cache` (512 MB cap,
  LRU). Design and measurements: `M3-CACHE-RESULTS.md`.
- Floating splash: the logo (with drop shadow) is shown centered on screen, borderless
  and click-through, while WebView2 boots and the document loads. It disappears when
  `DOMContentLoaded` fires, or after a 20 s fallback.
- Any top-level navigation or popup that is not `https://moomoo.io` / `https://www.moomoo.io`
  is cancelled and offered to the default browser.
- In-memory "remember this site" rule.
- Plain-text pages on moomoo.io (for example `/docs/versions.txt`) are re-rendered as a
  dark, readable document: parsed version cards, a **Back** button, `Esc` and `Alt+Left`.
  The parser detects version headings and bullets; the entries are not hardcoded.
- The document view uses the embedded `moomoobg.PNG` as a **fixed** background
  (`background-size: cover`, so it crops instead of stretching) with translucent cards
  for readability. The image is served through the `mooclient.local` virtual host.
- Dark title bar, `MooMoo.IO` title, per-monitor DPI, single instance, file log, crash-free
  close. The icon and splash both come from the official `moomoologo` artwork:
  `res/logo.png` is the master, `tools/make_assets.py` regenerates `res/app.ico`
  (10 sizes, 16-256 px) and `res/splash.png` (logo + baked shadow).
- `F11` or `Alt+Enter` toggles fullscreen (handled through WebView2's accelerator event,
  so it works while the game has focus).
- `Ctrl+F5` drops the cached bundle copies and reloads from the origin.
- `Ctrl+Alt+E` opens the navigation-policy probe for `example.com`.
- Unhandled exceptions write a minidump to `%LOCALAPPDATA%\MooClient\crashes\`.
- Process tree runs `ABOVE_NORMAL` in a job object with EcoQoS disabled.
- Independent runtime option: a WebView2 **Fixed Version** folder next to the exe
  (`runtime\`) is detected automatically, so the app does not depend on the installed
  Edge WebView2 runtime or its update schedule. `--runtime <dir>` overrides it.

Command line: `MooClient.exe [--url <url>] [--user-data <dir>] [--cache-dir <dir>]
[--no-cache] [--no-update] [--runtime <dir>] [--dev]`.

Log: `%LOCALAPPDATA%\MooClient\logs\client.log`
Profile: `%LOCALAPPDATA%\MooClient\profiles\default`
Cache: `%LOCALAPPDATA%\MooClient\cache`

## Performance notes

Measured on this machine, game page loaded and settled:

| State | Working set |
|---|---|
| Original flags, active | ~1.03 GB |
| Tuned flags, active | ~798 MB |
| Tuned flags, minimized (memory target `LOW`) | ~576 MB |
| Tuned flags, restored | ~647 MB |

Numbers fluctuate with the game's ad/consent iframes; the minimized row is the
repeatable one. The host itself is ~19 MB.

What changed:

- Background services disabled (component update, domain reliability, sync, breakpad,
  background networking), spare renderer disabled, 32 MB disk cache, V8 heap capped at
  512 MB, GPU rasterization/zero-copy/ANGLE D3D11 on.
- `WM_SIZE` minimized sets WebView2's memory target to `LOW`; restoring returns it to
  `NORMAL`.
- `--no-proxy-server` removes WPAD/proxy auto-detection from the connection path.
- The whole process tree (browser, GPU, network service, renderers) is in an
  `ABOVE_NORMAL` job with EcoQoS off, so the network service is not throttled while the
  renderer is busy.

Ping is still dominated by the physical route to the game server; no client flag can
change that. The client removes local overhead (proxy lookup, background traffic,
throttling) and leaves the network stack alone otherwise. On this machine the system
is already direct (no proxy), so `--no-proxy-server` is a no-op here; it removes WPAD
lookups on machines that have auto-proxy enabled.

## Install

Download `MooClientSetup-<version>.exe` from the
[releases page](https://github.com/Kaiso-666/MooClient/releases/latest) and run it.

- Per-user install in `%LOCALAPPDATA%\Programs\MooClient`; no administrator rights, no
  UAC prompt, no VC++ redistributable.
- Desktop icon (optional task), Start Menu entry, and a standard uninstall entry under
  *Settings → Apps*.
- The uninstaller keeps the WebView2 profile (login) and removes only the regenerable
  cache and logs.

### Updates

On startup, once the game window is up, MooClient asks GitHub for the latest release in
the background. If there is a newer one it shows a single dialog with **Ignore** and
**Update now**. Nothing is downloaded until Update is pressed; a failed or slow check is
invisible. `--no-update` skips the check.

Update downloads the setup exe to `%TEMP%`, verifies it is a non-empty executable, runs
it `/SILENT` (the installer closes the running app through its mutex), and exits.

## Build

Prerequisites: Windows 10 1809+, Visual Studio Build Tools with the C++ workload, Python 3.
A machine-wide Windows SDK is optional: the bootstrap script downloads the same
headers/libs from Microsoft's NuGet packages into `third_party/winsdk`.

```powershell
python -m pip install cmake ninja
powershell -ExecutionPolicy Bypass -File cmake/bootstrap_deps.ps1
powershell -ExecutionPolicy Bypass -File cmake/build.ps1
```

Run `build\bin\MooClient.exe`.

To build the installer as well (requires [Inno Setup 6](https://jrsoftware.org/isdl.php)):

```powershell
powershell -ExecutionPolicy Bypass -File cmake/package.ps1
```

It produces `dist\MooClientSetup-<version>.exe`, taking the version from the exe's
`VERSIONINFO` resource, which in turn comes from `project(... VERSION ...)` in
`CMakeLists.txt`.

For a build that does not use the installed Edge runtime, fetch the fixed runtime and
copy it next to the exe (about 670 MB extracted; required for packaging only):

```powershell
powershell -ExecutionPolicy Bypass -File cmake/fetch_webview2_runtime.ps1 -Deploy
```

`-Deploy` copies it to `build\bin\runtime`, which `MooClient.exe` picks up automatically.

First run may show a Windows Defender Firewall prompt for `msedgewebview2.exe`. The
game only needs outbound connections, so the prompt can be dismissed; it is not
something MooClient can suppress without admin rights.

## Verified so far

- Builds clean with MSVC 19.51 + the NuGet Windows SDK (no admin, no machine-wide SDK).
- Loads `https://moomoo.io` v1.9.4 in the WebView2 profile; game menu and canvas render.
- Fixed runtime `156.0.4314.50` loads via `--runtime` / `runtime\`.
- Splash renders centered with alpha and drop shadow; closes at `DOMContentLoaded`.
- Changelog renders as version cards with a working Back button (verified: returns to
  `https://moomoo.io/`); the embedded `moomoobg.PNG` stays fixed and `cover`-cropped
  while the cards scroll.
- Fixed runtime auto-detection verified: `build\bin\runtime` junction → runtime
  `156.0.4314.50`, no `--runtime` argument needed.
- Manifest is embedded: PerMonitorV2, comctl32 v6, `asInvoker`.

## Not yet implemented (next milestones)

Accounts + encrypted session snapshots (M2), SQLite control center (M4). The asset
cache (M3), installer, and updater (M6/M7) are implemented; see `M3-CACHE-RESULTS.md`.
