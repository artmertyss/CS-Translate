#include "config.h"

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <mutex>
#include <sstream>

Config g_cfg;

static const char* kDefaultSlang =
    "amk:am\xc4\xb1na koyay\xc4\xb1m,"
    "aq:am\xc4\xb1na koyay\xc4\xb1m,"
    "mk:am\xc4\xb1na koyay\xc4\xb1m,"
    "o\xc3\xa7:orospu \xc3\xa7ocu\xc4\x9fu,"
    "oc:orospu \xc3\xa7ocu\xc4\x9fu";

static std::mutex g_langMtx;
static std::string g_iniPath;

static std::string IniPath(const std::string& moduleDir) {
    char buf[MAX_PATH] = {0};
    DWORD n = GetEnvironmentVariableA("CS_TRANSLATE_HOME", buf, MAX_PATH);
    std::string dir;
    if (n > 0 && n < MAX_PATH) {
        dir = std::string(buf, n);
    } else {
        n = GetEnvironmentVariableA("APPDATA", buf, MAX_PATH);
        dir = (n > 0 && n < MAX_PATH) ? std::string(buf, n) + "\\cs_translate" : moduleDir;
    }
    CreateDirectoryA(dir.c_str(), NULL);
    return dir + "\\cs_translate.ini";
}

std::string IniFilePath() { return g_iniPath; }

std::string GetDstLang() {
    std::lock_guard<std::mutex> lk(g_langMtx);
    return g_cfg.dstLang;
}

std::string GetSrcLang() {
    std::lock_guard<std::mutex> lk(g_langMtx);
    return g_cfg.srcLang;
}

std::string GetPriorityLangs() {
    std::lock_guard<std::mutex> lk(g_langMtx);
    return g_cfg.priorityLangs;
}

bool SetLang(const std::string& key, const std::string& value, std::string& err) {
    err.clear();
    std::string v;
    for (char c : value) v.push_back(c == '_' ? '-' : (char)tolower((unsigned char)c));
    size_t b = v.find_first_not_of(" \t\r");
    size_t e = v.find_last_not_of(" \t\r");
    v = (b == std::string::npos) ? std::string() : v.substr(b, e - b + 1);

    if (v.empty()) {
        err = "empty value";
        return false;
    }
    if (v.size() > 12) {
        err = "too long (example: tr, en, ru, de)";
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(g_langMtx);
        if (key == "src_lang") {
            g_cfg.srcLang = v;
        } else {
            g_cfg.dstLang = v;
        }
    }

    if (!g_iniPath.empty()) {
        if (!WritePrivateProfileStringA("Settings", key.c_str(), v.c_str(), g_iniPath.c_str()))
            err = "ini could not be saved (check file permissions)";
    }
    return true;
}

static bool ReadBool(const char* file, const char* key, bool def) {
    char buf[32];
    GetPrivateProfileStringA("Settings", key, def ? "1" : "0", buf, sizeof(buf), file);
    return atoi(buf) != 0;
}

static int ReadInt(const char* file, const char* key, int def) {
    char buf[32];
    char defbuf[32];
    snprintf(defbuf, sizeof(defbuf), "%d", def);
    GetPrivateProfileStringA("Settings", key, defbuf, buf, sizeof(buf), file);
    return atoi(buf);
}

static std::string ReadStr(const char* file, const char* key, const std::string& def) {
    char buf[512];
    GetPrivateProfileStringA("Settings", key, def.c_str(), buf, sizeof(buf), file);
    std::string s(buf);
    std::string o;
    for (char c : s) o.push_back(c == '_' ? '-' : (char)tolower((unsigned char)c));
    size_t b = o.find_first_not_of(" \t\r");
    size_t e = o.find_last_not_of(" \t\r");
    if (b == std::string::npos) return "";
    return o.substr(b, e - b + 1);
}

static std::string ReadRaw(const char* file, const char* key, const std::string& def) {
    char buf[512];
    GetPrivateProfileStringA("Settings", key, def.c_str(), buf, sizeof(buf), file);
    std::string s(buf);
    size_t b = s.find_first_not_of(" \t\r");
    size_t e = s.find_last_not_of(" \t\r");
    if (b == std::string::npos) return "";
    return s.substr(b, e - b + 1);
}

static bool* FlagPtr(const std::string& key) {
    if (key == "translate_own") return &g_cfg.translateOwn;
    if (key == "show_words") return &g_cfg.showWords;
    if (key == "show_sentence") return &g_cfg.showSentence;
    if (key == "show_overlay") return &g_cfg.showOverlay;
    if (key == "show_console") return &g_cfg.showConsole;
    if (key == "enabled") return &g_cfg.enabled;
    return nullptr;
}

std::string GetFlag(const std::string& key) {
    bool* p = FlagPtr(key);
    if (!p) return std::string();
    std::lock_guard<std::mutex> lk(g_langMtx);
    return *p ? "1" : "0";
}

bool SetFlag(const std::string& key, const std::string& value, std::string& err) {
    err.clear();
    if (!FlagPtr(key)) {
        err = "unknown option";
        return false;
    }
    std::string v;
    for (char c : value) v.push_back((char)tolower((unsigned char)c));
    if (v == "on" || v == "true" || v == "1" || v == "yes")
        v = "1";
    else if (v == "off" || v == "false" || v == "0" || v == "no")
        v = "0";
    if (v != "0" && v != "1") {
        err = "value must be 0 or 1";
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(g_langMtx);
        *FlagPtr(key) = (v == "1");
    }
    if (!g_iniPath.empty()) {
        if (!WritePrivateProfileStringA("Settings", key.c_str(), v.c_str(), g_iniPath.c_str()))
            err = "ini could not be saved (check file permissions)";
    }
    return true;
}

int GetHudTime() {
    std::lock_guard<std::mutex> lk(g_langMtx);
    return g_cfg.hudTime;
}

bool SetHudTime(const std::string& value, std::string& err) {
    err.clear();
    int v = atoi(value.c_str());
    if (v < 2 || v > 30) {
        err = "must be 2-30 seconds (example: translate_hudtime 10)";
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(g_langMtx);
        g_cfg.hudTime = v;
    }
    if (!g_iniPath.empty()) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", v);
        if (!WritePrivateProfileStringA("Settings", "hud_time", buf, g_iniPath.c_str()))
            err = "ini could not be saved (check file permissions)";
    }
    return true;
}

void GetHudPos(int& x, int& y) {
    std::lock_guard<std::mutex> lk(g_langMtx);
    x = g_cfg.hudX;
    y = g_cfg.hudY;
}

bool SetHudPos(int x, int y, std::string& err) {
    err.clear();
    if (x < -1 || y < -1 || x > 10000 || y > 10000) {
        err = "out of range (example: translate_hudpos 40 400, or translate_hudpos default)";
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(g_langMtx);
        g_cfg.hudX = x;
        g_cfg.hudY = y;
    }
    if (!g_iniPath.empty()) {
        char bx[16], by[16];
        snprintf(bx, sizeof(bx), "%d", x);
        snprintf(by, sizeof(by), "%d", y);
        if (!WritePrivateProfileStringA("Settings", "hud_x", bx, g_iniPath.c_str()) ||
            !WritePrivateProfileStringA("Settings", "hud_y", by, g_iniPath.c_str()))
            err = "ini could not be saved (check file permissions)";
    }
    return true;
}

void LoadConfig(const std::string& dir) {
    std::string path = IniPath(dir);
    g_iniPath = path;
    bool exists = (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES);

    if (!exists) {
        static const char* kLegacy[] = {"\\cs16chattranslator.ini", "\\cs_translate.ini"};
        for (const char* rel : kLegacy) {
            std::string old = dir + rel;
            if (old != path && CopyFileA(old.c_str(), path.c_str(), TRUE)) {
                exists = true;
                break;
            }
        }
    }

    if (!exists) {
        FILE* f = nullptr;
        if (fopen_s(&f, path.c_str(), "w") == 0 && f) {
            fprintf(f,
                    "[Settings]\n"
                    "; 1 = on, 0 = off\n"
                    "enabled=1\n"
                    "show_sentence=1\n"
                    "show_words=0\n"
                    "max_words=6\n"
                    "; also write to the center of the screen (0/1)\n"
                    "show_center=0\n"
                    "; also show the colored lines bottom-right (0/1)\n"
                    "show_overlay=1\n"
                    "; write the translations to the console (0/1)\n"
                    "show_console=1\n"
                    "; seconds each message stays on the HUD before the next\n"
                    "; queued one replaces it (2-30)\n"
                    "hud_time=8\n"
                    "; HUD position in pixels from the top-left corner\n"
                    "; (-1 = anchored bottom right)\n"
                    "hud_x=10\n"
                    "hud_y=200\n"
                    "\n"
                    "; console encoding: utf8 | cp1251 | ascii\n"
                    ";   utf8   = raw UTF-8 (default, the CS 1.6 font is UTF-8)\n"
                    ";   cp1251 = one byte per letter (only if utf8 shows boxes)\n"
                    ";   ascii  = full transliteration (if the font shows none of them)\n"
                    "charset=utf8\n"
                    "prefix=[TR]\n"
                    "\n"
                    "; which chat messages to listen to (comma separated)\n"
                    "watch=SayText,TextMsg\n"
                    "; translate server / plugin announcements too (0/1)\n"
                    "; 0 = only real player chat is translated (they never\n"
                    ";     reach the HUD either way)\n"
                    "server_msgs=0\n"
                    "; also translate our own messages (0/1)\n"
                    "; word-by-word meanings apply the same way\n"
                    "translate_own=1\n"
                    "; TEST: if 1, we send a say ourselves in a map and\n"
                    "; verify the capture chain automatically (normally 0)\n"
                    "test_chat=0\n"
                    "\n"
                    "; target language to translate to: en | tr | ru | de | fr ...\n"
                    "translate_lang=en\n"
                    "; source language filter: auto = all languages (Google detects).\n"
                    "; to allow only one language, e.g. set src_lang=ru.\n"
                    "src_lang=auto\n"
                    "; priority languages (comma separated). Nothing is filtered:\n"
                    "; every language is still translated. Only when Google guesses\n"
                    "; something OUTSIDE this list (short words are often wrong), the\n"
                    "; source is re-assumed from this list (Cyrillic -> ru, Turkish\n"
                    "; letters -> tr, otherwise en). Empty = no hints.\n"
                    "priority_langs=tr,ru,bg,ro,en\n"
                    "\n"
                    "; minimum letters before a message is translated (1-10)\n"
                    "; 2 = hi / no / ok are translated too\n"
                    "min_letters=2\n"
                    "; words never shown in the word-by-word glossary line\n"
                    "; (game jargon - the sentence itself is still translated)\n"
                    "keep_words=boost,runboost,rayish,hns,sbj\n");
            fprintf(f,
                    "; chat slang kept as-is in the translation, with its\n"
                    "; meaning in parentheses (word:meaning, comma separated)\n"
                    "slang_words=%s\n"
                    "; show the meaning next to slang words (0/1)\n"
                    "slang_notes=1\n"
                    "; 1 = write cs_translate.log next to the .asi (0/1)\n"
                    "log=0\n",
                    kDefaultSlang);
            fclose(f);
        }
    }

    g_cfg.enabled = ReadBool(path.c_str(), "enabled", g_cfg.enabled);
    g_cfg.showSentence = ReadBool(path.c_str(), "show_sentence", g_cfg.showSentence);
    g_cfg.showWords = ReadBool(path.c_str(), "show_words", g_cfg.showWords);
    g_cfg.showCenter = ReadBool(path.c_str(), "show_center", g_cfg.showCenter);
    g_cfg.showOverlay = ReadBool(path.c_str(), "show_overlay", g_cfg.showOverlay);
    g_cfg.showConsole = ReadBool(path.c_str(), "show_console", g_cfg.showConsole);
    g_cfg.translateOwn = ReadBool(path.c_str(), "translate_own", g_cfg.translateOwn);
    g_cfg.serverMsgs = ReadBool(path.c_str(), "server_msgs", g_cfg.serverMsgs);
    g_cfg.testChat = ReadBool(path.c_str(), "test_chat", g_cfg.testChat);
    g_cfg.log = ReadBool(path.c_str(), "log", g_cfg.log);
    g_cfg.maxWords = ReadInt(path.c_str(), "max_words", g_cfg.maxWords);
    g_cfg.hudTime = ReadInt(path.c_str(), "hud_time", g_cfg.hudTime);
    if (g_cfg.hudTime < 2) g_cfg.hudTime = 2;
    if (g_cfg.hudTime > 30) g_cfg.hudTime = 30;
    g_cfg.hudX = ReadInt(path.c_str(), "hud_x", g_cfg.hudX);
    g_cfg.hudY = ReadInt(path.c_str(), "hud_y", g_cfg.hudY);
    if (g_cfg.hudX < -1) g_cfg.hudX = -1;
    if (g_cfg.hudY < -1) g_cfg.hudY = -1;
    g_cfg.charset = ReadStr(path.c_str(), "charset", g_cfg.charset);
    g_cfg.prefix = ReadRaw(path.c_str(), "prefix", g_cfg.prefix);
    g_cfg.dstLang = ReadStr(path.c_str(), "translate_lang", "");
    if (g_cfg.dstLang.empty())
        g_cfg.dstLang = ReadStr(path.c_str(), "dst_lang", g_cfg.dstLang);
    if (g_cfg.dstLang.empty()) g_cfg.dstLang = "en";
    g_cfg.srcLang = ReadStr(path.c_str(), "src_lang", g_cfg.srcLang);
    if (g_cfg.srcLang.empty()) g_cfg.srcLang = "auto";
    g_cfg.priorityLangs = ReadStr(path.c_str(), "priority_langs", g_cfg.priorityLangs);

    g_cfg.minLetters = ReadInt(path.c_str(), "min_letters", g_cfg.minLetters);
    if (g_cfg.minLetters < 1) g_cfg.minLetters = 1;
    if (g_cfg.minLetters > 10) g_cfg.minLetters = 10;

    {
        const std::string kw =
            ReadStr(path.c_str(), "keep_words", "boost,runboost,rayish,hns,sbj");
        g_cfg.keep.clear();
        std::stringstream ss(kw);
        std::string item;
        while (std::getline(ss, item, ',')) {
            size_t b = item.find_first_not_of(" \t");
            size_t e = item.find_last_not_of(" \t\r");
            if (b == std::string::npos) continue;
            std::string k = item.substr(b, e - b + 1);
            for (char& ch : k)
                if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
            if (!k.empty()) g_cfg.keep.push_back(k);
        }
    }

    g_cfg.slangNotes = ReadBool(path.c_str(), "slang_notes", g_cfg.slangNotes);
    {
        const std::string sw = ReadStr(path.c_str(), "slang_words", kDefaultSlang);
        g_cfg.slang.clear();
        std::stringstream ss(sw);
        std::string item;
        while (std::getline(ss, item, ',')) {
            size_t c = item.find(':');
            if (c == std::string::npos || c == 0) continue;
            size_t b = item.find_first_not_of(" \t");
            if (b == std::string::npos || b >= c) continue;
            size_t e = item.find_last_not_of(" \t\r");
            size_t vb = item.find_first_not_of(" \t", c + 1);
            if (e == std::string::npos || vb == std::string::npos || vb > e) continue;
            std::string k = item.substr(b, c - b);
            std::string v = item.substr(vb, e - vb + 1);
            for (char& ch : k)
                if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
            if (!k.empty() && !v.empty()) g_cfg.slang.push_back({k, v});
        }
    }

    std::string w = ReadStr(path.c_str(), "watch", "SayText,TextMsg");
    if (!w.empty()) {
        g_cfg.watch.clear();
        std::stringstream ss(w);
        std::string item;
        while (std::getline(ss, item, ',')) {
            size_t b = item.find_first_not_of(" \t");
            size_t e = item.find_last_not_of(" \t\r");
            if (b == std::string::npos) continue;
            g_cfg.watch.push_back(item.substr(b, e - b + 1));
        }
    }

    if (g_cfg.maxWords < 0) g_cfg.maxWords = 0;
    if (g_cfg.maxWords > 30) g_cfg.maxWords = 30;
}
