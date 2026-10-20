// Minimal JSON value with parser and serializer (enough for caches,
// manifests and settings).
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace sb2 {

class Json {
public:
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;

    Json() : v_(nullptr) {}
    Json(std::nullptr_t) : v_(nullptr) {}
    Json(bool b) : v_(b) {}
    Json(int i) : v_((double)i) {}
    Json(int64_t i) : v_((double)i) {}
    Json(uint32_t i) : v_((double)i) {}
    Json(uint64_t i) : v_((double)i) {}
    Json(double d) : v_(d) {}
    Json(const char* s) : v_(std::string(s)) {}
    Json(std::string s) : v_(std::move(s)) {}
    Json(Array a) : v_(std::move(a)) {}
    Json(Object o) : v_(std::move(o)) {}

    bool isNull() const { return std::holds_alternative<std::nullptr_t>(v_); }
    bool isBool() const { return std::holds_alternative<bool>(v_); }
    bool isNumber() const { return std::holds_alternative<double>(v_); }
    bool isString() const { return std::holds_alternative<std::string>(v_); }
    bool isArray() const { return std::holds_alternative<Array>(v_); }
    bool isObject() const { return std::holds_alternative<Object>(v_); }

    bool asBool(bool def = false) const { return isBool() ? std::get<bool>(v_) : def; }
    double asNumber(double def = 0) const { return isNumber() ? std::get<double>(v_) : def; }
    int64_t asInt(int64_t def = 0) const { return isNumber() ? (int64_t)std::get<double>(v_) : def; }
    std::string asString(const std::string& def = {}) const { return isString() ? std::get<std::string>(v_) : def; }
    const Array& asArray() const;
    const Object& asObject() const;
    Array& arr();
    Object& obj();

    // Object access (creates on write)
    Json& operator[](const std::string& key);
    const Json& get(const std::string& key) const; // null if missing
    bool has(const std::string& key) const;
    void push(Json v) { arr().push_back(std::move(v)); }

    std::string dump(int indent = 2) const;
    static Json parse(const std::string& text, std::string* err = nullptr);
    static Json load(const std::string& path, std::string* err = nullptr);
    bool save(const std::string& path, int indent = 2) const;

private:
    void dumpTo(std::string& out, int indent, int level) const;
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> v_;
};

} // namespace sb2
