// SPDX-License-Identifier: GPL-2.0-only
#include "worker.h"
#include <cwchar>
#include <devpkey.h>
#include <iostream>
#include <setupapi.h>
#include <string>

const DEVPROPKEY dmaPolicyKey{
    {0x83da6326,
     0x97a6,
     0x4088,
     {0x94, 0x53, 0xa1, 0x92, 0x3f, 0x57, 0x3b, 0x29}},
    18};

int main(int argc, char **argv) {
  if (argc == 2 && std::string(argv[1]) == "--verify-policy-key") {
    HDEVINFO list = SetupDiGetClassDevsW(nullptr, L"PCI", nullptr,
                                         DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (list == INVALID_HANDLE_VALUE) return 3;
    int result = 3;
    for (DWORD index = 0;; ++index) {
      SP_DEVINFO_DATA device{};
      device.cbSize = sizeof(device);
      if (!SetupDiEnumDeviceInfo(list, index, &device)) break;
      wchar_t id[512]{};
      if (!SetupDiGetDeviceInstanceIdW(list, &device, id, 512, nullptr))
        continue;
      constexpr auto prefix = L"PCI\\VEN_14C3&DEV_7925&SUBSYS_E112105B";
      if (_wcsnicmp(id, prefix, std::wcslen(prefix))) continue;
      DEVPROPTYPE type = 0;
      INT32 policy = 0;
      DWORD used = 0;
      const BOOL got = SetupDiGetDevicePropertyW(
          list, &device, &dmaPolicyKey, &type, reinterpret_cast<PBYTE>(&policy),
          sizeof(policy), &used, 0);
      std::cerr << "Policy key diagnostic: returned=" << got
                << " error=" << (got ? 0 : GetLastError()) << " type=" << type
                << " bytes=" << used << " value=" << policy << '\n';
      if (got && (type == DEVPROP_TYPE_INT32 || type == DEVPROP_TYPE_UINT32) &&
          used == sizeof(policy) && policy == 2)
        result = 0;
    }
    SetupDiDestroyDeviceInfoList(list);
    return result;
  }

  if (argc != 3 || std::string(argv[1]) != "--awdl-service") return 2;
  const std::string name = argv[2];
  const std::string prefix = "Global\\winawdl-mt7925-native-stop-";
  if (name.size() != prefix.size() + 32 ||
      name.compare(0, prefix.size(), prefix) != 0 ||
      name.find_first_not_of("0123456789abcdef", prefix.size()) !=
          std::string::npos)
    return 2;
  HANDLE stop = OpenEventA(SYNCHRONIZE, FALSE, name.c_str());
  if (!stop) return 2;
  int result = 3;
  try {
    result = runMediaTekSession(stop);
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  CloseHandle(stop);
  return result;
}
