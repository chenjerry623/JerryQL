#include "storage/file.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace jerryql {

namespace {

[[noreturn]] void throwErrno(const std::string& what, const std::string& path) {
    throw std::runtime_error(what + " " + path + ": " + std::strerror(errno));
}

}  // namespace

// ---------- MemoryFile ----------

void MemoryFile::read(uint64_t offset, char* out, size_t n) {
    std::memset(out, 0, n);
    if (offset >= bytes_.size()) return;
    size_t available = std::min<uint64_t>(n, bytes_.size() - offset);
    std::memcpy(out, bytes_.data() + offset, available);
}

void MemoryFile::write(uint64_t offset, const char* data, size_t n) {
    if (offset + n > bytes_.size()) bytes_.resize(offset + n);
    std::memcpy(bytes_.data() + offset, data, n);
}

// ---------- PosixFile ----------

PosixFile::PosixFile(const std::string& path) : path_(path) {
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) throwErrno("cannot open", path);
}

PosixFile::~PosixFile() {
    if (fd_ >= 0) ::close(fd_);
}

void PosixFile::read(uint64_t offset, char* out, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t got = ::pread(fd_, out + done, n - done, static_cast<off_t>(offset + done));
        if (got < 0) {
            if (errno == EINTR) continue;
            throwErrno("read failed on", path_);
        }
        if (got == 0) {  // past end of file
            std::memset(out + done, 0, n - done);
            return;
        }
        done += static_cast<size_t>(got);
    }
}

void PosixFile::write(uint64_t offset, const char* data, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t put = ::pwrite(fd_, data + done, n - done, static_cast<off_t>(offset + done));
        if (put < 0) {
            if (errno == EINTR) continue;
            throwErrno("write failed on", path_);
        }
        done += static_cast<size_t>(put);
    }
}

uint64_t PosixFile::size() const {
    struct stat info;
    if (::fstat(fd_, &info) != 0) throwErrno("stat failed on", path_);
    return static_cast<uint64_t>(info.st_size);
}

void PosixFile::sync() {
    if (::fsync(fd_) != 0) throwErrno("fsync failed on", path_);
}

}  // namespace jerryql
