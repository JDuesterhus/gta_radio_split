#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace radiosplit {
namespace {

std::mutex g_logMutex;
FILE* g_log = nullptr;

}  // namespace

void LogOpen(const wchar_t* path) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_log != nullptr) {
        return;
    }
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"wb") != 0) {
        return;
    }
    g_log = file;
    if (g_log != nullptr) {
        const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
        std::fwrite(bom, 1, sizeof bom, g_log);
        std::fflush(g_log);
    }
}

void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_log == nullptr) {
        return;
    }
    std::va_list args;
    va_start(args, fmt);
    std::vfprintf(g_log, fmt, args);
    va_end(args);
    std::fputc('\n', g_log);
    std::fflush(g_log);
}

}  // namespace radiosplit
