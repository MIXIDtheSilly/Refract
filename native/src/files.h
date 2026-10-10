// Guest file descriptors and the objects behind them.
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common.h"
#include "linux_abi.h"

namespace rn {

// poll(2) event bits
enum : u32 {
    kPollIn = 0x001, kPollPri = 0x002, kPollOut = 0x004, kPollErr = 0x008,
    kPollHup = 0x010, kPollNval = 0x020, kPollRdNorm = 0x040, kPollWrNorm = 0x100,
};

class FileObject {
public:
    virtual ~FileObject() = default;

    std::string path;  // guest path (for /proc/self/fd and maps)
    u64 inode = 0;
    int status_flags = 0;  // O_APPEND, O_NONBLOCK, access mode

    virtual s64 Read(void* buf, u64 n) { return -lx::EINVAL_; }
    virtual s64 Write(const void* buf, u64 n) { return -lx::EINVAL_; }
    virtual s64 Pread(void* buf, u64 n, u64 off) { return -lx::ESPIPE_; }
    virtual s64 Pwrite(const void* buf, u64 n, u64 off) { return -lx::ESPIPE_; }
    virtual s64 Seek(s64 off, int whence) { return -lx::ESPIPE_; }
    virtual int Stat(lx::Stat* st);
    virtual s64 Getdents(void* buf, u64 n) { return -lx::ENOTDIR_; }
    virtual s64 Ioctl(u64 req, u64 arg) { return -lx::ENOTTY_; }
    virtual int Truncate(u64 len) { return -lx::EINVAL_; }
    virtual int Sync() { return 0; }
    virtual bool IsDirectory() const { return false; }
    // Current readiness (kPoll* bits).
    virtual u32 Poll() { return kPollIn | kPollOut; }
};

using FilePtr = std::shared_ptr<FileObject>;

class FdTable {
public:
    int Install(FilePtr f, bool cloexec, int min_fd = 0);
    int InstallAt(int fd, FilePtr f, bool cloexec);
    FilePtr Get(int fd);
    int Close(int fd);
    bool GetCloexec(int fd);
    void SetCloexec(int fd, bool v);
    std::vector<int> List();

private:
    std::mutex mu_;
    std::vector<FilePtr> files_;
    std::vector<bool> cloexec_;
};

FdTable& Fds();

// Concrete objects -------------------------------------------------------

// Read-only in-memory content (/proc files, generated configs).
class MemFile : public FileObject {
public:
    explicit MemFile(std::string content, u32 mode = lx::S_IFREG_ | 0444)
        : content_(std::move(content)), mode_(mode) {}
    s64 Read(void* buf, u64 n) override;
    s64 Pread(void* buf, u64 n, u64 off) override;
    s64 Seek(s64 off, int whence) override;
    int Stat(lx::Stat* st) override;

private:
    std::mutex mu_;
    std::string content_;
    u64 pos_ = 0;
    u32 mode_;
};

// A regular host file.
class HostFile : public FileObject {
public:
    HostFile(void* handle, bool append) : h_(handle), append_(append) {}
    ~HostFile() override;
    s64 Read(void* buf, u64 n) override;
    s64 Write(const void* buf, u64 n) override;
    s64 Pread(void* buf, u64 n, u64 off) override;
    s64 Pwrite(const void* buf, u64 n, u64 off) override;
    s64 Seek(s64 off, int whence) override;
    int Stat(lx::Stat* st) override;
    int Truncate(u64 len) override;
    int Sync() override;

private:
    void* h_;
    bool append_;
    std::mutex mu_;
    u64 pos_ = 0;
};

// A directory: host entries merged with virtual ones.
class DirFile : public FileObject {
public:
    struct Entry {
        std::string name;
        u8 type;  // DT_*
        u64 ino;
    };
    explicit DirFile(std::vector<Entry> entries) : entries_(std::move(entries)) {}
    s64 Getdents(void* buf, u64 n) override;
    s64 Seek(s64 off, int whence) override;
    int Stat(lx::Stat* st) override;
    bool IsDirectory() const override { return true; }

private:
    std::mutex mu_;
    std::vector<Entry> entries_;
    size_t pos_ = 0;
};

class StdioFile : public FileObject {
public:
    explicit StdioFile(int which) : which_(which) {}
    s64 Read(void* buf, u64 n) override;
    s64 Write(const void* buf, u64 n) override;
    int Stat(lx::Stat* st) override;
    s64 Ioctl(u64 req, u64 arg) override;

private:
    int which_;
};

class DevFile : public FileObject {
public:
    enum Kind { Null, Zero, Random };
    explicit DevFile(Kind k) : kind_(k) {}
    s64 Read(void* buf, u64 n) override;
    s64 Write(const void* buf, u64 n) override { return static_cast<s64>(n); }
    s64 Pread(void* buf, u64 n, u64) override { return Read(buf, n); }
    s64 Seek(s64, int) override { return 0; }
    int Stat(lx::Stat* st) override;

private:
    Kind kind_;
};

u64 HashInode(const std::string& s);
// Map a Win32 error to a Linux errno (positive).
int WinErrToErrno(unsigned long err);

}  // namespace rn
