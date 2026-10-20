// Emotion Engine CPU context shared between the runtime and recompiled code.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(_MSC_VER) && !defined(__clang__)
#define PS2_FORCEINLINE __forceinline
#define PS2_LIKELY(x) (x)
#define PS2_UNLIKELY(x) (x)
#define PS2_NOINLINE __declspec(noinline)
#else
#define PS2_FORCEINLINE inline __attribute__((always_inline))
#define PS2_LIKELY(x) __builtin_expect(!!(x), 1)
#define PS2_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define PS2_NOINLINE __attribute__((noinline))
#endif

namespace ps2 {

union alignas(16) Reg128 {
    uint8_t u8[16];
    int8_t s8[16];
    uint16_t u16[8];
    int16_t s16[8];
    uint32_t u32[4];
    int32_t s32[4];
    uint64_t u64[2];
    int64_t s64[2];
    float f[4];
};
static_assert(sizeof(Reg128) == 16);

struct Machine;

// Per-thread CPU state. Each EE thread owns one Ctx; recompiled functions
// receive a pointer to the Ctx of the thread they run on.
struct Ctx {
    Reg128 r[32];
    Reg128 hi; // u64[0] = HI, u64[1] = HI1
    Reg128 lo; // u64[0] = LO, u64[1] = LO1
    uint32_t sa = 0; // funnel shift amount in bytes (0..15)
    uint32_t pc = 0; // dispatch target / resume address

    // COP1 (FPU)
    uint32_t f[32];
    uint32_t facc = 0;
    uint32_t fcr31 = 0;

    // Global time in EE cycles as seen by this context.
    int64_t cycles = 0;
    int64_t* nextEvent = nullptr; // points at Machine::nextEvent

    // Fast memory page tables (copied from Machine for locality).
    uint8_t* const* rpage = nullptr;
    uint8_t* const* wpage = nullptr;

    Reg128* vf = nullptr; // VU0 floating point registers (shared)

    Machine* m = nullptr;
    int threadId = -1;
    bool inInterrupt = false;
    bool intEnabled = true; // COP0 Status.EIE (per thread, saved on switch)
};

// Entry in the generated function table.
using RecompFn = void (*)(Ctx*);
struct FuncEntry {
    uint32_t start;
    uint32_t end;
    RecompFn fn;
};

// Information exported by a recompiled module.
struct ModuleInfo {
    const char* name;
    const char* elfSha256;   // hex digest of the source ELF
    uint32_t entry;
    const FuncEntry* funcs;  // sorted by start
    size_t funcCount;
    uint32_t textStart, textEnd;
    const char* recompilerVersion;
};

} // namespace ps2
