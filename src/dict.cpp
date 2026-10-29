#include "dict.h"

#include <cctype>
#include <cstdio>
#include <cstring>

#include "utf8.h"

namespace {

const char kDictJson[] = R"json({
"suka": {"sl": "ru", "tr": "orospu", "en": "bitch", "ru": "сука"},
"cyka": {"sl": "ru", "tr": "orospu", "en": "bitch", "ru": "сука"},
"blyat": {"sl": "ru", "tr": "lan", "en": "fuck", "ru": "блять"},
"blya": {"sl": "ru", "tr": "lan", "en": "fuck", "ru": "бля"},
"blyad": {"sl": "ru", "tr": "orospu", "en": "whore", "ru": "блядь"},
"nahui": {"sl": "ru", "tr": "siktir et", "en": "fuck it", "ru": "нахуй"},
"naxui": {"sl": "ru", "tr": "siktir et", "en": "fuck it", "ru": "нахуй"},
"pizdec": {"sl": "ru", "tr": "berbat", "en": "fucked up", "ru": "пиздец"},
"pizda": {"sl": "ru", "tr": "amcık", "en": "cunt", "ru": "пизда"},
"pidor": {"sl": "ru", "tr": "aptal", "en": "idiot", "ru": "пидор"},
"khuy": {"sl": "ru", "tr": "yarak", "en": "dick", "ru": "хуй"},
"xuy": {"sl": "ru", "tr": "yarak", "en": "dick", "ru": "хуй"},
"huy": {"sl": "ru", "tr": "yarak", "en": "dick", "ru": "хуй"},
"zhopa": {"sl": "ru", "tr": "göt", "en": "ass", "ru": "жопа"},
"govno": {"sl": "ru", "tr": "bok", "en": "shit", "ru": "говно"},
"mraz": {"sl": "ru", "tr": "yüzsüz", "en": "bastard", "ru": "мразь"},
"blin": {"sl": "ru", "tr": "lan", "en": "damn", "ru": "блин"},
"poshol": {"sl": "ru", "tr": "siktir git", "en": "fuck off", "ru": "пошёл"},
"poshi": {"sl": "ru", "tr": "siktir git", "en": "fuck off", "ru": "поши"},
"eblan": {"sl": "ru", "tr": "salak", "en": "wanker", "ru": "еблан"},
"kurwa": {"sl": "pl", "tr": "orospu", "en": "whore"},
"amk": {"sl": "tr", "tr": "amk", "en": "fuck", "ru": "блять"},
"aq": {"sl": "tr", "tr": "aq", "en": "fuck", "ru": "блять"},
"mk": {"sl": "tr", "tr": "mk", "en": "fuck", "ru": "блять"},
"oc": {"sl": "tr", "tr": "oc", "en": "son of a bitch", "ru": "сукин сын"},
"siktir": {"sl": "tr", "tr": "siktir", "en": "fuck off", "ru": "пошёл ты"},
"sikeyim": {"sl": "tr", "tr": "sikeyim", "en": "fuck", "ru": "ебал"},
"yarrak": {"sl": "tr", "tr": "yarrak", "en": "dick", "ru": "хуй"},
"amcik": {"sl": "tr", "tr": "amcık", "en": "cunt", "ru": "пизда"},
"wdym": {"sl": "en", "tr": "ne demek istiyorsun", "en": "what do you mean"},
"idk": {"sl": "en", "keep": 1},
"brb": {"sl": "en", "keep": 1},
"lol": {"sl": "en", "keep": 1},
"lmao": {"sl": "en", "keep": 1},
"tbh": {"sl": "en", "keep": 1},
"imo": {"sl": "en", "keep": 1},
"afk": {"sl": "en", "keep": 1},
"smh": {"sl": "en", "keep": 1},
"ngl": {"sl": "en", "keep": 1},
"ez": {"sl": "en", "keep": 1},
"gg": {"keep": 1},
"wp": {"keep": 1},
"ff": {"keep": 1},
"tk": {"keep": 1},
"gj": {"keep": 1},
"gl": {"keep": 1},
"hf": {"keep": 1},
"mix": {"keep": 1},
"recap": {"keep": 1},
"cap": {"keep": 1},
"boost": {"keep": 1},
"runboost": {"keep": 1},
"rayish": {"keep": 1},
"hns": {"keep": 1},
"sbj": {"keep": 1},
"nj": {"keep": 1},
"prestrafe": {"keep": 1},
"bhop": {"keep": 1},
"bunnyhop": {"keep": 1},
"strafe": {"keep": 1},
"airstrafe": {"keep": 1},
"lj": {"keep": 1},
"cj": {"keep": 1},
"kz": {"keep": 1},
"surf": {"keep": 1},
"awp": {"keep": 1},
"deagle": {"keep": 1},
"scout": {"keep": 1},
"mp5": {"keep": 1},
"usp": {"keep": 1},
"nade": {"keep": 1},
"wallhack": {"keep": 1},
"aimbot": {"keep": 1},
"noob": {"keep": 1},
"wt0": {"keep": 1},
"ezik": {"en": "noob", "ru": "ноуб", "tr": "ezik"},
"kanka": {"en": "buddy", "ru": "братан", "tr": "kanka"},
"ala": {"sl": "ro"},
"asteapt": {"sl": "ro"},
"asteapta": {"sl": "ro"},
"keske": {"sl": "tr"},
"nbr": {"sl": "tr"},
"naber": {"sl": "tr"},
"napiyon": {"sl": "tr"},
"napiyorsun": {"sl": "tr"},
"napcan": {"sl": "tr"},
"bisey": {"sl": "tr"},
"bisi": {"sl": "tr"},
"gelcen": {"sl": "tr"},
"gidiyon": {"sl": "tr"},
"yani": {"sl": "tr"},
"abi": {"sl": "tr"},
"olum": {"sl": "tr"},
"lan": {"sl": "tr"},
"tmm": {"sl": "tr"},
"tamam": {"sl": "tr"}
})json";

std::unordered_map<std::string, DictEntry> g_dict;
bool g_loaded = false;

void SkipWs(const std::string& s, size_t& i) {
    while (i < s.size() && (unsigned char)s[i] <= ' ') i++;
}

bool Expect(const std::string& s, size_t& i, char c) {
    SkipWs(s, i);
    if (i < s.size() && s[i] == c) {
        i++;
        return true;
    }
    return false;
}

void LowerAscii(std::string& s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
}

void Load() {
    if (g_loaded) return;
    g_loaded = true;
    const std::string s = kDictJson;
    size_t i = 0;
    if (!Expect(s, i, '{')) return;
    for (;;) {
        SkipWs(s, i);
        if (i < s.size() && s[i] == '}') return;
        std::string key;
        if (!JsonDecodeString(s, i, key)) return;
        if (!Expect(s, i, ':')) return;
        if (!Expect(s, i, '{')) return;
        DictEntry e;
        for (;;) {
            SkipWs(s, i);
            if (i >= s.size() || s[i] == '}') break;
            std::string k;
            if (!JsonDecodeString(s, i, k)) return;
            if (!Expect(s, i, ':')) return;
            SkipWs(s, i);
            if (i < s.size() && s[i] == '"') {
                std::string v;
                if (!JsonDecodeString(s, i, v)) return;
                if (k == "sl")
                    e.sl = v;
                else
                    e.tr[k] = v;
            } else {
                const size_t st = i;
                while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
                if (k == "keep") e.keep = (i > st && s[st] != '0');
            }
            if (!Expect(s, i, ',')) break;
        }
        if (!Expect(s, i, '}')) return;
        LowerAscii(key);
        if (!key.empty()) g_dict[key] = std::move(e);
        if (!Expect(s, i, ',')) return;
    }
}

bool TokenJunk(const std::string& tok) {
    if (tok.empty()) return false;
    bool allDigit = true;
    for (char c : tok) {
        if (c < '0' || c > '9') {
            allDigit = false;
            break;
        }
    }
    if (allDigit) return true;
    std::string low = tok;
    LowerAscii(low);
    if (low.size() >= 6) {
        char a = 0, b = 0;
        bool two = true;
        for (char c : low) {
            if (c == a || c == b) continue;
            if (a == 0) {
                a = c;
            } else if (b == 0) {
                b = c;
            } else {
                two = false;
                break;
            }
        }
        if (two) return true;
    }
    if (low.size() >= 10) {
        bool seen[256] = {false};
        size_t uniq = 0;
        for (char c : low) {
            const unsigned char u = (unsigned char)c;
            if (!seen[u]) {
                seen[u] = true;
                uniq++;
            }
        }
        if (uniq * 2 < low.size()) return true;
    }
    return false;
}

}

const DictEntry* DictFind(const std::string& wordLower) {
    Load();
    const auto it = g_dict.find(wordLower);
    if (it == g_dict.end()) return nullptr;
    return &it->second;
}

std::string DictForceSl(const std::string& text) {
    Load();
    std::string tok;
    auto check = [&]() -> std::string {
        if (tok.empty()) return std::string();
        std::string key = tok;
        LowerAscii(key);
        const DictEntry* e = DictFind(key);
        tok.clear();
        if (e && !e->sl.empty()) return e->sl;
        return std::string();
    };
    for (char c : text) {
        const unsigned char u = (unsigned char)c;
        if (u < 0x80 && (isalnum(u) != 0)) {
            tok.push_back(c);
            continue;
        }
        const std::string sl = check();
        if (!sl.empty()) return sl;
    }
    return check();
}

bool IsJunkText(const std::string& text) {
    std::string tok;
    bool any = false, all = true;
    auto flush = [&]() {
        if (tok.empty()) return;
        any = true;
        if (!TokenJunk(tok)) all = false;
        tok.clear();
    };
    for (char c : text) {
        const unsigned char u = (unsigned char)c;
        if ((u < 0x80 && isalnum(u) != 0) || u >= 0x80) {
            tok.push_back(c);
        } else {
            flush();
        }
    }
    flush();
    return any && all;
}

size_t DictSize() {
    Load();
    return g_dict.size();
}
