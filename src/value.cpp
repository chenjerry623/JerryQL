#include "value.h"

#include "sql_error.h"

namespace jerryql {

std::string typeName(Type type) {
    return type == Type::Int ? "INT" : "TEXT";
}

std::string Value::toString() const {
    return isInt() ? std::to_string(asInt()) : asText();
}

std::string Value::toSqlLiteral() const {
    if (isInt()) return std::to_string(asInt());
    std::string out = "'";
    for (char c : asText()) {
        if (c == '\'') out += '\'';
        out += c;
    }
    return out + "'";
}

int compareValues(const Value& a, const Value& b) {
    if (a.type() != b.type()) {
        throw SqlError("cannot compare " + typeName(a.type()) + " with " + typeName(b.type()));
    }
    if (a.isInt()) {
        if (a.asInt() < b.asInt()) return -1;
        return a.asInt() > b.asInt() ? 1 : 0;
    }
    int c = a.asText().compare(b.asText());
    return (c > 0) - (c < 0);
}

}  // namespace jerryql
