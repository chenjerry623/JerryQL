#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace jerryql {

enum class Type { Int, Text };

std::string typeName(Type type);

// A single SQL value: a 64-bit integer or a string. There is no NULL yet.
class Value {
public:
    Value() : data_(int64_t{0}) {}
    static Value integer(int64_t v) { return Value(Data(v)); }
    static Value text(std::string v) { return Value(Data(std::move(v))); }

    Type type() const { return isInt() ? Type::Int : Type::Text; }
    bool isInt() const { return std::holds_alternative<int64_t>(data_); }
    int64_t asInt() const { return std::get<int64_t>(data_); }
    const std::string& asText() const { return std::get<std::string>(data_); }

    // Display form: integers in decimal, text unquoted.
    std::string toString() const;
    // SQL literal form: text is single-quoted with '' escaping.
    std::string toSqlLiteral() const;

    bool operator==(const Value& other) const { return data_ == other.data_; }
    bool operator!=(const Value& other) const { return !(*this == other); }

private:
    using Data = std::variant<int64_t, std::string>;
    explicit Value(Data data) : data_(std::move(data)) {}
    Data data_;
};

// Three-way comparison (-1, 0, 1). Throws SqlError when the types differ.
int compareValues(const Value& a, const Value& b);

using Row = std::vector<Value>;

}  // namespace jerryql
