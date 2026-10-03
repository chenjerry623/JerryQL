#pragma once

#include <memory>

#include "storage/buffer_pool.h"
#include "storage/file.h"

namespace jerryql {

// Owns the database file and its buffer pool, and hands out pages.
//
// File layout: page 0 is the header, page 1 is the root of the schema tree,
// and the rest are B+tree nodes or free pages. Header fields:
//   [0, 8)   magic "JERRYQL1"
//   [8, 12)  page size (4096)
//   [12, 16) page count (pages ever allocated, including free ones)
//   [16, 20) first page of the freelist (0 = empty)
// A free page stores the next free page id at bytes [4, 8).
// Integers are stored in host byte order (little-endian on x86 and wasm).
class Pager {
public:
    static constexpr PageId kHeaderPage = 0;
    static constexpr PageId kSchemaRootPage = 1;

    Pager(std::unique_ptr<File> file, size_t poolPages);

    // True if the file was empty when opened; the caller creates the schema tree.
    bool isNew() const { return isNew_; }

    PageRef fetch(PageId id);
    PageRef allocate();         // reuses a free page if any; returned zero-filled
    void free(PageId id);
    void flush();               // writes dirty pages and the header, then syncs

    uint32_t pageCount() const { return pageCount_; }
    uint32_t freePageCount();  // walks the freelist
    BufferPool& pool() { return pool_; }

private:
    void readHeader();
    void writeHeader();

    std::unique_ptr<File> file_;
    BufferPool pool_;
    bool isNew_ = false;
    uint32_t pageCount_ = 1;
    PageId freeHead_ = 0;
};

}  // namespace jerryql
