#pragma once
#include <windows.h>

#include <string>

bool TryHookClient(HMODULE hClient, const char* via = "?");
bool IsClientHooked();

void DrainConsole();

void RegisterCommandsNow();
void ConsoleWriteNow(const std::string& text);
void PrintCommandList();
void PatchEngineTableEarly();
