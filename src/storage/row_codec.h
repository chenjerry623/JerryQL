#pragma once

#include <string>

#include "value.h"

namespace jerryql {

// Row <-> bytes for B+tree payloads. Each value is a type tag followed by
//   'I': 8-byte integer
//   'T': 4-byte length, then the bytes
std::string encodeRow(const Row& row);
Row decodeRow(const std::string& bytes);  // throws std::runtime_error if malformed
// Decodes into `row`, reusing its existing values' memory.
void decodeRowInto(const char* bytes, size_t length, Row& row);

}  // namespace jerryql
