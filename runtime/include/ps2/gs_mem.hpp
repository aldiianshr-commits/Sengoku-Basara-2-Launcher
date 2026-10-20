// GS local memory addressing (swizzling) for every pixel storage mode.
#pragma once
#include <cstdint>

namespace ps2::gsmem {

enum Psm : uint32_t {
    PSMCT32 = 0x00,
    PSMCT24 = 0x01,
    PSMCT16 = 0x02,
    PSMCT16S = 0x0A,
    PSMT8 = 0x13,
    PSMT4 = 0x14,
    PSMT8H = 0x1B,
    PSMT4HL = 0x24,
    PSMT4HH = 0x2C,
    PSMZ32 = 0x30,
    PSMZ24 = 0x31,
    PSMZ16 = 0x32,
    PSMZ16S = 0x3A,
};

constexpr uint32_t VRAM_SIZE = 4u * 1024 * 1024;

// Block index (0..31) of a block inside a page.
inline uint32_t block32(uint32_t bx, uint32_t by) {
    return (bx & 1) | ((by & 1) << 1) | ((bx & 2) << 1) | ((by & 2) << 2) | ((bx & 4) << 2);
}
inline uint32_t block16(uint32_t bx, uint32_t by) {
    return (by & 1) | ((bx & 1) << 1) | ((by & 2) << 1) | ((bx & 2) << 2) | ((by & 4) << 2);
}
inline uint32_t block16S(uint32_t bx, uint32_t by) {
    return (by & 1) | ((bx & 1) << 1) | (by & 4) | ((by & 2) << 2) | ((bx & 2) << 3);
}

// Word index (0..63) of a 32-bit pixel inside an 8x8 block.
inline uint32_t column32(uint32_t x, uint32_t y) { return ((y >> 1) << 4) | ((x >> 1) << 2) | ((y & 1) << 1) | (x & 1); }
// Halfword index (0..127) inside a 16x8 block.
inline uint32_t column16(uint32_t x, uint32_t y) {
    return ((y >> 1) << 5) | (((x & 7) >> 1) << 3) | ((y & 1) << 2) | ((x & 1) << 1) | ((x >> 3) & 1);
}
// Byte index (0..255) inside a 16x16 block.
inline uint32_t column8(uint32_t x, uint32_t y) {
    return ((x & 1) << 2) | (((x >> 1) & 1) << 4) | ((((x >> 2) ^ (y >> 1) ^ (y >> 2)) & 1) << 5) |
           (((x >> 3) & 1) << 1) | ((y & 1) << 3) | ((y >> 1) & 1) | (((y >> 2) & 3) << 6);
}
// Nibble index (0..511) inside a 32x16 block.
inline uint32_t column4(uint32_t x, uint32_t y) {
    return ((x & 1) << 3) | (((x >> 1) & 1) << 5) | ((((x >> 2) ^ (y >> 1) ^ (y >> 2)) & 1) << 6) |
           (((x >> 3) & 1) << 1) | (((x >> 4) & 1) << 2) | ((y & 1) << 4) | ((y >> 1) & 1) | (((y >> 2) & 3) << 7);
}

// Byte address of a 32-bit pixel (PSMCT32/24, Z32/24 when zfmt).
inline uint32_t addr32(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, bool z = false) {
    uint32_t blk = block32((x >> 3) & 7, (y >> 3) & 3);
    if (z) blk ^= 24;
    const uint32_t block = bp + (y >> 5) * bw * 32 + (x >> 6) * 32 + blk;
    return ((block * 64 + column32(x & 7, y & 7)) * 4) & (VRAM_SIZE - 1);
}
inline uint32_t addr16(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, bool s, bool z = false) {
    uint32_t blk = s ? block16S((x >> 4) & 3, (y >> 3) & 7) : block16((x >> 4) & 3, (y >> 3) & 7);
    if (z) blk ^= 24;
    const uint32_t block = bp + (y >> 6) * bw * 32 + (x >> 6) * 32 + blk;
    return ((block * 128 + column16(x & 15, y & 7)) * 2) & (VRAM_SIZE - 1);
}
inline uint32_t addr8(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) {
    const uint32_t block = bp + (y >> 6) * (bw >> 1) * 32 + (x >> 7) * 32 + block32((x >> 4) & 7, (y >> 4) & 3);
    return (block * 256 + column8(x & 15, y & 15)) & (VRAM_SIZE - 1);
}
// Returns nibble address (byte address * 2 + nibble).
inline uint32_t addr4(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) {
    const uint32_t block = bp + (y >> 7) * (bw >> 1) * 32 + (x >> 7) * 32 + block16((x >> 5) & 3, (y >> 4) & 7);
    return (block * 512 + column4(x & 31, y & 15)) & (VRAM_SIZE * 2 - 1);
}

inline uint32_t bitsPerPixel(uint32_t psm) {
    switch (psm) {
    case PSMCT32: case PSMCT24: case PSMZ32: case PSMZ24: case PSMT8H: case PSMT4HL: case PSMT4HH: return 32;
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return 16;
    case PSMT8: return 8;
    case PSMT4: return 4;
    default: return 32;
    }
}

} // namespace ps2::gsmem
