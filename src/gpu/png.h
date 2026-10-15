// ============================================================================
//  PSBET — png.h
//  Zero-dependency PNG encoder (RGBA8). Used for screenshot capture and
//  validation evidence. Implements CRC32, Adler32 and stored-block zlib
//  internally — no third-party libraries.
// ============================================================================
#pragma once

#include <string>
#include <vector>
#include "psbet/common.h"

namespace psb::gpu {

struct Image {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> rgba;   // width*height*4 bytes
};

// Writes an RGBA8 PNG. Returns false on I/O failure.
bool writePng(const std::string& path, const Image& img);

} // namespace psb::gpu
