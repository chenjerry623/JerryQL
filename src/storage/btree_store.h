#pragma once

#include "storage/btree.h"
#include "table_store.h"

namespace jerryql {

// TableStore over a B+tree: the table's rows are the tree's leaf cells,
// keyed by primary key or row id (a clustered table, as in SQLite).
// The tree's aux field holds the last row id handed out.
class BTreeStore : public TableStore {
public:
    BTreeStore(Pager& pager, PageId root) : tree_(pager, root) {}

    bool insert(int64_t key, Row row) override;
    bool replace(int64_t key, Row row) override;
    bool erase(int64_t key) override;
    bool contains(int64_t key) const override;
    std::optional<Row> get(int64_t key) const override;
    std::unique_ptr<Cursor> scan(const KeyRange& range) const override;
    size_t size() const override;
    int64_t allocateRowId() override;

    BTree& tree() { return tree_; }

private:
    mutable BTree tree_;  // reads pin pages, which isn't a logical mutation
};

}  // namespace jerryql
