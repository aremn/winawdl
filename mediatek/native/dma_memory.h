// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>
#include <windows.h>

struct MtkAweAllocation {
  std::vector<ULONG_PTR> frames;
  std::vector<size_t> low;
  unsigned attempts = 0, mib = 0, flags = 0;
};

// Keep OS-returned PFNs intact. Low indices, not guessed/truncated addresses,
// select the buffers that the MT7925's 32-bit DMA engine can reach.
inline MtkAweAllocation mtkAllocateAwe(unsigned required) {
  using Allocate2 = BOOL(WINAPI *)(HANDLE, PULONG_PTR, PULONG_PTR,
                                   PMEM_EXTENDED_PARAMETER, ULONG);
  const auto allocate2 = reinterpret_cast<Allocate2>(GetProcAddress(
      GetModuleHandleW(L"kernelbase.dll"), "AllocateUserPhysicalPages2"));
  struct Method {
    unsigned mib, flags;
  };
  // Accumulate useful candidates rather than discarding scarce low pages.
  // Never hold more than 256 MiB, and yield between attempts for reclamation.
  const Method methods[] = {{2, MEM_EXTENDED_PARAMETER_NONPAGED_LARGE},
                            {2, MEM_EXTENDED_PARAMETER_NONPAGED_LARGE},
                            {2, MEM_EXTENDED_PARAMETER_NONPAGED_LARGE},
                            {2, MEM_EXTENDED_PARAMETER_NONPAGED_LARGE},
                            {4, MEM_EXTENDED_PARAMETER_NONPAGED_LARGE},
                            {4, MEM_EXTENDED_PARAMETER_NONPAGED_LARGE},
                            {8, MEM_EXTENDED_PARAMETER_NONPAGED_LARGE},
                            {8, MEM_EXTENDED_PARAMETER_NONPAGED_LARGE},
                            {16, MEM_EXTENDED_PARAMETER_NONPAGED_LARGE},
                            {16, MEM_EXTENDED_PARAMETER_NONPAGED_LARGE},
                            {64, 0},
                            {64, 0},
                            {128, 0}};
  MtkAweAllocation result;
  bool largeOnly = true;
  for (const auto method : methods) {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status) ||
        status.ullAvailPhys < 4ull * 1024 * 1024 * 1024)
      break;
    const auto heldMiB = (result.frames.size() + 255) / 256;
    if (heldMiB >= 256) break;
    if (result.attempts) Sleep(500);
    ++result.attempts;
    std::vector<ULONG_PTR> candidate(
        (std::min)(size_t(method.mib), size_t(256) - heldMiB) * 256);
    ULONG_PTR count = candidate.size();
    MEM_EXTENDED_PARAMETER parameter{};
    parameter.Type = MemExtendedParameterAttributeFlags;
    parameter.ULong64 = method.flags;
    const BOOL allocated =
        method.flags ? (allocate2 && allocate2(GetCurrentProcess(), &count,
                                               candidate.data(), &parameter, 1))
                     : AllocateUserPhysicalPages(GetCurrentProcess(), &count,
                                                 candidate.data());
    if (!allocated) continue;
    candidate.resize(count);
    const bool useful =
        std::any_of(candidate.begin(), candidate.end(), [](ULONG_PTR pfn) {
          return pfn && pfn < (ULONG_PTR{1} << 20);
        });
    if (useful) {
      const auto base = result.frames.size();
      result.frames.insert(result.frames.end(), candidate.begin(),
                           candidate.end());
      for (size_t index = 0; index < count; ++index)
        if (candidate[index] && candidate[index] < (ULONG_PTR{1} << 20))
          result.low.push_back(base + index);
      largeOnly =
          largeOnly && method.flags == MEM_EXTENDED_PARAMETER_NONPAGED_LARGE;
      if (result.low.size() >= required) {
        result.mib = static_cast<unsigned>((result.frames.size() + 255) / 256);
        result.flags = largeOnly ? MEM_EXTENDED_PARAMETER_NONPAGED_LARGE : 0;
        return result;
      }
    } else {
      ULONG_PTR released = count;
      if (!FreeUserPhysicalPages(GetCurrentProcess(), &released,
                                 candidate.data()) ||
          released != count)
        throw std::runtime_error("A failed memory candidate could not be "
                                 "released; process must end before retry");
    }
  }
  const auto available = result.low.size();
  if (!result.frames.empty()) {
    ULONG_PTR released = result.frames.size();
    if (!FreeUserPhysicalPages(GetCurrentProcess(), &released,
                               result.frames.data()) ||
        released != result.frames.size())
      throw std::runtime_error("Accumulated memory could not be released; "
                               "process must end before retry");
  }
  throw std::runtime_error("Low physical pages: required " +
                           std::to_string(required) + ", available " +
                           std::to_string(available));
}
