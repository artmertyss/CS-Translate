#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static_assert(sizeof(void*) == 4, "This module was written for CS 1.6 (hl.exe, 32-bit).");

namespace engidx {
constexpr int RegisterVariable  = 14;
constexpr int GetCvarString     = 16;
constexpr int AddCommand        = 17;
constexpr int HookUserMsg       = 18;
constexpr int ServerCmd         = 19;
constexpr int ClientCmd         = 20;
constexpr int GetPlayerInfo     = 21;
constexpr int GetScreenInfo     = 12;
constexpr int DrawConsoleString = 27;
constexpr int DrawSetTextColor  = 28;
constexpr int ConsolePrint      = 30;
constexpr int CenterPrint       = 31;
constexpr int GetMaxClients     = 36;
constexpr int CmdArgc           = 38;
constexpr int CmdArgv           = 39;
constexpr int GetLocalPlayer    = 51;
constexpr int ConPrintf         = 40;
constexpr int ConNPrintf        = 42;
constexpr int ConNXPrintf       = 43;
constexpr int DrawString        = 114;
constexpr int kCount            = 135;
}

struct enginefuncs_t {
    void* f[engidx::kCount];
};

using UserMsgHook    = int(__cdecl*)(const char* pszName, int iSize, void* pbuf);
using FnHookUserMsg  = int(__cdecl*)(char* szMsgName, UserMsgHook pfn);
using FnConsolePrint = void(__cdecl*)(const char* szText);
using FnCenterPrint  = void(__cdecl*)(const char* szText);
using FnClientCmd    = void(__cdecl*)(char* szCmdString);
using FnInitialize   = int(__cdecl*)(enginefuncs_t* pEnginefuncs, int iVersion);
using FnHUDInit      = void(__cdecl*)(void);
using FnHUDRedraw    = int(__cdecl*)(float flTime, int intermission);
using FnConPrintf    = void(__cdecl*)(const char* fmt, ...);
using FnAddCommand   = int(__cdecl*)(const char* cmd, void(__cdecl*)(void));
using FnDrawString   = int(__cdecl*)(int x, int y, const char* str, int r, int g, int b);
using FnGetLocalPlayer = void*(__cdecl*)(void);
using FnGetCvarString  = char*(__cdecl*)(char*);
using FnCmdArgc        = int(__cdecl*)(void);
using FnCmdArgv        = char*(__cdecl*)(int);

struct OverlayLine {
    std::string text;
    unsigned char r = 255, g = 255, b = 255;
    DWORD expire = 0;
    bool chat = false;
};

struct ChatItem {
    std::string msgName;
    std::string name;
    std::string text;
    int clientIndex = 0;
    bool playerChat = false;
};

void JobPush(const ChatItem& item);
bool JobPop(ChatItem& out);
struct OutLine {
    std::string text;
    bool center = false;
    unsigned char r = 255, g = 255, b = 255;
    bool toConsole = true;
    bool chat = false;
    int block = 0;
};
void OutPush(const std::string& text, bool center = false, unsigned char r = 255,
             unsigned char g = 255, unsigned char b = 255, bool chat = false,
             int block = 0);
void OverlayPush(const std::string& text, unsigned char r = 255, unsigned char g = 255,
                 unsigned char b = 255, int block = 0);
void OutDrain(std::vector<OutLine>& out);
int NewOverlayBlockId();

void StartWorker();

void SayPush(const std::string& textUtf8, const std::string& lang, bool team = false);
struct SayDone {
    std::string text;
    std::string lang;
    std::string original;
    bool team = false;
};
void SayOutDrain(std::vector<SayDone>& out);
std::string EncodeForOutput(const std::string& utf8);

extern std::atomic<bool> g_shutdown;

FnConsolePrint GetConsolePrint();
FnCenterPrint  GetCenterPrint();
