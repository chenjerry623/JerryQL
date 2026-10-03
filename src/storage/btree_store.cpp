#include "storage/btree_store.h"

#include "storage/row_codec.h"

namespace jerryql {

namespace {

class BTreeRowCursor : public Cursor {
public:
    explicit BTreeRowCursor(BTreeCursor cursor) : cursor_(std::move(cursor)) {}

    bool next(int64_t& key, Row& row) override {
        if (!cursor_.next(key, payload_)) return false;
        row = decodeRow(payload_);
        return true;
    }

private:
    BTreeCursor cursor_;
    std::string payload_;
};

}  // namespace

bool BTreeStore::insert(int64_t key, Row row) {
    return tree_.insert(key, encodeRow(row));
}

bool BTreeStore::replace(int64_t key, Row row) {
    return tree_.replace(key, encodeRow(row));
}

bool BTreeStore::erase(int64_t key) {
    return tree_.erase(key);
}

bool BTreeStore::contains(int64_t key) const {
    return tree_.find(key).has_value();
}

std::unique_ptr<Cursor> BTreeStore::scan(const KeyRange& range) const {
    if (range.empty) return std::make_unique<BTreeRowCursor>(tree_.scan(1, 0));
    return std::make_unique<BTreeRowCursor>(tree_.scan(range.lo, range.hi));
}

size_t BTreeStore::size() const {
    return size_t(tree_.size());
}

int64_t BTreeStore::allocateRowId() {
    int64_t id = int64_t(tree_.aux()) + 1;
    tree_.setAux(uint64_t(id));
    return id;
}

}  // namespace jerryql
