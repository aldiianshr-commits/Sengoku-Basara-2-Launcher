#include "ps2/log.hpp"

#include <cstdio>
#include <mutex>

namespace ps2 {

namespace {
uint32_t g_mask = 0;
int g_level = 2;
FILE* g_file = nullptr;
std::mutex g_mutex;

void emit(const char* prefix, const char* fmt, va_list ap) {
    char buf[2048];
    vsnprintf(buf, sizeof buf, fmt, ap);
    std::lock_guard<std::mutex> lk(g_mutex);
    std::fprintf(stderr, "%s%s\n", prefix, buf);
    if (g_file) {
        std::fprintf(g_file, "%s%s\n", prefix, buf);
        std::fflush(g_file);
    }
}
} // namespace

void logSetMask(uint32_t mask) { g_mask = mask; }
void logSetLevel(int level) { g_level = level; }
void logSetFile(const std::string& path) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_file) std::fclose(g_file);
    g_file = path.empty() ? nullptr : std::fopen(path.c_str(), "a");
}
bool logEnabled(LogCat cat) { return (g_mask & (uint32_t)cat) != 0; }

void logDebug(LogCat, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    emit("[dbg] ", fmt, ap);
    va_end(ap);
}
void logInfo(const char* fmt, ...) {
    if (g_level < 2) return;
    va_list ap;
    va_start(ap, fmt);
    emit("[info] ", fmt, ap);
    va_end(ap);
}
void logWarn(const char* fmt, ...) {
    if (g_level < 1) return;
    va_list ap;
    va_start(ap, fmt);
    emit("[warn] ", fmt, ap);
    va_end(ap);
}
void logError(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    emit("[error] ", fmt, ap);
    va_end(ap);
}

} // namespace ps2
