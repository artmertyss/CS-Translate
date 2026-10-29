#include "translate.h"

#include <windows.h>
#include <winhttp.h>

#include <cctype>
#include <cstring>

#include "utf8.h"

#pragma comment(lib, "winhttp.lib")

static void SkipWs(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
}

static bool Expect(const std::string& s, size_t& i, char c) {
    SkipWs(s, i);
    if (i < s.size() && s[i] == c) {
        i++;
        return true;
    }
    return false;
}

static bool SkipValue(const std::string& s, size_t& i) {
    SkipWs(s, i);
    if (i >= s.size()) return false;
    char c = s[i];
    if (c == '"') {
        std::string tmp;
        return JsonDecodeString(s, i, tmp);
    }
    if (c == '[') {
        i++;
        if (Expect(s, i, ']')) return true;
        for (;;) {
            if (!SkipValue(s, i)) return false;
            SkipWs(s, i);
            if (i < s.size() && s[i] == ',') {
                i++;
                continue;
            }
            if (i < s.size() && s[i] == ']') {
                i++;
                return true;
            }
            return false;
        }
    }
    if (c == '{') {
        i++;
        if (Expect(s, i, '}')) return true;
        for (;;) {
            std::string key;
            if (!JsonDecodeString(s, i, key)) return false;
            if (!Expect(s, i, ':')) return false;
            if (!SkipValue(s, i)) return false;
            SkipWs(s, i);
            if (i < s.size() && s[i] == ',') {
                i++;
                continue;
            }
            if (i < s.size() && s[i] == '}') {
                i++;
                return true;
            }
            return false;
        }
    }
    while (i < s.size() && (isalnum((unsigned char)s[i]) || s[i] == '-' || s[i] == '+' ||
                            s[i] == '.')) {
        i++;
    }
    return true;
}

bool ParseGoogleResponse(const std::string& json, std::vector<std::string>& segs,
                         std::string* detected, double* conf) {
    segs.clear();
    if (detected) detected->clear();
    if (conf) {
        double best = -1.0;
        for (size_t p = 0; p < json.size(); p++) {
            if (json[p] != '[') continue;
            size_t k = p + 1;
            if (k < json.size() && json[k] == '-') continue;
            double v = 0.0;
            bool got = false;
            while (k < json.size() && json[k] >= '0' && json[k] <= '9') {
                v = v * 10.0 + (json[k] - '0');
                k++;
                got = true;
            }
            if (k < json.size() && json[k] == '.') {
                k++;
                double f = 0.1;
                while (k < json.size() && json[k] >= '0' && json[k] <= '9') {
                    v += (json[k] - '0') * f;
                    f *= 0.1;
                    k++;
                    got = true;
                }
            }
            if (got && k < json.size() && json[k] == ']' && v > 0.0 && v <= 1.0) best = v;
        }
        *conf = (best > 0.0) ? best : 1.0;
    }
    size_t i = 0;
    if (!Expect(json, i, '[')) return false;
    if (!Expect(json, i, '[')) return false;
    if (Expect(json, i, ']')) return true;
    for (;;) {
        if (!Expect(json, i, '[')) return false;
        std::string text;
        if (!JsonDecodeString(json, i, text)) return false;
        segs.push_back(text);
        SkipWs(json, i);
        while (i < json.size() && json[i] == ',') {
            i++;
            if (!SkipValue(json, i)) return false;
            SkipWs(json, i);
        }
        if (i >= json.size() || json[i] != ']') return false;
        i++;
        SkipWs(json, i);
        if (i < json.size() && json[i] == ',') {
            i++;
            continue;
        }
        if (i < json.size() && json[i] == ']') {
            i++;
            if (detected) {
                SkipWs(json, i);
                if (i < json.size() && json[i] == ',') {
                    i++;
                    SkipValue(json, i);
                    SkipWs(json, i);
                }
                if (i < json.size() && json[i] == ',') {
                    i++;
                    SkipWs(json, i);
                    if (i < json.size() && (json[i] == '"' || json[i] == '\''))
                        JsonDecodeString(json, i, *detected);
                }
            }
            return true;
        }
        return false;
    }
}

static std::wstring Widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static thread_local int g_httpCalls = 0;

void ResetHttpCalls() { g_httpCalls = 0; }
int HttpCalls() { return g_httpCalls; }

bool HttpGetUtf8(const std::string& host, const std::string& path, std::string& body,
                 std::string& err) {
    body.clear();
    err.clear();
    g_httpCalls++;
    static HINTERNET hSess = nullptr;
    auto newSession = [&]() -> HINTERNET {
        HINTERNET h = WinHttpOpen(L"Mozilla/5.0 (compatible; cs_translate/1.0)",
                                  WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (h) WinHttpSetTimeouts(h, 3000, 3000, 6000, 6000);
        return h;
    };
    auto killSession = [&]() {
        if (hSess) {
            WinHttpCloseHandle(hSess);
            hSess = nullptr;
        }
    };
    if (!hSess) {
        hSess = newSession();
        if (!hSess) {
            err = "WinHttpOpen failed: " + std::to_string(GetLastError());
            return false;
        }
    }

    HINTERNET hConn = WinHttpConnect(hSess, Widen(host).c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConn) {
        err = "WinHttpConnect failed: " + std::to_string(GetLastError());
        killSession();
        return false;
    }

    HINTERNET hReq = WinHttpOpenRequest(hConn, L"GET", Widen(path).c_str(), nullptr,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        WINHTTP_FLAG_SECURE);
    if (!hReq) {
        err = "WinHttpOpenRequest failed: " + std::to_string(GetLastError());
        WinHttpCloseHandle(hConn);
        killSession();
        return false;
    }

    BOOL ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
                                 0, 0, 0);
    if (ok) ok = WinHttpReceiveResponse(hReq, nullptr);

    if (!ok) {
        err = "http request failed: " + std::to_string(GetLastError());
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConn);
        killSession();
        return false;
    }

    DWORD status = 0;
    DWORD sz = sizeof(status);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        err = "http status " + std::to_string((int)status);
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConn);
        if (status == 429 || status >= 500) killSession();
        return false;
    }

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hReq, &avail)) {
            err = "QueryDataAvailable: " + std::to_string(GetLastError());
            break;
        }
        if (avail == 0) break;
        std::vector<char> chunk(avail + 1, 0);
        DWORD read = 0;
        if (!WinHttpReadData(hReq, chunk.data(), avail, &read)) {
            err = "ReadData: " + std::to_string(GetLastError());
            break;
        }
        if (read == 0) break;
        body.append(chunk.data(), read);
    }

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConn);
    if (!err.empty()) killSession();

    if (!err.empty()) return false;
    if (body.empty()) {
        err = "empty response";
        return false;
    }
    return true;
}

bool Translate(const std::string& srcLang, const std::string& dstLang,
               const std::string& sentenceUtf8, const std::vector<std::string>& wordsUtf8,
               TranslationResult& out) {
    out = TranslationResult();

    std::string q = sentenceUtf8;
    for (const auto& w : wordsUtf8) {
        q.push_back('\n');
        q += w;
    }

    std::string path =
        "/translate_a/single?client=gtx&sl=" + UrlEncode(srcLang) +
        "&tl=" + UrlEncode(dstLang) + "&dt=t&q=" + UrlEncode(q);

    std::string body, err;
    if (!HttpGetUtf8("translate.googleapis.com", path, body, err)) {
        if (!sentenceUtf8.empty()) {
            std::string path2 =
                "/translate_a/single?client=gtx&sl=" + UrlEncode(srcLang) +
                "&tl=" + UrlEncode(dstLang) + "&dt=t&q=" + UrlEncode(sentenceUtf8);
            if (!HttpGetUtf8("translate.googleapis.com", path2, body, err)) {
                out.error = err;
                return false;
            }
            std::vector<std::string> segs;
            if (!ParseGoogleResponse(body, segs, &out.detectedSrc, &out.conf)) {
                out.error = "json parse failed";
                return false;
            }
            out.sentence.clear();
            for (auto& s : segs) out.sentence += s;
            out.ok = true;
            return true;
        }
        out.error = err;
        return false;
    }

    std::vector<std::string> segs;
    if (!ParseGoogleResponse(body, segs, &out.detectedSrc, &out.conf)) {
        out.error = "json parse failed";
        return false;
    }

    std::string joined;
    for (auto& s : segs) joined += s;

    std::vector<std::string> lines = SplitLines(joined);
    if (lines.size() == wordsUtf8.size() + 1) {
        out.sentence = lines[0];
        out.words.assign(lines.begin() + 1, lines.end());
    } else if (wordsUtf8.empty()) {
        out.sentence = joined;
    } else {
        out.sentence = lines.empty() ? joined : lines[0];
        out.words.clear();
    }
    out.ok = true;
    return true;
}

std::string LangFullName(const std::string& code) {
    std::string c;
    for (char ch : code) c.push_back((char)tolower((unsigned char)ch));
    static const char* kMap[][2] = {
        {"en", "english"},     {"ru", "russian"},    {"tr", "turkish"},
        {"de", "german"},      {"fr", "french"},     {"es", "spanish"},
        {"it", "italian"},     {"pt", "portuguese"}, {"pl", "polish"},
        {"nl", "dutch"},       {"sv", "swedish"},    {"no", "norwegian"},
        {"da", "danish"},      {"fi", "finnish"},    {"cs", "czech"},
        {"hu", "hungarian"},   {"ro", "romanian"},   {"el", "greek"},
        {"bg", "bulgarian"},   {"sr", "serbian"},    {"hr", "croatian"},
        {"sk", "slovak"},      {"sl", "slovenian"},  {"et", "estonian"},
        {"lv", "latvian"},     {"lt", "lithuanian"}, {"uk", "ukrainian"},
        {"be", "belarusian"},  {"kk", "kazakh"},     {"uz", "uzbek"},
        {"az", "azerbaijani"}, {"ka", "georgian"},   {"hy", "armenian"},
        {"ar", "arabic"},      {"fa", "persian"},    {"he", "hebrew"},
        {"hi", "hindi"},       {"bn", "bengali"},    {"ur", "urdu"},
        {"ja", "japanese"},    {"ko", "korean"},     {"zh", "chinese"},
        {"vi", "vietnamese"},  {"th", "thai"},       {"id", "indonesian"},
        {"ms", "malay"},       {"tl", "filipino"},   {"mn", "mongolian"},
        {"ka", "georgian"},    {"sq", "albanian"},   {"mk", "macedonian"},
        {"mt", "maltese"},     {"is", "icelandic"},  {"ga", "irish"},
        {"cy", "welsh"},       {"ca", "catalan"},    {"eu", "basque"},
        {"gl", "galician"},    {"af", "afrikaans"},  {"sw", "swahili"},
        {"am", "amharic"},     {"so", "somali"},     {"ha", "hausa"},
        {"yo", "yoruba"},      {"zu", "zulu"},       {"", ""},
    };
    for (int i = 0; kMap[i][0][0]; i++)
        if (c == kMap[i][0]) return kMap[i][1];
    return c.empty() ? std::string("unknown") : c;
}
