// High-level emulation of the EE kernel (syscalls, threads, semaphores,
// interrupt handler chains, alarms).  Threads run on host fibers.
#pragma once
#include <cstdint>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "context.hpp"
#include "fiber.hpp"

namespace ps2 {

class Machine;

enum ThreadStatus : uint32_t {
    THS_RUN = 0x01,
    THS_READY = 0x02,
    THS_WAIT = 0x04,
    THS_SUSPEND = 0x08,
    THS_WAITSUSPEND = 0x0C,
    THS_DORMANT = 0x10,
};
enum WaitType : uint32_t { WAIT_NONE = 0, WAIT_SLEEP = 1, WAIT_SEMA = 2 };

struct Thread {
    int id = 0;
    uint32_t status = THS_DORMANT;
    uint32_t func = 0, stack = 0, stackSize = 0, gp = 0;
    int initPriority = 0, priority = 0;
    uint32_t attr = 0, option = 0;
    uint32_t waitType = WAIT_NONE;
    int waitId = 0;
    int wakeupCount = 0;
    int waitResult = 0;    // value returned by the blocking syscall
    uint32_t startArg = 0;
    Ctx ctx;
    std::unique_ptr<Fiber> fiber;
};

struct Semaphore {
    int id = 0;
    int count = 0, maxCount = 0, initCount = 0;
    uint32_t attr = 0, option = 0;
    std::deque<int> waiters; // thread ids, FIFO
};

struct IntHandler {
    int id = 0;
    uint32_t func = 0, arg = 0, gp = 0;
    bool enabled = true;
};

struct Alarm {
    int id = 0;
    uint64_t eventId = 0;
    uint32_t func = 0, arg = 0, gp = 0;
    uint16_t time = 0;
};

class Kernel {
public:
    explicit Kernel(Machine& m);
    ~Kernel();

    // Create the main thread at `entry` and run the scheduler until exit.
    int run(uint32_t entry);

    void syscall(Ctx* c, uint32_t addr);

    // Interrupt delivery (called from Machine::checkInterrupts).
    bool inInterrupt() const { return inInterrupt_; }
    void runIntcHandlers(int cause);
    void runDmacHandlers(int channel);
    // After interrupt handlers ran on `c`'s thread, switch if needed.
    void preemptIfNeeded(Ctx* c);

    Thread* current() { return current_; }
    Thread* thread(int id);
    int vsyncFlagAddr = 0, vsyncCounterAddr = 0;
    uint32_t gsImr = 0x7F00;

    // Calls guest code at `func` with up to 4 arguments using a dedicated
    // context and stack; used for interrupt handlers and callbacks.
    uint32_t callGuest(uint32_t func, uint32_t gp, uint32_t a0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0);

    // Kernel memory heap end, OSD settings
    uint32_t heapEnd = 0;
    uint32_t osdConfig = 0;
    uint32_t sifSysReg[32] = {};

private:
    // scheduling
    void schedulerLoop();
    void switchToScheduler(Thread* t);
    Thread* pickNext();
    void makeReady(Thread* t, bool front = false);
    void removeReady(Thread* t);
    void block(Ctx* c, uint32_t waitType, int waitId);
    void wakeWaiter(Thread* t, int result);
    void exitCurrent(Ctx* c, bool deleteSelf);
    void startFiber(Thread* t);
    void yieldIfHigherReady(Ctx* c);
    bool higherPriorityReady(const Thread* t) const;
    void reapZombies();

    // syscalls
    int32_t sysCreateThread(uint32_t paramAddr);
    int32_t sysStartThread(Ctx* c, int id, uint32_t arg);
    int32_t sysDeleteThread(int id);
    int32_t sysTerminateThread(Ctx* c, int id);
    int32_t sysReferThreadStatus(int id, uint32_t addr);
    int32_t sysChangeThreadPriority(Ctx* c, int id, int prio, bool isr);
    int32_t sysRotateReadyQueue(Ctx* c, int prio, bool isr);
    int32_t sysSleepThread(Ctx* c);
    int32_t sysWakeupThread(Ctx* c, int id, bool isr);
    int32_t sysCancelWakeup(int id);
    int32_t sysSuspendThread(Ctx* c, int id);
    int32_t sysResumeThread(Ctx* c, int id, bool isr);
    int32_t sysReleaseWaitThread(Ctx* c, int id, bool isr);
    int32_t sysCreateSema(uint32_t addr);
    int32_t sysDeleteSema(Ctx* c, int id);
    int32_t sysSignalSema(Ctx* c, int id, bool isr);
    int32_t sysWaitSema(Ctx* c, int id);
    int32_t sysPollSema(int id);
    int32_t sysReferSemaStatus(int id, uint32_t addr);
    int32_t addHandler(std::vector<IntHandler>& list, uint32_t func, int next, uint32_t arg, uint32_t gp);
    int32_t removeHandler(std::vector<IntHandler>& list, int id);
    int32_t sysSetAlarm(Ctx* c, uint16_t time, uint32_t func, uint32_t arg);
    int32_t sysReleaseAlarm(int id);
    uint32_t sysSetupThread(Ctx* c, uint32_t gp, uint32_t stack, uint32_t size, uint32_t args, uint32_t root);
    uint32_t sysSifSetDma(uint32_t addr, uint32_t count);

    Machine& m_;
    std::map<int, std::unique_ptr<Thread>> threads_;
    std::map<int, std::unique_ptr<Semaphore>> semas_;
    std::map<int, std::list<Thread*>> ready_; // priority -> FIFO
    std::vector<IntHandler> intcHandlers_[16];
    std::vector<IntHandler> dmacHandlers_[16];
    std::map<int, Alarm> alarms_;
    std::vector<std::unique_ptr<Fiber>> zombies_;
    std::unique_ptr<Fiber> schedFiber_;
    Thread* current_ = nullptr;
    int nextThreadId_ = 1, nextSemaId_ = 1, nextHandlerId_ = 1, nextAlarmId_ = 1;
    bool inInterrupt_ = false;
    bool dispatchDisabled_ = false;
    Ctx intCtx_;
    uint32_t mainStackTop_ = 0;
    uint32_t rootFunc_ = 0;
    int exitCode_ = 0;
};

} // namespace ps2
