// Small JSON reader for the messages clients send (commands, subscriptions).
// Recursive descent, depth limited, no dependencies. Non-ASCII characters in strings
// become '?' because EuroScope only handles ANSI text.
#pragma once

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace jsonr {

struct Value {
    enum Type { Null, Bool, Number, String, Array, Object };
    Type type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Value> a;
    std::vector<std::pair<std::string, Value>> o;

    const Value* Get(const char* key) const {
        if (type != Object) return nullptr;
        for (const auto& kv : o)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    bool Has(const char* key) const { return Get(key) != nullptr; }
    std::string GetString(const char* key, const std::string& def = std::string()) const {
        const Value* v = Get(key);
        return v && v->type == String ? v->s : def;
    }
    bool GetNumber(const char* key, double* out) const {
        const Value* v = Get(key);
        if (!v) return false;
        if (v->type == Number) { *out = v->n; return true; }
        if (v->type == String && !v->s.empty()) {  // accept "7000" as well as 7000
            char* end = nullptr;
            double d = std::strtod(v->s.c_str(), &end);
            if (end && *end == '\0') { *out = d; return true; }
        }
        return false;
    }
    bool GetBool(const char* key, bool def = false) const {
        const Value* v = Get(key);
        return v && v->type == Bool ? v->b : def;
    }
};

namespace detail {

class Parser {
public:
    explicit Parser(const std::string& text) : p_(text.c_str()), end_(text.c_str() + text.size()) {}

    bool Run(Value& out, std::string* err) {
        bool ok = ParseValue(out, 0);
        SkipWs();
        if (ok && p_ != end_) { ok = false; error_ = "trailing characters"; }
        if (!ok && err) *err = error_.empty() ? "invalid JSON" : error_;
        return ok;
    }

private:
    static const int kMaxDepth = 32;

    void SkipWs() { while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_; }
    bool Fail(const char* msg) { if (error_.empty()) error_ = msg; return false; }
    bool Literal(const char* lit) {
        const char* q = p_;
        for (const char* l = lit; *l; ++l, ++q)
            if (q >= end_ || *q != *l) return false;
        p_ = q;
        return true;
    }

    bool ParseValue(Value& v, int depth) {
        if (depth > kMaxDepth) return Fail("nested too deeply");
        SkipWs();
        if (p_ >= end_) return Fail("unexpected end");
        char c = *p_;
        if (c == '{') return ParseObject(v, depth);
        if (c == '[') return ParseArray(v, depth);
        if (c == '"') { v.type = Value::String; return ParseString(v.s); }
        if (c == 't' && Literal("true")) { v.type = Value::Bool; v.b = true; return true; }
        if (c == 'f' && Literal("false")) { v.type = Value::Bool; v.b = false; return true; }
        if (c == 'n' && Literal("null")) { v.type = Value::Null; return true; }
        if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber(v);
        return Fail("unexpected character");
    }

    bool ParseNumber(Value& v) {
        const char* start = p_;
        if (p_ < end_ && *p_ == '-') ++p_;
        while (p_ < end_ && ((*p_ >= '0' && *p_ <= '9') || *p_ == '.' || *p_ == 'e' || *p_ == 'E' || *p_ == '+' || *p_ == '-')) ++p_;
        std::string num(start, p_);
        char* e = nullptr;
        v.n = std::strtod(num.c_str(), &e);
        if (num.empty() || !e || *e != '\0') return Fail("invalid number");
        v.type = Value::Number;
        return true;
    }

    static int Hex(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    bool ParseString(std::string& out) {
        ++p_;  // opening quote
        while (p_ < end_) {
            unsigned char c = static_cast<unsigned char>(*p_++);
            if (c == '"') return true;
            if (c < 0x20) return Fail("control character in string");
            if (c != '\\') {
                if (c < 0x80) out += char(c);
                else if ((c & 0xC0) != 0x80) out += '?';  // one '?' per UTF-8 sequence
                continue;
            }
            if (p_ >= end_) break;
            char e = *p_++;
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
                    if (end_ - p_ < 4) return Fail("bad \\u escape");
                    int cp = 0;
                    for (int i = 0; i < 4; ++i) {
                        int h = Hex(p_[i]);
                        if (h < 0) return Fail("bad \\u escape");
                        cp = cp * 16 + h;
                    }
                    p_ += 4;
                    if (cp >= 0xDC00 && cp <= 0xDFFF) break;  // low surrogate: already emitted '?'
                    out += cp < 0x80 ? char(cp) : '?';
                    break;
                }
                default: return Fail("bad escape");
            }
        }
        return Fail("unterminated string");
    }

    bool ParseArray(Value& v, int depth) {
        ++p_;
        v.type = Value::Array;
        SkipWs();
        if (p_ < end_ && *p_ == ']') { ++p_; return true; }
        for (;;) {
            v.a.emplace_back();
            if (!ParseValue(v.a.back(), depth + 1)) return false;
            SkipWs();
            if (p_ < end_ && *p_ == ',') { ++p_; continue; }
            if (p_ < end_ && *p_ == ']') { ++p_; return true; }
            return Fail("expected , or ]");
        }
    }

    bool ParseObject(Value& v, int depth) {
        ++p_;
        v.type = Value::Object;
        SkipWs();
        if (p_ < end_ && *p_ == '}') { ++p_; return true; }
        for (;;) {
            SkipWs();
            if (p_ >= end_ || *p_ != '"') return Fail("expected key");
            std::string key;
            if (!ParseString(key)) return false;
            SkipWs();
            if (p_ >= end_ || *p_ != ':') return Fail("expected :");
            ++p_;
            v.o.emplace_back(std::move(key), Value());
            if (!ParseValue(v.o.back().second, depth + 1)) return false;
            SkipWs();
            if (p_ < end_ && *p_ == ',') { ++p_; continue; }
            if (p_ < end_ && *p_ == '}') { ++p_; return true; }
            return Fail("expected , or }");
        }
    }

    const char* p_;
    const char* end_;
    std::string error_;
};

}  // namespace detail

inline bool Parse(const std::string& text, Value& out, std::string* error = nullptr) {
    out = Value();
    return detail::Parser(text).Run(out, error);
}

}  // namespace jsonr
