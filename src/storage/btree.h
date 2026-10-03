#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "storage/pager.h"

namespace jerryql {

// Largest payload (encoded row) and key a B+tree cell can hold. Chosen so at
// least three of the largest cells fit in a 4 KiB leaf and a dozen of the
// largest separators fit in an internal page, which keeps splits simple:
// both halves of a split always fit. Larger rows would need overflow pages.
constexpr size_t kMaxPayload = 1000;
constexpr size_t kMaxKeySize = 256;

// Keys are byte strings ordered like memcmp (shorter first on a tie). Tables
// use 8-byte integer keys encoded so that byte order equals numeric order;
// secondary indexes use composite keys (see index_key.h).
int compareKeys(std::string_view a, std::string_view b);
std::string encodeIntKey(int64_t value);
int64_t decodeIntKey(std::string_view key);  // the first 8 bytes

// Iterates cells in key order across the linked list of leaves. Keeps the
// current leaf pinned between calls (one hash lookup per leaf, not per row)
// and releases it when exhausted. Any write to the tree invalidates it.
class BTreeCursor {
public:
    BTreeCursor(Pager& pager, PageId leaf, uint16_t slot, std::optional<std::string> hi);
    bool next(std::string& key, std::string& payload);
    // Zero-copy: `key` and `payload` point into the pinned page and stay
    // valid until the next call.
    bool nextRaw(std::string_view& key, std::string_view& payload);

private:
    Pager& pager_;
    PageId leaf_;  // 0 when exhausted
    uint16_t slot_;
    std::optional<std::string> hi_;  // inclusive upper bound; none = to the end
    PageRef page_;                   // pinned copy of leaf_, fetched lazily
};

struct BTreeShape {
    int depth = 0;             // 1 = the root is a leaf
    uint64_t leafPages = 0;
    uint64_t internalPages = 0;
    double leafFill = 0;       // average fraction of leaf page bytes in use
};

// A B+tree mapping byte-string keys to byte-string payloads, stored in pager pages.
//
// Leaf page:     header | slot array (u16 offsets, sorted by key) -> ... free ... <- cells
//                cell = u16 key length, u16 payload length, key, payload
// Internal page: header | child0 (u32) | slot array -> ... free ... <- cells
//                cell = u32 child, u16 key length, key
//                child0 holds keys < key[0]; cell i's child holds keys >= key[i]
//                (and < key[i+1])
// Header (24 bytes): u8 type, u16 count, u32 next leaf, u64 row count, u64 aux.
// The row count and aux fields are only meaningful on the root page, which
// never moves: when the root splits, its contents move to a new page and the
// root becomes an internal node above it.
//
// Deletes remove cells without merging pages, so heavy deletes leave
// underfull pages; dropping the tree returns all pages to the freelist.
class BTree {
public:
    static PageId create(Pager& pager);  // allocates an empty tree; returns its root
    BTree(Pager& pager, PageId root) : pager_(pager), root_(root) {}

    PageId root() const { return root_; }

    bool insert(std::string_view key, std::string_view payload);   // false if key exists
    bool replace(std::string_view key, std::string_view payload);  // false if key missing
    bool erase(std::string_view key);                              // false if key missing
    std::optional<std::string> find(std::string_view key);
    // Keys in [lo, hi], both inclusive; no hi = to the last key.
    BTreeCursor scan(std::string_view lo, std::optional<std::string> hi);

    uint64_t size();         // number of cells
    uint64_t aux();          // a spare u64 stored in the root, for the caller
    void setAux(uint64_t value);
    void destroy();          // frees every page, root included

    // Validates ordering, separator bounds, uniform leaf depth, the leaf chain
    // and the row count. Throws std::logic_error on the first violation.
    BTreeShape check();

private:
    struct Split {
        std::string separator;
        PageId right;
    };

    std::optional<Split> insertInto(PageId id, std::string_view key, std::string_view payload,
                                    bool& duplicate);
    std::optional<Split> insertIntoLeaf(PageRef& page, std::string_view key,
                                        std::string_view payload, bool& duplicate);
    std::optional<Split> insertIntoInternal(PageRef& page, std::string_view key,
                                            std::string_view payload, bool& duplicate);
    void growRoot(const Split& split);
    PageId findLeaf(std::string_view key);
    void addToRowCount(int64_t delta);
    void destroyPage(PageId id);

    Pager& pager_;
    PageId root_;
};

}  // namespace jerryql
