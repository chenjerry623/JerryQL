#include "storage/row_codec.h"

#include <cstring>
#include <stdexcept>

namespace jerryql {

std::string encodeRow(const Row& row) {
    std::string out;
    for (const Value& value : row) {
        if (value.isInt()) {
            int64_t v = value.asInt();
            out += 'I';
            out.append(reinterpret_cast<const char*>(&v), sizeof(v));
        } else {
            const std::string& text = value.asText();
            uint32_t length = uint32_t(text.size());
            out += 'T';
            out.append(reinterpret_cast<const char*>(&length), sizeof(length));
            out += text;
        }
    }
    return out;
}

void decodeRowInto(const char* bytes, size_t length, Row& row) {
    size_t pos = 0, column = 0;
    auto need = [&](size_t n) {
        if (pos + n > length) throw std::runtime_error("corrupt row encoding");
    };
    while (pos < length) {
        if (column == row.size()) row.emplace_back();
        Value& value = row[column++];
        char tag = bytes[pos++];
        if (tag == 'I') {
            need(8);
            int64_t v;
            std::memcpy(&v, bytes + pos, 8);
            pos += 8;
            value.assignInt(v);
        } else if (tag == 'T') {
            need(4);
            uint32_t textLength;
            std::memcpy(&textLength, bytes + pos, 4);
            pos += 4;
            need(textLength);
            value.assignText(bytes + pos, textLength);
            pos += textLength;
        } else {
            throw std::runtime_error("corrupt row encoding");
        }
    }
    row.resize(column);
}

Row decodeRow(const std::string& bytes) {
    Row row;
    decodeRowInto(bytes.data(), bytes.size(), row);
    return row;
}

}  // namespace jerryql
