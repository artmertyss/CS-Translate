#include "common.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <string>
#include <unordered_map>

#include "chat.h"
#include "config.h"
#include "dict.h"
#include "log.h"
#include "translate.h"
#include "utf8.h"

std::atomic<bool> g_shutdown{false};

static std::mutex g_jobMtx;
static std::deque<ChatItem> g_jobs;
static std::condition_variable g_jobCv;

static std::mutex g_outMtx;
static std::deque<OutLine> g_out;

void JobPush(const ChatItem& item) {
    if (!g_cfg.enabled) return;
    {
        std::lock_guard<std::mutex> lk(g_jobMtx);
        if (g_jobs.size() >= 64) return;
        g_jobs.push_back(item);
    }
    g_jobCv.notify_one();
}

bool JobPop(ChatItem& out) {
    std::unique_lock<std::mutex> lk(g_jobMtx);
    if (g_jobs.empty()) {
        g_jobCv.wait_for(lk, std::chrono::milliseconds(250));
        if (g_jobs.empty()) return false;
    }
    out = g_jobs.front();
    g_jobs.pop_front();
    return true;
}

void OutPush(const std::string& text, bool center, unsigned char r, unsigned char g,
             unsigned char b, bool chat, int block) {
    std::lock_guard<std::mutex> lk(g_outMtx);
    if (g_out.size() >= 64) return;
    OutLine l;
    l.text = text;
    l.center = center;
    l.r = r;
    l.g = g;
    l.b = b;
    l.toConsole = true;
    l.chat = chat;
    l.block = block;
    g_out.push_back(l);
}

void OverlayPush(const std::string& text, unsigned char r, unsigned char g, unsigned char b,
                 int block) {
    std::lock_guard<std::mutex> lk(g_outMtx);
    if (g_out.size() >= 64) return;
    OutLine l;
    l.text = text;
    l.center = false;
    l.r = r;
    l.g = g;
    l.b = b;
    l.toConsole = false;
    l.chat = true;
    l.block = block;
    g_out.push_back(l);
}

int NewOverlayBlockId() {
    static std::atomic<int> s_block{1};
    return s_block.fetch_add(1);
}

void OutDrain(std::vector<OutLine>& out) {
    out.clear();
    std::lock_guard<std::mutex> lk(g_outMtx);
    for (auto& l : g_out) out.push_back(l);
    g_out.clear();
}

static std::mutex g_recentMtx;
static std::deque<std::pair<std::string, DWORD>> g_recent;

static const DWORD kRecentMs = 15000;

static bool SeenRecently(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_recentMtx);
    const DWORD now = GetTickCount();
    for (auto it = g_recent.begin(); it != g_recent.end();) {
        if ((long)(now - it->second) >= (long)kRecentMs)
            it = g_recent.erase(it);
        else
            ++it;
    }
    for (const auto& r : g_recent)
        if (r.first == s) return true;
    g_recent.push_back({s, now});
    while (g_recent.size() > 8) g_recent.pop_front();
    return false;
}

static std::string FitLine(const std::string& s, size_t maxLen) {
    if (s.size() <= maxLen) return s;
    size_t cut = maxLen - 3;
    while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) cut--;
    return s.substr(0, cut) + "...";
}

static std::string ForConsole(const std::string& utf8) {
    if (g_cfg.charset == "utf8") return utf8;
    if (g_cfg.charset == "ascii") return ToAsciiTurkish(utf8);
    return ToConsoleCp1251(utf8);
}

static std::string NormLang(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '_') o.push_back('-');
        else o.push_back((char)tolower((unsigned char)c));
    }
    size_t b = o.find_first_not_of(" \t");
    size_t e = o.find_last_not_of(" \t\r");
    if (b == std::string::npos) return "";
    return o.substr(b, e - b + 1);
}

static bool SameLang(const std::string& a, const std::string& b) {
    std::string x = NormLang(a), y = NormLang(b);
    if (x.empty() || y.empty()) return false;
    if (x == y) return true;
    size_t nx = x.find('-'), ny = y.find('-');
    std::string bx = (nx == std::string::npos) ? x : x.substr(0, nx);
    std::string by = (ny == std::string::npos) ? y : y.substr(0, ny);
    return bx == by;
}

static std::vector<std::string> SplitLangs(const std::string& csv) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : csv) {
        if (c == ',' || c == ';' || c == ' ' || c == '\t') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static bool InLangList(const std::vector<std::string>& langs, const std::string& lang) {
    for (const auto& l : langs)
        if (SameLang(l, lang)) return true;
    return false;
}

static int CountWords(const std::string& text) {
    int n = 0;
    std::string cur;
    for (char c : text) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!cur.empty() && HasLettersUtf8(cur)) n++;
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty() && HasLettersUtf8(cur)) n++;
    return n;
}

static size_t NextCodepoint(const std::string& s, size_t i, unsigned& cp) {
    if (i >= s.size()) return 0;
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) {
        cp = c;
        return 1;
    }
    size_t n = 0;
    if ((c & 0xE0) == 0xC0)
        n = 2;
    else if ((c & 0xF0) == 0xE0)
        n = 3;
    else if ((c & 0xF8) == 0xF0)
        n = 4;
    else
        return 1;
    if (i + n > s.size()) return 1;
    cp = (n == 2) ? (c & 0x1F) : (n == 3) ? (c & 0x0F) : (c & 0x07);
    for (size_t k = 1; k < n; k++) {
        unsigned char b = (unsigned char)s[i + k];
        if ((b & 0xC0) != 0x80) return 1;
        cp = (cp << 6) | (b & 0x3F);
    }
    return n;
}

enum {
    kScriptArabic = 1,
    kScriptCyrillic = 2,
    kScriptGreek = 4,
    kScriptHebrew = 8,
    kScriptCJK = 16,
    kScriptThai = 32,
    kScriptDevanagari = 64,
};

static unsigned ScriptMaskOf(const std::string& text) {
    unsigned mask = 0;
    size_t i = 0;
    while (i < text.size()) {
        unsigned cp = 0;
        size_t n = NextCodepoint(text, i, cp);
        if (n == 0) break;
        if ((cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0x0750 && cp <= 0x077F) ||
            (cp >= 0xFB50 && cp <= 0xFDFF) || (cp >= 0xFE70 && cp <= 0xFEFF))
            mask |= kScriptArabic;
        else if (cp >= 0x0400 && cp <= 0x04FF)
            mask |= kScriptCyrillic;
        else if (cp >= 0x0370 && cp <= 0x03FF)
            mask |= kScriptGreek;
        else if (cp >= 0x0590 && cp <= 0x05FF)
            mask |= kScriptHebrew;
        else if ((cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
                 (cp >= 0xAC00 && cp <= 0xD7AF))
            mask |= kScriptCJK;
        else if (cp >= 0x0E00 && cp <= 0x0E7F)
            mask |= kScriptThai;
        else if (cp >= 0x0900 && cp <= 0x097F)
            mask |= kScriptDevanagari;
        i += n;
    }
    return mask;
}

static const struct {
    const char* lang;
    unsigned script;
} kLangScript[] = {
    {"ar", kScriptArabic},   {"fa", kScriptArabic},  {"ur", kScriptArabic},
    {"ps", kScriptArabic},   {"ru", kScriptCyrillic},{"uk", kScriptCyrillic},
    {"bg", kScriptCyrillic}, {"sr", kScriptCyrillic},{"mk", kScriptCyrillic},
    {"be", kScriptCyrillic}, {"kk", kScriptCyrillic},{"ky", kScriptCyrillic},
    {"tg", kScriptCyrillic}, {"el", kScriptGreek},   {"he", kScriptHebrew},
    {"yi", kScriptHebrew},   {"zh", kScriptCJK},     {"ja", kScriptCJK},
    {"ko", kScriptCJK},      {"th", kScriptThai},    {"hi", kScriptDevanagari},
    {"ne", kScriptDevanagari},{"mr", kScriptDevanagari},
};

static bool ScriptMismatch(const std::string& detected, const std::string& text) {
    const std::string d = NormLang(detected);
    if (d.empty()) return false;
    const unsigned have = ScriptMaskOf(text);
    for (const auto& e : kLangScript) {
        if (NormLang(e.lang) == d) return (have & e.script) == 0;
    }
    return false;
}

static bool SameTextIgnoringCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

static unsigned FoldLatin(unsigned cp) {
    switch (cp) {
        case 0x00E0: case 0x00E1: case 0x00E2: case 0x00E3: case 0x00C0:
        case 0x00C1: case 0x00C2: case 0x00C3: case 0x0101: case 0x0103:
        case 0x01CE:
            return 'a';
        case 0x00E8: case 0x00E9: case 0x00EA: case 0x00EB: case 0x00C8:
        case 0x00C9: case 0x00CA: case 0x00CB: case 0x0113: case 0x0115:
            return 'e';
        case 0x00EC: case 0x00ED: case 0x00EE: case 0x00EF: case 0x00CD:
        case 0x00CE: case 0x00CF: case 0x012B: case 0x012D: case 0x0131:
            return 'i';
        case 0x00F2: case 0x00F3: case 0x00F4: case 0x00F5: case 0x00D2:
        case 0x00D3: case 0x00D4: case 0x00D5: case 0x014D: case 0x014F:
            return 'o';
        case 0x00F9: case 0x00FA: case 0x00FB: case 0x00FC: case 0x00D9:
        case 0x00DA: case 0x00DB: case 0x00DC: case 0x016B: case 0x016D:
            return 'u';
        case 0x00F1: case 0x00D1: case 0x0144:
            return 'n';
        case 0x00E7: case 0x00C7: case 0x0107: case 0x010D:
            return 'c';
        case 0x015F: case 0x015E: case 0x0219: case 0x0218: case 0x0161:
            return 's';
        case 0x0163: case 0x0162: case 0x021B: case 0x021A: case 0x0165:
            return 't';
        case 0x011F: case 0x011E: case 0x01E7:
            return 'g';
        case 0x017E: case 0x017D:
            return 'z';
        case 0x00FD: case 0x00FF: case 0x00DD:
            return 'y';
        default:
            return 0;
    }
}

static std::string NormalizeLoose(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        unsigned cp = 0;
        size_t n = NextCodepoint(s, i, cp);
        if (n == 0) break;
        if (cp < 0x80) {
            if (isalnum((unsigned char)cp)) o.push_back((char)tolower((unsigned char)cp));
        } else {
            unsigned f = FoldLatin(cp);
            if (f)
                o.push_back((char)f);
            else
                o.append(s, i, n);
        }
        i += n;
    }
    return o;
}

static bool IsWordByte(char c) {
    const unsigned char u = (unsigned char)c;
    return u >= 0x80 || isalnum(u) != 0;
}

static bool HasWord(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return false;
    size_t pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) {
        const bool left = pos > 0 && IsWordByte(hay[pos - 1]);
        const size_t end = pos + needle.size();
        const bool right = end < hay.size() && IsWordByte(hay[end]);
        if (!left && !right) return true;
        pos++;
    }
    return false;
}

static bool RoundTripMatch(const std::string& aRaw, const std::string& bRaw,
                           const std::vector<std::string>& words, bool translitWords) {
    const std::string a = NormalizeLoose(aRaw), b = NormalizeLoose(bRaw);
    if (a.empty() || b.empty()) return false;
    if (a == b) return true;
    if (a.size() >= 3 && HasWord(b, a)) return true;
    if (b.size() >= 3 && HasWord(a, b)) return true;
    int total = 0, hit = 0;
    for (const auto& w : words) {
        const std::string fw = NormalizeLoose(translitWords ? LatinToCyrillic(w) : w);
        if (fw.size() < 3) continue;
        total++;
        if (HasWord(b, fw)) hit++;
    }
    return total > 0 && hit * 2 >= total + 1;
}

struct SlangHit {
    std::string ph;
    std::string word;
    std::string meaning;
};

static bool IsSlangChar(unsigned cp) {
    if (cp >= 'a' && cp <= 'z') return true;
    if (cp >= 'A' && cp <= 'Z') return true;
    if (cp >= '0' && cp <= '9') return true;
    return cp >= 0x80;
}

static void AppendCp(std::string& s, unsigned cp) {
    if (cp < 0x80) {
        s.push_back((char)cp);
    } else if (cp < 0x800) {
        s.push_back((char)(0xC0 | (cp >> 6)));
        s.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        s.push_back((char)(0xE0 | (cp >> 12)));
        s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

static std::string ProtectSlang(const std::string& text, std::vector<SlangHit>& hits) {
    hits.clear();
    if (g_cfg.slang.empty() && g_cfg.keep.empty()) return text;
    std::string out, low;
    size_t wordStart = 0, i = 0;
    auto flush = [&](size_t end) {
        if (low.empty()) return;
        const DictEntry* de = DictFind(low);
        if (de && (de->keep || !de->tr.empty())) {
            char ph[24];
            snprintf(ph, sizeof(ph), "zqslangqz%u", (unsigned)hits.size());
            hits.push_back({ph, text.substr(wordStart, end - wordStart), std::string()});
            out += ph;
            low.clear();
            return;
        }
        for (const auto& kv : g_cfg.slang) {
            if (low == kv.first) {
                char ph[24];
                snprintf(ph, sizeof(ph), "zqslangqz%u", (unsigned)hits.size());
                hits.push_back({ph, text.substr(wordStart, end - wordStart), kv.second});
                out += ph;
                low.clear();
                return;
            }
        }
        for (const auto& kw : g_cfg.keep) {
            if (low == kw) {
                char ph[24];
                snprintf(ph, sizeof(ph), "zqslangqz%u", (unsigned)hits.size());
                hits.push_back({ph, text.substr(wordStart, end - wordStart), std::string()});
                out += ph;
                low.clear();
                return;
            }
        }
        out.append(text, wordStart, end - wordStart);
        low.clear();
    };
    while (i < text.size()) {
        unsigned cp = 0;
        size_t n = NextCodepoint(text, i, cp);
        if (n == 0) break;
        if (IsSlangChar(cp)) {
            if (low.empty()) wordStart = i;
            if (cp >= 'A' && cp <= 'Z') cp += 0x20;
            AppendCp(low, cp);
        } else {
            flush(i);
            out.append(text, i, n);
        }
        i += n;
    }
    flush(text.size());
    return out;
}

static std::string RestoreSlang(const std::string& text, const std::vector<SlangHit>& hits,
                                bool annotate, const std::string& dstLang) {
    std::string out = text;
    for (size_t h = 0; h < hits.size(); h++) {
        bool first = true;
        for (size_t j = 0; j < h; j++)
            if (SameTextIgnoringCase(hits[j].word, hits[h].word)) {
                first = false;
                break;
            }
        std::string rep = hits[h].word;
        bool dictHit = false;
        {
            std::string key = hits[h].word;
            for (char& c : key)
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            const DictEntry* de = DictFind(key);
            if (de) {
                dictHit = true;
                const auto it = de->tr.find(NormLang(dstLang));
                if (it != de->tr.end()) rep = it->second;
            }
        }
        if (!dictHit && annotate && first && g_cfg.slangNotes &&
            !hits[h].meaning.empty() && SameLang(dstLang, "tr"))
            rep += " (" + hits[h].meaning + ")";
        size_t pos = 0;
        while ((pos = out.find(hits[h].ph, pos)) != std::string::npos) {
            out.replace(pos, hits[h].ph.size(), rep);
            pos += rep.size();
        }
    }
    return out;
}

static bool IsRuLike(const std::string& lang) {
    const std::string d = NormLang(lang);
    if (d.empty()) return false;
    return d == "ru" || d == "uk" || d == "bg" || d == "sr" || d == "mk" || d == "be";
}

static std::vector<std::string> CandidateOrder(const std::vector<std::string>& prio,
                                               const std::string& detected,
                                               const std::string& dst,
                                               const std::string& text) {
    const std::string det = NormLang(detected), d = NormLang(dst);
    std::vector<std::string> out;
    for (const auto& raw : prio) {
        const std::string c = NormLang(raw);
        if (c.empty() || c == "auto" || c == det || c == d) continue;
        bool dup = false;
        for (const auto& e : out)
            if (e == c) { dup = true; break; }
        if (!dup) out.push_back(c);
    }
    const unsigned have = ScriptMaskOf(text);
    bool hasEn = false;
    for (const auto& c : out)
        if (c == "en") hasEn = true;
    if (have == 0 && !hasEn && d != "en") out.push_back("en");
    auto score = [&](const std::string& c) {
        for (const auto& e : kLangScript)
            if (NormLang(e.lang) == c) return (have & e.script) ? 0 : 4;
        if (have == 0) return (c == "en") ? 0 : (c == "tr") ? 1 : 2;
        return 3;
    };
    std::stable_sort(out.begin(), out.end(),
                     [&](const std::string& a, const std::string& b) {
                         return score(a) < score(b);
                     });
    return out;
}

static bool TranslitRetry(const std::string& text, const std::string& dstLang,
                          TranslationResult& r) {
    if (!r.ok) return false;
    if (!SameTextIgnoringCase(r.sentence, text)) return false;
    if (HasCyrillicUtf8(text)) return false;
    if (!IsRuLike(r.detectedSrc)) return false;
    const std::string conv = LatinToCyrillic(text);
    if (SameTextIgnoringCase(conv, text)) return false;

    std::vector<std::string> tries;
    tries.push_back(NormLang(r.detectedSrc));
    if (tries[0] != "ru") tries.push_back("ru");
    for (const auto& sl : tries) {
        if (sl.empty()) continue;
        TranslationResult r2;
        if (Translate(sl, dstLang, conv, {}, r2) && r2.ok &&
            !SameTextIgnoringCase(r2.sentence, text)) {
            xlog::Write("latin->cyrillic retry (sl=%s): '%.70s' -> '%.70s'", sl.c_str(),
                        text.c_str(), r2.sentence.c_str());
            r = std::move(r2);
            return true;
        }
    }
    return false;
}

static std::mutex g_langMtx;
static std::unordered_map<std::string, std::pair<std::string, std::chrono::steady_clock::time_point>>
    g_langMem;

static std::string MemKey(const ChatItem& item) {
    if (!item.name.empty()) return item.name;
    return "#" + std::to_string(item.clientIndex);
}

static std::string RecallLang(const ChatItem& item) {
    std::lock_guard<std::mutex> lk(g_langMtx);
    const auto it = g_langMem.find(MemKey(item));
    if (it == g_langMem.end()) return std::string();
    if (std::chrono::steady_clock::now() - it->second.second >
        std::chrono::minutes(10)) {
        g_langMem.erase(it);
        return std::string();
    }
    return it->second.first;
}

static void RememberLang(const ChatItem& item, const std::string& lang) {
    if (lang.empty()) return;
    std::lock_guard<std::mutex> lk(g_langMtx);
    if (g_langMem.size() > 64) g_langMem.clear();
    g_langMem[MemKey(item)] = {lang, std::chrono::steady_clock::now()};
}

static void ProcessJob(const ChatItem& item) {
    ResetHttpCalls();
    if (SeenRecently(item.text)) {
        xlog::Write("skipped (duplicate): %.80s", item.text.c_str());
        return;
    }

    if (LetterCountUtf8(item.text) < g_cfg.minLetters) {
        xlog::Write("skipped (too short, min_letters=%d): %.80s", g_cfg.minLetters,
                    item.text.c_str());
        return;
    }

    if (IsJunkText(item.text)) {
        xlog::Write("skipped (random text): %.80s", item.text.c_str());
        return;
    }

    const std::string dstLang = GetDstLang();
    const std::string srcLang = GetSrcLang();

    std::vector<SlangHit> slangHits;
    const std::string text = ProtectSlang(item.text, slangHits);
    if (!slangHits.empty())
        xlog::Write("slang protected: %u hit(s): %.60s", (unsigned)slangHits.size(),
                    item.text.c_str());

    std::vector<std::string> words = ExtractWords(text, g_cfg.maxWords);
    if (!g_cfg.keep.empty() && !words.empty()) {
        std::vector<std::string> filtered;
        filtered.reserve(words.size());
        for (const auto& w : words) {
            bool jargon = false;
            for (const auto& k : g_cfg.keep)
                if (SameLang(w, k)) { jargon = true; break; }
            if (!jargon) filtered.push_back(w);
        }
        words = std::move(filtered);
    }

    TranslationResult r;
    if (!Translate("auto", dstLang, text, words, r) || !r.ok) {
        xlog::Write("translation error: %s", r.error.c_str());
        return;
    }

    const std::string forcedSl = DictForceSl(item.text);
    if (!forcedSl.empty() && !SameLang(r.detectedSrc, forcedSl)) {
        TranslationResult r2;
        if (Translate(forcedSl, dstLang, text, words, r2) && r2.ok &&
            !SameTextIgnoringCase(r2.sentence, text)) {
            xlog::Write("dict sl force %s -> %s ('%.70s' -> '%.70s')",
                        r.detectedSrc.empty() ? "?" : r.detectedSrc.c_str(),
                        forcedSl.c_str(), item.text.c_str(), r2.sentence.c_str());
            r = std::move(r2);
        }
    } else if (forcedSl.empty() && slangHits.empty()) {
        const std::string mem = RecallLang(item);
        const std::vector<std::string> prio = SplitLangs(GetPriorityLangs());
        const bool oddLang = !r.detectedSrc.empty() && !prio.empty() &&
                             !InLangList(prio, r.detectedSrc);
        const bool shortMsg = CountWords(item.text) <= 3 || LetterCountUtf8(item.text) <= 12;
        if (!mem.empty() && shortMsg && (oddLang || r.conf < 0.9) &&
            !SameLang(mem, dstLang) && !SameLang(mem, r.detectedSrc) &&
            HttpCalls() < 12) {
            TranslationResult r2;
            if (Translate(mem, dstLang, text, words, r2) && r2.ok &&
                !SameTextIgnoringCase(r2.sentence, text)) {
                xlog::Write("player lang memory %s (was %s): '%.60s' -> '%.60s'",
                            mem.c_str(),
                            r.detectedSrc.empty() ? "?" : r.detectedSrc.c_str(),
                            item.text.c_str(), r2.sentence.c_str());
                r = std::move(r2);
            }
        }
    }

    if (!r.detectedSrc.empty() && r.conf >= 0.9 && !SameLang(r.detectedSrc, dstLang))
        RememberLang(item, r.detectedSrc);

    bool hasRealSlang = false;
    for (const auto& h : slangHits)
        if (!h.meaning.empty()) { hasRealSlang = true; break; }
    const bool slangOnly =
        hasRealSlang && SameTextIgnoringCase(r.sentence, text);
    if (slangOnly) {
        r.sentence = RestoreSlang(r.sentence, slangHits, true, dstLang);
        for (auto& w : r.words) w = RestoreSlang(w, slangHits, false, dstLang);
        const std::vector<std::string> prio = SplitLangs(GetPriorityLangs());
        if (!r.detectedSrc.empty() && !prio.empty() && !InLangList(prio, r.detectedSrc))
            r.detectedSrc = "tr";
        xlog::Write("slang only, nothing to translate: '%.70s' -> '%.70s'",
                    item.text.c_str(), r.sentence.c_str());
    }

    const bool baseChanged = !SameTextIgnoringCase(r.sentence, text);

    {
        const std::vector<std::string> prio = SplitLangs(GetPriorityLangs());
        const bool detKnown = !r.detectedSrc.empty();
        if (!prio.empty() && (detKnown || !baseChanged)) {
            const bool badScript = detKnown && ScriptMismatch(r.detectedSrc, text);
            const bool coreCase =
                !slangOnly && (badScript || !baseChanged) && slangHits.empty();
            if (coreCase) {
                bool fixed = false;
                const int kMaxCalls = 12;

                const bool needTranslit =
                    slangHits.empty() && ScriptMaskOf(text) == 0 && detKnown &&
                    !SameLang(r.detectedSrc, dstLang) &&
                    LetterCountUtf8(text) >= 4;

                if (!fixed && needTranslit && HttpCalls() < kMaxCalls) {
                    const std::string conv = LatinToCyrillic(text);
                    if (!SameTextIgnoringCase(conv, text)) {
                        TranslationResult r2;
                        std::string slUsed = "auto";
                        bool accept = false;
                        if (Translate("auto", dstLang, conv, {}, r2) && r2.ok &&
                            IsRuLike(r2.detectedSrc) && r2.conf >= 0.6 &&
                            !SameTextIgnoringCase(r2.sentence, conv) &&
                            NormalizeLoose(r2.sentence) != NormalizeLoose(text)) {
                            accept = true;
                        } else if (r2.ok && IsRuLike(r2.detectedSrc)) {
                            TranslationResult r3;
                            if (Translate("ru", dstLang, conv, {}, r3) && r3.ok) {
                                if (r3.detectedSrc.empty()) r3.detectedSrc = "ru";
                                if (IsRuLike(r3.detectedSrc) &&
                                    !SameTextIgnoringCase(r3.sentence, conv) &&
                                    NormalizeLoose(r3.sentence) !=
                                        NormalizeLoose(text)) {
                                    r2 = std::move(r3);
                                    slUsed = "ru";
                                    accept = true;
                                }
                            }
                        }
                        if (accept) {
                            if (!InLangList(prio, r2.detectedSrc)) {
                                xlog::Write(
                                    "latin->cyrillic retry relabel %s -> ru (conf %.2f)",
                                    r2.detectedSrc.c_str(), r2.conf);
                                r2.detectedSrc = "ru";
                            }
                            xlog::Write(
                                "latin->cyrillic retry (sl=%s conf %.2f -> %s): "
                                "'%.70s' -> '%.70s'",
                                slUsed.c_str(), r2.conf, r2.detectedSrc.c_str(),
                                item.text.c_str(), r2.sentence.c_str());
                            r = std::move(r2);
                            fixed = true;
                        }
                    }
                }

                if (!fixed && baseChanged && HttpCalls() < kMaxCalls) {
                    const std::string probe = (dstLang == "en") ? "ru" : "en";
                    TranslationResult fwd, back;
                    if (Translate("auto", probe, text, {}, fwd) && fwd.ok &&
                        !SameTextIgnoringCase(fwd.sentence, text) &&
                        Translate("auto", dstLang, fwd.sentence, {}, back) &&
                        back.ok &&
                        RoundTripMatch(text, back.sentence, words, false)) {
                        xlog::Write(
                            "skipped (already %s, round-trip ok): %.80s -> "
                            "'%.60s'",
                            dstLang.c_str(), item.text.c_str(),
                            fwd.sentence.c_str());
                        return;
                    }
                }

                int tried = 0;
                for (const auto& cand :
                     CandidateOrder(prio, r.detectedSrc, dstLang, text)) {
                    if (tried >= 3 || HttpCalls() >= kMaxCalls) break;
                    if (ScriptMismatch(cand, text)) continue;
                    tried++;
                    const std::string probe = SameLang(cand, "en") ? dstLang : "en";
                    TranslationResult r2;
                    if (!Translate(cand, probe, text, {}, r2) || !r2.ok) continue;
                    if (SameTextIgnoringCase(r2.sentence, text))
                        continue;
                    if (NormalizeLoose(r2.sentence) == NormalizeLoose(text))
                        continue;
                    TranslationResult back;
                    if (!(Translate("auto", cand, r2.sentence, {}, back) && back.ok &&
                          RoundTripMatch(text, back.sentence, words, false)))
                        continue;
                    TranslationResult fin;
                    if (SameLang(probe, dstLang)) {
                        fin = std::move(r2);
                    } else if (!Translate(cand, dstLang, text, words, fin) || !fin.ok) {
                        continue;
                    }
                    xlog::Write(
                        "priority retry: detected %s (conf %.2f) -> sl=%s ('%.70s' -> '%.70s')",
                        detKnown ? r.detectedSrc.c_str() : "?", r.conf, cand.c_str(),
                        item.text.c_str(), fin.sentence.c_str());
                    r = std::move(fin);
                    r.detectedSrc = cand;
                    r.conf = 1.0;
                    fixed = true;
                    break;
                }
            }
        }
    }

    if (slangHits.empty()) {
        const std::vector<std::string> prio = SplitLangs(GetPriorityLangs());
        if (!r.detectedSrc.empty() && !prio.empty() &&
            !InLangList(prio, r.detectedSrc) && r.conf > 0.0 && r.conf < 1.0 &&
            CountWords(item.text) == 1 && LetterCountUtf8(item.text) <= 5) {
            xlog::Write("skipped (short word, odd detection %s): %.60s",
                        r.detectedSrc.c_str(), item.text.c_str());
            return;
        }
    }

    if (SameLang(r.detectedSrc, dstLang) && slangHits.empty()) {
        xlog::Write("skipped (already %s): %.80s", r.detectedSrc.c_str(), item.text.c_str());
        return;
    }
    if (slangHits.empty() && !srcLang.empty() && NormLang(srcLang) != "auto" &&
        !r.detectedSrc.empty() && !SameLang(r.detectedSrc, srcLang)) {
        xlog::Write("skipped (source %s, expected %s): %.80s", r.detectedSrc.c_str(),
                    srcLang.c_str(), item.text.c_str());
        return;
    }
    if (!slangHits.empty()) {
        r.sentence = RestoreSlang(r.sentence, slangHits, true, dstLang);
        for (auto& w : r.words) w = RestoreSlang(w, slangHits, false, dstLang);
    }

    if (SameTextIgnoringCase(r.sentence, item.text)) {
        xlog::Write("skipped (translation identical): %.80s", item.text.c_str());
        return;
    }

    xlog::Write("translate [%s] %s%s -> '%.100s' -> '%.100s'", item.msgName.c_str(),
                r.detectedSrc.empty() ? "?" : r.detectedSrc.c_str(),
                (r.conf > 0.0 && r.conf < 1.0) ? " (low conf)" : "", item.text.c_str(),
                r.sentence.c_str());

    std::string header = g_cfg.prefix;
    if (!item.name.empty()) header += " " + item.name;
    if (!r.detectedSrc.empty()) {
        header += " (" + LangFullName(r.detectedSrc);
        const std::string to = LangFullName(dstLang);
        if (!to.empty() && !SameLang(r.detectedSrc, dstLang)) header += " -> " + to;
        header += ")";
    }
    header += ": ";

    int block = NewOverlayBlockId();

    if (g_cfg.showSentence) {
        std::string line = header + r.sentence;
        OutPush(FitLine(ForConsole(line), 150), g_cfg.showCenter, 255, 205, 60,
                item.playerChat, block);
    }

    if (g_cfg.showWords && !r.words.empty() && !words.empty()) {
        std::string gloss;
        size_t n = words.size() < r.words.size() ? words.size() : r.words.size();
        for (size_t i = 0; i < n; i++) {
            const std::string& dst = r.words[i];
            const std::string src = RestoreSlang(words[i], slangHits, false, dstLang);
            if (src.empty() || dst.empty()) continue;
            if (NormLang(src) == NormLang(dst)) continue;
            if (!gloss.empty()) gloss += ", ";
            gloss += src + " -> " + dst;
        }
        if (!gloss.empty()) {
            std::string line = g_cfg.prefix + "   " + gloss;
            OutPush(FitLine(ForConsole(line), 150), false, 150, 220, 255, item.playerChat,
                    block);
        }
    }
}

struct SayJob {
    std::string text;
    std::string lang;
    bool team = false;
};
static std::mutex g_sayMtx;
static std::deque<SayJob> g_sayJobs;
static std::deque<SayDone> g_sayDone;

void SayPush(const std::string& textUtf8, const std::string& lang, bool team) {
    if (textUtf8.empty()) return;
    {
        std::lock_guard<std::mutex> lk(g_sayMtx);
        if (g_sayJobs.size() >= 8) return;
        g_sayJobs.push_back(SayJob{textUtf8, lang, team});
    }
}

void SayOutDrain(std::vector<SayDone>& out) {
    out.clear();
    std::lock_guard<std::mutex> lk(g_sayMtx);
    for (auto& s : g_sayDone) out.push_back(s);
    g_sayDone.clear();
}

static void ProcessSayJob(const SayJob& j) {
    ResetHttpCalls();
    std::vector<SlangHit> hits;
    const std::string text = ProtectSlang(j.text, hits);
    if (!hits.empty())
        xlog::Write("say jargon protected: %u hit(s): %.60s", (unsigned)hits.size(),
                    j.text.c_str());
    TranslationResult r;
    if (!Translate("auto", j.lang, text, {}, r) || !r.ok) {
        std::string msg = g_cfg.prefix + " translate_say failed: " + r.error;
        OutPush(EncodeForOutput(msg), false, 255, 90, 90, false, NewOverlayBlockId());
        xlog::Write("translate_say error (lang=%s): %s", j.lang.c_str(), r.error.c_str());
        return;
    }

    const std::string forcedSl = DictForceSl(j.text);
    if (!forcedSl.empty() && !SameLang(r.detectedSrc, forcedSl)) {
        TranslationResult r2;
        if (Translate(forcedSl, j.lang, text, {}, r2) && r2.ok &&
            !SameTextIgnoringCase(r2.sentence, text)) {
            xlog::Write("say dict sl force %s -> %s ('%.70s' -> '%.70s')",
                        r.detectedSrc.empty() ? "?" : r.detectedSrc.c_str(),
                        forcedSl.c_str(), j.text.c_str(), r2.sentence.c_str());
            r = std::move(r2);
        }
    }

    TranslitRetry(text, j.lang, r);
    r.sentence = RestoreSlang(r.sentence, hits, false, j.lang);

    std::string clean;
    for (char c : r.sentence) {
        if (c == '"' || c == '\n' || c == '\r' || c == ';') continue;
        clean.push_back(c);
    }
    while (!clean.empty() && clean.front() == ' ') clean.erase(clean.begin());
    while (!clean.empty() && clean.back() == ' ') clean.pop_back();
    if (clean.empty()) return;

    xlog::Write("translate_%s [%s] '%.80s' -> '%.80s'", j.team ? "teamsay" : "say",
                j.lang.c_str(), j.text.c_str(), clean.c_str());

    std::lock_guard<std::mutex> lk(g_sayMtx);
    if (g_sayDone.size() >= 8) return;
    g_sayDone.push_back(SayDone{clean, j.lang, j.text, j.team});
}

static void ProcessSayJobs() {
    for (;;) {
        SayJob j;
        {
            std::lock_guard<std::mutex> lk(g_sayMtx);
            if (g_sayJobs.empty()) return;
            j = g_sayJobs.front();
            g_sayJobs.pop_front();
        }
        try {
            ProcessSayJob(j);
        } catch (...) {
        }
    }
}

std::string EncodeForOutput(const std::string& utf8) { return ForConsole(utf8); }

static void WorkerProc() {
    xlog::Write("worker thread started");
    ChatItem item;
    while (!g_shutdown.load()) {
        if (!JobPop(item)) {
            if (g_shutdown.load()) break;
            ProcessSayJobs();
            continue;
        }
        try {
            ProcessJob(item);
        } catch (const std::exception& e) {
            xlog::Write("worker exception: %s", e.what());
        } catch (...) {
            xlog::Write("worker unknown exception");
        }
        ProcessSayJobs();
    }
    xlog::Write("worker thread stopped");
}

void StartWorker() {
    static std::thread t;
    if (t.joinable()) return;
    t = std::thread(WorkerProc);
    t.detach();
}
