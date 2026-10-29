#pragma once
#include <windows.h>

#include <string>

namespace xlog {

void SetModuleHandle(HMODULE h);

std::string ModuleDir();
std::string ModulePath();

void Init(bool enabled);
void Write(const char* fmt, ...);
bool Enabled();

}
