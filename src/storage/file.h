#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jerryql {

// Byte-addressed storage underneath the pager. PosixFile is a real file;
// MemoryFile backs in-memory databases (the browser demo, most tests) with
// the same code path as on-disk ones.
class File {
public:
    virtual ~File() = default;
    // Reads exactly n bytes; bytes past the end of the file read as zero.
    virtual void read(uint64_t offset, char* out, size_t n) = 0;
    virtual void write(uint64_t offset, const char* data, size_t n) = 0;
    virtual uint64_t size() const = 0;
    virtual void truncate(uint64_t size) = 0;
    // Durably persists everything written so far (fsync for real files).
    virtual void sync() = 0;
};

class MemoryFile : public File {
public:
    void read(uint64_t offset, char* out, size_t n) override;
    void write(uint64_t offset, const char* data, size_t n) override;
    uint64_t size() const override { return bytes_.size(); }
    void truncate(uint64_t size) override { bytes_.resize(size); }
    void sync() override {}

    const std::vector<char>& bytes() const { return bytes_; }
    std::vector<char>& bytes() { return bytes_; }

private:
    std::vector<char> bytes_;
};

class PosixFile : public File {
public:
    // Opens or creates the file. Throws std::runtime_error on failure.
    explicit PosixFile(const std::string& path);
    ~PosixFile() override;
    PosixFile(const PosixFile&) = delete;
    PosixFile& operator=(const PosixFile&) = delete;

    void read(uint64_t offset, char* out, size_t n) override;
    void write(uint64_t offset, const char* data, size_t n) override;
    uint64_t size() const override;
    void truncate(uint64_t size) override;
    void sync() override;

private:
    std::string path_;
    int fd_ = -1;
};

// fsyncs a directory, so a file just created in it survives a crash.
void syncDirectoryOf(const std::string& filePath);
bool fileExists(const std::string& path);

}  // namespace jerryql
