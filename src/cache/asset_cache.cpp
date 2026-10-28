#include "cache/asset_cache.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

#include "cache/sha256.h"
#include "diag/log.h"

namespace moo::cache {
namespace {

namespace fs = std::filesystem;

int64_t Now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string HashOf(const std::wstring& url) {
    std::string hex;
    if (!Sha256Hex(url.data(), url.size() * sizeof(wchar_t), &hex)) return {};
    return hex;
}

std::string Escape(const std::wstring& value) {
    std::string out;
    out.reserve(value.size());
    for (const wchar_t wide : value) {
        switch (wide) {
        case L'\\': out += "\\\\"; break;
        case L'\n': out += "\\n"; break;
        case L'\r': out += "\\r"; break;
        default:
            // Header blocks and URLs are ASCII in practice; anything above the
            // ASCII range is written as '?' rather than mis-encoded.
            out.push_back(wide < 128 ? static_cast<char>(wide) : '?');
            break;
        }
    }
    return out;
}

std::wstring Unescape(const std::string& value) {
    std::wstring out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '\\' || i + 1 >= value.size()) {
            out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(value[i])));
            continue;
        }
        switch (value[++i]) {
        case 'n': out.push_back(L'\n'); break;
        case 'r': out.push_back(L'\r'); break;
        case '\\': out.push_back(L'\\'); break;
        default: out.push_back(static_cast<wchar_t>(value[i])); break;
        }
    }
    return out;
}

bool ReadWholeFile(const std::wstring& path, std::string* out) {
    std::ifstream file(fs::path(path), std::ios::binary);
    if (!file) return false;
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size < 0) return false;
    file.seekg(0, std::ios::beg);
    out->resize(static_cast<size_t>(size));
    if (size > 0) file.read(out->data(), size);
    return file.good() || file.eof();
}

bool WriteWholeFile(const std::wstring& path, const std::string& data) {
    const fs::path target(path);
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);

    const fs::path temp = target.wstring() + L".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        if (!data.empty()) file.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!file) return false;
    }

    if (!MoveFileExW(temp.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code remove_error;
        fs::remove(temp, remove_error);
        return false;
    }
    return true;
}

uint64_t FileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)) return UINT64_MAX;
    return (static_cast<uint64_t>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
}

} // namespace

bool AssetCache::Open(const std::wstring& root, uint64_t cap_bytes) {
    std::lock_guard lock(mutex_);

    root_ = root;
    cap_ = cap_bytes;

    std::error_code ec;
    fs::create_directories(fs::path(root_) / L"b", ec);
    fs::create_directories(fs::path(root_) / L"m", ec);
    if (ec) {
        moo::diag::Write(L"cache: cannot create %s (%d)", root_.c_str(), ec.value());
        return false;
    }

    opened_ = true;
    if (!LoadIndex()) return false;

    EvictToFit();
    moo::diag::Write(L"cache: %llu entries, %.1f MB in %s",
                     static_cast<unsigned long long>(stats_.entries),
                     static_cast<double>(stats_.bytes) / (1024.0 * 1024.0), root_.c_str());
    return true;
}

bool AssetCache::opened() const {
    std::lock_guard lock(mutex_);
    return opened_;
}

std::wstring AssetCache::root() const {
    std::lock_guard lock(mutex_);
    return root_;
}

std::wstring AssetCache::BlobPath(const std::string& hash) const {
    return (fs::path(root_) / L"b" / hash.substr(0, 2) / hash.substr(2, 2) / hash).wstring();
}

std::wstring AssetCache::MetaPath(const std::string& hash) const {
    return (fs::path(root_) / L"m" / hash.substr(0, 2) / hash.substr(2, 2) / (hash + ".meta"))
        .wstring();
}

bool AssetCache::LoadIndex() {
    index_.clear();

    std::error_code ec;
    fs::recursive_directory_iterator it(fs::path(root_) / L"m", ec);
    if (ec) return false;

    const fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        if (it->path().extension() != L".meta") continue;

        std::string text;
        if (!ReadWholeFile(it->path().wstring(), &text)) continue;

        Entry entry;
        std::string hash;
        size_t line_start = 0;
        while (line_start <= text.size()) {
            size_t line_end = text.find('\n', line_start);
            if (line_end == std::string::npos) line_end = text.size();
            const std::string line = text.substr(line_start, line_end - line_start);
            line_start = line_end + 1;

            const size_t equals = line.find('=');
            if (equals == std::string::npos) continue;
            const std::string key = line.substr(0, equals);
            const std::string value = line.substr(equals + 1);

            if (key == "url") entry.url = Unescape(value);
            else if (key == "headers") entry.headers = Unescape(value);
            else if (key == "size") entry.size = strtoull(value.c_str(), nullptr, 10);
            else if (key == "tier") entry.tier = static_cast<Tier>(atoi(value.c_str()));
            else if (key == "fetched") entry.fetched = _strtoi64(value.c_str(), nullptr, 10);
            else if (key == "used") entry.used = _strtoi64(value.c_str(), nullptr, 10);
            else if (key == "hash") hash = value;
        }

        if (entry.url.empty() || hash.size() != 64) {
            std::error_code remove_error;
            fs::remove(it->path(), remove_error);
            continue;
        }

        entry.path = BlobPath(hash);
        if (FileSize(entry.path) != entry.size) {
            std::error_code remove_error;
            fs::remove(entry.path, remove_error);
            fs::remove(it->path(), remove_error);
            continue;
        }

        index_.insert_or_assign(entry.url, std::move(entry));
    }

    RecomputeTotals();
    return true;
}

void AssetCache::RecomputeTotals() {
    stats_.entries = index_.size();
    stats_.bytes = 0;
    for (const auto& [url, entry] : index_) {
        (void)url;
        stats_.bytes += entry.size;
    }
}

bool AssetCache::Lookup(const std::wstring& url, const Policy& policy, Hit* out) {
    if (!out || policy.tier == Tier::Never) return false;
    std::lock_guard lock(mutex_);
    if (!opened_) return false;

    const auto found = index_.find(url);
    if (found == index_.end()) return false;

    Entry& entry = found->second;

    if (FileSize(entry.path) != entry.size) {
        DropLocked(url);
        return false;
    }

    out->path = entry.path;
    out->headers = entry.headers;
    out->size = entry.size;
    return true;
}

void AssetCache::Touch(const std::wstring& url) {
    std::lock_guard lock(mutex_);

    const auto found = index_.find(url);
    if (found == index_.end()) return;

    Entry& entry = found->second;
    entry.used = Now();
    ++entry.touches;

    // Persisting every hit would double the disk cost of a warm load. The
    // timestamps only drive LRU, so a periodic flush is enough.
    if (entry.touches % 64 == 0) WriteEntry(entry);
}

void AssetCache::Drop(const std::wstring& url) {
    std::lock_guard lock(mutex_);
    DropLocked(url);
}

void AssetCache::DropLocked(const std::wstring& url) {
    const auto found = index_.find(url);
    if (found == index_.end()) return;

    std::error_code ec;
    fs::remove(found->second.path, ec);
    fs::remove(MetaPath(HashOf(url)), ec);

    stats_.bytes -= std::min(stats_.bytes, found->second.size);
    index_.erase(found);
    stats_.entries = index_.size();
}

void AssetCache::Store(const std::wstring& url, const std::wstring& headers,
                       const std::string& body, const Policy& policy,
                       const std::string& cache_control, uint64_t started_tick) {
    if (body.empty() || policy.tier == Tier::Never) return;
    if (IsUncacheable(cache_control)) return;

    const std::string url_hash = HashOf(url);
    if (url_hash.empty()) return;

    std::lock_guard lock(mutex_);
    if (!opened_) return;

    const auto existing = index_.find(url);
    // A capture that started before the copy we already hold must not win, even
    // if its response arrived later.
    if (existing != index_.end() && started_tick != 0 &&
        existing->second.started > started_tick) {
        return;
    }

    Entry entry;
    entry.url = url;
    entry.path = BlobPath(url_hash);
    entry.headers = headers;
    entry.size = body.size();
    entry.fetched = Now();
    entry.used = entry.fetched;
    entry.tier = policy.tier;
    entry.started = started_tick;

    if (!WriteWholeFile(entry.path, body)) {
        moo::diag::Write(L"cache: write failed for %s", url.c_str());
        return;
    }

    WriteEntry(entry);

    if (existing != index_.end()) {
        stats_.bytes -= std::min(stats_.bytes, existing->second.size);
    }
    stats_.bytes += entry.size;
    stats_.stores += 1;
    stats_.bytes_stored += entry.size;

    index_.insert_or_assign(url, std::move(entry));
    stats_.entries = index_.size();

    EvictToFit();
}

void AssetCache::MarkFresh(const std::wstring& url) {
    std::lock_guard lock(mutex_);
    const auto found = index_.find(url);
    if (found == index_.end()) return;
    found->second.fetched = Now();
    ++stats_.revalidated;
    WriteEntry(found->second);
}

void AssetCache::WriteEntry(const Entry& entry) {
    const std::string url_hash = HashOf(entry.url);
    if (url_hash.empty()) return;

    std::string text;
    text += "v=1\n";
    text += "hash=" + url_hash + "\n";
    text += "tier=" + std::to_string(static_cast<int>(entry.tier)) + "\n";
    text += "size=" + std::to_string(entry.size) + "\n";
    text += "fetched=" + std::to_string(entry.fetched) + "\n";
    text += "used=" + std::to_string(entry.used) + "\n";
    text += "url=" + Escape(entry.url) + "\n";
    text += "headers=" + Escape(entry.headers) + "\n";

    WriteWholeFile(MetaPath(url_hash), text);
}

void AssetCache::EvictToFit() {
    if (stats_.bytes <= cap_) return;

    std::vector<Entry*> entries;
    entries.reserve(index_.size());
    for (auto& [url, entry] : index_) {
        (void)url;
        entries.push_back(&entry);
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry* a, const Entry* b) { return a->used < b->used; });

    const uint64_t target = cap_ - (cap_ / 10);
    for (Entry* entry : entries) {
        if (stats_.bytes <= target) break;

        std::error_code ec;
        fs::remove(entry->path, ec);
        fs::remove(MetaPath(HashOf(entry->url)), ec);

        stats_.bytes -= std::min(stats_.bytes, entry->size);
        stats_.evictions += 1;
        entry->size = 0;
    }

    std::erase_if(index_, [](const auto& pair) { return pair.second.size == 0; });
    stats_.entries = index_.size();
}

void AssetCache::PurgeMutable() {
    std::lock_guard lock(mutex_);

    for (auto it = index_.begin(); it != index_.end();) {
        if (it->second.tier == Tier::Immutable) {
            ++it;
            continue;
        }
        std::error_code ec;
        fs::remove(it->second.path, ec);
        fs::remove(MetaPath(HashOf(it->second.url)), ec);
        stats_.bytes -= std::min(stats_.bytes, it->second.size);
        it = index_.erase(it);
    }
    stats_.entries = index_.size();
    moo::diag::Write(L"cache: purged mutable entries, %llu left",
                     static_cast<unsigned long long>(stats_.entries));
}

void AssetCache::PurgeAll() {
    std::lock_guard lock(mutex_);

    index_.clear();
    std::error_code ec;
    fs::remove_all(fs::path(root_) / L"b", ec);
    fs::remove_all(fs::path(root_) / L"m", ec);
    fs::create_directories(fs::path(root_) / L"b", ec);
    fs::create_directories(fs::path(root_) / L"m", ec);
    stats_.bytes = 0;
    stats_.entries = 0;
    moo::diag::Write(L"cache: purged everything");
}

void AssetCache::Flush() {
    std::lock_guard lock(mutex_);
    for (auto& [url, entry] : index_) {
        (void)url;
        WriteEntry(entry);
    }
}

Stats AssetCache::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

void AssetCache::RecordHit(uint64_t bytes) {
    std::lock_guard lock(mutex_);
    ++stats_.hits;
    stats_.bytes_served += bytes;
}

void AssetCache::RecordMiss() {
    std::lock_guard lock(mutex_);
    ++stats_.misses;
}

} // namespace moo::cache
