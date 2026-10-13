// SPDX-License-Identifier: MIT
#pragma once
#include "../regproxy/regproxy_ioctl.h"

// Restrict the worker to DMA enable bits and its own queue configuration.
static BOOLEAN mtkRegisterOperationIsValid(const REGPROXY_OP *op) {
  if (op->width != 4 || (op->offset & 3)) return FALSE;
  if (op->kind == REGPROXY_READ) {
    return op->offset == 0x2208 ||
           (op->offset >= 0x2300 && op->offset <= 0x241c) ||
           (op->offset >= 0x2500 && op->offset <= 0x253c);
  }
  if (op->offset == 0x2208) {
    return (op->kind == REGPROXY_CLEAR_BITS && op->value == 5) ||
           (op->kind == REGPROXY_SET_BITS &&
            (op->value == 1 || op->value == 5));
  }
  if (op->kind != REGPROXY_WRITE) return FALSE;
  switch (op->offset) {
  case 0x220c:
    return op->value == 0x8000;
  case 0x2280:
    return op->value == 15;
  case 0x23f0:
  case 0x2500:
  case 0x2520:
    return op->value != 0 && !(op->value & 4095);
  case 0x23f4:
    return op->value == 256;
  case 0x23f8:
    return op->value <= 255;
  case 0x23fc:
  case 0x2514:
  case 0x2534:
  case 0x2518:
  case 0x2538:
    return op->value == 0;
  case 0x2504:
  case 0x2524:
    return op->value == 0 || op->value == 32;
  case 0x2508:
  case 0x2528:
    return op->value <= 31;
  default:
    return FALSE;
  }
}
