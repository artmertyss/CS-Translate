#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace xlog {

static std::mutex g_mtx;
static std::string g_path;
static bool g_enabled = true;
static bool g_inited = false;
static HMODULE g_hModule = nullptr;

void SetModuleHandle(HMODULE h) { g_hModule = h; }

std::string ModulePath() {
    char buf[MAX_PATH] = {0};
    if (g_hModule) GetModuleFileNameA(g_hModule, buf, MAX_PATH);
    return std::string(buf);
}

std::string ModuleDir() {
    std::string p = ModulePath();
    size_t slash = p.find_last_of("\\/");
    if (slash == std::string::npos) return std::string(".");
    return p.substr(0, slash);
}

void Init(bool enabled) {
    if (g_inited) {
        g_enabled = enabled;
        return;
    }
    g_inited = true;
    g_enabled = enabled;
    g_path = ModuleDir() + "\\cs_translate.log";
    if (!g_enabled) return;
    std::lock_guard<std::mutex> lk(g_mtx);
    FILE* f = nullptr;
    if (fopen_s(&f, g_path.c_str(), "w") == 0 && f) {
        fprintf(f, "=== cs_translate.log started ===\n");
        fclose(f);
    }
}

bool Enabled() { return g_enabled; }

void Write(const char* fmt, ...) {
    if (!g_enabled) return;
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);

    std::lock_guard<std::mutex> lk(g_mtx);
    FILE* f = nullptr;
    if (fopen_s(&f, g_path.c_str(), "a") == 0 && f) {
        fprintf(f, "[%02u:%02u:%02u.%03u] %s\n", st.wHour, st.wMinute, st.wSecond,
                st.wMilliseconds, msg);
        fclose(f);
    }
}

}
