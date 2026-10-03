#include "storage/index_key.h"

#include "storage/btree.h"

namespace jerryql {

namespace {

// Every entry for value v is exactly encodeValueKey(v) followed by 8 bytes,
// so encodeValueKey(v) + 8 x 0xFF is >= all of them.
const std::string kMaxPrimaryKeyBytes(8, '\xff');

}  // namespace

std::string encodeValueKey(const Value& value) {
    if (value.isInt()) return "\x01" + encodeIntKey(value.asInt());
    std::string key = "\x02";
    for (char c : value.asText()) {
        key += c;
        if (c == '\0') key += '\xff';
    }
    key += std::string("\0\0", 2);
    return key;
}

std::string indexEntryKey(const Value& value, int64_t primaryKey) {
    return encodeValueKey(value) + encodeIntKey(primaryKey);
}

int64_t primaryKeyOfEntry(std::string_view entryKey) {
    return decodeIntKey(entryKey.substr(entryKey.size() - 8));
}

std::string indexLowerBound(const Value& value, bool inclusive) {
    std::string bound = encodeValueKey(value);
    // Strictly after every entry for v: the smallest string above v's last entry.
    if (!inclusive) bound += kMaxPrimaryKeyBytes + std::string(1, '\0');
    return bound;
}

std::string indexUpperBound(const Value& value, bool inclusive) {
    std::string bound = encodeValueKey(value);
    // Entries for v are longer than encodeValueKey(v), so they sort after it:
    // the bare encoding excludes v, and adding 0xFF bytes includes it.
    if (inclusive) bound += kMaxPrimaryKeyBytes;
    return bound;
}

}  // namespace jerryql
