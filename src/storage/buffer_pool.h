#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "storage/file.h"

namespace jerryql {

using PageId = uint32_t;
constexpr size_t kPageSize = 4096;

struct BufferPoolStats {
    uint64_t hits = 0;        // fetch found the page in memory
    uint64_t misses = 0;      // fetch had to read the page from the file
    uint64_t writeBacks = 0;  // dirty pages written to the file
};

// Where the buffer pool reads missing pages from and writes evicted dirty
// pages to. The pager routes these through the write-ahead log.
class PageIO {
public:
    virtual ~PageIO() = default;
    virtual void readPage(PageId id, char* out) = 0;
    virtual void writePage(PageId id, const char* data) = 0;
};

// PageIO straight onto a file (page id * page size). Used in tests.
class FilePageIO : public PageIO {
public:
    explicit FilePageIO(File& file) : file_(file) {}
    void readPage(PageId id, char* out) override { file_.read(uint64_t(id) * kPageSize, out, kPageSize); }
    void writePage(PageId id, const char* data) override {
        file_.write(uint64_t(id) * kPageSize, data, kPageSize);
    }

private:
    File& file_;
};

class BufferPool;

// A pinned page. While a PageRef exists its frame can't be evicted; the pin
// is released when the PageRef is destroyed. Writing through mutableData()
// marks the page dirty so it is written back before eviction.
class PageRef {
public:
    PageRef() = default;
    PageRef(BufferPool* pool, size_t frame) : pool_(pool), frame_(frame) {}
    PageRef(PageRef&& other) noexcept;
    PageRef& operator=(PageRef&& other) noexcept;
    PageRef(const PageRef&) = delete;
    PageRef& operator=(const PageRef&) = delete;
    ~PageRef() { release(); }

    bool valid() const { return pool_ != nullptr; }
    PageId id() const;
    const char* data() const;
    char* mutableData();

private:
    void release();
    BufferPool* pool_ = nullptr;
    size_t frame_ = 0;
};

// Fixed-size cache of pages with LRU eviction. Only unpinned frames are in
// the LRU list, so finding a victim is O(1). Single-threaded.
class BufferPool {
public:
    BufferPool(PageIO& io, size_t capacity);

    PageRef fetch(PageId id);     // reads through PageIO on a miss
    PageRef fetchNew(PageId id);  // zero-filled and dirty; skips the read

    bool hasDirtyPages() const;
    // Calls fn(id, data) for each dirty page, in page id order.
    void forEachDirty(const std::function<void(PageId, const char*)>& fn) const;
    void markAllClean();
    void writeBackAll();          // writes every dirty page through PageIO
    // Drops a cached page without writing it back (used by rollback).
    void discard(PageId id);
    void discardDirty();

    size_t capacity() const { return frames_.size(); }
    const BufferPoolStats& stats() const { return stats_; }

private:
    friend class PageRef;

    struct Frame {
        PageId id = 0;
        bool used = false;
        bool dirty = false;
        int pins = 0;
        bool inLru = false;                       // true when unpinned and evictable
        std::list<size_t>::iterator lruPosition;  // valid only when inLru
        std::unique_ptr<char[]> data;
    };

    size_t frameFor(PageId id, bool readFromFile);
    size_t takeVictim();
    void writeBack(Frame& frame);
    void pin(size_t frame);
    void unpin(size_t frame);

    void dropFrame(size_t index);

    PageIO& io_;
    std::vector<Frame> frames_;
    std::unordered_map<PageId, size_t> pageTable_;
    std::list<size_t> lru_;  // unpinned frames, most recently used first
    std::vector<size_t> freeFrames_;
    BufferPoolStats stats_;
};

}  // namespace jerryql
