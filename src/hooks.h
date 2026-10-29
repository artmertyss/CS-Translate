#pragma once
#include <windows.h>

#include <string>

bool TryHookClient(HMODULE hClient, const char* via = "?");
bool IsClientHooked();

bool InstallLoaderHooks();

void DrainConsole();

void RegisterCommandsNow();
void ConsoleWriteNow(const std::string& text);
void PrintCommandList();
void PatchEngineTableEarly();
