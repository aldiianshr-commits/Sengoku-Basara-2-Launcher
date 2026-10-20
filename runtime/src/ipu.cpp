// IPU: MPEG-1/2 macroblock decoding coprocessor.  See ps2/ipu.hpp.
//
// The VLC tables are those of ISO/IEC 13818-2 Annex B (B.1 macroblock
// address increment, B.2-B.4 macroblock types, B.9 coded block pattern,
// B.10 motion codes, B.11 dmvector, B.12/B.13 DC size, B.14/B.15 DCT
// coefficients).  Commands that need more bitstream than is buffered are
// retried from their start once more data arrives (FIFO write or DMA).
#include "ps2/ipu.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>

#include "ps2/context.hpp"
#include "ps2/hw.hpp"
#include "ps2/log.hpp"
#include "ps2/machine.hpp"

namespace ps2 {

namespace {

bool ipuTrace() {
    static const bool on = std::getenv("SB2_IPU_TRACE") != nullptr;
    return on;
}

// ------------------------------------------------------------------ VLC tables

struct VlcEntry {
    uint8_t len = 0; // 0 = invalid
    int16_t a = 0;   // value / run
    int16_t b = 0;   // level
};

class Vlc {
public:
    struct Code {
        const char* bits;
        int a, b;
    };
    Vlc(int maxLen, std::initializer_list<Code> codes) : maxLen_(maxLen), lut_((size_t)1 << maxLen) {
        for (const Code& c : codes) {
            std::string s;
            for (const char* p = c.bits; *p; ++p)
                if (*p == '0' || *p == '1') s.push_back(*p);
            const int len = (int)s.size();
            uint32_t v = 0;
            for (char ch : s) v = (v << 1) | (uint32_t)(ch - '0');
            const int shift = maxLen - len;
            for (uint32_t k = 0; k < (1u << shift); ++k) {
                VlcEntry& e = lut_[(v << shift) | k];
                e.len = (uint8_t)len;
                e.a = (int16_t)c.a;
                e.b = (int16_t)c.b;
            }
        }
    }
    int maxLen() const { return maxLen_; }
    const VlcEntry& lookup(uint32_t bits) const { return lut_[bits]; }

private:
    int maxLen_;
    std::vector<VlcEntry> lut_;
};

// Special codes in the DCT tables.
constexpr int kEob = 100, kEsc = 101;

const Vlc& tableMbai() {
    static const Vlc t(11, {{"1", 1, 0}, {"011", 2, 0}, {"010", 3, 0}, {"0011", 4, 0}, {"0010", 5, 0}, {"00011", 6, 0},
                            {"00010", 7, 0}, {"0000111", 8, 0}, {"0000110", 9, 0}, {"00001011", 10, 0}, {"00001010", 11, 0},
                            {"00001001", 12, 0}, {"00001000", 13, 0}, {"00000111", 14, 0}, {"00000110", 15, 0},
                            {"0000010111", 16, 0}, {"0000010110", 17, 0}, {"0000010101", 18, 0}, {"0000010100", 19, 0},
                            {"0000010011", 20, 0}, {"0000010010", 21, 0}, {"00000100011", 22, 0}, {"00000100010", 23, 0},
                            {"00000100001", 24, 0}, {"00000100000", 25, 0}, {"00000011111", 26, 0}, {"00000011110", 27, 0},
                            {"00000011101", 28, 0}, {"00000011100", 29, 0}, {"00000011011", 30, 0}, {"00000011010", 31, 0},
                            {"00000011001", 32, 0}, {"00000011000", 33, 0}, {"00000001000", 0x23, 0}, {"00000001111", 0x22, 0}});
    return t;
}

// macroblock_type flags: intra 1, pattern 2, backward 4, forward 8, quant 16
const Vlc& tableMbType(int pct) {
    static const Vlc i(2, {{"1", 0x01, 0}, {"01", 0x11, 0}});
    static const Vlc p(6, {{"1", 0x0A, 0}, {"01", 0x02, 0}, {"001", 0x08, 0}, {"00011", 0x01, 0}, {"00010", 0x1A, 0},
                           {"00001", 0x12, 0}, {"000001", 0x11, 0}});
    static const Vlc b(6, {{"10", 0x0C, 0}, {"11", 0x0E, 0}, {"010", 0x04, 0}, {"011", 0x06, 0}, {"0010", 0x08, 0},
                           {"0011", 0x0A, 0}, {"00011", 0x01, 0}, {"00010", 0x1E, 0}, {"000011", 0x1A, 0},
                           {"000010", 0x16, 0}, {"000001", 0x11, 0}});
    static const Vlc d(1, {{"1", 0x01, 0}});
    switch (pct) {
    case 2: return p;
    case 3: return b;
    case 4: return d;
    default: return i;
    }
}

const Vlc& tableCbp() {
    static const Vlc t(9, {{"111", 60, 0}, {"1101", 4, 0}, {"1100", 8, 0}, {"1011", 16, 0}, {"1010", 32, 0},
                           {"10011", 12, 0}, {"10010", 48, 0}, {"10001", 20, 0}, {"10000", 40, 0}, {"01111", 28, 0},
                           {"01110", 44, 0}, {"01101", 52, 0}, {"01100", 56, 0}, {"01011", 1, 0}, {"01010", 61, 0},
                           {"01001", 2, 0}, {"01000", 62, 0}, {"001111", 24, 0}, {"001110", 36, 0}, {"001101", 3, 0},
                           {"001100", 63, 0}, {"0010111", 5, 0}, {"0010110", 9, 0}, {"0010101", 17, 0},
                           {"0010100", 33, 0}, {"0010011", 6, 0}, {"0010010", 10, 0}, {"0010001", 18, 0},
                           {"0010000", 34, 0}, {"00011111", 7, 0}, {"00011110", 11, 0}, {"00011101", 19, 0},
                           {"00011100", 35, 0}, {"00011011", 13, 0}, {"00011010", 49, 0}, {"00011001", 21, 0},
                           {"00011000", 41, 0}, {"00010111", 14, 0}, {"00010110", 50, 0}, {"00010101", 22, 0},
                           {"00010100", 42, 0}, {"00010011", 15, 0}, {"00010010", 51, 0}, {"00010001", 23, 0},
                           {"00010000", 43, 0}, {"00001111", 25, 0}, {"00001110", 37, 0}, {"00001101", 26, 0},
                           {"00001100", 38, 0}, {"00001011", 29, 0}, {"00001010", 45, 0}, {"00001001", 53, 0},
                           {"00001000", 57, 0}, {"00000111", 30, 0}, {"00000110", 46, 0}, {"00000101", 54, 0},
                           {"00000100", 58, 0}, {"000000111", 31, 0}, {"000000110", 47, 0}, {"000000101", 55, 0},
                           {"000000100", 59, 0}, {"000000011", 27, 0}, {"000000010", 39, 0}, {"000000001", 0, 0}});
    return t;
}

// motion_code magnitude (sign bit follows unless 0)
const Vlc& tableMotion() {
    static const Vlc t(10, {{"1", 0, 0}, {"01", 1, 0}, {"001", 2, 0}, {"0001", 3, 0}, {"000011", 4, 0}, {"0000101", 5, 0},
                            {"0000100", 6, 0}, {"0000011", 7, 0}, {"000001011", 8, 0}, {"000001010", 9, 0},
                            {"000001001", 10, 0}, {"0000010001", 11, 0}, {"0000010000", 12, 0}, {"0000001111", 13, 0},
                            {"0000001110", 14, 0}, {"0000001101", 15, 0}, {"0000001100", 16, 0}});
    return t;
}

const Vlc& tableDmv() {
    static const Vlc t(2, {{"0", 0, 0}, {"10", 1, 0}, {"11", -1, 0}});
    return t;
}

const Vlc& tableDcLuma() {
    static const Vlc t(9, {{"100", 0, 0}, {"00", 1, 0}, {"01", 2, 0}, {"101", 3, 0}, {"110", 4, 0}, {"1110", 5, 0},
                           {"11110", 6, 0}, {"111110", 7, 0}, {"1111110", 8, 0}, {"11111110", 9, 0}, {"111111110", 10, 0},
                           {"111111111", 11, 0}});
    return t;
}

const Vlc& tableDcChroma() {
    static const Vlc t(10, {{"00", 0, 0}, {"01", 1, 0}, {"10", 2, 0}, {"110", 3, 0}, {"1110", 4, 0}, {"11110", 5, 0},
                            {"111110", 6, 0}, {"1111110", 7, 0}, {"11111110", 8, 0}, {"111111110", 9, 0},
                            {"1111111110", 10, 0}, {"1111111111", 11, 0}});
    return t;
}

// Long codes shared by B.14 and B.15 (13+ bits, without the sign bit).
#define DCT_LONG_CODES                                                                                                   \
    {"0000000011111", 0, 16}, {"0000000011110", 0, 17}, {"0000000011101", 0, 18}, {"0000000011100", 0, 19},              \
        {"0000000011011", 0, 20}, {"0000000011010", 0, 21}, {"0000000011001", 0, 22}, {"0000000011000", 0, 23},          \
        {"0000000010111", 0, 24}, {"0000000010110", 0, 25}, {"0000000010101", 0, 26}, {"0000000010100", 0, 27},          \
        {"0000000010011", 0, 28}, {"0000000010010", 0, 29}, {"0000000010001", 0, 30}, {"0000000010000", 0, 31},          \
        {"00000000011000", 0, 32}, {"00000000010111", 0, 33}, {"00000000010110", 0, 34}, {"00000000010101", 0, 35},      \
        {"00000000010100", 0, 36}, {"00000000010011", 0, 37}, {"00000000010010", 0, 38}, {"00000000010001", 0, 39},      \
        {"00000000010000", 0, 40}, {"00000000011111", 1, 8}, {"00000000011110", 1, 9}, {"00000000011101", 1, 10},        \
        {"00000000011100", 1, 11}, {"00000000011011", 1, 12}, {"00000000011010", 1, 13}, {"00000000011001", 1, 14},      \
        {"000000000010011", 1, 15}, {"000000000010010", 1, 16}, {"000000000010001", 1, 17}, {"000000000010000", 1, 18},  \
        {"000000000010100", 6, 3}, {"000000000011010", 11, 2}, {"000000000011001", 12, 2}, {"000000000011000", 13, 2},   \
        {"000000000010111", 14, 2}, {"000000000010110", 15, 2}, {"000000000010101", 16, 2}, {"000000000011111", 27, 1},  \
        {"000000000011110", 28, 1}, {"000000000011101", 29, 1}, {"000000000011100", 30, 1}, {"000000000011011", 31, 1}

// B.14 (codes without the trailing sign bit).  The "first coefficient"
// variant (code '1s' = 0,1) is handled by the caller.
const Vlc& tableDct0() {
    static const Vlc t(16, {{"10", kEob, 0},
                            {"11", 0, 1},
                            {"011", 1, 1},
                            {"0100", 0, 2},
                            {"0101", 2, 1},
                            {"00101", 0, 3},
                            {"00111", 3, 1},
                            {"00110", 4, 1},
                            {"000110", 1, 2},
                            {"000111", 5, 1},
                            {"000101", 6, 1},
                            {"000100", 7, 1},
                            {"0000110", 0, 4},
                            {"0000100", 2, 2},
                            {"0000111", 8, 1},
                            {"0000101", 9, 1},
                            {"000001", kEsc, 0},
                            {"00100110", 0, 5},
                            {"00100001", 0, 6},
                            {"00100101", 1, 3},
                            {"00100100", 3, 2},
                            {"00100111", 10, 1},
                            {"00100011", 11, 1},
                            {"00100010", 12, 1},
                            {"00100000", 13, 1},
                            {"0000001010", 0, 7},
                            {"0000001100", 1, 4},
                            {"0000001011", 2, 3},
                            {"0000001111", 4, 2},
                            {"0000001001", 5, 2},
                            {"0000001110", 14, 1},
                            {"0000001101", 15, 1},
                            {"0000001000", 16, 1},
                            {"000000011101", 0, 8},
                            {"000000011000", 0, 9},
                            {"000000010011", 0, 10},
                            {"000000010000", 0, 11},
                            {"000000011011", 1, 5},
                            {"000000010100", 2, 4},
                            {"000000011100", 3, 3},
                            {"000000010010", 4, 3},
                            {"000000011110", 6, 2},
                            {"000000010101", 7, 2},
                            {"000000010001", 8, 2},
                            {"000000011111", 17, 1},
                            {"000000011010", 18, 1},
                            {"000000011001", 19, 1},
                            {"000000010111", 20, 1},
                            {"000000010110", 21, 1},
                            {"0000000011010", 0, 12},
                            {"0000000011001", 0, 13},
                            {"0000000011000", 0, 14},
                            {"0000000010111", 0, 15},
                            {"0000000010110", 1, 6},
                            {"0000000010101", 1, 7},
                            {"0000000010100", 2, 5},
                            {"0000000010011", 3, 4},
                            {"0000000010010", 5, 3},
                            {"0000000010001", 9, 2},
                            {"0000000010000", 10, 2},
                            {"0000000011111", 22, 1},
                            {"0000000011110", 23, 1},
                            {"0000000011101", 24, 1},
                            {"0000000011100", 25, 1},
                            {"0000000011011", 26, 1},
                            {"00000000011111", 0, 16},
                            {"00000000011110", 0, 17},
                            {"00000000011101", 0, 18},
                            {"00000000011100", 0, 19},
                            {"00000000011011", 0, 20},
                            {"00000000011010", 0, 21},
                            {"00000000011001", 0, 22},
                            {"00000000011000", 0, 23},
                            {"00000000010111", 0, 24},
                            {"00000000010110", 0, 25},
                            {"00000000010101", 0, 26},
                            {"00000000010100", 0, 27},
                            {"00000000010011", 0, 28},
                            {"00000000010010", 0, 29},
                            {"00000000010001", 0, 30},
                            {"00000000010000", 0, 31},
                            {"000000000011000", 0, 32},
                            {"000000000010111", 0, 33},
                            {"000000000010110", 0, 34},
                            {"000000000010101", 0, 35},
                            {"000000000010100", 0, 36},
                            {"000000000010011", 0, 37},
                            {"000000000010010", 0, 38},
                            {"000000000010001", 0, 39},
                            {"000000000010000", 0, 40},
                            {"000000000011111", 1, 8},
                            {"000000000011110", 1, 9},
                            {"000000000011101", 1, 10},
                            {"000000000011100", 1, 11},
                            {"000000000011011", 1, 12},
                            {"000000000011010", 1, 13},
                            {"000000000011001", 1, 14},
                            {"0000000000010011", 1, 15},
                            {"0000000000010010", 1, 16},
                            {"0000000000010001", 1, 17},
                            {"0000000000010000", 1, 18},
                            {"0000000000010100", 6, 3},
                            {"0000000000011010", 11, 2},
                            {"0000000000011001", 12, 2},
                            {"0000000000011000", 13, 2},
                            {"0000000000010111", 14, 2},
                            {"0000000000010110", 15, 2},
                            {"0000000000010101", 16, 2},
                            {"0000000000011111", 27, 1},
                            {"0000000000011110", 28, 1},
                            {"0000000000011101", 29, 1},
                            {"0000000000011100", 30, 1},
                            {"0000000000011011", 31, 1}});
    return t;
}

// B.15 (intra_vlc_format = 1, intra blocks only).
const Vlc& tableDct1() {
    static const Vlc t(16, {{"0110", kEob, 0},
                            {"10", 0, 1},
                            {"010", 1, 1},
                            {"110", 0, 2},
                            {"00101", 2, 1},
                            {"0111", 0, 3},
                            {"00111", 3, 1},
                            {"000110", 4, 1},
                            {"00110", 1, 2},
                            {"000111", 5, 1},
                            {"0000110", 6, 1},
                            {"0000100", 7, 1},
                            {"11100", 0, 4},
                            {"0000111", 2, 2},
                            {"0000101", 8, 1},
                            {"1111000", 9, 1},
                            {"000001", kEsc, 0},
                            {"11101", 0, 5},
                            {"000101", 0, 6},
                            {"1111001", 1, 3},
                            {"00100110", 3, 2},
                            {"1111010", 10, 1},
                            {"00100001", 11, 1},
                            {"00100101", 12, 1},
                            {"00100100", 13, 1},
                            {"000100", 0, 7},
                            {"00100111", 1, 4},
                            {"11111100", 2, 3},
                            {"11111101", 4, 2},
                            {"000000100", 5, 2},
                            {"000000101", 14, 1},
                            {"000000111", 15, 1},
                            {"0000001101", 16, 1},
                            {"1111011", 0, 8},
                            {"1111100", 0, 9},
                            {"00100011", 0, 10},
                            {"00100010", 0, 11},
                            {"00100000", 1, 5},
                            {"0000001100", 2, 4},
                            {"000000011100", 3, 3},
                            {"000000010010", 4, 3},
                            {"000000011110", 6, 2},
                            {"000000010101", 7, 2},
                            {"000000010001", 8, 2},
                            {"000000011111", 17, 1},
                            {"000000011010", 18, 1},
                            {"000000011001", 19, 1},
                            {"000000010111", 20, 1},
                            {"000000010110", 21, 1},
                            {"11111010", 0, 12},
                            {"11111011", 0, 13},
                            {"11111110", 0, 14},
                            {"11111111", 0, 15},
                            {"0000000010110", 1, 6},
                            {"0000000010101", 1, 7},
                            {"0000000010100", 2, 5},
                            {"0000000010011", 3, 4},
                            {"0000000010010", 5, 3},
                            {"0000000010001", 9, 2},
                            {"0000000010000", 10, 2},
                            {"0000000011111", 22, 1},
                            {"0000000011110", 23, 1},
                            {"0000000011101", 24, 1},
                            {"0000000011100", 25, 1},
                            {"0000000011011", 26, 1},
                            {"00000000011111", 0, 16},
                            {"00000000011110", 0, 17},
                            {"00000000011101", 0, 18},
                            {"00000000011100", 0, 19},
                            {"00000000011011", 0, 20},
                            {"00000000011010", 0, 21},
                            {"00000000011001", 0, 22},
                            {"00000000011000", 0, 23},
                            {"00000000010111", 0, 24},
                            {"00000000010110", 0, 25},
                            {"00000000010101", 0, 26},
                            {"00000000010100", 0, 27},
                            {"00000000010011", 0, 28},
                            {"00000000010010", 0, 29},
                            {"00000000010001", 0, 30},
                            {"00000000010000", 0, 31},
                            {"000000000011000", 0, 32},
                            {"000000000010111", 0, 33},
                            {"000000000010110", 0, 34},
                            {"000000000010101", 0, 35},
                            {"000000000010100", 0, 36},
                            {"000000000010011", 0, 37},
                            {"000000000010010", 0, 38},
                            {"000000000010001", 0, 39},
                            {"000000000010000", 0, 40},
                            {"000000000011111", 1, 8},
                            {"000000000011110", 1, 9},
                            {"000000000011101", 1, 10},
                            {"000000000011100", 1, 11},
                            {"000000000011011", 1, 12},
                            {"000000000011010", 1, 13},
                            {"000000000011001", 1, 14},
                            {"0000000000010011", 1, 15},
                            {"0000000000010010", 1, 16},
                            {"0000000000010001", 1, 17},
                            {"0000000000010000", 1, 18},
                            {"0000000000010100", 6, 3},
                            {"0000000000011010", 11, 2},
                            {"0000000000011001", 12, 2},
                            {"0000000000011000", 13, 2},
                            {"0000000000010111", 14, 2},
                            {"0000000000010110", 15, 2},
                            {"0000000000010101", 16, 2},
                            {"0000000000011111", 27, 1},
                            {"0000000000011110", 28, 1},
                            {"0000000000011101", 29, 1},
                            {"0000000000011100", 30, 1},
                            {"0000000000011011", 31, 1}});
    return t;
}

constexpr uint8_t kZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                                 41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                                 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
constexpr uint8_t kAltScan[64] = {0,  8,  16, 24, 1,  9,  2,  10, 17, 25, 32, 40, 48, 56, 57, 49, 41, 33, 26, 18, 3,  11,
                                  4,  12, 19, 27, 34, 42, 50, 58, 35, 43, 51, 59, 20, 28, 5,  13, 6,  14, 21, 29, 36, 44,
                                  52, 60, 37, 45, 53, 61, 22, 30, 7,  15, 23, 31, 38, 46, 54, 62, 39, 47, 55, 63};
constexpr uint8_t kNonLinearQ[32] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  10, 12, 14, 16, 18,  20,  22,
                                     24, 28, 32, 36, 40, 44, 48, 52, 56, 64, 72, 80, 88, 96, 104, 112};
constexpr uint8_t kDefaultIntra[64] = {8,  16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37, 19, 22, 26, 27, 29, 34,
                                       34, 38, 22, 22, 26, 27, 29, 34, 37, 40, 22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32,
                                       35, 40, 48, 58, 26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83};
constexpr int kDither[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};

void idct8x8(const int16_t in[64], int16_t out[64]) {
    static double cosTab[8][8];
    static bool init = false;
    if (!init) {
        for (int x = 0; x < 8; ++x)
            for (int u = 0; u < 8; ++u) cosTab[x][u] = (u == 0 ? std::sqrt(0.125) : 0.5) * std::cos((2 * x + 1) * u * 3.14159265358979323846 / 16.0);
        init = true;
    }
    double tmp[64];
    for (int y = 0; y < 8; ++y) // rows: transform over u
        for (int x = 0; x < 8; ++x) {
            double s = 0;
            for (int u = 0; u < 8; ++u) s += cosTab[x][u] * in[y * 8 + u];
            tmp[y * 8 + x] = s;
        }
    for (int x = 0; x < 8; ++x)
        for (int y = 0; y < 8; ++y) {
            double s = 0;
            for (int v = 0; v < 8; ++v) s += cosTab[y][v] * tmp[v * 8 + x];
            const int r = (int)std::floor(s + 0.5);
            out[y * 8 + x] = (int16_t)std::clamp(r, -256, 255);
        }
}

} // namespace

// ------------------------------------------------------------------ bit reader with retry

// The reader walks the virtual stream spent_ ++ in_.  spent_ holds the
// qwords the command in progress has already consumed from the hardware
// FIFO (so IPU_CTRL.IFC / IPU_BP.FP stay exact - libmpeg rewinds DMA
// channel 4 by that count when it suspends decoding), which lets a command
// that runs out of data restart from its first bit once more arrives.
class IpuReader {
public:
    explicit IpuReader(Ipu& ipu) : ipu_(ipu), bp_(ipu.cmdBp_) {}
    uint32_t available() const { return (uint32_t)(ipu_.spent_.size() + ipu_.in_.size() - qi_) * 128 - bp_; }
    void ensure(uint32_t n) {
        while (available() < n)
            if (!ipu_.pullInput()) throw Ipu::NeedData{};
    }
    // Peek without throwing: missing bits read as zero.
    uint32_t peekSoft(int n) {
        while (available() < (uint32_t)n && ipu_.pullInput()) {
        }
        return peekRaw(n);
    }
    uint32_t peek(int n) {
        ensure((uint32_t)n);
        return peekRaw(n);
    }
    uint32_t get(int n) {
        const uint32_t v = peek(n);
        skip(n);
        return v;
    }
    void skip(int n) {
        ensure((uint32_t)n); // the hardware stalls until the bits are there
        bp_ += (uint32_t)n;
        while (bp_ >= 128) {
            bp_ -= 128;
            if (qi_ >= ipu_.spent_.size() && !ipu_.in_.empty()) {
                // first pass over this qword: the hardware FIFO releases it
                ipu_.spent_.push_back(ipu_.in_.front());
                ipu_.in_.pop_front();
                while (ipu_.pullInput()) {
                }
            }
            ++qi_;
        }
        if (qi_ >= ipu_.spent_.size()) ipu_.bp_ = bp_;
    }
    int32_t getSigned(int n) {
        const uint32_t v = get(n);
        return (int32_t)(v << (32 - n)) >> (32 - n);
    }
    const VlcEntry& vlc(const Vlc& t) {
        const VlcEntry& e = t.lookup(peekSoft(t.maxLen()));
        if (!e.len) {
            // An invalid code is only an error once enough bits are buffered.
            if (available() < (uint32_t)t.maxLen()) throw Ipu::NeedData{};
            return e;
        }
        ensure(e.len);
        skip(e.len);
        return e;
    }
    // Make the progress so far permanent (the command restarts here).
    void commit() {
        const size_t drop = std::min(qi_, ipu_.spent_.size());
        ipu_.spent_.erase(ipu_.spent_.begin(), ipu_.spent_.begin() + (ptrdiff_t)drop);
        qi_ -= drop;
        ipu_.cmdBp_ = bp_;
        if (ipu_.spent_.empty()) ipu_.bp_ = bp_;
    }
    uint32_t position() const { return (uint32_t)qi_ * 128 + bp_; }

private:
    uint8_t byteAt(size_t q, uint32_t i) const {
        const size_t ns = ipu_.spent_.size();
        if (q < ns) return ipu_.spent_[q].b[i];
        q -= ns;
        return q < ipu_.in_.size() ? ipu_.in_[q].b[i] : 0;
    }
    uint32_t peekRaw(int n) const {
        if (n == 0) return 0;
        uint64_t acc = 0;
        for (int k = 0; k < 8; ++k) {
            const uint32_t byteIdx = bp_ / 8 + (uint32_t)k;
            acc = (acc << 8) | byteAt(qi_ + byteIdx / 16, byteIdx % 16);
        }
        return (uint32_t)((acc << (bp_ % 8)) >> (64 - n));
    }
    Ipu& ipu_;
    size_t qi_ = 0;
    uint32_t bp_;
};

// ------------------------------------------------------------------ Ipu

Ipu::Ipu(Machine& m) : m_(m) {
    reset();
    for (int i = 0; i < 64; ++i) {
        iqIntra_[(size_t)i] = kDefaultIntra[i];
        iqNonIntra_[(size_t)i] = 16;
    }
}

Ipu::~Ipu() = default;

void Ipu::reset() {
    in_.clear();
    spent_.clear();
    out_.clear();
    outPartialBytes_ = 0;
    bp_ = cmdBp_ = 0;
    busy_ = false;
    ecd_ = scd_ = false;
    cmd_ = 0;
    data_ = 0;
    mbRemaining_ = 0;
    dcPred_[0] = dcPred_[1] = dcPred_[2] = 128;
}

uint32_t Ipu::bitsAvailable() const { return (uint32_t)in_.size() * 128 - bp_; }

uint32_t Ipu::ctrlValue() const {
    const uint32_t n = (uint32_t)in_.size();
    const uint32_t fp = std::min<uint32_t>(n, 2);
    const uint32_t ifc = std::min<uint32_t>(n - fp, 8);
    const uint32_t ofc = std::min<uint32_t>((uint32_t)out_.size(), 8);
    return ifc | (ofc << 4) | ((cbp_ & 0x3F) << 8) | (ecd_ ? 1u << 14 : 0) | (scd_ ? 1u << 15 : 0) | (ctrl_ & 0x07F30000u) |
           (busy_ ? 1u << 31 : 0);
}

uint64_t Ipu::read64(uint32_t phys) {
    if (ipuTrace() && busy_) {
        static uint64_t n = 0;
        if ((++n % 200000) == 0) {
            const DmaRegs& d4 = m_.dmac->ch[DMA_TO_IPU];
            std::fprintf(stderr, "ipu: stalled cmd %08x bits %u in %zu spent %zu out %zu d3 %08x/%u d4 chcr %08x qwc %u madr %08x\n", cmd_, bitsAvailable(),
                         in_.size(), spent_.size(), out_.size(), m_.dmac->ch[DMA_FROM_IPU].chcr, m_.dmac->ch[DMA_FROM_IPU].qwc, d4.chcr, d4.qwc, d4.madr);
        }
    }
    switch (phys & 0xF0) {
    case 0x00: // IPU_CMD
        return (uint64_t)data_ | (busy_ ? (1ull << 63) : 0);
    case 0x10: // IPU_CTRL
        return ctrlValue();
    case 0x20: { // IPU_BP
        const uint32_t n = (uint32_t)in_.size();
        const uint32_t fp = std::min<uint32_t>(n, 2);
        const uint32_t ifc = std::min<uint32_t>(n - fp, 8);
        return (bp_ & 0x7F) | (ifc << 8) | (fp << 16);
    }
    case 0x30: { // IPU_TOP: next 32 bits of the bitstream
        IpuReader r(*this);
        const uint32_t top = r.peekSoft(32);
        return (uint64_t)top | (busy_ ? (1ull << 63) : 0);
    }
    default: return 0;
    }
}

void Ipu::write64(uint32_t phys, uint64_t v64) {
    const uint32_t v = (uint32_t)v64;
    switch (phys & 0xF0) {
    case 0x00: { // IPU_CMD
        cmd_ = v;
        const uint32_t op = v >> 28;
        ++commands;
        if (op == 0) { // BCLR
            in_.clear();
            spent_.clear();
            bp_ = cmdBp_ = v & 0x7F;
            ecd_ = scd_ = false;
            busy_ = false;
            // DMA may refill right away.
            while (in_.size() < 10 && pullInput()) {
            }
            return;
        }
        if (op == 9) { // SETTH
            th0_ = v & 0x1FF;
            th1_ = (v >> 16) & 0x1FF;
            busy_ = false;
            return;
        }
        busy_ = true;
        spent_.clear();
        cmdBp_ = bp_;
        if (op == 7 || op == 8) mbRemaining_ = v & 0x7FF;
        if (op == 1) mbRemaining_ = 0;
        tryRun();
        return;
    }
    case 0x10: // IPU_CTRL
        if (v & (1u << 30)) {
            reset();
            ctrl_ = 0;
            return;
        }
        ctrl_ = v & 0x07F30000u;
        return;
    default: return;
    }
}

void Ipu::readFifo(Reg128* out) {
    drainOut();
    if (out_.empty()) {
        std::memset(out, 0, 16);
        return;
    }
    std::memcpy(out, out_.front().b, 16);
    out_.pop_front();
    tryRun();
}

void Ipu::writeFifo(const Reg128* in) {
    Qw q;
    std::memcpy(q.b, in, 16);
    in_.push_back(q);
    tryRun();
}

bool Ipu::pullInput() {
    if (in_.size() >= 10) return false; // 8-qword FIFO + 2 qwords in the decoder
    DmaRegs& r = m_.dmac->ch[DMA_TO_IPU];
    if (!(r.chcr & 0x100)) return false;
    const uint32_t mode = (r.chcr >> 2) & 3;
    while (r.qwc == 0) {
        if (mode != 1 || chainEnd4_) {
            m_.dmac->complete(DMA_TO_IPU);
            chainEnd4_ = false;
            return false;
        }
        // Source chain: fetch the next tag.
        Reg128 t;
        if (r.tadr & 0x80000000u) std::memcpy(&t, m_.spr + (r.tadr & (SPR_SIZE - 1)), 16);
        else m_.read128(r.tadr, &t);
        const uint32_t qwc = t.u32[0] & 0xFFFF, id = (t.u32[0] >> 28) & 7, addr = t.u32[1];
        r.chcr = (r.chcr & 0xFFFF) | (t.u32[0] & 0xFFFF0000u);
        switch (id) {
        case 0: r.madr = addr; r.qwc = qwc; r.tadr += 16; chainEnd4_ = true; break;          // refe
        case 1: r.madr = r.tadr + 16; r.qwc = qwc; r.tadr = r.madr + qwc * 16; break;     // cnt
        case 2: r.madr = r.tadr + 16; r.qwc = qwc; r.tadr = addr; break;                  // next
        case 3: case 4: r.madr = addr; r.qwc = qwc; r.tadr += 16; break;                  // ref / refs
        default: r.madr = r.tadr + 16; r.qwc = qwc; chainEnd4_ = true; break;             // end
        }
        if ((t.u32[0] >> 31) && (r.chcr & 0x80)) chainEnd4_ = true; // IRQ + TIE
        if (r.qwc == 0 && chainEnd4_) continue;
    }
    Qw q;
    if (r.madr & 0x80000000u) std::memcpy(q.b, m_.spr + (r.madr & (SPR_SIZE - 1) & ~15u), 16);
    else {
        Reg128 v;
        m_.read128(r.madr, &v);
        std::memcpy(q.b, &v, 16);
    }
    r.madr += 16;
    r.qwc--;
    in_.push_back(q);
    if (r.qwc == 0 && (mode != 1 || chainEnd4_)) {
        m_.dmac->complete(DMA_TO_IPU);
        chainEnd4_ = false;
    }
    return true;
}

void Ipu::dmaInKick() {
    chainEnd4_ = false;
    while (in_.size() < 10 && pullInput()) {
    }
    tryRun();
}

void Ipu::dmaOutKick() {
    drainOut();
    tryRun();
}

void Ipu::pushOut(const uint8_t* data, size_t bytes) {
    while (bytes) {
        const size_t k = std::min<size_t>(bytes, 16 - outPartialBytes_);
        std::memcpy(outPartial_.data() + outPartialBytes_, data, k);
        outPartialBytes_ += (uint32_t)k;
        data += k;
        bytes -= k;
        if (outPartialBytes_ == 16) {
            Qw q;
            std::memcpy(q.b, outPartial_.data(), 16);
            out_.push_back(q);
            outPartialBytes_ = 0;
        }
    }
}

void Ipu::drainOut() {
    DmaRegs& r = m_.dmac->ch[DMA_FROM_IPU];
    while ((r.chcr & 0x100) && r.qwc > 0 && !out_.empty()) {
        const Qw& q = out_.front();
        if (r.madr & 0x80000000u) std::memcpy(m_.spr + (r.madr & (SPR_SIZE - 1) & ~15u), q.b, 16);
        else {
            Reg128 v;
            std::memcpy(&v, q.b, 16);
            m_.write128(r.madr, &v);
        }
        out_.pop_front();
        r.madr += 16;
        if (--r.qwc == 0) m_.dmac->complete(DMA_FROM_IPU);
    }
}

void Ipu::tryRun() {
    // Keep the FIFO topped up like the hardware DMA does.
    while (in_.size() < 10 && pullInput()) {
    }
    if (!busy_) {
        drainOut();
        return;
    }
    const uint32_t op = cmd_ >> 28;
    bool done = false;
    try {
        IpuReader r(*this);
        switch (op) {
        case 1: done = cmdIdec(r); break;
        case 2: done = cmdBdec(r); break;
        case 3: done = cmdVdec(r); break;
        case 4: done = cmdFdec(r); break;
        case 5: done = cmdSetIq(r); break;
        case 6: done = cmdSetVq(r); break;
        case 7: done = cmdCsc(r); break;
        case 8: done = cmdPack(r); break;
        default: done = true; break;
        }
    } catch (const NeedData&) {
        done = false;
    }
    drainOut();
    if (done && ipuTrace())
        std::fprintf(stderr, "ipu: cmd %08x -> data %08x ecd %d scd %d bits %u out %zu\n", cmd_, data_, ecd_, scd_, bitsAvailable(),
                     out_.size());
    if (done) {
        busy_ = false;
        if (op == 1 || op == 2 || op == 7 || op == 8) m_.intc->raise(INTC_IPU);
        while (in_.size() < 10 && pullInput()) {
        }
    }
}

// ------------------------------------------------------------------ commands

bool Ipu::cmdFdec(IpuReader& r) {
    r.skip((int)(cmd_ & 0x3F));
    data_ = r.peek(32);
    r.commit();
    return true;
}

bool Ipu::cmdVdec(IpuReader& r) {
    r.skip((int)(cmd_ & 0x3F));
    const int type = (cmd_ >> 26) & 3;
    const int pct = (ctrl_ >> 24) & 7;
    const Vlc* t = nullptr;
    switch (type) {
    case 0: t = &tableMbai(); break;
    case 1: t = &tableMbType(pct); break;
    case 2: t = &tableMotion(); break;
    default: t = &tableDmv(); break;
    }
    const uint32_t startPos = r.position();
    const VlcEntry& e = r.vlc(*t);
    if (!e.len) {
        data_ = 0;
        ecd_ = true;
        r.commit();
        return true;
    }
    int value = e.a;
    if (type == 2 && value != 0 && r.get(1)) value = -value;
    const uint32_t len = r.position() - startPos;
    data_ = (len << 16) | ((uint32_t)value & 0xFFFF);
    r.commit();
    return true;
}

bool Ipu::cmdSetIq(IpuReader& r) {
    std::array<uint8_t, 64>& m = (cmd_ & (1u << 27)) ? iqNonIntra_ : iqIntra_;
    uint8_t tmp[64];
    for (int i = 0; i < 64; ++i) tmp[i] = (uint8_t)r.get(8);
    for (int i = 0; i < 64; ++i) m[kZigzag[i]] = tmp[i];
    r.commit();
    return true;
}

bool Ipu::cmdSetVq(IpuReader& r) {
    uint16_t tmp[16];
    for (int i = 0; i < 16; ++i) {
        const uint32_t lo = r.get(8), hi = r.get(8);
        tmp[i] = (uint16_t)(lo | (hi << 8));
    }
    std::memcpy(vqClut_.data(), tmp, sizeof tmp);
    r.commit();
    return true;
}

bool Ipu::decodeBlock(IpuReader& r, int comp, bool intra, int16_t out[64]) {
    const bool mpeg1 = (ctrl_ >> 23) & 1;
    const int idp = (ctrl_ >> 16) & 3;
    const bool altScan = (ctrl_ >> 20) & 1;
    const bool ivf = (ctrl_ >> 21) & 1;
    const bool qst = (ctrl_ >> 22) & 1;
    const int qsc = (int)((cmd_ >> 16) & 0x1F);
    const int qs = mpeg1 ? qsc : (qst ? kNonLinearQ[qsc] : qsc * 2);
    const uint8_t* scan = altScan ? kAltScan : kZigzag;
    const uint8_t* W = intra ? iqIntra_.data() : iqNonIntra_.data();
    int32_t coef[64] = {};
    int i = 0;
    if (intra) {
        const VlcEntry& e = r.vlc(comp == 0 ? tableDcLuma() : tableDcChroma());
        if (!e.len) return false;
        const int size = e.a;
        int diff = 0;
        if (size) {
            const uint32_t v = r.get(size);
            diff = (v & (1u << (size - 1))) ? (int)v : (int)v - (1 << size) + 1;
        }
        dcPred_[comp] += diff;
        coef[0] = mpeg1 ? dcPred_[comp] * 8 : dcPred_[comp] << (3 - idp);
        i = 0;
    } else {
        i = -1;
    }
    const Vlc& table = (intra && ivf && !mpeg1) ? tableDct1() : tableDct0();
    bool first = !intra;
    for (;;) {
        int run, level;
        if (first && r.peekSoft(1) == 1) {
            // First coefficient of a non-intra block: '1s' = (0, +-1).
            r.ensure(2);
            r.skip(1);
            run = 0;
            level = r.get(1) ? -1 : 1;
        } else {
            const VlcEntry& e = r.vlc(table);
            if (!e.len) return false;
            if (e.a == kEob) break;
            if (e.a == kEsc) {
                run = (int)r.get(6);
                if (mpeg1) {
                    int l = r.getSigned(8);
                    if (l == 0) l = (int)r.get(8);
                    else if (l == -128) l = (int)r.get(8) - 256;
                    level = l;
                } else {
                    level = r.getSigned(12);
                }
            } else {
                run = e.a;
                level = e.b;
                if (r.get(1)) level = -level;
            }
        }
        first = false;
        i += run + 1;
        if (i > 63) return false;
        const int pos = scan[i];
        int32_t v;
        const int sgn = level > 0 ? 1 : (level < 0 ? -1 : 0);
        if (mpeg1) {
            if (intra) v = (level * qs * W[pos]) / 8;
            else v = ((2 * level + sgn) * qs * W[pos]) / 16;
            if (v && !(v & 1)) v -= sgn; // oddification
        } else {
            if (intra) v = (level * W[pos] * qs) / 16;
            else v = ((2 * level + sgn) * W[pos] * qs) / 32;
        }
        coef[pos] = std::clamp(v, -2048, 2047);
    }
    if (!mpeg1) {
        int32_t sum = 0;
        for (int k = 0; k < 64; ++k) sum += coef[k];
        if (!(sum & 1)) coef[63] ^= 1;
    }
    int16_t in16[64];
    for (int k = 0; k < 64; ++k) in16[k] = (int16_t)std::clamp(coef[k], -2048, 2047);
    idct8x8(in16, out);
    return true;
}

bool Ipu::cmdBdec(IpuReader& r) {
    const int savedPred[3] = {dcPred_[0], dcPred_[1], dcPred_[2]};
    r.skip((int)(cmd_ & 0x3F));
    const bool intra = (cmd_ >> 27) & 1;
    const bool dcr = (cmd_ >> 26) & 1;
    const bool fieldDct = (cmd_ >> 25) & 1;
    const int idp = (ctrl_ >> 16) & 3;
    try {
        if (dcr) dcPred_[0] = dcPred_[1] = dcPred_[2] = 128 << idp;
        int cbp = 0x3F;
        if (!intra) {
            const VlcEntry& e = r.vlc(tableCbp());
            if (!e.len) {
                ecd_ = true;
                r.commit();
                return true;
            }
            cbp = e.a;
        }
        int16_t blocks[6][64] = {};
        for (int b = 0; b < 6; ++b) {
            if (!(cbp & (32 >> b))) continue;
            if (!decodeBlock(r, b < 4 ? 0 : b - 3, intra, blocks[b])) {
                ecd_ = true;
                r.commit();
                return true;
            }
        }
        // RAW16 macroblock: Y 16x16 raster, Cb 8x8, Cr 8x8.
        int16_t mb[384];
        for (int b = 0; b < 4; ++b) {
            const int bx = (b & 1) * 8;
            for (int y = 0; y < 8; ++y) {
                const int row = fieldDct ? ((b >> 1) + y * 2) : ((b >> 1) * 8 + y);
                for (int x = 0; x < 8; ++x) mb[row * 16 + bx + x] = blocks[b][y * 8 + x];
            }
        }
        std::memcpy(mb + 256, blocks[4], 128);
        std::memcpy(mb + 320, blocks[5], 128);
        cbp_ = (uint32_t)cbp;
        r.commit();
        pushOut((const uint8_t*)mb, sizeof mb);
        ++macroblocks;
        return true;
    } catch (const NeedData&) {
        dcPred_[0] = savedPred[0];
        dcPred_[1] = savedPred[1];
        dcPred_[2] = savedPred[2];
        throw;
    }
}

void Ipu::csc(const uint8_t* mb, uint8_t* out, bool rgb16, bool dither) {
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const int Y = mb[y * 16 + x];
            const int cb = mb[256 + (y >> 1) * 8 + (x >> 1)] - 128;
            const int cr = mb[320 + (y >> 1) * 8 + (x >> 1)] - 128;
            const int lum = (Y - 16) * 149;
            int r = (lum + cr * 204 + 64) >> 7;
            int g = (lum - cb * 50 - cr * 104 + 64) >> 7;
            int b = (lum + cb * 258 + 64) >> 7;
            r = std::clamp(r, 0, 255);
            g = std::clamp(g, 0, 255);
            b = std::clamp(b, 0, 255);
            int a;
            if (r < (int)th0_ && g < (int)th0_ && b < (int)th0_) {
                r = g = b = 0;
                a = 0;
            } else if (r < (int)th1_ && g < (int)th1_ && b < (int)th1_) {
                a = 0x40;
            } else {
                a = 0x80;
            }
            if (!rgb16) {
                uint8_t* p = out + (y * 16 + x) * 4;
                p[0] = (uint8_t)r;
                p[1] = (uint8_t)g;
                p[2] = (uint8_t)b;
                p[3] = (uint8_t)a;
            } else {
                if (dither) {
                    const int d = kDither[y & 3][x & 3];
                    r = std::clamp(r + d, 0, 255);
                    g = std::clamp(g + d, 0, 255);
                    b = std::clamp(b + d, 0, 255);
                }
                const uint16_t px = (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | (a ? 0x8000 : 0));
                std::memcpy(out + (y * 16 + x) * 2, &px, 2);
            }
        }
    }
}

bool Ipu::cmdCsc(IpuReader& r) {
    const bool rgb16 = (cmd_ >> 27) & 1;
    const bool dither = (cmd_ >> 26) & 1;
    while (mbRemaining_ > 0) {
        uint8_t mb[384];
        for (int i = 0; i < 384; ++i) mb[i] = (uint8_t)r.get(8);
        r.commit();
        uint8_t outb[1024];
        csc(mb, outb, rgb16, dither);
        pushOut(outb, rgb16 ? 512 : 1024);
        --mbRemaining_;
        drainOut();
    }
    return true;
}

bool Ipu::cmdPack(IpuReader& r) {
    const bool rgb16 = (cmd_ >> 27) & 1; // OFM: 1 = RGB16, 0 = INDX4
    const bool dither = (cmd_ >> 26) & 1;
    while (mbRemaining_ > 0) {
        uint8_t in[1024];
        for (int i = 0; i < 1024; ++i) in[i] = (uint8_t)r.get(8);
        r.commit();
        if (rgb16) {
            uint8_t outb[512];
            for (int p = 0; p < 256; ++p) {
                int R = in[p * 4], G = in[p * 4 + 1], B = in[p * 4 + 2];
                if (dither) {
                    const int d = kDither[(p >> 4) & 3][p & 3];
                    R = std::clamp(R + d, 0, 255);
                    G = std::clamp(G + d, 0, 255);
                    B = std::clamp(B + d, 0, 255);
                }
                const uint16_t px = (uint16_t)((R >> 3) | ((G >> 3) << 5) | ((B >> 3) << 10) | (in[p * 4 + 3] ? 0x8000 : 0));
                std::memcpy(outb + p * 2, &px, 2);
            }
            pushOut(outb, 512);
        } else {
            uint8_t outb[128] = {};
            for (int p = 0; p < 256; ++p) {
                const int R = in[p * 4] >> 3, G = in[p * 4 + 1] >> 3, B = in[p * 4 + 2] >> 3;
                int best = 0, bestD = 1 << 30;
                for (int k = 0; k < 16; ++k) {
                    const uint16_t c = vqClut_[(size_t)k];
                    const int dr = R - (c & 31), dg = G - ((c >> 5) & 31), db = B - ((c >> 10) & 31);
                    const int d = dr * dr + dg * dg + db * db;
                    if (d < bestD) {
                        bestD = d;
                        best = k;
                    }
                }
                outb[p >> 1] |= (uint8_t)(best << ((p & 1) * 4));
            }
            pushOut(outb, 128);
        }
        --mbRemaining_;
        drainOut();
    }
    return true;
}

bool Ipu::cmdIdec(IpuReader& r) {
    // Intra decode of a whole slice: per macroblock MBAI, macroblock_type
    // (I table), optional quantiser, optional DCT type, 6 intra blocks and
    // CSC to RGB32/RGB16.  Ends at the next start code.
    const bool dtd = (cmd_ >> 24) & 1;
    const bool sgn = (cmd_ >> 25) & 1;
    const bool dither = (cmd_ >> 26) & 1;
    const bool rgb16 = (cmd_ >> 27) & 1;
    const int idp = (ctrl_ >> 16) & 3;
    if (mbRemaining_ == 0) { // first attempt of this command
        r.skip((int)(cmd_ & 0x3F));
        dcPred_[0] = dcPred_[1] = dcPred_[2] = 128 << idp;
        mbRemaining_ = 1;
        r.commit();
    }
    for (;;) {
        if (r.peekSoft(23) == 0) { // start code: end of slice
            scd_ = true;
            r.commit();
            mbRemaining_ = 0;
            return true;
        }
        const int saved[3] = {dcPred_[0], dcPred_[1], dcPred_[2]};
        const uint32_t savedCmd = cmd_;
        try {
            const VlcEntry& ai = r.vlc(tableMbai());
            if (!ai.len) {
                ecd_ = true;
                r.commit();
                mbRemaining_ = 0;
                return true;
            }
            const VlcEntry& mt = r.vlc(tableMbType(1));
            if (!mt.len) {
                ecd_ = true;
                r.commit();
                mbRemaining_ = 0;
                return true;
            }
            if (mt.a & 0x10) cmd_ = (cmd_ & ~(0x1Fu << 16)) | (r.get(5) << 16);
            const bool fieldDct = dtd ? r.get(1) != 0 : false;
            int16_t blocks[6][64];
            for (int b = 0; b < 6; ++b)
                if (!decodeBlock(r, b < 4 ? 0 : b - 3, true, blocks[b])) {
                    ecd_ = true;
                    r.commit();
                    mbRemaining_ = 0;
                    return true;
                }
            uint8_t mb[384];
            for (int b = 0; b < 4; ++b) {
                const int bx = (b & 1) * 8;
                for (int y = 0; y < 8; ++y) {
                    const int row = fieldDct ? ((b >> 1) + y * 2) : ((b >> 1) * 8 + y);
                    for (int x = 0; x < 8; ++x)
                        mb[row * 16 + bx + x] = (uint8_t)std::clamp(blocks[b][y * 8 + x] + (sgn ? 128 : 0), 0, 255);
                }
            }
            for (int k = 0; k < 64; ++k) {
                mb[256 + k] = (uint8_t)std::clamp((int)blocks[4][k] + (sgn ? 128 : 0), 0, 255);
                mb[320 + k] = (uint8_t)std::clamp((int)blocks[5][k] + (sgn ? 128 : 0), 0, 255);
            }
            r.commit();
            uint8_t outb[1024];
            csc(mb, outb, rgb16, dither);
            pushOut(outb, rgb16 ? 512 : 1024);
            ++macroblocks;
            drainOut();
        } catch (const NeedData&) {
            dcPred_[0] = saved[0];
            dcPred_[1] = saved[1];
            dcPred_[2] = saved[2];
            cmd_ = savedCmd;
            throw;
        }
    }
}

} // namespace ps2
