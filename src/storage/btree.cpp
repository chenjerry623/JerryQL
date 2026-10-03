#include "storage/btree.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace jerryql {

// ---------- Keys ----------

int compareKeys(std::string_view a, std::string_view b) {
    size_t common = std::min(a.size(), b.size());
    int c = common == 0 ? 0 : std::memcmp(a.data(), b.data(), common);
    if (c != 0) return c < 0 ? -1 : 1;
    if (a.size() == b.size()) return 0;
    return a.size() < b.size() ? -1 : 1;
}

// Big-endian with the sign bit flipped: memcmp order == signed integer order.
std::string encodeIntKey(int64_t value) {
    uint64_t bits = uint64_t(value) ^ (uint64_t(1) << 63);
    std::string key(8, '\0');
    for (int i = 7; i >= 0; --i) {
        key[size_t(i)] = char(bits & 0xff);
        bits >>= 8;
    }
    return key;
}

int64_t decodeIntKey(std::string_view key) {
    uint64_t bits = 0;
    for (size_t i = 0; i < 8; ++i) bits = (bits << 8) | uint8_t(key[i]);
    return int64_t(bits ^ (uint64_t(1) << 63));
}

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

constexpr size_t kSlotSize = 2;              // u16 cell offset
constexpr size_t kLeafCellOverhead = 4;      // u16 key length, u16 payload length
constexpr size_t kChild0Offset = kHeaderSize;
constexpr size_t kInternalSlotsOffset = kHeaderSize + 4;
constexpr size_t kInternalCellOverhead = 6;  // u32 child, u16 key length

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

const char* leafCell(const char* page, size_t i) {
    return page + load<uint16_t>(page + kHeaderSize + i * kSlotSize);
}

std::string_view leafKey(const char* page, size_t i) {
    const char* cell = leafCell(page, i);
    return {cell + kLeafCellOverhead, load<uint16_t>(cell)};
}

std::string_view leafPayload(const char* page, size_t i) {
    const char* cell = leafCell(page, i);
    uint16_t keyLength = load<uint16_t>(cell);
    return {cell + kLeafCellOverhead + keyLength, load<uint16_t>(cell + 2)};
}

// Index of the first cell with key >= target.
size_t leafLowerBound(const char* page, std::string_view target) {
    size_t lo = 0, hi = cellCount(page);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (compareKeys(leafKey(page, mid), target) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

const char* internalCell(const char* page, size_t i) {
    return page + load<uint16_t>(page + kInternalSlotsOffset + i * kSlotSize);
}

std::string_view internalKey(const char* page, size_t i) {
    const char* cell = internalCell(page, i);
    return {cell + kInternalCellOverhead, load<uint16_t>(cell + 4)};
}

PageId internalChild(const char* page, size_t i) {
    if (i == 0) return load<PageId>(page + kChild0Offset);
    return load<PageId>(internalCell(page, i - 1));
}

// Index of the child whose range contains target: the number of keys <= target.
size_t internalChildIndex(const char* page, std::string_view target) {
    size_t lo = 0, hi = cellCount(page);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (compareKeys(internalKey(page, mid), target) <= 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

// ---------- Decoded nodes (used on the write path) ----------

struct LeafNode {
    std::vector<std::string> keys;
    std::vector<std::string> payloads;
    PageId next = 0;
};

struct InternalNode {
    std::vector<std::string> keys;
    std::vector<PageId> children;  // keys.size() + 1 entries
};

size_t leafCellBytes(const LeafNode& leaf, size_t i) {
    return kSlotSize + kLeafCellOverhead + leaf.keys[i].size() + leaf.payloads[i].size();
}

size_t leafBytes(const LeafNode& leaf, size_t begin, size_t end) {
    size_t bytes = kHeaderSize;
    for (size_t i = begin; i < end; ++i) bytes += leafCellBytes(leaf, i);
    return bytes;
}

size_t internalCellBytes(const InternalNode& node, size_t i) {
    return kSlotSize + kInternalCellOverhead + node.keys[i].size();
}

size_t internalBytes(const InternalNode& node, size_t begin, size_t end) {
    size_t bytes = kInternalSlotsOffset;
    for (size_t i = begin; i < end; ++i) bytes += internalCellBytes(node, i);
    return bytes;
}

LeafNode decodeLeaf(const char* page) {
    LeafNode leaf;
    size_t count = cellCount(page);
    leaf.keys.reserve(count + 1);
    leaf.payloads.reserve(count + 1);
    for (size_t i = 0; i < count; ++i) {
        leaf.keys.emplace_back(leafKey(page, i));
        leaf.payloads.emplace_back(leafPayload(page, i));
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
        const std::string& key = leaf.keys[i];
        const std::string& payload = leaf.payloads[i];
        contentStart -= kLeafCellOverhead + key.size() + payload.size();
        char* cell = page + contentStart;
        store<uint16_t>(cell, uint16_t(key.size()));
        store<uint16_t>(cell + 2, uint16_t(payload.size()));
        std::memcpy(cell + kLeafCellOverhead, key.data(), key.size());
        std::memcpy(cell + kLeafCellOverhead + key.size(), payload.data(), payload.size());
        store<uint16_t>(page + kHeaderSize + (i - begin) * kSlotSize, uint16_t(contentStart));
    }
}

InternalNode decodeInternal(const char* page) {
    InternalNode node;
    size_t count = cellCount(page);
    node.keys.reserve(count + 1);
    node.children.reserve(count + 2);
    for (size_t i = 0; i < count; ++i) node.keys.emplace_back(internalKey(page, i));
    for (size_t i = 0; i <= count; ++i) node.children.push_back(internalChild(page, i));
    return node;
}

// Writes keys [begin, end) and children [begin, end]. Leaves root-only fields alone.
void encodeInternal(char* page, const InternalNode& node, size_t begin, size_t end) {
    if (internalBytes(node, begin, end) > kPageSize) throw std::logic_error("internal node overflow");
    store<uint8_t>(page + kTypeOffset, kInternal);
    store<uint16_t>(page + kCountOffset, uint16_t(end - begin));
    store<PageId>(page + kNextOffset, 0);
    store<PageId>(page + kChild0Offset, node.children[begin]);
    size_t contentStart = kPageSize;
    for (size_t i = begin; i < end; ++i) {
        const std::string& key = node.keys[i];
        contentStart -= kInternalCellOverhead + key.size();
        char* cell = page + contentStart;
        store<PageId>(cell, node.children[i + 1]);
        store<uint16_t>(cell + 4, uint16_t(key.size()));
        std::memcpy(cell + kInternalCellOverhead, key.data(), key.size());
        store<uint16_t>(page + kInternalSlotsOffset + (i - begin) * kSlotSize, uint16_t(contentStart));
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
        running += leafCellBytes(leaf, i);
        if (running * 2 >= total) return std::clamp<size_t>(i + 1, 1, count - 1);
    }
    return count / 2;
}

// The key that moves up when an internal node splits: the one at the byte
// midpoint. It separates the halves and stays in neither.
size_t internalSplitPoint(const InternalNode& node) {
    size_t count = node.keys.size();
    size_t total = internalBytes(node, 0, count) - kInternalSlotsOffset;
    size_t running = 0;
    for (size_t i = 0; i < count; ++i) {
        running += internalCellBytes(node, i);
        if (running * 2 >= total) return std::clamp<size_t>(i, 1, count - 2);
    }
    return count / 2;
}

void checkSizes(std::string_view key, std::string_view payload) {
    if (key.size() > kMaxKeySize) throw std::invalid_argument("key too large");
    if (payload.size() > kMaxPayload) throw std::invalid_argument("payload too large");
}

}  // namespace

// ---------- Cursor ----------

BTreeCursor::BTreeCursor(Pager& pager, PageId leaf, uint16_t slot, std::optional<std::string> hi)
    : pager_(pager), leaf_(leaf), slot_(slot), hi_(std::move(hi)) {}

bool BTreeCursor::nextRaw(std::string_view& key, std::string_view& payload) {
    while (leaf_ != 0) {
        if (!page_.valid()) page_ = pager_.fetch(leaf_);
        const char* p = page_.data();
        if (slot_ < cellCount(p)) {
            std::string_view k = leafKey(p, slot_);
            if (hi_ && compareKeys(k, *hi_) > 0) break;
            key = k;
            payload = leafPayload(p, slot_);
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

bool BTreeCursor::next(std::string& key, std::string& payload) {
    std::string_view k, v;
    if (!nextRaw(k, v)) return false;
    key.assign(k);
    payload.assign(v);
    return true;
}

// ---------- BTree ----------

PageId BTree::create(Pager& pager) {
    PageRef page = pager.allocate();
    LeafNode empty;
    encodeLeaf(page.mutableData(), empty, 0, 0, 0);
    return page.id();
}

PageId BTree::findLeaf(std::string_view key) {
    PageId id = root_;
    while (true) {
        PageRef page = pager_.fetch(id);
        if (nodeType(page.data()) == kLeaf) return id;
        id = internalChild(page.data(), internalChildIndex(page.data(), key));
    }
}

std::optional<std::string> BTree::find(std::string_view key) {
    PageRef page = pager_.fetch(findLeaf(key));
    size_t i = leafLowerBound(page.data(), key);
    if (i < cellCount(page.data()) && compareKeys(leafKey(page.data(), i), key) == 0) {
        return std::string(leafPayload(page.data(), i));
    }
    return std::nullopt;
}

BTreeCursor BTree::scan(std::string_view lo, std::optional<std::string> hi) {
    if (hi && compareKeys(lo, *hi) > 0) return BTreeCursor(pager_, 0, 0, std::move(hi));
    PageId leaf = findLeaf(lo);
    PageRef page = pager_.fetch(leaf);
    return BTreeCursor(pager_, leaf, uint16_t(leafLowerBound(page.data(), lo)), std::move(hi));
}

bool BTree::insert(std::string_view key, std::string_view payload) {
    checkSizes(key, payload);
    bool duplicate = false;
    std::optional<Split> split = insertInto(root_, key, payload, duplicate);
    if (duplicate) return false;
    if (split) growRoot(*split);
    addToRowCount(1);
    return true;
}

std::optional<BTree::Split> BTree::insertInto(PageId id, std::string_view key,
                                              std::string_view payload, bool& duplicate) {
    PageRef page = pager_.fetch(id);
    if (nodeType(page.data()) == kLeaf) return insertIntoLeaf(page, key, payload, duplicate);
    return insertIntoInternal(page, key, payload, duplicate);
}

std::optional<BTree::Split> BTree::insertIntoLeaf(PageRef& page, std::string_view key,
                                                  std::string_view payload, bool& duplicate) {
    size_t pos = leafLowerBound(page.data(), key);
    if (pos < cellCount(page.data()) && compareKeys(leafKey(page.data(), pos), key) == 0) {
        duplicate = true;
        return std::nullopt;
    }
    LeafNode leaf = decodeLeaf(page.data());
    leaf.keys.emplace(leaf.keys.begin() + long(pos), key);
    leaf.payloads.emplace(leaf.payloads.begin() + long(pos), payload);
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

std::optional<BTree::Split> BTree::insertIntoInternal(PageRef& page, std::string_view key,
                                                      std::string_view payload, bool& duplicate) {
    size_t index = internalChildIndex(page.data(), key);
    std::optional<Split> childSplit =
        insertInto(internalChild(page.data(), index), key, payload, duplicate);
    if (!childSplit) return std::nullopt;

    InternalNode node = decodeInternal(page.data());
    node.keys.insert(node.keys.begin() + long(index), std::move(childSplit->separator));
    node.children.insert(node.children.begin() + long(index) + 1, childSplit->right);
    size_t count = node.keys.size();
    if (internalBytes(node, 0, count) <= kPageSize) {
        encodeInternal(page.mutableData(), node, 0, count);
        return std::nullopt;
    }

    size_t mid = internalSplitPoint(node);
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

bool BTree::replace(std::string_view key, std::string_view payload) {
    checkSizes(key, payload);
    PageRef page = pager_.fetch(findLeaf(key));
    size_t pos = leafLowerBound(page.data(), key);
    if (pos >= cellCount(page.data()) || compareKeys(leafKey(page.data(), pos), key) != 0) return false;

    LeafNode leaf = decodeLeaf(page.data());
    leaf.payloads[pos].assign(payload);
    if (leafBytes(leaf, 0, leaf.keys.size()) <= kPageSize) {
        encodeLeaf(page.mutableData(), leaf, 0, leaf.keys.size(), leaf.next);
        return true;
    }
    // The bigger payload doesn't fit: remove and re-insert, which may split.
    page = PageRef();
    std::string keyCopy(key), payloadCopy(payload);  // key may point into the page
    erase(keyCopy);
    insert(keyCopy, payloadCopy);
    return true;
}

bool BTree::erase(std::string_view key) {
    PageRef page = pager_.fetch(findLeaf(key));
    size_t pos = leafLowerBound(page.data(), key);
    if (pos >= cellCount(page.data()) || compareKeys(leafKey(page.data(), pos), key) != 0) return false;

    LeafNode leaf = decodeLeaf(page.data());
    leaf.keys.erase(leaf.keys.begin() + long(pos));
    leaf.payloads.erase(leaf.payloads.begin() + long(pos));
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
void checkNode(CheckState& state, PageId id, const std::optional<std::string>& lo,
               const std::optional<std::string>& hi, int depth) {
    PageRef page = state.pager.fetch(id);
    const char* p = page.data();
    size_t count = cellCount(p);
    auto inBounds = [&](std::string_view k) {
        return (!lo || compareKeys(k, *lo) >= 0) && (!hi || compareKeys(k, *hi) < 0);
    };

    if (nodeType(p) == kLeaf) {
        if (state.leafDepth == -1) state.leafDepth = depth;
        if (depth != state.leafDepth) fail("leaves at different depths", id);
        for (size_t i = 0; i < count; ++i) {
            if (!inBounds(leafKey(p, i))) fail("leaf key outside separator bounds", id);
            if (i > 0 && compareKeys(leafKey(p, i - 1), leafKey(p, i)) >= 0) {
                fail("leaf keys not strictly increasing", id);
            }
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
        if (!inBounds(internalKey(p, i))) fail("separator outside parent bounds", id);
        if (i > 0 && compareKeys(internalKey(p, i - 1), internalKey(p, i)) >= 0) {
            fail("separators not strictly increasing", id);
        }
    }
    InternalNode node = decodeInternal(p);
    page = PageRef();  // don't hold pins down the whole tree
    for (size_t i = 0; i <= count; ++i) {
        std::optional<std::string> childLo = i == 0 ? lo : std::optional<std::string>(node.keys[i - 1]);
        std::optional<std::string> childHi = i == count ? hi : std::optional<std::string>(node.keys[i]);
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
