#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "storage/pager.h"

namespace jerryql {

// Largest payload (encoded row) a B+tree cell can hold. Chosen so at least
// four cells always fit in a 4 KiB leaf, which keeps splits simple: both
// halves of a split always fit. Larger rows would need overflow pages.
constexpr size_t kMaxPayload = 1000;

// Iterates cells in key order across the linked list of leaves. Holds no
// page pins between calls; any write to the tree invalidates it.
class BTreeCursor {
public:
    BTreeCursor(Pager& pager, PageId leaf, uint16_t slot, int64_t hi);
    bool next(int64_t& key, std::string& payload);

private:
    Pager& pager_;
    PageId leaf_;  // 0 when exhausted
    uint16_t slot_;
    int64_t hi_;
};

struct BTreeShape {
    int depth = 0;             // 1 = the root is a leaf
    uint64_t leafPages = 0;
    uint64_t internalPages = 0;
    double leafFill = 0;       // average fraction of leaf page bytes in use
};

// A B+tree mapping int64 keys to byte-string payloads, stored in pager pages.
//
// Leaf page:     header | slot array (u16 offsets, sorted by key) -> ... free ... <- cells
//                cell = i64 key, u16 length, payload bytes
// Internal page: header | child0 (u32) | (i64 key, u32 child) * count
//                child[i] holds keys < key[i]; child[i+1] holds keys >= key[i]
// Header (24 bytes): u8 type, u16 count, u32 next leaf, u64 row count, u64 aux.
// The row count and aux fields are only meaningful on the root page, which
// never moves: when the root splits, its contents move to a new page and the
// root becomes an internal node above it.
//
// Deletes remove cells without merging pages, so heavy deletes leave
// underfull pages; DROP TABLE returns all pages to the freelist.
class BTree {
public:
    static PageId create(Pager& pager);  // allocates an empty tree; returns its root
    BTree(Pager& pager, PageId root) : pager_(pager), root_(root) {}

    PageId root() const { return root_; }

    bool insert(int64_t key, const std::string& payload);   // false if key exists
    bool replace(int64_t key, const std::string& payload);  // false if key missing
    bool erase(int64_t key);                                // false if key missing
    std::optional<std::string> find(int64_t key);
    BTreeCursor scan(int64_t lo, int64_t hi);  // keys in [lo, hi]

    uint64_t size();         // number of cells
    uint64_t aux();          // a spare u64 stored in the root, for the caller
    void setAux(uint64_t value);
    void destroy();          // frees every page, root included

    // Validates ordering, separator bounds, uniform leaf depth, the leaf chain
    // and the row count. Throws std::logic_error on the first violation.
    BTreeShape check();

private:
    struct Split {
        int64_t separator;
        PageId right;
    };

    std::optional<Split> insertInto(PageId id, int64_t key, const std::string& payload,
                                    bool& duplicate);
    std::optional<Split> insertIntoLeaf(PageRef& page, int64_t key, const std::string& payload,
                                        bool& duplicate);
    std::optional<Split> insertIntoInternal(PageRef& page, int64_t key, const std::string& payload,
                                            bool& duplicate);
    void growRoot(const Split& split);
    PageId findLeaf(int64_t key);
    void addToRowCount(int64_t delta);
    void destroyPage(PageId id);

    Pager& pager_;
    PageId root_;
};

}  // namespace jerryql
