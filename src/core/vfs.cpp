// ============================================================================
//  PSBET — vfs.cpp
//  Guest filesystem virtualization: mounts, path resolution, fd table.
// ============================================================================
#include "vfs.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <filesystem>
#include <system_error>
#include "core/log.h"
#include "core/pfs.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#else
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace psb::vfs {

namespace {

// ---------------------------------------------------------------------------
// HostDirFile — writable passthrough file in the /data mount.
// ---------------------------------------------------------------------------
class HostDirFile : public File {
public:
    HostDirFile(std::FILE* f, bool writable) : f_(f), writable_(writable) {}
    ~HostDirFile() override { if (f_) std::fclose(f_); }

    i64 read(void* buf, u64 n) override {
        if (!f_) return -1;
        const size_t r = std::fread(buf, 1, static_cast<size_t>(n), f_);
        return static_cast<i64>(r);
    }
    i64 write(const void* buf, u64 n) override {
        if (!f_ || !writable_) return -1;
        const size_t w = std::fwrite(buf, 1, static_cast<size_t>(n), f_);
        return static_cast<i64>(w);
    }
    i64 lseek(i64 offset, i32 whence) override {
        if (!f_) return -1;
        if (std::fseek(f_, static_cast<long>(offset), whence) != 0) return -1;
        return static_cast<i64>(std::ftell(f_));
    }
    bool stat(FileStat* out) override {
        if (!f_ || !out) return false;
        const long cur = std::ftell(f_);
        std::fseek(f_, 0, SEEK_END);
        out->size = static_cast<u64>(std::ftell(f_));
        std::fseek(f_, cur, SEEK_SET);
        out->isDir = false;
        out->readOnly = !writable_;
        return true;
    }

private:
    std::FILE* f_ = nullptr;
    bool writable_ = false;
};

struct FdEntry {
    std::unique_ptr<File> file;
    std::string path;
};

std::mutex g_mtx;
std::map<i32, FdEntry> g_fds;
i32 g_nextFd = 3;
std::string g_dataDir;                       // host dir backing /data
std::shared_ptr<const pfs::PsbArchive> g_app0;
bool g_imageTried = false;

std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

// Resolve a guest path to (mount, relative). Returns false when unmounted.
bool resolve(const std::string& guestPath, std::string* mountOut,
             std::string* relOut) {
    std::string p;
    p.reserve(guestPath.size());
    for (char c : guestPath) p.push_back(c == '\\' ? '/' : c);

    auto stripRoot = [&](const std::string& root) -> bool {
        // Matches "app0:/x", "app0:x", "/app0/x" (and the /data trio).
        size_t pos = std::string::npos;
        if (p.rfind(root + ":", 0) == 0) pos = root.size() + 1;
        else if (p.rfind("/" + root + "/", 0) == 0) pos = root.size() + 1;
        else if (p == "/" + root) { *relOut = ""; return true; }
        if (pos == std::string::npos) return false;
        std::string rest = p.substr(pos);
        if (!rest.empty() && rest.front() == '/') rest.erase(0, 1);
        *relOut = rest;
        return true;
    };

    if (stripRoot("app0")) { if (mountOut) *mountOut = "app0"; return true; }
    if (stripRoot("data")) { if (mountOut) *mountOut = "data"; return true; }
    // Absolute paths outside the mounts never resolve to the host FS.
    if (mountOut) mountOut->clear();
    return false;
}

i32 newFd(std::unique_ptr<File> file, const std::string& path) {
    std::lock_guard<std::mutex> lk(g_mtx);
    const i32 fd = g_nextFd++;
    g_fds[fd] = {std::move(file), path};
    return fd;
}

FdEntry* findFd(i32 fd) {
    auto it = g_fds.find(fd);
    return it == g_fds.end() ? nullptr : &it->second;
}

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
bool initDataDir(const std::string& hostDir) {
    std::error_code ec;
    std::filesystem::create_directories(hostDir, ec);
    if (!std::filesystem::is_directory(hostDir, ec)) {
        LOG_ERROR("VFS: /data host directory unusable: %s", hostDir.c_str());
        return false;
    }
    std::lock_guard<std::mutex> lk(g_mtx);
    g_dataDir = hostDir;
    LOG_INFO("VFS: /data mounted (host: %s)", hostDir.c_str());
    return true;
}

bool mountImage(const std::string& imagePath) {
    auto arc = std::make_shared<pfs::PsbArchive>();
    if (!arc->open(imagePath)) return false;
    std::lock_guard<std::mutex> lk(g_mtx);
    g_app0 = std::move(arc);
    g_imageTried = true;
    return true;
}

bool imageMounted() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_app0 != nullptr;
}

void shutdown() {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_fds.clear();
}

u32 openCount() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return static_cast<u32>(g_fds.size());
}

bool exists(const std::string& guestPath) {
    std::string mount, rel;
    if (!resolve(guestPath, &mount, &rel)) return false;
    std::lock_guard<std::mutex> lk(g_mtx);
    if (mount == "app0") {
        if (!g_app0) return false;
        if (rel.empty()) return true;
        return g_app0->find(rel, nullptr, nullptr) || g_app0->isDir(rel);
    }
    if (mount == "data") {
        if (g_dataDir.empty()) return false;
        std::error_code ec;
        return std::filesystem::exists(joinPath(g_dataDir, rel), ec);
    }
    return false;
}

// ---------------------------------------------------------------------------
// fd surface
// ---------------------------------------------------------------------------
i32 open(const std::string& guestPath, u32 flags) {
    std::string mount, rel;
    if (!resolve(guestPath, &mount, &rel)) {
        LOG_ERROR("VFS: open '%s' — path is outside every mount "
                  "(guests cannot touch the host filesystem)",
                  guestPath.c_str());
        return -1;
    }

    if (mount == "app0") {
        std::shared_ptr<const pfs::PsbArchive> arc;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            arc = g_app0;
        }
        if (!arc) { LOG_ERROR("VFS: app0 not mounted"); return -1; }
        if (rel.empty()) {
            LOG_ERROR("VFS: app0:/ is a mount root, not a file");
            return -1;
        }
        u64 off = 0, size = 0;
        if (!arc->find(rel, &off, &size)) {
            LOG_DEBUG("VFS: app0 member not found: %s", rel.c_str());
            return -1;
        }
        if (flags & (kOpenWrOnly | kOpenRdWr | kOpenCreate | kOpenTrunc)) {
            LOG_ERROR("VFS: app0 is read-only (open '%s')", guestPath.c_str());
            return -1;
        }
        return newFd(std::make_unique<pfs::ArchiveFile>(arc, off, size),
                     guestPath);
    }

    if (mount == "data") {
        std::string hostDir;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            hostDir = g_dataDir;
        }
        if (hostDir.empty()) { LOG_ERROR("VFS: /data not mounted"); return -1; }

        const bool wantsWrite = (flags & (kOpenWrOnly | kOpenRdWr)) != 0;
        const char* mode = wantsWrite
            ? ((flags & kOpenRdWr) ? "r+b" : "wb")
            : "rb";
        if (wantsWrite && (flags & kOpenCreate) && !(flags & kOpenRdWr)) {
            // "wb" already creates/truncates.
        }
        std::FILE* f = std::fopen(joinPath(hostDir, rel).c_str(), mode);
        if (!f && wantsWrite && (flags & kOpenRdWr)) {
            // r+b fails when the file does not exist: create then reopen.
            f = std::fopen(joinPath(hostDir, rel).c_str(), "w+b");
        }
        if (!f) return -1;
        if (!(flags & kOpenAppend)) {
            // default; "a" semantics arrive with a real need
        }
        return newFd(std::make_unique<HostDirFile>(f, wantsWrite), guestPath);
    }

    return -1;
}

i64 read(i32 fd, void* buf, u64 n) {
    std::lock_guard<std::mutex> lk(g_mtx);
    FdEntry* e = findFd(fd);
    return e && e->file ? e->file->read(buf, n) : -1;
}

i64 write(i32 fd, const void* buf, u64 n) {
    std::lock_guard<std::mutex> lk(g_mtx);
    FdEntry* e = findFd(fd);
    return e && e->file ? e->file->write(buf, n) : -1;
}

i64 lseek(i32 fd, i64 offset, i32 whence) {
    std::lock_guard<std::mutex> lk(g_mtx);
    FdEntry* e = findFd(fd);
    return e && e->file ? e->file->lseek(offset, whence) : -1;
}

i32 close(i32 fd) {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_fds.erase(fd) ? 0 : -1;
}

bool stat(i32 fd, FileStat* out) {
    std::lock_guard<std::mutex> lk(g_mtx);
    FdEntry* e = findFd(fd);
    return e && e->file ? e->file->stat(out) : false;
}

} // namespace psb::vfs
