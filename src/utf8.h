#pragma once
#include <string>
#include <vector>

bool LooksLikeUtf8(const std::string& s);

bool HasCyrillicUtf8(const std::string& utf8);

bool HasCyrillicCp1251(const std::string& raw);

std::string Cp1251ToUtf8(const std::string& raw);

bool NormalizeToUtf8(const std::string& raw, std::string& outUtf8);

std::string ToAsciiTurkish(const std::string& utf8);

std::string LatinToCyrillic(const std::string& utf8);

std::string ToConsoleCp1251(const std::string& utf8);

std::string UrlEncode(const std::string& s);

bool JsonDecodeString(const std::string& json, size_t& pos, std::string& out);

std::vector<std::string> SplitLines(const std::string& s);

std::vector<std::string> ExtractWords(const std::string& utf8, int maxWords);

bool HasLettersUtf8(const std::string& utf8);

int LetterCountUtf8(const std::string& utf8);

bool HasTurkishUtf8(const std::string& utf8);

bool IsCyrillicCodepoint(unsigned cp);
