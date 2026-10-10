// Linux arm64 (asm-generic) ABI values the guest uses.
#pragma once

#include "common.h"

namespace rn::lx {

// errno
enum : int {
    EPERM_ = 1, ENOENT_ = 2, ESRCH_ = 3, EINTR_ = 4, EIO_ = 5, ENXIO_ = 6, E2BIG_ = 7,
    ENOEXEC_ = 8, EBADF_ = 9, ECHILD_ = 10, EAGAIN_ = 11, ENOMEM_ = 12, EACCES_ = 13,
    EFAULT_ = 14, EBUSY_ = 16, EEXIST_ = 17, EXDEV_ = 18, ENODEV_ = 19, ENOTDIR_ = 20,
    EISDIR_ = 21, EINVAL_ = 22, ENFILE_ = 23, EMFILE_ = 24, ENOTTY_ = 25, EFBIG_ = 27,
    ENOSPC_ = 28, ESPIPE_ = 29, EROFS_ = 30, EMLINK_ = 31, EPIPE_ = 32, ERANGE_ = 34,
    EDEADLK_ = 35, ENAMETOOLONG_ = 36, ENOSYS_ = 38, ENOTEMPTY_ = 39, ELOOP_ = 40,
    ENODATA_ = 61, ETIME_ = 62, EOVERFLOW_ = 75, ENOTSOCK_ = 88, EDESTADDRREQ_ = 89,
    EMSGSIZE_ = 90, EPROTOTYPE_ = 91, ENOPROTOOPT_ = 92, EPROTONOSUPPORT_ = 93,
    EOPNOTSUPP_ = 95, EAFNOSUPPORT_ = 97, EADDRINUSE_ = 98, EADDRNOTAVAIL_ = 99,
    ENETDOWN_ = 100, ENETUNREACH_ = 101, ECONNABORTED_ = 103, ECONNRESET_ = 104,
    ENOBUFS_ = 105, EISCONN_ = 106, ENOTCONN_ = 107, ETIMEDOUT_ = 110,
    ECONNREFUSED_ = 111, EHOSTUNREACH_ = 113, EALREADY_ = 114, EINPROGRESS_ = 115,
};

// open flags
enum : int {
    O_ACCMODE_ = 3, O_RDONLY_ = 0, O_WRONLY_ = 1, O_RDWR_ = 2,
    O_CREAT_ = 0100, O_EXCL_ = 0200, O_NOCTTY_ = 0400, O_TRUNC_ = 01000,
    O_APPEND_ = 02000, O_NONBLOCK_ = 04000, O_DSYNC_ = 010000,
    O_DIRECTORY_ = 040000, O_NOFOLLOW_ = 0100000, O_DIRECT_ = 0200000,
    O_LARGEFILE_ = 0400000, O_CLOEXEC_ = 02000000, O_PATH_ = 010000000,
    O_TMPFILE_ = 020000000,
};
constexpr int AT_FDCWD_ = -100;
constexpr int AT_SYMLINK_NOFOLLOW_ = 0x100;
constexpr int AT_REMOVEDIR_ = 0x200;
constexpr int AT_EMPTY_PATH_ = 0x1000;

// mmap
enum : int {
    PROT_NONE_ = 0, PROT_READ_ = 1, PROT_WRITE_ = 2, PROT_EXEC_ = 4,
};
enum : int {
    MAP_SHARED_ = 0x01, MAP_PRIVATE_ = 0x02, MAP_FIXED_ = 0x10, MAP_ANONYMOUS_ = 0x20,
    MAP_GROWSDOWN_ = 0x100, MAP_NORESERVE_ = 0x4000, MAP_POPULATE_ = 0x8000,
    MAP_STACK_ = 0x20000, MAP_FIXED_NOREPLACE_ = 0x100000,
};

// file types (st_mode)
enum : u32 {
    S_IFMT_ = 0170000, S_IFSOCK_ = 0140000, S_IFLNK_ = 0120000, S_IFREG_ = 0100000,
    S_IFBLK_ = 0060000, S_IFDIR_ = 0040000, S_IFCHR_ = 0020000, S_IFIFO_ = 0010000,
};

// struct stat (asm-generic, 128 bytes)
struct Stat {
    u64 st_dev;
    u64 st_ino;
    u32 st_mode;
    u32 st_nlink;
    u32 st_uid;
    u32 st_gid;
    u64 st_rdev;
    u64 pad1;
    s64 st_size;
    s32 st_blksize;
    s32 pad2;
    s64 st_blocks;
    s64 st_atime_sec;
    u64 st_atime_nsec;
    s64 st_mtime_sec;
    u64 st_mtime_nsec;
    s64 st_ctime_sec;
    u64 st_ctime_nsec;
    u32 unused4;
    u32 unused5;
};
static_assert(sizeof(Stat) == 128);

struct Timespec {
    s64 tv_sec;
    s64 tv_nsec;
};

struct Statfs {
    u64 f_type;
    u64 f_bsize;
    u64 f_blocks;
    u64 f_bfree;
    u64 f_bavail;
    u64 f_files;
    u64 f_ffree;
    s32 f_fsid[2];
    u64 f_namelen;
    u64 f_frsize;
    u64 f_flags;
    u64 f_spare[4];
};
static_assert(sizeof(Statfs) == 120);

struct Iovec {
    u64 base;
    u64 len;
};

// clone flags
enum : u64 {
    CLONE_VM_ = 0x100, CLONE_FS_ = 0x200, CLONE_FILES_ = 0x400, CLONE_SIGHAND_ = 0x800,
    CLONE_PIDFD_ = 0x1000, CLONE_VFORK_ = 0x4000, CLONE_PARENT_ = 0x8000,
    CLONE_THREAD_ = 0x10000, CLONE_SYSVSEM_ = 0x40000, CLONE_SETTLS_ = 0x80000,
    CLONE_PARENT_SETTID_ = 0x100000, CLONE_CHILD_CLEARTID_ = 0x200000,
    CLONE_CHILD_SETTID_ = 0x1000000,
};

// futex
enum : int {
    FUTEX_WAIT_ = 0, FUTEX_WAKE_ = 1, FUTEX_FD_ = 2, FUTEX_REQUEUE_ = 3,
    FUTEX_CMP_REQUEUE_ = 4, FUTEX_WAKE_OP_ = 5, FUTEX_LOCK_PI_ = 6,
    FUTEX_UNLOCK_PI_ = 7, FUTEX_TRYLOCK_PI_ = 8, FUTEX_WAIT_BITSET_ = 9,
    FUTEX_WAKE_BITSET_ = 10, FUTEX_WAIT_REQUEUE_PI_ = 11, FUTEX_CMP_REQUEUE_PI_ = 12,
    FUTEX_LOCK_PI2_ = 13,
    FUTEX_PRIVATE_FLAG_ = 128, FUTEX_CLOCK_REALTIME_ = 256,
};

// signals
enum : int {
    SIGHUP_ = 1, SIGINT_ = 2, SIGQUIT_ = 3, SIGILL_ = 4, SIGTRAP_ = 5, SIGABRT_ = 6,
    SIGBUS_ = 7, SIGFPE_ = 8, SIGKILL_ = 9, SIGUSR1_ = 10, SIGSEGV_ = 11, SIGUSR2_ = 12,
    SIGPIPE_ = 13, SIGALRM_ = 14, SIGTERM_ = 15, SIGSTKFLT_ = 16, SIGCHLD_ = 17,
    SIGCONT_ = 18, SIGSTOP_ = 19, SIGTSTP_ = 20, SIGTTIN_ = 21, SIGTTOU_ = 22,
    SIGURG_ = 23, SIGXCPU_ = 24, SIGXFSZ_ = 25, SIGVTALRM_ = 26, SIGPROF_ = 27,
    SIGWINCH_ = 28, SIGIO_ = 29, SIGPWR_ = 30, SIGSYS_ = 31, NSIG_ = 64,
};
enum : u64 {
    SA_NOCLDSTOP_ = 1, SA_NOCLDWAIT_ = 2, SA_SIGINFO_ = 4, SA_ONSTACK_ = 0x08000000,
    SA_RESTART_ = 0x10000000, SA_NODEFER_ = 0x40000000, SA_RESETHAND_ = 0x80000000,
    SA_RESTORER_ = 0x04000000,
};
constexpr u64 SIG_DFL_ = 0;
constexpr u64 SIG_IGN_ = 1;

struct KSigaction {  // kernel struct sigaction on arm64
    u64 handler;
    u64 flags;
    u64 restorer;
    u64 mask;
};

// clocks
enum : int {
    CLOCK_REALTIME_ = 0, CLOCK_MONOTONIC_ = 1, CLOCK_PROCESS_CPUTIME_ID_ = 2,
    CLOCK_THREAD_CPUTIME_ID_ = 3, CLOCK_MONOTONIC_RAW_ = 4, CLOCK_REALTIME_COARSE_ = 5,
    CLOCK_MONOTONIC_COARSE_ = 6, CLOCK_BOOTTIME_ = 7, CLOCK_REALTIME_ALARM_ = 8,
    CLOCK_BOOTTIME_ALARM_ = 9,
};

// auxv
enum : u64 {
    AT_NULL_ = 0, AT_IGNORE_ = 1, AT_EXECFD_ = 2, AT_PHDR_ = 3, AT_PHENT_ = 4,
    AT_PHNUM_ = 5, AT_PAGESZ_ = 6, AT_BASE_ = 7, AT_FLAGS_ = 8, AT_ENTRY_ = 9,
    AT_NOTELF_ = 10, AT_UID_ = 11, AT_EUID_ = 12, AT_GID_ = 13, AT_EGID_ = 14,
    AT_PLATFORM_ = 15, AT_HWCAP_ = 16, AT_CLKTCK_ = 17, AT_SECURE_ = 23,
    AT_BASE_PLATFORM_ = 24, AT_RANDOM_ = 25, AT_HWCAP2_ = 26, AT_EXECFN_ = 31,
    AT_SYSINFO_EHDR_ = 33, AT_MINSIGSTKSZ_ = 51,
};

// AT_HWCAP bits (arm64)
enum : u64 {
    HWCAP_FP_ = 1 << 0, HWCAP_ASIMD_ = 1 << 1, HWCAP_EVTSTRM_ = 1 << 2,
    HWCAP_AES_ = 1 << 3, HWCAP_PMULL_ = 1 << 4, HWCAP_SHA1_ = 1 << 5,
    HWCAP_SHA2_ = 1 << 6, HWCAP_CRC32_ = 1 << 7, HWCAP_ATOMICS_ = 1 << 8,
    HWCAP_FPHP_ = 1 << 9, HWCAP_ASIMDHP_ = 1 << 10, HWCAP_CPUID_ = 1 << 11,
    HWCAP_ASIMDRDM_ = 1 << 12, HWCAP_JSCVT_ = 1 << 13, HWCAP_FCMA_ = 1 << 14,
    HWCAP_LRCPC_ = 1 << 15, HWCAP_DCPOP_ = 1 << 16, HWCAP_ASIMDDP_ = 1 << 20,
};

}  // namespace rn::lx
