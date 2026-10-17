// ============================================================================
//  PSBET — vfs.h
//  Guest filesystem virtualization (Phase 3).
//
//  Guests never see the host filesystem directly (GUEST_ABI v1.2). The VFS
//  exposes two mount roots:
//    app0:/  — a read-only mounted image (PSBET Archive container, pfs.h)
//    /data   — a writable passthrough backed by one host directory
//  Raw host paths resolve to nothing: a guest opening "/home/user/secret"
//  gets an error, not the file. Kernel fds 0/1/2 stay host std streams.
// ============================================================================
#pragma once

#include <memory>
#include <string>
#include <vector>
#include "psbet/common.h"

namespace psb::vfs {

struct FileStat {
    u64 size = 0;
    bool isDir = false;
    bool readOnly = false;
};

// One open file in the guest namespace.
class File {
public:
    virtual ~File() = default;
    virtual i64 read(void* buf, u64 n) = 0;
    virtual i64 write(const void* buf, u64 n) { (void)buf; (void)n; return -1; }
    virtual i64 lseek(i64 offset, i32 whence) = 0;  // 0=SET 1=CUR 2=END
    virtual bool stat(FileStat* out) = 0;
};

// --- lifecycle ---------------------------------------------------------------
// Writable root (/data) backed by an existing host directory. Creates the
// directory when missing. Returns false when the host dir cannot be used.
bool initDataDir(const std::string& hostDir);
// Mount a read-only container image at app0:/ (see pfs.h for formats).
bool mountImage(const std::string& imagePath);
bool imageMounted();
// Close every open guest fd (process teardown).
void shutdown();

// --- fd surface (kernel-side; 0/1/2 are host std streams, never stored here) --
// Open flags (PSBET definitions, GUEST_ABI v1.2 §8):
constexpr u32 kOpenRdOnly  = 0x0000;
constexpr u32 kOpenWrOnly  = 0x0001;
constexpr u32 kOpenRdWr    = 0x0002;
constexpr u32 kOpenCreate  = 0x0040;
constexpr u32 kOpenTrunc   = 0x0200;
constexpr u32 kOpenAppend  = 0x0400;

i32  open(const std::string& guestPath, u32 flags);   // fd or -1
i64  read(i32 fd, void* buf, u64 n);
i64  write(i32 fd, const void* buf, u64 n);
i64  lseek(i32 fd, i64 offset, i32 whence);
i32  close(i32 fd);
bool stat(i32 fd, FileStat* out);
bool exists(const std::string& guestPath);

// Test seam: number of currently open guest fds.
u32  openCount();

} // namespace psb::vfs
