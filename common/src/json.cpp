#include "sb2/json.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace sb2 {

namespace {
const Json kNull;
const Json::Array kEmptyArray;
const Json::Object kEmptyObject;

struct Parser {
    const char* p;
    const char* end;
    std::string err;
    void ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }
    bool fail(const char* m) {
        if (err.empty()) err = m;
        return false;
    }
    bool str(std::string& out) {
        if (p >= end || *p != '"') return fail("expected string");
        ++p;
        while (p < end && *p != '"') {
            char c = *p++;
            if (c == '\\') {
                if (p >= end) return fail("bad escape");
                char e = *p++;
                switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    if (end - p < 4) return fail("bad unicode escape");
                    unsigned cp = (unsigned)std::stoul(std::string(p, p + 4), nullptr, 16);
                    p += 4;
                    if (cp < 0x80) out += (char)cp;
                    else if (cp < 0x800) {
                        out += (char)(0xC0 | (cp >> 6));
                        out += (char)(0x80 | (cp & 0x3F));
                    } else {
                        out += (char)(0xE0 | (cp >> 12));
                        out += (char)(0x80 | ((cp >> 6) & 0x3F));
                        out += (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: out += e; break;
                }
            } else {
                out += c;
            }
        }
        if (p >= end) return fail("unterminated string");
        ++p;
        return true;
    }
    bool value(Json& out) {
        ws();
        if (p >= end) return fail("unexpected end");
        if (*p == '{') {
            ++p;
            Json::Object o;
            ws();
            if (p < end && *p == '}') {
                ++p;
                out = Json(std::move(o));
                return true;
            }
            for (;;) {
                ws();
                std::string k;
                if (!str(k)) return false;
                ws();
                if (p >= end || *p != ':') return fail("expected ':'");
                ++p;
                Json v;
                if (!value(v)) return false;
                o[k] = std::move(v);
                ws();
                if (p < end && *p == ',') {
                    ++p;
                    continue;
                }
                if (p < end && *p == '}') {
                    ++p;
                    break;
                }
                return fail("expected ',' or '}'");
            }
            out = Json(std::move(o));
            return true;
        }
        if (*p == '[') {
            ++p;
            Json::Array a;
            ws();
            if (p < end && *p == ']') {
                ++p;
                out = Json(std::move(a));
                return true;
            }
            for (;;) {
                Json v;
                if (!value(v)) return false;
                a.push_back(std::move(v));
                ws();
                if (p < end && *p == ',') {
                    ++p;
                    continue;
                }
                if (p < end && *p == ']') {
                    ++p;
                    break;
                }
                return fail("expected ',' or ']'");
            }
            out = Json(std::move(a));
            return true;
        }
        if (*p == '"') {
            std::string s;
            if (!str(s)) return false;
            out = Json(std::move(s));
            return true;
        }
        if (end - p >= 4 && std::strncmp(p, "true", 4) == 0) {
            p += 4;
            out = Json(true);
            return true;
        }
        if (end - p >= 5 && std::strncmp(p, "false", 5) == 0) {
            p += 5;
            out = Json(false);
            return true;
        }
        if (end - p >= 4 && std::strncmp(p, "null", 4) == 0) {
            p += 4;
            out = Json();
            return true;
        }
        char* e = nullptr;
        const std::string num(p, (size_t)std::min<ptrdiff_t>(end - p, 64));
        const double d = std::strtod(num.c_str(), &e);
        if (e == num.c_str()) return fail("invalid value");
        p += (e - num.c_str());
        out = Json(d);
        return true;
    }
};

void escape(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char b[8];
                snprintf(b, sizeof b, "\\u%04x", c);
                out += b;
            } else {
                out += (char)c;
            }
        }
    }
    out += '"';
}
} // namespace

const Json::Array& Json::asArray() const { return isArray() ? std::get<Array>(v_) : kEmptyArray; }
const Json::Object& Json::asObject() const { return isObject() ? std::get<Object>(v_) : kEmptyObject; }
Json::Array& Json::arr() {
    if (!isArray()) v_ = Array{};
    return std::get<Array>(v_);
}
Json::Object& Json::obj() {
    if (!isObject()) v_ = Object{};
    return std::get<Object>(v_);
}
Json& Json::operator[](const std::string& key) { return obj()[key]; }
const Json& Json::get(const std::string& key) const {
    if (!isObject()) return kNull;
    auto& o = std::get<Object>(v_);
    auto it = o.find(key);
    return it == o.end() ? kNull : it->second;
}
bool Json::has(const std::string& key) const { return isObject() && std::get<Object>(v_).count(key); }

void Json::dumpTo(std::string& out, int indent, int level) const {
    auto nl = [&](int l) {
        if (indent <= 0) return;
        out += '\n';
        out.append((size_t)(l * indent), ' ');
    };
    if (isNull()) out += "null";
    else if (isBool()) out += asBool() ? "true" : "false";
    else if (isNumber()) {
        const double d = asNumber();
        char b[64];
        if (std::isfinite(d) && d == std::floor(d) && std::fabs(d) < 9.0e15) snprintf(b, sizeof b, "%.0f", d);
        else snprintf(b, sizeof b, "%.17g", d);
        out += b;
    } else if (isString()) escape(out, asString());
    else if (isArray()) {
        const auto& a = asArray();
        out += '[';
        for (size_t i = 0; i < a.size(); ++i) {
            nl(level + 1);
            a[i].dumpTo(out, indent, level + 1);
            if (i + 1 < a.size()) out += ',';
        }
        if (!a.empty()) nl(level);
        out += ']';
    } else {
        const auto& o = asObject();
        out += '{';
        size_t i = 0;
        for (auto& [k, v] : o) {
            nl(level + 1);
            escape(out, k);
            out += indent > 0 ? ": " : ":";
            v.dumpTo(out, indent, level + 1);
            if (++i < o.size()) out += ',';
        }
        if (!o.empty()) nl(level);
        out += '}';
    }
}

std::string Json::dump(int indent) const {
    std::string s;
    dumpTo(s, indent, 0);
    return s;
}

Json Json::parse(const std::string& text, std::string* err) {
    Parser ps{text.data(), text.data() + text.size(), {}};
    Json v;
    if (!ps.value(v)) {
        if (err) *err = ps.err;
        return Json();
    }
    return v;
}

Json Json::load(const std::string& path, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (err) *err = "cannot open " + path;
        return Json();
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return parse(ss.str(), err);
}

bool Json::save(const std::string& path, int indent) const {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return false;
        f << dump(indent) << '\n';
        if (!f) return false;
    }
    std::remove(path.c_str());
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

} // namespace sb2
