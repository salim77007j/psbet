// ============================================================================
//  PSBET — shader_cache.cpp
// ============================================================================
#include "gpu/shader_cache.h"

#include <cstdio>
#include <cstring>
#include <filesystem>

#include "gpu/spirv_emit.h"
#include "core/log.h"

namespace psb::gpu {

namespace {
constexpr u32 kMagic = 0x42535450;   // "PTSB" little-endian view of "PSBSC" tag
constexpr u32 kVersion = 1;

u32 crc32(const spv::Word* words, size_t count) {
    // Standard CRC-32 (IEEE 802.3 polynomial), byte-wise over the words.
    u32 crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < count; ++i) {
        for (int b = 0; b < 4; ++b) {
            crc ^= (words[i] >> (b * 8)) & 0xFFu;
            for (int k = 0; k < 8; ++k)
                crc = (crc & 1) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}
} // namespace

ShaderCache::ShaderCache(std::string dir) : dir_(std::move(dir)) {
    if (!dir_.empty()) load();
}

void ShaderCache::load() {
    namespace fs = std::filesystem;
    const fs::path p = fs::path(dir_) / "psbet_shaders.cache";
    std::FILE* f = std::fopen(p.string().c_str(), "rb");
    if (!f) return; // first run

    u32 hdr[3] = {0, 0, 0};
    if (std::fread(hdr, sizeof(u32), 3, f) != 3 || hdr[0] != kMagic ||
        hdr[1] != kVersion) {
        std::fclose(f);
        LOG_WARN("ShaderCache: stale or foreign cache file — replacing");
        dirty_ = true;
        return;
    }
    const u32 count = hdr[2];
    bool ok = true;
    for (u32 i = 0; i < count && ok; ++i) {
        u64 key = 0;
        u32 wc = 0, crc = 0;
        if (std::fread(&key, sizeof(key), 1, f) != 1 ||
            std::fread(&wc, sizeof(wc), 1, f) != 1 ||
            std::fread(&crc, sizeof(crc), 1, f) != 1 || wc == 0 || wc > (1u << 22)) {
            ok = false;
            break;
        }
        Entry e;
        e.key = key;
        e.words.resize(wc);
        if (std::fread(e.words.data(), sizeof(spv::Word), wc, f) != wc) {
            ok = false;
            break;
        }
        if (crc32(e.words.data(), e.words.size()) != crc) {
            LOG_WARN("ShaderCache: CRC mismatch — dropping entry %016llx",
                     static_cast<unsigned long long>(key));
            continue;
        }
        entries_.push_back(std::move(e));
    }
    std::fclose(f);
    if (!ok) dirty_ = true;
    LOG_INFO("ShaderCache: loaded %zu entries from %s", entries_.size(),
             p.string().c_str());
}

const std::vector<spv::Word>* ShaderCache::find(u64 key) const {
    for (const Entry& e : entries_) {
        if (e.key == key) {
            ++hits_;
            return &e.words;
        }
    }
    ++misses_;
    return nullptr;
}

void ShaderCache::insert(u64 key, const std::vector<spv::Word>& words) {
    for (Entry& e : entries_) {
        if (e.key == key) {
            if (e.words != words) {
                e.words = words;
                dirty_ = true;
            }
            return;
        }
    }
    Entry e;
    e.key = key;
    e.words = words;
    entries_.push_back(std::move(e));
    dirty_ = true;
}

void ShaderCache::flush() {
    if (!dirty_ || dir_.empty()) return;
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(dir_, ec);

    const fs::path final = fs::path(dir_) / "psbet_shaders.cache";
    const fs::path tmp = fs::path(dir_) / "psbet_shaders.cache.tmp";

    std::FILE* f = std::fopen(tmp.string().c_str(), "wb");
    if (!f) {
        LOG_WARN("ShaderCache: cannot open %s for write", tmp.string().c_str());
        return;
    }
    const u32 count = static_cast<u32>(entries_.size());
    const u32 magic = kMagic;
    const u32 version = kVersion;
    bool ok = std::fwrite(&magic, sizeof(u32), 1, f) == 1;
    ok = ok && std::fwrite(&version, sizeof(u32), 1, f) == 1;
    ok = ok && std::fwrite(&count, sizeof(u32), 1, f) == 1;
    for (const Entry& e : entries_) {
        const u32 wc = static_cast<u32>(e.words.size());
        const u32 crc = crc32(e.words.data(), e.words.size());
        ok = ok && std::fwrite(&e.key, sizeof(u64), 1, f) == 1;
        ok = ok && std::fwrite(&wc, sizeof(u32), 1, f) == 1;
        ok = ok && std::fwrite(&crc, sizeof(u32), 1, f) == 1;
        ok = ok && std::fwrite(e.words.data(), sizeof(spv::Word), wc, f) == wc;
        if (!ok) break;
    }
    std::fclose(f);
    if (ok) {
        fs::rename(tmp, final, ec);
        dirty_ = false;
        LOG_INFO("ShaderCache: persisted %zu entries", entries_.size());
    } else {
        fs::remove(tmp, ec);
        LOG_WARN("ShaderCache: write failed — cache left untouched");
    }
}

u64 ShaderCache::hashKey(const char* domain, const spv::Word* words, size_t count) {
    u64 h = 1469598103934665603ull;
    auto byte = [&](u8 b) {
        h ^= b;
        h *= 1099511628211ull;
    };
    for (const char* p = domain; *p; ++p) byte(static_cast<u8>(*p));
    byte(0);
    for (size_t i = 0; i < count; ++i)
        for (int b = 0; b < 4; ++b)
            byte(static_cast<u8>((words[i] >> (b * 8)) & 0xFF));
    return h;
}

} // namespace psb::gpu
