#include "vfs.h"

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <set>

namespace rn {

using namespace lx;

namespace {
constexpr u8 DT_CHR = 2, DT_DIR = 4, DT_REG = 8, DT_LNK = 10;

bool StartsWithDir(const std::string& s, const std::string& prefix) {
    if (prefix == "/")
        return !s.empty() && s[0] == '/';
    return s.size() > prefix.size() && s.compare(0, prefix.size(), prefix) == 0 &&
           s[prefix.size()] == '/';
}

std::string Parent(const std::string& p) {
    size_t slash = p.rfind('/');
    if (slash == 0 || slash == std::string::npos)
        return "/";
    return p.substr(0, slash);
}
}  // namespace

Vfs& Fs() {
    static Vfs v;
    return v;
}

void Vfs::Mount(const std::string& guest_prefix, const std::wstring& host_dir, bool writable) {
    std::lock_guard lock(mu_);
    mounts_.push_back({Normalize(guest_prefix), host_dir, writable});
    // Longest prefix first.
    std::sort(mounts_.begin(), mounts_.end(),
              [](const MountPoint& a, const MountPoint& b) { return a.prefix.size() > b.prefix.size(); });
}

void Vfs::AddSymlink(const std::string& path, const std::string& target) {
    std::lock_guard lock(mu_);
    symlinks_[Normalize(path)] = target;
}

bool Vfs::LoadSymlinkList(const std::wstring& host_file) {
    std::ifstream in(host_file);
    if (!in)
        return false;
    std::string line;
    size_t n = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        size_t tab = line.find('\t');
        if (tab == std::string::npos)
            continue;
        AddSymlink(line.substr(0, tab), line.substr(tab + 1));
        ++n;
    }
    RN_INFO("loaded %zu symlinks", n);
    return true;
}

void Vfs::AddFile(const std::string& path, std::function<std::string()> gen) {
    std::lock_guard lock(mu_);
    files_[Normalize(path)] = std::move(gen);
}

void Vfs::AddGenerator(const std::string& prefix,
                       std::function<std::optional<std::string>(const std::string&)> gen) {
    std::lock_guard lock(mu_);
    generators_.emplace_back(Normalize(prefix), std::move(gen));
}

void Vfs::AddLinkResolver(std::function<std::optional<std::string>(const std::string&)> fn) {
    std::lock_guard lock(mu_);
    link_resolvers_.push_back(std::move(fn));
}

void Vfs::AddDevice(const std::string& path, std::function<FilePtr()> factory) {
    std::lock_guard lock(mu_);
    devices_[Normalize(path)] = std::move(factory);
}

std::string Vfs::Normalize(const std::string& abs) {
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos <= abs.size()) {
        size_t next = abs.find('/', pos);
        if (next == std::string::npos)
            next = abs.size();
        std::string comp = abs.substr(pos, next - pos);
        if (comp == "..") {
            if (!parts.empty())
                parts.pop_back();
        } else if (!comp.empty() && comp != ".") {
            parts.push_back(std::move(comp));
        }
        pos = next + 1;
    }
    std::string out;
    for (const auto& p : parts) {
        out += '/';
        out += p;
    }
    return out.empty() ? "/" : out;
}

std::string Vfs::GuestPathForHost(const std::wstring& host_path) {
    std::wstring h = host_path;
    std::replace(h.begin(), h.end(), L'/', L'\\');
    std::lock_guard lock(mu_);
    const MountPoint* best = nullptr;
    for (const auto& m : mounts_) {
        if (h.size() >= m.host.size() && _wcsnicmp(h.c_str(), m.host.c_str(), m.host.size()) == 0 &&
            (h.size() == m.host.size() || h[m.host.size()] == L'\\') &&
            (!best || m.host.size() > best->host.size()))
            best = &m;
    }
    if (!best)
        return {};
    std::string rest = Narrow(h.substr(best->host.size()));
    std::replace(rest.begin(), rest.end(), '\\', '/');
    return Normalize(best->prefix + rest);
}

std::string Vfs::Absolute(int dirfd, const std::string& path, int* err) {
    *err = 0;
    // Host paths that leaked into the guest (e.g. via java.io.File) map back.
    if (path.size() > 2 && path[1] == ':' && (path[2] == '\\' || path[2] == '/') && isalpha(static_cast<unsigned char>(path[0]))) {
        std::string g = GuestPathForHost(Widen(path));
        if (!g.empty())
            return g;
        // A guest path that Java resolved against the current drive ("C:\data\...").
        std::string rest = path.substr(2);
        std::replace(rest.begin(), rest.end(), '\\', '/');
        for (const char* root : {"/data/", "/storage/", "/sdcard", "/system/", "/apex/", "/vendor/", "/proc/", "/dev/"})
            if (rest.rfind(root, 0) == 0)
                return Normalize(rest);
    }
    if (!path.empty() && path[0] == '\\') {
        std::string fixed = path;
        std::replace(fixed.begin(), fixed.end(), '\\', '/');
        return Normalize(fixed);
    }
    if (!path.empty() && path[0] == '/')
        return Normalize(path);
    std::string base;
    if (dirfd == AT_FDCWD_) {
        base = Cwd();
    } else {
        FilePtr f = Fds().Get(dirfd);
        if (!f) {
            *err = -EBADF_;
            return {};
        }
        base = f->path;
    }
    if (path.empty())
        return Normalize(base);
    return Normalize(base + "/" + path);
}

std::string Vfs::Cwd() {
    std::lock_guard lock(mu_);
    return cwd_;
}

std::optional<std::string> Vfs::LinkTarget(const std::string& p) {
    std::vector<std::function<std::optional<std::string>(const std::string&)>> resolvers;
    {
        std::lock_guard lock(mu_);
        auto it = symlinks_.find(p);
        if (it != symlinks_.end())
            return it->second;
        resolvers = link_resolvers_;
    }
    for (auto& fn : resolvers)
        if (auto t = fn(p))
            return t;
    return std::nullopt;
}

std::optional<std::pair<std::wstring, bool>> Vfs::MountHost(const std::string& p) {
    std::lock_guard lock(mu_);
    for (const auto& m : mounts_) {
        if (p == m.prefix || StartsWithDir(p, m.prefix)) {
            std::string rest = p.substr(m.prefix == "/" ? 0 : m.prefix.size());
            std::wstring host = m.host + Widen(rest);
            std::replace(host.begin(), host.end(), L'/', L'\\');
            return std::make_pair(host, m.writable);
        }
    }
    return std::nullopt;
}

void Vfs::Classify(const std::string& p, Node* out) {
    out->path = p;
    out->kind = Node::None;
    {
        std::lock_guard lock(mu_);
        if (devices_.count(p)) {
            out->kind = Node::Device;
            return;
        }
        if (files_.count(p)) {
            out->kind = Node::Generated;
            return;
        }
        for (const auto& [prefix, gen] : generators_) {
            if (p == prefix || StartsWithDir(p, prefix)) {
                out->kind = Node::Generated;
                break;
            }
        }
    }
    if (out->kind == Node::Generated) {
        // Generators may decline; check now so ENOENT is reported correctly.
        std::function<std::optional<std::string>(const std::string&)> gen;
        {
            std::lock_guard lock(mu_);
            if (files_.count(p))
                return;
            for (const auto& [prefix, g] : generators_)
                if (p == prefix || StartsWithDir(p, prefix))
                    gen = g;
        }
        if (gen && gen(p))
            return;
        out->kind = Node::None;
    }
    if (auto mh = MountHost(p)) {
        DWORD attr = GetFileAttributesW(mh->first.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES) {
            out->host = mh->first;
            out->writable = mh->second;
            out->kind = (attr & FILE_ATTRIBUTE_DIRECTORY) ? Node::HostDir : Node::HostFile;
            return;
        }
    }
    // Implied directories (parents of mounts, links and generated files).
    std::lock_guard lock(mu_);
    auto implies = [&](const std::string& s) { return StartsWithDir(s, p); };
    for (const auto& m : mounts_)
        if (implies(m.prefix) || m.prefix == p) {
            out->kind = Node::VirtualDir;
            return;
        }
    for (const auto& [k, v] : symlinks_)
        if (implies(k)) {
            out->kind = Node::VirtualDir;
            return;
        }
    for (const auto& [k, v] : files_)
        if (implies(k)) {
            out->kind = Node::VirtualDir;
            return;
        }
    for (const auto& [k, v] : devices_)
        if (implies(k)) {
            out->kind = Node::VirtualDir;
            return;
        }
    for (const auto& [k, v] : generators_)
        if (implies(k) || k == p) {
            out->kind = Node::VirtualDir;
            return;
        }
}

// Firmware libraries that only talk to Horizon OS services over binder (absent here;
// their clients wait forever). Apps treat them as optional, as on non-Quest devices.
static bool HiddenFirmwareFile(const std::string& path) {
    static const char* const kHidden[] = {
        "/system_ext/lib64/libossdk.oculus.so",  // OVRPlugin's OsSdkLoader
        "/system_ext/lib64/libOVRMrcLib.oculus.so",  // mixed reality capture; calls System.exit off-device
    };
    for (const char* h : kHidden)
        if (path == h)
            return true;
    return false;
}

int Vfs::Resolve(const std::string& abs, bool follow_last, Node* out) {
    std::string path = Normalize(abs);
    for (int hops = 0; hops < 40; ++hops) {
        std::string cur;
        size_t pos = 1;
        bool restarted = false;
        while (pos <= path.size()) {
            size_t next = path.find('/', pos);
            if (next == std::string::npos)
                next = path.size();
            std::string comp = path.substr(pos, next - pos);
            pos = next + 1;
            if (comp.empty())
                continue;
            std::string cand = cur + "/" + comp;
            bool last = next >= path.size();
            if (auto t = LinkTarget(cand)) {
                if (last && !follow_last) {
                    out->kind = Node::Symlink;
                    out->path = cand;
                    out->link_target = *t;
                    return 0;
                }
                std::string rest = last ? "" : path.substr(next);
                std::string tgt = (!t->empty() && (*t)[0] == '/') ? *t : cur + "/" + *t;
                path = Normalize(tgt + rest);
                restarted = true;
                break;
            }
            cur = cand;
        }
        if (!restarted) {
            if (HiddenFirmwareFile(path))
                return -ENOENT_;
            Classify(path, out);
            return out->kind == Node::None ? -ENOENT_ : 0;
        }
    }
    return -ELOOP_;
}

std::vector<DirFile::Entry> Vfs::ListDir(const Node& n) {
    std::vector<DirFile::Entry> out;
    std::set<std::string> seen;
    auto add = [&](const std::string& name, u8 type) {
        if (name.empty() || !seen.insert(name).second)
            return;
        out.push_back({name, type, HashInode(n.path + "/" + name)});
    };
    add(".", DT_DIR);
    add("..", DT_DIR);
    if (n.kind == Node::HostDir) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((n.host + L"\\*").c_str(), FindExInfoBasic, &fd,
                                    FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                add(Narrow(fd.cFileName),
                    (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? DT_DIR : DT_REG);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    std::lock_guard lock(mu_);
    auto child = [&](const std::string& key, u8 type) {
        if (!StartsWithDir(key, n.path))
            return;
        std::string rest = key.substr(n.path == "/" ? 1 : n.path.size() + 1);
        size_t slash = rest.find('/');
        if (slash == std::string::npos)
            add(rest, type);
        else
            add(rest.substr(0, slash), DT_DIR);
    };
    for (const auto& m : mounts_)
        child(m.prefix, DT_DIR);
    for (const auto& [k, v] : symlinks_)
        child(k, DT_LNK);
    for (const auto& [k, v] : files_)
        child(k, DT_REG);
    for (const auto& [k, v] : devices_)
        child(k, DT_CHR);
    for (const auto& [k, v] : generators_)
        child(k, DT_DIR);
    return out;
}


// Inode of a node: host files use the NTFS file index so hard links share it (the linker
// recognizes an already loaded library by st_dev/st_ino); everything else is path-derived.
static u64 NodeInode(const Vfs::Node& n) {
    if (n.kind == Vfs::Node::HostFile) {
        HANDLE h = CreateFileW(n.host.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            BY_HANDLE_FILE_INFORMATION info;
            bool ok = GetFileInformationByHandle(h, &info) != 0;
            CloseHandle(h);
            if (ok)
                return ((static_cast<u64>(info.nFileIndexHigh) << 32) | info.nFileIndexLow) | (1ull << 62);
        }
    }
    return HashInode(n.path);
}

int Vfs::Open(int dirfd, const std::string& path, int flags, u32 mode, FilePtr* out) {
    int err;
    std::string abs = Absolute(dirfd, path, &err);
    if (err)
        return err;
    Node n;
    int r = Resolve(abs, !(flags & O_NOFOLLOW_), &n);
    const int acc = flags & O_ACCMODE_;
    const bool want_write = acc == O_WRONLY_ || acc == O_RDWR_ || (flags & O_TRUNC_);
    if (r == 0 && n.kind == Node::Symlink)
        return (flags & O_PATH_) ? -ENOENT_ : -ELOOP_;
    if (r == -ENOENT_ && (flags & O_CREAT_)) {
        // Create in a writable host mount (the parent must exist).
        Node parent;
        std::string full = Normalize(abs);
        if (Resolve(Parent(full), true, &parent) != 0)
            return -ENOENT_;
        if (parent.kind != Node::HostDir)
            return -EROFS_;
        if (!parent.writable)
            return -EROFS_;
        size_t slash = full.rfind('/');
        n.kind = Node::HostFile;
        n.path = (parent.path == "/" ? "" : parent.path) + full.substr(slash);
        n.host = parent.host + L"\\" + Widen(full.substr(slash + 1));
        n.writable = true;
        r = 0;
    } else if (r == 0 && (flags & O_CREAT_) && (flags & O_EXCL_)) {
        return -EEXIST_;
    }
    if (r != 0)
        return r;

    FilePtr f;
    switch (n.kind) {
    case Node::HostDir:
    case Node::VirtualDir:
        if (want_write)
            return -EISDIR_;
        f = std::make_shared<DirFile>(ListDir(n));
        break;
    case Node::HostFile: {
        if (flags & O_DIRECTORY_)
            return -ENOTDIR_;
        if (want_write && !n.writable)
            return -EROFS_;
        if (flags & O_PATH_) {
            f = std::make_shared<MemFile>("");
            break;
        }
        DWORD access = GENERIC_READ;
        if (acc == O_WRONLY_)
            access = GENERIC_WRITE;
        else if (acc == O_RDWR_)
            access = GENERIC_READ | GENERIC_WRITE;
        DWORD disp = OPEN_EXISTING;
        if (flags & O_CREAT_)
            disp = (flags & O_EXCL_) ? CREATE_NEW : (flags & O_TRUNC_) ? CREATE_ALWAYS : OPEN_ALWAYS;
        else if (flags & O_TRUNC_)
            disp = TRUNCATE_EXISTING;
        HANDLE h = CreateFileW(n.host.c_str(), access,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               disp, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return -WinErrToErrno(GetLastError());
        f = std::make_shared<HostFile>(h, (flags & O_APPEND_) != 0);
        break;
    }
    case Node::Generated: {
        if (flags & O_DIRECTORY_)
            return -ENOTDIR_;
        std::string content;
        std::function<std::string()> file_gen;
        std::function<std::optional<std::string>(const std::string&)> gen;
        {
            std::lock_guard lock(mu_);
            auto it = files_.find(n.path);
            if (it != files_.end())
                file_gen = it->second;
            else
                for (const auto& [prefix, g] : generators_)
                    if (n.path == prefix || StartsWithDir(n.path, prefix))
                        gen = g;
        }
        if (file_gen) {
            content = file_gen();
        } else if (gen) {
            auto c = gen(n.path);
            if (!c)
                return -ENOENT_;
            content = std::move(*c);
        }
        f = std::make_shared<MemFile>(std::move(content));
        break;
    }
    case Node::Device: {
        std::function<FilePtr()> factory;
        {
            std::lock_guard lock(mu_);
            factory = devices_[n.path];
        }
        f = factory();
        if (!f)
            return -ENODEV_;
        break;
    }
    default:
        return -ENOENT_;
    }
    f->path = n.path;
    if (!f->inode)
        f->inode = NodeInode(n);
    f->status_flags = flags & (O_ACCMODE_ | O_APPEND_ | O_NONBLOCK_ | O_PATH_ | O_DIRECTORY_);
    *out = std::move(f);
    return 0;
}

int Vfs::Stat(int dirfd, const std::string& path, bool follow, lx::Stat* st) {
    int err;
    std::string abs = Absolute(dirfd, path, &err);
    if (err)
        return err;
    Node n;
    int r = Resolve(abs, follow, &n);
    if (r)
        return r;
    memset(st, 0, sizeof(*st));
    st->st_ino = NodeInode(n);
    st->st_dev = 0xfe2a;
    st->st_nlink = 1;
    st->st_blksize = 4096;
    switch (n.kind) {
    case Node::Symlink:
        st->st_mode = S_IFLNK_ | 0777;
        st->st_size = static_cast<s64>(n.link_target.size());
        return 0;
    case Node::HostDir:
    case Node::VirtualDir:
        st->st_mode = S_IFDIR_ | 0755;
        st->st_nlink = 2;
        st->st_size = 4096;
        if (n.writable)
            st->st_uid = st->st_gid = 10100;
        return 0;
    case Node::HostFile: {
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (!GetFileAttributesExW(n.host.c_str(), GetFileExInfoStandard, &data))
            return -WinErrToErrno(GetLastError());
        st->st_mode = S_IFREG_ | (n.writable ? 0660 : 0755);
        if (n.writable)
            st->st_uid = st->st_gid = 10100;
        st->st_size = (static_cast<s64>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        st->st_blocks = (st->st_size + 511) / 512;
        LARGE_INTEGER t;
        t.LowPart = data.ftLastWriteTime.dwLowDateTime;
        t.HighPart = static_cast<LONG>(data.ftLastWriteTime.dwHighDateTime);
        s64 ns = (t.QuadPart - 116444736000000000ll) * 100;
        st->st_mtime_sec = st->st_ctime_sec = st->st_atime_sec = ns / 1000000000;
        st->st_mtime_nsec = st->st_ctime_nsec = st->st_atime_nsec = ns % 1000000000;
        return 0;
    }
    default: {
        FilePtr f;
        r = Open(AT_FDCWD_, n.path, O_RDONLY_, 0, &f);
        if (r)
            return r;
        f->Stat(st);
        st->st_ino = HashInode(n.path);
        return 0;
    }
    }
}

int Vfs::Readlink(int dirfd, const std::string& path, std::string* out) {
    int err;
    std::string abs = Absolute(dirfd, path, &err);
    if (err)
        return err;
    Node n;
    int r = Resolve(abs, false, &n);
    if (r)
        return r;
    if (n.kind != Node::Symlink)
        return -EINVAL_;
    *out = n.link_target;
    return 0;
}

int Vfs::Access(int dirfd, const std::string& path, int mode) {
    int err;
    std::string abs = Absolute(dirfd, path, &err);
    if (err)
        return err;
    Node n;
    int r = Resolve(abs, true, &n);
    if (r)
        return r;
    if ((mode & 2) && !n.writable && n.kind != Node::Device)
        return -EACCES_;
    return 0;
}

int Vfs::Mkdir(int dirfd, const std::string& path, u32 mode) {
    int err;
    std::string abs = Absolute(dirfd, path, &err);
    if (err)
        return err;
    Node n;
    if (Resolve(abs, false, &n) == 0)
        return -EEXIST_;
    Node parent;
    if (Resolve(Parent(abs), true, &parent) != 0)
        return -ENOENT_;
    if (parent.kind != Node::HostDir || !parent.writable)
        return -EROFS_;
    std::wstring host = parent.host + L"\\" + Widen(abs.substr(abs.rfind('/') + 1));
    if (!CreateDirectoryW(host.c_str(), nullptr))
        return -WinErrToErrno(GetLastError());
    return 0;
}

int Vfs::Unlink(int dirfd, const std::string& path, int flags) {
    int err;
    std::string abs = Absolute(dirfd, path, &err);
    if (err)
        return err;
    Node n;
    int r = Resolve(abs, false, &n);
    if (r)
        return r;
    if (!n.writable)
        return -EROFS_;
    if (flags & AT_REMOVEDIR_) {
        if (n.kind != Node::HostDir)
            return -ENOTDIR_;
        if (!RemoveDirectoryW(n.host.c_str()))
            return -WinErrToErrno(GetLastError());
        return 0;
    }
    if (n.kind == Node::HostDir)
        return -EISDIR_;
    if (!DeleteFileW(n.host.c_str()))
        return -WinErrToErrno(GetLastError());
    return 0;
}

int Vfs::Rename(int olddirfd, const std::string& oldp, int newdirfd, const std::string& newp) {
    int err;
    std::string a = Absolute(olddirfd, oldp, &err);
    if (err)
        return err;
    std::string b = Absolute(newdirfd, newp, &err);
    if (err)
        return err;
    Node src;
    int r = Resolve(a, false, &src);
    if (r)
        return r;
    if (!src.writable)
        return -EROFS_;
    Node parent;
    if (Resolve(Parent(b), true, &parent) != 0 || parent.kind != Node::HostDir)
        return -ENOENT_;
    if (!parent.writable)
        return -EROFS_;
    std::wstring dst = parent.host + L"\\" + Widen(b.substr(b.rfind('/') + 1));
    if (!MoveFileExW(src.host.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING))
        return -WinErrToErrno(GetLastError());
    return 0;
}

int Vfs::Chdir(const std::string& path) {
    int err;
    std::string abs = Absolute(AT_FDCWD_, path, &err);
    if (err)
        return err;
    Node n;
    int r = Resolve(abs, true, &n);
    if (r)
        return r;
    if (n.kind != Node::HostDir && n.kind != Node::VirtualDir)
        return -ENOTDIR_;
    std::lock_guard lock(mu_);
    cwd_ = n.path;
    return 0;
}

std::optional<std::wstring> Vfs::HostPath(const std::string& guest_path) {
    Node n;
    if (Resolve(guest_path, true, &n) != 0)
        return std::nullopt;
    if (n.kind == Node::HostFile || n.kind == Node::HostDir)
        return n.host;
    return std::nullopt;
}

}  // namespace rn
