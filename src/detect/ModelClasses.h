#pragma once
#include <cctype>
#include <map>
#include <string>
#include <utility>
#include <vector>

struct ModelClassInfo {
    bool loaded = false;
    std::string path;
    std::vector<std::pair<int, std::string>> classes;
};

namespace model_classes {
// ONNX exporters commonly store names as a Python dict or a JSON dict/list.
// Parse data only; never evaluate model metadata as code.
class NamesParser {
public:
    explicit NamesParser(const std::string& text) : text_(text) {}
    std::map<int, std::string> Parse() {
        std::map<int, std::string> result;
        Space();
        const char open = Take();
        if (open != '{' && open != '[') return {};
        const char close = open == '{' ? '}' : ']';
        Space();
        if (Peek() == close) { Take(); return {}; }
        for (int index = 0; index < 10000; ++index) {
            Space();
            int id = index;
            if (open == '{') {
                std::string key;
                if (Peek() == '\'' || Peek() == '"') {
                    if (!Quoted(key)) return {};
                } else {
                    while (std::isdigit(static_cast<unsigned char>(Peek()))) key += Take();
                }
                id = Count(key);
                if (id < 0 || id >= 10000) return {};
                Space();
                if (Take() != ':') return {};
                Space();
            }
            std::string name;
            if (!Quoted(name) || !result.emplace(id, name).second) return {};
            Space();
            if (Peek() == close) { Take(); Space(); return pos_ == text_.size() ? result : std::map<int, std::string>{}; }
            if (Take() != ',') return {};
            Space();
            if (Peek() == close) { Take(); Space(); return pos_ == text_.size() ? result : std::map<int, std::string>{}; }
        }
        return {};
    }
    static int Count(const std::string& text) {
        if (text.empty()) return -1;
        int value = 0;
        for (unsigned char c : text) {
            if (c < '0' || c > '9' || value > 10000) return -1;
            value = value * 10 + c - '0';
        }
        return value <= 10000 ? value : -1;
    }
private:
    const std::string& text_;
    size_t pos_ = 0;
    char Peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }
    char Take() { return pos_ < text_.size() ? text_[pos_++] : '\0'; }
    void Space() { while (std::isspace(static_cast<unsigned char>(Peek()))) ++pos_; }
    bool Hex(int digits, unsigned& value) {
        value = 0;
        for (int i = 0; i < digits; ++i) {
            const char c = Take();
            int n = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (n < 0) return false;
            value = (value << 4) | static_cast<unsigned>(n);
        }
        return true;
    }
    static bool Utf8(std::string& out, unsigned cp) {
        if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) {
            out += static_cast<char>(0xc0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 63));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xe0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 63)); out += static_cast<char>(0x80 | (cp & 63));
        } else {
            out += static_cast<char>(0xf0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 63)); out += static_cast<char>(0x80 | ((cp >> 6) & 63)); out += static_cast<char>(0x80 | (cp & 63));
        }
        return true;
    }
    bool Quoted(std::string& out) {
        const char quote = Take();
        if (quote != '\'' && quote != '"') return false;
        while (pos_ < text_.size()) {
            char c = Take();
            if (c == quote) return true;
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') { out += c; continue; }
            c = Take();
            switch (c) {
            case '\\': case '\'': case '"': case '/': out += c; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': case 'U': case 'x': {
                unsigned cp;
                if (!Hex(c == 'u' ? 4 : c == 'U' ? 8 : 2, cp)) return false;
                if (c == 'u' && cp >= 0xd800 && cp <= 0xdbff) {
                    unsigned low;
                    if (Take() != '\\' || Take() != 'u' || !Hex(4, low) || low < 0xdc00 || low > 0xdfff) return false;
                    cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
                }
                if (!Utf8(out, cp)) return false;
                break;
            }
            default: return false;
            }
        }
        return false;
    }
};

inline std::vector<std::pair<int, std::string>> Resolve(const std::string& names, int count) {
    auto parsed = NamesParser(names).Parse();
    if (count > 0 && count <= 10000) {
        for (int i = 0; i < count; ++i) parsed.emplace(i, "");
    }
    return {parsed.begin(), parsed.end()};
}
} // namespace model_classes
