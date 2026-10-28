#pragma once

#include <string>

namespace moo::cache {

// Which requests the L2 store is allowed to touch.
//
// Measured against the live origin (2026-10-28):
//   /assets/index-<hash>.js   public, max-age=31536000, immutable
//   /p/<module>.js            public, max-age=7200, immutable   (not hashed)
//   /css/main.css?52          public, max-age=120
//   /libs/*.js?4              public, max-age=120
//   /img/*.png                public, max-age=120
//   /                         no-store
//
// The origin sends no ETag, and `Last-Modified` is the *edge node's* cache-fill
// time: two Cloudflare nodes return different values for the same unchanged
// file. So there is no validator we can trust for the 120-second assets.
//
// The rule that follows: the store only holds content whose URL proves the
// bytes — a hashed bundle name, or the `/p/` modules which are checked once per
// run. Everything else is left to Chromium, whose own HTTP cache revalidates it
// correctly and cheaply.
enum class Tier {
    // Not ours: documents, the API, images, styles, fonts, anything unversioned.
    Never,
    // The filename is a content hash, so a new build is a new URL. Served from
    // disk forever, with no network request at all.
    Immutable,
    // Changes in place, but is small and load-critical. The first request of
    // each run goes to the network (so an update is picked up on the next
    // launch); after that the stored copy is used for the rest of the run.
    RevalidateOnce,
};

struct Policy {
    Tier tier = Tier::Never;
};

// Classifies a full URL. Only https on the two game hosts can ever be cached.
Policy Decide(const std::wstring& url);

// True for `no-store` and `private`, which must never reach the disk.
bool IsUncacheable(const std::string& cache_control);

} // namespace moo::cache
