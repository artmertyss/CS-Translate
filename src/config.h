#pragma once
#include <string>
#include <utility>
#include <vector>

struct Config {
    bool enabled = true;

    bool showSentence = true;
    bool showWords = false;
    int  maxWords = 6;
    bool showCenter = false;
    bool showOverlay = true;
    bool showConsole = true;
    int  hudTime = 8;
    int  hudX = 20;
    int  hudY = 200;
    bool log = true;

    std::string prefix = "[TR]";

    std::string charset = "utf8";

    std::string srcLang = "auto";
    std::string dstLang = "en";

    std::string priorityLangs = "tr,ru,bg,ro,en";

    std::vector<std::string> watch = {"SayText", "TextMsg"};

    int minLetters = 2;
    std::vector<std::string> keep = {"boost", "runboost", "rayish", "hns", "sbj"};

    bool slangNotes = true;
    std::vector<std::pair<std::string, std::string>> slang;

    unsigned gEngfuncsRva = 0x125CC0;

    bool translateOwn = false;
    bool serverMsgs = false;

    bool testChat = false;
};

extern Config g_cfg;

std::string GetDstLang();
std::string GetSrcLang();
std::string GetPriorityLangs();
bool SetLang(const std::string& key, const std::string& value, std::string& err);

bool SetFlag(const std::string& key, const std::string& value, std::string& err);
std::string GetFlag(const std::string& key);

int GetHudTime();
bool SetHudTime(const std::string& value, std::string& err);

void GetHudPos(int& x, int& y);
bool SetHudPos(int x, int y, std::string& err);

void LoadConfig(const std::string& dir);
std::string IniFilePath();
