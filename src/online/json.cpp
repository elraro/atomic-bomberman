#include "online/json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ab::online {

namespace {

constexpr int kMaxDepth = 32;
constexpr std::size_t kMaxText = 1u << 20;

struct Parser {
    const std::string& s;
    std::size_t i = 0;

    void space() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }
    bool literal(const char* word) {
        std::size_t n = 0;
        while (word[n] != 0) ++n;
        if (s.compare(i, n, word) != 0) return false;
        i += n;
        return true;
    }
    static void utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(unsigned& v) {
        if (i + 4 > s.size()) return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string& out) {
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        while (i < s.size()) {
            const char c = s[i++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size()) return false;
            const char e = s[i++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00) {  // the first half of a pair
                        unsigned low = 0;
                        if (i + 2 > s.size() || s[i] != '\\' || s[i + 1] != 'u') return false;
                        i += 2;
                        if (!hex4(low) || low < 0xDC00 || low > 0xDFFF) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return false;
                    }
                    utf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }
    bool value(Json& out, int depth) {
        if (depth > kMaxDepth) return false;
        space();
        if (i >= s.size()) return false;
        const char c = s[i];
        if (c == '{') {
            ++i;
            out = Json::object();
            space();
            if (i < s.size() && s[i] == '}') {
                ++i;
                return true;
            }
            while (true) {
                space();
                std::string key;
                if (!string(key)) return false;
                space();
                if (i >= s.size() || s[i++] != ':') return false;
                Json member;
                if (!value(member, depth + 1)) return false;
                out.set(key, std::move(member));
                space();
                if (i >= s.size()) return false;
                if (s[i] == ',') {
                    ++i;
                    continue;
                }
                if (s[i] == '}') {
                    ++i;
                    return true;
                }
                return false;
            }
        }
        if (c == '[') {
            ++i;
            out = Json::array();
            space();
            if (i < s.size() && s[i] == ']') {
                ++i;
                return true;
            }
            while (true) {
                Json item;
                if (!value(item, depth + 1)) return false;
                out.push(std::move(item));
                space();
                if (i >= s.size()) return false;
                if (s[i] == ',') {
                    ++i;
                    continue;
                }
                if (s[i] == ']') {
                    ++i;
                    return true;
                }
                return false;
            }
        }
        if (c == '"') {
            std::string text;
            if (!string(text)) return false;
            out = Json(std::move(text));
            return true;
        }
        if (literal("true")) return out = Json(true), true;
        if (literal("false")) return out = Json(false), true;
        if (literal("null")) return out = Json(), true;
        // A number: checked by hand, then read by the library.
        const std::size_t start = i;
        if (i < s.size() && s[i] == '-') ++i;
        const std::size_t digits = i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
        if (i == digits || (s[digits] == '0' && i - digits > 1)) return false;  // no digits, or a leading zero
        if (i < s.size() && s[i] == '.') {
            ++i;
            const std::size_t frac = i;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
            if (i == frac) return false;
        }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            ++i;
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
            const std::size_t exp = i;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
            if (i == exp) return false;
        }
        const double v = std::strtod(s.substr(start, i - start).c_str(), nullptr);
        if (!std::isfinite(v)) return false;
        out = Json(v);
        return true;
    }
};

void dumpString(std::string& out, const std::string& s) {
    out += '"';
    for (const char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

void dumpValue(std::string& out, const Json& j) {
    switch (j.type()) {
        case Json::Type::Null: out += "null"; break;
        case Json::Type::Bool: out += j.boolean() ? "true" : "false"; break;
        case Json::Type::Number: {
            const double v = j.number();
            char buf[40];
            if (v == std::floor(v) && std::fabs(v) < 9.0e15) std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v));
            else std::snprintf(buf, sizeof buf, "%.17g", v);
            out += buf;
            break;
        }
        case Json::Type::String: dumpString(out, j.str()); break;
        case Json::Type::Array: {
            out += '[';
            bool first = true;
            for (const Json& item : j.items()) {
                if (!first) out += ',';
                first = false;
                dumpValue(out, item);
            }
            out += ']';
            break;
        }
        case Json::Type::Object: out += j.dump(); break;
    }
}

}  // namespace

bool Json::parse(const std::string& text, Json& out) {
    if (text.size() > kMaxText) return false;
    Parser p{text};
    Json value;
    if (!p.value(value, 0)) return false;
    p.space();
    if (p.i != text.size()) return false;
    out = std::move(value);
    return true;
}

std::string Json::dump() const {
    std::string out;
    if (type_ != Type::Object) {
        dumpValue(out, *this);
        return out;
    }
    out += '{';
    bool first = true;
    for (const auto& [key, value] : members_) {
        if (!first) out += ',';
        first = false;
        dumpString(out, key);
        out += ':';
        dumpValue(out, value);
    }
    out += '}';
    return out;
}

const Json& Json::operator[](const std::string& key) const {
    static const Json kNull;
    const auto it = members_.find(key);
    return it == members_.end() ? kNull : it->second;
}

Json& Json::set(const std::string& key, Json value) {
    type_ = Type::Object;
    members_[key] = std::move(value);
    return *this;
}

Json& Json::push(Json value) {
    type_ = Type::Array;
    items_.push_back(std::move(value));
    return *this;
}

}  // namespace ab::online
