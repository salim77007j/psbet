// ============================================================================
//  PSBET — shader_cache.h
//  Disk-backed SPIR-V shader cache.
//
//  Keys are FNV-1a 64 over (source words + a domain tag). Entries store the
//  compiled/translated SPIR-V module so repeated runs skip translation work.
//  Format (little-endian):
//    magic  "PSBSC"   u32 x1
//    version 1        u32 x1
//    entry count      u32 x1
//    per entry:       u64 key, u32 wordCount, u32 crc32, words...
//  Corrupt or foreign files are replaced, never trusted (Golden Rule: a
//  cache that can poison rendering is worse than no cache).
// ============================================================================
#pragma once

#include <string>
#include <vector>

#include "gpu/spirv_emit.h"
#include "psbet/common.h"

namespace psb::gpu {

class ShaderCache {
public:
    // `dir` may be empty → in-memory only (still dedups within a run).
    explicit ShaderCache(std::string dir);

    // Returns nullptr on miss.
    const std::vector<spv::Word>* find(u64 key) const;

    // Inserts (or overwrites) an entry and marks the store dirty.
    void insert(u64 key, const std::vector<spv::Word>& words);

    // Persists pending changes atomically (tmp file + rename). Safe to call
    // repeatedly; only writes when dirty.
    void flush();

    // Convenience: FNV-1a 64 over words + a domain tag byte string.
    static u64 hashKey(const char* domain, const spv::Word* words, size_t count);

    size_t size() const { return entries_.size(); }
    u64 hits() const { return hits_; }
    u64 misses() const { return misses_; }

private:
    struct Entry {
        u64 key = 0;
        std::vector<spv::Word> words;
    };
    void load();

    std::string dir_;
    std::vector<Entry> entries_;
    mutable u64 hits_ = 0;
    mutable u64 misses_ = 0;
    bool dirty_ = false;
};

} // namespace psb::gpu
