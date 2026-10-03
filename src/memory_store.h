#pragma once

#include <map>

#include "table_store.h"

namespace jerryql {

// In-memory TableStore backed by std::map (a balanced tree). Used until the
// on-disk B+tree exists, and kept afterwards as the reference implementation.
class MemoryStore : public TableStore {
public:
    bool insert(int64_t key, Row row) override;
    bool replace(int64_t key, Row row) override;
    bool erase(int64_t key) override;
    bool contains(int64_t key) const override;
    std::unique_ptr<Cursor> scan(const KeyRange& range) const override;
    size_t size() const override { return rows_.size(); }

private:
    std::map<int64_t, Row> rows_;
};

}  // namespace jerryql
