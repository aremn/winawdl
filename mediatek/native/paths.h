// SPDX-License-Identifier: MIT
#pragma once
#include <filesystem>
#include <stdexcept>
#include <windows.h>

inline std::filesystem::path mtkRoot() {
  wchar_t name[32768]{};
  const auto size = GetModuleFileNameW(nullptr, name, 32768);
  if (!size || size == 32768)
    throw std::runtime_error("Executable path is unavailable");
  return std::filesystem::path(name).parent_path().parent_path().parent_path();
}

inline std::filesystem::path statePath(const wchar_t *name) {
  wchar_t base[32768]{};
  const auto size = GetEnvironmentVariableW(L"ProgramData", base, 32768);
  if (!size || size >= 32768)
    throw std::runtime_error("ProgramData is unavailable");
  return std::filesystem::path(base) / L"winawdl" / L"radio" / name;
}
