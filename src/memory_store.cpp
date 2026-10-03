#include "memory_store.h"

namespace jerryql {

namespace {

class MemoryCursor : public Cursor {
public:
    using Iterator = std::map<int64_t, Row>::const_iterator;
    MemoryCursor(Iterator begin, Iterator end) : it_(begin), end_(end) {}

    bool next(int64_t& key, Row& row) override {
        if (it_ == end_) return false;
        key = it_->first;
        row = it_->second;
        ++it_;
        return true;
    }

private:
    Iterator it_;
    Iterator end_;
};

}  // namespace

bool MemoryStore::insert(int64_t key, Row row) {
    return rows_.emplace(key, std::move(row)).second;
}

bool MemoryStore::replace(int64_t key, Row row) {
    auto it = rows_.find(key);
    if (it == rows_.end()) return false;
    it->second = std::move(row);
    return true;
}

bool MemoryStore::erase(int64_t key) {
    return rows_.erase(key) > 0;
}

bool MemoryStore::contains(int64_t key) const {
    return rows_.count(key) > 0;
}

std::unique_ptr<Cursor> MemoryStore::scan(const KeyRange& range) const {
    if (range.empty || range.lo > range.hi) {
        return std::make_unique<MemoryCursor>(rows_.end(), rows_.end());
    }
    return std::make_unique<MemoryCursor>(rows_.lower_bound(range.lo),
                                          rows_.upper_bound(range.hi));
}

}  // namespace jerryql
