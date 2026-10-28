# M3 — asset cache: results

Date: 2026-10-28
Scope: L2 content store for the real moomoo.io client, plus update semantics.
Code: `src/cache/` (`asset_policy`, `asset_cache`, `sha256`), wired in
`src/webview/webview_host.cpp`.

## 1. What the origin actually promises

Measured with HEAD requests against `https://moomoo.io` (v1.9.4, build
`index-993d072d.js`):

| URL | Cache-Control | ETag | Last-Modified |
|---|---|---|---|
| `/` | `no-store` | – | – |
| `/assets/index-993d072d.js` | `public, max-age=31536000, immutable` | – | `Wed, 21 Oct 2026 11:19:21 GMT` |
| `/p/s16pdk.js` | `public, max-age=7200, immutable` | – | deploy time |
| `/css/main.css?52` | `public, max-age=120` | – | deploy time |
| `/libs/frvr-sdk.min.js?4` | `public, max-age=120` | – | deploy time |
| `/img/favicon.png?v=2` | `public, max-age=120` | – | deploy time |

Two findings drove the design:

1. **No ETag anywhere.** The only validator is `Last-Modified`.
2. **`Last-Modified` is not a content validator.** It is the edge node's
   cache-fill time: repeated HEADs against the same node return a stable value,
   but a different node returns a different one for an unchanged file. A
   background revalidation sweep reported "changed" for 78 of 80 files that had
   not changed at all.

Consequence: you cannot prove an unversioned body is current without downloading
it. So the cache must key on URLs that are themselves content-addressed.

## 2. What is cached

| Tier | Paths | Behaviour |
|---|---|---|
| Immutable | `/assets/*` | Served from disk forever. New build ⇒ new hash ⇒ new key. |
| RevalidateOnce | `/p/*` | First request of each run goes to the network; the rest of the run is served from disk. |
| Never | everything else | Left to Chromium's HTTP cache. |

Not cached, deliberately:

- Images, CSS, fonts, `?v=` libraries — `max-age=120`, and WebView2 does **not**
  store responses produced by `CreateWebResourceResponse`, so an L2 copy would be
  re-downloaded in full on every launch. Chromium's own cache revalidates them
  with `If-Modified-Since` and usually gets a 304, which is strictly better.
- `index.html`, `manifest.json`, `/docs/*` — must always be live.
- `api.moomoo.io`, Turnstile, ad/consent iframes — outside the filter.

## 3. Measured effect

Same machine, same account, game menu reached (`DOMContentLoaded`), Chromium
cache cleared before the cold run:

| Run | L1 | L2 | DOMContentLoaded | L2 hits | Bytes from L2 |
|---|---|---|---|---|---|
| cold | empty | empty | 2781 ms | 0 | 0 |
| warm | warm | 3 entries | 2375 ms | 3 | 0.6 MB |
| warm | warm | 3 entries | 2234 ms | 3 | 0.6 MB |

L2 holds 3 entries / 0.34 MB: `assets/index-993d072d.js` (294 KB),
`assets/vendor-a3a301f0.js` (43 KB), `p/s16pdo.js` (7 KB).

With both caches empty the same three files cost ~1.1 s of the 2.8 s menu time;
the rest is the game's own JS execution and the FRVR SDK.

## 4. Update semantics

| Event | Result |
|---|---|
| Game ships a new bundle | New hashed filename ⇒ cache miss ⇒ fetched once, old key ages out via LRU. |
| `/p/` module changes in place | Caught on the next launch, because the first request of every run is a real network request. |
| Any other asset changes | Chromium's normal revalidation (`max-age=120`, `If-Modified-Since`). |
| User presses `Ctrl+F5` | All non-immutable entries dropped, page reloaded. |
| Stale copy on disk | Never served: there is no TTL to outlive, because only content-addressed URLs are stored. |

Race guard: `Store` records the tick at which the originating request started and
ignores a capture older than the copy already on disk, so a slow response cannot
overwrite a newer body.

## 5. Storage

```
%LOCALAPPDATA%\MooClient\cache\
  b\<hh>\<hh>\<sha256(url)>          decoded response body
  m\<hh>\<hh>\<sha256(url)>.meta     url, headers, tier, timestamps
```

- 512 MB cap, LRU eviction to 90% of the cap.
- Bodies written atomically (`.tmp` + `MoveFileExW`), size verified on load and on
  every serve; a mismatch drops the entry.
- Header block is stored ready to replay, so serving is a file open plus
  `CreateWebResourceResponse` — no parsing on the hot path.
- Cache is shared by all profiles: it is keyed by URL and holds no account data.

## 6. What was rejected

| Option | Why not |
|---|---|
| Cache `/img`, `/css`, `/libs` with a long TTL | Would serve stale art/CSS. There is no validator to prevent it. |
| Background HEAD sweep before serving | `Last-Modified` is per-edge-node, so it produces false "changed" for most files. |
| Serve stale + revalidate in background | Same staleness hole, just smaller. Rejected on principle. |
| Cache `?v=` libraries forever | 700 KB/launch saved, but a silent update without a version bump would never be picked up. |
| Native HTTP fetch for revalidation | Violates the "native code never speaks HTTP for the game" invariant. |
