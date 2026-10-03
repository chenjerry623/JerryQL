#pragma once

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>

#include "value.h"

namespace jerryql {

// An inclusive range of row keys [lo, hi]. The planner narrows it from WHERE
// clauses on the primary key; the store uses it to avoid a full scan.
struct KeyRange {
    int64_t lo = std::numeric_limits<int64_t>::min();
    int64_t hi = std::numeric_limits<int64_t>::max();
    bool empty = false;

    bool isAll() const {
        return !empty && lo == std::numeric_limits<int64_t>::min() &&
               hi == std::numeric_limits<int64_t>::max();
    }
    bool isPoint() const { return !empty && lo == hi; }
};

// Iterates rows in ascending key order.
class Cursor {
public:
    virtual ~Cursor() = default;
    virtual bool next(int64_t& key, Row& row) = 0;
};

// Storage for one table: rows keyed by a 64-bit integer (the INT primary key,
// or a hidden row id). This interface is the seam where the in-memory map is
// replaced by an on-disk B+tree. Cursors are invalidated by any write.
class TableStore {
public:
    virtual ~TableStore() = default;
    virtual bool insert(int64_t key, Row row) = 0;  // false if the key exists
    virtual bool replace(int64_t key, Row row) = 0; // false if the key is missing
    virtual bool erase(int64_t key) = 0;            // false if the key is missing
    virtual bool contains(int64_t key) const = 0;
    virtual std::unique_ptr<Cursor> scan(const KeyRange& range) const = 0;
    virtual size_t size() const = 0;
    // A fresh hidden key for tables without a primary key. Never reused.
    virtual int64_t allocateRowId() = 0;
};

}  // namespace jerryql
