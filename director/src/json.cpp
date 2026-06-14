#include "director/json.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace br::director::json {

namespace {

// A tiny recursive-descent parser over [p, end). On error it sets `err` and
// returns false; callers bubble up immediately.
struct Parser {
    const char* p;
    const char* end;
    std::string& err;

    bool fail(const char* msg) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s at byte %lld", msg,
                      static_cast<long long>(p - begin_));
        err = buf;
        return false;
    }
    const char* begin_;

    void skip_ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }

    bool parse_value(Value& v) {
        skip_ws();
        if (p >= end) return fail("unexpected end of input");
        switch (*p) {
            case '{': return parse_object(v);
            case '[': return parse_array(v);
            case '"': return parse_string_value(v);
            case 't': case 'f': return parse_bool(v);
            case 'n': return parse_null(v);
            default:  return parse_number(v);
        }
    }

    bool parse_literal(const char* lit) {
        const size_t n = std::strlen(lit);
        if (static_cast<size_t>(end - p) < n || std::strncmp(p, lit, n) != 0) return fail("bad literal");
        p += n;
        return true;
    }
    bool parse_bool(Value& v) {
        if (*p == 't') { if (!parse_literal("true")) return false; v.type = Type::Bool; v.b = true; }
        else { if (!parse_literal("false")) return false; v.type = Type::Bool; v.b = false; }
        return true;
    }
    bool parse_null(Value& v) {
        if (!parse_literal("null")) return false;
        v.type = Type::Null;
        return true;
    }

    bool parse_number(Value& v) {
        const char* start = p;
        if (p < end && *p == '-') ++p;
        while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '+' || *p == '-')) ++p;
        if (p == start) return fail("invalid value");
        const std::string tok(start, static_cast<size_t>(p - start));
        char* endp = nullptr;
        const double d = std::strtod(tok.c_str(), &endp);
        if (endp != tok.c_str() + tok.size()) return fail("invalid number");
        v.type = Type::Number;
        v.num = d;
        return true;
    }

    // Reads a JSON string body (cursor must be on the opening quote) into `out`.
    bool parse_string(std::string& out) {
        if (*p != '"') return fail("expected string");
        ++p;
        out.clear();
        while (p < end) {
            const char c = *p++;
            if (c == '"') return true;
            if (c == '\\') {
                if (p >= end) return fail("unterminated escape");
                const char e = *p++;
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        if (end - p < 4) return fail("bad \\u escape");
                        unsigned cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            const char h = *p++;
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                            else return fail("bad hex in \\u escape");
                        }
                        // Encode the BMP code point as UTF-8 (surrogate pairs collapse
                        // to the replacement char — fine for caption sanitisation).
                        if (cp < 0x80) {
                            out.push_back(static_cast<char>(cp));
                        } else if (cp < 0x800) {
                            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        } else {
                            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        }
                        break;
                    }
                    default: return fail("invalid escape");
                }
            } else {
                out.push_back(c);
            }
        }
        return fail("unterminated string");
    }
    bool parse_string_value(Value& v) {
        v.type = Type::String;
        return parse_string(v.str);
    }

    bool parse_array(Value& v) {
        v.type = Type::Array;
        ++p;  // consume '['
        skip_ws();
        if (p < end && *p == ']') { ++p; return true; }
        for (;;) {
            Value el;
            if (!parse_value(el)) return false;
            v.arr.push_back(std::move(el));
            skip_ws();
            if (p >= end) return fail("unterminated array");
            if (*p == ',') { ++p; continue; }
            if (*p == ']') { ++p; return true; }
            return fail("expected ',' or ']'");
        }
    }

    bool parse_object(Value& v) {
        v.type = Type::Object;
        ++p;  // consume '{'
        skip_ws();
        if (p < end && *p == '}') { ++p; return true; }
        for (;;) {
            skip_ws();
            if (p >= end || *p != '"') return fail("expected object key");
            std::string key;
            if (!parse_string(key)) return false;
            skip_ws();
            if (p >= end || *p != ':') return fail("expected ':'");
            ++p;
            Value val;
            if (!parse_value(val)) return false;
            v.obj[key] = std::move(val);
            skip_ws();
            if (p >= end) return fail("unterminated object");
            if (*p == ',') { ++p; continue; }
            if (*p == '}') { ++p; return true; }
            return fail("expected ',' or '}'");
        }
    }
};

}  // namespace

bool parse(const std::string& text, Value& out, std::string& err) {
    Parser ps{ text.data(), text.data() + text.size(), err, text.data() };
    out = Value{};
    if (!ps.parse_value(out)) return false;
    ps.skip_ws();
    if (ps.p != ps.end) { ps.fail("trailing characters after JSON value"); return false; }
    return true;
}

}  // namespace br::director::json
