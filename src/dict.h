#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>

struct DictEntry {
    std::string sl;
    bool keep = false;
    std::unordered_map<std::string, std::string> tr;
};

const DictEntry* DictFind(const std::string& wordLower);
std::string DictForceSl(const std::string& text);
bool IsJunkText(const std::string& text);
size_t DictSize();
