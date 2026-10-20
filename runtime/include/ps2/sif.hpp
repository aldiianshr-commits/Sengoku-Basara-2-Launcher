// SIF (EE <-> IOP interface) and the high-level IOP.
//
// The IOP is not emulated as a CPU.  Instead the SIF command protocol (the
// packet format shared by Sony's libraries and ps2sdk) is implemented and
// RPC servers for the standard IOP modules are provided natively:
// FILEIO, CDVD, PAD, MCSERV, LIBSD, LOADFILE, IOP heap.
#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ps2 {

class Machine;

constexpr uint32_t SIF_CMD_CHANGE_SADDR = 0x80000000;
constexpr uint32_t SIF_CMD_SET_SREG = 0x80000001;
constexpr uint32_t SIF_CMD_INIT_CMD = 0x80000002;
constexpr uint32_t SIF_CMD_RESET_CMD = 0x80000003;
constexpr uint32_t SIF_CMD_RPC_END = 0x80000008;
constexpr uint32_t SIF_CMD_RPC_BIND = 0x80000009;
constexpr uint32_t SIF_CMD_RPC_CALL = 0x8000000A;
constexpr uint32_t SIF_CMD_RPC_RDATA = 0x8000000C;

// An RPC server implemented natively.  `call` receives the data the EE sent
// (already copied out of IOP memory) and returns the reply bytes; the SIF
// layer copies at most recvSize bytes of it to the EE receive buffer.
class RpcServer {
public:
    virtual ~RpcServer() = default;
    virtual const char* name() const = 0;
    virtual std::vector<uint8_t> call(uint32_t fno, const std::vector<uint8_t>& in, uint32_t recvSize) = 0;
    // Delay (EE cycles) before the reply is delivered (models IOP latency).
    virtual int64_t latency(uint32_t fno) const { (void)fno; return 2000; }
};

class Sif {
public:
    explicit Sif(Machine& m);
    ~Sif();

    // EE -> IOP DMA (SIF1) with sceSifSetDma semantics.
    void dmaToIop(uint32_t eeSrc, uint32_t iopDest, uint32_t size, uint32_t attr);
    // EE re-arms SIF0 (sceSifSetDChain).
    void setDChain();
    void setReg(uint32_t reg, uint32_t val);
    uint32_t getReg(uint32_t reg);
    uint32_t mmioRead(uint32_t phys);
    void mmioWrite(uint32_t phys, uint32_t v);
    void onSetGsCrt(uint32_t interlace, uint32_t mode, uint32_t field);

    void registerServer(uint32_t sid, std::shared_ptr<RpcServer> s);
    RpcServer* server(uint32_t sid);
    // Send a packet IOP -> EE (queued until SIF0 is armed).  `data` is
    // copied to eeDest before the packet is delivered.
    void sendToEe(std::vector<uint8_t> pkt, uint32_t eeDest, std::vector<uint8_t> data);
    void pump(); // deliver queued packets if possible

    uint32_t iopCmdBuffer() const { return kIopCmdBuf; }
    uint32_t iopAlloc(uint32_t size); // simple IOP heap
    uint32_t iopFree(uint32_t addr);

    // statistics / debug
    std::map<uint32_t, uint32_t> rpcCallCount;

private:
    void handleIopCommand(uint32_t iopAddr);
    void handleBind(const uint32_t* w);
    void handleCall(const uint32_t* w, uint32_t pktAddr);
    void handleRdata(const uint32_t* w);

    static constexpr uint32_t kIopCmdBuf = 0x0001F000;
    struct Pending {
        std::vector<uint8_t> pkt;
        uint32_t eeDest = 0;
        std::vector<uint8_t> data;
    };
    Machine& m_;
    std::deque<Pending> toEe_;
    uint32_t eePktBuf_ = 0;
    uint32_t regs_[8] = {};      // MAINADDR, SUBADDR, MSFLAG, SMFLAG ...
    uint32_t sysRegs_[8] = {};   // 0x80000000+ registers
    uint32_t mscom_ = 0, smcom_ = 0, msflg_ = 0, smflg_ = 0, ctrl_ = 0, bd6_ = 0;
    std::map<uint32_t, std::shared_ptr<RpcServer>> servers_;
    std::map<uint32_t, uint32_t> serverAddr_; // sid -> fake IOP server struct
    std::map<uint32_t, uint32_t> serverBuf_;  // sid -> IOP receive buffer
    uint32_t heapNext_ = 0x00100000;
    std::map<uint32_t, uint32_t> heapBlocks_;
    bool pumping_ = false;
};

} // namespace ps2
