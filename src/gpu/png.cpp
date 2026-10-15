// ============================================================================
//  PSBET — png.cpp
//  Minimal correct PNG writer:
//    * PNG signature, IHDR (8-bit RGBA, color type 6)
//    * zlib stream with stored (uncompressed) deflate blocks — valid zlib,
//      maximum compatibility, trivially correct
//    * CRC32 per chunk, Adler32 for zlib
//  File size is acceptable for screenshots (RGBA row data compresses well
//  with the Phase 2 filter pipeline; correctness first here).
// ============================================================================
#include "png.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>

namespace psb::gpu {

namespace {

// ---- CRC32 (ISO 3309, as required by PNG) ---------------------------------
u32 crc32(const u8* data, size_t len) {
    static u32 table[256];
    static bool init = false;
    if (!init) {
        for (u32 n = 0; n < 256; ++n) {
            u32 c = n;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[n] = c;
        }
        init = true;
    }
    u32 crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i)
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

// ---- Adler-32 (RFC 1950) --------------------------------------------------
u32 adler32(const u8* data, size_t len) {
    u32 a = 1, b = 0;
    for (size_t i = 0; i < len; ++i) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

void be32(u8* p, u32 v) {
    p[0] = static_cast<u8>(v >> 24);
    p[1] = static_cast<u8>(v >> 16);
    p[2] = static_cast<u8>(v >> 8);
    p[3] = static_cast<u8>(v);
}

void appendChunk(std::vector<u8>& out, const char* type, const std::vector<u8>& payload) {
    u8 lenBe[4];
    be32(lenBe, static_cast<u32>(payload.size()));
    out.insert(out.end(), lenBe, lenBe + 4);

    std::vector<u8> crcInput(type, type + 4);
    crcInput.insert(crcInput.end(), payload.begin(), payload.end());
    const u32 crc = crc32(crcInput.data(), crcInput.size());

    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), payload.begin(), payload.end());

    u8 crcBe[4];
    be32(crcBe, crc);
    out.insert(out.end(), crcBe, crcBe + 4);
}

// zlib-wrapped stored blocks over the raw scanlines.
std::vector<u8> zlibStore(const std::vector<u8>& raw) {
    std::vector<u8> out;
    out.push_back(0x78); // CMF: deflate, 32K window
    out.push_back(0x01); // FLG: check bits, no dict, fastest

    size_t off = 0;
    while (off < raw.size()) {
        const size_t chunk = std::min<size_t>(65535, raw.size() - off);
        const bool last = (off + chunk == raw.size());
        out.push_back(last ? 1 : 0);            // BFINAL + BTYPE=00 (stored)
        u8 lenBe[2] = { static_cast<u8>(chunk & 0xFF),
                        static_cast<u8>(chunk >> 8) };
        out.push_back(lenBe[0]);
        out.push_back(lenBe[1]);
        out.push_back(static_cast<u8>(~chunk & 0xFF));
        out.push_back(static_cast<u8>((~chunk >> 8) & 0xFF));
        out.insert(out.end(), raw.begin() + static_cast<long>(off),
                   raw.begin() + static_cast<long>(off + chunk));
        off += chunk;
    }

    const u32 ad = adler32(raw.data(), raw.size());
    u8 adBe[4] = { static_cast<u8>(ad >> 24), static_cast<u8>(ad >> 16),
                   static_cast<u8>(ad >> 8),  static_cast<u8>(ad) };
    out.insert(out.end(), adBe, adBe + 4);
    return out;
}

} // namespace

bool writePng(const std::string& path, const Image& img) {
    if (!img.width || !img.height || img.rgba.size() != static_cast<size_t>(img.width) * img.height * 4)
        return false;

    static const u8 kSignature[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<u8> file(kSignature, kSignature + 8);

    // IHDR
    std::vector<u8> ihdr(13);
    be32(ihdr.data() + 0, img.width);
    be32(ihdr.data() + 4, img.height);
    ihdr[8]  = 8;   // bit depth
    ihdr[9]  = 6;   // color type RGBA
    ihdr[10] = 0;   // compression: deflate
    ihdr[11] = 0;   // filter: adaptive (we emit type 0 rows)
    ihdr[12] = 0;   // interlace: none
    appendChunk(file, "IHDR", ihdr);

    // Raw scanlines with filter byte 0 (None) per row.
    std::vector<u8> raw;
    raw.reserve(static_cast<size_t>(img.height) * (1 + img.width * 4));
    const size_t rowBytes = static_cast<size_t>(img.width) * 4;
    for (u32 y = 0; y < img.height; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), img.rgba.begin() + static_cast<long>(y * rowBytes),
                   img.rgba.begin() + static_cast<long>((y + 1) * rowBytes));
    }

    appendChunk(file, "IDAT", zlibStore(raw));
    appendChunk(file, "IEND", {});

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(file.data()),
            static_cast<std::streamsize>(file.size()));
    return f.good();
}

} // namespace psb::gpu
