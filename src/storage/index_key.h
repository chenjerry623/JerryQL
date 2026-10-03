#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "value.h"

namespace jerryql {

// Secondary index keys. An index entry's key is
//   encodeValueKey(column value) + encodeIntKey(primary key)
// with an empty payload. Equal values sort together, ordered by primary key,
// and every entry is unique even when values repeat.
//
// encodeValueKey preserves order under memcmp and is prefix-free:
//   INT:  0x01, then 8 bytes big-endian with the sign bit flipped
//   TEXT: 0x02, then the bytes with 0x00 escaped as 0x00 0xFF, then 0x00 0x00
// so "ab" < "ab\0" < "abc" and no encoded value is a prefix of another.
std::string encodeValueKey(const Value& value);
std::string indexEntryKey(const Value& value, int64_t primaryKey);
int64_t primaryKeyOfEntry(std::string_view entryKey);  // the last 8 bytes

// Inclusive byte bounds that select index entries by value:
//   value >= v  ->  lowerBound(v, true)      value > v  ->  lowerBound(v, false)
//   value <= v  ->  upperBound(v, true)      value < v  ->  upperBound(v, false)
std::string indexLowerBound(const Value& value, bool inclusive);
std::string indexUpperBound(const Value& value, bool inclusive);

}  // namespace jerryql
