#pragma once

#include <memory>
#include <unordered_map>

#include "storage/buffer_pool.h"
#include "storage/file.h"
#include "storage/wal.h"

namespace jerryql {

struct PagerOptions {
    size_t poolPages = 1024;
    // Checkpoint once the log holds this many committed frames.
    uint64_t checkpointFrames = 1000;
    // false skips the fsync at commit: faster, but a crash can lose recent
    // commits (never corrupts). Like SQLite's synchronous=OFF.
    bool syncOnCommit = true;
};

struct PagerStats {
    uint64_t commits = 0;
    uint64_t framesWritten = 0;
    uint64_t checkpoints = 0;
    uint64_t recoveredFrames = 0;   // committed frames replayed when the file was opened
    uint64_t discardedFrames = 0;   // uncommitted frames thrown away at open
};

// Owns the database file, its write-ahead log and the buffer pool.
//
// Database file: page 0 is the header, page 1 the schema tree root, the rest
// B+tree nodes or free pages. Header fields:
//   [0, 8)   magic "JERRYQL1"
//   [8, 12)  page size (4096)
//   [12, 16) page count (pages ever allocated, including free ones)
//   [16, 20) first page of the freelist (0 = empty)
// A free page stores the next free page id at bytes [4, 8). Integers are in
// host byte order (little-endian on x86 and wasm).
//
// Pages are read from, in order of preference: the buffer pool, this
// transaction's frames in the log, committed frames in the log, then the
// database file. Dirty pages evicted mid-transaction become uncommitted log
// frames, so a transaction may be larger than the buffer pool.
class Pager : private PageIO {
public:
    static constexpr PageId kHeaderPage = 0;
    static constexpr PageId kSchemaRootPage = 1;

    // Opens the files and runs crash recovery: committed log frames are
    // checkpointed into the database file; anything after the last commit
    // is discarded.
    Pager(std::unique_ptr<File> dbFile, std::unique_ptr<File> walFile, PagerOptions options = {});

    // True if there was no database yet; the caller creates the schema tree.
    bool isNew() const { return isNew_; }

    PageRef fetch(PageId id);
    PageRef allocate();         // reuses a free page if any; returned zero-filled
    void free(PageId id);

    // Makes every change since the last commit durable (one fsync of the log),
    // then checkpoints if the log has grown past the threshold.
    void commit();
    // Discards every change since the last commit.
    void rollback();
    // Copies committed pages into the database file and empties the log.
    void checkpoint();

    uint32_t pageCount() const { return pageCount_; }
    uint32_t freePageCount();  // walks the freelist
    BufferPool& pool() { return pool_; }
    const PagerStats& stats() const { return stats_; }

private:
    void readPage(PageId id, char* out) override;
    void writePage(PageId id, const char* data) override;

    void readHeader();
    void writeHeader();

    std::unique_ptr<File> dbFile_;
    std::unique_ptr<File> walFile_;
    PagerOptions options_;
    Wal wal_;
    BufferPool pool_;
    std::unordered_map<PageId, uint64_t> committedFrames_;  // page -> newest committed frame
    std::unordered_map<PageId, uint64_t> pendingFrames_;    // page -> frame written this transaction
    bool isNew_ = false;
    uint32_t pageCount_ = 1;
    PageId freeHead_ = 0;
    PagerStats stats_;
};

}  // namespace jerryql
