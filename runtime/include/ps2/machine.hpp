// The emulated PS2 system state used by recompiled code.  Hardware blocks
// (GS, VIF, VU, SPU2, IOP services) live in their own classes owned here.
#pragma once
#include <cstdint>
#include <atomic>
#include <functional>
#include <memory>
#include <queue>
#include <string>
#include <vector>

#include "context.hpp"
#include "fiber.hpp"

namespace ps2 {

constexpr uint32_t RAM_SIZE = 32u * 1024 * 1024;
constexpr uint32_t SPR_SIZE = 16u * 1024;
constexpr uint32_t IOP_RAM_SIZE = 2u * 1024 * 1024;

// Pseudo addresses used as return targets for code invoked by the runtime.
// They lie in the (absent) BIOS ROM, where no recompiled code exists.
constexpr uint32_t MAGIC_BASE = 0xBFC0F000u;
constexpr uint32_t MAGIC_THREAD_EXIT = 0xBFC0F000u;
constexpr uint32_t MAGIC_HANDLER_RETURN = 0xBFC0F010u;
constexpr uint32_t MAGIC_CALLBACK_RETURN = 0xBFC0F020u;
inline bool isMagic(uint32_t a) { return (a & 0xFFFFF000u) == MAGIC_BASE; }

// EE clock and video timing.
constexpr int64_t EE_CLOCK = 294912000;
constexpr int64_t BUS_CLOCK = EE_CLOCK / 2;

class Kernel;
class Sif;
class Iop;
class Dmac;
class Intc;
class Timers;
class Gs;
class Gif;
class Vif;
class Vu;
class Ipu;
class Spu2;
class Pad;
class Cdvd;
struct Config;

struct Event {
    int64_t time;
    uint64_t seq;
    std::function<void()> fn;
    bool operator>(const Event& o) const { return time != o.time ? time > o.time : seq > o.seq; }
};

struct RuntimeConfig {
    std::string elfPath;          // EE executable being run
    std::string bootPath;         // argv[0] passed to the game (e.g. cdrom0:\SLPM_123.45;1)
    std::string discImage;        // ISO / BIN image backing cdrom0:
    std::string hostRoot;         // directory backing host0:
    std::string saveDir;          // memory card storage
    double cyclesPerInstruction = 2.0;
    bool realTimePacing = true;   // throttle to video rate
    bool headless = false;        // no window (tests)
    uint64_t maxFrames = 0;       // stop after N vsyncs (0 = unlimited)
    uint64_t maxCycles = 0;       // stop after N EE cycles (0 = unlimited)
    std::string missingEntriesFile;
};

class Machine {
public:
    explicit Machine(const ModuleInfo* mod, const RuntimeConfig& cfg);
    ~Machine();
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;

    // Load the executable segments and run until the program exits.
    int run();
    void requestExit(int code);
    bool exiting() const { return exitRequested_.load(std::memory_order_relaxed); }
    int exitCode() const { return exitCode_; }

    // ---- memory
    uint8_t* ram = nullptr;
    uint8_t* spr = nullptr;
    uint8_t* iopRam = nullptr;
    uint8_t* vu0Code = nullptr; // 4 KB
    uint8_t* vu0Data = nullptr; // 4 KB
    uint8_t* vu1Code = nullptr; // 16 KB
    uint8_t* vu1Data = nullptr; // 16 KB
    std::vector<uint8_t*> rpage, wpage;
    // Translate an EE virtual address to host memory (RAM/SPR/VU) or nullptr.
    uint8_t* hostPtr(uint32_t addr, uint32_t len = 1) const;
    uint32_t ramPhys(uint32_t addr) const { return addr & (RAM_SIZE - 1); }
    uint8_t read8(uint32_t a);
    uint16_t read16(uint32_t a);
    uint32_t read32(uint32_t a);
    uint64_t read64(uint32_t a);
    void write8(uint32_t a, uint8_t v);
    void write16(uint32_t a, uint16_t v);
    void write32(uint32_t a, uint32_t v);
    void write64(uint32_t a, uint64_t v);
    void read128(uint32_t a, Reg128* out);
    void write128(uint32_t a, const Reg128* v);
    std::string readCString(uint32_t a, size_t max = 1024);

    // ---- MMIO (hw.cpp)
    uint32_t mmioRead32(uint32_t phys);
    void mmioWrite32(uint32_t phys, uint32_t v);
    uint64_t mmioRead64(uint32_t phys);
    void mmioWrite64(uint32_t phys, uint64_t v);
    void mmioRead128(uint32_t phys, Reg128* out);
    void mmioWrite128(uint32_t phys, const Reg128* v);

    // ---- code
    const ModuleInfo* module() const { return mod_; }
    RecompFn lookup(uint32_t pc, const FuncEntry** entry = nullptr) const;
    // Run code at c->pc until control reaches `stopAt` (a magic address).
    void runUntil(Ctx* c, uint32_t stopAt);
    void reportMissingEntry(uint32_t pc);

    // ---- time / events
    int64_t cycles = 0;
    int64_t nextEvent = INT64_MAX;
    uint64_t schedule(int64_t when, std::function<void()> fn);
    void cancel(uint64_t id);
    void processDueEvents();
    void syncFrom(Ctx* c) { cycles = c->cycles; }
    void syncTo(Ctx* c) { c->cycles = cycles; c->nextEvent = &nextEvent; }
    void initCtx(Ctx* c);

    // ---- interrupts (raised by hardware blocks, delivered at safe points)
    uint32_t cop0[32] = {};
    bool interruptsEnabled() const;
    bool interruptPending() const;
    void checkInterrupts(Ctx* current); // deliver pending interrupts now if possible

    // ---- subsystems
    std::unique_ptr<Kernel> kernel;
    std::unique_ptr<Intc> intc;
    std::unique_ptr<Dmac> dmac;
    std::unique_ptr<Timers> timers;
    std::unique_ptr<Sif> sif;
    std::unique_ptr<Iop> iop;
    std::unique_ptr<Gs> gs;
    std::unique_ptr<Gif> gif;
    std::unique_ptr<Vif> vif;
    std::unique_ptr<Vu> vu;
    std::unique_ptr<Spu2> spu2;
    std::unique_ptr<Ipu> ipu;
    std::unique_ptr<Pad> pad;
    std::unique_ptr<Cdvd> cdvd;

    RuntimeConfig cfg;
    // Console output of the game (stdout of fileio, SIO, kernel print).
    std::function<void(const char*, size_t)> consoleOut;
    void console(const char* s, size_t n);
    // Called on the emulation thread at the start of every vertical blank,
    // after the frame has been handed to Gs::onFrame (pacing, pause, audio).
    std::function<void()> onVsync;
    // Runtime services that run once per vertical blank (IOP pad updates...).
    std::vector<std::function<void()>> vblankHooks;
    // Video standard selected by the game (SetGsCrt): true = PAL (50 Hz).
    bool palMode = false;

    // Statistics
    uint64_t frames = 0;
    uint64_t missingEntryCount = 0;

private:
    void setupMemory();
    void loadElf();
    const ModuleInfo* mod_;
    std::priority_queue<Event, std::vector<Event>, std::greater<Event>> events_;
    std::vector<uint64_t> cancelled_;
    uint64_t eventSeq_ = 1;
    std::atomic<bool> exitRequested_{false};
    int exitCode_ = 0;
    std::string consoleLine_;
};

// Thrown to unwind recompiled code when the program exits or hits a fatal
// condition; caught at fiber / scheduler boundaries.
struct GuestExit {
    int code;
};

// The machine the current host thread is running (one per process).
Machine* currentMachine();

} // namespace ps2
