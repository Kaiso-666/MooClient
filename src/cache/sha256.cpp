#include "cache/sha256.h"

#include <windows.h>

#include <bcrypt.h>

namespace moo::cache {
namespace {

constexpr char kHex[] = "0123456789abcdef";

} // namespace

bool Sha256Hex(const void* data, size_t size, std::string* hex_out) {
    if (!hex_out) return false;

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        return false;
    }

    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    if (ok) {
        // BCryptHashData takes a non-const buffer pointer but never writes to it.
        auto* bytes = static_cast<PUCHAR>(const_cast<void*>(data));
        ok = BCryptHashData(hash, bytes, static_cast<ULONG>(size), 0) >= 0;
    }

    unsigned char digest[32]{};
    if (ok) ok = BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;

    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return false;

    hex_out->clear();
    hex_out->reserve(64);
    for (const unsigned char byte : digest) {
        hex_out->push_back(kHex[byte >> 4]);
        hex_out->push_back(kHex[byte & 0x0F]);
    }
    return true;
}

} // namespace moo::cache
