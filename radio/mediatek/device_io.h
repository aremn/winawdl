// SPDX-License-Identifier: MIT
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../../driver/regproxy/regproxy_ioctl.h"
#include <windows.h>
#include <winioctl.h>

HANDLE openProxy();
unsigned operation(HANDLE handle, unsigned offset,
                   unsigned kind = REGPROXY_READ, unsigned value = 0);
bool stopDma(HANDLE handle);
