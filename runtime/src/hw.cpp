// Memory-mapped I/O dispatch, DMA controller and EE timers.
#include "ps2/hw.hpp"
#include "ps2/ipu.hpp"

#include <cstring>

#include "ps2/gs.hpp"
#include "ps2/log.hpp"
#include "ps2/machine.hpp"
#include "ps2/sif.hpp"
#include "ps2/vif.hpp"

namespace ps2 {

namespace {
const uint32_t kChanBase[10] = {0x10008000, 0x10009000, 0x1000A000, 0x1000B000, 0x1000B400,
                                0x1000C000, 0x1000C400, 0x1000C800, 0x1000D000, 0x1000D400};
int channelOf(uint32_t phys) {
    for (int i = 0; i < 10; ++i)
        if ((phys & ~0xFFu) == kChanBase[i]) return i;
    return -1;
}
} // namespace

// ------------------------------------------------------------------ MMIO dispatch

uint32_t Machine::mmioRead32(uint32_t p) {
    if (p >= 0x12000000 && p < 0x12002000) return gs->readPriv32(p);
    if (p < 0x10002000 && p >= 0x10000000) return timers->read(p);
    if (p >= 0x10002000 && p < 0x10003000) return (uint32_t)(ipu->read64(p & ~7u) >> ((p & 4) * 8));
    if (p >= 0x10003000 && p < 0x10003800) return gif->read(p);
    if (p >= 0x10003800 && p < 0x10003C00) return vif->readReg(0, p & 0x3FF);
    if (p >= 0x10003C00 && p < 0x10004000) return vif->readReg(1, p & 0x3FF);
    if (p >= 0x10008000 && p < 0x1000F000) return dmac->read(p);
    switch (p) {
    case 0x1000F000: return intc->stat;
    case 0x1000F010: return intc->mask;
    case 0x1000F130: return 0;          // SIO ISR
    case 0x1000F180: return 0;
    case 0x1000F520: return dmac->enable; // D_ENABLER
    case 0x1000F430: case 0x1000F440: return 0; // MCH
    default: break;
    }
    if (p >= 0x1000F200 && p < 0x1000F270) return sif->mmioRead(p);
    if (p >= 0x1FC00000 && p < 0x20000000) return 0; // BIOS (not present)
    static int warned = 0;
    if (warned++ < 32) PS2_DEBUG(Mem, "unmapped read32 %08x", p);
    return 0;
}

void Machine::mmioWrite32(uint32_t p, uint32_t v) {
    if (p >= 0x12000000 && p < 0x12002000) {
        gs->writePriv32(p, v);
        return;
    }
    if (p >= 0x10000000 && p < 0x10002000) {
        timers->write(p, v);
        return;
    }
    if (p >= 0x10002000 && p < 0x10003000) {
        if (!(p & 4)) ipu->write64(p, v); // upper words of the 64-bit registers are unused
        return;
    }
    if (p >= 0x10003000 && p < 0x10003800) {
        gif->write(p, v);
        return;
    }
    if (p >= 0x10003800 && p < 0x10003C00) {
        vif->writeReg(0, p & 0x3FF, v);
        return;
    }
    if (p >= 0x10003C00 && p < 0x10004000) {
        vif->writeReg(1, p & 0x3FF, v);
        return;
    }
    if (p >= 0x10008000 && p < 0x1000F000) {
        dmac->write(p, v);
        return;
    }
    switch (p) {
    case 0x1000F000: intc->stat &= ~v; return;
    case 0x1000F010: intc->mask ^= v; return;
    case 0x1000F180: {
        char ch = (char)v;
        console(&ch, 1);
        return;
    }
    case 0x1000F590: dmac->enable = v; return; // D_ENABLEW
    default: break;
    }
    if (p >= 0x1000F100 && p < 0x1000F200) return; // other SIO registers
    if (p >= 0x1000F200 && p < 0x1000F270) {
        sif->mmioWrite(p, v);
        return;
    }
    static int warned = 0;
    if (warned++ < 32) PS2_DEBUG(Mem, "unmapped write32 %08x = %08x", p, v);
}

uint64_t Machine::mmioRead64(uint32_t p) {
    if (p >= 0x12000000 && p < 0x12002000) return gs->readPriv64(p);
    return (uint64_t)mmioRead32(p) | ((uint64_t)mmioRead32(p + 4) << 32);
}

void Machine::mmioWrite64(uint32_t p, uint64_t v) {
    if (p >= 0x12000000 && p < 0x12002000) {
        gs->writePriv64(p, v);
        return;
    }
    if (p >= 0x10004000 && p < 0x10008000) {
        Reg128 q{};
        q.u64[0] = v;
        mmioWrite128(p, &q);
        return;
    }
    mmioWrite32(p, (uint32_t)v);
    mmioWrite32(p + 4, (uint32_t)(v >> 32));
}

void Machine::mmioRead128(uint32_t p, Reg128* out) {
    std::memset(out, 0, 16);
    if (p >= 0x10007000 && p < 0x10007010) {
        ipu->readFifo(out);
        return;
    }
    if (p >= 0x10004000 && p < 0x10008000) return; // FIFO reads
    out->u64[0] = mmioRead64(p);
    out->u64[1] = mmioRead64(p + 8);
}

void Machine::mmioWrite128(uint32_t p, const Reg128* v) {
    if (p >= 0x10004000 && p < 0x10005000) {
        vif->feed(0, v->u32, 4);
        return;
    }
    if (p >= 0x10005000 && p < 0x10006000) {
        vif->feed(1, v->u32, 4);
        return;
    }
    if (p >= 0x10006000 && p < 0x10007000) {
        gif->writeFifo(v->u8);
        return;
    }
    if (p >= 0x10007010 && p < 0x10007020) {
        ipu->writeFifo(v);
        return;
    }
    if (p >= 0x10007000 && p < 0x10008000) return;
    mmioWrite64(p, v->u64[0]);
    mmioWrite64(p + 8, v->u64[1]);
}

// ------------------------------------------------------------------ DMAC

uint32_t Dmac::read(uint32_t p) {
    const int c = channelOf(p);
    if (c >= 0) {
        const DmaRegs& r = ch[c];
        switch (p & 0xF0) {
        case 0x00: return r.chcr;
        case 0x10: return r.madr;
        case 0x20: return r.qwc;
        case 0x30: return r.tadr;
        case 0x40: return r.asr0;
        case 0x50: return r.asr1;
        case 0x80: return r.sadr;
        default: return 0;
        }
    }
    switch (p) {
    case 0x1000E000: return ctrl;
    case 0x1000E010: return stat;
    case 0x1000E020: return pcr;
    case 0x1000E030: return sqwc;
    case 0x1000E040: return rbsr;
    case 0x1000E050: return rbor;
    case 0x1000E060: return stadr;
    default: return 0;
    }
}

void Dmac::write(uint32_t p, uint32_t v) {
    const int c = channelOf(p);
    if (c >= 0) {
        DmaRegs& r = ch[c];
        switch (p & 0xF0) {
        case 0x00: {
            const bool wasStarted = r.chcr & 0x100;
            r.chcr = v;
            if (!wasStarted && (v & 0x100)) kick(c);
            break;
        }
        case 0x10: r.madr = (c == DMA_TO_SPR || c == DMA_FROM_SPR) ? (v & 0x7FFFFFF0u) : (v & ~0xFu); break;
        case 0x20: r.qwc = v & 0xFFFF; break;
        case 0x30: r.tadr = v & ~0xFu; break;
        case 0x40: r.asr0 = v & ~0xFu; break;
        case 0x50: r.asr1 = v & ~0xFu; break;
        case 0x80: r.sadr = v & 0x3FF0; break;
        default: break;
        }
        return;
    }
    switch (p) {
    case 0x1000E000: ctrl = v; break;
    case 0x1000E010:
        stat &= ~(v & 0xFFFF);
        stat ^= (v & 0xFFFF0000u);
        break;
    case 0x1000E020: pcr = v; break;
    case 0x1000E030: sqwc = v; break;
    case 0x1000E040: rbsr = v; break;
    case 0x1000E050: rbor = v; break;
    case 0x1000E060: stadr = v; break;
    default: break;
    }
}

void Dmac::complete(int c) {
    ch[c].chcr &= ~0x100u;
    stat |= 1u << c;
}

void Dmac::kick(int c) {
    if (!(ctrl & 1) && c != DMA_SIF0) {
        // DMAC disabled; transfer will start when enabled (rare).
    }
    switch (c) {
    case DMA_TO_SPR: transferToSpr(c); break;
    case DMA_FROM_SPR: transferFromSpr(c); break;
    case DMA_GIF:
    case DMA_VIF0:
    case DMA_VIF1: transferGifVif(c); break;
    case DMA_SIF0: m_.sif->pump(); return;
    case DMA_SIF1: transferSif1(); break;
    case DMA_FROM_IPU: m_.ipu->dmaOutKick(); return;
    case DMA_TO_IPU: m_.ipu->dmaInKick(); return;
    default: break;
    }
}

// Generic source-chain walker: calls sink(qwordPtr, count) for data, and
// tagSink(tagQword) for transfer tags when TTE is set.
template <typename DataFn, typename TagFn>
static void walkSource(Machine& m, DmaRegs& r, DataFn&& data, TagFn&& tag, int64_t& costQw) {
    auto xfer = [&](uint32_t addr, uint32_t qwc, bool fromSpr) {
        for (uint32_t i = 0; i < qwc; ++i) {
            Reg128 q;
            if (fromSpr) std::memcpy(&q, m.spr + ((addr + i * 16) & (SPR_SIZE - 1)), 16);
            else m.read128(addr + i * 16, &q);
            data(q);
        }
        costQw += qwc;
    };
    const uint32_t mod = (r.chcr >> 2) & 3;
    if (mod == 0) { // normal
        xfer(r.madr, r.qwc, (r.madr & 0x80000000u) != 0);
        r.madr += r.qwc * 16;
        r.qwc = 0;
        return;
    }
    // chain
    if (r.qwc) { // residual from a previous run
        xfer(r.madr, r.qwc, (r.madr & 0x80000000u) != 0);
        r.madr += r.qwc * 16;
        r.qwc = 0;
    }
    int asp = (int)((r.chcr >> 4) & 3);
    for (int guard = 0; guard < 1 << 20; ++guard) {
        Reg128 t;
        const bool tagSpr = (r.tadr & 0x80000000u) != 0;
        if (tagSpr) std::memcpy(&t, m.spr + (r.tadr & (SPR_SIZE - 1)), 16);
        else m.read128(r.tadr, &t);
        const uint32_t qwc = t.u32[0] & 0xFFFF;
        const uint32_t id = (t.u32[0] >> 28) & 7;
        const bool irq = (t.u32[0] >> 31) & 1;
        const uint32_t addr = t.u32[1] & 0x7FFFFFF0u;
        const bool spr = (t.u32[1] >> 31) & 1;
        r.chcr = (r.chcr & 0xFFFF) | (t.u32[0] & 0xFFFF0000u);
        if (r.chcr & 0x40) tag(t); // TTE
        bool end = false;
        switch (id) {
        case 0: // refe
            xfer(addr, qwc, spr);
            r.madr = addr + qwc * 16;
            r.tadr += 16;
            end = true;
            break;
        case 1: // cnt
            xfer(r.tadr + 16, qwc, tagSpr);
            r.madr = r.tadr + 16 + qwc * 16;
            r.tadr = r.madr;
            break;
        case 2: // next
            xfer(r.tadr + 16, qwc, tagSpr);
            r.madr = r.tadr + 16 + qwc * 16;
            r.tadr = addr;
            break;
        case 3: // ref
        case 4: // refs
            xfer(addr, qwc, spr);
            r.madr = addr + qwc * 16;
            r.tadr += 16;
            break;
        case 5: { // call
            xfer(r.tadr + 16, qwc, tagSpr);
            const uint32_t ret = r.tadr + 16 + qwc * 16;
            if (asp == 0) r.asr0 = ret;
            else r.asr1 = ret;
            asp = std::min(asp + 1, 2);
            r.madr = ret;
            r.tadr = addr;
            break;
        }
        case 6: // ret
            xfer(r.tadr + 16, qwc, tagSpr);
            r.madr = r.tadr + 16 + qwc * 16;
            if (asp > 0) {
                --asp;
                r.tadr = asp == 0 ? r.asr0 : r.asr1;
            } else {
                end = true;
            }
            break;
        case 7: // end
            xfer(r.tadr + 16, qwc, tagSpr);
            r.madr = r.tadr + 16 + qwc * 16;
            end = true;
            break;
        }
        if (irq && (r.chcr & 0x80)) end = true; // TIE
        if (end) break;
    }
    r.chcr = (r.chcr & ~0x30u) | ((uint32_t)asp << 4);
}

void Dmac::transferGifVif(int c) {
    DmaRegs& r = ch[c];
    int64_t cost = 0;
    if (c == DMA_GIF) {
        std::vector<uint8_t> buf;
        walkSource(
            m_, r, [&](const Reg128& q) { buf.insert(buf.end(), q.u8, q.u8 + 16); }, [&](const Reg128&) {}, cost);
        if (!buf.empty()) m_.gs->gifTransfer(buf.data(), buf.size() / 16, 3);
    } else {
        const int v = c == DMA_VIF0 ? 0 : 1;
        std::vector<uint32_t> words;
        walkSource(
            m_, r, [&](const Reg128& q) { words.insert(words.end(), q.u32, q.u32 + 4); },
            [&](const Reg128& t) { words.push_back(t.u32[2]); words.push_back(t.u32[3]); }, cost);
        if (!words.empty()) m_.vif->feed(v, words.data(), words.size());
    }
    const int ch_ = c;
    m_.schedule(m_.cycles + 8 + cost, [this, ch_]() { complete(ch_); });
}

void Dmac::transferToSpr(int c) {
    DmaRegs& r = ch[c];
    int64_t cost = 0;
    uint32_t sadr = r.sadr;
    const uint32_t mod = (r.chcr >> 2) & 3;
    if (mod == 2) { // interleave: TQWC qwords transferred, SQWC qwords skipped
        const uint32_t tq = (sqwc >> 16) & 0xFF, sq = sqwc & 0xFF;
        uint32_t inBlock = 0;
        while (r.qwc) {
            Reg128 q;
            m_.read128(r.madr, &q);
            std::memcpy(m_.spr + (sadr & (SPR_SIZE - 1)), &q, 16);
            sadr = (sadr + 16) & (SPR_SIZE - 1);
            r.madr += 16;
            --r.qwc;
            ++cost;
            if (tq && ++inBlock == tq) {
                inBlock = 0;
                r.madr += sq * 16;
            }
        }
    } else {
        walkSource(
            m_, r,
            [&](const Reg128& q) {
                std::memcpy(m_.spr + (sadr & (SPR_SIZE - 1)), &q, 16);
                sadr = (sadr + 16) & (SPR_SIZE - 1);
            },
            [&](const Reg128&) {}, cost);
    }
    r.sadr = sadr;
    const int ch_ = c;
    m_.schedule(m_.cycles + 8 + cost, [this, ch_]() { complete(ch_); });
}

void Dmac::transferFromSpr(int c) {
    DmaRegs& r = ch[c];
    const uint32_t mod = (r.chcr >> 2) & 3;
    const uint32_t tq = (sqwc >> 16) & 0xFF, sq = sqwc & 0xFF;
    uint32_t inBlock = 0;
    int64_t cost = 0;
    while (r.qwc) {
        Reg128 q;
        std::memcpy(&q, m_.spr + (r.sadr & (SPR_SIZE - 1)), 16);
        m_.write128(r.madr, &q);
        r.madr += 16;
        r.sadr = (r.sadr + 16) & (SPR_SIZE - 1);
        --r.qwc;
        ++cost;
        if (mod == 2 && tq && ++inBlock == tq) {
            inBlock = 0;
            r.madr += sq * 16;
        }
    }
    const int ch_ = c;
    m_.schedule(m_.cycles + 8 + cost, [this, ch_]() { complete(ch_); });
}

void Dmac::transferSif1() {
    DmaRegs& r = ch[DMA_SIF1];
    logWarn("dmac: direct SIF1 DMA (bypassing sceSifSetDma) is not supported");
    r.qwc = 0;
    m_.schedule(m_.cycles + 64, [this]() { complete(DMA_SIF1); });
}

void Dmac::runNormalOrChain(int) {}

// ------------------------------------------------------------------ timers

int64_t Timers::divider(int i) const {
    switch (t[i].mode & 3) {
    case 0: return 2;           // bus clock
    case 1: return 2 * 16;
    case 2: return 2 * 256;
    default: return 18743;      // HBLANK
    }
}

uint32_t Timers::count(int i) const {
    const T& x = t[i];
    if (!(x.mode & 0x80)) return x.countBase & 0xFFFF;
    return (uint32_t)((x.countBase + (m_.cycles - x.baseCycle) / divider(i)) & 0xFFFF);
}

uint32_t Timers::read(uint32_t p) {
    const int i = (int)((p >> 11) & 3);
    switch (p & 0x30) {
    case 0x00: return count(i);
    case 0x10: return t[i].mode;
    case 0x20: return t[i].comp;
    case 0x30: return t[i].hold;
    }
    return 0;
}

void Timers::reschedule(int i) {
    T& x = t[i];
    if (x.evt) {
        m_.cancel(x.evt);
        x.evt = 0;
    }
    if (!(x.mode & 0x80) || !(x.mode & 0x300)) return;
    const uint32_t cur = count(i);
    uint32_t target;
    if ((x.mode & 0x100) && x.comp > cur) target = x.comp;
    else target = 0x10000;
    const int64_t when = m_.cycles + (int64_t)(target - cur) * divider(i);
    x.evt = m_.schedule(when, [this, i]() {
        T& y = t[i];
        y.evt = 0;
        const uint32_t c = count(i);
        bool irq = false;
        if ((y.mode & 0x100) && c >= y.comp && !(y.mode & 0x400)) {
            y.mode |= 0x400; // EQUF
            irq = true;
            if (y.mode & 0x40) { // ZRET
                y.countBase = 0;
                y.baseCycle = m_.cycles;
            }
        }
        if (count(i) >= 0xFFFF || (!(y.mode & 0x100) && (y.mode & 0x200))) {
            y.mode |= 0x800; // OVFF
            y.countBase = 0;
            y.baseCycle = m_.cycles;
            irq = true;
        }
        if (irq) m_.intc->raise(INTC_TIM0 + i);
        reschedule(i);
    });
}

void Timers::write(uint32_t p, uint32_t v) {
    const int i = (int)((p >> 11) & 3);
    T& x = t[i];
    switch (p & 0x30) {
    case 0x00:
        x.countBase = v & 0xFFFF;
        x.baseCycle = m_.cycles;
        break;
    case 0x10: {
        const uint32_t cur = count(i);
        x.mode = (x.mode & 0xC00 & ~(v & 0xC00)) | (v & 0x3FF);
        x.countBase = cur;
        x.baseCycle = m_.cycles;
        break;
    }
    case 0x20: x.comp = v & 0xFFFF; break;
    case 0x30: x.hold = v & 0xFFFF; break;
    }
    reschedule(i);
}

void Timers::onHblank() {}

} // namespace ps2
