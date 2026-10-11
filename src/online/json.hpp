// A small JSON value: enough to read the configuration document and to talk to
// the master server. Numbers are doubles; whole numbers up to 2^53 are exact.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ab::online {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(bool v) : type_(Type::Bool), bool_(v) {}
    Json(double v) : type_(Type::Number), number_(v) {}
    Json(int v) : type_(Type::Number), number_(v) {}
    Json(long long v) : type_(Type::Number), number_(static_cast<double>(v)) {}
    Json(const char* v) : type_(Type::String), string_(v) {}
    Json(std::string v) : type_(Type::String), string_(std::move(v)) {}
    static Json array() {
        Json j;
        j.type_ = Type::Array;
        return j;
    }
    static Json object() {
        Json j;
        j.type_ = Type::Object;
        return j;
    }

    // False on anything that is not one JSON value, is nested deeper than 32, or longer than 1 MiB.
    static bool parse(const std::string& text, Json& out);
    std::string dump() const;

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isObject() const { return type_ == Type::Object; }
    bool isArray() const { return type_ == Type::Array; }
    bool isString() const { return type_ == Type::String; }
    bool isNumber() const { return type_ == Type::Number; }

    // Reading never fails: a value of another kind reads as empty / zero / false.
    const std::string& str() const { return string_; }
    double number() const { return type_ == Type::Number ? number_ : 0.0; }
    long long integer() const { return static_cast<long long>(number()); }
    bool boolean() const { return type_ == Type::Bool && bool_; }
    const std::vector<Json>& items() const { return items_; }
    const Json& operator[](const std::string& key) const;  // a member of an object, or null
    bool has(const std::string& key) const { return members_.count(key) != 0; }

    Json& set(const std::string& key, Json value);
    Json& push(Json value);

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<Json> items_;
    std::map<std::string, Json> members_;
};

}  // namespace ab::online
