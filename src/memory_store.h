#pragma once

#include <map>

#include "table_store.h"

namespace jerryql {

// In-memory TableStore backed by std::map. The engine stores tables in
// B+trees (BTreeStore); this is the reference implementation tests compare
// the B+tree against.
class MemoryStore : public TableStore {
public:
    bool insert(int64_t key, Row row) override;
    bool replace(int64_t key, Row row) override;
    bool erase(int64_t key) override;
    bool contains(int64_t key) const override;
    std::unique_ptr<Cursor> scan(const KeyRange& range) const override;
    size_t size() const override { return rows_.size(); }
    int64_t allocateRowId() override { return ++lastRowId_; }

private:
    std::map<int64_t, Row> rows_;
    int64_t lastRowId_ = 0;
};

}  // namespace jerryql
