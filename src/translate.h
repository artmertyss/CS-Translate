#pragma once
#include <string>
#include <vector>

struct TranslationResult {
    bool ok = false;
    std::string sentence;
    std::vector<std::string> words;
    std::string detectedSrc;
    double conf = 1.0;
    std::string error;
};

bool Translate(const std::string& srcLang, const std::string& dstLang,
               const std::string& sentenceUtf8, const std::vector<std::string>& wordsUtf8,
               TranslationResult& out);

bool HttpGetUtf8(const std::string& host, const std::string& path, std::string& body,
                 std::string& err);

bool ParseGoogleResponse(const std::string& json, std::vector<std::string>& segs,
                         std::string* detected = nullptr, double* conf = nullptr);

void ResetHttpCalls();
int HttpCalls();

std::string LangFullName(const std::string& code);
