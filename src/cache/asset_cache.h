#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include "cache/asset_policy.h"

namespace moo::cache {

struct Stats {
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t stores = 0;
    uint64_t evictions = 0;
    uint64_t revalidated = 0;
    uint64_t bytes_served = 0;
    uint64_t bytes_stored = 0;
    uint64_t entries = 0;
    uint64_t bytes = 0;
};

struct Hit {
    std::wstring path;     // blob on disk
    std::wstring headers;  // ready-to-replay header block (CRLF separated)
    uint64_t size = 0;
};

// L2 content store: one body file per URL, one small metadata file beside it.
//
// Layout under `root`:
//   b/<hh>/<hh>/<sha256(url)>          response body, decoded
//   m/<hh>/<hh>/<sha256(url)>.meta     url, headers, tier, timestamps
//
// The URL is the key. A hashed URL can never change content, so a game update
// simply produces new keys and the old ones age out through LRU.
//
// All calls arrive on the WebView2 UI thread; the lock exists only so the type
// stays safe if a caller ever moves work off it.
class AssetCache {
public:
    bool Open(const std::wstring& root, uint64_t cap_bytes);
    bool opened() const;

    // True when the entry may be served for this policy right now. A missing or
    // corrupt blob is dropped as a side effect.
    bool Lookup(const std::wstring& url, const Policy& policy, Hit* out);

    void Touch(const std::wstring& url);
    void Drop(const std::wstring& url);

    // Writes the body and metadata. `started_tick` is when the request that
    // produced this body began, so a slow older response cannot overwrite a
    // newer one that landed first.
    void Store(const std::wstring& url, const std::wstring& headers, const std::string& body,
               const Policy& policy, const std::string& cache_control, uint64_t started_tick);

    // The server answered 304: the stored bytes are still current.
    void MarkFresh(const std::wstring& url);

    void PurgeMutable();
    void PurgeAll();
    void Flush();

    void RecordHit(uint64_t bytes);
    void RecordMiss();

    Stats stats() const;
    std::wstring root() const;

private:
    struct Entry {
        std::wstring url;
        std::wstring path;
        std::wstring headers;
        uint64_t size = 0;
        int64_t fetched = 0;
        int64_t used = 0;
        Tier tier = Tier::Never;
        uint32_t touches = 0;
        // Runtime only: request start tick of the capture that produced this
        // body. Not persisted; disk entries load as 0.
        uint64_t started = 0;
    };

    bool LoadIndex();
    void WriteEntry(const Entry& entry);
    void RecomputeTotals();
    void EvictToFit();
    void DropLocked(const std::wstring& url);

    std::wstring BlobPath(const std::string& hash) const;
    std::wstring MetaPath(const std::string& hash) const;

    mutable std::mutex mutex_;
    std::wstring root_;
    uint64_t cap_ = 512ull << 20;
    bool opened_ = false;
    std::unordered_map<std::wstring, Entry> index_;
    Stats stats_;
};

} // namespace moo::cache
