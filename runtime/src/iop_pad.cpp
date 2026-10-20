// PADMAN (libpad) service.
//
// The EE library opens a pad by passing a 256-byte area; padman then writes
// the controller state into that area every vertical blank, double buffered
// with a frame counter (the EE reads the buffer with the larger counter).
// Both protocol generations are served:
//   new  (XPADMAN / padman >= 1.4): RPC 0x80000100/0x80000101, 2 x 128-byte records
//   old  (rom0:PADMAN):             RPC 0x8000010F/0x8000011F, 2 x 64-byte records
#include <algorithm>
#include <array>

#include "iop_services.hpp"
#include "ps2/log.hpp"
#include "ps2/machine.hpp"
#include "ps2/pad.hpp"
#include "ps2/sif.hpp"

namespace ps2 {

using namespace hle;

namespace {

constexpr uint8_t STATE_DISCONN = 0x00, STATE_STABLE = 0x06;
constexpr uint8_t RSTAT_COMPLETE = 0x00, RSTAT_BUSY = 0x02;

struct PadSlot {
    bool open = false;
    int protocol = 2; // 2 = new, 1 = old
    uint32_t area = 0;
    uint32_t frame = 0;
    uint8_t modeId = 0x41; // 0x41 digital, 0x73 analog, 0x79 analog + pressure
    bool locked = false;
    bool pressure = false;
    uint8_t reqState = RSTAT_COMPLETE;
    int busyFrames = 0;
    std::array<uint8_t, 6> actAlign{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    std::array<uint8_t, 6> actDirect{};
};

class PadService {
public:
    explicit PadService(Machine& m) : m_(m) {}

    std::vector<uint8_t> call(int protocol, const std::vector<uint8_t>& in) {
        std::vector<uint8_t> out = in;
        out.resize(128, 0);
        uint32_t cmd = rd32(in, 0);
        // Map old command numbers onto the new ones.
        if (protocol == 1) {
            switch (cmd) {
            case 0x80000100: cmd = 0x01; break;
            case 0x80000102: cmd = 0x102; break; // INFO_ACT (old: via RPC)
            case 0x80000103: cmd = 0x103; break; // INFO_COMB
            case 0x80000104: cmd = 0x104; break; // INFO_MODE (old: via RPC)
            case 0x80000105: cmd = 0x06; break;
            case 0x80000106: cmd = 0x07; break;
            case 0x80000107: cmd = 0x08; break;
            case 0x80000108: cmd = 0x09; break;
            case 0x80000109: cmd = 0x0A; break;
            case 0x8000010A: cmd = 0x0B; break;
            case 0x8000010B: cmd = 0x0C; break;
            case 0x8000010C: cmd = 0x0D; break;
            case 0x8000010D: cmd = 0x0E; break;
            case 0x8000010E: cmd = 0x0F; break;
            default: break;
            }
        }
        const int port = (int)rd32(in, 4), slot = (int)rd32(in, 8);
        PadSlot* ps = (port >= 0 && port < 2 && slot >= 0 && slot < 4) ? &slots_[port][slot] : nullptr;
        switch (cmd) {
        case 0x01: { // OPEN {cmd, port, slot, ?, padArea} -> result @12, padBuf @20
            if (!ps) {
                wr32(out, 12, 0);
                break;
            }
            ps->open = true;
            ps->protocol = protocol;
            ps->area = rd32(in, 16);
            ps->frame = 0;
            ps->reqState = RSTAT_COMPLETE;
            ps->busyFrames = 0;
            wr32(out, 12, 1);
            wr32(out, 20, 0x000F0000u + (uint32_t)(port * 0x400 + slot * 0x100)); // IOP-side buffer
            PS2_DEBUG(Pad, "padPortOpen(%d,%d) area=%08x protocol=%d", port, slot, ps->area, protocol);
            update(port, slot); // make data available immediately
            break;
        }
        case 0x0E: // CLOSE
            if (ps) ps->open = false;
            wr32(out, 12, 1);
            break;
        case 0x0F: // END
            for (auto& p : slots_)
                for (auto& s : p) s.open = false;
            wr32(out, 12, 1);
            break;
        case 0x10: // INIT {cmd, ?, ?, ?, statBuf}
            statBuf_ = rd32(in, 16);
            wr32(out, 12, 1);
            break;
        case 0x12: wr32(out, 12, 0x0400); break; // GET_MODVER
        case 0x0C: wr32(out, 12, 2); break;      // GET_PORTMAX
        case 0x0D: wr32(out, 12, 1); break;      // GET_SLOTMAX (no multitap)
        case 0x06: { // SET_MMODE {cmd, port, slot, mode, lock} -> result @20
            if (ps && slot == 0) {
                const int mode = (int)rd32(in, 12), lock = (int)rd32(in, 16);
                ps->modeId = mode == 1 ? (ps->pressure ? 0x79 : 0x73) : 0x41;
                if (mode != 1) ps->pressure = false;
                ps->locked = lock == 3;
                setBusy(*ps);
                wr32(out, 20, 1);
            } else {
                wr32(out, 20, 0);
            }
            break;
        }
        case 0x09: wr32(out, 12, 0x3FFFF); break; // GET_BTNMASK: all buttons pressure sensitive
        case 0x0A: { // SET_BTNINFO {cmd, port, slot, info} -> result @16
            if (ps) {
                ps->pressure = rd32(in, 12) != 0;
                if (ps->modeId != 0x41) ps->modeId = ps->pressure ? 0x79 : 0x73;
                setBusy(*ps);
            }
            wr32(out, 16, ps ? 1 : 0);
            break;
        }
        case 0x0B: wr32(out, 16, 1); break; // SET_VREF
        case 0x08: { // SET_ACTALIGN {cmd, port, slot, align[6]} -> result @20
            if (ps) {
                for (int i = 0; i < 6; ++i) ps->actAlign[(size_t)i] = in.size() > (size_t)(12 + i) ? in[(size_t)(12 + i)] : 0xFF;
                setBusy(*ps);
            }
            wr32(out, 20, ps ? 1 : 0);
            break;
        }
        case 0x07: { // SET_ACTDIR {cmd, port, slot, values[6]} -> result @20
            if (ps) {
                for (int i = 0; i < 6; ++i) ps->actDirect[(size_t)i] = in.size() > (size_t)(12 + i) ? in[(size_t)(12 + i)] : 0;
                applyMotors(port, *ps);
            }
            wr32(out, 20, ps ? 1 : 0);
            break;
        }
        case 0x104: { // old INFO_MODE {cmd, port, slot, infoMode, index} -> result @20
            int r = 0;
            if (ps) {
                const int info = (int)rd32(in, 12), index = (int)rd32(in, 16);
                if (info == 1) r = ps->modeId >> 4;           // PAD_MODECURID
                else if (info == 2) r = ps->modeId == 0x41 ? 4 : 7; // PAD_MODECUREXID
                else if (info == 3) r = ps->modeId == 0x41 ? 0 : 1; // PAD_MODECUROFFS
                else if (info == 4) r = index == -1 ? 2 : (index == 0 ? 4 : (index == 1 ? 7 : 0)); // PAD_MODETABLE
            }
            wr32(out, 20, (uint32_t)r);
            break;
        }
        case 0x102: { // old INFO_ACT {cmd, port, slot, actuator, cmd} -> result @20
            const int act = (int)rd32(in, 12), acmd = (int)rd32(in, 16);
            static const uint8_t kAct[2][4] = {{0, 1, 1, 1}, {0, 2, 1, 1}};
            int r = 0;
            if (act == -1) r = 2;
            else if (act >= 0 && act < 2 && acmd >= 0 && acmd < 4) r = kAct[act][acmd];
            wr32(out, 20, (uint32_t)r);
            break;
        }
        case 0x103: wr32(out, 20, 1); break; // old INFO_COMB
        default:
            logWarn("pad: unsupported command %08x", rd32(in, 0));
            wr32(out, 12, 0);
            break;
        }
        return out;
    }

    void vblank() {
        ++globalFrame_;
        for (int p = 0; p < 2; ++p)
            for (int s = 0; s < 4; ++s)
                if (slots_[p][s].open) update(p, s);
        if (statBuf_) {
            // open_slot {frame, openSlots[2], padding} x2, double buffered.
            const uint32_t base = statBuf_ + (globalFrame_ & 1) * 128;
            m_.write32(base, globalFrame_);
            for (int p = 0; p < 2; ++p) {
                uint32_t bits = 0;
                for (int s = 0; s < 4; ++s)
                    if (slots_[p][s].open) bits |= 1u << s;
                m_.write32(base + 4 + (uint32_t)p * 4, bits);
            }
        }
    }

private:
    void setBusy(PadSlot& ps) {
        ps.reqState = RSTAT_BUSY;
        ps.busyFrames = 3;
    }

    void applyMotors(int port, const PadSlot& ps) {
        uint8_t small = 0, large = 0;
        for (int i = 0; i < 6; ++i) {
            if (ps.actAlign[(size_t)i] == 0) small = ps.actDirect[(size_t)i] ? 1 : 0;
            else if (ps.actAlign[(size_t)i] == 1) large = ps.actDirect[(size_t)i];
        }
        m_.pad->setMotors(port, small, large);
    }

    void update(int port, int slot) {
        PadSlot& ps = slots_[port][slot];
        if (ps.busyFrames > 0 && --ps.busyFrames == 0) ps.reqState = RSTAT_COMPLETE;
        const PadState st = m_.pad->state(port);
        const bool connected = slot == 0 && st.connected;
        ps.frame++;
        uint8_t data[32];
        std::memset(data, 0xFF, sizeof data);
        uint8_t mode = ps.modeId;
        int length = 0;
        if (connected) {
            const uint16_t inv = (uint16_t)~st.buttons;
            data[0] = 0;
            data[1] = mode;
            data[2] = (uint8_t)(inv & 0xFF);
            data[3] = (uint8_t)(inv >> 8);
            data[4] = st.rx;
            data[5] = st.ry;
            data[6] = st.lx;
            data[7] = st.ly;
            static const uint16_t kOrder[12] = {PAD_RIGHT, PAD_LEFT, PAD_UP, PAD_DOWN, PAD_TRIANGLE, PAD_CIRCLE,
                                                PAD_CROSS, PAD_SQUARE, PAD_L1, PAD_R1, PAD_L2, PAD_R2};
            for (int i = 0; i < 12; ++i)
                data[8 + i] = (st.buttons & kOrder[i]) ? (st.pressure[(size_t)i] ? st.pressure[(size_t)i] : 0xFF) : 0;
            for (int i = 20; i < 31; ++i) data[i] = 0;
            data[31] = 0x5A;
            length = (mode & 0x0F) * 2 + 2;
        }
        if (ps.protocol == 2) {
            const uint32_t b = ps.area + (ps.frame & 1) * 128;
            uint8_t rec[128] = {};
            std::memcpy(rec, data, 32);
            auto put32 = [&](int off, uint32_t v) { std::memcpy(rec + off, &v, 4); };
            auto put16 = [&](int off, uint16_t v) { std::memcpy(rec + off, &v, 2); };
            for (int i = 0; i < 6; ++i) {
                rec[32 + i] = ps.actDirect[(size_t)i]; // actDirData
                rec[40 + i] = ps.actAlign[(size_t)i];  // actAlignData
            }
            const uint8_t act[2][4] = {{0, 1, 1, 1}, {0, 2, 1, 1}};
            std::memcpy(rec + 48, act, sizeof act);
            put16(80, 4);
            put16(82, 7);
            put32(88, ps.frame);
            put32(92, 0);          // findPadRetries
            put32(96, (uint32_t)length);
            rec[100] = connected ? 2 : 0;          // modeConfig
            rec[101] = connected ? mode : 0;       // modeCurId
            rec[102] = connected ? 3 : 0;          // model: DualShock 2
            rec[103] = connected ? 1 : 0;          // buttonDataReady
            rec[104] = connected ? 2 : 0;          // nrOfModes
            rec[105] = mode == 0x41 ? 0 : 1;       // modeCurOffs
            rec[106] = connected ? 2 : 0;          // nrOfActuators
            rec[107] = connected ? 1 : 0;          // numActComb
            rec[109] = mode == 0x41 ? 0 : 1;       // mode
            rec[110] = ps.locked ? 3 : 2;          // lock
            rec[111] = 6;                          // actDirSize
            rec[112] = connected ? STATE_STABLE : STATE_DISCONN;
            rec[113] = ps.reqState;
            rec[114] = 1;                          // currentTask = update pad
            rec[115] = 0;                          // runTask = none
            if (uint8_t* p = m_.hostPtr(b, 128)) std::memcpy(p, rec, 128);
            else
                for (int i = 0; i < 128; ++i) m_.write8(b + (uint32_t)i, rec[i]);
        } else {
            const uint32_t b = ps.area + (ps.frame & 1) * 64;
            uint8_t rec[64] = {};
            std::memcpy(rec + 0, &ps.frame, 4);
            rec[4] = connected ? STATE_STABLE : STATE_DISCONN;
            rec[5] = ps.reqState;
            rec[6] = connected ? 1 : 0; // ok
            std::memcpy(rec + 8, data, 32);
            const uint32_t len = (uint32_t)length;
            std::memcpy(rec + 40, &len, 4);
            rec[45] = 2; // CTP: config available
            rec[46] = 3; // model
            rec[47] = 1; // correction
            if (uint8_t* p = m_.hostPtr(b, 64)) std::memcpy(p, rec, 64);
            else
                for (int i = 0; i < 64; ++i) m_.write8(b + (uint32_t)i, rec[i]);
        }
    }

    Machine& m_;
    PadSlot slots_[2][4];
    uint32_t statBuf_ = 0;
    uint32_t globalFrame_ = 0;
};

class PadServer final : public RpcServer {
public:
    PadServer(std::shared_ptr<PadService> svc, int protocol, const char* nm) : svc_(std::move(svc)), protocol_(protocol), name_(nm) {}
    const char* name() const override { return name_; }
    int64_t latency(uint32_t) const override { return 4000; }
    std::vector<uint8_t> call(uint32_t, const std::vector<uint8_t>& in, uint32_t) override { return svc_->call(protocol_, in); }

private:
    std::shared_ptr<PadService> svc_;
    int protocol_;
    const char* name_;
};

} // namespace

void registerPadServices(Machine& m) {
    auto svc = std::make_shared<PadService>(m);
    m.sif->registerServer(0x80000100, std::make_shared<PadServer>(svc, 2, "PADMAN"));
    m.sif->registerServer(0x80000101, std::make_shared<PadServer>(svc, 2, "PADMAN ext"));
    m.sif->registerServer(0x8000010F, std::make_shared<PadServer>(svc, 1, "PADMAN (rom0)"));
    m.sif->registerServer(0x8000011F, std::make_shared<PadServer>(svc, 1, "PADMAN (rom0) ext"));
    m.vblankHooks.push_back([svc]() { svc->vblank(); });
}

} // namespace ps2
