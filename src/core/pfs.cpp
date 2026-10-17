// ============================================================================
//  PSBET — pfs.cpp
//  PSBET Archive (PSA) container: parser + packer + VFS adapter.
// ============================================================================
#include "pfs.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include "core/log.h"

namespace psb::pfs {

namespace {

constexpr u8 kMagic[10] = {'P', 'S', 'B', '-', 'A', 'R', 'C', 'H', 'I', 'V'};
constexpr u16 kVersion = 1;
constexpr u32 kFlagDir = 1;

struct Header {
    u8     magic[10];
    u16    version;
    u32    entryCount;
    u32    namePoolSize;
    u64    dataRegionOffset;
};
static_assert(sizeof(Header) == 32, "PSA header must be 32 bytes");

struct EntryRec {
    u32 nameOff;
    u32 flags;
    u64 off;
    u64 size;
};

std::string normalizeRel(const std::string& p) {
    std::string out;
    out.reserve(p.size());
    bool prevSlash = false;
    for (char c : p) {
        if (c == '/' || c == '\\') {
            if (prevSlash || out.empty()) { prevSlash = true; continue; }
            prevSlash = true;
            out.push_back('/');
        } else {
            prevSlash = false;
            out.push_back(c);
        }
    }
    while (!out.empty() && out.back() == '/') out.pop_back();
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// PsbArchive
// ---------------------------------------------------------------------------
bool PsbArchive::open(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        LOG_ERROR("PFS: cannot open container image: %s", path.c_str());
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long fsize = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (fsize < static_cast<long>(sizeof(Header))) {
        std::fclose(f);
        LOG_ERROR("PFS: image too small: %s", path.c_str());
        return false;
    }

    std::unique_ptr<u8[]> buf(new u8[static_cast<size_t>(fsize)]);
    if (std::fread(buf.get(), 1, static_cast<size_t>(fsize), f)
        != static_cast<size_t>(fsize)) {
        std::fclose(f);
        LOG_ERROR("PFS: short read on %s", path.c_str());
        return false;
    }
    std::fclose(f);

    Header hdr{};
    std::memcpy(&hdr, buf.get(), sizeof(hdr));
    if (std::memcmp(hdr.magic, kMagic, sizeof(kMagic)) != 0) {
        LOG_ERROR("PFS: not a PSBET Archive image (bad magic): %s", path.c_str());
        return false;
    }
    if (hdr.version != kVersion) {
        LOG_ERROR("PFS: unsupported archive version %u", hdr.version);
        return false;
    }
    const u64 fileSize = static_cast<u64>(fsize);
    // Bounds: entry table + name pool must live inside the file, and the data
    // region must start after them.
    const u64 tableOff = sizeof(Header);
    const u64 tableBytes = static_cast<u64>(hdr.entryCount) * sizeof(EntryRec);
    const u64 namePoolOff = tableOff + tableBytes;
    if (hdr.entryCount > 100000 ||
        namePoolOff + hdr.namePoolSize > fileSize ||
        hdr.dataRegionOffset > fileSize) {
        LOG_ERROR("PFS: header bounds violation (entries=%u pool=%u data@%llu, file=%llu)",
                  hdr.entryCount, hdr.namePoolSize,
                  static_cast<unsigned long long>(hdr.dataRegionOffset),
                  static_cast<unsigned long long>(fileSize));
        return false;
    }

    const EntryRec* recs = reinterpret_cast<const EntryRec*>(buf.get() + tableOff);
    const char* namePool = reinterpret_cast<const char*>(buf.get() + namePoolOff);

    entries_.clear();
    for (u32 i = 0; i < hdr.entryCount; ++i) {
        if (recs[i].nameOff >= hdr.namePoolSize) {
            LOG_ERROR("PFS: entry %u name offset out of pool", i);
            return false;
        }
        const char* name = namePool + recs[i].nameOff;
        const std::string rel = normalizeRel(name);
        if (rel.empty()) {
            LOG_ERROR("PFS: entry %u has an empty name", i);
            return false;
        }
        if (recs[i].flags & kFlagDir) {
            entries_[rel] = {kFlagDir, 0, 0};
            continue;
        }
        // File data must sit inside the data region.
        if (recs[i].off < hdr.dataRegionOffset ||
            recs[i].off + recs[i].size > fileSize) {
            LOG_ERROR("PFS: entry '%s' data out of bounds (off=%llu size=%llu)",
                      rel.c_str(), static_cast<unsigned long long>(recs[i].off),
                      static_cast<unsigned long long>(recs[i].size));
            return false;
        }
        entries_[rel] = {recs[i].flags, recs[i].off, recs[i].size};
    }

    data_ = std::move(buf);
    fileSize_ = fileSize;
    path_ = path;
    LOG_INFO("PFS: mounted '%s' (%u entries, %llu bytes)",
             path.c_str(), hdr.entryCount,
             static_cast<unsigned long long>(fileSize));
    return true;
}

bool PsbArchive::find(const std::string& relPath, u64* offOut, u64* sizeOut) const {
    auto it = entries_.find(normalizeRel(relPath));
    if (it == entries_.end() || (it->second.flags & kFlagDir)) return false;
    if (offOut) *offOut = it->second.off;
    if (sizeOut) *sizeOut = it->second.size;
    return true;
}

bool PsbArchive::isDir(const std::string& relPath) const {
    auto it = entries_.find(normalizeRel(relPath));
    return it != entries_.end() && (it->second.flags & kFlagDir);
}

// ---------------------------------------------------------------------------
// ArchiveFile
// ---------------------------------------------------------------------------
ArchiveFile::ArchiveFile(std::shared_ptr<const PsbArchive> arc, u64 off, u64 size)
    : arc_(std::move(arc)), off_(off), size_(size) {}

i64 ArchiveFile::read(void* buf, u64 n) {
    if (pos_ >= size_) return 0;
    const u64 avail = size_ - pos_;
    const u64 take = std::min(n, avail);
    std::memcpy(buf, arc_->base() + off_ + pos_, static_cast<size_t>(take));
    pos_ += take;
    return static_cast<i64>(take);
}

i64 ArchiveFile::lseek(i64 offset, i32 whence) {
    i64 target = 0;
    switch (whence) {
        case 0: target = offset; break;
        case 1: target = static_cast<i64>(pos_) + offset; break;
        case 2: target = static_cast<i64>(size_) + offset; break;
        default: return -1;
    }
    if (target < 0 || static_cast<u64>(target) > size_) return -1;
    pos_ = static_cast<u64>(target);
    return static_cast<i64>(pos_);
}

bool ArchiveFile::stat(vfs::FileStat* out) {
    if (out) {
        out->size = size_;
        out->isDir = false;
        out->readOnly = true;
    }
    return true;
}

// ---------------------------------------------------------------------------
// packDirectory — build a PSA image from a host directory tree.
// ---------------------------------------------------------------------------
bool PsbArchive::packDirectory(const std::string& hostDir,
                               const std::string& outPath) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(hostDir, ec)) {
        LOG_ERROR("PFS: packDirectory: not a directory: %s", hostDir.c_str());
        return false;
    }

    struct Item { std::string rel; bool dir; u64 off; u64 size; };
    std::vector<Item> items;
    u64 dataCursor = 0;

    std::vector<fs::path> stack{fs::path(hostDir)};
    while (!stack.empty()) {
        const fs::path dir = stack.back();
        stack.pop_back();
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            std::string rel = fs::relative(e.path(), hostDir, ec).generic_string();
            if (rel.empty()) continue;
            if (e.is_directory(ec)) {
                items.push_back({rel, true, 0, 0});
                stack.push_back(e.path());
            } else {
                const u64 sz = e.file_size(ec);
                items.push_back({rel, false, 0, sz});
                dataCursor += sz;
            }
        }
    }

    // Layout: header + entries + name pool + data.
    u64 namePoolSize = 0;
    for (const Item& it : items) namePoolSize += it.rel.size() + 1;
    const u64 tableOff = sizeof(Header);
    const u64 namePoolOff = tableOff + items.size() * sizeof(EntryRec);
    const u64 dataOff = namePoolOff + namePoolSize;

    std::vector<u8> image(static_cast<size_t>(dataOff + dataCursor), 0);
    Header hdr{};
    std::memcpy(hdr.magic, kMagic, sizeof(kMagic));
    hdr.version = kVersion;
    hdr.entryCount = static_cast<u32>(items.size());
    hdr.namePoolSize = static_cast<u32>(namePoolSize);
    hdr.dataRegionOffset = dataOff;
    std::memcpy(image.data(), &hdr, sizeof(hdr));

    u64 nameCursor = 0;
    u64 dataWrite = dataOff;
    for (size_t i = 0; i < items.size(); ++i) {
        EntryRec rec{};
        rec.nameOff = static_cast<u32>(nameCursor);
        rec.flags = items[i].dir ? kFlagDir : 0;
        if (!items[i].dir) {
            rec.off = dataWrite;
            rec.size = items[i].size;
            items[i].off = dataWrite;   // second pass reads this back
            dataWrite += items[i].size;
        }
        std::memcpy(image.data() + tableOff + i * sizeof(EntryRec), &rec,
                    sizeof(rec));
        std::memcpy(image.data() + namePoolOff + nameCursor,
                    items[i].rel.c_str(), items[i].rel.size() + 1);
        nameCursor += items[i].rel.size() + 1;
    }

    // Second pass: file contents.
    for (const Item& it : items) {
        if (it.dir) continue;
        std::FILE* f = std::fopen((fs::path(hostDir) / it.rel).c_str(), "rb");
        if (!f) {
            LOG_ERROR("PFS: pack: cannot read %s", it.rel.c_str());
            return false;
        }
        const size_t got = std::fread(image.data() + it.off, 1,
                                      static_cast<size_t>(it.size), f);
        std::fclose(f);
        if (got != it.size) {
            LOG_ERROR("PFS: pack: short read on %s", it.rel.c_str());
            return false;
        }
    }

    std::FILE* out = std::fopen(outPath.c_str(), "wb");
    if (!out) {
        LOG_ERROR("PFS: pack: cannot create %s", outPath.c_str());
        return false;
    }
    const size_t w = std::fwrite(image.data(), 1, image.size(), out);
    std::fclose(out);
    if (w != image.size()) {
        LOG_ERROR("PFS: pack: short write on %s", outPath.c_str());
        return false;
    }
    LOG_INFO("PFS: packed %zu entries from %s into %s (%zu bytes)",
             items.size(), hostDir.c_str(), outPath.c_str(), image.size());
    return true;
}

} // namespace psb::pfs
