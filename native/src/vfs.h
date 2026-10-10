// Guest filesystem view: host directories mounted at guest paths, a symlink table
// (Windows extraction of the firmware dump loses symlinks), and generated files.
#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "files.h"

namespace rn {

class Vfs {
public:
    struct Node {
        enum Kind { None, HostFile, HostDir, Generated, VirtualDir, Device, Symlink } kind = None;
        std::string path;   // canonical guest path
        std::wstring host;  // for HostFile/HostDir
        bool writable = false;
        std::string link_target;  // for Symlink
    };

    void Mount(const std::string& guest_prefix, const std::wstring& host_dir, bool writable);
    void AddSymlink(const std::string& path, const std::string& target);
    bool LoadSymlinkList(const std::wstring& host_file);
    void AddFile(const std::string& path, std::function<std::string()> gen);
    // Handler for every path under prefix; returns content or nullopt (ENOENT).
    void AddGenerator(const std::string& prefix,
                      std::function<std::optional<std::string>(const std::string&)> gen);
    // Dynamic symlinks such as /proc/self/exe and /proc/self/fd/N.
    void AddLinkResolver(std::function<std::optional<std::string>(const std::string&)> fn);
    // Directories a generator's tree has besides those implied by its files (e.g. /proc/self).
    void AddDirectories(std::function<bool(const std::string&)> is_dir);
    void AddDevice(const std::string& path, std::function<FilePtr()> factory);

    // Makes `path` absolute against cwd (or the directory of dirfd) and removes
    // "." and ".." components.
    std::string Absolute(int dirfd, const std::string& path, int* err);
    static std::string Normalize(const std::string& abs);

    int Resolve(const std::string& abs, bool follow_last, Node* out);
    int Open(int dirfd, const std::string& path, int flags, u32 mode, FilePtr* out);
    int Stat(int dirfd, const std::string& path, bool follow, lx::Stat* st);
    int Readlink(int dirfd, const std::string& path, std::string* out);
    int Access(int dirfd, const std::string& path, int mode);
    int Mkdir(int dirfd, const std::string& path, u32 mode);
    int Unlink(int dirfd, const std::string& path, int flags);
    int Rename(int olddirfd, const std::string& oldp, int newdirfd, const std::string& newp);
    int Chdir(const std::string& path);
    std::string Cwd();

    // Host path for a guest path, if it lives in a host mount (after symlinks).
    std::optional<std::wstring> HostPath(const std::string& guest_path);
    // Guest path for a host path inside a mount ("" if none).
    std::string GuestPathForHost(const std::wstring& host_path);

private:
    std::optional<std::string> LinkTarget(const std::string& p);
    void Classify(const std::string& p, Node* out);
    std::vector<DirFile::Entry> ListDir(const Node& n);
    std::optional<std::pair<std::wstring, bool>> MountHost(const std::string& p);

    struct MountPoint {
        std::string prefix;
        std::wstring host;
        bool writable;
    };
    std::mutex mu_;
    std::vector<MountPoint> mounts_;
    std::map<std::string, std::string> symlinks_;
    std::map<std::string, std::function<std::string()>> files_;
    std::vector<std::pair<std::string,
                          std::function<std::optional<std::string>(const std::string&)>>>
        generators_;
    std::vector<std::function<std::optional<std::string>(const std::string&)>> link_resolvers_;
    std::vector<std::function<bool(const std::string&)>> dir_predicates_;
    std::map<std::string, std::function<FilePtr()>> devices_;
    std::string cwd_ = "/";
};

Vfs& Fs();

}  // namespace rn
