// ============================================================================
//  PSBET — pfs.h
//  Read-only container mounting (Phase 3).
//
//  PSBET Archive (PSA) v1 — PSBET's own clean-room container format for
//  mounting game-content images at app0:/. Layout (all little-endian):
//
//    offset 0   : magic "PSB-ARCHIV" (9 bytes) + u16 version (=1)
//    offset 16  : u32 entryCount
//    offset 20  : u32 namePoolSize
//    offset 24  : u64 dataRegionOffset (absolute)
//    offset 32  : entries[entryCount] { u32 nameOff; u32 flags; u64 off; u64 size; }
//                 flags bit0 = directory
//    then       : name pool (entryCount NUL-terminated UTF-8 relative paths)
//    then       : data region
//
//  Every offset/size is bounds-checked against the file before use; malformed
//  images are rejected, never trusted.
//
//  Encrypted Sony PFS containers: NOT supported (clean-room policy — no key
//  material; the public structural documentation base was insufficient for a
//  verified reader at Phase-3 time; see PHASE_3_REPORT §5). PSA is the mount
//  format until then; a `psbet mkfsa` tool packs a directory into one.
// ============================================================================
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>
#include "psbet/common.h"
#include "core/vfs.h"

namespace psb::pfs {

class PsbArchive {
public:
    // Parse an image file; returns false (with reason logged) on any
    // structural problem. The whole index is validated up-front.
    bool open(const std::string& path);
    bool isOpen() const { return data_ != nullptr; }

    // Look up a file by its image-relative path ("sub/dir/file.bin").
    bool find(const std::string& relPath, u64* offOut, u64* sizeOut) const;
    bool isDir(const std::string& relPath) const;
    // Raw file access for the mounted File implementation.
    const u8* base() const { return data_.get(); }
    u64 fileSize() const { return fileSize_; }
    const std::string& imagePath() const { return path_; }

    // Pack a host directory tree into a PSA image (used by tests + the
    // `psbet mkfsa` CLI tool).
    static bool packDirectory(const std::string& hostDir,
                              const std::string& outPath);

private:
    struct Entry { u32 flags; u64 off; u64 size; };
    std::map<std::string, Entry> entries_;
    std::unique_ptr<u8[]> data_;
    u64 fileSize_ = 0;
    std::string path_;
};

// VFS File adapter over an archive member (read-only).
class ArchiveFile : public vfs::File {
public:
    ArchiveFile(std::shared_ptr<const PsbArchive> arc, u64 off, u64 size);
    i64 read(void* buf, u64 n) override;
    i64 lseek(i64 offset, i32 whence) override;
    bool stat(vfs::FileStat* out) override;

private:
    std::shared_ptr<const PsbArchive> arc_;
    u64 off_ = 0, size_ = 0, pos_ = 0;
};

} // namespace psb::pfs
