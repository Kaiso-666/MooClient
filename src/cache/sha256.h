#pragma once

#include <cstddef>
#include <string>

namespace moo::cache {

// Lowercase hex SHA-256 of a buffer, using CNG (bcrypt). Returns false only when
// the algorithm cannot be opened, which on a supported Windows build means a
// broken system rather than bad input.
bool Sha256Hex(const void* data, size_t size, std::string* hex_out);

} // namespace moo::cache
