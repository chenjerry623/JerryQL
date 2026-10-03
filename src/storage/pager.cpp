#include "storage/pager.h"

#include <cstring>
#include <stdexcept>

namespace jerryql {

namespace {

constexpr char kMagic[8] = {'J', 'E', 'R', 'R', 'Y', 'Q', 'L', '1'};

uint32_t readU32(const char* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

void writeU32(char* p, uint32_t v) {
    std::memcpy(p, &v, 4);
}

}  // namespace

Pager::Pager(std::unique_ptr<File> file, size_t poolPages)
    : file_(std::move(file)), pool_(*file_, poolPages) {
    isNew_ = file_->size() == 0;
    if (isNew_) {
        writeHeader();
    } else {
        readHeader();
    }
}

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
    char* p = header.mutableData();
    std::memcpy(p, kMagic, sizeof(kMagic));
    writeU32(p + 8, kPageSize);
    writeU32(p + 12, pageCount_);
    writeU32(p + 16, freeHead_);
}

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

void Pager::flush() {
    writeHeader();
    pool_.flushAll();
    file_->sync();
}

}  // namespace jerryql
