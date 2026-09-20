#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <string>

#include <windows.h>

namespace deepagent {
namespace {

std::string g_logFile;

void OutputLine(const std::string& line) {
    OutputDebugStringA((line + "\r\n").c_str());
    if (g_logFile.empty()) return;
    FILE* f = nullptr;
    if (fopen_s(&f, g_logFile.c_str(), "a") != 0 || !f) return;
    fputs(line.c_str(), f);
    fputc('\n', f);
    fclose(f);
}

} // namespace

void InitLog(const char* logFile) {
    g_logFile = logFile ? logFile : "";
}

void Logf(const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
    va_end(args);

    char stamp[64];
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);
    OutputLine(std::string("[") + stamp + "] " + buf);
}

} // namespace deepagent
