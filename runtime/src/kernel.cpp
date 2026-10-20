#include "ps2/kernel.hpp"

#include <algorithm>
#include <cstring>

#include "ps2/hw.hpp"
#include "ps2/log.hpp"
#include "ps2/machine.hpp"
#include "ps2/sif.hpp"

namespace ps2 {

namespace {
constexpr size_t kFiberStack = 1024 * 1024;
constexpr uint32_t kIntStackTop = 0x0007F000; // kernel RAM, unused by games
constexpr uint32_t kCtxReserve = 0x2A0;        // kernel thread context area
constexpr int64_t kHsyncCycles = 18743;        // NTSC: 294.912 MHz / 15734.26 Hz
} // namespace

Kernel::Kernel(Machine& m) : m_(m) {
    std::memset(&intCtx_, 0, sizeof intCtx_);
    // OSD config: Japanese, 4:3, RGB.  bit layout per libosd.
    osdConfig = 0;
}

Kernel::~Kernel() = default;

Thread* Kernel::thread(int id) {
    if (id == 0 && current_) return current_;
    auto it = threads_.find(id);
    return it == threads_.end() ? nullptr : it->second.get();
}

// ------------------------------------------------------------------ scheduling

void Kernel::makeReady(Thread* t, bool front) {
    t->status = THS_READY;
    auto& q = ready_[t->priority];
    q.remove(t);
    if (front) q.push_front(t);
    else q.push_back(t);
}

void Kernel::removeReady(Thread* t) {
    auto it = ready_.find(t->priority);
    if (it != ready_.end()) it->second.remove(t);
    for (auto& [p, q] : ready_) q.remove(t);
}

Thread* Kernel::pickNext() {
    for (auto& [prio, q] : ready_)
        if (!q.empty()) return q.front();
    return nullptr;
}

bool Kernel::higherPriorityReady(const Thread* t) const {
    for (auto& [prio, q] : ready_) {
        if (q.empty()) continue;
        return prio < t->priority;
    }
    return false;
}

void Kernel::switchToScheduler(Thread* t) {
    m_.syncFrom(&t->ctx);
    Fiber::switchTo(t->fiber.get(), schedFiber_.get());
    // Resumed by the scheduler.
    m_.syncTo(&t->ctx);
}

void Kernel::yieldIfHigherReady(Ctx* c) {
    if (inInterrupt_ || dispatchDisabled_ || !current_ || c != &current_->ctx) return;
    if (!higherPriorityReady(current_)) return;
    Thread* t = current_;
    makeReady(t, true);
    switchToScheduler(t);
}

void Kernel::preemptIfNeeded(Ctx* c) { yieldIfHigherReady(c); }

void Kernel::block(Ctx* c, uint32_t waitType, int waitId) {
    Thread* t = current_;
    if (!t || c != &t->ctx || inInterrupt_) {
        logWarn("kernel: blocking call outside of a thread context (ignored)");
        return;
    }
    t->status = THS_WAIT;
    t->waitType = waitType;
    t->waitId = waitId;
    switchToScheduler(t);
}

void Kernel::wakeWaiter(Thread* t, int result) {
    t->waitResult = result;
    t->waitType = WAIT_NONE;
    t->waitId = 0;
    if (t->status == THS_WAIT) makeReady(t);
    else if (t->status == THS_WAITSUSPEND) t->status = THS_SUSPEND;
}

void Kernel::startFiber(Thread* t) {
    if (t->fiber) zombies_.push_back(std::move(t->fiber));
    t->fiber = Fiber::create(
        [this, t]() {
            m_.syncTo(&t->ctx);
            try {
                m_.runUntil(&t->ctx, MAGIC_THREAD_EXIT);
            } catch (const GuestExit&) {
                // Program is shutting down: return control to the scheduler for good.
                m_.syncFrom(&t->ctx);
                Fiber::switchTo(t->fiber.get(), schedFiber_.get());
            }
            exitCurrent(&t->ctx, false);
        },
        kFiberStack);
}

void Kernel::exitCurrent(Ctx* c, bool deleteSelf) {
    Thread* t = current_;
    if (!t || c != &t->ctx) {
        logError("kernel: ExitThread outside thread context");
        m_.requestExit(1);
        return;
    }
    t->status = THS_DORMANT;
    t->priority = t->initPriority;
    t->wakeupCount = 0;
    m_.syncFrom(c);
    std::unique_ptr<Fiber> self = std::move(t->fiber);
    Fiber* selfPtr = self.get();
    zombies_.push_back(std::move(self));
    if (deleteSelf) threads_.erase(t->id);
    current_ = nullptr;
    Fiber::switchTo(selfPtr, schedFiber_.get());
    // never resumed
    logError("kernel: exited thread resumed");
    std::abort();
}

void Kernel::reapZombies() { zombies_.clear(); }

void Kernel::schedulerLoop() {
    while (!m_.exiting()) {
        reapZombies();
        Thread* t = pickNext();
        if (t) {
            removeReady(t);
            t->status = THS_RUN;
            current_ = t;
            m_.syncTo(&t->ctx);
            Fiber::switchTo(schedFiber_.get(), t->fiber.get());
            current_ = nullptr;
            continue;
        }
        // Idle: no runnable thread; advance time to the next event.
        if (m_.nextEvent == INT64_MAX) {
            logError("kernel: deadlock - no runnable threads and no pending events");
            m_.requestExit(3);
            break;
        }
        if (m_.cycles < m_.nextEvent) m_.cycles = m_.nextEvent;
        try {
            m_.processDueEvents();
            m_.checkInterrupts(nullptr);
        } catch (const GuestExit&) {
            break;
        }
    }
}

int Kernel::run(uint32_t entry) {
    schedFiber_ = Fiber::fromCurrentThread();
    auto main = std::make_unique<Thread>();
    Thread* t = main.get();
    t->id = nextThreadId_++;
    t->func = entry;
    t->initPriority = t->priority = 0;
    t->stack = RAM_SIZE - 0x10000;
    t->stackSize = 0x10000;
    m_.initCtx(&t->ctx);
    t->ctx.threadId = t->id;
    t->ctx.pc = entry;
    t->ctx.r[29].u64[0] = RAM_SIZE - 0x100;
    t->ctx.r[31].u64[0] = MAGIC_THREAD_EXIT;
    threads_[t->id] = std::move(main);
    startFiber(t);
    makeReady(t);
    schedulerLoop();
    return exitCode_;
}

// ------------------------------------------------------------------ guest calls

uint32_t Kernel::callGuest(uint32_t func, uint32_t gp, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    const bool nested = inInterrupt_;
    Ctx local;
    Ctx& c = nested ? local : intCtx_;
    m_.initCtx(&c);
    c.inInterrupt = true;
    c.intEnabled = false;
    c.threadId = current_ ? current_->id : -1;
    c.r[4].u64[0] = (int64_t)(int32_t)a0;
    c.r[5].u64[0] = (int64_t)(int32_t)a1;
    c.r[6].u64[0] = (int64_t)(int32_t)a2;
    c.r[7].u64[0] = (int64_t)(int32_t)a3;
    c.r[28].u64[0] = (int64_t)(int32_t)gp;
    c.r[29].u64[0] = nested ? kIntStackTop - 0x4000 : kIntStackTop;
    c.r[31].u64[0] = (int64_t)(int32_t)MAGIC_HANDLER_RETURN;
    c.pc = func;
    inInterrupt_ = true;
    m_.syncTo(&c);
    m_.runUntil(&c, MAGIC_HANDLER_RETURN);
    m_.syncFrom(&c);
    inInterrupt_ = nested;
    return c.r[2].u32[0];
}

void Kernel::runIntcHandlers(int cause) {
    auto list = intcHandlers_[cause & 15];
    for (auto& h : list) {
        if (!h.enabled) continue;
        int32_t r = (int32_t)callGuest(h.func, h.gp, (uint32_t)cause, h.arg, 0);
        if (r < 0) break;
    }
}

void Kernel::runDmacHandlers(int channel) {
    auto list = dmacHandlers_[channel & 15];
    for (auto& h : list) {
        if (!h.enabled) continue;
        int32_t r = (int32_t)callGuest(h.func, h.gp, (uint32_t)channel, h.arg, 0);
        if (r < 0) break;
    }
}

int32_t Kernel::addHandler(std::vector<IntHandler>& list, uint32_t func, int next, uint32_t arg, uint32_t gp) {
    IntHandler h;
    h.id = nextHandlerId_++;
    h.func = func;
    h.arg = arg;
    h.gp = gp;
    if (next == -1) {
        list.push_back(h);
    } else if (next == 0) {
        list.insert(list.begin(), h);
    } else {
        auto it = std::find_if(list.begin(), list.end(), [&](const IntHandler& x) { return x.id == next; });
        list.insert(it, h);
    }
    return h.id;
}

int32_t Kernel::removeHandler(std::vector<IntHandler>& list, int id) {
    auto it = std::find_if(list.begin(), list.end(), [&](const IntHandler& x) { return x.id == id; });
    if (it == list.end()) return -1;
    list.erase(it);
    return 0;
}

// ------------------------------------------------------------------ threads

int32_t Kernel::sysCreateThread(uint32_t p) {
    auto t = std::make_unique<Thread>();
    t->id = nextThreadId_++;
    t->func = m_.read32(p + 4);
    t->stack = m_.read32(p + 8);
    t->stackSize = m_.read32(p + 12);
    t->gp = m_.read32(p + 16);
    t->initPriority = t->priority = (int)m_.read32(p + 20);
    t->attr = m_.read32(p + 28);
    t->option = m_.read32(p + 32);
    t->status = THS_DORMANT;
    if (t->initPriority < 0 || t->initPriority > 127) return -1;
    const int id = t->id;
    threads_[id] = std::move(t);
    PS2_DEBUG(Thread, "CreateThread id=%d func=%08x prio=%d", id, threads_[id]->func, threads_[id]->priority);
    return id;
}

int32_t Kernel::sysStartThread(Ctx* c, int id, uint32_t arg) {
    Thread* t = thread(id);
    if (!t || id == 0 || t->status != THS_DORMANT) return -1;
    m_.initCtx(&t->ctx);
    t->ctx.threadId = t->id;
    t->ctx.pc = t->func;
    t->ctx.r[4].u64[0] = (int64_t)(int32_t)arg;
    t->ctx.r[28].u64[0] = (int64_t)(int32_t)t->gp;
    t->ctx.r[29].u64[0] = (int64_t)(int32_t)((t->stack + t->stackSize - kCtxReserve) & ~15u);
    t->ctx.r[31].u64[0] = (int64_t)(int32_t)MAGIC_THREAD_EXIT;
    t->startArg = arg;
    t->priority = t->initPriority;
    t->wakeupCount = 0;
    startFiber(t);
    makeReady(t);
    yieldIfHigherReady(c);
    return id;
}

int32_t Kernel::sysDeleteThread(int id) {
    Thread* t = thread(id);
    if (!t || t == current_ || t->status != THS_DORMANT) return -1;
    if (t->fiber) zombies_.push_back(std::move(t->fiber));
    threads_.erase(t->id);
    return id;
}

int32_t Kernel::sysTerminateThread(Ctx*, int id) {
    Thread* t = thread(id);
    if (!t || t == current_ || t->status == THS_DORMANT) return -1;
    removeReady(t);
    if (t->waitType == WAIT_SEMA) {
        auto it = semas_.find(t->waitId);
        if (it != semas_.end()) {
            auto& w = it->second->waiters;
            w.erase(std::remove(w.begin(), w.end(), t->id), w.end());
        }
    }
    t->status = THS_DORMANT;
    t->waitType = WAIT_NONE;
    t->waitId = 0;
    t->priority = t->initPriority;
    if (t->fiber) zombies_.push_back(std::move(t->fiber));
    return id;
}

int32_t Kernel::sysReferThreadStatus(int id, uint32_t a) {
    Thread* t = thread(id);
    if (!t) return -1;
    if (a) {
        m_.write32(a + 0, t->status);
        m_.write32(a + 4, t->func);
        m_.write32(a + 8, t->stack);
        m_.write32(a + 12, t->stackSize);
        m_.write32(a + 16, t->gp);
        m_.write32(a + 20, (uint32_t)t->initPriority);
        m_.write32(a + 24, (uint32_t)t->priority);
        m_.write32(a + 28, t->attr);
        m_.write32(a + 32, t->option);
        m_.write32(a + 36, t->waitType);
        m_.write32(a + 40, (uint32_t)t->waitId);
        m_.write32(a + 44, (uint32_t)t->wakeupCount);
    }
    return (int32_t)t->status;
}

int32_t Kernel::sysChangeThreadPriority(Ctx* c, int id, int prio, bool isr) {
    Thread* t = thread(id);
    if (!t || prio < 0 || prio > 127 || t->status == THS_DORMANT) return -1;
    const int old = t->priority;
    if (t->status == THS_READY) {
        removeReady(t);
        t->priority = prio;
        makeReady(t);
    } else {
        t->priority = prio;
    }
    if (!isr) {
        // Lowering our own priority may let another thread run.
        yieldIfHigherReady(c);
    }
    return old;
}

int32_t Kernel::sysRotateReadyQueue(Ctx* c, int prio, bool isr) {
    if (prio < 0 || prio > 127) return -1;
    if (current_ && current_->priority == prio && !isr && c == &current_->ctx) {
        Thread* t = current_;
        makeReady(t, false);
        switchToScheduler(t);
        return prio;
    }
    auto& q = ready_[prio];
    if (!q.empty()) {
        Thread* f = q.front();
        q.pop_front();
        q.push_back(f);
    }
    return prio;
}

int32_t Kernel::sysSleepThread(Ctx* c) {
    Thread* t = current_;
    if (!t) return -1;
    if (t->wakeupCount > 0) {
        t->wakeupCount--;
        return t->id;
    }
    block(c, WAIT_SLEEP, 0);
    return t->id;
}

int32_t Kernel::sysWakeupThread(Ctx* c, int id, bool isr) {
    if (id == 0) return -1;
    Thread* t = thread(id);
    if (!t || t == current_ || t->status == THS_DORMANT) return -1;
    if ((t->status == THS_WAIT || t->status == THS_WAITSUSPEND) && t->waitType == WAIT_SLEEP) {
        wakeWaiter(t, t->id);
        if (!isr) yieldIfHigherReady(c);
    } else {
        t->wakeupCount++;
    }
    return id;
}

int32_t Kernel::sysCancelWakeup(int id) {
    Thread* t = thread(id);
    if (!t) return -1;
    int n = t->wakeupCount;
    t->wakeupCount = 0;
    return n;
}

int32_t Kernel::sysSuspendThread(Ctx*, int id) {
    Thread* t = thread(id);
    if (!t || t->status == THS_DORMANT) return -1;
    if (t == current_) {
        t->status = THS_SUSPEND;
        return id;
    }
    if (t->status == THS_READY) {
        removeReady(t);
        t->status = THS_SUSPEND;
    } else if (t->status == THS_WAIT) {
        t->status = THS_WAITSUSPEND;
    } else {
        return -1;
    }
    return id;
}

int32_t Kernel::sysResumeThread(Ctx* c, int id, bool isr) {
    Thread* t = thread(id);
    if (!t || t == current_) return -1;
    if (t->status == THS_SUSPEND) {
        makeReady(t);
        if (!isr) yieldIfHigherReady(c);
    } else if (t->status == THS_WAITSUSPEND) {
        t->status = THS_WAIT;
    } else {
        return -1;
    }
    return id;
}

int32_t Kernel::sysReleaseWaitThread(Ctx* c, int id, bool isr) {
    Thread* t = thread(id);
    if (!t || t == current_) return -1;
    if (t->status != THS_WAIT && t->status != THS_WAITSUSPEND) return -1;
    if (t->waitType == WAIT_SEMA) {
        auto it = semas_.find(t->waitId);
        if (it != semas_.end()) {
            auto& w = it->second->waiters;
            w.erase(std::remove(w.begin(), w.end(), t->id), w.end());
        }
    }
    wakeWaiter(t, -1);
    if (!isr) yieldIfHigherReady(c);
    return id;
}

uint32_t Kernel::sysSetupThread(Ctx* c, uint32_t gp, uint32_t stack, uint32_t size, uint32_t args, uint32_t root) {
    (void)gp;
    rootFunc_ = root;
    uint32_t base = (stack == 0xFFFFFFFFu) ? (RAM_SIZE - size) : stack;
    uint32_t sp = (base + size - kCtxReserve) & ~15u;
    mainStackTop_ = base;
    if (current_) {
        current_->stack = base;
        current_->stackSize = size;
        current_->gp = gp;
    }
    if (args) {
        // struct { int argc; char *argv[16]; char payload[256]; }
        std::string boot = m_.cfg.bootPath.empty() ? std::string("host0:main.elf") : m_.cfg.bootPath;
        if (boot.size() > 250) boot.resize(250);
        const uint32_t payload = args + 4 + 16 * 4;
        for (size_t i = 0; i <= boot.size(); ++i) m_.write8(payload + (uint32_t)i, i < boot.size() ? (uint8_t)boot[i] : 0);
        m_.write32(args, 1);
        m_.write32(args + 4, payload);
    }
    (void)c;
    return sp;
}

// ------------------------------------------------------------------ semaphores

int32_t Kernel::sysCreateSema(uint32_t a) {
    auto s = std::make_unique<Semaphore>();
    s->maxCount = (int)m_.read32(a + 4);
    s->initCount = (int)m_.read32(a + 8);
    s->attr = m_.read32(a + 16);
    s->option = m_.read32(a + 20);
    if (s->initCount < 0) return -1;
    s->count = s->initCount;
    s->id = nextSemaId_++;
    const int id = s->id;
    semas_[id] = std::move(s);
    return id;
}

int32_t Kernel::sysDeleteSema(Ctx* c, int id) {
    auto it = semas_.find(id);
    if (it == semas_.end()) return -1;
    auto waiters = it->second->waiters;
    semas_.erase(it);
    for (int tid : waiters)
        if (Thread* t = thread(tid)) wakeWaiter(t, -1);
    if (c) yieldIfHigherReady(c);
    return id;
}

int32_t Kernel::sysSignalSema(Ctx* c, int id, bool isr) {
    auto it = semas_.find(id);
    if (it == semas_.end()) return -1;
    Semaphore& s = *it->second;
    if (!s.waiters.empty()) {
        int tid = s.waiters.front();
        s.waiters.pop_front();
        if (Thread* t = thread(tid)) wakeWaiter(t, id);
        if (!isr) yieldIfHigherReady(c);
    } else {
        s.count++;
    }
    return id;
}

int32_t Kernel::sysWaitSema(Ctx* c, int id) {
    auto it = semas_.find(id);
    if (it == semas_.end()) return -1;
    Semaphore& s = *it->second;
    if (s.count > 0) {
        s.count--;
        return id;
    }
    if (!current_ || inInterrupt_) return -1;
    s.waiters.push_back(current_->id);
    Thread* t = current_;
    block(c, WAIT_SEMA, id);
    return t->waitResult;
}

int32_t Kernel::sysPollSema(int id) {
    auto it = semas_.find(id);
    if (it == semas_.end()) return -1;
    if (it->second->count > 0) {
        it->second->count--;
        return id;
    }
    return -1;
}

int32_t Kernel::sysReferSemaStatus(int id, uint32_t a) {
    auto it = semas_.find(id);
    if (it == semas_.end()) return -1;
    Semaphore& s = *it->second;
    if (a) {
        m_.write32(a + 0, (uint32_t)s.count);
        m_.write32(a + 4, (uint32_t)s.maxCount);
        m_.write32(a + 8, (uint32_t)s.initCount);
        m_.write32(a + 12, (uint32_t)s.waiters.size());
        m_.write32(a + 16, s.attr);
        m_.write32(a + 20, s.option);
    }
    return id;
}

// ------------------------------------------------------------------ alarms

int32_t Kernel::sysSetAlarm(Ctx*, uint16_t time, uint32_t func, uint32_t arg) {
    if (alarms_.size() >= 64) return -1;
    Alarm a;
    a.id = nextAlarmId_++;
    a.func = func;
    a.arg = arg;
    a.time = time;
    a.gp = current_ ? (uint32_t)current_->ctx.r[28].u32[0] : 0;
    const int id = a.id;
    a.eventId = m_.schedule(m_.cycles + (int64_t)time * kHsyncCycles, [this, id]() {
        auto it = alarms_.find(id);
        if (it == alarms_.end()) return;
        Alarm al = it->second;
        alarms_.erase(it);
        callGuest(al.func, al.gp, (uint32_t)id, al.time, al.arg);
    });
    alarms_[id] = a;
    return id;
}

int32_t Kernel::sysReleaseAlarm(int id) {
    auto it = alarms_.find(id);
    if (it == alarms_.end()) return -1;
    m_.cancel(it->second.eventId);
    alarms_.erase(it);
    return id;
}

// ------------------------------------------------------------------ SIF DMA

uint32_t Kernel::sysSifSetDma(uint32_t addr, uint32_t count) {
    static uint32_t s_id = 1;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t e = addr + i * 16;
        const uint32_t src = m_.read32(e), dest = m_.read32(e + 4), size = m_.read32(e + 8),
                       attr = m_.read32(e + 12);
        m_.sif->dmaToIop(src, dest, size, attr);
    }
    s_id = (s_id + 1) & 0xFFFF;
    if (s_id == 0) s_id = 1;
    return s_id;
}

// ------------------------------------------------------------------ dispatcher

void Kernel::syscall(Ctx* c, uint32_t addr) {
    (void)addr;
    m_.syncFrom(c);
    const int32_t num = (int32_t)c->r[3].u32[0];
    const uint32_t a0 = c->r[4].u32[0], a1 = c->r[5].u32[0], a2 = c->r[6].u32[0], a3 = c->r[7].u32[0];
    const uint32_t t0 = c->r[8].u32[0];
    const bool isr = num < 0 || inInterrupt_;
    int64_t ret = 0;
    PS2_DEBUG(Kernel, "syscall %d (0x%x) a0=%08x a1=%08x a2=%08x a3=%08x", num, (uint32_t)num, a0, a1, a2, a3);
    switch (num < 0 ? -num : num) {
    case 0x01: // ResetEE
        break;
    case 0x02: // SetGsCrt(interlace, mode, field)
        m_.sif->onSetGsCrt(a0, a1, a2);
        break;
    case 0x04: // Exit
        m_.requestExit((int)a0);
        exitCode_ = (int)a0;
        throw GuestExit{(int)a0};
    case 0x06: // LoadExecPS2
    case 0x07: // ExecPS2
        logError("kernel: LoadExecPS2/ExecPS2(%s) not supported", m_.readCString(a0).c_str());
        m_.requestExit(4);
        throw GuestExit{4};
    case 0x10: // AddIntcHandler(2)
        ret = addHandler(intcHandlers_[a0 & 15], a1, (int32_t)a2, a3, c->r[28].u32[0]);
        break;
    case 0x11: ret = removeHandler(intcHandlers_[a0 & 15], (int)a1); break;
    case 0x12: // AddDmacHandler(2)
        ret = addHandler(dmacHandlers_[a0 & 15], a1, (int32_t)a2, a3, c->r[28].u32[0]);
        break;
    case 0x13: ret = removeHandler(dmacHandlers_[a0 & 15], (int)a1); break;
    case 0x14: // _EnableIntc
    case 0x1a: {
        const uint32_t bit = 1u << (a0 & 15);
        ret = (m_.intc->mask & bit) ? 0 : 1;
        m_.intc->mask |= bit;
        break;
    }
    case 0x15: // _DisableIntc
    case 0x1b: {
        const uint32_t bit = 1u << (a0 & 15);
        ret = (m_.intc->mask & bit) ? 1 : 0;
        m_.intc->mask &= ~bit;
        break;
    }
    case 0x16: // _EnableDmac
    case 0x1c: {
        const uint32_t bit = 1u << (16 + (a0 & 15));
        ret = (m_.dmac->stat & bit) ? 0 : 1;
        m_.dmac->stat |= bit;
        break;
    }
    case 0x17: // _DisableDmac
    case 0x1d: {
        const uint32_t bit = 1u << (16 + (a0 & 15));
        ret = (m_.dmac->stat & bit) ? 1 : 0;
        m_.dmac->stat &= ~bit;
        break;
    }
    case 0xfc: // SetAlarm(time, func, arg)
    case 0xfd:
    case 0x18:
    case 0x1e:
        ret = sysSetAlarm(c, (uint16_t)a0, a1, a2);
        break;
    case 0xfe: // ReleaseAlarm
    case 0xff:
    case 0x19:
    case 0x1f:
        ret = sysReleaseAlarm((int)a0);
        break;
    case 0x20: ret = sysCreateThread(a0); break;
    case 0x21: ret = sysDeleteThread((int)a0); break;
    case 0x22: ret = sysStartThread(c, (int)a0, a1); break;
    case 0x23: // ExitThread
        exitCurrent(c, false);
        return;
    case 0x24: // ExitDeleteThread
        exitCurrent(c, true);
        return;
    case 0x25:
    case 0x26: ret = sysTerminateThread(c, (int)a0); break;
    case 0x27: dispatchDisabled_ = true; break;
    case 0x28:
        dispatchDisabled_ = false;
        yieldIfHigherReady(c);
        break;
    case 0x29:
    case 0x2a: ret = sysChangeThreadPriority(c, (int)a0, (int)a1, isr); break;
    case 0x2b:
    case 0x2c: ret = sysRotateReadyQueue(c, (int)a0, isr); break;
    case 0x2d:
    case 0x2e: ret = sysReleaseWaitThread(c, (int)a0, isr); break;
    case 0x2f: ret = current_ ? current_->id : 0; break; // GetThreadId
    case 0x30:
    case 0x31: ret = sysReferThreadStatus((int)a0, a1); break;
    case 0x32: ret = sysSleepThread(c); break;
    case 0x33:
    case 0x34: ret = sysWakeupThread(c, (int)a0, isr); break;
    case 0x35:
    case 0x36: ret = sysCancelWakeup((int)a0); break;
    case 0x37:
    case 0x38: ret = sysSuspendThread(c, (int)a0); break;
    case 0x39:
    case 0x3a: ret = sysResumeThread(c, (int)a0, isr); break;
    case 0x3c: ret = sysSetupThread(c, a0, a1, a2, a3, t0); break;
    case 0x3d: { // SetupHeap(start, size)
        heapEnd = (a1 == 0xFFFFFFFFu) ? mainStackTop_ : a0 + a1;
        ret = heapEnd;
        break;
    }
    case 0x3e: ret = heapEnd; break; // EndOfHeap
    case 0x40: ret = sysCreateSema(a0); break;
    case 0x41:
    case 0x49: ret = sysDeleteSema(isr ? nullptr : c, (int)a0); break;
    case 0x42:
    case 0x43: ret = sysSignalSema(c, (int)a0, isr); break;
    case 0x44: ret = sysWaitSema(c, (int)a0); break;
    case 0x45:
    case 0x46: ret = sysPollSema((int)a0); break;
    case 0x47:
    case 0x48: ret = sysReferSemaStatus((int)a0, a1); break;
    case 0x4a: osdConfig = m_.read32(a0); break; // SetOsdConfigParam
    case 0x4b: m_.write32(a0, osdConfig); break; // GetOsdConfigParam
    case 0x4c: case 0x4d: case 0x4e: case 0x4f: break; // Gs H/V params
    case 0x5a: { // Copy(dest, src, size)
        for (uint32_t i = 0; i < a2; ++i) m_.write8(a0 + i, m_.read8(a1 + i));
        break;
    }
    case 0x5b: ret = a0; break; // GetEntryAddress
    case 0x5c: case 0x5d: case 0x5e: case 0x5f: { // Enable/Disable Intc/Dmac handler
        auto& list = (num == 0x5c || num == 0x5d || num == -0x5c || num == -0x5d) ? intcHandlers_[a0 & 15]
                                                                              : dmacHandlers_[a0 & 15];
        const bool en = (num == 0x5c || num == 0x5e || num == -0x5c || num == -0x5e);
        ret = -1;
        for (auto& h : list)
            if (h.id == (int)a1) {
                h.enabled = en;
                ret = 0;
            }
        break;
    }
    case 0x60: case 0x61: case 0x62: break; // KSeg0, EnableCache, DisableCache
    case 0x63:
    case 0x67: ret = m_.cop0[a0 & 31]; break; // GetCop0
    case 0x64:
    case 0x68: break; // FlushCache
    case 0x66:
    case 0x6a: ret = 0; break; // CpuConfig
    case 0x6b: break;          // SifStopDma
    case 0x70: ret = gsImr; break;                 // GsGetIMR
    case 0x71: ret = gsImr; gsImr = a0; break;     // GsPutIMR
    case 0x73: // SetVSyncFlag(flagAddr, counterAddr)
        vsyncFlagAddr = (int)a0;
        vsyncCounterAddr = (int)a1;
        break;
    case 0x74: break; // SetSyscall
    case 0x75: {      // _print
        std::string s = m_.readCString(a0);
        m_.console(s.data(), s.size());
        break;
    }
    case 0x76: ret = -1; break; // SifDmaStat: transfers complete immediately
    case 0x77: ret = sysSifSetDma(a0, a1); break;
    case 0x78: m_.sif->setDChain(); break;
    case 0x79: m_.sif->setReg(a0, a1); ret = 1; break;
    case 0x7a: ret = m_.sif->getReg(a0); break;
    case 0x7c: ret = 0; break; // Deci2Call
    case 0x7d: ret = 0; break; // PSMode
    case 0x7e: ret = 0; break; // MachineType
    case 0x7f: ret = RAM_SIZE; break; // GetMemorySize
    case 0x54: break;                  // xlaunch (unused by games)
    case 0x55:                         // PutTLBEntry
    case 0x56:                         // _SetTLBEntry
        ret = (int32_t)a0;             // TLB is not modelled: identity-mapped memory
        break;
    case 0x57: ret = 0; break;         // GetTLBEntry
    case 0x58: ret = -1; break;        // ProbeTLBEntry: no match
    case 0x59: ret = 0; break;         // ExpandScratchPad
    case 0x6c: ret = 0; break;         // SetCPUTimerHandler
    case 0x6d: ret = 0; break;         // SetCPUTimer
    case 0x6e: break;                  // SetOsdConfigParam2
    case 0x6f: {                       // GetOsdConfigParam2(config, size, offset)
        for (uint32_t i = 0; i < a1 && i < 64; ++i) m_.write8(a0 + i, 0);
        ret = 0;
        break;
    }
    case 0x7b:                         // ExecOSD: return to the system menu
        logInfo("kernel: ExecOSD - the game asked to return to the PS2 menu");
        m_.requestExit(0);
        throw GuestExit{0};
    case 0x80: ret = 0; break;         // _GetGsDxDyOffset
    case 0x82: ret = 0; break;         // _InitTLB
    case 0x83: {                       // FindAddress(start, end, value)
        ret = 0;
        for (uint32_t p = a0 & ~3u; p < a1; p += 4)
            if (m_.read32(p) == a2) {
                ret = (int32_t)p;
                break;
            }
        break;
    }
    case 0x85: break;                  // SetMemoryMode
    case 0x86: ret = 0; break;         // GetMemoryMode: 32 MB retail
    default:
        if (!warnedSyscalls_.count(num)) {
            warnedSyscalls_.insert(num);
            logWarn("kernel: unimplemented syscall %d (0x%x) at %08x", num, (uint32_t)num, addr);
        }
        ret = 0;
        break;
    }
    c->r[2].u64[0] = (uint64_t)(int64_t)(int32_t)ret;
    m_.syncTo(c);
    // Syscalls may have raised interrupts (SIF, DMA) - deliver them now.
    if (!inInterrupt_) m_.checkInterrupts(c);
}

} // namespace ps2
