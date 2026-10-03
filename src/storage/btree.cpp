#include "storage/btree.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace jerryql {

namespace {

// ---------- Page layout ----------

constexpr uint8_t kLeaf = 1;
constexpr uint8_t kInternal = 2;

constexpr size_t kTypeOffset = 0;      // u8
constexpr size_t kCountOffset = 2;     // u16
constexpr size_t kNextOffset = 4;      // u32, leaves only
constexpr size_t kRowCountOffset = 8;  // u64, root only
constexpr size_t kAuxOffset = 16;      // u64, root only
constexpr size_t kHeaderSize = 24;

constexpr size_t kSlotSize = 2;            // leaf slot: u16 cell offset
constexpr size_t kCellOverhead = 8 + 2;    // leaf cell: i64 key, u16 length
constexpr size_t kInternalEntrySize = 12;  // i64 key, u32 child
constexpr size_t kMaxInternalKeys = (kPageSize - kHeaderSize - 4) / kInternalEntrySize;

template <typename T>
T load(const char* p) {
    T value;
    std::memcpy(&value, p, sizeof(T));
    return value;
}

template <typename T>
void store(char* p, T value) {
    std::memcpy(p, &value, sizeof(T));
}

uint8_t nodeType(const char* page) { return load<uint8_t>(page + kTypeOffset); }
uint16_t cellCount(const char* page) { return load<uint16_t>(page + kCountOffset); }
PageId nextLeaf(const char* page) { return load<PageId>(page + kNextOffset); }

// ---------- Reading pages in place (no copies on the lookup path) ----------

uint16_t leafCellOffset(const char* page, size_t i) {
    return load<uint16_t>(page + kHeaderSize + i * kSlotSize);
}

int64_t leafKey(const char* page, size_t i) {
    return load<int64_t>(page + leafCellOffset(page, i));
}

std::string leafPayload(const char* page, size_t i) {
    const char* cell = page + leafCellOffset(page, i);
    uint16_t length = load<uint16_t>(cell + 8);
    return std::string(cell + kCellOverhead, length);
}

// Index of the first cell with key >= target.
size_t leafLowerBound(const char* page, int64_t target) {
    size_t lo = 0, hi = cellCount(page);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (leafKey(page, mid) < target) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

int64_t internalKey(const char* page, size_t i) {
    return load<int64_t>(page + kHeaderSize + 4 + i * kInternalEntrySize);
}

PageId internalChild(const char* page, size_t i) {
    if (i == 0) return load<PageId>(page + kHeaderSize);
    return load<PageId>(page + kHeaderSize + 4 + (i - 1) * kInternalEntrySize + 8);
}

// Index of the child whose range contains target: the number of keys <= target.
size_t internalChildIndex(const char* page, int64_t target) {
    size_t lo = 0, hi = cellCount(page);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (internalKey(page, mid) <= target) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

// ---------- Decoded nodes (used on the write path) ----------

struct LeafNode {
    std::vector<int64_t> keys;
    std::vector<std::string> payloads;
    PageId next = 0;
};

struct InternalNode {
    std::vector<int64_t> keys;
    std::vector<PageId> children;  // keys.size() + 1 entries
};

size_t leafBytes(const LeafNode& leaf, size_t begin, size_t end) {
    size_t bytes = kHeaderSize;
    for (size_t i = begin; i < end; ++i) bytes += kSlotSize + kCellOverhead + leaf.payloads[i].size();
    return bytes;
}

LeafNode decodeLeaf(const char* page) {
    LeafNode leaf;
    size_t count = cellCount(page);
    leaf.keys.reserve(count + 1);
    leaf.payloads.reserve(count + 1);
    for (size_t i = 0; i < count; ++i) {
        leaf.keys.push_back(leafKey(page, i));
        leaf.payloads.push_back(leafPayload(page, i));
    }
    leaf.next = nextLeaf(page);
    return leaf;
}

// Writes cells [begin, end) of `leaf`. Leaves the root-only fields alone.
void encodeLeaf(char* page, const LeafNode& leaf, size_t begin, size_t end, PageId next) {
    if (leafBytes(leaf, begin, end) > kPageSize) throw std::logic_error("leaf overflow");
    store<uint8_t>(page + kTypeOffset, kLeaf);
    store<uint16_t>(page + kCountOffset, uint16_t(end - begin));
    store<PageId>(page + kNextOffset, next);
    size_t contentStart = kPageSize;
    for (size_t i = begin; i < end; ++i) {
        const std::string& payload = leaf.payloads[i];
        contentStart -= kCellOverhead + payload.size();
        store<int64_t>(page + contentStart, leaf.keys[i]);
        store<uint16_t>(page + contentStart + 8, uint16_t(payload.size()));
        std::memcpy(page + contentStart + kCellOverhead, payload.data(), payload.size());
        store<uint16_t>(page + kHeaderSize + (i - begin) * kSlotSize, uint16_t(contentStart));
    }
}

InternalNode decodeInternal(const char* page) {
    InternalNode node;
    size_t count = cellCount(page);
    node.keys.reserve(count + 1);
    node.children.reserve(count + 2);
    for (size_t i = 0; i < count; ++i) node.keys.push_back(internalKey(page, i));
    for (size_t i = 0; i <= count; ++i) node.children.push_back(internalChild(page, i));
    return node;
}

// Writes keys [begin, end) and children [begin, end]. Leaves root-only fields alone.
void encodeInternal(char* page, const InternalNode& node, size_t begin, size_t end) {
    if (end - begin > kMaxInternalKeys) throw std::logic_error("internal node overflow");
    store<uint8_t>(page + kTypeOffset, kInternal);
    store<uint16_t>(page + kCountOffset, uint16_t(end - begin));
    store<PageId>(page + kNextOffset, 0);
    store<PageId>(page + kHeaderSize, node.children[begin]);
    for (size_t i = begin; i < end; ++i) {
        char* entry = page + kHeaderSize + 4 + (i - begin) * kInternalEntrySize;
        store<int64_t>(entry, node.keys[i]);
        store<PageId>(entry + 8, node.children[i + 1]);
    }
}

// Where to split an overfull leaf: the first index that goes to the new right
// page. Appending past the last key of the rightmost leaf (sequential
// inserts) moves only the new cell, so earlier leaves stay full instead of
// half-empty. Otherwise split by bytes, so both halves get similar free space.
size_t leafSplitPoint(const LeafNode& leaf, size_t insertedAt) {
    size_t count = leaf.keys.size();
    if (insertedAt == count - 1 && leaf.next == 0) return count - 1;
    size_t total = leafBytes(leaf, 0, count) - kHeaderSize;
    size_t running = 0;
    for (size_t i = 0; i < count; ++i) {
        running += kSlotSize + kCellOverhead + leaf.payloads[i].size();
        if (running * 2 >= total) return std::clamp<size_t>(i + 1, 1, count - 1);
    }
    return count / 2;
}

}  // namespace

// ---------- Cursor ----------

BTreeCursor::BTreeCursor(Pager& pager, PageId leaf, uint16_t slot, int64_t hi)
    : pager_(pager), leaf_(leaf), slot_(slot), hi_(hi) {}

bool BTreeCursor::nextRaw(int64_t& key, const char*& payload, size_t& length) {
    while (leaf_ != 0) {
        if (!page_.valid()) page_ = pager_.fetch(leaf_);
        const char* p = page_.data();
        if (slot_ < cellCount(p)) {
            const char* cell = p + leafCellOffset(p, slot_);
            int64_t k = load<int64_t>(cell);
            if (k > hi_) break;
            key = k;
            length = load<uint16_t>(cell + 8);
            payload = cell + kCellOverhead;
            ++slot_;
            return true;
        }
        leaf_ = nextLeaf(p);  // empty or finished leaf: move right
        slot_ = 0;
        page_ = PageRef();
    }
    leaf_ = 0;
    page_ = PageRef();
    return false;
}

bool BTreeCursor::next(int64_t& key, std::string& payload) {
    const char* data;
    size_t length;
    if (!nextRaw(key, data, length)) return false;
    payload.assign(data, length);
    return true;
}

// ---------- BTree ----------

PageId BTree::create(Pager& pager) {
    PageRef page = pager.allocate();
    LeafNode empty;
    encodeLeaf(page.mutableData(), empty, 0, 0, 0);
    return page.id();
}

PageId BTree::findLeaf(int64_t key) {
    PageId id = root_;
    while (true) {
        PageRef page = pager_.fetch(id);
        if (nodeType(page.data()) == kLeaf) return id;
        id = internalChild(page.data(), internalChildIndex(page.data(), key));
    }
}

std::optional<std::string> BTree::find(int64_t key) {
    PageRef page = pager_.fetch(findLeaf(key));
    size_t i = leafLowerBound(page.data(), key);
    if (i < cellCount(page.data()) && leafKey(page.data(), i) == key) {
        return leafPayload(page.data(), i);
    }
    return std::nullopt;
}

BTreeCursor BTree::scan(int64_t lo, int64_t hi) {
    if (lo > hi) return BTreeCursor(pager_, 0, 0, hi);
    PageId leaf = findLeaf(lo);
    PageRef page = pager_.fetch(leaf);
    return BTreeCursor(pager_, leaf, uint16_t(leafLowerBound(page.data(), lo)), hi);
}

bool BTree::insert(int64_t key, const std::string& payload) {
    if (payload.size() > kMaxPayload) throw std::invalid_argument("payload too large");
    bool duplicate = false;
    std::optional<Split> split = insertInto(root_, key, payload, duplicate);
    if (duplicate) return false;
    if (split) growRoot(*split);
    addToRowCount(1);
    return true;
}

std::optional<BTree::Split> BTree::insertInto(PageId id, int64_t key, const std::string& payload,
                                              bool& duplicate) {
    PageRef page = pager_.fetch(id);
    if (nodeType(page.data()) == kLeaf) return insertIntoLeaf(page, key, payload, duplicate);
    return insertIntoInternal(page, key, payload, duplicate);
}

std::optional<BTree::Split> BTree::insertIntoLeaf(PageRef& page, int64_t key,
                                                  const std::string& payload, bool& duplicate) {
    size_t pos = leafLowerBound(page.data(), key);
    if (pos < cellCount(page.data()) && leafKey(page.data(), pos) == key) {
        duplicate = true;
        return std::nullopt;
    }
    LeafNode leaf = decodeLeaf(page.data());
    leaf.keys.insert(leaf.keys.begin() + pos, key);
    leaf.payloads.insert(leaf.payloads.begin() + pos, payload);
    size_t count = leaf.keys.size();
    if (leafBytes(leaf, 0, count) <= kPageSize) {
        encodeLeaf(page.mutableData(), leaf, 0, count, leaf.next);
        return std::nullopt;
    }

    size_t splitAt = leafSplitPoint(leaf, pos);
    PageRef right = pager_.allocate();
    encodeLeaf(right.mutableData(), leaf, splitAt, count, leaf.next);
    encodeLeaf(page.mutableData(), leaf, 0, splitAt, right.id());
    return Split{leaf.keys[splitAt], right.id()};
}

std::optional<BTree::Split> BTree::insertIntoInternal(PageRef& page, int64_t key,
                                                      const std::string& payload,
                                                      bool& duplicate) {
    size_t index = internalChildIndex(page.data(), key);
    std::optional<Split> childSplit =
        insertInto(internalChild(page.data(), index), key, payload, duplicate);
    if (!childSplit) return std::nullopt;

    InternalNode node = decodeInternal(page.data());
    node.keys.insert(node.keys.begin() + index, childSplit->separator);
    node.children.insert(node.children.begin() + index + 1, childSplit->right);
    size_t count = node.keys.size();
    if (count <= kMaxInternalKeys) {
        encodeInternal(page.mutableData(), node, 0, count);
        return std::nullopt;
    }

    // The middle key moves up; it separates the two halves and stays in neither.
    size_t mid = count / 2;
    PageRef right = pager_.allocate();
    encodeInternal(right.mutableData(), node, mid + 1, count);
    encodeInternal(page.mutableData(), node, 0, mid);
    return Split{node.keys[mid], right.id()};
}

// The root page id must stay fixed (the catalog stores it), so instead of
// creating a new root, move the old root's contents to a fresh page and turn
// the root into an internal node over that page and the split's right half.
void BTree::growRoot(const Split& split) {
    PageRef root = pager_.fetch(root_);
    PageRef left = pager_.allocate();
    char* leftData = left.mutableData();
    std::memcpy(leftData, root.data(), kPageSize);
    std::memset(leftData + kRowCountOffset, 0, kHeaderSize - kRowCountOffset);

    InternalNode node;
    node.keys = {split.separator};
    node.children = {left.id(), split.right};
    encodeInternal(root.mutableData(), node, 0, 1);
}

bool BTree::replace(int64_t key, const std::string& payload) {
    if (payload.size() > kMaxPayload) throw std::invalid_argument("payload too large");
    PageRef page = pager_.fetch(findLeaf(key));
    size_t pos = leafLowerBound(page.data(), key);
    if (pos >= cellCount(page.data()) || leafKey(page.data(), pos) != key) return false;

    LeafNode leaf = decodeLeaf(page.data());
    leaf.payloads[pos] = payload;
    if (leafBytes(leaf, 0, leaf.keys.size()) <= kPageSize) {
        encodeLeaf(page.mutableData(), leaf, 0, leaf.keys.size(), leaf.next);
        return true;
    }
    // The bigger payload doesn't fit: remove and re-insert, which may split.
    page = PageRef();
    erase(key);
    insert(key, payload);
    return true;
}

bool BTree::erase(int64_t key) {
    PageRef page = pager_.fetch(findLeaf(key));
    size_t pos = leafLowerBound(page.data(), key);
    if (pos >= cellCount(page.data()) || leafKey(page.data(), pos) != key) return false;

    LeafNode leaf = decodeLeaf(page.data());
    leaf.keys.erase(leaf.keys.begin() + pos);
    leaf.payloads.erase(leaf.payloads.begin() + pos);
    encodeLeaf(page.mutableData(), leaf, 0, leaf.keys.size(), leaf.next);
    page = PageRef();
    addToRowCount(-1);
    return true;
}

uint64_t BTree::size() {
    PageRef root = pager_.fetch(root_);
    return load<uint64_t>(root.data() + kRowCountOffset);
}

void BTree::addToRowCount(int64_t delta) {
    PageRef root = pager_.fetch(root_);
    char* p = root.mutableData();
    store<uint64_t>(p + kRowCountOffset, load<uint64_t>(p + kRowCountOffset) + uint64_t(delta));
}

uint64_t BTree::aux() {
    PageRef root = pager_.fetch(root_);
    return load<uint64_t>(root.data() + kAuxOffset);
}

void BTree::setAux(uint64_t value) {
    PageRef root = pager_.fetch(root_);
    store<uint64_t>(root.mutableData() + kAuxOffset, value);
}

void BTree::destroy() {
    destroyPage(root_);
}

void BTree::destroyPage(PageId id) {
    std::vector<PageId> children;
    {
        PageRef page = pager_.fetch(id);
        if (nodeType(page.data()) == kInternal) children = decodeInternal(page.data()).children;
    }
    for (PageId child : children) destroyPage(child);
    pager_.free(id);
}

// ---------- Invariant checking (tests and debugging) ----------

namespace {

struct CheckState {
    explicit CheckState(Pager& p) : pager(p) {}
    Pager& pager;
    int leafDepth = -1;
    std::vector<PageId> leavesInOrder;
    uint64_t cells = 0;
    BTreeShape shape;
    double fillSum = 0;
};

void fail(const std::string& message, PageId id) {
    throw std::logic_error("B+tree invariant violated at page " + std::to_string(id) + ": " +
                           message);
}

// Every key in this subtree must be in [lo, hi).
void checkNode(CheckState& state, PageId id, std::optional<int64_t> lo, std::optional<int64_t> hi,
               int depth) {
    PageRef page = state.pager.fetch(id);
    const char* p = page.data();
    size_t count = cellCount(p);
    auto inBounds = [&](int64_t k) { return (!lo || k >= *lo) && (!hi || k < *hi); };

    if (nodeType(p) == kLeaf) {
        if (state.leafDepth == -1) state.leafDepth = depth;
        if (depth != state.leafDepth) fail("leaves at different depths", id);
        for (size_t i = 0; i < count; ++i) {
            int64_t k = leafKey(p, i);
            if (!inBounds(k)) fail("leaf key " + std::to_string(k) + " outside separator bounds", id);
            if (i > 0 && leafKey(p, i - 1) >= k) fail("leaf keys not strictly increasing", id);
        }
        LeafNode leaf = decodeLeaf(p);
        state.fillSum += double(leafBytes(leaf, 0, count)) / kPageSize;
        state.leavesInOrder.push_back(id);
        state.cells += count;
        ++state.shape.leafPages;
        return;
    }
    if (nodeType(p) != kInternal) fail("unknown node type", id);
    if (count == 0) fail("internal node without keys", id);
    ++state.shape.internalPages;
    for (size_t i = 0; i < count; ++i) {
        int64_t k = internalKey(p, i);
        if (!inBounds(k)) fail("separator outside parent bounds", id);
        if (i > 0 && internalKey(p, i - 1) >= k) fail("separators not strictly increasing", id);
    }
    InternalNode node = decodeInternal(p);
    page = PageRef();  // don't hold pins down the whole tree
    for (size_t i = 0; i <= count; ++i) {
        std::optional<int64_t> childLo = i == 0 ? lo : std::optional<int64_t>(node.keys[i - 1]);
        std::optional<int64_t> childHi = i == count ? hi : std::optional<int64_t>(node.keys[i]);
        checkNode(state, node.children[i], childLo, childHi, depth + 1);
    }
}

}  // namespace

BTreeShape BTree::check() {
    CheckState state(pager_);
    checkNode(state, root_, std::nullopt, std::nullopt, 1);
    for (size_t i = 0; i < state.leavesInOrder.size(); ++i) {
        PageRef page = pager_.fetch(state.leavesInOrder[i]);
        PageId expected = i + 1 < state.leavesInOrder.size() ? state.leavesInOrder[i + 1] : 0;
        if (nextLeaf(page.data()) != expected) fail("broken leaf chain", state.leavesInOrder[i]);
    }
    if (state.cells != size()) fail("row count doesn't match cells", root_);
    state.shape.depth = state.leafDepth;
    state.shape.leafFill = state.fillSum / double(state.shape.leafPages);
    return state.shape;
}

}  // namespace jerryql
