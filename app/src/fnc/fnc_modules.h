// fnc-nx: the SDK's DLLs carry the build type's suffix (rexruntime.dll in Release, rexruntimerd.dll in RelWithDebInfo,
// rexruntimed.dll in Debug). FNC Reborn's code looked them up by the Release name only; this finds whichever is loaded.
#pragma once
#if defined(_WIN32)
#include <windows.h>
#include <string>

inline HMODULE FncSdkModule(const wchar_t* stem) {
  for (const wchar_t* suffix : {L".dll", L"rd.dll", L"d.dll"}) {
    if (HMODULE m = GetModuleHandleW((std::wstring(stem) + suffix).c_str())) return m;
  }
  return nullptr;
}
#endif
