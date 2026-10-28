#pragma once

#include <string>
#include <utility>
#include <vector>

namespace moo::update {

// Just enough JSON to read the GitHub release endpoint. Not a general-purpose
// library: no number precision guarantees beyond what we need, no duplicate-key
// handling, no comments.
struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> members;

    const Json* Find(const char* key) const;
    const std::string* AsString() const;
    bool IsArray() const { return type == Type::Array; }
};

// Returns false on malformed input. Input is UTF-8.
bool ParseJson(const std::string& text, Json* out);

// Appends a code point as UTF-8.
void AppendUtf8(std::string* out, unsigned int code_point);

} // namespace moo::update
