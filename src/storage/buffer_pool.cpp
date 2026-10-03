#include "storage/buffer_pool.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace jerryql {

// ---------- PageRef ----------

PageRef::PageRef(PageRef&& other) noexcept : pool_(other.pool_), frame_(other.frame_) {
    other.pool_ = nullptr;
}

PageRef& PageRef::operator=(PageRef&& other) noexcept {
    if (this != &other) {
        release();
        pool_ = other.pool_;
        frame_ = other.frame_;
        other.pool_ = nullptr;
    }
    return *this;
}

void PageRef::release() {
    if (pool_) pool_->unpin(frame_);
    pool_ = nullptr;
}

PageId PageRef::id() const {
    return pool_->frames_[frame_].id;
}

const char* PageRef::data() const {
    return pool_->frames_[frame_].data.get();
}

char* PageRef::mutableData() {
    BufferPool::Frame& frame = pool_->frames_[frame_];
    frame.dirty = true;
    return frame.data.get();
}

// ---------- BufferPool ----------

BufferPool::BufferPool(PageIO& io, size_t capacity) : io_(io), frames_(capacity) {
    if (capacity < 8) throw std::invalid_argument("buffer pool needs at least 8 frames");
    for (size_t i = 0; i < capacity; ++i) {
        frames_[i].data = std::make_unique<char[]>(kPageSize);
        freeFrames_.push_back(capacity - 1 - i);
    }
}

PageRef BufferPool::fetch(PageId id) {
    return PageRef(this, frameFor(id, true));
}

PageRef BufferPool::fetchNew(PageId id) {
    size_t index = frameFor(id, false);
    std::memset(frames_[index].data.get(), 0, kPageSize);
    frames_[index].dirty = true;
    return PageRef(this, index);
}

size_t BufferPool::frameFor(PageId id, bool readFromFile) {
    auto hit = pageTable_.find(id);
    if (hit != pageTable_.end()) {
        ++stats_.hits;
        pin(hit->second);
        return hit->second;
    }
    ++stats_.misses;
    size_t index = takeVictim();
    Frame& frame = frames_[index];
    frame.id = id;
    frame.used = true;
    frame.dirty = false;
    frame.pins = 0;
    if (readFromFile) io_.readPage(id, frame.data.get());
    pageTable_[id] = index;
    pin(index);
    return index;
}

size_t BufferPool::takeVictim() {
    if (!freeFrames_.empty()) {
        size_t index = freeFrames_.back();
        freeFrames_.pop_back();
        return index;
    }
    if (lru_.empty()) throw std::runtime_error("buffer pool exhausted: every page is pinned");
    size_t index = lru_.back();
    lru_.pop_back();
    Frame& frame = frames_[index];
    frame.inLru = false;
    if (frame.dirty) writeBack(frame);
    pageTable_.erase(frame.id);
    frame.used = false;
    return index;
}

void BufferPool::writeBack(Frame& frame) {
    io_.writePage(frame.id, frame.data.get());
    frame.dirty = false;
    ++stats_.writeBacks;
}

void BufferPool::pin(size_t index) {
    Frame& frame = frames_[index];
    if (frame.inLru) {
        lru_.erase(frame.lruPosition);
        frame.inLru = false;
    }
    ++frame.pins;
}

void BufferPool::unpin(size_t index) {
    Frame& frame = frames_[index];
    if (--frame.pins == 0) {
        lru_.push_front(index);
        frame.lruPosition = lru_.begin();
        frame.inLru = true;
    }
}

bool BufferPool::hasDirtyPages() const {
    for (const Frame& frame : frames_) {
        if (frame.used && frame.dirty) return true;
    }
    return false;
}

void BufferPool::forEachDirty(const std::function<void(PageId, const char*)>& fn) const {
    std::vector<const Frame*> dirty;
    for (const Frame& frame : frames_) {
        if (frame.used && frame.dirty) dirty.push_back(&frame);
    }
    std::sort(dirty.begin(), dirty.end(),
              [](const Frame* a, const Frame* b) { return a->id < b->id; });
    for (const Frame* frame : dirty) fn(frame->id, frame->data.get());
}

void BufferPool::markAllClean() {
    for (Frame& frame : frames_) frame.dirty = false;
}

void BufferPool::writeBackAll() {
    for (Frame& frame : frames_) {
        if (frame.used && frame.dirty) writeBack(frame);
    }
}

void BufferPool::dropFrame(size_t index) {
    Frame& frame = frames_[index];
    if (frame.pins != 0) throw std::logic_error("cannot discard a pinned page");
    if (frame.inLru) {
        lru_.erase(frame.lruPosition);
        frame.inLru = false;
    }
    pageTable_.erase(frame.id);
    frame.used = false;
    frame.dirty = false;
    freeFrames_.push_back(index);
}

void BufferPool::discard(PageId id) {
    auto it = pageTable_.find(id);
    if (it != pageTable_.end()) dropFrame(it->second);
}

void BufferPool::discardDirty() {
    for (size_t i = 0; i < frames_.size(); ++i) {
        if (frames_[i].used && frames_[i].dirty) dropFrame(i);
    }
}

}  // namespace jerryql
