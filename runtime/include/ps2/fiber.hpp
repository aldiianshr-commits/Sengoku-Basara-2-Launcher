// Minimal cooperative fibers. Each emulated EE thread runs recompiled code
// on its own host stack so the PS2 kernel's thread switches map onto fiber
// switches.  Windows uses native fibers, other platforms use ucontext.
#pragma once
#include <cstddef>
#include <functional>
#include <memory>

namespace ps2 {

class Fiber {
public:
    // Wrap the calling OS thread as a fiber (the scheduler fiber).
    static std::unique_ptr<Fiber> fromCurrentThread();
    // New fiber that will run fn when first switched to. fn must never return
    // normally; it must switch away (the fiber is then destroyed by its owner).
    static std::unique_ptr<Fiber> create(std::function<void()> fn, size_t stackSize);
    ~Fiber();

    // Switch from `from` (the currently running fiber) to `to`.
    static void switchTo(Fiber* from, Fiber* to);

    struct Impl;
    Impl* impl() { return impl_.get(); }

private:
    Fiber();
    std::unique_ptr<Impl> impl_;
};

} // namespace ps2
