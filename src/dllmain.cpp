#include <windows.h>

#include "common.h"
#include "config.h"
#include "hooks.h"
#include "log.h"

static HMODULE g_hModule = nullptr;

static DWORD WINAPI ClientWatch(LPVOID) {
    while (!g_shutdown.load() && !IsClientHooked()) {
        HMODULE h = GetModuleHandleA("client.dll");
        if (h) TryHookClient(h, "watch thread");
        Sleep(100);
    }
    xlog::Write("client watch finished (clientHooked=%d)", IsClientHooked() ? 1 : 0);
    return 0;
}

static DWORD WINAPI InitThread(LPVOID) {
    try {
        const std::string dir = xlog::ModuleDir();
        LoadConfig(dir);
        xlog::Init(g_cfg.log);

        xlog::Write("=== cs_translate started ===");
        xlog::Write("module: %s", xlog::ModulePath().c_str());
        xlog::Write("settings: %s", IniFilePath().c_str());
        xlog::Write("enabled=%d show_sentence=%d show_words=%d max_words=%d charset=%s",
                    g_cfg.enabled ? 1 : 0, g_cfg.showSentence ? 1 : 0, g_cfg.showWords ? 1 : 0,
                    g_cfg.maxWords, g_cfg.charset.c_str());

        StartWorker();

        if (!g_cfg.enabled)
            xlog::Write("config: enabled=0 -> translating paused (translate_active 1 to resume)");

        {
            HANDLE t = CreateThread(nullptr, 0, ClientWatch, nullptr, 0, nullptr);
            if (t) CloseHandle(t);
        }

        PatchEngineTableEarly();

        for (int i = 0; i < 30 && !IsClientHooked(); i++) Sleep(100);
        xlog::Write("ready (clientHooked=%d)", IsClientHooked() ? 1 : 0);

        RegisterCommandsNow();
        {
            const std::string ready =
                g_cfg.prefix + " translator ready - target: " + GetDstLang() +
                " | source: " + GetSrcLang() +
                (g_cfg.enabled ? "" : " | PAUSED (translate_active 1)");
            ConsoleWriteNow(ready);
            if (g_cfg.enabled)
                OverlayPush(ready, 255, 205, 60);
        }
        PrintCommandList();
    } catch (const std::exception& e) {
        xlog::Write("init exception: %s", e.what());
    } catch (...) {
        xlog::Write("init unknown exception");
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            g_hModule = hModule;
            xlog::SetModuleHandle(hModule);
            xlog::Init(false);

            xlog::Write("DllMain(attach): client.dll present=%d",
                        GetModuleHandleA("client.dll") ? 1 : 0);

            {
                HMODULE pinned = nullptr;
                GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_PIN,
                                   (LPCSTR)&DllMain, &pinned);
            }


            {
                HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
                if (t) CloseHandle(t);
            }
            break;

        case DLL_PROCESS_DETACH:
            g_shutdown.store(true);
            break;
    }
    return TRUE;
}
