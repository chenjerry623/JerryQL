#pragma once

#include <stdexcept>
#include <string>

namespace jerryql {

// Any user-facing error: bad syntax, unknown table, type mismatch, duplicate key.
// The shell prints these as "Error: <message>" and keeps running.
class SqlError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

}  // namespace jerryql
