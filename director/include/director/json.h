#pragma once
//
// director/json.h — a minimal, dependency-free JSON reader (M11).
//
// Just enough to parse the Director's small directive objects and KEEL's
// OpenAI-compatible response envelope. NOT a general-purpose library; it exists so
// the Director can validate untrusted model output without pulling in a JSON
// dependency (Catch2 + stb stay the only third-party libs).
//
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace br::director::json {

enum class Type { Null, Bool, Number, String, Array, Object };

struct Value;
using Array = std::vector<Value>;
using Object = std::map<std::string, Value>;

struct Value {
    Type type = Type::Null;
    bool b = false;
    double num = 0.0;
    std::string str;
    Array arr;
    Object obj;

    bool is_object() const { return type == Type::Object; }
    bool is_array() const { return type == Type::Array; }
    bool is_string() const { return type == Type::String; }
    bool is_number() const { return type == Type::Number; }

    // Object field by key (nullptr if not an object or key absent).
    const Value* find(const std::string& key) const {
        if (type != Type::Object) return nullptr;
        const auto it = obj.find(key);
        return it == obj.end() ? nullptr : &it->second;
    }
    // Array element by index (nullptr if not an array or out of range).
    const Value* at(std::size_t i) const {
        if (type != Type::Array || i >= arr.size()) return nullptr;
        return &arr[i];
    }
};

// Parse `text` as a single JSON value. Returns true + fills `out` on success;
// false + sets `err` (with a byte offset) on malformed input. Total: rejects
// trailing garbage, unterminated strings/containers, and bad escapes.
bool parse(const std::string& text, Value& out, std::string& err);

// Escape `s` as the body of a JSON string (WITHOUT surrounding quotes): handles
// " \ and control bytes; other bytes pass through. For building request bodies.
std::string escape(const std::string& s);

}  // namespace br::director::json
