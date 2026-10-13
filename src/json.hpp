#pragma once

#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <cctype>
#include <cmath>
#include <algorithm>

namespace chokidar {

enum class JsonType {
    Null,
    Bool,
    Number,
    String,
    Array,
    Object
};

class JsonValue {
public:
    JsonType type = JsonType::Null;
    bool bool_val = false;
    double num_val = 0.0;
    int64_t int_val = 0;
    std::string str_val;
    std::vector<JsonValue> arr_val;
    std::vector<std::pair<std::string, JsonValue>> obj_val;

    JsonValue() : type(JsonType::Null) {}
    JsonValue(std::nullptr_t) : type(JsonType::Null) {}
    JsonValue(bool b) : type(JsonType::Bool), bool_val(b) {}
    JsonValue(int n) : type(JsonType::Number), num_val(n), int_val(n) {}
    JsonValue(int64_t n) : type(JsonType::Number), num_val(static_cast<double>(n)), int_val(n) {}
    JsonValue(double n) : type(JsonType::Number), num_val(n), int_val(static_cast<int64_t>(n)) {}
    JsonValue(const char* s) : type(JsonType::String), str_val(s ? s : "") {}
    JsonValue(const std::string& s) : type(JsonType::String), str_val(s) {}

    static JsonValue object() {
        JsonValue v;
        v.type = JsonType::Object;
        return v;
    }

    static JsonValue array() {
        JsonValue v;
        v.type = JsonType::Array;
        return v;
    }

    bool is_null() const { return type == JsonType::Null; }
    bool is_bool() const { return type == JsonType::Bool; }
    bool is_number() const { return type == JsonType::Number; }
    bool is_string() const { return type == JsonType::String; }
    bool is_array() const { return type == JsonType::Array; }
    bool is_object() const { return type == JsonType::Object; }

    const std::string& as_string(const std::string& def = "") const {
        return is_string() ? str_val : def;
    }

    int64_t as_int(int64_t def = 0) const {
        return is_number() ? int_val : def;
    }

    double as_double(double def = 0.0) const {
        return is_number() ? num_val : def;
    }

    bool as_bool(bool def = false) const {
        return is_bool() ? bool_val : def;
    }

    size_t size() const {
        if (is_array()) return arr_val.size();
        if (is_object()) return obj_val.size();
        return 0;
    }

    bool contains(const std::string& key) const {
        if (!is_object()) return false;
        for (const auto& kv : obj_val) {
            if (kv.first == key) return true;
        }
        return false;
    }

    const JsonValue& operator[](const std::string& key) const {
        static const JsonValue null_val;
        if (!is_object()) return null_val;
        for (const auto& kv : obj_val) {
            if (kv.first == key) return kv.second;
        }
        return null_val;
    }

    JsonValue& operator[](const std::string& key) {
        if (!is_object()) {
            type = JsonType::Object;
            obj_val.clear();
        }
        for (auto& kv : obj_val) {
            if (kv.first == key) return kv.second;
        }
        obj_val.emplace_back(key, JsonValue());
        return obj_val.back().second;
    }

    const JsonValue& operator[](size_t index) const {
        static const JsonValue null_val;
        if (!is_array() || index >= arr_val.size()) return null_val;
        return arr_val[index];
    }

    JsonValue& operator[](size_t index) {
        if (!is_array()) {
            type = JsonType::Array;
            arr_val.clear();
        }
        if (index >= arr_val.size()) {
            arr_val.resize(index + 1);
        }
        return arr_val[index];
    }

    void push_back(const JsonValue& v) {
        if (!is_array()) {
            type = JsonType::Array;
            arr_val.clear();
        }
        arr_val.push_back(v);
    }

    static std::string escape_string(const std::string& s) {
        std::string out = "\"";
        out.reserve(s.size() + 16);
        for (unsigned char c : s) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    } else {
                        out += static_cast<char>(c);
                    }
                    break;
            }
        }
        out += '"';
        return out;
    }

    std::string dump() const {
        switch (type) {
            case JsonType::Null: return "null";
            case JsonType::Bool: return bool_val ? "true" : "false";
            case JsonType::Number: {
                if (num_val == static_cast<double>(int_val)) {
                    return std::to_string(int_val);
                }
                char buf[32];
                snprintf(buf, sizeof(buf), "%.17g", num_val);
                return buf;
            }
            case JsonType::String: return escape_string(str_val);
            case JsonType::Array: {
                std::string res = "[";
                for (size_t i = 0; i < arr_val.size(); ++i) {
                    if (i > 0) res += ",";
                    res += arr_val[i].dump();
                }
                res += "]";
                return res;
            }
            case JsonType::Object: {
                std::string res = "{";
                for (size_t i = 0; i < obj_val.size(); ++i) {
                    if (i > 0) res += ",";
                    res += escape_string(obj_val[i].first);
                    res += ":";
                    res += obj_val[i].second.dump();
                }
                res += "}";
                return res;
            }
        }
        return "null";
    }

    static JsonValue parse(const std::string& str, std::string* err = nullptr) {
        size_t idx = 0;
        skip_ws(str, idx);
        try {
            JsonValue val = parse_val(str, idx);
            skip_ws(str, idx);
            if (idx != str.size()) {
                throw std::runtime_error("Unexpected trailing data after JSON value");
            }
            return val;
        } catch (const std::exception& e) {
            if (err) *err = e.what();
            return JsonValue();
        }
    }

private:
    static void skip_ws(const std::string& s, size_t& i) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) {
            i++;
        }
    }

    static JsonValue parse_val(const std::string& s, size_t& i) {
        skip_ws(s, i);
        if (i >= s.size()) throw std::runtime_error("Unexpected EOF");
        char c = s[i];
        if (c == '{') return parse_obj(s, i);
        if (c == '[') return parse_arr(s, i);
        if (c == '"') return parse_str_val(s, i);
        if (c == 't' || c == 'f') return parse_bool(s, i);
        if (c == 'n') return parse_null(s, i);
        if (c == '-' || (c >= '0' && c <= '9')) return parse_num(s, i);
        throw std::runtime_error(std::string("Unexpected char: ") + c);
    }

    static JsonValue parse_obj(const std::string& s, size_t& i) {
        i++; // '{'
        JsonValue val = JsonValue::object();
        skip_ws(s, i);
        if (i < s.size() && s[i] == '}') {
            i++;
            return val;
        }
        while (i < s.size()) {
            skip_ws(s, i);
            if (i >= s.size() || s[i] != '"') throw std::runtime_error("Expected key in object");
            std::string key = parse_str(s, i);
            skip_ws(s, i);
            if (i >= s.size() || s[i] != ':') throw std::runtime_error("Expected ':' after key");
            i++; // ':'
            JsonValue v = parse_val(s, i);
            val.obj_val.emplace_back(std::move(key), std::move(v));
            skip_ws(s, i);
            if (i < s.size() && s[i] == '}') {
                i++;
                return val;
            }
            if (i >= s.size() || s[i] != ',') throw std::runtime_error("Expected ',' or '}'");
            i++; // ','
        }
        throw std::runtime_error("Unterminated object");
    }

    static JsonValue parse_arr(const std::string& s, size_t& i) {
        i++; // '['
        JsonValue val = JsonValue::array();
        skip_ws(s, i);
        if (i < s.size() && s[i] == ']') {
            i++;
            return val;
        }
        while (i < s.size()) {
            val.push_back(parse_val(s, i));
            skip_ws(s, i);
            if (i < s.size() && s[i] == ']') {
                i++;
                return val;
            }
            if (i >= s.size() || s[i] != ',') throw std::runtime_error("Expected ',' or ']'");
            i++; // ','
        }
        throw std::runtime_error("Unterminated array");
    }

    static std::string parse_str(const std::string& s, size_t& i) {
        i++; // '"'
        std::string res;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return res;
            if (c == '\\') {
                if (i >= s.size()) throw std::runtime_error("Unterminated string escape");
                char e = s[i++];
                switch (e) {
                    case '"': res += '"'; break;
                    case '\\': res += '\\'; break;
                    case '/': res += '/'; break;
                    case 'b': res += '\b'; break;
                    case 'f': res += '\f'; break;
                    case 'n': res += '\n'; break;
                    case 'r': res += '\r'; break;
                    case 't': res += '\t'; break;
                    case 'u': {
                        if (i + 4 > s.size()) throw std::runtime_error("Bad unicode escape");
                        uint32_t codepoint = 0;
                        for (int k = 0; k < 4; ++k) {
                            char h = s[i++];
                            codepoint <<= 4;
                            if (h >= '0' && h <= '9') codepoint |= (h - '0');
                            else if (h >= 'a' && h <= 'f') codepoint |= (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') codepoint |= (h - 'A' + 10);
                            else throw std::runtime_error("Bad hex in unicode escape");
                        }
                        if (codepoint <= 0x7F) {
                            res += static_cast<char>(codepoint);
                        } else if (codepoint <= 0x7FF) {
                            res += static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F));
                            res += static_cast<char>(0x80 | (codepoint & 0x3F));
                        } else {
                            res += static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F));
                            res += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                            res += static_cast<char>(0x80 | (codepoint & 0x3F));
                        }
                        break;
                    }
                    default: res += e; break;
                }
            } else {
                res += c;
            }
        }
        throw std::runtime_error("Unterminated string");
    }

    static JsonValue parse_str_val(const std::string& s, size_t& i) {
        return JsonValue(parse_str(s, i));
    }

    static JsonValue parse_bool(const std::string& s, size_t& i) {
        if (s.compare(i, 4, "true") == 0) {
            i += 4;
            return JsonValue(true);
        }
        if (s.compare(i, 5, "false") == 0) {
            i += 5;
            return JsonValue(false);
        }
        throw std::runtime_error("Bad boolean literal");
    }

    static JsonValue parse_null(const std::string& s, size_t& i) {
        if (s.compare(i, 4, "null") == 0) {
            i += 4;
            return JsonValue(nullptr);
        }
        throw std::runtime_error("Bad null literal");
    }

    static JsonValue parse_num(const std::string& s, size_t& i) {
        size_t start = i;
        if (s[i] == '-') i++;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
        bool is_float = false;
        if (i < s.size() && s[i] == '.') {
            is_float = true;
            i++;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
        }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            is_float = true;
            i++;
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) i++;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
        }
        std::string num_str = s.substr(start, i - start);
        if (is_float) {
            double d = std::stod(num_str);
            return JsonValue(d);
        } else {
            int64_t v = std::stoll(num_str);
            return JsonValue(v);
        }
    }
};

} // namespace chokidar
