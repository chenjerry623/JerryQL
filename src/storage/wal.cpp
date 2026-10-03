#include "storage/wal.h"

#include <chrono>
#include <cstring>
#include <random>
#include <stdexcept>
#include <vector>

namespace jerryql {

namespace {

constexpr char kMagic[8] = {'J', 'Q', 'L', 'W', 'A', 'L', '0', '1'};
constexpr uint32_t kVersion = 1;

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

uint64_t seedFromSalt(uint64_t salt) {
    return walChecksum(0x6a65727279716cULL, reinterpret_cast<const char*>(&salt), sizeof(salt));
}

uint64_t freshSalt() {
    std::random_device device;
    uint64_t salt = (uint64_t(device()) << 32) ^ device();
    return salt ^ uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
}

// Checksum of a frame: its first 16 header bytes (page id, commit, salt)
// followed by the page bytes.
uint64_t frameChecksum(uint64_t seed, const char* frame) {
    uint64_t sum = walChecksum(seed, frame, 16);
    return walChecksum(sum, frame + Wal::kFrameHeaderSize, kPageSize);
}

}  // namespace

// Mixes 8 bytes at a time with a multiply and rotate (FNV-style), then the
// tail bytes. Not cryptographic; it only needs to catch torn or stale data.
uint64_t walChecksum(uint64_t seed, const char* data, size_t n) {
    uint64_t h = seed ^ 0xcbf29ce484222325ULL;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        h ^= load<uint64_t>(data + i);
        h *= 0x100000001b3ULL;
        h = (h << 29) | (h >> 35);
    }
    for (; i < n; ++i) {
        h ^= uint8_t(data[i]);
        h *= 0x100000001b3ULL;
    }
    return h ^ (h >> 31);
}

void Wal::writeHeader() {
    char header[kHeaderSize] = {};
    std::memcpy(header, kMagic, sizeof(kMagic));
    store<uint32_t>(header + 8, uint32_t(kPageSize));
    store<uint32_t>(header + 12, kVersion);
    store<uint64_t>(header + 16, salt_);
    store<uint64_t>(header + 24, walChecksum(0, header, 24));
    file_.write(0, header, kHeaderSize);
}

Wal::Recovery Wal::open() {
    Recovery recovery;
    char header[kHeaderSize];
    file_.read(0, header, kHeaderSize);
    bool valid = file_.size() >= kHeaderSize && std::memcmp(header, kMagic, sizeof(kMagic)) == 0 &&
                 load<uint32_t>(header + 8) == kPageSize &&
                 load<uint64_t>(header + 24) == walChecksum(0, header, 24);
    if (!valid) {
        reset();  // missing, empty or torn header: no committed frames
        return recovery;
    }
    salt_ = load<uint64_t>(header + 16);

    std::unordered_map<PageId, uint64_t> pending;
    uint64_t checksum = seedFromSalt(salt_);
    uint64_t offset = kHeaderSize;
    uint64_t lastCommitEnd = kHeaderSize, lastCommitChecksum = checksum;
    std::vector<char> frame(kFrameSize);
    uint64_t sinceCommit = 0;
    while (offset + kFrameSize <= file_.size()) {
        file_.read(offset, frame.data(), kFrameSize);
        if (load<uint64_t>(frame.data() + 8) != salt_) break;
        uint64_t expected = frameChecksum(checksum, frame.data());
        if (load<uint64_t>(frame.data() + 16) != expected) break;
        checksum = expected;
        pending[load<PageId>(frame.data())] = offset;
        ++sinceCommit;
        offset += kFrameSize;
        uint32_t commitPageCount = load<uint32_t>(frame.data() + 4);
        if (commitPageCount != 0) {
            for (const auto& entry : pending) recovery.pages[entry.first] = entry.second;
            pending.clear();
            recovery.pageCount = commitPageCount;
            recovery.frames += sinceCommit;
            sinceCommit = 0;
            lastCommitEnd = offset;
            lastCommitChecksum = checksum;
        }
    }
    recovery.discardedFrames = sinceCommit;
    committedEnd_ = appendOffset_ = lastCommitEnd;
    committedChecksum_ = lastChecksum_ = lastCommitChecksum;
    return recovery;
}

uint64_t Wal::append(PageId id, const char* page, uint32_t commitPageCount) {
    std::vector<char> frame(kFrameSize);
    store<PageId>(frame.data(), id);
    store<uint32_t>(frame.data() + 4, commitPageCount);
    store<uint64_t>(frame.data() + 8, salt_);
    std::memcpy(frame.data() + kFrameHeaderSize, page, kPageSize);
    lastChecksum_ = frameChecksum(lastChecksum_, frame.data());
    store<uint64_t>(frame.data() + 16, lastChecksum_);
    uint64_t offset = appendOffset_;
    file_.write(offset, frame.data(), kFrameSize);
    appendOffset_ += kFrameSize;
    return offset;
}

void Wal::readPage(uint64_t frameOffset, char* out) {
    file_.read(frameOffset + kFrameHeaderSize, out, kPageSize);
}

void Wal::sync() {
    file_.sync();
}

void Wal::markCommitted() {
    committedEnd_ = appendOffset_;
    committedChecksum_ = lastChecksum_;
}

void Wal::rewindToCommitted() {
    appendOffset_ = committedEnd_;
    lastChecksum_ = committedChecksum_;
}

void Wal::reset() {
    salt_ = salt_ == 0 ? freshSalt() : salt_ + 1;
    writeHeader();
    file_.truncate(kHeaderSize);
    appendOffset_ = committedEnd_ = kHeaderSize;
    lastChecksum_ = committedChecksum_ = seedFromSalt(salt_);
}

}  // namespace jerryql
