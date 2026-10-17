// ============================================================================
//  PSBET — elf.h
//  PS5-style ELF64 image loading.
//
//  Loads position-independent x86-64 ELF images (ET_DYN) into the guest
//  address space: segments, relocations, TLS, dynamic imports, initializers.
//  Encrypted SELF containers are detected and rejected with guidance — PSBET
//  ships no key material (clean-room policy).
// ============================================================================
#pragma once

#include <functional>
#include <string>
#include <vector>
#include "psbet/common.h"

namespace psb {

// --------------------------------------------------------------------------
// Minimal ELF64 structures (self-contained; no system elf.h dependency so the
// same code compiles on Linux and Windows hosts).
// --------------------------------------------------------------------------
#pragma pack(push, 1)

struct Elf64Ehdr {
    u8  ident[16];
    u16 type;
    u16 machine;
    u32 version;
    u64 entry;
    u64 phoff;
    u64 shoff;
    u32 flags;
    u16 ehsize;
    u16 phentsize;
    u16 phnum;
    u16 shentsize;
    u16 shnum;
    u16 shstrndx;
};

struct Elf64Phdr {
    u32 type;
    u32 flags;
    u64 offset;
    u64 vaddr;
    u64 paddr;
    u64 filesz;
    u64 memsz;
    u64 align;
};

struct Elf64Sym {
    u32 name;
    u8  info;
    u8  other;
    u16 shndx;
    u64 value;
    u64 size;
};

struct Elf64Rela {
    u64 offset;   // r_offset
    u64 info;     // r_info: sym = info >> 32, type = info & 0xffffffff
    i64 addend;   // r_addend
};

struct Elf64Dyn {
    i64 tag;
    u64 val;
};

#pragma pack(pop)

// ELF types
constexpr u16 ET_EXEC = 2;
constexpr u16 ET_DYN  = 3;

// Program header types
constexpr u32 PT_LOAD       = 1;
constexpr u32 PT_DYNAMIC    = 2;
constexpr u32 PT_TLS        = 7;
constexpr u32 PT_GNU_RELRO  = 0x6474E552;
constexpr u32 PT_GNU_STACK  = 0x6474E551;

// Segment flags
constexpr u32 PF_X = 1, PF_W = 2, PF_R = 4;

// Dynamic tags
constexpr i64 DT_NULL         = 0;
constexpr i64 DT_NEEDED       = 1;
constexpr i64 DT_STRTAB       = 5;
constexpr i64 DT_SYMTAB       = 6;
constexpr i64 DT_RELA         = 7;
constexpr i64 DT_RELASZ       = 8;
constexpr i64 DT_RELAENT      = 9;
constexpr i64 DT_SYMENT       = 11;
constexpr i64 DT_INIT         = 12;
constexpr i64 DT_FINI         = 13;
constexpr i64 DT_SONAME       = 14;
constexpr i64 DT_JMPREL       = 23;
constexpr i64 DT_PLTRELSZ     = 2;
constexpr i64 DT_INIT_ARRAY   = 25;
constexpr i64 DT_FINI_ARRAY   = 26;
constexpr i64 DT_INIT_ARRAYSZ = 27;
constexpr i64 DT_FINI_ARRAYSZ = 28;

// x86-64 relocation types
constexpr u32 R_X86_64_64        = 1;
constexpr u32 R_X86_64_TPOFF64   = 3;
constexpr u32 R_X86_64_GLOB_DAT  = 6;
constexpr u32 R_X86_64_JUMP_SLOT = 7;
constexpr u32 R_X86_64_RELATIVE  = 8;
constexpr u32 R_X86_64_DTPMOD64  = 16;
constexpr u32 R_X86_64_DTPOFF64  = 17;
constexpr u32 R_X86_64_DTPOFF32  = 21;
constexpr u32 R_X86_64_IRELATIVE = 37;

// --------------------------------------------------------------------------
// LoadedModule — everything the runtime knows about one guest image.
// --------------------------------------------------------------------------
struct LoadedModule {
    std::string path;
    u64  base      = 0;   // host address anchoring vaddr 0
    u64  entry     = 0;   // host address of the entry point
    u64  entryOffset = 0; // entry vaddr (from the file header)
    u64  imageSize = 0;
    u64  paBase    = 0;   // physical span base (ET_DYN); 0 for ET_EXEC
    bool isExec    = false; // ET_EXEC (absolute vaddrs)

    struct Segment { u64 vaddr; u64 memsz; u32 flags; };
    std::vector<Segment> segments;

    u64 relroVaddr = 0;   // PT_GNU_RELRO (made read-only after relocation)
    u64 relroSize  = 0;
    u64 vaddrLo    = 0;   // lowest PT_LOAD vaddr (ehdr/phdr anchor for scans)

    // TLS block template (initial image of .tdata + .tbss zero region)
    u64  tlsMemSize   = 0;
    u64  tlsImageSize = 0;
    u64  tlsAlignment = 8;
    std::vector<u8> tlsImage;

    // Dynamic table locations (vaddrs relative to image base)
    u64 dynStrtab = 0, dynSymtab = 0;
    u64 dynRela = 0, dynRelasz = 0;
    u64 dynJmprel = 0, dynPltrelsz = 0;
    u64 dynInit = 0, dynInitArr = 0, dynInitArrSz = 0;

    std::string soname;
    std::vector<std::string> needed;

    u32 importsResolved = 0;

    std::vector<u8> fileBytes; // released after mapping; see scanDynamic()
};

// --------------------------------------------------------------------------
// ImportResolver — maps an undefined guest symbol name to a host function
// pointer (kernel shim, GPU shim, runtime, host libc, ...).
// --------------------------------------------------------------------------
using ImportFn = std::function<void*(const char* name)>;

// --------------------------------------------------------------------------
// ElfLoader
// --------------------------------------------------------------------------
class ElfLoader {
public:
    ElfLoader(class MemoryManager& mem, const std::vector<ImportFn>& resolvers)
        : mem_(mem), resolvers_(resolvers) {}

    // Quick header inspection without mapping (used by `psbet info`).
    static Result inspect(const std::string& path, std::string& reportOut);

    // Full pipeline: map segments -> scan dynamic -> relocate.
    // Initializers run separately via runInitializers().
    Result load(const std::string& path, u32 moduleId, LoadedModule& out);

    Result runInitializers(LoadedModule& mod);

private:
    Result mapSegments(LoadedModule& mod);
    Result scanDynamic(LoadedModule& mod);
    Result relocate(LoadedModule& mod);

    MemoryManager&        mem_;
    std::vector<ImportFn> resolvers_;
};

} // namespace psb
