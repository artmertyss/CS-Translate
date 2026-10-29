#include "chat.h"

#include "config.h"
#include "log.h"
#include "utf8.h"

#include <cstring>
#include <vector>

static const unsigned char kChatMarker = 2;
static const unsigned char kServerMarker = 4;

static size_t FindNoCase(const std::string& hay, const std::string& needle) {
    if (needle.empty() || needle.size() > hay.size()) return std::string::npos;
    for (size_t i = 0; i + needle.size() <= hay.size(); i++) {
        size_t j = 0;
        for (; j < needle.size(); j++) {
            char a = hay[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
        }
        if (j == needle.size()) return i;
    }
    return std::string::npos;
}

static std::string StripControl(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '(') {
            i += 2;
            while (i < s.size() && s[i] != ')') i++;
            if (i < s.size()) i++;
            continue;
        }
        unsigned char u = (unsigned char)c;
        bool ws = (u < 0x20) || (c == ' ');
        if (ws) {
            if (out.empty() || out.back() == ' ') continue;
            out.push_back(' ');
            continue;
        }
        out.push_back(c);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

static std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static void SplitName(const std::string& cleaned, std::string& name, std::string& text) {
    size_t pos = cleaned.find(" : ");
    size_t sepLen = 3;
    if (pos == std::string::npos) {
        pos = cleaned.find(": ");
        sepLen = 2;
    }
    if (pos == std::string::npos || pos == 0) {
        name.clear();
        text = cleaned;
        return;
    }
    name = Trim(cleaned.substr(0, pos));
    text = Trim(cleaned.substr(pos + sepLen));
    if (text.empty()) text = cleaned;
}

static bool ToUtf8IfTranslatable(const std::string& raw, std::string& out) {
    if (!NormalizeToUtf8(raw, out)) return false;
    return HasLettersUtf8(out);
}

static std::string ReadCString(const char* p, int maxLen) {
    if (maxLen <= 0 || !p) return std::string();
    int n = 0;
    while (n < maxLen && p[n] != '\0') n++;
    return std::string(p, (size_t)n);
}

static std::string ApplyFormat(const std::string& fmt, const std::vector<std::string>& args) {
    std::string out;
    out.reserve(fmt.size() + 64);
    int seq = 0;
    for (size_t i = 0; i < fmt.size(); i++) {
        char c = fmt[i];
        if (c != '%' || i + 1 >= fmt.size()) {
            out.push_back(c);
            continue;
        }
        char n = fmt[i + 1];
        if (n == '%') {
            out.push_back('%');
            i++;
            continue;
        }
        if (n == 's') {
            int idx;
            size_t adv = 2;
            if (i + 2 < fmt.size() && fmt[i + 2] >= '1' && fmt[i + 2] <= '4') {
                idx = fmt[i + 2] - '1';
                adv = 3;
            } else {
                idx = seq++;
            }
            i += adv - 1;
            if (idx >= 0 && idx < (int)args.size()) out += args[(size_t)idx];
            continue;
        }
        out.push_back(c);
    }
    return out;
}

bool ExtractChat(const char* msgName, const void* buf, int size, ChatItem& out) {
    if (!buf || size <= 0) return false;
    const char* p = (const char*)buf;

    std::string candidate;
    bool playerChat = false;
    int clientIndex = 0;

    if (msgName && strcmp(msgName, "SayText") == 0) {
        if (size < 2) return false;
        clientIndex = (unsigned char)p[0];

        const char* q = p + 1;
        const char* end = p + size;
        std::vector<std::string> parts;
        while (q < end && parts.size() < 8) {
            const char* z = (const char*)memchr(q, '\0', (size_t)(end - q));
            size_t n = z ? (size_t)(z - q) : (size_t)(end - q);
            parts.push_back(std::string(q, n));
            if (!z) break;
            q = z + 1;
        }
        if (parts.empty()) return false;
        candidate = parts[0];

        const unsigned char lead =
            candidate.empty() ? 0 : (unsigned char)candidate[0];
        playerChat = (lead != kServerMarker);

        if (!candidate.empty() && (unsigned char)candidate[0] == kChatMarker) {
            playerChat = true;
            candidate.erase(0, 1);
        } else if (!candidate.empty() && candidate[0] == '#') {
            playerChat = true;
            if (parts.size() >= 3) {
                if (parts[1].empty())
                    candidate = parts[2];
                else if (parts[2].empty() ||
                         parts[1].find(" : ") != std::string::npos)
                    candidate = parts[1];
                else
                    candidate = parts[1] + " : " + parts[2];
            } else if (parts.size() == 2) {
                candidate = parts[1];
            } else {
                candidate.clear();
            }
        } else if (candidate.find("%s") != std::string::npos) {
            if (parts.size() < 2) return false;
            std::vector<std::string> args(parts.begin() + 1, parts.end());
            int ph = 0;
            for (size_t i = 0; i + 1 < parts[0].size(); i++) {
                if (parts[0][i] == '%' && parts[0][i + 1] == 's') { ph++; i++; }
            }
            candidate = ApplyFormat(parts[0], args);
            for (size_t i = (size_t)ph; i < args.size(); i++)
                candidate += " : " + args[i];
            playerChat = playerChat && candidate.find(" : ") != std::string::npos;
        }
        if (candidate.empty()) return false;
        std::string all;
        for (size_t i = 0; i < parts.size(); i++) {
            if (i) all += " | ";
            all += "[" + parts[i].substr(0, 40) + "]";
        }
        xlog::Write("SayText parts: %u part(s)  %s", (unsigned)parts.size(), all.c_str());
    } else {
        const char* q = p + 1;
        const char* end = p + size;
        std::vector<std::string> strs;
        while (q < end && strs.size() < 5) {
            size_t n = 0;
            while (q + n < end && q[n] != '\0') n++;
            strs.push_back(std::string(q, n));
            q += n + 1;
        }
        if (strs.empty() || strs[0].empty()) return false;

        const std::string& fmt = strs[0];
        if (fmt[0] != '#') {
            std::vector<std::string> args(strs.begin() + 1, strs.end());
            candidate = ApplyFormat(fmt, args);
        }
        {
            std::string chk;
            if (!ToUtf8IfTranslatable(candidate, chk)) {
                candidate.clear();
                for (size_t i = 1; i < strs.size(); i++) {
                    std::string u;
                    if (ToUtf8IfTranslatable(strs[i], u)) {
                        candidate = strs[i];
                        break;
                    }
                }
            }
        }
        if (candidate.empty()) return false;
    }

    std::string cleaned = StripControl(candidate);
    if (cleaned.empty()) return false;

    if (!cleaned.empty() && cleaned[0] == '*') {
        cleaned.erase(0, 1);
        size_t b = cleaned.find_first_not_of(' ');
        cleaned = (b == std::string::npos) ? std::string() : cleaned.substr(b);
    }
    if (cleaned.empty()) return false;

    std::string rawUtf8;
    if (!NormalizeToUtf8(cleaned, rawUtf8)) return false;
    if (!HasLettersUtf8(rawUtf8)) return false;

    std::string name, text;
    SplitName(rawUtf8, name, text);
    if (text.empty() || !HasLettersUtf8(text)) {
        name.clear();
        text = rawUtf8;
    }

    if (text.find("%s") != std::string::npos || text == "%s") return false;
    if (name == "%s") name.clear();

    if (!g_cfg.serverMsgs) {
        bool srv = false;
        if (text.size() > 2 && text[0] == '[') {
            size_t close = text.find(']');
            if (close != std::string::npos && close <= 20) srv = true;
        }
        if (!srv) {
            size_t lb = text.rfind('[');
            if (lb != std::string::npos && text.size() - lb <= 12 &&
                text.find(']', lb) != std::string::npos)
                srv = true;
        }
        if (!srv && name.size() >= 4 && FindNoCase(text, name) != std::string::npos)
            srv = true;
        if (srv) playerChat = false;
    }

    out.msgName = msgName ? msgName : "";
    out.clientIndex = clientIndex;
    out.playerChat = playerChat;
    out.name = name;
    out.text = text;
    return true;
}
