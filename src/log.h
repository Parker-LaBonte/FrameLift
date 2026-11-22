#pragma once
#include "common.h"

#include <cstdarg>
#include <cstdio>

// Plain-text diagnostics at %APPDATA%\FrameLift\log.txt (truncated each launch).
inline std::mutex& LogMutex() {
    static std::mutex m;
    return m;
}
inline FILE*& LogFile() {
    static FILE* f = nullptr;
    return f;
}
inline void LogInit() {
    const wchar_t* appdata = _wgetenv(L"APPDATA");
    std::wstring dir = appdata ? std::wstring(appdata) + L"\\FrameLift" : L".";
    CreateDirectoryW(dir.c_str(), nullptr);
    LogFile() = _wfopen((dir + L"\\log.txt").c_str(), L"w");
}
inline void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> l(LogMutex());
    FILE* f = LogFile();
    if (!f) f = stderr;  // --bench: no log file; report on the console instead
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%02d:%02d:%02d.%03d  ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fflush(f);
}
