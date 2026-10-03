#include "storage/pager.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace jerryql {

namespace {

constexpr char kMagic[8] = {'J', 'E', 'R', 'R', 'Y', 'Q', 'L', '2'};  // 2: byte-string keys

uint32_t readU32(const char* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

void writeU32(char* p, uint32_t v) {
    std::memcpy(p, &v, 4);
}

}  // namespace

Pager::Pager(std::unique_ptr<File> dbFile, std::unique_ptr<File> walFile, PagerOptions options)
    : dbFile_(std::move(dbFile)),
      walFile_(std::move(walFile)),
      options_(options),
      wal_(*walFile_),
      pool_(*this, options.poolPages) {
    Wal::Recovery recovery = wal_.open();
    committedFrames_ = std::move(recovery.pages);
    stats_.recoveredFrames = recovery.frames;
    stats_.discardedFrames = recovery.discardedFrames;

    isNew_ = dbFile_->size() == 0 && committedFrames_.empty();
    if (isNew_) {
        writeHeader();
        return;
    }
    readHeader();
    // Recovery: move committed frames into the database file right away, so
    // the log only ever holds work from this session.
    if (!committedFrames_.empty()) checkpoint();
}

// ---------- Page I/O for the buffer pool ----------

void Pager::readPage(PageId id, char* out) {
    auto pending = pendingFrames_.find(id);
    if (pending != pendingFrames_.end()) return wal_.readPage(pending->second, out);
    auto committed = committedFrames_.find(id);
    if (committed != committedFrames_.end()) return wal_.readPage(committed->second, out);
    dbFile_->read(uint64_t(id) * kPageSize, out, kPageSize);
}

// A dirty page evicted before commit: log it as an uncommitted frame.
void Pager::writePage(PageId id, const char* data) {
    pendingFrames_[id] = wal_.append(id, data, 0);
    ++stats_.framesWritten;
}

// ---------- Header ----------

void Pager::readHeader() {
    PageRef header = pool_.fetch(kHeaderPage);
    const char* p = header.data();
    if (std::memcmp(p, kMagic, sizeof(kMagic)) != 0) {
        throw std::runtime_error("not a JerryQL database file");
    }
    if (readU32(p + 8) != kPageSize) throw std::runtime_error("unsupported page size");
    pageCount_ = readU32(p + 12);
    freeHead_ = readU32(p + 16);
    if (pageCount_ < 2) throw std::runtime_error("corrupt database header");
}

void Pager::writeHeader() {
    PageRef header = pool_.fetch(kHeaderPage);
    const char* current = header.data();
    bool unchanged = std::memcmp(current, kMagic, sizeof(kMagic)) == 0 &&
                     readU32(current + 12) == pageCount_ && readU32(current + 16) == freeHead_;
    if (unchanged) return;  // don't dirty page 0 for nothing
    char* p = header.mutableData();
    std::memcpy(p, kMagic, sizeof(kMagic));
    writeU32(p + 8, kPageSize);
    writeU32(p + 12, pageCount_);
    writeU32(p + 16, freeHead_);
}

// ---------- Pages ----------

PageRef Pager::fetch(PageId id) {
    if (id == kHeaderPage || id >= pageCount_) {
        throw std::runtime_error("invalid page id " + std::to_string(id));
    }
    return pool_.fetch(id);
}

PageRef Pager::allocate() {
    if (freeHead_ != 0) {
        PageId id = freeHead_;
        PageRef page = pool_.fetch(id);
        freeHead_ = readU32(page.data() + 4);
        std::memset(page.mutableData(), 0, kPageSize);
        return page;
    }
    return pool_.fetchNew(pageCount_++);
}

void Pager::free(PageId id) {
    PageRef page = fetch(id);
    char* p = page.mutableData();
    std::memset(p, 0, kPageSize);
    writeU32(p + 4, freeHead_);
    freeHead_ = id;
}

uint32_t Pager::freePageCount() {
    uint32_t count = 0;
    for (PageId id = freeHead_; id != 0; ++count) {
        PageRef page = pool_.fetch(id);
        id = readU32(page.data() + 4);
    }
    return count;
}

// ---------- Transactions ----------

// Appends every dirty page to the log, with the header page last as the
// commit frame, and fsyncs the log. After the fsync returns the transaction
// survives a crash; until then recovery ignores all of its frames.
void Pager::commit() {
    writeHeader();
    if (!pool_.hasDirtyPages() && pendingFrames_.empty()) return;  // read-only

    // The header page is always part of a commit: it carries the commit flag.
    { PageRef header = pool_.fetch(kHeaderPage); header.mutableData(); }
    std::vector<std::pair<PageId, const char*>> dirty;
    pool_.forEachDirty([&](PageId id, const char* data) {
        if (id != kHeaderPage) dirty.emplace_back(id, data);
    });
    for (const auto& [id, data] : dirty) pendingFrames_[id] = wal_.append(id, data, 0);
    {
        PageRef header = pool_.fetch(kHeaderPage);
        pendingFrames_[kHeaderPage] = wal_.append(kHeaderPage, header.data(), pageCount_);
    }
    stats_.framesWritten += dirty.size() + 1;
    if (options_.syncOnCommit) wal_.sync();

    wal_.markCommitted();
    pool_.markAllClean();
    for (const auto& entry : pendingFrames_) committedFrames_[entry.first] = entry.second;
    pendingFrames_.clear();
    ++stats_.commits;

    if (wal_.committedFrames() >= options_.checkpointFrames) checkpoint();
}

void Pager::rollback() {
    pool_.discardDirty();
    // Clean cached copies of pages re-read from this transaction's frames
    // are just as uncommitted.
    for (const auto& entry : pendingFrames_) pool_.discard(entry.first);
    pendingFrames_.clear();
    wal_.rewindToCommitted();
    pool_.discard(kHeaderPage);
    readHeader();
}

// Order matters for crash safety: the database file is fsynced before the
// log is emptied, so at every instant either the log or the file holds each
// committed page. Replaying a log twice is harmless (full page images).
void Pager::checkpoint() {
    if (pool_.hasDirtyPages() || !pendingFrames_.empty()) {
        throw std::logic_error("checkpoint with uncommitted changes");
    }
    std::vector<std::pair<PageId, uint64_t>> frames(committedFrames_.begin(), committedFrames_.end());
    std::sort(frames.begin(), frames.end());
    std::vector<char> page(kPageSize);
    for (const auto& [id, offset] : frames) {
        wal_.readPage(offset, page.data());
        dbFile_->write(uint64_t(id) * kPageSize, page.data(), kPageSize);
    }
    dbFile_->sync();
    wal_.reset();
    wal_.sync();
    committedFrames_.clear();
    ++stats_.checkpoints;
}

}  // namespace jerryql
