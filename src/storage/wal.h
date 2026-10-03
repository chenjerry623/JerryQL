#pragma once

#include <cstdint>
#include <unordered_map>

#include "storage/buffer_pool.h"
#include "storage/file.h"

namespace jerryql {

// Write-ahead log of full page images ("frames"), in the style of SQLite's
// WAL mode. Redo-only: a transaction is committed once its frames, ending in
// a frame marked as a commit, are fsynced. Nothing in the main database file
// changes until a checkpoint copies committed pages back.
//
// File layout:
//   header (32 bytes): magic "JQLWAL01", u32 page size, u32 version,
//                      u64 salt, u64 header checksum
//   frame  (24 + 4096): u32 page id, u32 commit (database page count if this
//                      frame ends a transaction, else 0), u64 salt,
//                      u64 checksum, page bytes
//
// Checksums are chained: each frame's checksum covers its header fields and
// page bytes, seeded with the previous frame's checksum (the first frame is
// seeded from the salt). Recovery stops at the first frame whose salt or
// checksum doesn't match, which catches torn writes, stale frames left over
// from a rolled-back transaction, and frames from before the last reset.
class Wal {
public:
    static constexpr uint64_t kHeaderSize = 32;
    static constexpr uint64_t kFrameHeaderSize = 24;
    static constexpr uint64_t kFrameSize = kFrameHeaderSize + kPageSize;

    struct Recovery {
        std::unordered_map<PageId, uint64_t> pages;  // page -> offset of its newest committed frame
        uint32_t pageCount = 0;                      // from the last commit frame; 0 if none
        uint64_t frames = 0;                         // committed frames found
        uint64_t discardedFrames = 0;                // valid frames after the last commit
    };

    explicit Wal(File& file) : file_(file) {}

    // Reads an existing log (or starts a fresh one) and returns the committed
    // state. Positions the log to append after the last committed frame.
    Recovery open();

    // Appends a frame and returns its offset. commitPageCount != 0 marks the
    // end of a transaction.
    uint64_t append(PageId id, const char* page, uint32_t commitPageCount);
    void readPage(uint64_t frameOffset, char* out);
    void sync();

    // Remembers the current end of the log as the last commit point, or
    // rewinds appends to it (rollback). Frames past it are dead.
    void markCommitted();
    void rewindToCommitted();

    // Empties the log under a new salt (after a checkpoint).
    void reset();

    uint64_t committedFrames() const { return (committedEnd_ - kHeaderSize) / kFrameSize; }

private:
    void writeHeader();

    File& file_;
    uint64_t salt_ = 0;
    uint64_t appendOffset_ = kHeaderSize;
    uint64_t lastChecksum_ = 0;
    uint64_t committedEnd_ = kHeaderSize;
    uint64_t committedChecksum_ = 0;
};

// 64-bit checksum used by the WAL; chained through `seed`.
uint64_t walChecksum(uint64_t seed, const char* data, size_t n);

}  // namespace jerryql
