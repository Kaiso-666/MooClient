#include "update/json.h"

#include <cstdlib>
#include <cstring>

namespace moo::update {
namespace {

constexpr int kMaxDepth = 64;

struct Parser {
    const char* cursor = nullptr;
    const char* end = nullptr;
    bool ok = true;
    int depth = 0;

    void SkipSpace() {
        while (cursor < end) {
            const char c = *cursor;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++cursor;
            } else {
                break;
            }
        }
    }

    bool Consume(char expected) {
        SkipSpace();
        if (cursor >= end || *cursor != expected) return false;
        ++cursor;
        return true;
    }

    bool ParseString(std::string* out) {
        SkipSpace();
        if (cursor >= end || *cursor != '"') return false;
        ++cursor;

        out->clear();
        while (cursor < end) {
            const unsigned char c = static_cast<unsigned char>(*cursor++);
            if (c == '"') return true;
            if (c != '\\') {
                out->push_back(static_cast<char>(c));
                continue;
            }
            if (cursor >= end) return false;
            const char escape = *cursor++;
            switch (escape) {
            case '"': out->push_back('"'); break;
            case '\\': out->push_back('\\'); break;
            case '/': out->push_back('/'); break;
            case 'b': out->push_back('\b'); break;
            case 'f': out->push_back('\f'); break;
            case 'n': out->push_back('\n'); break;
            case 'r': out->push_back('\r'); break;
            case 't': out->push_back('\t'); break;
            case 'u': {
                if (end - cursor < 4) return false;
                unsigned int value = 0;
                for (int i = 0; i < 4; ++i) {
                    const char digit = *cursor++;
                    value <<= 4;
                    if (digit >= '0' && digit <= '9') value |= static_cast<unsigned>(digit - '0');
                    else if (digit >= 'a' && digit <= 'f') value |= static_cast<unsigned>(digit - 'a' + 10);
                    else if (digit >= 'A' && digit <= 'F') value |= static_cast<unsigned>(digit - 'A' + 10);
                    else return false;
                }
                // Surrogate pair: combine with the following \uXXXX.
                if (value >= 0xD800 && value <= 0xDBFF && end - cursor >= 6 && cursor[0] == '\\' &&
                    cursor[1] == 'u') {
                    cursor += 2;
                    unsigned int low = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char digit = *cursor++;
                        low <<= 4;
                        if (digit >= '0' && digit <= '9') low |= static_cast<unsigned>(digit - '0');
                        else if (digit >= 'a' && digit <= 'f') low |= static_cast<unsigned>(digit - 'a' + 10);
                        else if (digit >= 'A' && digit <= 'F') low |= static_cast<unsigned>(digit - 'A' + 10);
                        else return false;
                    }
                    if (low >= 0xDC00 && low <= 0xDFFF) {
                        value = 0x10000 + ((value - 0xD800) << 10) + (low - 0xDC00);
                    }
                }
                AppendUtf8(out, value);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    bool ParseValue(Json* out) {
        if (++depth > kMaxDepth) return false;

        SkipSpace();
        if (cursor >= end) return false;

        bool ok_here = true;
        switch (*cursor) {
        case '{': {
            out->type = Json::Type::Object;
            ++cursor;
            SkipSpace();
            if (cursor < end && *cursor == '}') {
                ++cursor;
                break;
            }
            for (;;) {
                std::string key;
                if (!ParseString(&key)) { ok_here = false; break; }
                if (!Consume(':')) { ok_here = false; break; }
                Json value;
                if (!ParseValue(&value)) { ok_here = false; break; }
                out->members.emplace_back(std::move(key), std::move(value));
                SkipSpace();
                if (cursor < end && *cursor == ',') { ++cursor; continue; }
                if (cursor < end && *cursor == '}') { ++cursor; break; }
                ok_here = false;
                break;
            }
            break;
        }
        case '[': {
            out->type = Json::Type::Array;
            ++cursor;
            SkipSpace();
            if (cursor < end && *cursor == ']') {
                ++cursor;
                break;
            }
            for (;;) {
                Json value;
                if (!ParseValue(&value)) { ok_here = false; break; }
                out->items.push_back(std::move(value));
                SkipSpace();
                if (cursor < end && *cursor == ',') { ++cursor; continue; }
                if (cursor < end && *cursor == ']') { ++cursor; break; }
                ok_here = false;
                break;
            }
            break;
        }
        case '"':
            out->type = Json::Type::String;
            ok_here = ParseString(&out->text);
            break;
        case 't':
            if (end - cursor >= 4 && std::memcmp(cursor, "true", 4) == 0) {
                out->type = Json::Type::Bool;
                out->boolean = true;
                cursor += 4;
            } else {
                ok_here = false;
            }
            break;
        case 'f':
            if (end - cursor >= 5 && std::memcmp(cursor, "false", 5) == 0) {
                out->type = Json::Type::Bool;
                out->boolean = false;
                cursor += 5;
            } else {
                ok_here = false;
            }
            break;
        case 'n':
            if (end - cursor >= 4 && std::memcmp(cursor, "null", 4) == 0) {
                out->type = Json::Type::Null;
                cursor += 4;
            } else {
                ok_here = false;
            }
            break;
        default: {
            char* stop = nullptr;
            const double value = std::strtod(cursor, &stop);
            if (stop == cursor || stop > end) {
                ok_here = false;
            } else {
                out->type = Json::Type::Number;
                out->number = value;
                cursor = stop;
            }
            break;
        }
        }

        --depth;
        return ok_here;
    }
};

} // namespace

void AppendUtf8(std::string* out, unsigned int code_point) {
    if (code_point <= 0x7F) {
        out->push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7FF) {
        out->push_back(static_cast<char>(0xC0 | (code_point >> 6)));
        out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point <= 0xFFFF) {
        out->push_back(static_cast<char>(0xE0 | (code_point >> 12)));
        out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
        out->push_back(static_cast<char>(0xF0 | (code_point >> 18)));
        out->push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
}

const Json* Json::Find(const char* key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& [name, value] : members) {
        if (name == key) return &value;
    }
    return nullptr;
}

const std::string* Json::AsString() const {
    return type == Type::String ? &text : nullptr;
}

bool ParseJson(const std::string& text, Json* out) {
    if (!out) return false;
    *out = Json{};

    Parser parser;
    parser.cursor = text.data();
    parser.end = text.data() + text.size();
    if (!parser.ParseValue(out)) return false;
    parser.SkipSpace();
    return parser.cursor == parser.end;
}

} // namespace moo::update
