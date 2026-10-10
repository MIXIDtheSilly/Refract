#include "files.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstring>

namespace rn {

using namespace lx;

u64 HashInode(const std::string& s) {
    u64 h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return (h & 0x7fffffffffffull) | 1;
}

int WinErrToErrno(unsigned long err) {
    switch (err) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
    case ERROR_BAD_NETPATH:
        return ENOENT_;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return EACCES_;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        return EEXIST_;
    case ERROR_DIR_NOT_EMPTY:
        return ENOTEMPTY_;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return ENOSPC_;
    case ERROR_DIRECTORY:
        return ENOTDIR_;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return ENOMEM_;
    case ERROR_INVALID_HANDLE:
        return EBADF_;
    case ERROR_BROKEN_PIPE:
    case ERROR_NO_DATA:
        return EPIPE_;
    case ERROR_FILENAME_EXCED_RANGE:
        return ENAMETOOLONG_;
    default:
        return EIO_;
    }
}

static s64 FileTimeToUnixNs(const LARGE_INTEGER& ft) {
    // 100ns ticks since 1601 -> ns since 1970
    return (ft.QuadPart - 116444736000000000ll) * 100;
}

// FdTable ------------------------------------------------------------------

FdTable& Fds() {
    static FdTable t;
    return t;
}

int FdTable::Install(FilePtr f, bool cloexec, int min_fd) {
    std::lock_guard lock(mu_);
    for (size_t i = min_fd; i < 65536; ++i) {
        if (i >= files_.size()) {
            files_.resize(i + 1);
            cloexec_.resize(i + 1);
        }
        if (!files_[i]) {
            files_[i] = std::move(f);
            cloexec_[i] = cloexec;
            return static_cast<int>(i);
        }
    }
    return -EMFILE_;
}

int FdTable::InstallAt(int fd, FilePtr f, bool cloexec) {
    if (fd < 0 || fd >= 65536)
        return -EBADF_;
    std::lock_guard lock(mu_);
    if (static_cast<size_t>(fd) >= files_.size()) {
        files_.resize(fd + 1);
        cloexec_.resize(fd + 1);
    }
    files_[fd] = std::move(f);
    cloexec_[fd] = cloexec;
    return fd;
}

FilePtr FdTable::Get(int fd) {
    std::lock_guard lock(mu_);
    if (fd < 0 || static_cast<size_t>(fd) >= files_.size())
        return nullptr;
    return files_[fd];
}

int FdTable::Close(int fd) {
    FilePtr victim;
    {
        std::lock_guard lock(mu_);
        if (fd < 0 || static_cast<size_t>(fd) >= files_.size() || !files_[fd])
            return -EBADF_;
        victim = std::move(files_[fd]);
        files_[fd] = nullptr;
    }
    return 0;  // victim released outside the lock
}

bool FdTable::GetCloexec(int fd) {
    std::lock_guard lock(mu_);
    return fd >= 0 && static_cast<size_t>(fd) < cloexec_.size() && cloexec_[fd];
}

void FdTable::SetCloexec(int fd, bool v) {
    std::lock_guard lock(mu_);
    if (fd >= 0 && static_cast<size_t>(fd) < cloexec_.size())
        cloexec_[fd] = v;
}

std::vector<int> FdTable::List() {
    std::lock_guard lock(mu_);
    std::vector<int> out;
    for (size_t i = 0; i < files_.size(); ++i)
        if (files_[i])
            out.push_back(static_cast<int>(i));
    return out;
}

// FileObject -----------------------------------------------------------------

int FileObject::Stat(lx::Stat* st) {
    memset(st, 0, sizeof(*st));
    st->st_ino = inode;
    st->st_mode = S_IFREG_ | 0644;
    st->st_nlink = 1;
    st->st_blksize = 4096;
    return 0;
}

// MemFile ----------------------------------------------------------------------

s64 MemFile::Read(void* buf, u64 n) {
    std::lock_guard lock(mu_);
    if (pos_ >= content_.size())
        return 0;
    u64 take = std::min<u64>(n, content_.size() - pos_);
    if (!SafeCopyToGuest(reinterpret_cast<u64>(buf), content_.data() + pos_, take))
        return -EFAULT_;
    pos_ += take;
    return static_cast<s64>(take);
}

s64 MemFile::Pread(void* buf, u64 n, u64 off) {
    if (off >= content_.size())
        return 0;
    u64 take = std::min<u64>(n, content_.size() - off);
    if (!SafeCopyToGuest(reinterpret_cast<u64>(buf), content_.data() + off, take))
        return -EFAULT_;
    return static_cast<s64>(take);
}

s64 MemFile::Seek(s64 off, int whence) {
    std::lock_guard lock(mu_);
    s64 base = whence == 0 ? 0 : whence == 1 ? static_cast<s64>(pos_)
                                             : static_cast<s64>(content_.size());
    if (whence > 2 || base + off < 0)
        return -EINVAL_;
    pos_ = base + off;
    return static_cast<s64>(pos_);
}

int MemFile::Stat(lx::Stat* st) {
    FileObject::Stat(st);
    st->st_mode = mode_;
    st->st_size = static_cast<s64>(content_.size());
    return 0;
}

// HostFile ---------------------------------------------------------------------

HostFile::~HostFile() {
    if (h_ && h_ != INVALID_HANDLE_VALUE)
        CloseHandle(h_);
}

s64 HostFile::Pread(void* buf, u64 n, u64 off) {
    u64 done = 0;
    while (done < n) {
        DWORD chunk = static_cast<DWORD>(std::min<u64>(n - done, 1u << 30));
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(off + done);
        ov.OffsetHigh = static_cast<DWORD>((off + done) >> 32);
        DWORD got = 0;
        if (!ReadFile(h_, static_cast<char*>(buf) + done, chunk, &got, &ov)) {
            DWORD err = GetLastError();
            if (err == ERROR_HANDLE_EOF)
                break;
            if (err == ERROR_NOACCESS)
                return done ? static_cast<s64>(done) : -EFAULT_;
            return done ? static_cast<s64>(done) : -WinErrToErrno(err);
        }
        done += got;
        if (got < chunk)
            break;
    }
    return static_cast<s64>(done);
}

s64 HostFile::Pwrite(const void* buf, u64 n, u64 off) {
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(off);
    ov.OffsetHigh = static_cast<DWORD>(off >> 32);
    DWORD put = 0;
    if (!WriteFile(h_, buf, static_cast<DWORD>(std::min<u64>(n, 1u << 30)), &put, &ov)) {
        DWORD err = GetLastError();
        return err == ERROR_NOACCESS ? -EFAULT_ : -WinErrToErrno(err);
    }
    return put;
}

s64 HostFile::Read(void* buf, u64 n) {
    std::lock_guard lock(mu_);
    s64 r = Pread(buf, n, pos_);
    if (r > 0)
        pos_ += r;
    return r;
}

s64 HostFile::Write(const void* buf, u64 n) {
    std::lock_guard lock(mu_);
    if (append_) {
        LARGE_INTEGER size;
        if (GetFileSizeEx(h_, &size))
            pos_ = size.QuadPart;
    }
    s64 r = Pwrite(buf, n, pos_);
    if (r > 0)
        pos_ += r;
    return r;
}

s64 HostFile::Seek(s64 off, int whence) {
    std::lock_guard lock(mu_);
    s64 base;
    if (whence == 0) {
        base = 0;
    } else if (whence == 1) {
        base = static_cast<s64>(pos_);
    } else if (whence == 2) {
        LARGE_INTEGER size;
        if (!GetFileSizeEx(h_, &size))
            return -EIO_;
        base = size.QuadPart;
    } else {
        return -EINVAL_;  // SEEK_DATA/SEEK_HOLE unsupported
    }
    if (base + off < 0)
        return -EINVAL_;
    pos_ = base + off;
    return static_cast<s64>(pos_);
}

int HostFile::Stat(lx::Stat* st) {
    FileObject::Stat(st);
    FILE_BASIC_INFO basic{};
    FILE_STANDARD_INFO std_info{};
    if (GetFileInformationByHandleEx(h_, FileBasicInfo, &basic, sizeof(basic))) {
        s64 m = FileTimeToUnixNs(basic.LastWriteTime);
        s64 a = FileTimeToUnixNs(basic.LastAccessTime);
        s64 c = FileTimeToUnixNs(basic.ChangeTime);
        st->st_mtime_sec = m / 1000000000;
        st->st_mtime_nsec = m % 1000000000;
        st->st_atime_sec = a / 1000000000;
        st->st_atime_nsec = a % 1000000000;
        st->st_ctime_sec = c / 1000000000;
        st->st_ctime_nsec = c % 1000000000;
    }
    if (GetFileInformationByHandleEx(h_, FileStandardInfo, &std_info, sizeof(std_info))) {
        st->st_size = std_info.EndOfFile.QuadPart;
        st->st_blocks = (std_info.AllocationSize.QuadPart + 511) / 512;
    }
    st->st_mode = S_IFREG_ | 0755;
    st->st_dev = 0xfe2a;
    return 0;
}

int HostFile::Truncate(u64 len) {
    FILE_END_OF_FILE_INFO info{};
    info.EndOfFile.QuadPart = static_cast<LONGLONG>(len);
    if (!SetFileInformationByHandle(h_, FileEndOfFileInfo, &info, sizeof(info)))
        return -WinErrToErrno(GetLastError());
    return 0;
}

int HostFile::Sync() {
    FlushFileBuffers(h_);
    return 0;
}

// DirFile ----------------------------------------------------------------------

s64 DirFile::Getdents(void* buf, u64 n) {
    std::lock_guard lock(mu_);
    u64 used = 0;
    std::vector<u8> tmp;
    while (pos_ < entries_.size()) {
        const Entry& e = entries_[pos_];
        u64 reclen = (19 + e.name.size() + 1 + 7) & ~7ull;
        if (used + reclen > n)
            break;
        tmp.assign(reclen, 0);
        u64 ino = e.ino;
        s64 off = static_cast<s64>(pos_ + 1);
        u16 rl = static_cast<u16>(reclen);
        memcpy(&tmp[0], &ino, 8);
        memcpy(&tmp[8], &off, 8);
        memcpy(&tmp[16], &rl, 2);
        tmp[18] = e.type;
        memcpy(&tmp[19], e.name.data(), e.name.size());
        if (!SafeCopyToGuest(reinterpret_cast<u64>(buf) + used, tmp.data(), reclen))
            return -EFAULT_;
        used += reclen;
        ++pos_;
    }
    if (used == 0 && pos_ < entries_.size())
        return -EINVAL_;
    return static_cast<s64>(used);
}

s64 DirFile::Seek(s64 off, int whence) {
    std::lock_guard lock(mu_);
    if (whence != 0 || off < 0)
        return -EINVAL_;
    pos_ = static_cast<size_t>(off);
    return off;
}

int DirFile::Stat(lx::Stat* st) {
    FileObject::Stat(st);
    st->st_mode = S_IFDIR_ | 0755;
    st->st_nlink = 2;
    st->st_size = 4096;
    return 0;
}

// StdioFile ---------------------------------------------------------------------

s64 StdioFile::Read(void* buf, u64 n) {
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD got = 0;
    if (!h || !ReadFile(h, buf, static_cast<DWORD>(n), &got, nullptr))
        return 0;
    return got;
}

s64 StdioFile::Write(const void* buf, u64 n) {
    HANDLE h = GetStdHandle(which_ == 2 ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    DWORD put = 0;
    if (!h || !WriteFile(h, buf, static_cast<DWORD>(n), &put, nullptr))
        return static_cast<s64>(n);
    return put;
}

int StdioFile::Stat(lx::Stat* st) {
    FileObject::Stat(st);
    st->st_mode = S_IFCHR_ | 0620;
    st->st_rdev = 0x8800 + which_;
    return 0;
}

s64 StdioFile::Ioctl(u64 req, u64 arg) {
    return -ENOTTY_;
}

// DevFile -----------------------------------------------------------------------

s64 DevFile::Read(void* buf, u64 n) {
    if (kind_ == Null)
        return 0;
    std::vector<u8> tmp(std::min<u64>(n, 1 << 20));
    if (kind_ == Random)
        BCryptGenRandom(nullptr, tmp.data(), static_cast<ULONG>(tmp.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!SafeCopyToGuest(reinterpret_cast<u64>(buf), tmp.data(), tmp.size()))
        return -EFAULT_;
    return static_cast<s64>(tmp.size());
}

int DevFile::Stat(lx::Stat* st) {
    FileObject::Stat(st);
    st->st_mode = S_IFCHR_ | 0666;
    st->st_rdev = kind_ == Null ? 0x103 : kind_ == Zero ? 0x105 : 0x109;
    return 0;
}

}  // namespace rn
