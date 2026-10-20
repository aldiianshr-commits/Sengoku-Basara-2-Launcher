#include "ps2/sif.hpp"

#include <algorithm>
#include <cstring>

#include "ps2/hw.hpp"
#include "ps2/iop.hpp"
#include "ps2/log.hpp"
#include "ps2/machine.hpp"

namespace ps2 {

namespace {
constexpr uint32_t SIF_DMA_INT_O = 0x04;
constexpr uint32_t SIF_STAT_READY = 0x10000 | 0x20000 | 0x40000; // SIFINIT | CMDINIT | BOOTEND
uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
void put32(std::vector<uint8_t>& v, size_t off, uint32_t x) {
    if (v.size() < off + 4) v.resize(off + 4);
    std::memcpy(v.data() + off, &x, 4);
}
} // namespace

Sif::Sif(Machine& m) : m_(m) {}
Sif::~Sif() = default;

void Sif::registerServer(uint32_t sid, std::shared_ptr<RpcServer> s) {
    servers_[sid] = std::move(s);
    // Fake IOP-side server descriptor and receive buffer.
    serverAddr_[sid] = iopAlloc(0x80);
    serverBuf_[sid] = iopAlloc(0x10000);
}

RpcServer* Sif::server(uint32_t sid) {
    auto it = servers_.find(sid);
    return it == servers_.end() ? nullptr : it->second.get();
}

uint32_t Sif::iopAlloc(uint32_t size) {
    size = (size + 63) & ~63u;
    if (heapNext_ + size > IOP_RAM_SIZE) {
        logError("sif: IOP heap exhausted");
        return 0;
    }
    uint32_t a = heapNext_;
    heapNext_ += size;
    heapBlocks_[a] = size;
    return a;
}

uint32_t Sif::iopFree(uint32_t addr) {
    heapBlocks_.erase(addr);
    return 0;
}

uint32_t Sif::iopHeapFree() const { return heapNext_ < IOP_RAM_SIZE ? IOP_RAM_SIZE - heapNext_ : 0; }

void Sif::onSetGsCrt(uint32_t interlace, uint32_t mode, uint32_t field) {
    PS2_DEBUG(Gs, "SetGsCrt interlace=%u mode=%u field=%u", interlace, mode, field);
    m_.palMode = (mode == 3);
}

// ------------------------------------------------------------------ registers

void Sif::setReg(uint32_t reg, uint32_t val) {
    if (reg & 0x80000000u) {
        sysRegs_[reg & 7] = val;
        return;
    }
    switch (reg) {
    case 1: regs_[1] = val; break;          // MAINADDR
    case 2: regs_[2] = val; break;          // SUBADDR (IOP owned)
    case 3: msflg_ |= val; break;           // MSFLAG: EE sets bits
    case 4: smflg_ &= ~val; break;          // SMFLAG: EE clears bits
    default: regs_[reg & 7] = val; break;
    }
}

uint32_t Sif::getReg(uint32_t reg) {
    if (reg & 0x80000000u) return sysRegs_[reg & 7];
    switch (reg) {
    case 1: return regs_[1];
    case 2: return kIopCmdBuf;
    case 3: return msflg_;
    case 4: return SIF_STAT_READY | smflg_;
    default: return regs_[reg & 7];
    }
}

uint32_t Sif::mmioRead(uint32_t phys) {
    switch (phys) {
    case 0x1000F200: return mscom_;
    case 0x1000F210: return kIopCmdBuf;
    case 0x1000F220: return msflg_;
    case 0x1000F230: return SIF_STAT_READY | smflg_;
    case 0x1000F240: return ctrl_;
    case 0x1000F260: return bd6_;
    }
    return 0;
}

void Sif::mmioWrite(uint32_t phys, uint32_t v) {
    switch (phys) {
    case 0x1000F200: mscom_ = v; break;
    case 0x1000F220: msflg_ |= v; break;
    case 0x1000F230: smflg_ &= ~v; break;
    case 0x1000F240: ctrl_ = v; break;
    case 0x1000F260: bd6_ = v; break;
    }
}

// ------------------------------------------------------------------ EE -> IOP

void Sif::dmaToIop(uint32_t eeSrc, uint32_t iopDest, uint32_t size, uint32_t attr) {
    iopDest &= IOP_RAM_SIZE - 1;
    size = (size + 3) & ~3u;
    for (uint32_t i = 0; i < size; i += 4) {
        const uint32_t v = m_.read32(eeSrc + i);
        std::memcpy(m_.iopRam + ((iopDest + i) & (IOP_RAM_SIZE - 1)), &v, 4);
    }
    PS2_DEBUG(Sif, "SIF1 dma ee=%08x -> iop=%08x size=%u attr=%x", eeSrc, iopDest, size, attr);
    if ((attr & SIF_DMA_INT_O) && iopDest == kIopCmdBuf) handleIopCommand(iopDest);
}

void Sif::handleIopCommand(uint32_t iopAddr) {
    uint32_t w[32];
    std::memcpy(w, m_.iopRam + iopAddr, sizeof w);
    const uint32_t cid = w[2];
    PS2_DEBUG(Sif, "IOP cmd cid=%08x psize=%u", cid, w[0] & 0xFF);
    switch (cid) {
    case SIF_CMD_INIT_CMD:
        if (w[3] == 0) {
            eePktBuf_ = w[4];
        } else {
            // sceSifInitRpc: reply with SET_SREG(SIF_SREG_RPCINIT = 1)
            std::vector<uint8_t> pkt(24, 0);
            put32(pkt, 0, 24);
            put32(pkt, 8, SIF_CMD_SET_SREG);
            put32(pkt, 16, 0);
            put32(pkt, 20, 1);
            sendToEe(std::move(pkt), 0, {});
        }
        break;
    case SIF_CMD_CHANGE_SADDR:
        eePktBuf_ = w[4];
        break;
    case SIF_CMD_SET_SREG:
        break;
    case SIF_CMD_RESET_CMD: {
        // sceSifResetIop / SifIopReboot: "rom0:UDNL <IOPRP image>".  The HLE
        // IOP keeps its native services; only the bookkeeping is reset.
        const uint32_t arglen = std::min<uint32_t>(w[4], 79);
        std::string arg;
        for (uint32_t i = 0; i < arglen; ++i) arg.push_back((char)m_.iopRam[(iopAddr + 24 + i) & (IOP_RAM_SIZE - 1)]);
        resetCount++;
        lastResetArg = arg;
        logInfo("sif: IOP reboot #%d requested (%s)", resetCount, arg.c_str());
        eePktBuf_ = 0;
        if (m_.iop) m_.iop->onReboot(arg);
        break;
    }
    case SIF_CMD_RPC_BIND:
        handleBind(w);
        break;
    case SIF_CMD_RPC_CALL:
        handleCall(w, iopAddr);
        break;
    case SIF_CMD_RPC_RDATA:
        handleRdata(w);
        break;
    default:
        logWarn("sif: unhandled IOP command %08x", cid);
        break;
    }
}

void Sif::handleBind(const uint32_t* w) {
    const uint32_t sid = w[8];
    std::vector<uint8_t> pkt(64, 0);
    put32(pkt, 0, 64);
    put32(pkt, 8, SIF_CMD_RPC_END);
    put32(pkt, 16, w[4]); // rec_id
    put32(pkt, 20, w[5]); // pkt_addr
    put32(pkt, 24, w[6]); // rpc_id
    put32(pkt, 28, w[7]); // cd
    put32(pkt, 32, SIF_CMD_RPC_BIND);
    if (servers_.count(sid)) {
        put32(pkt, 36, serverAddr_[sid]);
        put32(pkt, 40, serverBuf_[sid]);
        put32(pkt, 44, serverBuf_[sid] + 0x8000);
        PS2_DEBUG(Sif, "bind sid=%08x -> %s", sid, servers_[sid]->name());
    } else {
        static std::map<uint32_t, bool> warned;
        if (!warned[sid]) {
            logWarn("sif: bind to unknown RPC server %08x (IOP module not available)", sid);
            warned[sid] = true;
        }
    }
    m_.schedule(m_.cycles + 1000, [this, pkt]() mutable { sendToEe(std::move(pkt), 0, {}); });
}

void Sif::handleCall(const uint32_t* w, uint32_t) {
    const uint32_t fno = w[8], ssize = w[9], recvbuf = w[10], rsize = w[11], rmode = w[12], sd = w[13];
    uint32_t sid = 0;
    for (auto& [s, a] : serverAddr_)
        if (a == sd) sid = s;
    RpcServer* srv = server(sid);
    std::vector<uint8_t> reply;
    int64_t latency = 1000;
    std::function<void()> complete;
    if (!srv) {
        logWarn("sif: RPC call to unknown server descriptor %08x fno=%u", sd, fno);
    } else {
        std::vector<uint8_t> in(ssize);
        const uint32_t buf = serverBuf_[sid];
        for (uint32_t i = 0; i < ssize; ++i) in[i] = m_.iopRam[(buf + i) & (IOP_RAM_SIZE - 1)];
        rpcCallCount[sid]++;
        srv->nextLatency = -1;
        srv->onComplete = nullptr;
        reply = srv->call(fno, in, rsize);
        latency = srv->nextLatency >= 0 ? srv->nextLatency : srv->latency(fno);
        complete = std::move(srv->onComplete);
        srv->onComplete = nullptr;
    }
    if (reply.size() > rsize) reply.resize(rsize);
    std::vector<uint8_t> pkt(64, 0);
    put32(pkt, 0, 64);
    put32(pkt, 8, SIF_CMD_RPC_END);
    put32(pkt, 16, w[4]);
    put32(pkt, 20, w[5]);
    put32(pkt, 24, w[6]);
    put32(pkt, 28, w[7]);
    put32(pkt, 32, SIF_CMD_RPC_CALL);
    put32(pkt, 36, sd);
    if (latency == INT64_MAX) return; // the server answers later (or never)
    const uint32_t dest = recvbuf;
    const uint32_t eePkt = w[5];
    if (rmode == 0) {
        // No completion packet requested (NOWAIT without end function): the
        // IOP DMAs the reply and writes the call packet back with rpc_id and
        // rec_id cleared, which returns it to the EE's packet pool.
        m_.schedule(m_.cycles + latency, [this, dest, reply, eePkt, complete]() {
            if (complete) complete();
            for (size_t i = 0; i < reply.size(); ++i) m_.write8(dest + (uint32_t)i, reply[i]);
            if (eePkt) {
                m_.write32(eePkt + 16, 0); // rec_id
                m_.write32(eePkt + 24, 0); // rpc_id
            }
        });
        return;
    }
    m_.schedule(m_.cycles + latency, [this, pkt, dest, reply, complete]() mutable {
        if (complete) complete();
        sendToEe(std::move(pkt), dest, std::move(reply));
    });
}

void Sif::handleRdata(const uint32_t* w) {
    const uint32_t src = w[8], dest = w[9], size = w[10];
    std::vector<uint8_t> data(size);
    for (uint32_t i = 0; i < size; ++i) data[i] = m_.iopRam[(src + i) & (IOP_RAM_SIZE - 1)];
    std::vector<uint8_t> pkt(64, 0);
    put32(pkt, 0, 64);
    put32(pkt, 8, SIF_CMD_RPC_END);
    put32(pkt, 16, w[4]);
    put32(pkt, 20, w[5]);
    put32(pkt, 24, w[6]);
    put32(pkt, 28, w[7]);
    put32(pkt, 32, SIF_CMD_RPC_RDATA);
    m_.schedule(m_.cycles + 1000, [this, pkt, dest, data]() mutable {
        sendToEe(std::move(pkt), dest, std::move(data));
    });
}

// ------------------------------------------------------------------ IOP -> EE

void Sif::sendToEe(std::vector<uint8_t> pkt, uint32_t eeDest, std::vector<uint8_t> data) {
    toEe_.push_back({std::move(pkt), eeDest, std::move(data)});
    pump();
}

void Sif::setDChain() {
    m_.dmac->ch[DMA_SIF0].chcr |= 0x100;
    pump();
}

void Sif::pump() {
    if (pumping_) return;
    pumping_ = true;
    while (!toEe_.empty() && m_.dmac->sif0Armed()) {
        if (eePktBuf_ == 0) {
            logWarn("sif: IOP->EE packet but EE command buffer unknown");
            toEe_.pop_front();
            continue;
        }
        Pending p = std::move(toEe_.front());
        toEe_.pop_front();
        if (p.eeDest && !p.data.empty())
            for (size_t i = 0; i < p.data.size(); ++i) m_.write8(p.eeDest + (uint32_t)i, p.data[i]);
        // Header: psize | dsize<<8, dest, cid, opt
        uint32_t psize = (uint32_t)p.pkt.size();
        uint32_t hdr0 = (psize & 0xFF) | ((uint32_t)p.data.size() << 8);
        std::memcpy(p.pkt.data(), &hdr0, 4);
        uint32_t destField = p.eeDest;
        std::memcpy(p.pkt.data() + 4, &destField, 4);
        for (size_t i = 0; i < p.pkt.size(); ++i) m_.write8(eePktBuf_ + (uint32_t)i, p.pkt[i]);
        PS2_DEBUG(Sif, "SIF0 -> EE pkt cid=%08x to %08x", rd32(p.pkt.data() + 8), eePktBuf_);
        m_.dmac->ch[DMA_SIF0].chcr &= ~0x100u;
        m_.dmac->complete(DMA_SIF0);
    }
    pumping_ = false;
}

} // namespace ps2
