#include "ps2/gs.hpp"

#include <algorithm>
#include <cstring>

#include "gs_raster.hpp"
#include "ps2/gs_mem.hpp"
#include "ps2/hw.hpp"
#include "ps2/kernel.hpp"
#include "ps2/log.hpp"
#include "ps2/machine.hpp"
#include "ps2/vif.hpp"

namespace ps2 {

using namespace gsmem;

namespace {
constexpr int64_t kFieldCycles = 4920125;   // EE cycles per NTSC field (59.94 Hz)
constexpr int64_t kVblankCycles = 22 * 18743; // ~22 lines of vertical blank

inline uint32_t bits(uint64_t v, int lo, int n) { return (uint32_t)((v >> lo) & ((1ull << n) - 1)); }
} // namespace

Gs::Gs(Machine& m) : m_(m), vram_(VRAM_SIZE, 0) {
    raster_ = std::make_unique<GsRasterizer>(*this);
    csr_ = 0;
}

Gs::~Gs() = default;

// ------------------------------------------------------------------ pixels

uint32_t Gs::readPixel(uint32_t bp, uint32_t bw, uint32_t psm, int x, int y) const {
    const uint8_t* vm = vram_.data();
    switch (psm) {
    case PSMCT32:
    case PSMZ32: {
        uint32_t v;
        std::memcpy(&v, vm + addr32(bp, bw, x, y, psm == PSMZ32), 4);
        return v;
    }
    case PSMCT24:
    case PSMZ24: {
        uint32_t v;
        std::memcpy(&v, vm + addr32(bp, bw, x, y, psm == PSMZ24), 4);
        return v & 0xFFFFFF;
    }
    case PSMCT16:
    case PSMCT16S:
    case PSMZ16:
    case PSMZ16S: {
        uint16_t v;
        std::memcpy(&v, vm + addr16(bp, bw, x, y, psm == PSMCT16S || psm == PSMZ16S, psm >= PSMZ32), 2);
        return v;
    }
    case PSMT8: return vm[addr8(bp, bw, x, y)];
    case PSMT4: {
        const uint32_t a = addr4(bp, bw, x, y);
        return (vm[a >> 1] >> ((a & 1) * 4)) & 0xF;
    }
    case PSMT8H: {
        uint32_t v;
        std::memcpy(&v, vm + addr32(bp, bw, x, y), 4);
        return v >> 24;
    }
    case PSMT4HL: {
        uint32_t v;
        std::memcpy(&v, vm + addr32(bp, bw, x, y), 4);
        return (v >> 24) & 0xF;
    }
    case PSMT4HH: {
        uint32_t v;
        std::memcpy(&v, vm + addr32(bp, bw, x, y), 4);
        return v >> 28;
    }
    default: return 0;
    }
}

void Gs::writePixel(uint32_t bp, uint32_t bw, uint32_t psm, int x, int y, uint32_t c) {
    uint8_t* vm = vram_.data();
    switch (psm) {
    case PSMCT32:
    case PSMZ32: std::memcpy(vm + addr32(bp, bw, x, y, psm == PSMZ32), &c, 4); break;
    case PSMCT24:
    case PSMZ24: {
        uint8_t* p = vm + addr32(bp, bw, x, y, psm == PSMZ24);
        p[0] = (uint8_t)c;
        p[1] = (uint8_t)(c >> 8);
        p[2] = (uint8_t)(c >> 16);
        break;
    }
    case PSMCT16:
    case PSMCT16S:
    case PSMZ16:
    case PSMZ16S: {
        const uint16_t v = (uint16_t)c;
        std::memcpy(vm + addr16(bp, bw, x, y, psm == PSMCT16S || psm == PSMZ16S, psm >= PSMZ32), &v, 2);
        break;
    }
    case PSMT8: vm[addr8(bp, bw, x, y)] = (uint8_t)c; break;
    case PSMT4: {
        const uint32_t a = addr4(bp, bw, x, y);
        uint8_t& b = vm[a >> 1];
        b = (a & 1) ? (uint8_t)((b & 0x0F) | ((c & 0xF) << 4)) : (uint8_t)((b & 0xF0) | (c & 0xF));
        break;
    }
    case PSMT8H: vm[addr32(bp, bw, x, y) + 3] = (uint8_t)c; break;
    case PSMT4HL: {
        uint8_t& b = vm[addr32(bp, bw, x, y) + 3];
        b = (uint8_t)((b & 0xF0) | (c & 0xF));
        break;
    }
    case PSMT4HH: {
        uint8_t& b = vm[addr32(bp, bw, x, y) + 3];
        b = (uint8_t)((b & 0x0F) | ((c & 0xF) << 4));
        break;
    }
    default: break;
    }
}

// ------------------------------------------------------------------ privileged registers

uint64_t Gs::readPriv64(uint32_t phys) {
    switch (phys & 0x1FF0) {
    case 0x0000: return pmode_;
    case 0x0020: return smode2_;
    case 0x0070: return dispfb_[0];
    case 0x0080: return display_[0];
    case 0x0090: return dispfb_[1];
    case 0x00A0: return display_[1];
    case 0x00E0: return bgcolor_;
    case 0x1000: { // CSR
        uint64_t v = csr_ & 0x1F;
        v |= (uint64_t)(field_ & 1) << 13;
        v |= 0x4000;              // FIFO empty
        v |= (uint64_t)0x1B << 16; // revision
        v |= (uint64_t)0x55 << 24; // id
        return v;
    }
    case 0x1010: return imr_;
    case 0x1040: return busdir_;
    case 0x1080: return siglblid_;
    default: return 0;
    }
}

void Gs::writePriv32(uint32_t phys, uint32_t v) {
    uint64_t cur = readPriv64(phys & ~7u);
    if ((phys & 0x1FF0) == 0x1000) cur = 0; // CSR: write-to-clear semantics
    if (phys & 4) cur = (cur & 0xFFFFFFFFull) | ((uint64_t)v << 32);
    else cur = (cur & ~0xFFFFFFFFull) | v;
    writePriv64(phys & ~7u, cur);
}

void Gs::writePriv64(uint32_t phys, uint64_t v) {
    switch (phys & 0x1FF0) {
    case 0x0000: pmode_ = v; break;
    case 0x0010: smode1_ = v; break;
    case 0x0020: smode2_ = v; break;
    case 0x0030: srfsh_ = v; break;
    case 0x0040: synch1_ = v; break;
    case 0x0050: synch2_ = v; break;
    case 0x0060: syncv_ = v; break;
    case 0x0070: dispfb_[0] = v; break;
    case 0x0080: display_[0] = v; break;
    case 0x0090: dispfb_[1] = v; break;
    case 0x00A0: display_[1] = v; break;
    case 0x00B0: extbuf_ = v; break;
    case 0x00C0: extdata_ = v; break;
    case 0x00D0: extwrite_ = v; break;
    case 0x00E0: bgcolor_ = v; break;
    case 0x1000: // CSR
        csr_ &= ~(v & 0x1F);
        if (v & 0x200) { // RESET
            csr_ = 0;
            for (auto& p : path_) p = GifTagState{};
            vcount_ = 0;
        }
        break;
    case 0x1010: imr_ = v & 0x7F00; break;
    case 0x1040: busdir_ = v; break;
    case 0x1080: siglblid_ = v; break;
    default: break;
    }
}

// ------------------------------------------------------------------ video timing

bool Gs::interlaced() const { return (smode2_ & 1) != 0; }

void Gs::startVideo() {
    const int64_t t0 = m_.cycles;
    m_.schedule(t0 + kFieldCycles - kVblankCycles, [this]() { vblankStart(); });
}

void Gs::vblankStart() {
    csr_ |= 0x8; // VSINT
    field_ ^= 1;
    m_.intc->raise(INTC_VBLANK_START);
    if (!(imr_ & 0x800)) m_.intc->raise(INTC_GS);
    if (m_.kernel->vsyncFlagAddr) {
        m_.write32((uint32_t)m_.kernel->vsyncFlagAddr, 1);
        if (m_.kernel->vsyncCounterAddr) {
            const uint32_t a = (uint32_t)m_.kernel->vsyncCounterAddr;
            m_.write64(a, m_.read64(a) + 1);
        }
    }
    m_.frames++;
    GsFrame f;
    buildFrame(f);
    f.frameNumber = m_.frames;
    {
        std::lock_guard<std::mutex> lk(frameMutex_);
        frame_ = f;
        frameValid_ = true;
    }
    if (onFrame) onFrame(f);
    if (m_.cfg.maxFrames && m_.frames >= m_.cfg.maxFrames) m_.requestExit(0);
    m_.schedule(m_.cycles + kVblankCycles, [this]() { vblankEnd(); });
}

void Gs::vblankEnd() {
    m_.intc->raise(INTC_VBLANK_END);
    m_.timers->onHblank();
    m_.schedule(m_.cycles + kFieldCycles - kVblankCycles, [this]() { vblankStart(); });
}

bool Gs::latestFrame(GsFrame& out) {
    std::lock_guard<std::mutex> lk(frameMutex_);
    if (!frameValid_) return false;
    out = frame_;
    return true;
}

void Gs::buildFrame(GsFrame& f) {
    const bool en1 = pmode_ & 1, en2 = (pmode_ >> 1) & 1;
    int circuit = en1 ? 0 : (en2 ? 1 : -1);
    if (circuit < 0) {
        f.width = 640;
        f.height = 448;
        f.pixels.assign((size_t)f.width * f.height, 0xFF000000u);
        return;
    }
    const uint64_t fb = dispfb_[circuit], disp = display_[circuit];
    const uint32_t fbp = bits(fb, 0, 9) * 32, fbw = bits(fb, 9, 6), psm = bits(fb, 15, 5);
    const uint32_t dbx = bits(fb, 32, 11), dby = bits(fb, 43, 11);
    const uint32_t magh = bits(disp, 23, 4) + 1, magv = bits(disp, 27, 2) + 1;
    uint32_t w = (bits(disp, 32, 12) + 1) / magh;
    uint32_t h = (bits(disp, 44, 11) + 1) / magv;
    if (w == 0 || w > 2048) w = 640;
    if (h == 0 || h > 2048) h = 448;
    const bool fieldMode = interlaced() && ((smode2_ >> 1) & 1); // FFMD
    f.interlaced = interlaced();
    const uint32_t outH = fieldMode ? h * 2 : h;
    f.width = w;
    f.height = outH;
    f.pixels.resize((size_t)w * outH);
    const bool second = en1 && en2;
    const uint64_t fb2 = dispfb_[1];
    const uint32_t fbp2 = bits(fb2, 0, 9) * 32, fbw2 = bits(fb2, 9, 6), psm2 = bits(fb2, 15, 5);
    const uint32_t dbx2 = bits(fb2, 32, 11), dby2 = bits(fb2, 43, 11);
    const uint32_t alp = bits(pmode_, 8, 8);
    const bool mmod = (pmode_ >> 5) & 1;
    auto fetch = [&](uint32_t bp, uint32_t bw, uint32_t pm, uint32_t x, uint32_t y) -> uint32_t {
        uint32_t c = readPixel(bp, bw, pm, (int)x, (int)y);
        if (pm == PSMCT16 || pm == PSMCT16S) {
            const uint32_t r = (c & 0x1F) << 3, g = ((c >> 5) & 0x1F) << 3, b = ((c >> 10) & 0x1F) << 3;
            const uint32_t a = (c & 0x8000) ? 0x80 : 0;
            c = r | (g << 8) | (b << 16) | (a << 24);
        } else if (pm == PSMCT24) {
            c |= 0x80000000u;
        }
        return c;
    };
    for (uint32_t y = 0; y < outH; ++y) {
        const uint32_t sy = fieldMode ? (y >> 1) : y;
        for (uint32_t x = 0; x < w; ++x) {
            uint32_t c = fetch(fbp, fbw, psm, dbx + x, dby + sy);
            if (second) {
                const uint32_t c2 = fetch(fbp2, fbw2, psm2, dbx2 + x, dby2 + sy);
                const uint32_t a = mmod ? alp : std::min<uint32_t>(255, ((c >> 24) & 0xFF) * 2);
                uint32_t out = 0;
                for (int k = 0; k < 3; ++k) {
                    const uint32_t c1 = (c >> (k * 8)) & 0xFF, cc2 = (c2 >> (k * 8)) & 0xFF;
                    out |= (std::min<uint32_t>(255, (c1 * a + cc2 * (255 - a)) / 255)) << (k * 8);
                }
                c = out;
            }
            f.pixels[(size_t)y * w + x] = (c & 0x00FFFFFFu) | 0xFF000000u;
        }
    }
}

// ------------------------------------------------------------------ general registers

void Gs::writePacked(uint32_t reg, uint64_t lo, uint64_t hi) {
    switch (reg) {
    case 0x0: writeReg(0x00, lo); break;
    case 0x1: { // RGBAQ
        const uint64_t r = lo & 0xFF, g = (lo >> 32) & 0xFF, b = hi & 0xFF, a = (hi >> 32) & 0xFF;
        uint32_t qbits;
        std::memcpy(&qbits, &q_, 4);
        writeReg(0x01, r | (g << 8) | (b << 16) | (a << 24) | ((uint64_t)qbits << 32));
        break;
    }
    case 0x2: { // ST (+Q)
        writeReg(0x02, lo);
        uint32_t qb = (uint32_t)hi;
        std::memcpy(&q_, &qb, 4);
        break;
    }
    case 0x3: writeReg(0x03, (lo & 0x3FFF) | (((lo >> 32) & 0x3FFF) << 16)); break;
    case 0x4: { // XYZF2
        const uint64_t x = lo & 0xFFFF, y = (lo >> 32) & 0xFFFF;
        const uint64_t z = (hi >> 4) & 0xFFFFFF, f = (hi >> 36) & 0xFF;
        const bool adc = (hi >> 47) & 1;
        writeReg(adc ? 0x0C : 0x04, x | (y << 16) | (z << 32) | (f << 56));
        break;
    }
    case 0x5: { // XYZ2
        const uint64_t x = lo & 0xFFFF, y = (lo >> 32) & 0xFFFF, z = hi & 0xFFFFFFFF;
        const bool adc = (hi >> 47) & 1;
        writeReg(adc ? 0x0D : 0x05, x | (y << 16) | (z << 32));
        break;
    }
    case 0x6: case 0x7: case 0x8: case 0x9: writeReg(reg, lo); break;
    case 0xA: writeReg(0x0A, ((hi >> 36) & 0xFF) << 56); break;
    case 0xC: case 0xD: writeReg(reg, lo); break;
    case 0xE: writeReg((uint32_t)(hi & 0xFF), lo); break;
    default: break; // NOP
    }
}

void Gs::writeReg(uint32_t reg, uint64_t v) {
    switch (reg) {
    case 0x00: // PRIM
        prim_ = v;
        vcount_ = 0;
        stripIndex_ = 0;
        break;
    case 0x01: rgbaq_ = v; break;
    case 0x02: st_ = v; break;
    case 0x03: uv_ = v; break;
    case 0x04: case 0x05: case 0x0C: case 0x0D: { // XYZ(F)2/3
        const bool fog = (reg == 0x04 || reg == 0x0C);
        const bool drawing = (reg == 0x04 || reg == 0x05);
        const uint64_t pr = (prmodecont_ & 1) ? prim_ : ((prim_ & 7) | (prmode_ & ~7ull));
        const GsContextRegs& c = ctx_[(pr >> 9) & 1];
        GsVertex vx;
        vx.x = (int32_t)(v & 0xFFFF) - (int32_t)(c.xyoffset & 0xFFFF);
        vx.y = (int32_t)((v >> 16) & 0xFFFF) - (int32_t)((c.xyoffset >> 32) & 0xFFFF);
        vx.z = fog ? (uint32_t)((v >> 32) & 0xFFFFFF) : (uint32_t)(v >> 32);
        vx.fog = fog ? (uint8_t)(v >> 56) : (uint8_t)(fog_ >> 56);
        vx.r = (uint8_t)rgbaq_;
        vx.g = (uint8_t)(rgbaq_ >> 8);
        vx.b = (uint8_t)(rgbaq_ >> 16);
        vx.a = (uint8_t)(rgbaq_ >> 24);
        uint32_t qb = (uint32_t)(rgbaq_ >> 32);
        std::memcpy(&vx.q, &qb, 4);
        uint32_t sb = (uint32_t)st_, tb = (uint32_t)(st_ >> 32);
        std::memcpy(&vx.s, &sb, 4);
        std::memcpy(&vx.t, &tb, 4);
        vx.u = (uint16_t)(uv_ & 0x3FFF);
        vx.v = (uint16_t)((uv_ >> 16) & 0x3FFF);
        stats.vertices++;
        const uint32_t type = pr & 7;
        auto push = [&](const GsVertex& p) {
            if (vcount_ < 3) vq_[vcount_++] = p;
            else {
                vq_[0] = vq_[1];
                vq_[1] = vq_[2];
                vq_[2] = p;
            }
        };
        switch (type) {
        case 0: // point
            vq_[0] = vx;
            vcount_ = 1;
            if (drawing) raster_->draw(pr, vq_, 1);
            vcount_ = 0;
            break;
        case 1: // line
            push(vx);
            if (vcount_ == 2) {
                if (drawing) raster_->draw(pr, vq_, 2);
                vcount_ = 0;
            }
            break;
        case 2: // line strip
            if (vcount_ >= 2) {
                vq_[0] = vq_[1];
                vcount_ = 1;
            }
            push(vx);
            if (vcount_ == 2 && drawing) raster_->draw(pr, vq_, 2);
            break;
        case 3: // triangle
            push(vx);
            if (vcount_ == 3) {
                if (drawing) raster_->draw(pr, vq_, 3);
                vcount_ = 0;
            }
            break;
        case 4: // triangle strip
            push(vx);
            if (vcount_ == 3 && drawing) raster_->draw(pr, vq_, 3);
            break;
        case 5: // triangle fan
            if (vcount_ == 0) {
                fanFirst_ = vx;
                vq_[0] = vx;
                vcount_ = 1;
            } else if (vcount_ == 1) {
                vq_[1] = vx;
                vcount_ = 2;
            } else {
                vq_[0] = fanFirst_;
                vq_[2] = vx;
                if (drawing) raster_->draw(pr, vq_, 3);
                vq_[1] = vx;
            }
            break;
        case 6: // sprite
            push(vx);
            if (vcount_ == 2) {
                if (drawing) raster_->draw(pr, vq_, 2);
                vcount_ = 0;
            }
            break;
        default:
            vcount_ = 0;
            break;
        }
        break;
    }
    case 0x06: case 0x07: {
        GsContextRegs& c = ctx_[reg - 0x06];
        c.tex0 = v;
        const uint32_t cld = bits(v, 61, 3);
        if (cld) loadClut(v);
        break;
    }
    case 0x08: case 0x09: ctx_[reg - 0x08].clamp = v; break;
    case 0x0A: fog_ = v; break;
    case 0x14: case 0x15: ctx_[reg - 0x14].tex1 = v; break;
    case 0x16: case 0x17: { // TEX2: update palette fields of TEX0
        GsContextRegs& c = ctx_[reg - 0x16];
        const uint64_t mask = 0xFFFFFFE0'03F00000ull; // PSM + CBP/CPSM/CSM/CSA/CLD
        c.tex0 = (c.tex0 & ~mask) | (v & mask);
        c.tex2 = v;
        if (bits(v, 61, 3)) loadClut(c.tex0);
        break;
    }
    case 0x18: case 0x19: ctx_[reg - 0x18].xyoffset = v; break;
    case 0x1A: prmodecont_ = v; break;
    case 0x1B: prmode_ = v; break;
    case 0x1C: texclut_ = v; break;
    case 0x22: scanmsk_ = v; break;
    case 0x34: case 0x35: ctx_[reg - 0x34].miptbp1 = v; break;
    case 0x36: case 0x37: ctx_[reg - 0x36].miptbp2 = v; break;
    case 0x3B: texa_ = v; break;
    case 0x3D: fogcol_ = v; break;
    case 0x3F: break; // TEXFLUSH
    case 0x40: case 0x41: ctx_[reg - 0x40].scissor = v; break;
    case 0x42: case 0x43: ctx_[reg - 0x42].alpha = v; break;
    case 0x44: dimx_ = v; break;
    case 0x45: dthe_ = v; break;
    case 0x46: colclamp_ = v; break;
    case 0x47: case 0x48: ctx_[reg - 0x47].test = v; break;
    case 0x49: pabe_ = v; break;
    case 0x4A: case 0x4B: ctx_[reg - 0x4A].fba = v; break;
    case 0x4C: case 0x4D: ctx_[reg - 0x4C].frame = v; break;
    case 0x4E: case 0x4F: ctx_[reg - 0x4E].zbuf = v; break;
    case 0x50: bitbltbuf_ = v; break;
    case 0x51: trxpos_ = v; break;
    case 0x52: trxreg_ = v; break;
    case 0x53:
        trxdir_ = v;
        transferBegin();
        break;
    case 0x54: { // HWREG
        uint8_t q[8];
        std::memcpy(q, &v, 8);
        xfer_.partial.insert(xfer_.partial.end(), q, q + 8);
        writeImage(nullptr, 0);
        break;
    }
    case 0x60: { // SIGNAL
        const uint32_t id = (uint32_t)v, mask = (uint32_t)(v >> 32);
        siglblid_ = (siglblid_ & ~(uint64_t)mask) | (id & mask);
        csr_ |= 1;
        if (!(imr_ & 0x100)) m_.intc->raise(INTC_GS);
        break;
    }
    case 0x61: // FINISH
        csr_ |= 2;
        if (!(imr_ & 0x200)) m_.intc->raise(INTC_GS);
        break;
    case 0x62: { // LABEL
        const uint32_t id = (uint32_t)v, mask = (uint32_t)(v >> 32);
        const uint64_t lbl = (siglblid_ >> 32);
        siglblid_ = (siglblid_ & 0xFFFFFFFFull) | ((uint64_t)((lbl & ~mask) | (id & mask)) << 32);
        break;
    }
    default:
        break;
    }
}

// ------------------------------------------------------------------ CLUT

void Gs::loadClut(uint64_t tex0) {
    const uint32_t cld = bits(tex0, 61, 3);
    const uint32_t cbp = bits(tex0, 37, 14), cpsm = bits(tex0, 51, 4), csm = bits(tex0, 55, 1);
    const uint32_t csa = bits(tex0, 56, 5), psm = bits(tex0, 20, 6);
    static uint32_t cbp0 = ~0u, cbp1 = ~0u;
    bool load = false;
    switch (cld) {
    case 1: load = true; break;
    case 2: load = true; cbp0 = cbp; break;
    case 3: load = true; cbp1 = cbp; break;
    case 4: load = cbp != cbp0; cbp0 = cbp; break;
    case 5: load = cbp != cbp1; cbp1 = cbp; break;
    default: break;
    }
    if (!load) return;
    const bool is8 = psm == PSMT8 || psm == PSMT8H;
    const uint32_t entries = is8 ? 256 : 16;
    const bool ct32 = cpsm == PSMCT32 || cpsm == PSMCT24;
    for (uint32_t i = 0; i < entries; ++i) {
        uint32_t x, y;
        if (csm == 0) {
            if (is8) {
                const uint32_t j = (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1);
                x = j & 15;
                y = j >> 4;
            } else {
                x = i & 7;
                y = i >> 3;
            }
            const uint32_t c = readPixel(cbp, 1, ct32 ? PSMCT32 : cpsm, (int)x, (int)y);
            const uint32_t idx = is8 ? i : (csa * 16 + i);
            if (ct32) {
                const uint32_t base = is8 ? i : ((csa & 15) * 16 + i);
                clut_[base & 255] = (uint16_t)c;
                clut_[(base & 255) + 256] = (uint16_t)(c >> 16);
            } else {
                clut_[idx & 511] = (uint16_t)c;
            }
        } else {
            const uint32_t cbw = bits(texclut_, 0, 6), cou = bits(texclut_, 6, 6), cov = bits(texclut_, 12, 10);
            x = cou * 16 + i;
            y = cov;
            const uint32_t c = readPixel(cbp, cbw, cpsm == PSMCT32 ? PSMCT32 : PSMCT16, (int)x, (int)y);
            if (ct32) {
                clut_[i & 255] = (uint16_t)c;
                clut_[(i & 255) + 256] = (uint16_t)(c >> 16);
            } else {
                clut_[(csa * 16 + i) & 511] = (uint16_t)c;
            }
        }
    }
}

// ------------------------------------------------------------------ transfers

void Gs::transferBegin() {
    xfer_ = Xfer{};
    xfer_.dir = (int)(trxdir_ & 3);
    xfer_.sx = bits(trxpos_, 0, 11);
    xfer_.sy = bits(trxpos_, 16, 11);
    xfer_.dx = bits(trxpos_, 32, 11);
    xfer_.dy = bits(trxpos_, 48, 11);
    xfer_.w = bits(trxreg_, 0, 12);
    xfer_.h = bits(trxreg_, 32, 12);
    xfer_.x = 0;
    xfer_.y = 0;
    stats.transfers++;
    if (xfer_.dir == 0) {
        xfer_.bp = bits(bitbltbuf_, 32, 14);
        xfer_.bw = bits(bitbltbuf_, 48, 6);
        xfer_.psm = bits(bitbltbuf_, 56, 6);
        xfer_.active = xfer_.w && xfer_.h;
    } else if (xfer_.dir == 1) {
        xfer_.bp = bits(bitbltbuf_, 0, 14);
        xfer_.bw = bits(bitbltbuf_, 16, 6);
        xfer_.psm = bits(bitbltbuf_, 24, 6);
        xfer_.active = xfer_.w && xfer_.h;
    } else if (xfer_.dir == 2) {
        localToLocal();
    }
}

void Gs::localToLocal() {
    const uint32_t sbp = bits(bitbltbuf_, 0, 14), sbw = bits(bitbltbuf_, 16, 6), spsm = bits(bitbltbuf_, 24, 6);
    const uint32_t dbp = bits(bitbltbuf_, 32, 14), dbw = bits(bitbltbuf_, 48, 6), dpsm = bits(bitbltbuf_, 56, 6);
    const uint32_t dirBits = bits(trxpos_, 59, 2);
    const uint32_t w = xfer_.w, h = xfer_.h;
    std::vector<uint32_t> tmp((size_t)w * h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            tmp[(size_t)y * w + x] = readPixel(sbp, sbw, spsm, (int)((xfer_.sx + x) & 2047), (int)((xfer_.sy + y) & 2047));
    (void)dirBits;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            writePixel(dbp, dbw, dpsm, (int)((xfer_.dx + x) & 2047), (int)((xfer_.dy + y) & 2047), tmp[(size_t)y * w + x]);
}

void Gs::writeImage(const uint8_t* data, size_t qwords) {
    if (data && qwords) xfer_.partial.insert(xfer_.partial.end(), data, data + qwords * 16);
    if (!xfer_.active || xfer_.dir != 0) {
        xfer_.partial.clear();
        return;
    }
    const uint32_t bpp = bitsPerPixel(xfer_.psm);
    const uint32_t bytesPer = (xfer_.psm == PSMCT24 || xfer_.psm == PSMZ24) ? 3 : 0;
    size_t pos = 0;
    auto& buf = xfer_.partial;
    auto advance = [&]() {
        if (++xfer_.x >= xfer_.w) {
            xfer_.x = 0;
            if (++xfer_.y >= xfer_.h) xfer_.active = false;
        }
    };
    while (xfer_.active) {
        const int px = (int)((xfer_.dx + xfer_.x) & 2047), py = (int)((xfer_.dy + xfer_.y) & 2047);
        if (bytesPer == 3) {
            if (pos + 3 > buf.size()) break;
            const uint32_t c = buf[pos] | (buf[pos + 1] << 8) | (buf[pos + 2] << 16);
            pos += 3;
            writePixel(xfer_.bp, xfer_.bw, xfer_.psm, px, py, c);
            advance();
        } else if (bpp == 32) {
            if (pos + 4 > buf.size()) break;
            uint32_t c;
            std::memcpy(&c, &buf[pos], 4);
            pos += 4;
            writePixel(xfer_.bp, xfer_.bw, xfer_.psm, px, py, c);
            advance();
        } else if (bpp == 16) {
            if (pos + 2 > buf.size()) break;
            const uint32_t c = buf[pos] | (buf[pos + 1] << 8);
            pos += 2;
            writePixel(xfer_.bp, xfer_.bw, xfer_.psm, px, py, c);
            advance();
        } else if (bpp == 8) {
            if (pos + 1 > buf.size()) break;
            writePixel(xfer_.bp, xfer_.bw, xfer_.psm, px, py, buf[pos++]);
            advance();
        } else { // 4-bit: two pixels per byte, low nibble first
            if (pos + 1 > buf.size()) break;
            const uint8_t b = buf[pos++];
            writePixel(xfer_.bp, xfer_.bw, xfer_.psm, px, py, b & 0xF);
            advance();
            if (!xfer_.active) break;
            const int px2 = (int)((xfer_.dx + xfer_.x) & 2047), py2 = (int)((xfer_.dy + xfer_.y) & 2047);
            writePixel(xfer_.bp, xfer_.bw, xfer_.psm, px2, py2, b >> 4);
            advance();
        }
    }
    buf.erase(buf.begin(), buf.begin() + (ptrdiff_t)std::min(pos, buf.size()));
    if (!xfer_.active) buf.clear();
}

size_t Gs::readImage(uint8_t* out, size_t maxQwords) {
    if (!xfer_.active || xfer_.dir != 1) return 0;
    size_t produced = 0;
    const uint32_t bpp = bitsPerPixel(xfer_.psm);
    std::vector<uint8_t> bytes;
    while (xfer_.active && bytes.size() < maxQwords * 16) {
        const int px = (int)((xfer_.sx + xfer_.x) & 2047), py = (int)((xfer_.sy + xfer_.y) & 2047);
        const uint32_t c = readPixel(xfer_.bp, xfer_.bw, xfer_.psm, px, py);
        for (uint32_t k = 0; k < std::max<uint32_t>(1, bpp / 8); ++k) bytes.push_back((uint8_t)(c >> (k * 8)));
        if (++xfer_.x >= xfer_.w) {
            xfer_.x = 0;
            if (++xfer_.y >= xfer_.h) xfer_.active = false;
        }
    }
    produced = (bytes.size() + 15) / 16;
    std::memset(out, 0, produced * 16);
    std::memcpy(out, bytes.data(), bytes.size());
    return produced;
}

// ------------------------------------------------------------------ GIF packets

size_t Gs::processTag(const uint8_t* data, size_t qwords, GifTagState& st, int path) {
    (void)path;
    size_t i = 0;
    while (i < qwords) {
        const uint8_t* q = data + i * 16;
        if (!st.active) {
            uint64_t lo, hi;
            std::memcpy(&lo, q, 8);
            std::memcpy(&hi, q + 8, 8);
            st.nloop = (uint32_t)(lo & 0x7FFF);
            st.eop = (uint32_t)((lo >> 15) & 1);
            const bool pre = (lo >> 46) & 1;
            const uint32_t primField = (uint32_t)((lo >> 47) & 0x7FF);
            st.flg = (uint32_t)((lo >> 58) & 3);
            st.nreg = (uint32_t)((lo >> 60) & 0xF);
            if (st.nreg == 0) st.nreg = 16;
            st.regs = hi;
            st.regIndex = 0;
            ++i;
            stats.gifPackets++;
            if (pre && st.flg == 0) writeReg(0x00, primField);
            st.active = st.nloop > 0;
            if (!st.active && st.eop) return i;
            continue;
        }
        if (st.flg == 0) { // PACKED
            uint64_t lo, hi;
            std::memcpy(&lo, q, 8);
            std::memcpy(&hi, q + 8, 8);
            const uint32_t reg = (uint32_t)((st.regs >> (st.regIndex * 4)) & 0xF);
            writePacked(reg, lo, hi);
            ++i;
            if (++st.regIndex >= st.nreg) {
                st.regIndex = 0;
                if (--st.nloop == 0) {
                    st.active = false;
                    if (st.eop) return i;
                }
            }
        } else if (st.flg == 1) { // REGLIST
            for (int half = 0; half < 2 && st.active; ++half) {
                uint64_t val;
                std::memcpy(&val, q + half * 8, 8);
                const uint32_t reg = (uint32_t)((st.regs >> (st.regIndex * 4)) & 0xF);
                if (reg != 0xE) writePacked(reg, val, 0);
                if (++st.regIndex >= st.nreg) {
                    st.regIndex = 0;
                    if (--st.nloop == 0) st.active = false;
                }
            }
            ++i;
            if (!st.active && st.eop) return i;
        } else { // IMAGE
            const size_t n = std::min<size_t>(st.nloop, qwords - i);
            writeImage(q, n);
            i += n;
            st.nloop -= (uint32_t)n;
            if (st.nloop == 0) {
                st.active = false;
                if (st.eop) return i;
            }
        }
    }
    return i;
}

size_t Gs::gifTransfer(const uint8_t* data, size_t qwords, int path) {
    GifTagState& st = path_[path & 3];
    size_t done = 0;
    while (done < qwords) {
        const size_t n = processTag(data + done * 16, qwords - done, st, path);
        done += n;
        if (n == 0) break;
        if (path == 1 && !st.active && st.eop) {
            st = GifTagState{};
            break; // XGKICK packet ends at EOP
        }
        if (!st.active && st.eop) st = GifTagState{};
    }
    return done;
}

void Gs::gifPath1(const uint8_t* vuMem, uint32_t addr) {
    // Walk packets in VU1 memory until EOP.
    GifTagState st;
    uint32_t a = addr & 0x3FF;
    for (int guard = 0; guard < 1024 * 16; ++guard) {
        const uint8_t* q = vuMem + a * 16;
        size_t n = processTag(q, 1, st, 1);
        (void)n;
        a = (a + 1) & 0x3FF;
        if (!st.active && st.eop) break;
    }
}

// ------------------------------------------------------------------ GIF registers

uint32_t Gif::read(uint32_t phys) {
    switch (phys & 0xF0) {
    case 0x00: return ctrl;
    case 0x10: return mode;
    case 0x20: return (stat & ~3u) | (m_.vif->state(1).maskPath3 ? 2u : 0u) | (mode & 1);
    default: return 0;
    }
}
void Gif::write(uint32_t phys, uint32_t v) {
    switch (phys & 0xF0) {
    case 0x00:
        ctrl = v & 9;
        break;
    case 0x10: mode = v; break;
    default: break;
    }
}
void Gif::writeFifo(const uint8_t* qword) { m_.gs->gifTransfer(qword, 1, 3); }

} // namespace ps2
