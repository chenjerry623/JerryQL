#include "storage/btree_store.h"

#include "storage/row_codec.h"

namespace jerryql {

namespace {

class BTreeRowCursor : public Cursor {
public:
    explicit BTreeRowCursor(BTreeCursor cursor) : cursor_(std::move(cursor)) {}

    bool next(int64_t& key, Row& row) override {
        std::string_view rawKey, payload;
        if (!cursor_.nextRaw(rawKey, payload)) return false;
        key = decodeIntKey(rawKey);
        decodeRowInto(payload.data(), payload.size(), row);
        return true;
    }

private:
    BTreeCursor cursor_;
};

}  // namespace

bool BTreeStore::insert(int64_t key, Row row) {
    return tree_.insert(encodeIntKey(key), encodeRow(row));
}

bool BTreeStore::replace(int64_t key, Row row) {
    return tree_.replace(encodeIntKey(key), encodeRow(row));
}

bool BTreeStore::erase(int64_t key) {
    return tree_.erase(encodeIntKey(key));
}

bool BTreeStore::contains(int64_t key) const {
    return tree_.find(encodeIntKey(key)).has_value();
}

std::optional<Row> BTreeStore::get(int64_t key) const {
    std::optional<std::string> payload = tree_.find(encodeIntKey(key));
    if (!payload) return std::nullopt;
    return decodeRow(*payload);
}

std::unique_ptr<Cursor> BTreeStore::scan(const KeyRange& range) const {
    if (range.empty) {
        return std::make_unique<BTreeRowCursor>(tree_.scan(encodeIntKey(1), encodeIntKey(0)));
    }
    return std::make_unique<BTreeRowCursor>(tree_.scan(encodeIntKey(range.lo), encodeIntKey(range.hi)));
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
