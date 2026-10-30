#include "hooks.h"

#include <cstring>

#include "chat.h"
#include "common.h"
#include "config.h"
#include "log.h"
#include "utf8.h"

static FnHUDRedraw g_realHUDRedraw = nullptr;
static FnHookUserMsg g_realHookUserMsg = nullptr;
static FnConsolePrint g_consolePrint = nullptr;
static FnCenterPrint g_centerPrint = nullptr;
static FnConPrintf g_conPrintf = nullptr;
static FnAddCommand g_addCommand = nullptr;
static FnDrawString g_drawString = nullptr;
static FnGetLocalPlayer g_getLocalPlayer = nullptr;
static FnGetCvarString g_getCvarString = nullptr;
static FnCmdArgc g_cmdArgc = nullptr;
static FnCmdArgv g_cmdArgv = nullptr;
static FnClientCmd g_clientCmd = nullptr;

static int __cdecl Hook_UserMsg(const char* name, int iSize, void* pbuf);
static int __cdecl Hook_HookUserMsg(char* name, UserMsgHook fn);
static void EnsureSayTextHook(HMODULE hClient);

static std::mutex g_umMtx;
static std::map<std::string, UserMsgHook> g_origUserMsg;

static enginefuncs_t* g_engineTable = nullptr;
static HMODULE g_hClient = nullptr;
static enginefuncs_t* g_engfuncs = nullptr;
static std::atomic<bool> g_clientHooked{false};

FnConsolePrint GetConsolePrint() { return g_consolePrint; }
FnCenterPrint GetCenterPrint() { return g_centerPrint; }

static std::string LowerCopy(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

static bool IsWatched(const std::string& n) {
    const std::string l = LowerCopy(n);
    for (const auto& w : g_cfg.watch)
        if (LowerCopy(w) == l) return true;
    return false;
}

static bool IsValidCodePtr(const void* p) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    DWORD prot = mbi.Protect & 0xFF;
    return prot == PAGE_EXECUTE || prot == PAGE_EXECUTE_READ ||
           prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
}

static void CaptureSlots(enginefuncs_t* t) {
    if (!t) return;
    if (!g_consolePrint && IsValidCodePtr(t->f[engidx::ConsolePrint]))
        g_consolePrint = (FnConsolePrint)t->f[engidx::ConsolePrint];
    if (!g_centerPrint && IsValidCodePtr(t->f[engidx::CenterPrint]))
        g_centerPrint = (FnCenterPrint)t->f[engidx::CenterPrint];
    if (!g_conPrintf && IsValidCodePtr(t->f[engidx::ConPrintf]))
        g_conPrintf = (FnConPrintf)t->f[engidx::ConPrintf];
    if (!g_addCommand && IsValidCodePtr(t->f[engidx::AddCommand]))
        g_addCommand = (FnAddCommand)t->f[engidx::AddCommand];
    if (!g_drawString && IsValidCodePtr(t->f[engidx::DrawString]))
        g_drawString = (FnDrawString)t->f[engidx::DrawString];
    if (!g_getLocalPlayer && IsValidCodePtr(t->f[engidx::GetLocalPlayer]))
        g_getLocalPlayer = (FnGetLocalPlayer)t->f[engidx::GetLocalPlayer];

    if (!g_getCvarString && IsValidCodePtr(t->f[engidx::GetCvarString]))
        g_getCvarString = (FnGetCvarString)t->f[engidx::GetCvarString];
    if (!g_cmdArgc && IsValidCodePtr(t->f[engidx::CmdArgc]))
        g_cmdArgc = (FnCmdArgc)t->f[engidx::CmdArgc];
    if (!g_cmdArgv && IsValidCodePtr(t->f[engidx::CmdArgv]))
        g_cmdArgv = (FnCmdArgv)t->f[engidx::CmdArgv];
    if (!g_clientCmd && IsValidCodePtr(t->f[engidx::ClientCmd]))
        g_clientCmd = (FnClientCmd)t->f[engidx::ClientCmd];
}

static HMODULE ModuleOfPtr(const void* p) {
    if (!p) return nullptr;
    HMODULE m = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)p, &m))
        return m;
    return nullptr;
}

static void RevalidateReal(enginefuncs_t* real) {
    if (!real || !g_realHookUserMsg) return;
    void* slot = real->f[engidx::HookUserMsg];
    if (slot == (void*)&Hook_HookUserMsg || !IsValidCodePtr(slot)) return;
    HMODULE a = ModuleOfPtr((const void*)g_realHookUserMsg);
    HMODULE b = ModuleOfPtr(slot);
    if (!a || !b || a == b) return;
    g_realHookUserMsg = (FnHookUserMsg)slot;
    g_consolePrint = nullptr;
    g_centerPrint = nullptr;
    g_conPrintf = nullptr;
    g_addCommand = nullptr;
    g_drawString = nullptr;
    g_getLocalPlayer = nullptr;
    g_getCvarString = nullptr;
    g_cmdArgc = nullptr;
    g_cmdArgv = nullptr;
    g_clientCmd = nullptr;
    CaptureSlots(real);
    xlog::Write("early capture was bogus, engine funcs recaptured from real table");
}

static bool IsOwnMessage(int clientIndex) {
    if (clientIndex <= 0 || !g_getLocalPlayer) return false;
    void* ent = nullptr;
    try {
        ent = g_getLocalPlayer();
    } catch (...) {
        return false;
    }
    if (!ent) return false;
    return *(int*)ent == clientIndex;
}

static std::string LocalPlayerName() {
    static std::string cached;
    static bool ok = false;
    if (ok) return cached;
    if (!g_getCvarString) return cached;
    const char* s = nullptr;
    try {
        s = g_getCvarString((char*)"name");
    } catch (...) {
        return cached;
    }
    if (!s || !*s) return cached;
    std::string n;
    for (const char* p = s; *p && n.size() < 32; p++) {
        unsigned char c = (unsigned char)*p;
        if (c >= 32 && c < 127) n.push_back((char)c);
    }
    if (n.empty()) return cached;
    cached = n;
    ok = true;
    xlog::Write("local player name: %s", cached.c_str());
    return cached;
}

static bool IsWritablePtr(const void* p) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    DWORD prot = mbi.Protect & 0xFF;
    return prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
           prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
}

static bool WriteSlotRaw(void* slot, void* value) {
    *(void**)slot = value;
    return true;
}

static bool WriteSlot(void* slot, void* value) {
    if (!slot) return false;
    if (IsWritablePtr(slot)) {
        __try {
            return WriteSlotRaw(slot, value);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    bool ok = false;
    __try {
        ok = WriteSlotRaw(slot, value);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DWORD tmp = 0;
    VirtualProtect(slot, sizeof(void*), old, &tmp);
    return ok;
}

static bool IsReadableRegion(const void* lo, size_t len) {
    if (!lo || !len) return false;
    const BYTE* b = (const BYTE*)lo;
    const BYTE* e = b + len;
    while (b < e) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(b, &mbi, sizeof(mbi)) == 0) return false;
        if (mbi.State != MEM_COMMIT) return false;
        DWORD prot = mbi.Protect & 0xFF;
        if (prot == PAGE_NOACCESS || prot == (PAGE_GUARD & 0xFF)) return false;
        b = (const BYTE*)mbi.BaseAddress + mbi.RegionSize;
    }
    return true;
}

static bool LooksLikeEngineFuncs(const enginefuncs_t* t) {
    static const int idx[] = {11, engidx::RegisterVariable, engidx::GetCvarString,
                              engidx::AddCommand, engidx::HookUserMsg,
                              engidx::ClientCmd, engidx::ConsolePrint,
                              engidx::GetMaxClients, engidx::CmdArgc,
                              engidx::ConPrintf};
    if (!IsReadableRegion(t, engidx::kCount * sizeof(void*))) return false;

    HMODULE same = nullptr;
    for (int i = 0; i < 10; i++) {
        const void* p = t->f[idx[i]];
        if (!IsValidCodePtr(p)) return false;
        HMODULE m = ModuleOfPtr(p);
        if (!m) return false;
        if (!same)
            same = m;
        else if (m != same)
            return false;
    }
    return true;
}

struct Section {
    DWORD va;
    DWORD vsize;
};

static bool GetSection(HMODULE h, const char* name, Section& out) {
    BYTE* base = (BYTE*)h;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        char sname[9] = {0};
        memcpy(sname, sec[i].Name, 8);
        if (_stricmp(sname, name) == 0) {
            out.va = sec[i].VirtualAddress;
            out.vsize = sec[i].Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

static enginefuncs_t* ScanEngineTable(HMODULE h) {
    if (!h) return nullptr;
    Section data{};
    const char* secNames[] = {".data", ".bss", ".rdata"};
    for (int si = 0; si < 2; si++) {
        if (!GetSection(h, secNames[si], data)) continue;
        BYTE* base = (BYTE*)h;
        const uintptr_t step = sizeof(void*);
        const size_t need = engidx::kCount * sizeof(void*);
        if (data.vsize < need) continue;
        for (uintptr_t off = 0; off + need <= data.vsize; off += step) {
            auto* cand = (enginefuncs_t*)(base + data.va + off);
            if (LooksLikeEngineFuncs(cand)) {
                xlog::Write("engine table found (%s+0x%X)", secNames[si], (unsigned)off);
                return cand;
            }
        }
    }
    return nullptr;
}

void PatchEngineTableEarly() {
    enginefuncs_t* tab = ScanEngineTable(GetModuleHandleA(nullptr));
    if (!tab) {
        xlog::Write("early patch: engine table not found (scan) (hl.exe .data)");
        return;
    }
    void* cur = tab->f[engidx::HookUserMsg];
    if (cur == (void*)&Hook_HookUserMsg) {
        xlog::Write("early patch: pfnHookUserMsg already ours");
        return;
    }
    if (!IsValidCodePtr(cur)) {
        xlog::Write("early patch: pfnHookUserMsg invalid slot (%p)", cur);
        return;
    }
    if (!g_realHookUserMsg) g_realHookUserMsg = (FnHookUserMsg)cur;
    CaptureSlots(tab);
    if (WriteSlot(&tab->f[engidx::HookUserMsg], (void*)&Hook_HookUserMsg)) {
        g_engineTable = tab;
        xlog::Write("early patch OK: engine pfnHookUserMsg wrapped (before client)");
    } else {
        xlog::Write("early patch: engine table write failed (protection) -> handed over to A/B");
    }
}

static enginefuncs_t* ResolveEngfuncs(HMODULE h) {
    if (!h) return nullptr;

    if (g_cfg.gEngfuncsRva) {
        auto* cand = (enginefuncs_t*)((BYTE*)h + g_cfg.gEngfuncsRva);
        if (LooksLikeEngineFuncs(cand)) {
            xlog::Write("gEngfuncs found (RVA 0x%X)", g_cfg.gEngfuncsRva);
            return cand;
        }
        xlog::Write("gEngfuncs RVA 0x%X invalid (Initialize may not have run yet)",
                    g_cfg.gEngfuncsRva);
    }

    Section data{};
    bool found = false;
    enginefuncs_t* res = nullptr;
    const char* secNames[] = {".data", ".bss"};
    for (int si = 0; si < 2 && !found; si++) {
        if (!GetSection(h, secNames[si], data)) continue;
        BYTE* base = (BYTE*)h;
        const uintptr_t step = sizeof(void*);
        const size_t need = engidx::kCount * sizeof(void*);
        if (data.vsize < need) continue;
        for (uintptr_t off = 0; off + need <= data.vsize; off += step) {
            auto* cand = (enginefuncs_t*)(base + data.va + off);
            if (LooksLikeEngineFuncs(cand)) {
                xlog::Write("gEngfuncs found (scan, %s+0x%X)", secNames[si], (unsigned)off);
                res = cand;
                found = true;
                break;
            }
        }
    }
    return res;
}

static bool PatchUserMsg(enginefuncs_t* tab) {
    if (!tab) return false;
    void* cur = tab->f[engidx::HookUserMsg];
    if (cur == (void*)&Hook_HookUserMsg) return true;
    if (!IsValidCodePtr(cur)) return false;

    if (!g_realHookUserMsg) {
        g_realHookUserMsg = (FnHookUserMsg)cur;
        CaptureSlots(tab);
        xlog::Write("enginefuncs: HookUserMsg=%p ConsolePrint=%p Con_Printf=%p DrawString=%p",
                    (void*)g_realHookUserMsg, (void*)g_consolePrint, (void*)g_conPrintf,
                    (void*)g_drawString);
    }
    if (!g_realHookUserMsg) return false;

    if (!WriteSlot(&tab->f[engidx::HookUserMsg], (void*)&Hook_HookUserMsg)) {
        xlog::Write("enginefuncs pfnHookUserMsg write failed (protection?)");
        return false;
    }

    return true;
}

static int __cdecl Hook_HookUserMsg(char* name, UserMsgHook fn) {
    try {
        if (!g_realHookUserMsg) return 0;
        std::string n = name ? name : "";
        if (IsWatched(n)) {
            if (fn && fn != &Hook_UserMsg) {
                std::lock_guard<std::mutex> lk(g_umMtx);
                g_origUserMsg[n] = fn;
            }
            xlog::Write("listening: %s (original %p)", n.c_str(), (void*)fn);
            return g_realHookUserMsg(name, &Hook_UserMsg);
        }
        xlog::Write("usermsg registered: %s", n.c_str());
        return g_realHookUserMsg(name, fn);
    } catch (...) {
        return g_realHookUserMsg ? g_realHookUserMsg(name, fn) : 0;
    }
}

static int __cdecl Hook_UserMsg(const char* name, int iSize, void* pbuf) {
    UserMsgHook orig = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_umMtx);
        auto it = g_origUserMsg.find(name ? name : "");
        if (it != g_origUserMsg.end()) orig = it->second;
    }

    int r;
    if (orig) {
        r = orig(name, iSize, pbuf);
    } else {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true))
            xlog::Write("WARNING: no original handler for %s, message passed through",
                        name ? name : "?");
        r = 1;
    }

    try {
        if (g_cfg.enabled && pbuf && iSize > 0 && name) {
            ChatItem item;
            if (ExtractChat(name, pbuf, iSize, item)) {
                if (IsOwnMessage(item.clientIndex) && !g_cfg.translateOwn) {
                    xlog::Write("skipped (own message, translate_own=0): %.80s",
                                item.text.c_str());
                } else if (!item.playerChat && !g_cfg.serverMsgs) {
                    xlog::Write("skipped (server message, player chat only): %.80s",
                                item.text.c_str());
                } else {
                    if (item.name.empty() && IsOwnMessage(item.clientIndex)) {
                        std::string me = LocalPlayerName();
                        if (!me.empty()) item.name = me;
                    }
                    xlog::Write("caught [%s] idx=%d%s%s name='%s' text='%.100s'", name,
                                item.clientIndex,
                                IsOwnMessage(item.clientIndex) ? " (mine)" : "",
                                item.playerChat ? "" : " (server)",
                                item.name.c_str(), item.text.c_str());
                    JobPush(item);
                }
            }
        }
    } catch (...) {
    }
    return r;
}

static std::string CmdArg(int i) {
    if (!g_cmdArgc || !g_cmdArgv) return std::string();
    int n = g_cmdArgc();
    if (i < 0 || i >= n) return std::string();
    char* a = g_cmdArgv(i);
    return a ? std::string(a) : std::string();
}

static std::string InputToUtf8(const std::string& s) {
    if (s.empty() || LooksLikeUtf8(s)) return s;
    int w = MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (w <= 0) return s;
    std::wstring ws((size_t)w, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), &ws[0], w);
    int u = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), w, nullptr, 0, nullptr, nullptr);
    if (u <= 0) return s;
    std::string out((size_t)u, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), w, &out[0], u, nullptr, nullptr);
    return out;
}

static bool LooksLikeLang(const std::string& s) {
    if (s.size() < 2 || s.size() > 4) return false;
    for (char c : s)
        if (!isalpha((unsigned char)c)) return false;
    return true;
}

static void Cmd_TranslateSayEx(bool team) {
    std::string cmdName = CmdArg(0);
    if (cmdName.empty()) cmdName = team ? "translate_teamsay" : "translate_say";
    const std::string usage = " usage: " + cmdName +
                              " <lang> <text>   e.g. " + cmdName + " en hello all";
    int argc = g_cmdArgc ? g_cmdArgc() : 0;
    if (argc < 2) {
        ConsoleWriteNow(g_cfg.prefix + usage);
        return;
    }

    std::string first = CmdArg(1);
    bool hasLang = LooksLikeLang(first);
    if (hasLang && argc < 3) {
        ConsoleWriteNow(g_cfg.prefix + usage);
        return;
    }
    const std::string lang = hasLang ? first : GetDstLang();
    int start = hasLang ? 2 : 1;

    std::string text;
    for (int i = start; i < argc; i++) {
        if (!text.empty()) text += ' ';
        text += CmdArg(i);
    }
    text = InputToUtf8(text);
    if (text.empty()) {
        ConsoleWriteNow(g_cfg.prefix + usage);
        return;
    }

    xlog::Write("%s command: lang=%s '%.80s'", cmdName.c_str(), lang.c_str(),
                text.c_str());
    ConsoleWriteNow(EncodeForOutput(g_cfg.prefix + " translating to " + lang + ": " +
                                    text + (team ? "  (team)" : "")));
    SayPush(text, lang, team);
}

static void __cdecl Cmd_TranslateSay() { Cmd_TranslateSayEx(false); }
static void __cdecl Cmd_TranslateTeamSay() { Cmd_TranslateSayEx(true); }

static void __cdecl Cmd_TranslateOwn() {
    const std::string key = "translate_own";
    std::string v = CmdArg(1);
    if (v.empty()) {
        ConsoleWriteNow(g_cfg.prefix + " translate_own = " + GetFlag(key) +
                        "   (set: translate_own 1)");
        return;
    }
    std::string err;
    if (!SetFlag(key, v, err)) {
        ConsoleWriteNow(g_cfg.prefix + " translate_own error: " + err);
        return;
    }
    ConsoleWriteNow(g_cfg.prefix + " translate_own -> " + GetFlag(key) + "  (saved)");
}

static void __cdecl Cmd_TranslateWords() {
    const std::string key = "show_words";
    std::string v = CmdArg(1);
    if (v.empty()) {
        ConsoleWriteNow(g_cfg.prefix + " word meanings = " + GetFlag(key) +
                        "   (set: translate_words 0)");
        return;
    }
    std::string err;
    if (!SetFlag(key, v, err)) {
        ConsoleWriteNow(g_cfg.prefix + " translate_words error: " + err);
        return;
    }
    ConsoleWriteNow(g_cfg.prefix + " word meanings -> " + GetFlag(key) + "  (saved)");
}

void PrintCommandList() {
    ConsoleWriteNow(g_cfg.prefix + " commands (type in console):");
    ConsoleWriteNow("  translate_lang [code]   show / set target language  e.g. translate_lang en");
    ConsoleWriteNow("  translate_src [code]    source filter (auto = all)   e.g. translate_src ru");
    ConsoleWriteNow("  translate_say <lang> <text>   translate your text and say it in game");
    ConsoleWriteNow("  translate_teamsay <lang> <text>  team chat (alias: translate_sayteam)");
    ConsoleWriteNow("  translate_own [0|1]   also translate your own messages");
    ConsoleWriteNow("  translate_words [0|1] word-by-word meanings on/off");
    ConsoleWriteNow("  translate_hudtime [sec]  seconds each message stays on HUD (2-30)");
    ConsoleWriteNow("  translate_active [0|1] pause / resume translating");
    ConsoleWriteNow("  translate_hud [0|1]  colored HUD lines bottom-right on/off");
    ConsoleWriteNow("  translate_console [0|1]  translations in the console on/off");
    ConsoleWriteNow("  translate_hudpos <x> <y>  move the HUD (pixels, top-left origin)");
    ConsoleWriteNow("  translate_hudpos default  back to bottom right");
    ConsoleWriteNow("  translate_test         print test lines (white + colored)");
    ConsoleWriteNow("  translate_help         show this list");
    ConsoleWriteNow(g_cfg.prefix + " translations: console (white) + HUD bottom-right (colored)");
}

static void __cdecl Cmd_Help() { PrintCommandList(); }

static void __cdecl Cmd_Test() {
    xlog::Write("translate_test ran (console test)");
    ConsoleWriteNow(g_cfg.prefix + " test: console write OK (Con_Printf)");
    ConsoleWriteNow(g_cfg.prefix + " test: console is white - colors go to the overlay");
    int blk = NewOverlayBlockId();
    OverlayPush(g_cfg.prefix + " test: GOLD", 255, 205, 60, blk);
    OverlayPush(g_cfg.prefix + " test: LIGHT BLUE", 150, 220, 255, blk);
    OverlayPush(g_cfg.prefix + " test: GREEN", 120, 255, 130, blk);
}
static bool g_cmdRegistered = false;

static void __cdecl Cmd_Lang() {
    std::string v = CmdArg(1);
    if (v.empty()) {
        ConsoleWriteNow(g_cfg.prefix + " target language: " + GetDstLang() +
                        "   (set: translate_lang en)");
        return;
    }
    std::string err;
    if (!SetLang("translate_lang", v, err)) {
        ConsoleWriteNow(g_cfg.prefix + " translate_lang error: " + err);
        return;
    }
    ConsoleWriteNow(g_cfg.prefix + " target language -> " + GetDstLang() + "  (saved)");
}

static void __cdecl Cmd_Src() {
    std::string v = CmdArg(1);
    if (v.empty()) {
        ConsoleWriteNow(g_cfg.prefix + " source filter: " + GetSrcLang() +
                        "   (set: translate_src auto)");
        return;
    }
    std::string err;
    if (!SetLang("src_lang", v, err)) {
        ConsoleWriteNow(g_cfg.prefix + " translate_src error: " + err);
        return;
    }
    ConsoleWriteNow(g_cfg.prefix + " source filter -> " + GetSrcLang() + "  (saved)");
}

static void __cdecl Cmd_HudTime() {
    std::string v = CmdArg(1);
    if (v.empty()) {
        ConsoleWriteNow(g_cfg.prefix + " HUD time per message: " +
                        std::to_string(GetHudTime()) +
                        " sec   (set: translate_hudtime 10)");
        return;
    }
    std::string err;
    if (!SetHudTime(v, err)) {
        ConsoleWriteNow(g_cfg.prefix + " translate_hudtime error: " + err);
        return;
    }
    ConsoleWriteNow(g_cfg.prefix + " HUD time -> " + std::to_string(GetHudTime()) +
                    " sec  (saved)");
}

static void __cdecl Cmd_Active() {
    std::string v = CmdArg(1);
    if (v.empty()) {
        ConsoleWriteNow(g_cfg.prefix + " translating: " + GetFlag("enabled") +
                        "   (set: translate_active 0)");
        return;
    }
    std::string err;
    if (!SetFlag("enabled", v, err)) {
        ConsoleWriteNow(g_cfg.prefix + " translate_active error: " + err);
        return;
    }
    std::string on = GetFlag("enabled");
    ConsoleWriteNow(g_cfg.prefix + " translating -> " + on +
                    (on == "1" ? "  (active, saved)" : "  (paused, saved)"));
}

static void ClearOverlayNow();
static void __cdecl Cmd_Hud() {
    std::string v = CmdArg(1);
    if (v.empty()) {
        ConsoleWriteNow(g_cfg.prefix + " HUD overlay: " + GetFlag("show_overlay") +
                        "   (set: translate_hud 0)");
        return;
    }
    std::string err;
    if (!SetFlag("show_overlay", v, err)) {
        ConsoleWriteNow(g_cfg.prefix + " translate_hud error: " + err);
        return;
    }
    std::string on = GetFlag("show_overlay");
    if (on == "0") ClearOverlayNow();
    ConsoleWriteNow(g_cfg.prefix + " HUD overlay -> " + on + "  (saved)");
}

static void __cdecl Cmd_Console() {
    std::string v = CmdArg(1);
    if (v.empty()) {
        ConsoleWriteNow(g_cfg.prefix + " console output: " + GetFlag("show_console") +
                        "   (set: translate_console 0)");
        return;
    }
    std::string err;
    if (!SetFlag("show_console", v, err)) {
        ConsoleWriteNow(g_cfg.prefix + " translate_console error: " + err);
        return;
    }
    ConsoleWriteNow(g_cfg.prefix + " console output -> " + GetFlag("show_console") +
                    "  (saved)");
}

static void __cdecl Cmd_HudPos() {
    int x = 0, y = 0;
    GetHudPos(x, y);
    std::string a = CmdArg(1), b = CmdArg(2);

    if (a.empty()) {
        char buf[176];
        if (x < 0)
            snprintf(buf, sizeof(buf),
                     " HUD position: bottom right (default)   (set: translate_hudpos "
                     "40 400 | translate_hudpos default)");
        else
            snprintf(buf, sizeof(buf),
                     " HUD position: x=%d y=%d   (set: translate_hudpos 40 400 | "
                     "translate_hudpos default)",
                     x, y);
        ConsoleWriteNow(g_cfg.prefix + buf);
        return;
    }

    std::string err;
    bool ok;
    if (a == "default" || a == "reset") {
        ok = SetHudPos(-1, -1, err);
    } else if (b.empty()) {
        err = "usage: translate_hudpos <x> <y>  (pixels from the top-left corner)";
        ok = false;
    } else {
        ok = SetHudPos(atoi(a.c_str()), atoi(b.c_str()), err);
    }
    if (!ok) {
        ConsoleWriteNow(g_cfg.prefix + " translate_hudpos error: " + err);
        return;
    }
    GetHudPos(x, y);
    char buf[176];
    if (x < 0)
        snprintf(buf, sizeof(buf), " HUD position -> bottom right (default)  (saved)");
    else
        snprintf(buf, sizeof(buf), " HUD position -> x=%d y=%d  (saved)", x, y);
    ConsoleWriteNow(g_cfg.prefix + buf);
}

void RegisterCommandsNow() {
    try {
        CaptureSlots(g_engfuncs);
        if (g_cmdRegistered) return;
        if (g_addCommand) {
            g_addCommand("translate_test", &Cmd_Test);
            g_addCommand("translate_lang", &Cmd_Lang);
            g_addCommand("translate_src", &Cmd_Src);
            g_addCommand("translate_say", &Cmd_TranslateSay);
            g_addCommand("translate_teamsay", &Cmd_TranslateTeamSay);
            g_addCommand("translate_sayteam", &Cmd_TranslateTeamSay);
            g_addCommand("translate_own", &Cmd_TranslateOwn);
            g_addCommand("translate_words", &Cmd_TranslateWords);
            g_addCommand("translate_hudtime", &Cmd_HudTime);
            g_addCommand("translate_active", &Cmd_Active);
            g_addCommand("translate_hud", &Cmd_Hud);
            g_addCommand("translate_console", &Cmd_Console);
            g_addCommand("translate_hudpos", &Cmd_HudPos);
            g_addCommand("translate_help", &Cmd_Help);
            g_cmdRegistered = true;
            xlog::Write(
                "commands registered: translate_test, translate_lang, translate_src, "
                "translate_say, translate_teamsay, translate_own, translate_words, "
                "translate_hudtime, "
                "translate_active, translate_hud, translate_console, translate_hudpos, "
                "translate_help");
        } else {
            xlog::Write("WARNING: pfnAddCommand missing, commands could not be registered");
        }
    } catch (...) {
    }
}

void ConsoleWriteNow(const std::string& text) {
    try {
        CaptureSlots(g_engfuncs);
        if (!g_conPrintf && !g_consolePrint) {
            xlog::Write("WARNING: no console function for immediate write");
            return;
        }
        std::string s = text;
        s.push_back('\n');
        if (g_conPrintf)
            g_conPrintf("%s", s.c_str());
        else
            g_consolePrint(s.c_str());
        xlog::Write("console (immediate): %.100s", text.c_str());
    } catch (...) {
        xlog::Write("WARNING: immediate console write threw an exception");
    }
}

struct OverlayBlock {
    std::vector<OverlayLine> lines;
    DWORD expire = 0;
};
static std::vector<OverlayBlock> g_overlay;
static std::deque<OverlayBlock> g_overlayWait;

static void ClearOverlayNow() {
    g_overlay.clear();
    g_overlayWait.clear();
}
static const int kOverlayMaxLines = 6;
static const int kOverlayMaxWaiting = 8;
static const int kOverlayLineH = 13;
static const int kOverlayCharW = 9;

static int HudAnchorX(int screenW) {
    int hx = 0, hy = 0;
    GetHudPos(hx, hy);
    int x = (hx >= 0) ? hx : screenW - 700;
    if (x < 8) x = 8;
    if (x > screenW - 160) x = screenW - 160;
    return x;
}

static int HudAnchorY(int screenH) {
    int hx = 0, hy = 0;
    GetHudPos(hx, hy);
    int y = (hy >= 0) ? hy : screenH - 130;
    if (y < 8) y = 8;
    if (y > screenH - 20) y = screenH - 20;
    return y;
}

static void WrapLine(const std::string& s, int maxChars,
                     std::vector<std::string>& out) {
    if (maxChars < 16) maxChars = 16;
    if ((int)s.size() <= maxChars) {
        out.push_back(s);
        return;
    }
    size_t i = 0;
    while (i < s.size()) {
        size_t limit = i + (size_t)maxChars;
        if (limit >= s.size()) {
            out.push_back(s.substr(i));
            break;
        }
        size_t cut = limit;
        while (cut > i && s[cut] != ' ') cut--;
        if (cut == i) {
            cut = limit;
            while (cut > i && ((unsigned char)s[cut] & 0xC0) == 0x80) cut--;
            if (cut == i) cut = limit;
        }
        out.push_back(s.substr(i, cut - i));
        i = cut;
        while (i < s.size() && s[i] == ' ') i++;
    }
}

static int VisibleLines() {
    int n = 0;
    for (const auto& b : g_overlay) n += (int)b.lines.size();
    return n;
}

static void OfferBlock(std::vector<OverlayLine>&& lines) {
    if (lines.empty()) return;
    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int maxChars = (screenW - HudAnchorX(screenW) - 8) / kOverlayCharW;
    {
        std::vector<OverlayLine> wrapped;
        wrapped.reserve(lines.size());
        for (const auto& l : lines) {
            std::vector<std::string> parts;
            WrapLine(l.text, maxChars, parts);
            for (auto& p : parts) {
                OverlayLine o = l;
                o.text = std::move(p);
                wrapped.push_back(std::move(o));
            }
        }
        lines = std::move(wrapped);
    }
    if (VisibleLines() + (int)lines.size() <= kOverlayMaxLines) {
        const int n = (int)lines.size();
        OverlayBlock b;
        b.lines = std::move(lines);
        b.expire = GetTickCount() + (DWORD)GetHudTime() * 1000;
        g_overlay.push_back(std::move(b));
        xlog::Write("HUD: showing %d line(s) (visible=%d, ttl=%ds)", n, VisibleLines(),
                    GetHudTime());
        return;
    }
    if ((int)g_overlayWait.size() >= kOverlayMaxWaiting) {
        xlog::Write("HUD queue full, message dropped (queue=%d)", kOverlayMaxWaiting);
        return;
    }
    const int n = (int)lines.size();
    OverlayBlock b;
    b.lines = std::move(lines);
    g_overlayWait.push_back(std::move(b));
    xlog::Write("HUD: %d line(s) queued (waiting=%d)", n, (int)g_overlayWait.size());
}

static void OverlayAddBatch(const std::vector<OutLine>& lines) {
    std::vector<int> ids;
    std::vector<std::vector<OverlayLine>> groups;
    int consoleOnly = 0;
    for (const auto& l : lines) {
        if (!l.chat) {
            consoleOnly++;
            continue;
        }
        size_t gi = ids.size();
        for (size_t i = 0; i < ids.size(); i++)
            if (ids[i] == l.block) { gi = i; break; }
        if (gi == ids.size()) {
            ids.push_back(l.block);
            groups.push_back(std::vector<OverlayLine>());
        }
        OverlayLine o;
        o.text = l.text;
        o.r = l.r;
        o.g = l.g;
        o.b = l.b;
        o.chat = l.chat;
        groups[gi].push_back(std::move(o));
    }
    if (consoleOnly)
        xlog::Write("HUD: %d line(s) kept console-only (not player chat)", consoleOnly);
    for (auto& g : groups) OfferBlock(std::move(g));
}

static void PumpOverlay() {
    DWORD now = GetTickCount();
    for (size_t i = 0; i < g_overlay.size();) {
        if ((long)(now - g_overlay[i].expire) >= 0)
            g_overlay.erase(g_overlay.begin() + (long)i);
        else
            i++;
    }
    while (!g_overlayWait.empty() &&
           VisibleLines() + (int)g_overlayWait.front().lines.size() <= kOverlayMaxLines) {
        OverlayBlock b = std::move(g_overlayWait.front());
        g_overlayWait.pop_front();
        b.expire = GetTickCount() + (DWORD)GetHudTime() * 1000;
        g_overlay.push_back(std::move(b));
    }
}

void DrainConsole() {
    std::vector<OutLine> lines;
    OutDrain(lines);
    for (const auto& l : lines) {
        if (l.toConsole && g_cfg.showConsole) {
            xlog::Write("wrote to console: %.120s", l.text.c_str());

            std::string s = l.text;
            s.push_back('\n');
            if (g_conPrintf)
                g_conPrintf("%s", s.c_str());
            else if (g_consolePrint)
                g_consolePrint(s.c_str());
            else
                xlog::Write("WARNING: no console write function (Con_Printf and ConsolePrint invalid)");

            if (l.center && g_centerPrint) g_centerPrint(l.text.c_str());
        }
    }

    if (g_cfg.showOverlay) OverlayAddBatch(lines);
}

static void DrawOverlay() {
    if (!g_cfg.showOverlay) return;
    if (!g_drawString) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            xlog::Write("WARNING: DrawString slot missing - HUD overlay cannot draw");
        }
        return;
    }

    PumpOverlay();
    if (g_overlay.empty()) return;

    static bool drawn = false;
    if (!drawn) {
        drawn = true;
        xlog::Write("HUD: first frame drawn (%d block(s))", (int)g_overlay.size());
    }

    int w = GetSystemMetrics(SM_CXSCREEN);
    int h = GetSystemMetrics(SM_CYSCREEN);
    int x = HudAnchorX(w);
    int y = HudAnchorY(h);
    for (const auto& b : g_overlay) {
        for (const auto& l : b.lines) {
            g_drawString(x, y, l.text.c_str(), (int)l.r, (int)l.g, (int)l.b);
            y += kOverlayLineH;
        }
    }
}

static void FlushSayQueue() {
    if (!g_clientCmd) return;
    std::vector<SayDone> done;
    SayOutDrain(done);
    for (const auto& d : done) {
        std::string payload = EncodeForOutput(d.text);
        if (payload.size() > 120) {
            payload.resize(120);
            while (!payload.empty() && ((unsigned char)payload.back() & 0xC0) == 0x80)
                payload.pop_back();
        }
        std::string cmd = (d.team ? "say_team " : "say ") + payload;
        xlog::Write("sending %s [%s]: %.80s", d.team ? "say_team" : "say",
                    d.lang.c_str(), payload.c_str());
        std::vector<char> buf(cmd.begin(), cmd.end());
        buf.push_back('\0');
        g_clientCmd(buf.data());

        std::string ok = g_cfg.prefix + " said (" + d.lang + "): " + d.text;
        ConsoleWriteNow(ok);
        OverlayPush(EncodeForOutput(ok), 120, 255, 130, NewOverlayBlockId());
    }
}

static int __cdecl Hook_HUDRedraw(float t, int inter) {
    try {
        static bool first = false;
        if (!first) {
            first = true;
            xlog::Write("HUD_Redraw first call -> console dump and screen drawing active");
        }
        if (g_hClient) EnsureSayTextHook(g_hClient);
        FlushSayQueue();
        if (g_cfg.testChat) {
            static ULONGLONG t0 = 0;
            static bool sent = false;
            static ULONGLONG beat = 0;
            if (!t0) {
                t0 = GetTickCount64();
                xlog::Write("test_chat armed (clientCmd=%p)", (void*)g_clientCmd);
            }
            const ULONGLONG now = GetTickCount64();
            if (now - beat > 5000) {
                beat = now;
                xlog::Write("test_chat beat: %.1fs after first redraw", (now - t0) / 1000.0);
            }
            if (!sent && now - t0 > 8000) {
                sent = true;
                if (!g_clientCmd) {
                    xlog::Write("test_chat: ClientCmd slot missing, say skipped");
                    return g_realHUDRedraw ? g_realHUDRedraw(t, inter) : 1;
                }
                xlog::Write("test_chat: sending selftest say");
                const char* m = "say hello how are you";
                std::vector<char> buf(m, m + strlen(m) + 1);
                g_clientCmd(buf.data());
            }
        }
        DrainConsole();
        DrawOverlay();
    } catch (...) {
    }
    return g_realHUDRedraw ? g_realHUDRedraw(t, inter) : 1;
}

static void* g_hudRedrawSlot = nullptr;
static void* g_saySlot = nullptr;

static uintptr_t FindStringAddr(HMODULE h, const char* needle) {
    if (!h || !needle || !*needle) return 0;
    const size_t nlen = strlen(needle);
    Section sec{};
    const char* secs[] = {".rdata", ".data"};
    for (int s = 0; s < 2; s++) {
        if (!GetSection(h, secs[s], sec)) continue;
        BYTE* base = (BYTE*)h + sec.va;
        for (size_t off = 0; off + nlen + 1 <= sec.vsize; off++) {
            if (base[off] != (BYTE)needle[0]) continue;
            if (memcmp(base + off, needle, nlen) == 0 && base[off + nlen] == 0)
                return (uintptr_t)(base + off);
        }
    }
    return 0;
}

static void* FindUserMsgSlot(const char* name, HMODULE hClient) {
    HMODULE hw = GetModuleHandleA("hw.dll");
    if (!hw) hw = GetModuleHandleA("sw.dll");
    if (!hw) return nullptr;
    const uintptr_t strAddr = FindStringAddr(hw, name);
    if (!strAddr) return nullptr;
    Section sec{};
    const char* secs[] = {".data", ".rdata"};
    for (int s = 0; s < 2; s++) {
        if (!GetSection(hw, secs[s], sec)) continue;
        BYTE* base = (BYTE*)hw + sec.va;
        for (size_t off = 0; off + 8 <= sec.vsize; off += 4) {
            auto* p = (void**)(base + off);
            if ((uintptr_t)p[0] != strAddr) continue;
            void* fn = p[1];
            if (!IsValidCodePtr(fn)) continue;
            if (hClient && ModuleOfPtr(fn) != hClient) continue;
            return (void*)&p[1];
        }
    }
    return nullptr;
}

static void EnsureSayTextHook(HMODULE hClient) {
    if (!g_saySlot) g_saySlot = FindUserMsgSlot("SayText", hClient);
    if (!g_saySlot) return;
    void* cur = *(void**)g_saySlot;
    if (cur == (void*)&Hook_UserMsg) return;
    if (IsValidCodePtr(cur)) {
        std::lock_guard<std::mutex> lk(g_umMtx);
        g_origUserMsg["SayText"] = (UserMsgHook)cur;
    }
    if (WriteSlot(g_saySlot, (void*)&Hook_UserMsg))
        xlog::Write("SayText handler installed in usermsg table (data only)");
}

static bool WindowHasClientFuncs(void* slot, HMODULE hClient, int span) {
    if (!slot || !hClient) return false;
    void* pInit = (void*)GetProcAddress(hClient, "Initialize");
    void* pHudInit = (void*)GetProcAddress(hClient, "HUD_Init");
    void* pRedraw = (void*)GetProcAddress(hClient, "HUD_Redraw");
    if (!pRedraw) return false;
    auto* base = (void**)slot;
    bool sawInit = false, sawHudInit = false;
    for (int i = -span; i <= span; i++) {
        void* p = base[i];
        if (!IsValidCodePtr(p)) continue;
        if (ModuleOfPtr(p) != hClient) continue;
        if (p == pInit) sawInit = true;
        if (p == pHudInit) sawHudInit = true;
        if (sawInit && sawHudInit) return true;
    }
    return false;
}

static void* FindHudRedrawSlot(HMODULE hClient) {
    void* pRedraw = (void*)GetProcAddress(hClient, "HUD_Redraw");
    if (!pRedraw) return nullptr;

    const char* mods[] = {"hw.dll", "sw.dll", nullptr};
    for (int m = 0; mods[m]; m++) {
        HMODULE h = GetModuleHandleA(mods[m]);
        if (!h) continue;
        Section sec{};
        const char* names[] = {".data", ".rdata"};
        for (int si = 0; si < 2; si++) {
            if (!GetSection(h, names[si], sec)) continue;
            BYTE* base = (BYTE*)h + sec.va;
            for (uintptr_t off = 0; off + sizeof(void*) <= sec.vsize;
                 off += sizeof(void*)) {
                auto* p = (void**)(base + off);
                if (*p != pRedraw) continue;
                void* slot = (void*)(base + off);
                if (!WindowHasClientFuncs(slot, hClient, 96)) continue;
                xlog::Write("client func table found (%s+0x%X), HUD_Redraw slot %p",
                            mods[m], (unsigned)off, slot);
                return slot;
            }
        }
    }
    return nullptr;
}

static void InstallHudRedrawHook(HMODULE hClient) {
    if (g_hudRedrawSlot) return;
    void* slot = FindHudRedrawSlot(hClient);
    if (!slot) {
        xlog::Write("client func table not found - overlay drawing disabled");
        return;
    }
    void* cur = *(void**)slot;
    if (!IsValidCodePtr(cur)) {
        xlog::Write("HUD_Redraw slot has no valid pointer");
        return;
    }
    if (!g_realHUDRedraw) g_realHUDRedraw = (FnHUDRedraw)cur;
    if (WriteSlot(slot, (void*)&Hook_HUDRedraw)) {
        g_hudRedrawSlot = slot;
        xlog::Write("HUD_Redraw swapped in client func table (data only, no code patch)");
    } else {
        xlog::Write("HUD_Redraw slot write failed");
    }
}

bool IsClientHooked() { return g_clientHooked.load(); }

bool TryHookClient(HMODULE hClient, const char* via) {
    if (!hClient) return false;
    if (g_clientHooked.load()) return true;

    g_hClient = hClient;
    xlog::Write("client.dll caught: %p (via: %s)", (void*)hClient, via ? via : "?");

    if (!g_engfuncs) {
        if (g_engineTable) {
            g_engfuncs = g_engineTable;
        } else {
            g_engfuncs = ResolveEngfuncs(hClient);
        }
    }
    if (!g_engfuncs) {
        xlog::Write("client: engine table not resolved yet, retrying");
        return false;
    }

    CaptureSlots(g_engfuncs);

    RegisterCommandsNow();

    if (g_engfuncs->f[engidx::HookUserMsg] != (void*)&Hook_HookUserMsg) {
        if (PatchUserMsg(g_engfuncs)) {
            xlog::Write("A) engine table pfnHookUserMsg swapped (data only)");
        } else {
            xlog::Write("A) engine table pfnHookUserMsg swap failed");
        }
    } else {
        xlog::Write("A) engine table pfnHookUserMsg already ours");
    }

    InstallHudRedrawHook(hClient);
    EnsureSayTextHook(hClient);
    g_clientHooked = true;
    return true;
}