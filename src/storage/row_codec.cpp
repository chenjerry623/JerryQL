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

Row decodeRow(const std::string& bytes) {
    Row row;
    size_t pos = 0;
    auto need = [&](size_t n) {
        if (pos + n > bytes.size()) throw std::runtime_error("corrupt row encoding");
    };
    while (pos < bytes.size()) {
        char tag = bytes[pos++];
        if (tag == 'I') {
            need(8);
            int64_t v;
            std::memcpy(&v, bytes.data() + pos, 8);
            pos += 8;
            row.push_back(Value::integer(v));
        } else if (tag == 'T') {
            need(4);
            uint32_t length;
            std::memcpy(&length, bytes.data() + pos, 4);
            pos += 4;
            need(length);
            row.push_back(Value::text(bytes.substr(pos, length)));
            pos += length;
        } else {
            throw std::runtime_error("corrupt row encoding");
        }
    }
    return row;
}

}  // namespace jerryql
