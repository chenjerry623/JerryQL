#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <random>
#include <stdexcept>
#include <vector>

#include "storage/file.h"

namespace jerryql::crash {

// Thrown by every FaultFile operation once the simulated power cut happens.
struct PowerCut : std::runtime_error {
    PowerCut() : std::runtime_error("simulated power loss") {}
};

// Counts file operations across all files of one database, and cuts the
// power at a chosen operation.
struct PowerSwitch {
    int64_t operationsLeft = -1;  // -1 = never
    int64_t operations = 0;
    bool cut = false;

    void tick() {
        if (cut) throw PowerCut();
        ++operations;
        if (operationsLeft >= 0 && operationsLeft-- == 0) {
            cut = true;
            throw PowerCut();
        }
    }
};

// A disk with a volatile write cache. Writes and truncates are visible to
// reads at once but only become durable at sync(). After a power cut,
// crashImage() builds what the disk might hold: the durable contents plus an
// arbitrary subset of the unsynced writes, each one applied, lost, or torn
// at a 512-byte sector boundary. Lost earlier writes with later ones applied
// also model reordering.
class FaultDisk {
public:
    void read(uint64_t offset, char* out, size_t n) const {
        for (size_t i = 0; i < n; ++i) out[i] = offset + i < current_.size() ? current_[offset + i] : 0;
    }
    void write(uint64_t offset, const char* data, size_t n) {
        Op op{false, offset, std::vector<char>(data, data + n)};
        apply(current_, op, n);
        pending_.push_back(std::move(op));
    }
    void truncate(uint64_t size) {
        Op op{true, size, {}};
        apply(current_, op, 0);
        pending_.push_back(std::move(op));
    }
    void sync() {
        durable_ = current_;
        pending_.clear();
    }
    uint64_t size() const { return current_.size(); }

    std::vector<char> crashImage(std::mt19937_64& rng) const {
        std::vector<char> image = durable_;
        for (const Op& op : pending_) {
            switch (rng() % 4) {
                case 0:
                case 1:
                    apply(image, op, op.data.size());
                    break;
                case 2:
                    break;  // lost
                default: {
                    size_t sectors = op.data.size() / 512;
                    size_t keep = sectors == 0 ? 0 : size_t(rng() % sectors) * 512;
                    apply(image, op, keep);  // torn: only the first `keep` bytes landed
                    break;
                }
            }
        }
        return image;
    }

private:
    struct Op {
        bool isTruncate;
        uint64_t offset;  // for a truncate: the new size
        std::vector<char> data;
    };

    static void apply(std::vector<char>& bytes, const Op& op, size_t length) {
        if (op.isTruncate) {
            bytes.resize(op.offset);
            return;
        }
        if (length == 0) return;
        if (op.offset + length > bytes.size()) bytes.resize(op.offset + length);
        std::copy(op.data.begin(), op.data.begin() + long(length), bytes.begin() + long(op.offset));
    }

    std::vector<char> current_;
    std::vector<char> durable_;
    std::vector<Op> pending_;
};

// File handle onto a shared FaultDisk, so the disk outlives the Database.
class FaultFile : public File {
public:
    FaultFile(std::shared_ptr<FaultDisk> disk, std::shared_ptr<PowerSwitch> power)
        : disk_(std::move(disk)), power_(std::move(power)) {}

    void read(uint64_t offset, char* out, size_t n) override {
        if (power_->cut) throw PowerCut();
        disk_->read(offset, out, n);
    }
    void write(uint64_t offset, const char* data, size_t n) override {
        power_->tick();
        disk_->write(offset, data, n);
    }
    uint64_t size() const override { return disk_->size(); }
    void truncate(uint64_t size) override {
        power_->tick();
        disk_->truncate(size);
    }
    void sync() override {
        power_->tick();
        disk_->sync();
    }

private:
    std::shared_ptr<FaultDisk> disk_;
    std::shared_ptr<PowerSwitch> power_;
};

}  // namespace jerryql::crash
