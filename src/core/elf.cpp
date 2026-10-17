// ============================================================================
//  PSBET — elf.cpp
//  ELF64 image loading: mapping, dynamic scanning, relocation, initializers.
//
//  Guest ABI contract (enforced, see docs/GUEST_ABI.md):
//    * ET_DYN x86-64 image at any layout (vaddr 0 anchoring is the common
//      PIE case, no longer mandatory) or ET_EXEC at absolute vaddrs.
//    * Plain SysV entry. Images without PT_DYNAMIC load as static-style.
// ============================================================================
#include "elf.h"

#include <cstring>
#include <fstream>
#include "core/log.h"
#include "core/memory.h"

namespace psb {

namespace {

constexpr u8  kElfClass64     = 2;
constexpr u8  kElfDataLE      = 1;
constexpr u16 kElfTypeExec    = 2;
constexpr u16 kElfTypeDyn     = 3;
constexpr u16 kElfMachineX64  = 62;
constexpr u16 kShnUndef       = 0;
constexpr u16 kShnAbs         = 0xFFF1;
constexpr u8  kSymBindWeak    = 2;

bool isWeakSym(const Elf64Sym& s) { return (s.info >> 4) == kSymBindWeak; }

const char* relocName(u32 t) {
    switch (t) {
        case R_X86_64_64:        return "R_X86_64_64";
        case R_X86_64_TPOFF64:   return "R_X86_64_TPOFF64";
        case R_X86_64_GLOB_DAT:  return "R_X86_64_GLOB_DAT";
        case R_X86_64_JUMP_SLOT: return "R_X86_64_JUMP_SLOT";
        case R_X86_64_RELATIVE:  return "R_X86_64_RELATIVE";
        case R_X86_64_DTPMOD64:  return "R_X86_64_DTPMOD64";
        case R_X86_64_DTPOFF64:  return "R_X86_64_DTPOFF64";
        case R_X86_64_DTPOFF32:  return "R_X86_64_DTPOFF32";
        case R_X86_64_IRELATIVE: return "R_X86_64_IRELATIVE";
        default:                 return "R_X86_64_?";
    }
}

// Symbols that are legitimate-but-unresolvable in a freestanding guest
// (compiler-emitted weak references).
bool ignorableSymbol(const char* name) {
    return std::strncmp(name, "_ITM_", 5) == 0
        || std::strncmp(name, "__gmon__", 8) == 0;
}

} // namespace

// ---------------------------------------------------------------------------
// inspect — header-only report
// ---------------------------------------------------------------------------
Result ElfLoader::inspect(const std::string& path, std::string& reportOut) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return Result::fail(Status::IoError, "cannot open " + path);

    Elf64Ehdr eh{};
    f.read(reinterpret_cast<char*>(&eh), sizeof(eh));
    if (!f || std::memcmp(eh.ident, "\x7f""ELF", 4) != 0)
        return Result::fail(Status::InvalidArgument, "not an ELF image");

    char line[192];
    std::snprintf(line, sizeof(line),
                  "Class: %s  Data: %s  Machine: %u  Type: %s (%u)  OSABI: %u\n",
                  eh.ident[4] == kElfClass64 ? "ELF64" : "other",
                  eh.ident[5] == kElfDataLE ? "LE" : "other",
                  eh.machine,
                  eh.type == kElfTypeExec ? "ET_EXEC"
                  : eh.type == kElfTypeDyn ? "ET_DYN" : "?",
                  eh.type, eh.ident[7]);
    reportOut += line;
    std::snprintf(line, sizeof(line), "Entry: 0x%llx  Phoff: %llu  Phnum: %u\n",
                  static_cast<unsigned long long>(eh.entry),
                  static_cast<unsigned long long>(eh.phoff), eh.phnum);
    reportOut += line;
    reportOut += "  #   TYPE       FLAGS  OFFSET            VADDR             MEMSZ\n";

    std::vector<Elf64Phdr> ph(eh.phnum);
    if (eh.phnum) {
        f.seekg(static_cast<std::streamoff>(eh.phoff));
        f.read(reinterpret_cast<char*>(ph.data()),
               static_cast<std::streamsize>(sizeof(Elf64Phdr) * eh.phnum));
    }

    for (u32 i = 0; i < eh.phnum; ++i) {
        const char* t = ph[i].type == PT_LOAD ? "LOAD"
                      : ph[i].type == PT_DYNAMIC ? "DYNAMIC"
                      : ph[i].type == PT_TLS ? "TLS"
                      : ph[i].type == PT_GNU_RELRO ? "RELRO"
                      : ph[i].type == PT_GNU_STACK ? "GNU_STACK" : "OTHER";
        std::snprintf(line, sizeof(line),
                      "  %-2u  %-9s  %c%c%c    0x%012llx  0x%012llx  %llu\n",
                      i, t,
                      (ph[i].flags & PF_R) ? 'r' : '-',
                      (ph[i].flags & PF_W) ? 'w' : '-',
                      (ph[i].flags & PF_X) ? 'x' : '-',
                      static_cast<unsigned long long>(ph[i].offset),
                      static_cast<unsigned long long>(ph[i].vaddr),
                      static_cast<unsigned long long>(ph[i].memsz));
        reportOut += line;
    }
    return Result::ok();
}

// ---------------------------------------------------------------------------
// load
// ---------------------------------------------------------------------------
// Phase-3 loader contract (docs/GUEST_ABI.md §1):
//   * ET_DYN images load at any layout (first PT_LOAD may sit at any vaddr;
//     relocations anchor the image).
//   * ET_EXEC images load at their absolute vaddrs — the address space must
//     be free there (PA = vaddr inside the regular pool).
//   * Images without PT_DYNAMIC load as static-style (no relocations).
//   * R_X86_64_IRELATIVE is resolved by executing the addend resolver.
// ---------------------------------------------------------------------------
Result ElfLoader::load(const std::string& path, u32 /*moduleId*/, LoadedModule& mod) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return Result::fail(Status::IoError, "cannot open image: " + path);

    const std::streamsize size = f.tellg();
    f.seekg(0);
    mod.fileBytes.resize(static_cast<size_t>(size));
    if (size > 0 && !f.read(reinterpret_cast<char*>(mod.fileBytes.data()), size))
        return Result::fail(Status::IoError, "short read on " + path);

    if (mod.fileBytes.size() < sizeof(Elf64Ehdr))
        return Result::fail(Status::InvalidArgument, "file too small to be an ELF image");

    const Elf64Ehdr* eh = reinterpret_cast<const Elf64Ehdr*>(mod.fileBytes.data());

    if (std::memcmp(eh->ident, "\x7f""ELF", 4) != 0)
        return Result::fail(Status::InvalidArgument, "not an ELF image: " + path);

    // Encrypted SELF (PS4/PS5 container) detection: magic "\0SCE".
    if (std::memcmp(eh->ident, "\x00SCE", 4) == 0 || std::memcmp(eh->ident, "\x7fSCE", 4) == 0)
        return Result::fail(Status::Unsupported,
            "encrypted SELF container detected. PSBET ships no key material "
            "(clean-room policy). Provide a decrypted ELF image of content you "
            "legally own. See docs/GUEST_ABI.md.");

    if (eh->ident[4] != kElfClass64 || eh->ident[5] != kElfDataLE)
        return Result::fail(Status::Unsupported, "only ELF64 little-endian is supported");

    if (eh->machine != kElfMachineX64)
        return Result::fail(Status::Unsupported, "only x86-64 guest images are supported");

    if (eh->type != kElfTypeExec && eh->type != kElfTypeDyn)
        return Result::fail(Status::Unsupported,
            "unsupported ELF type (want ET_DYN or ET_EXEC)");
    mod.isExec = (eh->type == kElfTypeExec);

    if (eh->ident[7] == 9) // ELFOSABI_FREEBSD — PS5's ABI flavour
        LOG_INFO("ELF: PS5-style OSABI (FreeBSD) detected — accepted");

    if (eh->phnum == 0 || eh->phnum > 256)
        return Result::fail(Status::InvalidArgument, "implausible program header count");

    // Entry offset from the FILE header (valid for every layout: vaddr 0
    // anchors ET_DYN, absolute vaddrs anchor ET_EXEC).
    mod.entryOffset = eh->entry;

    mod.path = path;
    Result r = mapSegments(mod);
    if (!r) return r;
    mod.entry = mod.base + mod.entryOffset;
    r = scanDynamic(mod);      // tolerant: static-style images pass through
    if (!r) return r;
    r = relocate(mod);
    if (!r) return r;

    // RELRO: after relocations, make the GNU_RELRO region read-only.
    if (mod.relroSize) {
        if (!mem_.protectRange(mod.base + mod.relroVaddr, mod.relroSize,
                               vm::Prot::ReadOnly))
            LOG_WARN("ELF: RELRO protection could not be fully applied");
        else
            LOG_DEBUG("ELF: RELRO region made read-only (vaddr 0x%llx, %llu B)",
                      static_cast<unsigned long long>(mod.relroVaddr),
                      static_cast<unsigned long long>(mod.relroSize));
    }
    return Result::ok();
}

// ---------------------------------------------------------------------------
// mapSegments — map every PT_LOAD into the guest address space.
//
//   ET_DYN: one contiguous span covers [lo, hi); the image's vaddr 0 anchors
//           at (span - lo), so mod.base + vaddr always lands inside the span.
//   ET_EXEC: the span reserves PA = vaddr (absolute layout); mod.base is the
//            arena base (guest vaddrs are absolute).
//   The span is mapped read-write first (the loader's patch window), file
//   bytes + bss are copied, then each segment gets its final protections.
// ---------------------------------------------------------------------------
Result ElfLoader::mapSegments(LoadedModule& mod) {
    const Elf64Ehdr* eh = reinterpret_cast<const Elf64Ehdr*>(mod.fileBytes.data());
    const u8* file = mod.fileBytes.data();
    const u64 fileSize = mod.fileBytes.size();

    std::vector<Elf64Phdr> ph(eh->phnum);
    if (eh->phoff + sizeof(Elf64Phdr) * eh->phnum > fileSize)
        return Result::fail(Status::InvalidArgument, "program header table out of bounds");
    std::memcpy(ph.data(), file + eh->phoff, sizeof(Elf64Phdr) * eh->phnum);

    u64 lo = ~0ull, hi = 0, maxAlign = 0x1000;
    for (const auto& p : ph) {
        if (p.type != PT_LOAD) continue;
        lo = std::min<u64>(lo, p.vaddr);
        hi = std::max<u64>(hi, p.vaddr + p.memsz);
        if (p.align) maxAlign = std::max<u64>(maxAlign, p.align);
    }
    if (hi <= lo) return Result::fail(Status::InvalidArgument, "no PT_LOAD segments");

    const std::string imageName = mod.soname.empty() ? "guest-image" : mod.soname;
    GuestMemory& gm = mem_.guest();

    // Span in PA space: page-aligned superset of every PT_LOAD.
    const u64 spanLo = mod.isExec ? (lo & ~0xFFFull) : lo;
    const u64 spanHi = alignUp(hi, 0x1000);
    const u64 spanLen = spanHi - spanLo;
    if (spanHi > GuestMemory::kPaRegularSize)
        return Result::fail(Status::Unsupported,
            "image vaddrs exceed the regular physical pool "
            "(ET_EXEC images must live below 8 GiB)");

    u64 paSpan = 0;
    if (mod.isExec) {
        paSpan = gm.allocDirectFixed(GuestMemory::kPoolRegular, spanLo, spanLen,
                                     imageName.c_str());
        if (!paSpan)
            return Result::fail(Status::OutOfMemory,
                "ET_EXEC image vaddrs collide with existing allocations "
                "(or exceed the pool)");
        mod.base = mem_.arenaBase();
    } else {
        paSpan = gm.allocDirect(GuestMemory::kPoolRegular, spanLen, maxAlign,
                                0, 0, imageName.c_str());
        if (!paSpan)
            return Result::fail(Status::OutOfMemory, "failed to allocate image span");
        mod.base = mem_.arenaBase() + paSpan - lo;
    }
    mod.paBase = paSpan;
    mod.imageSize = spanLen;
    mod.vaddrLo = lo;

    // Loader patch window: whole span read-write (copy + relocation phase),
    // final per-segment protections applied below.
    if (!gm.mapDirect(paSpan, spanLen, vm::Prot::ReadWrite, imageName.c_str()))
        return Result::fail(Status::OutOfMemory,
                            "page-table map failed for the image span");
    u8* span = reinterpret_cast<u8*>(mem_.arenaBase() + paSpan);

    for (const auto& p : ph) {
        if (p.type != PT_LOAD) continue;
        if (p.offset + p.filesz > fileSize)
            return Result::fail(Status::InvalidArgument, "PT_LOAD segment out of file bounds");

        u8* dst = span + (p.vaddr - spanLo);
        std::memcpy(dst, file + p.offset, p.filesz);
        if (p.memsz > p.filesz)
            std::memset(dst + p.filesz, 0, p.memsz - p.filesz);

        mod.segments.push_back({p.vaddr, p.memsz, p.flags});

        vm::Prot prot = vm::Prot::ReadOnly;
        if (p.flags & PF_W) prot = vm::Prot::ReadWrite;
        if (p.flags & PF_X) prot = (p.flags & PF_W) ? vm::Prot::ReadWriteExec
                                                    : vm::Prot::ReadExec;
        if (!mem_.protectRange(reinterpret_cast<u64>(dst), p.memsz, prot))
            return Result::fail(Status::InternalError, "failed to apply segment protections");

        LOG_DEBUG("ELF: mapped %c%c%c seg vaddr 0x%llx (%llu B)",
                  (p.flags & PF_R) ? 'R' : '-', (p.flags & PF_W) ? 'W' : '-',
                  (p.flags & PF_X) ? 'X' : '-',
                  static_cast<unsigned long long>(p.vaddr),
                  static_cast<unsigned long long>(p.memsz));
    }

    for (const auto& p : ph) {
        if (p.type == PT_TLS) {
            if (mod.tlsImageSize) {
                LOG_WARN("ELF: multiple PT_TLS segments — the first one is used");
                continue;
            }
            mod.tlsImageSize = p.filesz;
            mod.tlsMemSize   = p.memsz;
            mod.tlsAlignment = p.align ? p.align : 8;
            mod.tlsImage.assign(file + p.offset, file + p.offset + p.filesz);
            mod.tlsImage.resize(static_cast<size_t>(p.memsz)); // zero .tbss tail
        } else if (p.type == PT_GNU_RELRO) {
            mod.relroVaddr = p.vaddr;
            mod.relroSize  = p.memsz;
        }
    }
    return Result::ok();
}

// ---------------------------------------------------------------------------
// scanDynamic — walk PT_DYNAMIC from the mapped image, record everything the
// relocation and initializer passes need, then release the file bytes.
// Static-style images (no PT_DYNAMIC) pass through with a log line.
// ---------------------------------------------------------------------------
Result ElfLoader::scanDynamic(LoadedModule& mod) {
    // ehdr/phdrs sit at vaddr lo + file offset (the first PT_LOAD carries
    // them with p_offset 0), so anchor the walk at base + vaddrLo.
    const Elf64Ehdr* eh = reinterpret_cast<const Elf64Ehdr*>(mod.base + mod.vaddrLo);
    const Elf64Phdr* phIt = reinterpret_cast<const Elf64Phdr*>(
        mod.base + mod.vaddrLo + eh->phoff);

    u64 dynVaddr = 0;
    for (u32 i = 0; i < eh->phnum; ++i)
        if (phIt[i].type == PT_DYNAMIC) dynVaddr = phIt[i].vaddr;
    if (!dynVaddr) {
        LOG_INFO("ELF: image has no PT_DYNAMIC — loading as static-style");
        mod.fileBytes.clear();
        mod.fileBytes.shrink_to_fit();
        return Result::ok();
    }

    const Elf64Dyn* dyn = reinterpret_cast<const Elf64Dyn*>(mod.base + dynVaddr);

    bool haveStrtab = false;
    for (size_t i = 0; dyn[i].tag != DT_NULL; ++i) {
        switch (dyn[i].tag) {
            case DT_STRTAB:       mod.dynStrtab  = dyn[i].val; haveStrtab = true; break;
            case DT_SYMTAB:       mod.dynSymtab  = dyn[i].val; break;
            case DT_RELA:         mod.dynRela    = dyn[i].val; break;
            case DT_RELASZ:       mod.dynRelasz  = dyn[i].val; break;
            case DT_JMPREL:       mod.dynJmprel  = dyn[i].val; break;
            case DT_PLTRELSZ:     mod.dynPltrelsz = dyn[i].val; break;
            case DT_INIT:         mod.dynInit    = dyn[i].val; break;
            case DT_INIT_ARRAY:   mod.dynInitArr = dyn[i].val; break;
            case DT_INIT_ARRAYSZ: mod.dynInitArrSz = dyn[i].val; break;
            case DT_SONAME:
                if (haveStrtab)
                    mod.soname = reinterpret_cast<const char*>(
                        mod.base + mod.dynStrtab + dyn[i].val);
                break;
            case DT_NEEDED:
                if (haveStrtab)
                    mod.needed.emplace_back(reinterpret_cast<const char*>(
                        mod.base + mod.dynStrtab + dyn[i].val));
                break;
            default: break;
        }
    }

    if (mod.dynStrtab && !mod.dynSymtab)
        return Result::fail(Status::InvalidArgument, "image lacks DT_SYMTAB");

    mod.fileBytes.clear();
    mod.fileBytes.shrink_to_fit();
    return Result::ok();
}

// ---------------------------------------------------------------------------
// relocate
// ---------------------------------------------------------------------------
Result ElfLoader::relocate(LoadedModule& mod) {
    if (!mod.dynRela && !mod.dynJmprel) {
        LOG_DEBUG("relocate: no relocation tables (static-style image)");
        return Result::ok();
    }
    if (!mod.dynStrtab || !mod.dynSymtab)
        return Result::fail(Status::InvalidArgument, "relocations without DT_STRTAB/DT_SYMTAB");

    const char*     strBase = reinterpret_cast<const char*>(mod.base + mod.dynStrtab);
    const Elf64Sym* symBase = reinterpret_cast<const Elf64Sym*>(mod.base + mod.dynSymtab);

    auto resolveSymbol = [&](const Elf64Sym& s, u64& out, std::string& err) -> bool {
        if (s.shndx != kShnUndef) {
            out = (s.shndx == kShnAbs) ? s.value : mod.base + s.value;
            return true;
        }
        const char* name = strBase + s.name;
        if (!name[0]) { out = 0; return true; }
        for (const auto& fn : resolvers_) {
            if (!fn) continue;
            if (void* p = fn(name)) { out = reinterpret_cast<u64>(p); return true; }
        }
        if (ignorableSymbol(name) || isWeakSym(s)) { out = 0; return true; }
        err = std::string("unresolved import: ") + name;
        return false;
    };

    auto processRelas = [&](u64 tableVaddr, u64 bytes) -> Result {
        if (!tableVaddr || !bytes) return Result::ok();
        const Elf64Rela* rel = reinterpret_cast<const Elf64Rela*>(mod.base + tableVaddr);
        const size_t n = bytes / sizeof(Elf64Rela);
        LOG_DEBUG("relocate: processing %zu relocations at vaddr 0x%llx", n,
                  static_cast<unsigned long long>(tableVaddr));
        for (size_t i = 0; i < n; ++i) {
            LOG_DEBUG("relocate: [%zu] off=0x%llx info=0x%llx (sym=%u type=%u)",
                      i, static_cast<unsigned long long>(rel[i].offset),
                      static_cast<unsigned long long>(rel[i].info),
                      static_cast<unsigned>(static_cast<u64>(rel[i].info) >> 32),
                      static_cast<unsigned>(rel[i].info & 0xffffffffu));
            const u64 where = mod.base + rel[i].offset;
            const u64 info = rel[i].info;
            const u32 type  = static_cast<u32>(info & 0xffffffffu);
            const u32 symIx = static_cast<u32>(info >> 32);

            if (type == R_X86_64_RELATIVE) {
                *reinterpret_cast<u64*>(where) = mod.base + static_cast<u64>(rel[i].addend);
                continue;
            }
            if (type == R_X86_64_IRELATIVE) {
                // Phase 3: resolve by executing the addend resolver (guest
                // code, C ABI, returns the implementation address).
                using IrelFn = u64 (*)(void);
                const u64 resolver = symIx
                    ? mod.base + symBase[symIx].value + static_cast<u64>(rel[i].addend)
                    : mod.base + static_cast<u64>(rel[i].addend);
                const u64 impl = reinterpret_cast<IrelFn>(resolver)();
                *reinterpret_cast<u64*>(where) = impl;
                LOG_DEBUG("relocate: IRELATIVE at 0x%llx -> 0x%llx",
                          static_cast<unsigned long long>(rel[i].offset),
                          static_cast<unsigned long long>(impl));
                continue;
            }

            u64 S = 0;
            if (symIx) {
                const Elf64Sym* sym = &symBase[symIx];
                std::string err;
                if (!resolveSymbol(*sym, S, err))
                    return Result::fail(Status::NotFound, err);
            }
            const u64 A = static_cast<u64>(rel[i].addend);

            switch (type) {
                case R_X86_64_64:
                case R_X86_64_GLOB_DAT:
                case R_X86_64_JUMP_SLOT:
                    *reinterpret_cast<u64*>(where) = S + A;
                    if (S) mod.importsResolved++;
                    break;
                case R_X86_64_DTPMOD64:
                    *reinterpret_cast<u64*>(where) = 1; // single TLS module
                    break;
                case R_X86_64_DTPOFF64:
                    *reinterpret_cast<u64*>(where) =
                        (symIx ? symBase[symIx].value : 0) + A;
                    break;
                case R_X86_64_DTPOFF32:
                    *reinterpret_cast<i32*>(where) =
                        static_cast<i32>((symIx ? symBase[symIx].value : 0) + A);
                    break;
                case R_X86_64_TPOFF64:
                    return Result::fail(Status::Unsupported,
                        "R_X86_64_TPOFF64 (initial-exec TLS) unsupported; build "
                        "guests with the general-dynamic TLS model (the default "
                        "for -shared; see docs/GUEST_ABI.md)");
                default:
                    return Result::fail(Status::Unsupported,
                        std::string("unsupported relocation ") + relocName(type)
                        + " (type=" + std::to_string(type) + ")");
            }
        }
        return Result::ok();
    };

    Result r = processRelas(mod.dynRela, mod.dynRelasz);
    if (!r) return r;
    r = processRelas(mod.dynJmprel, mod.dynPltrelsz);
    if (!r) return r;

    LOG_INFO("ELF: relocations applied, %u dynamic imports bound", mod.importsResolved);
    return Result::ok();
}

// ---------------------------------------------------------------------------
// runInitializers — DT_INIT then DT_INIT_ARRAY (like a real dynamic linker)
// ---------------------------------------------------------------------------
Result ElfLoader::runInitializers(LoadedModule& mod) {
    using InitFn = void (*)();
    if (mod.dynInit) {
        reinterpret_cast<InitFn>(mod.base + mod.dynInit)();
        LOG_DEBUG("ELF: DT_INIT executed");
    }
    if (mod.dynInitArr && mod.dynInitArrSz) {
        const u64* arr = reinterpret_cast<const u64*>(mod.base + mod.dynInitArr);
        const size_t n = mod.dynInitArrSz / sizeof(u64);
        for (size_t i = 0; i < n; ++i)
            reinterpret_cast<InitFn>(mod.base + arr[i])();
        LOG_DEBUG("ELF: %zu DT_INIT_ARRAY entries executed", n);
    }
    return Result::ok();
}

} // namespace psb
