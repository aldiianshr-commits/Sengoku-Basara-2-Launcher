// Lightweight categorized logging.
#pragma once
#include <cstdarg>
#include <cstdint>
#include <string>

namespace ps2 {

enum class LogCat : uint32_t {
    Core = 1u << 0,
    Kernel = 1u << 1,
    Sif = 1u << 2,
    Iop = 1u << 3,
    Mem = 1u << 4,
    Dma = 1u << 5,
    Gs = 1u << 6,
    Vif = 1u << 7,
    Vu = 1u << 8,
    Spu = 1u << 9,
    Pad = 1u << 10,
    Cdvd = 1u << 11,
    Mc = 1u << 12,
    Thread = 1u << 13,
    Intc = 1u << 14,
    Frontend = 1u << 15,
};

void logSetMask(uint32_t mask);           // categories enabled for debug output
void logSetFile(const std::string& path); // additionally append to a file
bool logEnabled(LogCat cat);
void logDebug(LogCat cat, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
void logInfo(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
void logWarn(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
void logError(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

#define PS2_DEBUG(cat, ...)                                          \
    do {                                                             \
        if (::ps2::logEnabled(::ps2::LogCat::cat)) ::ps2::logDebug(::ps2::LogCat::cat, __VA_ARGS__); \
    } while (0)

} // namespace ps2
