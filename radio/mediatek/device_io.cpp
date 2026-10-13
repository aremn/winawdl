// SPDX-License-Identifier: MIT
#include "device_io.h"
#include <cwchar>
#include <devpkey.h>
#include <setupapi.h>
#include <stdexcept>
#include <vector>
// Microsoft documents this property key; it is absent from this SDK's
// devpkey.h.
const DEVPROPKEY dmaPolicyKey{
    {0x83da6326,
     0x97a6,
     0x4088,
     {0x94, 0x53, 0xa1, 0x92, 0x3f, 0x57, 0x3b, 0x29}},
    18};

HANDLE openProxy() {
  HDEVINFO list =
      SetupDiGetClassDevsW(&GUID_DEVINTERFACE_WINAWDL_REGPROXY, nullptr,
                           nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (list == INVALID_HANDLE_VALUE)
    throw std::runtime_error("Device enumeration failed");
  HANDLE handle = INVALID_HANDLE_VALUE;
  for (DWORD index = 0;; ++index) {
    SP_DEVICE_INTERFACE_DATA item{};
    item.cbSize = sizeof(item);
    if (!SetupDiEnumDeviceInterfaces(
            list, nullptr, &GUID_DEVINTERFACE_WINAWDL_REGPROXY, index, &item))
      break;
    DWORD bytes = 0;
    SetupDiGetDeviceInterfaceDetailW(list, &item, nullptr, 0, &bytes, nullptr);
    if (bytes < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) || bytes > 65536)
      continue;
    std::vector<unsigned char> storage(bytes);
    auto *detail =
        reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(storage.data());
    detail->cbSize = sizeof(*detail);
    SP_DEVINFO_DATA device{};
    device.cbSize = sizeof(device);
    if (!SetupDiGetDeviceInterfaceDetailW(list, &item, detail, bytes, nullptr,
                                          &device))
      continue;
    wchar_t id[512]{};
    if (!SetupDiGetDeviceInstanceIdW(list, &device, id, 512, nullptr)) continue;
    constexpr auto prefix = L"PCI\\VEN_14C3&DEV_7925&SUBSYS_E112105B";
    if (_wcsnicmp(id, prefix, std::wcslen(prefix))) continue;
    DEVPROPTYPE type = 0;
    INT32 policy = 0;
    DWORD used = 0;
    const BOOL got = SetupDiGetDevicePropertyW(
        list, &device, &dmaPolicyKey, &type, reinterpret_cast<PBYTE>(&policy),
        sizeof(policy), &used, 0);
    if ((got && ((type != DEVPROP_TYPE_INT32 && type != DEVPROP_TYPE_UINT32) ||
                 used != sizeof(policy) || policy != 0)) ||
        (!got && GetLastError() != ERROR_NOT_FOUND)) {
      SetupDiDestroyDeviceInfoList(list);
      throw std::runtime_error(
          "Physical DMA policy is not the reviewed UMDF policy");
    }
    if (handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
      SetupDiDestroyDeviceInfoList(list);
      throw std::runtime_error("Multiple supported MT7925 devices");
    }
    handle = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, 0,
                         nullptr, OPEN_EXISTING, 0, nullptr);
  }
  SetupDiDestroyDeviceInfoList(list);
  if (handle == INVALID_HANDLE_VALUE)
    throw std::runtime_error("MT7925 radio driver could not be opened");
  return handle;
}

unsigned operation(HANDLE handle, unsigned offset, unsigned kind,
                   unsigned value) {
  REGPROXY_OP op{offset, value, 4, static_cast<unsigned char>(kind), 0};
  DWORD bytes = 0;
  if (!DeviceIoControl(handle, IOCTL_REGPROXY_BATCH, &op, sizeof(op), &op,
                       sizeof(op), &bytes, nullptr) ||
      bytes != sizeof(op))
    throw std::runtime_error("Register request failed");
  return op.value;
}

bool stopDma(HANDLE handle) {
  try {
    operation(handle, 0x2208, REGPROXY_CLEAR_BITS, 5);
    for (int attempt = 0; attempt < 200; ++attempt) {
      if ((operation(handle, 0x2208) & 15u) == 0) return true;
      Sleep(1);
    }
  } catch (...) {
  }
  return false;
}
