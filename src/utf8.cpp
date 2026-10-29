#include "utf8.h"

#include <cctype>
#include <cstdio>
#include <cstring>

static size_t Utf8Len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 0;
}

bool LooksLikeUtf8(const std::string& s) {
    size_t i = 0;
    size_t multi = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        size_t n = Utf8Len(c);
        if (n == 0) return false;
        if (n == 1) {
            if (c >= 0x80) return false;
            i += 1;
            continue;
        }
        if (i + n > s.size()) return false;
        for (size_t k = 1; k < n; k++) {
            unsigned char cc = (unsigned char)s[i + k];
            if ((cc & 0xC0) != 0x80) return false;
        }
        multi++;
        i += n;
    }
    return multi > 0;
}

bool IsCyrillicCodepoint(unsigned cp) {
    return (cp >= 0x0400 && cp <= 0x04FF);
}

bool HasCyrillicUtf8(const std::string& utf8) {
    size_t i = 0;
    while (i < utf8.size()) {
        unsigned char c = (unsigned char)utf8[i];
        size_t n = Utf8Len(c);
        if (n == 0) return false;
        if (i + n > utf8.size()) return false;
        for (size_t k = 1; k < n; k++)
            if (((unsigned char)utf8[i + k] & 0xC0) != 0x80) return false;
        unsigned cp = c;
        if (n == 2) cp = ((c & 0x1F) << 6) | ((unsigned char)utf8[i + 1] & 0x3F);
        else if (n == 3) {
            cp = ((c & 0x0F) << 12) | (((unsigned char)utf8[i + 1] & 0x3F) << 6) |
                 ((unsigned char)utf8[i + 2] & 0x3F);
        } else if (n == 4) {
            cp = ((c & 0x07) << 18) | (((unsigned char)utf8[i + 1] & 0x3F) << 12) |
                 (((unsigned char)utf8[i + 2] & 0x3F) << 6) |
                 ((unsigned char)utf8[i + 3] & 0x3F);
        }
        if (IsCyrillicCodepoint(cp)) return true;
        i += n;
    }
    return false;
}

static unsigned Cp1251ToUnicode(unsigned char b) {
    if (b >= 0xC0) return 0x0410 + (b - 0xC0);
    switch (b) {
        case 0xA8: return 0x0401;
        case 0xB8: return 0x0451;
        case 0xA1: return 0x040E;
        case 0xB3: return 0x045E;
        case 0xAA: return 0x0490;
        case 0xBA: return 0x0491;
        case 0xB2: return 0x0406;
        case 0xB4: return 0x0456;
        case 0xAF: return 0x0407;
        case 0xBF: return 0x0457;
        case 0xBE: return 0x0404;
        default: return 0;
    }
}

static void AppendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
        out.push_back((char)cp);
    } else if (cp < 0x800) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

bool HasCyrillicCp1251(const std::string& raw) {
    for (unsigned char b : raw) {
        if (b >= 0xC0 && b <= 0xFF) return true;
        if (b == 0xA8 || b == 0xB8) return true;
    }
    return false;
}

std::string Cp1251ToUtf8(const std::string& raw) {
    std::string out;
    out.reserve(raw.size() * 2);
    for (unsigned char b : raw) {
        unsigned cp = Cp1251ToUnicode(b);
        if (cp) AppendUtf8(out, cp);
        else if (b < 0x80) out.push_back((char)b);
        else out.push_back('?');
    }
    return out;
}

bool NormalizeToUtf8(const std::string& raw, std::string& outUtf8) {
    if (raw.empty()) {
        outUtf8.clear();
        return false;
    }
    if (HasCyrillicUtf8(raw) || LooksLikeUtf8(raw)) {
        outUtf8 = raw;
        return true;
    }
    if (HasCyrillicCp1251(raw)) {
        outUtf8 = Cp1251ToUtf8(raw);
        return true;
    }
    outUtf8 = raw;
    return true;
}

static bool IsWordCodepoint(unsigned cp) {
    if (cp < 0x80) {
        return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
               (cp >= '0' && cp <= '9') || cp == '_';
    }
    if (cp < 0xC0) return false;
    if (cp >= 0x2000 && cp <= 0x2BFF) return false;
    if (cp >= 0x3000 && cp <= 0x303F) return false;
    if (cp >= 0xFE00 && cp <= 0xFE0F) return false;
    if (cp >= 0x1F000) return false;
    return true;
}

static size_t ForEachCodepoint(const std::string& s, size_t i, unsigned& cp) {
    if (i >= s.size()) return 0;
    unsigned char c = (unsigned char)s[i];
    size_t n = Utf8Len(c);
    if (n == 1) {
        cp = c;
        return 1;
    }
    if (n == 0) {
        cp = c;
        return 1;
    }
    if (i + n > s.size()) {
        cp = c;
        return 1;
    }
    for (size_t k = 1; k < n; k++)
        if (((unsigned char)s[i + k] & 0xC0) != 0x80) {
            cp = c;
            return 1;
        }
    if (n == 2) cp = ((c & 0x1F) << 6) | ((unsigned char)s[i + 1] & 0x3F);
    else if (n == 3)
        cp = ((c & 0x0F) << 12) | (((unsigned char)s[i + 1] & 0x3F) << 6) |
             ((unsigned char)s[i + 2] & 0x3F);
    else
        cp = ((c & 0x07) << 18) | (((unsigned char)s[i + 1] & 0x3F) << 12) |
             (((unsigned char)s[i + 2] & 0x3F) << 6) | ((unsigned char)s[i + 3] & 0x3F);
    return n;
}

bool HasLettersUtf8(const std::string& utf8) {
    size_t i = 0;
    while (i < utf8.size()) {
        unsigned cp = 0;
        size_t n = ForEachCodepoint(utf8, i, cp);
        if (n == 0) break;
        if (cp < 0x80) {
            if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')) return true;
        } else if (IsWordCodepoint(cp)) {
            return true;
        }
        i += n;
    }
    return false;
}

int LetterCountUtf8(const std::string& utf8) {
    int count = 0;
    size_t i = 0;
    while (i < utf8.size()) {
        unsigned cp = 0;
        size_t n = ForEachCodepoint(utf8, i, cp);
        if (n == 0) break;
        if (cp < 0x80) {
            if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')) count++;
        } else if (IsWordCodepoint(cp)) {
            count++;
        }
        i += n;
    }
    return count;
}

bool HasTurkishUtf8(const std::string& utf8) {
    size_t i = 0;
    while (i < utf8.size()) {
        unsigned cp = 0;
        size_t n = ForEachCodepoint(utf8, i, cp);
        if (n == 0) break;
        switch (cp) {
            case 0x011F: case 0x0130: case 0x015F:
            case 0x00F6: case 0x00FC:
            case 0x00E7: case 0x00C7:
            case 0x011E: case 0x0131: case 0x015E:
            case 0x00D6: case 0x00DC:
                return true;
        }
        i += n;
    }
    return false;
}

static bool CyrilToLatin(unsigned cp, std::string& out) {
    bool upper = false;

    if (cp >= 0x0410 && cp <= 0x042F) { upper = true; cp += 0x20; }
    else if (cp >= 0x0400 && cp <= 0x040F) { upper = true; cp += 0x50; }

    switch (cp) {
        case 0x04D8: out = "A"; return true;
        case 0x04D9: out = "a"; return true;
        case 0x0492: out = "G"; return true;
        case 0x0493: out = "g"; return true;
        case 0x049A: out = "K"; return true;
        case 0x049B: out = "k"; return true;
        case 0x04A2: out = "N"; return true;
        case 0x04A3: out = "n"; return true;
        case 0x04E8: out = "O"; return true;
        case 0x04E9: out = "o"; return true;
        case 0x04AE: out = "U"; return true;
        case 0x04AF: out = "u"; return true;
        case 0x04B0: out = "U"; return true;
        case 0x04B1: out = "u"; return true;
        case 0x0406: out = "I"; return true;
        case 0x0456: out = "i"; return true;
        case 0x04BA: out = "H"; return true;
        case 0x04BB: out = "h"; return true;
        default: break;
    }

    const char* s = nullptr;
    switch (cp) {
        case 0x0430: s = "a"; break;  case 0x0431: s = "b"; break;
        case 0x0432: s = "v"; break;  case 0x0433: s = "g"; break;
        case 0x0434: s = "d"; break;  case 0x0435: s = "e"; break;
        case 0x0436: s = "zh"; break; case 0x0437: s = "z"; break;
        case 0x0438: s = "i"; break;  case 0x0439: s = "y"; break;
        case 0x043A: s = "k"; break;  case 0x043B: s = "l"; break;
        case 0x043C: s = "m"; break;  case 0x043D: s = "n"; break;
        case 0x043E: s = "o"; break;  case 0x043F: s = "p"; break;
        case 0x0440: s = "r"; break;  case 0x0441: s = "s"; break;
        case 0x0442: s = "t"; break;  case 0x0443: s = "u"; break;
        case 0x0444: s = "f"; break;  case 0x0445: s = "kh"; break;
        case 0x0446: s = "ts"; break; case 0x0447: s = "ch"; break;
        case 0x0448: s = "sh"; break; case 0x0449: s = "sch"; break;
        case 0x044A: s = ""; break;   case 0x044B: s = "y"; break;
        case 0x044C: s = ""; break;   case 0x044D: s = "e"; break;
        case 0x044E: s = "yu"; break; case 0x044F: s = "ya"; break;
        case 0x0451: s = "yo"; break;
        case 0x0491: s = "g"; break;
        default: return false;
    }
    if (!s[0]) return true;
    if (upper) {
        out.push_back((char)(s[0] - 'a' + 'A'));
        out += s + 1;
    } else {
        out += s;
    }
    return true;
}

struct TranslitWord {
    const char* lat;
    unsigned short c[16];
};
static const TranslitWord kTranslitWords[] = {
    {"blyat", {0x0431, 0x043B, 0x044F, 0x0442, 0x044C}},
    {"blyatj", {0x0431, 0x043B, 0x044F, 0x0442, 0x044C}},
    {"bliat", {0x0431, 0x043B, 0x044F, 0x0442, 0x044C}},
    {"blyad", {0x0431, 0x043B, 0x044F, 0x0434, 0x044C}},
    {"bliad", {0x0431, 0x043B, 0x044F, 0x0434, 0x044C}},
    {"suka", {0x0441, 0x0443, 0x043A, 0x0430}},
    {"syka", {0x0441, 0x0443, 0x043A, 0x0430}},
    {"pashol", {0x043F, 0x043E, 0x0448, 0x0435, 0x043B}},
    {"poshol", {0x043F, 0x043E, 0x0448, 0x0435, 0x043B}},
    {"nahui", {0x043D, 0x0430, 0x0445, 0x0443, 0x0439}},
    {"nahuy", {0x043D, 0x0430, 0x0445, 0x0443, 0x0439}},
    {"chmo", {0x0447, 0x043C, 0x043E}},
    {"blin", {0x0431, 0x043B, 0x0438, 0x043D}},
    {"chto", {0x0447, 0x0442, 0x043E}},
    {"shto", {0x0447, 0x0442, 0x043E}},
    {"privet", {0x043F, 0x0440, 0x0438, 0x0432, 0x0435, 0x0442}},
    {"zdarova", {0x0437, 0x0434, 0x0430, 0x0440, 0x043E, 0x0432, 0x0430}},
    {"zdorova", {0x0437, 0x0434, 0x0430, 0x0440, 0x043E, 0x0432, 0x0430}},
    {"davai", {0x0434, 0x0430, 0x0432, 0x0430, 0x0439}},
    {"davay", {0x0434, 0x0430, 0x0432, 0x0430, 0x0439}},
    {"poyehali", {0x043F, 0x043E, 0x0435, 0x0445, 0x0430, 0x043B, 0x0438}},
    {"poehali", {0x043F, 0x043E, 0x0435, 0x0445, 0x0430, 0x043B, 0x0438}},
    {"spasibo", {0x0441, 0x043F, 0x0430, 0x0441, 0x0438, 0x0431, 0x043E}},
    {"poka", {0x043F, 0x043E, 0x043A, 0x0430}},
    {"brat", {0x0431, 0x0440, 0x0430, 0x0442}},
    {"koroche", {0x043A, 0x043E, 0x0440, 0x043E, 0x0447, 0x0435}},
    {"normalno", {0x043D, 0x043E, 0x0440, 0x043C, 0x0430, 0x043B, 0x044C, 0x043D, 0x043E}},
    {"gde", {0x0433, 0x0434, 0x0435}},
    {"kogda", {0x043A, 0x043E, 0x0433, 0x0434, 0x0430}},
    {"pochemu", {0x043F, 0x043E, 0x0447, 0x0435, 0x043C, 0x0443}},
    {"vsyo", {0x0432, 0x0441, 0x0451}},
    {"ty", {0x0442, 0x044B}},
    {"eto", {0x044D, 0x0442, 0x043E}},
    {"cho", {0x0447, 0x0451}},
    {"nado", {0x043D, 0x0430, 0x0434, 0x043E}},
    {"mozhno", {0x043C, 0x043E, 0x0436, 0x043D, 0x043E}},
    {"horosho", {0x0445, 0x043E, 0x0440, 0x043E, 0x0448, 0x043E}},
    {"chuvak", {0x0447, 0x0443, 0x0432, 0x0430, 0x043A}},
    {"chelovek", {0x0447, 0x0435, 0x043B, 0x043E, 0x0432, 0x0435, 0x043A}},
    {"pizdec", {0x043F, 0x0438, 0x0437, 0x0434, 0x0435, 0x0446}},
    {"pizda", {0x043F, 0x0438, 0x0437, 0x0434, 0x0430}},
    {"debil", {0x0434, 0x0435, 0x0431, 0x0438, 0x043B}},
    {"loh", {0x043B, 0x043E, 0x0445}},
    {"huy", {0x0445, 0x0443, 0x0439}},
    {"xuy", {0x0445, 0x0443, 0x0439}},
    {"kak", {0x043A, 0x0430, 0x043A}},
    {"dela", {0x0434, 0x0435, 0x043B, 0x0430}},
    {"delaesh", {0x0434, 0x0435, 0x043B, 0x0430, 0x0435, 0x0448}},
    {"oi", {0x043E, 0x0439}},
    {"oy", {0x043E, 0x0439}},
};

std::string LatinToCyrillic(const std::string& utf8) {
    std::string out;
    out.reserve(utf8.size() * 2);
    size_t i = 0;
    while (i < utf8.size()) {
        unsigned char c = (unsigned char)utf8[i];
        if (c >= 0x80) { out.push_back((char)c); i++; continue; }
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) {
            out.push_back((char)c);
            i++;
            continue;
        }
        {
            size_t j = i;
            while (j < utf8.size() &&
                   (((utf8[j] >= 'A' && utf8[j] <= 'Z')) ||
                    ((utf8[j] >= 'a' && utf8[j] <= 'z'))))
                j++;
            std::string low;
            for (size_t k = i; k < j; k++)
                low.push_back((char)tolower((unsigned char)utf8[k]));
            bool hit = false;
            for (const auto& e : kTranslitWords) {
                if (low == e.lat) {
                    for (const unsigned short* p = e.c; *p; p++) AppendUtf8(out, *p);
                    hit = true;
                    break;
                }
            }
            if (hit) {
                i = j;
                continue;
            }
        }
        auto low = [&](size_t k) -> char {
            return k < utf8.size() ? (char)tolower((unsigned char)utf8[k]) : '\0';
        };
        const char a = (char)tolower(c);
        const char b = low(i + 1), d = low(i + 2), e = low(i + 3);
        unsigned cp = 0;
        size_t adv = 1;
        if (a == 's' && b == 'h' && d == 'c' && e == 'h') { cp = 0x0449; adv = 4; }
        else if (a == 'z' && b == 'h') { cp = 0x0436; adv = 2; }
        else if (a == 'k' && b == 'h') { cp = 0x0445; adv = 2; }
        else if (a == 't' && b == 's') { cp = 0x0446; adv = 2; }
        else if (a == 'c' && b == 'h') { cp = 0x0447; adv = 2; }
        else if (a == 's' && b == 'h') { cp = 0x0448; adv = 2; }
        else if (a == 'y' && b == 'u') { cp = 0x044E; adv = 2; }
        else if (a == 'y' && b == 'a') { cp = 0x044F; adv = 2; }
        else if (a == 'y' && b == 'o') { cp = 0x0451; adv = 2; }
        else if (a == 'y' && b == 'e') { cp = 0x0435; adv = 2; }
        else if (a == 'x') {
            AppendUtf8(out, 0x043A);
            AppendUtf8(out, 0x0441);
            i++;
            continue;
        } else {
            switch (a) {
                case 'a': cp = 0x0430; break;  case 'b': cp = 0x0431; break;
                case 'v': cp = 0x0432; break;  case 'g': cp = 0x0433; break;
                case 'd': cp = 0x0434; break;  case 'e': cp = 0x0435; break;
                case 'z': cp = 0x0437; break;  case 'i': cp = 0x0438; break;
                case 'j': cp = 0x0439; break;  case 'k': cp = 0x043A; break;
                case 'l': cp = 0x043B; break;  case 'm': cp = 0x043C; break;
                case 'n': cp = 0x043D; break;  case 'o': cp = 0x043E; break;
                case 'p': cp = 0x043F; break;  case 'r': cp = 0x0440; break;
                case 's': cp = 0x0441; break;  case 't': cp = 0x0442; break;
                case 'u': cp = 0x0443; break;  case 'f': cp = 0x0444; break;
                case 'y': cp = 0x044B; break;  case 'c': cp = 0x043A; break;
                case 'h': cp = 0x0445; break;  case 'w': cp = 0x0432; break;
                case 'q': cp = 0x043A; break;
                default: cp = c; break;
            }
        }
        AppendUtf8(out, cp);
        i += adv;
    }
    return out;
}

static bool Cp1251Encode(unsigned cp, char& out) {
    if (cp < 0x80) { out = (char)cp; return true; }
    if (cp >= 0x0410 && cp <= 0x044F) { out = (char)(0xC0 + (cp - 0x0410)); return true; }
    switch (cp) {
        case 0x0401: out = (char)0xA8; return true;
        case 0x0451: out = (char)0xB8; return true;
        case 0x040E: out = (char)0xA1; return true;
        case 0x045E: out = (char)0xB3; return true;
        case 0x0490: out = (char)0xAA; return true;
        case 0x0491: out = (char)0xBA; return true;
        case 0x0406: out = (char)0xB2; return true;
        case 0x0456: out = (char)0xB4; return true;
        case 0x0407: out = (char)0xAF; return true;
        case 0x0457: out = (char)0xBF; return true;
        case 0x0404: out = (char)0xBE; return true;
        case 0x0454: out = (char)0xBC; return true;
        case 0x00C7: out = (char)0xC7; return true;
        case 0x00D6: out = (char)0xD6; return true;
        case 0x00DC: out = (char)0xDC; return true;
        case 0x00E7: out = (char)0xE7; return true;
        case 0x00F6: out = (char)0xF6; return true;
        case 0x00FC: out = (char)0xFC; return true;
        case 0x011E: out = 'G'; return true;
        case 0x011F: out = 'g'; return true;
        case 0x015E: out = 'S'; return true;
        case 0x015F: out = 's'; return true;
        case 0x0130: out = 'I'; return true;
        case 0x0131: out = 'i'; return true;
        default: return false;
    }
}

std::string ToConsoleCp1251(const std::string& utf8) {
    std::string out;
    out.reserve(utf8.size());
    for (size_t i = 0; i < utf8.size();) {
        unsigned char c = (unsigned char)utf8[i];
        size_t n = Utf8Len(c);
        if (n == 0 || i + n > utf8.size()) { out.push_back('?'); i++; continue; }
        if (n == 1) { out.push_back((char)c); i++; continue; }

        unsigned cp = c;
        if (n == 2) cp = ((c & 0x1F) << 6) | ((unsigned char)utf8[i + 1] & 0x3F);
        else if (n == 3)
            cp = ((c & 0x0F) << 12) | (((unsigned char)utf8[i + 1] & 0x3F) << 6) |
                 ((unsigned char)utf8[i + 2] & 0x3F);
        else
            cp = ((c & 0x07) << 18) | (((unsigned char)utf8[i + 1] & 0x3F) << 12) |
                 (((unsigned char)utf8[i + 2] & 0x3F) << 6) |
                 ((unsigned char)utf8[i + 3] & 0x3F);
        i += n;

        char b;
        if (Cp1251Encode(cp, b)) {
            out.push_back(b);
        } else if (IsCyrillicCodepoint(cp)) {
            if (!CyrilToLatin(cp, out)) out.push_back('?');
        } else {
            out.push_back('?');
        }
    }
    return out;
}

std::string ToAsciiTurkish(const std::string& utf8) {
    std::string out;
    out.reserve(utf8.size());
    for (size_t i = 0; i < utf8.size();) {
        unsigned char c = (unsigned char)utf8[i];
        size_t n = Utf8Len(c);
        if (n == 0) {
            out.push_back('?');
            i++;
            continue;
        }
        if (n == 1) {
            out.push_back((char)c);
            i++;
            continue;
        }
        unsigned cp = c;
        if (n == 2) cp = ((c & 0x1F) << 6) | ((unsigned char)utf8[i + 1] & 0x3F);
        else if (n == 3)
            cp = ((c & 0x0F) << 12) | (((unsigned char)utf8[i + 1] & 0x3F) << 6) |
                 ((unsigned char)utf8[i + 2] & 0x3F);
        else
            cp = ((c & 0x07) << 18) | (((unsigned char)utf8[i + 1] & 0x3F) << 12) |
                 (((unsigned char)utf8[i + 2] & 0x3F) << 6) |
                 ((unsigned char)utf8[i + 3] & 0x3F);
        i += n;

        switch (cp) {
            case 0x00E7: out.push_back('c'); break;
            case 0x00F6: out.push_back('o'); break;
            case 0x00FC: out.push_back('u'); break;
            case 0x011F: out.push_back('g'); break;
            case 0x0130: out.push_back('I'); break;
            case 0x0131: out.push_back('i'); break;
            case 0x015E: out.push_back('S'); break;
            case 0x015F: out.push_back('s'); break;
            case 0x00C7: out.push_back('C'); break;
            case 0x00D6: out.push_back('O'); break;
            case 0x00DC: out.push_back('U'); break;
            case 0x011E: out.push_back('G'); break;
            default:
                if (IsCyrillicCodepoint(cp)) {
                    if (!CyrilToLatin(cp, out)) AppendUtf8(out, cp);
                } else {
                    AppendUtf8(out, cp);
                }
                break;
        }
    }
    return out;
}

std::string UrlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back((char)c);
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

bool JsonDecodeString(const std::string& json, size_t& pos, std::string& out) {
    out.clear();
    if (pos >= json.size() || json[pos] != '"') return false;
    pos++;
    while (pos < json.size()) {
        char c = json[pos];
        if (c == '"') {
            pos++;
            return true;
        }
        if (c == '\\') {
            pos++;
            if (pos >= json.size()) return false;
            char e = json[pos++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (pos + 4 > json.size()) return false;
                    unsigned cp = 0;
                    for (int k = 0; k < 4; k++) {
                        char h = json[pos + k];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                        else return false;
                    }
                    pos += 4;
                    if (cp >= 0xD800 && cp < 0xDC00 && pos + 6 < json.size() &&
                        json[pos] == '\\' && json[pos + 1] == 'u') {
                        unsigned lo = 0;
                        bool ok = true;
                        for (int k = 0; k < 4; k++) {
                            char h = json[pos + 2 + k];
                            lo <<= 4;
                            if (h >= '0' && h <= '9') lo |= (unsigned)(h - '0');
                            else if (h >= 'a' && h <= 'f') lo |= (unsigned)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') lo |= (unsigned)(h - 'A' + 10);
                            else ok = false;
                        }
                        if (ok && lo >= 0xDC00 && lo < 0xE000) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            pos += 6;
                        }
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default: out.push_back(e); break;
            }
        } else {
            out.push_back(c);
            pos++;
        }
    }
    return false;
}

std::vector<std::string> SplitLines(const std::string& s) {
    std::vector<std::string> v;
    std::string cur;
    for (char c : s) {
        if (c == '\n') {
            v.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur.push_back(c);
        }
    }
    v.push_back(cur);
    return v;
}

std::vector<std::string> ExtractWords(const std::string& utf8, int maxWords) {
    std::vector<std::string> words;
    std::string cur;
    size_t i = 0;
    while (i < utf8.size()) {
        unsigned cp = 0;
        size_t n = ForEachCodepoint(utf8, i, cp);
        if (n == 0) break;

        if (IsWordCodepoint(cp)) {
            if (cp >= 'A' && cp <= 'Z') cp += 0x20;
            else if (cp >= 0x0410 && cp <= 0x042F) cp += 0x20;
            else if (cp >= 0x0400 && cp <= 0x040F) cp += 0x50;
            AppendUtf8(cur, cp);
        } else {
            if (!cur.empty()) {
                bool dup = false;
                for (const auto& w : words)
                    if (w == cur) {
                        dup = true;
                        break;
                    }
                if (!dup && (int)words.size() < maxWords) words.push_back(cur);
                cur.clear();
            }
        }
        i += n;
    }
    if (!cur.empty() && (int)words.size() < maxWords) words.push_back(cur);
    return words;
}
