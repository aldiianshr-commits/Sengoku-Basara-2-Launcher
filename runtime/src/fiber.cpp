#include "ps2/fiber.hpp"

#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <ucontext.h>
#endif

namespace ps2 {

#if defined(_WIN32)

struct Fiber::Impl {
    void* handle = nullptr;
    bool converted = false;
    std::function<void()> fn;
};

static void WINAPI fiberEntry(void* p) {
    auto* impl = static_cast<Fiber::Impl*>(p);
    impl->fn();
    std::fprintf(stderr, "fatal: fiber function returned\n");
    std::abort();
}

Fiber::Fiber() : impl_(std::make_unique<Impl>()) {}

std::unique_ptr<Fiber> Fiber::fromCurrentThread() {
    std::unique_ptr<Fiber> f(new Fiber());
    void* cur = GetCurrentFiber();
    if (cur == nullptr || cur == (void*)0x1E00) {
        f->impl_->handle = ConvertThreadToFiber(nullptr);
        f->impl_->converted = true;
    } else {
        f->impl_->handle = cur;
    }
    return f;
}

std::unique_ptr<Fiber> Fiber::create(std::function<void()> fn, size_t stackSize) {
    std::unique_ptr<Fiber> f(new Fiber());
    f->impl_->fn = std::move(fn);
    f->impl_->handle = CreateFiberEx(stackSize, stackSize, 0, fiberEntry, f->impl_.get());
    if (!f->impl_->handle) {
        std::fprintf(stderr, "fatal: CreateFiberEx failed\n");
        std::abort();
    }
    return f;
}

Fiber::~Fiber() {
    if (!impl_ || !impl_->handle) return;
    if (impl_->converted) ConvertFiberToThread();
    else if (impl_->fn) DeleteFiber(impl_->handle);
}

void Fiber::switchTo(Fiber*, Fiber* to) { SwitchToFiber(to->impl_->handle); }

#else

struct Fiber::Impl {
    ucontext_t ctx{};
    void* stack = nullptr;
    std::function<void()> fn;
};

static void fiberEntry(unsigned lo, unsigned hi) {
    auto* impl = reinterpret_cast<Fiber::Impl*>(((uintptr_t)hi << 32) | (uintptr_t)lo);
    impl->fn();
    std::fprintf(stderr, "fatal: fiber function returned\n");
    std::abort();
}

Fiber::Fiber() : impl_(std::make_unique<Impl>()) {}

std::unique_ptr<Fiber> Fiber::fromCurrentThread() {
    std::unique_ptr<Fiber> f(new Fiber());
    getcontext(&f->impl_->ctx);
    return f;
}

std::unique_ptr<Fiber> Fiber::create(std::function<void()> fn, size_t stackSize) {
    std::unique_ptr<Fiber> f(new Fiber());
    f->impl_->fn = std::move(fn);
    f->impl_->stack = std::malloc(stackSize);
    getcontext(&f->impl_->ctx);
    f->impl_->ctx.uc_stack.ss_sp = f->impl_->stack;
    f->impl_->ctx.uc_stack.ss_size = stackSize;
    f->impl_->ctx.uc_link = nullptr;
    const uintptr_t p = reinterpret_cast<uintptr_t>(f->impl_.get());
    makecontext(&f->impl_->ctx, (void (*)())fiberEntry, 2, (unsigned)(p & 0xFFFFFFFFu), (unsigned)(p >> 32));
    return f;
}

Fiber::~Fiber() {
    if (impl_ && impl_->stack) std::free(impl_->stack);
}

void Fiber::switchTo(Fiber* from, Fiber* to) { swapcontext(&from->impl_->ctx, &to->impl_->ctx); }

#endif

} // namespace ps2
