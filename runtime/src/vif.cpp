#include "ps2/vif.hpp"

#include <algorithm>
#include <cstring>

#include "ps2/gs.hpp"
#include "ps2/hw.hpp"
#include "ps2/log.hpp"
#include "ps2/machine.hpp"
#include "ps2/vu.hpp"

namespace ps2 {

Vif::Vif(Machine& m) : m_(m) {
    v_[0].index = 0;
    v_[1].index = 1;
    for (auto& s : v_) s.cycle = 0x0101; // CL=1, WL=1
}

uint32_t Vif::readReg(int vif, uint32_t off) {
    VifState& s = v_[vif & 1];
    switch (off & 0x1F0) {
    case 0x000: return s.stat;
    case 0x010: return s.fbrst;
    case 0x020: return s.err;
    case 0x030: return s.mark;
    case 0x040: return s.cycle;
    case 0x050: return s.mode;
    case 0x060: return s.num;
    case 0x070: return s.mask;
    case 0x080: return s.code;
    case 0x090: return s.itops;
    case 0x0A0: return s.base;
    case 0x0B0: return s.ofst;
    case 0x0C0: return s.tops;
    case 0x0D0: return s.itop;
    case 0x0E0: return s.top;
    case 0x100: case 0x110: case 0x120: case 0x130: return s.row[((off & 0x1F0) - 0x100) >> 4];
    case 0x140: case 0x150: case 0x160: case 0x170: return s.col[((off & 0x1F0) - 0x140) >> 4];
    default: return 0;
    }
}

void Vif::writeReg(int vif, uint32_t off, uint32_t v) {
    VifState& s = v_[vif & 1];
    switch (off & 0x1F0) {
    case 0x010: // FBRST
        if (v & 1) { // reset
            s.cmd = 0;
            s.wordsLeft = 0;
            s.buf.clear();
            s.stat = 0;
        }
        break;
    case 0x020: s.err = v; break;
    case 0x030: s.mark = v; break;
    case 0x100: case 0x110: case 0x120: case 0x130: s.row[((off & 0x1F0) - 0x100) >> 4] = v; break;
    case 0x140: case 0x150: case 0x160: case 0x170: s.col[((off & 0x1F0) - 0x140) >> 4] = v; break;
    default: break;
    }
}

static uint32_t unpackDataBits(uint32_t cmd) {
    const uint32_t vn = (cmd >> 26) & 3, vl = (cmd >> 24) & 3;
    if (vl == 3) return 16; // V4-5
    return (32u >> vl) * (vn + 1);
}

void Vif::startCommand(VifState& s, uint32_t code) {
    s.code = code;
    s.cmd = code;
    s.buf.clear();
    s.commands++;
    const uint32_t op = (code >> 24) & 0x7F;
    const uint32_t imm = code & 0xFFFF;
    const uint32_t num = (code >> 16) & 0xFF;
    s.wordsLeft = 0;
    if ((op & 0x60) == 0x60) { // UNPACK
        const uint32_t n = num ? num : 256;
        const uint32_t cl = s.cycle & 0xFF, wl = (s.cycle >> 8) & 0xFF;
        uint32_t elements = n;
        if (wl && cl < wl) elements = (n / wl) * cl + std::min(n % wl, cl);
        const uint32_t bitsTotal = elements * unpackDataBits(code);
        s.wordsLeft = (bitsTotal + 31) / 32;
        s.num = n;
        if (s.wordsLeft == 0) finishCommand(s);
        return;
    }
    switch (op) {
    case 0x00: break;                                   // NOP
    case 0x01: s.cycle = imm; break;                    // STCYCL
    case 0x02: s.ofst = imm & 0x3FF; s.dbf = false; s.stat &= ~0x80u; s.tops = s.base; break; // OFFSET
    case 0x03: s.base = imm & 0x3FF; break;             // BASE
    case 0x04: s.itops = imm & 0x3FF; break;            // ITOP
    case 0x05: s.mode = imm & 3; break;                 // STMOD
    case 0x06: s.maskPath3 = (imm >> 15) & 1; break;    // MSKPATH3
    case 0x07: s.mark = imm; break;                     // MARK
    case 0x10: case 0x11: case 0x13: break;             // FLUSHE/FLUSH/FLUSHA
    case 0x14: mscal(s, imm); break;                    // MSCAL
    case 0x15: mscal(s, imm); break;                    // MSCALF
    case 0x17: mscal(s, 0xFFFFFFFF); break;             // MSCNT
    case 0x20: s.wordsLeft = 1; break;                  // STMASK
    case 0x30: case 0x31: s.wordsLeft = 4; break;       // STROW / STCOL
    case 0x4A: s.wordsLeft = (num ? num : 256) * 2; s.mpgAddr = imm; break; // MPG
    case 0x50: case 0x51: s.wordsLeft = (imm ? imm : 65536) * 4; break;     // DIRECT/DIRECTHL
    default:
        PS2_DEBUG(Vif, "vif%d: unknown command %08x", s.index, code);
        break;
    }
    if (s.wordsLeft == 0) finishCommand(s);
}

void Vif::mscal(VifState& s, uint32_t addr) {
    VuState& vu = m_.vu->state(s.index);
    if (s.index == 1) {
        s.top = s.tops;
        s.itop = s.itops;
        s.dbf = !s.dbf;
        s.tops = s.base + (s.dbf ? s.ofst : 0);
        if (s.dbf) s.stat |= 0x80;
        else s.stat &= ~0x80u;
    } else {
        s.itop = s.itops;
    }
    const uint32_t start = addr == 0xFFFFFFFF ? vu.tpc : addr;
    m_.vu->startMicro(s.index, start);
}

void Vif::finishCommand(VifState& s) {
    const uint32_t code = s.cmd;
    const uint32_t op = (code >> 24) & 0x7F;
    if ((op & 0x60) == 0x60) {
        unpackWords(s);
    } else {
        switch (op) {
        case 0x20: s.mask = s.buf.empty() ? 0 : s.buf[0]; break;
        case 0x30: for (int i = 0; i < 4 && i < (int)s.buf.size(); ++i) s.row[i] = s.buf[i]; break;
        case 0x31: for (int i = 0; i < 4 && i < (int)s.buf.size(); ++i) s.col[i] = s.buf[i]; break;
        case 0x4A: {
            VuState& vu = m_.vu->state(s.index);
            const uint32_t base = (s.mpgAddr * 8) & vu.codeMask;
            for (size_t i = 0; i < s.buf.size(); ++i)
                std::memcpy(vu.code + ((base + i * 4) & vu.codeMask), &s.buf[i], 4);
            break;
        }
        case 0x50:
        case 0x51:
            if (!s.buf.empty())
                m_.gs->gifTransfer(reinterpret_cast<const uint8_t*>(s.buf.data()), s.buf.size() / 4, 2);
            break;
        default: break;
        }
    }
    if (code & 0x80000000u) {
        m_.intc->raise(s.index == 0 ? INTC_VIF0 : INTC_VIF1);
        s.stat |= 0x800; // INT
    }
    s.cmd = 0;
    s.buf.clear();
}

void Vif::unpackWords(VifState& s) {
    const uint32_t code = s.cmd;
    const uint32_t vn = (code >> 26) & 3, vl = (code >> 24) & 3;
    const bool masked = (code >> 28) & 1;
    const bool usn = (code >> 14) & 1;
    const bool flg = (code >> 15) & 1;
    uint32_t addr = code & 0x3FF;
    if (flg && s.index == 1) addr += s.tops;
    VuState& vu = m_.vu->state(s.index);
    const uint32_t qmask = vu.memMask >> 4;
    const uint32_t cl = s.cycle & 0xFF, wl = (s.cycle >> 8) & 0xFF;
    const uint8_t* data = reinterpret_cast<const uint8_t*>(s.buf.data());
    const size_t dataBytes = s.buf.size() * 4;
    size_t bitPos = 0;
    auto readBits = [&](uint32_t nbits) -> uint32_t {
        uint32_t v = 0;
        const size_t byte = bitPos / 8;
        const uint32_t nbytes = nbits / 8;
        for (uint32_t i = 0; i < nbytes && byte + i < dataBytes; ++i) v |= (uint32_t)data[byte + i] << (i * 8);
        bitPos += nbits;
        return v;
    };
    auto ext = [&](uint32_t v, uint32_t nbits) -> uint32_t {
        if (nbits == 32 || usn) return v;
        const uint32_t sh = 32 - nbits;
        return (uint32_t)((int32_t)(v << sh) >> sh);
    };
    const uint32_t n = s.num;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t qaddr;
        uint32_t cycIdx;
        bool fill = false;
        if (wl == 0 || cl >= wl) { // skipping (or normal) write
            const uint32_t w = wl ? wl : 1;
            qaddr = addr + (i / w) * (cl ? cl : 1) + (i % w);
            cycIdx = i % w;
        } else { // filling write
            qaddr = addr + i;
            cycIdx = i % wl;
            fill = cycIdx >= cl;
        }
        uint32_t val[4] = {0, 0, 0, 0};
        if (!fill) {
            if (vl == 3) { // V4-5
                const uint32_t c = readBits(16);
                val[0] = (c & 0x1F) << 3;
                val[1] = ((c >> 5) & 0x1F) << 3;
                val[2] = ((c >> 10) & 0x1F) << 3;
                val[3] = ((c >> 15) & 1) << 7;
            } else {
                const uint32_t eb = 32u >> vl;
                uint32_t comp[4] = {0, 0, 0, 0};
                for (uint32_t k = 0; k <= vn; ++k) comp[k] = ext(readBits(eb), eb);
                switch (vn) {
                case 0: val[0] = val[1] = val[2] = val[3] = comp[0]; break;
                case 1: val[0] = comp[0]; val[1] = comp[1]; val[2] = comp[0]; val[3] = comp[1]; break;
                case 2: val[0] = comp[0]; val[1] = comp[1]; val[2] = comp[2]; val[3] = 0; break;
                case 3: val[0] = comp[0]; val[1] = comp[1]; val[2] = comp[2]; val[3] = comp[3]; break;
                }
            }
        }
        uint8_t* dst = vu.mem + ((qaddr & qmask) * 16);
        uint32_t out[4];
        std::memcpy(out, dst, 16);
        const uint32_t row = std::min<uint32_t>(cycIdx, 3);
        for (int f = 0; f < 4; ++f) {
            uint32_t mctl = masked ? ((s.mask >> ((row * 4 + f) * 2)) & 3) : 0;
            if (fill && mctl == 0) mctl = 1; // filling positions use the row register when not masked
            switch (mctl) {
            case 0: {
                uint32_t d = val[f];
                if (s.mode == 1) d += s.row[f];
                else if (s.mode == 2) {
                    d += s.row[f];
                    s.row[f] = d;
                }
                out[f] = d;
                break;
            }
            case 1: out[f] = s.row[f]; break;
            case 2: out[f] = s.col[row]; break;
            case 3: break; // write protect
            }
        }
        std::memcpy(dst, out, 16);
    }
}

void Vif::feed(int vif, const uint32_t* words, size_t count) {
    VifState& s = v_[vif & 1];
    size_t i = 0;
    while (i < count) {
        if (s.cmd == 0 && s.wordsLeft == 0) {
            startCommand(s, words[i++]);
            continue;
        }
        const size_t take = std::min<size_t>(s.wordsLeft, count - i);
        s.buf.insert(s.buf.end(), words + i, words + i + take);
        i += take;
        s.wordsLeft -= (uint32_t)take;
        if (s.wordsLeft == 0) finishCommand(s);
    }
}

} // namespace ps2
