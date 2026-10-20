#include "ps2/machine.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <unordered_set>

#include "ps2/cdvd.hpp"
#include "ps2/gs.hpp"
#include "ps2/hw.hpp"
#include "ps2/iop.hpp"
#include "ps2/ipu.hpp"
#include "ps2/kernel.hpp"
#include "ps2/log.hpp"
#include "ps2/pad.hpp"
#include "ps2/sif.hpp"
#include "ps2/spu2.hpp"
#include "ps2/vif.hpp"
#include "ps2/vu.hpp"
#include "sb2/elf_loader.hpp"

namespace ps2 {

namespace {
Machine* g_machine = nullptr;
std::unordered_set<uint64_t> g_cancelled;
} // namespace

Machine* currentMachine() { return g_machine; }

Machine::Machine(const ModuleInfo* mod, const RuntimeConfig& c) : cfg(c), mod_(mod) {
    g_machine = this;
    setupMemory();
    kernel = std::make_unique<Kernel>(*this);
    intc = std::make_unique<Intc>(*this);
    dmac = std::make_unique<Dmac>(*this);
    timers = std::make_unique<Timers>(*this);
    sif = std::make_unique<Sif>(*this);
    vu = std::make_unique<Vu>(*this);
    gs = std::make_unique<Gs>(*this);
    gif = std::make_unique<Gif>(*this);
    vif = std::make_unique<Vif>(*this);
    spu2 = std::make_unique<Spu2>(*this);
    ipu = std::make_unique<Ipu>(*this);
    pad = std::make_unique<Pad>(*this);
    cdvd = std::make_unique<Cdvd>(*this);
    iop = std::make_unique<Iop>(*this);
    // The SPU2 runs in emulated time (voice status registers must advance
    // even without an audio device); the front-end only consumes the output.
    vblankHooks.push_back([this]() { spu2->advanceField(palMode ? 50.0 : 59.94); });
    consoleOut = [](const char* s, size_t n) {
        std::fwrite(s, 1, n, stdout);
        std::fflush(stdout);
    };
}

Machine::~Machine() {
    iop.reset();
    cdvd.reset();
    pad.reset();
    spu2.reset();
    vif.reset();
    gif.reset();
    gs.reset();
    vu.reset();
    sif.reset();
    timers.reset();
    dmac.reset();
    intc.reset();
    kernel.reset();
    delete[] ram;
    delete[] spr;
    delete[] iopRam;
    delete[] vu0Code;
    delete[] vu0Data;
    delete[] vu1Code;
    delete[] vu1Data;
    if (g_machine == this) g_machine = nullptr;
}

void Machine::setupMemory() {
    ram = new uint8_t[RAM_SIZE]();
    spr = new uint8_t[SPR_SIZE]();
    iopRam = new uint8_t[IOP_RAM_SIZE]();
    vu0Code = new uint8_t[4096]();
    vu0Data = new uint8_t[4096]();
    vu1Code = new uint8_t[16384]();
    vu1Data = new uint8_t[16384]();
    rpage.assign(1u << 20, nullptr);
    wpage.assign(1u << 20, nullptr);
    auto map = [&](uint32_t vbase, uint8_t* host, uint32_t size, bool writable = true) {
        for (uint32_t off = 0; off < size; off += 4096) {
            rpage[(vbase + off) >> 12] = host + off;
            if (writable) wpage[(vbase + off) >> 12] = host + off;
        }
    };
    for (uint32_t seg : {0x00000000u, 0x20000000u, 0x30000000u, 0x80000000u, 0xA0000000u}) map(seg, ram, RAM_SIZE);
    map(0x70000000u, spr, SPR_SIZE);
    for (uint32_t seg : {0x00000000u, 0xA0000000u}) {
        map(seg + 0x11000000u, vu0Code, 4096);
        for (uint32_t m = 0; m < 4; ++m) map(seg + 0x11004000u + m * 4096, vu0Data, 4096);
        map(seg + 0x11008000u, vu1Code, 16384);
        map(seg + 0x1100C000u, vu1Data, 16384);
        map(seg + 0x1C000000u, iopRam, IOP_RAM_SIZE);
    }
}

uint8_t* Machine::hostPtr(uint32_t addr, uint32_t len) const {
    uint8_t* p = rpage[addr >> 12];
    if (!p) return nullptr;
    if (len > 1 && ((addr & 0xFFF) + len) > 4096) {
        // Must be contiguous across pages (true for RAM mappings).
        uint8_t* last = rpage[(addr + len - 1) >> 12];
        if (!last || last != p + (((addr + len - 1) & ~0xFFFu) - (addr & ~0xFFFu))) return nullptr;
    }
    return p + (addr & 0xFFF);
}

// ------------------------------------------------------------------ slow memory paths

static inline uint32_t physOf(uint32_t a) {
    if (a >= 0x80000000u && a < 0xC0000000u) return a & 0x1FFFFFFFu;
    return a;
}

uint8_t Machine::read8(uint32_t a) {
    if (uint8_t* p = rpage[a >> 12]) return p[a & 0xFFF];
    return (uint8_t)(mmioRead32(physOf(a) & ~3u) >> ((a & 3) * 8));
}
uint16_t Machine::read16(uint32_t a) {
    if (uint8_t* p = rpage[a >> 12]) {
        uint16_t v;
        std::memcpy(&v, p + (a & 0xFFF), 2);
        return v;
    }
    return (uint16_t)(mmioRead32(physOf(a) & ~3u) >> ((a & 2) * 8));
}
uint32_t Machine::read32(uint32_t a) {
    if (uint8_t* p = rpage[a >> 12]) {
        uint32_t v;
        std::memcpy(&v, p + (a & 0xFFF), 4);
        return v;
    }
    return mmioRead32(physOf(a));
}
uint64_t Machine::read64(uint32_t a) {
    if (uint8_t* p = rpage[a >> 12]) {
        uint64_t v;
        std::memcpy(&v, p + (a & 0xFFF), 8);
        return v;
    }
    return mmioRead64(physOf(a));
}
void Machine::read128(uint32_t a, Reg128* out) {
    a &= ~15u;
    if (uint8_t* p = rpage[a >> 12]) {
        std::memcpy(out, p + (a & 0xFFF), 16);
        return;
    }
    mmioRead128(physOf(a), out);
}
void Machine::write8(uint32_t a, uint8_t v) {
    if (uint8_t* p = wpage[a >> 12]) {
        p[a & 0xFFF] = v;
        return;
    }
    const uint32_t ph = physOf(a);
    // Byte writes to registers: SIO TXFIFO is the common case.
    if ((ph & ~3u) == 0x1000F180u) {
        char ch = (char)v;
        console(&ch, 1);
        return;
    }
    mmioWrite32(ph & ~3u, (uint32_t)v << ((a & 3) * 8));
}
void Machine::write16(uint32_t a, uint16_t v) {
    if (uint8_t* p = wpage[a >> 12]) {
        std::memcpy(p + (a & 0xFFF), &v, 2);
        return;
    }
    mmioWrite32(physOf(a) & ~3u, (uint32_t)v << ((a & 2) * 8));
}
void Machine::write32(uint32_t a, uint32_t v) {
    if (uint8_t* p = wpage[a >> 12]) {
        std::memcpy(p + (a & 0xFFF), &v, 4);
        return;
    }
    mmioWrite32(physOf(a), v);
}
void Machine::write64(uint32_t a, uint64_t v) {
    if (uint8_t* p = wpage[a >> 12]) {
        std::memcpy(p + (a & 0xFFF), &v, 8);
        return;
    }
    mmioWrite64(physOf(a), v);
}
void Machine::write128(uint32_t a, const Reg128* v) {
    a &= ~15u;
    if (uint8_t* p = wpage[a >> 12]) {
        std::memcpy(p + (a & 0xFFF), v, 16);
        return;
    }
    mmioWrite128(physOf(a), v);
}

std::string Machine::readCString(uint32_t a, size_t max) {
    std::string s;
    for (size_t i = 0; i < max; ++i) {
        char ch = (char)read8(a + (uint32_t)i);
        if (!ch) break;
        s.push_back(ch);
    }
    return s;
}

void Machine::console(const char* s, size_t n) {
    if (consoleOut) consoleOut(s, n);
}

// ------------------------------------------------------------------ code lookup

RecompFn Machine::lookup(uint32_t pc, const FuncEntry** entry) const {
    if (!mod_ || !mod_->funcCount) return nullptr;
    const FuncEntry* b = mod_->funcs;
    const FuncEntry* e = mod_->funcs + mod_->funcCount;
    const FuncEntry* it = std::upper_bound(b, e, pc, [](uint32_t a, const FuncEntry& f) { return a < f.start; });
    if (it == b) return nullptr;
    --it;
    if (pc >= it->end) return nullptr;
    if (entry) *entry = it;
    return it->fn;
}

void Machine::reportMissingEntry(uint32_t pc) {
    static std::set<uint32_t> reported;
    missingEntryCount++;
    if (!reported.insert(pc).second) return;
    logError("runtime: no recompiled entry for %08x (code reached dynamically)", pc);
    if (!cfg.missingEntriesFile.empty()) {
        std::ofstream f(cfg.missingEntriesFile, std::ios::app);
        char buf[16];
        snprintf(buf, sizeof buf, "%08x\n", pc);
        f << buf;
    }
}

void Machine::runUntil(Ctx* c, uint32_t stopAt) {
    while (c->pc != stopAt && !exitRequested_) {
        if (isMagic(c->pc)) {
            logError("runtime: unexpected return to %08x while waiting for %08x", c->pc, stopAt);
            requestExit(5);
            return;
        }
        RecompFn fn = lookup(c->pc);
        if (!fn) {
            reportMissingEntry(c->pc);
            requestExit(6);
            return;
        }
        fn(c);
    }
}

void Machine::initCtx(Ctx* c) {
    std::memset((void*)c, 0, sizeof(Ctx));
    c->rpage = rpage.data();
    c->wpage = wpage.data();
    c->m = this;
    c->vf = vu->vu0().vf;
    c->nextEvent = &nextEvent;
    c->cycles = cycles;
    c->fcr31 = 0x01000001;
    c->intEnabled = true;
    c->threadId = -1;
}

// ------------------------------------------------------------------ events

uint64_t Machine::schedule(int64_t when, std::function<void()> fn) {
    Event e{when, eventSeq_++, std::move(fn)};
    const uint64_t id = e.seq;
    events_.push(std::move(e));
    nextEvent = events_.top().time;
    return id;
}

void Machine::cancel(uint64_t id) { g_cancelled.insert(id); }

void Machine::processDueEvents() {
    while (!events_.empty() && events_.top().time <= cycles) {
        Event e = events_.top();
        events_.pop();
        nextEvent = events_.empty() ? INT64_MAX : events_.top().time;
        if (g_cancelled.erase(e.seq)) continue;
        e.fn();
    }
    nextEvent = events_.empty() ? INT64_MAX : events_.top().time;
    if (cfg.maxCycles && cycles >= (int64_t)cfg.maxCycles) {
        logWarn("runtime: cycle limit reached");
        requestExit(7);
    }
}

// ------------------------------------------------------------------ interrupts

bool Machine::interruptPending() const { return intc->pending() != 0 || dmac->pending() != 0; }

bool Machine::interruptsEnabled() const { return true; }

void Machine::checkInterrupts(Ctx* current) {
    if (kernel->inInterrupt()) return;
    if (current && !current->intEnabled) return;
    bool any = false;
    for (int guard = 0; guard < 64; ++guard) {
        const uint32_t ip = intc->pending();
        const uint32_t dp = dmac->pending();
        if (!ip && !dp) break;
        any = true;
        for (int i = 0; i < 15; ++i) {
            if (ip & (1u << i)) {
                intc->stat &= ~(1u << i);
                kernel->runIntcHandlers(i);
            }
        }
        for (int i = 0; i < 10; ++i) {
            if (dp & (1u << i)) {
                dmac->stat &= ~(1u << i);
                kernel->runDmacHandlers(i);
            }
        }
    }
    if (any && current) {
        syncTo(current);
        kernel->preemptIfNeeded(current);
    }
}

// ------------------------------------------------------------------ boot / run

void Machine::loadElf() {
    sb2::LoadedElf elf;
    std::string err;
    if (!sb2::loadElfSegments(cfg.elfPath, elf, err)) {
        logError("runtime: %s", err.c_str());
        requestExit(2);
        return;
    }
    for (auto& s : elf.segments) {
        for (size_t i = 0; i < s.data.size(); ++i) write8(s.vaddr + (uint32_t)i, s.data[i]);
        for (uint32_t i = (uint32_t)s.data.size(); i < s.memsz; ++i) write8(s.vaddr + i, 0);
    }
    if (mod_ && mod_->entry != elf.entry)
        logWarn("runtime: ELF entry %08x differs from module entry %08x", elf.entry, mod_->entry);
}

void Machine::requestExit(int code) {
    if (!exitRequested_) exitCode_ = code;
    exitRequested_ = true;
}

int Machine::run() {
    loadElf();
    if (exitRequested_) return exitCode_;
    iop->boot();
    gs->startVideo();
    kernel->run(mod_ ? mod_->entry : 0);
    return exitCode_;
}

} // namespace ps2
