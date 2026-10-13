// SPDX-License-Identifier: GPL-2.0-only
// MT7925 AWDL session. Hardware command layouts are shared with offline tests.
// mt76 reference layouts: Copyright (C) 2023 MediaTek Inc., BSD-3-Clause-Clear.
#include "worker.h"
#include "../../mediatek/native/dma_memory.h"
#include "../../mediatek/native/paths.h"
#include "../../mediatek/native/protocol.h"
#include "device_io.h"
#include "json_string.h"
#include "owl_link.h"
#include <chrono>
#include <deque>
#include <fstream>
#include <memory>
#include <mmsystem.h>

// WFDMA descriptor: two buffer addresses, ownership/control and metadata.
struct Descriptor {
  unsigned buf0, ctrl, buf1, info;
};
static_assert(sizeof(Descriptor) == 16);

static uint64_t monotonicMicros() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

int runMediaTekSession(HANDLE stopEvent) {
  const auto reportPath = statePath(L"session-result.json");
  const auto releasePath = statePath(L"memory-release.signal");
  std::unique_ptr<OwlLink> awdlLink;
  // Replace stale health before memory allocation or hardware access.
  {
    std::ofstream health(statePath(L"native-health.json.tmp"), std::ios::trunc);
    health << "{\"running\":false,\"state\":\"allocating-memory\"}";
    health.close();
    MoveFileExW(statePath(L"native-health.json.tmp").c_str(),
                statePath(L"native-health.json").c_str(),
                MOVEFILE_REPLACE_EXISTING);
  }

  struct TimerResolution {
    bool active = timeBeginPeriod(1) == TIMERR_NOERROR;
    ~TimerResolution() {
      if (active) timeEndPeriod(1);
    }
  } timerResolution;
  HANDLE handle = INVALID_HANDLE_VALUE, token = nullptr;
  // Prefer a 2 MiB nonpaged-large allocation. All OS-returned PFNs are
  // checked before the radio can use the 37 packed 32-bit DMA pages.
  std::vector<ULONG_PTR> frames;
  unsigned allocationMiB = 0, allocationFlags = 0, allocationAttempts = 0;
  unsigned lowPagesAvailable = 0, lowPagesRequired = 0;
  ULONG_PTR count = 0;
  void *view = nullptr;
  bool programmed = false, done = false, stopped = false, mapped = false,
       freed = true;
  unsigned ringAddress = 0, dmaIndex = 0, descriptorControl = 0, setupBase = 0,
           setupIndex = 0;
  unsigned receivedEvents = 0, rxDmaIndex = 0;
  MtkNicCapability capability;
  std::array<uint8_t, 6> radioAddress{}, awdlPeer{};
  bool radioEnabled = false, snifferEnabled = false, radioDisabled = false,
       snifferDisabled = false;
  bool clockAttempted = false, clockEnabled = false, clockDisabled = false;

  
  unsigned rawFrames = 0, awdlFrames = 0, invalidFrames = 0, dataPackets = 0;
  unsigned radioTxSubmitted = 0, probeResponses = 0;
  unsigned ackFrames = 0;
  unsigned fragmentedDescriptors = 0, oversizedDropped = 0;
  unsigned controlDataDrained = 0;
  unsigned mcuForwardedFrames = 0;
  MtkTxStatus broadcastStatus, unicastStatus, alternateStatus,
      fwBroadcastStatus, fwUnicastStatus, fwAlternateStatus;
  bool deviceEnabled = false, bssEnabled = false, peerEnabled = false;
  bool deviceDisabled = false, bssDisabled = false, peerDisabled = false;
  bool rocAttempted = false, rocCommandAck = false, rocAborted = false;
  bool continuousRoc = false;
  MtkRocGrant rocGrant;
  unsigned awdlTxSubmitted = 0, awdlTxFirmwareCompleted = 0;
  unsigned awdlUnicastSubmitted = 0;
  unsigned dataTxSubmitted = 0, dataTxFirmwareCompleted = 0;
  bool dataTxAck = false;
  unsigned hostInterfaceIndex = 0, hostOtherPeerDropped = 0;
  unsigned hostUnicastDropped = 0, hostMulticastDropped = 0,
           dataUnicastAckCount = 0, dataUnicastFailedCount = 0;
  unsigned peerDataWindows = 0;
  unsigned ringWraps = 0;

  uint64_t peerLastRxUs = 0, maxUnicastDequeueDelayUs = 0;
  uint64_t lastUnicastPeerRxUs = 0;
  unsigned unicastInWindow = 0;
  uint64_t lastManagementTrialUs = 0;
  bool awdlPeerReferenced = false, awdlUnicastAttempted = false,
       awdlUnicastAck = false, awdlPeerLearned = false;
  struct PendingAction {
    bool active = false;
    bool unicast = false;
    uint16_t sequence = 0;
    bool data = false;
  };
  std::array<PendingAction, 3> pendingActions{};
  struct HeardPeer {
    std::array<uint8_t, 6> address{};
    uint64_t received = 0;
  };
  std::array<HeardPeer, 16> heardPeers{};
  auto peerReceived = [&](const std::array<uint8_t, 6> &mac) {
    for (const auto &item : heardPeers)
      if (item.address == mac) return item.received;
    return uint64_t(0);
  };
  auto hearPeer = [&](const uint8_t *mac) {
    if (mac[0] & 1 || std::equal(radioAddress.begin(), radioAddress.end(), mac))
      return;
    auto item = std::find_if(
        heardPeers.begin(), heardPeers.end(), [&](const HeardPeer &value) {
          return std::equal(value.address.begin(), value.address.end(), mac);
        });
    if (item == heardPeers.end())
      item = std::min_element(heardPeers.begin(), heardPeers.end(),
                              [](const HeardPeer &a, const HeardPeer &b) {
                                return a.received < b.received;
                              });
    std::copy(mac, mac + 6, item->address.begin());
    item->received = monotonicMicros();
  };
  std::vector<uint8_t> hostCandidate;
  uint64_t hostCandidateAt = 0;
  unsigned peerSwitches = 0, hostExpired = 0;
  std::string error;
  try {
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    if (system.dwPageSize != 4096)
      throw std::runtime_error("Unexpected page size");
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
      throw std::runtime_error("Token open failed");
    TOKEN_PRIVILEGES privilege{};
    privilege.PrivilegeCount = 1;
    privilege.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValueW(nullptr, L"SeLockMemoryPrivilege",
                               &privilege.Privileges[0].Luid))
      throw std::runtime_error("Privilege lookup failed");
    SetLastError(ERROR_SUCCESS);
    if (!AdjustTokenPrivileges(token, FALSE, &privilege, 0, nullptr, nullptr) ||
        GetLastError() != ERROR_SUCCESS)
      throw std::runtime_error("Lock memory privilege is absent");
    CloseHandle(token);
    token = nullptr;
    lowPagesRequired = 37u;
    auto allocation = mtkAllocateAwe(lowPagesRequired);
    allocationMiB = allocation.mib;
    allocationFlags = allocation.flags;
    allocationAttempts = allocation.attempts;
    frames = std::move(allocation.frames);
    auto low = std::move(allocation.low);
    count = frames.size();
    lowPagesAvailable = static_cast<unsigned>(low.size());
    view = VirtualAlloc(nullptr, count * 4096, MEM_RESERVE | MEM_PHYSICAL,
                        PAGE_READWRITE);
    if (!view || !MapUserPhysicalPages(view, count, frames.data()))
      throw std::runtime_error("Physical mapping failed");
    mapped = true;
    ZeroMemory(view, count * 4096);
    auto bufferPointer = [&](unsigned logical) {
      const auto slice = mtkDmaSlice(logical);
      return static_cast<unsigned char *>(view) + low.at(slice.page) * 4096 +
             slice.offset;
    };
    auto bufferAddress = [&](unsigned logical) {
      const auto slice = mtkDmaSlice(logical);
      return static_cast<unsigned>((frames[low.at(slice.page)] << 12) +
                                   slice.offset);
    };
    auto *ring = reinterpret_cast<volatile Descriptor *>(bufferPointer(0));
    auto *command = bufferPointer(1);
    ZeroMemory(command, 2048);
    for (unsigned index = 0; index < 256; ++index) {
      ring[index].buf0 = 0;
      ring[index].ctrl = 0x80000000;
      ring[index].buf1 = 0;
      ring[index].info = 0;
    }
    const auto request = mtkNicCapabilityRequest(1);
    CopyMemory(command, request.data(), request.size());
    ringAddress = bufferAddress(0);
    ring[0].buf0 = bufferAddress(1);
    ring[0].ctrl = 0x40380000; // LAST_SEC0 | (56 << 16), DDONE initially clear.
    volatile Descriptor *rxRing = nullptr;
    volatile Descriptor *dataRing = nullptr;

    rxRing = reinterpret_cast<volatile Descriptor *>(bufferPointer(2));
    for (unsigned index = 0; index < 32; ++index) {
      ZeroMemory(bufferPointer(3 + index), 2048);
      rxRing[index].buf0 = bufferAddress(3 + index);
      rxRing[index].buf1 = 0;
      rxRing[index].info = 0;
      rxRing[index].ctrl = 2048u << 16;
    }

    dataRing = reinterpret_cast<volatile Descriptor *>(bufferPointer(35));
    for (unsigned index = 0; index < 32; ++index) {
      ZeroMemory(bufferPointer(36 + index), 2048);
      dataRing[index].buf0 = bufferAddress(36 + index);
      dataRing[index].buf1 = 0;
      dataRing[index].info = 0;
      dataRing[index].ctrl = 2048u << 16;
    }

    MemoryBarrier();
    handle = openProxy();
    REGPROXY_INFO info{};
    DWORD received = 0;
    if (!DeviceIoControl(handle, IOCTL_REGPROXY_GET_INFO, nullptr, 0, &info,
                         sizeof(info), &received, nullptr) ||
        received != sizeof(info) || info.version != 2 ||
        info.bar_len < 0x200000)
      throw std::runtime_error("Unexpected BAR description");
    const auto global = operation(handle, 0x2208);
    if (global == 0 || global == 0xffffffff)
      throw std::runtime_error("DMA configuration is inaccessible");
    // Every OEM TX queue must be empty before handing its engine over.
    for (unsigned queue : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 15u, 16u, 17u})
      if (operation(handle, 0x2308 + queue * 16) !=
          operation(handle, 0x230c + queue * 16))
        throw std::runtime_error("An OEM TX queue is still pending");
    REGPROXY_OP cleanup{0x2208, 5, 4, REGPROXY_CLEAR_BITS, 0};
    if (!DeviceIoControl(handle, IOCTL_REGPROXY_SET_CLOSE_OPS, &cleanup,
                         sizeof(cleanup), nullptr, 0, &received, nullptr))
      throw std::runtime_error("DMA cleanup could not be armed");
    programmed = true;
    if (!stopDma(handle))
      throw std::runtime_error("The DMA engine did not become idle");
    operation(handle, 0x23f0, REGPROXY_WRITE, ringAddress);
    operation(handle, 0x23f4, REGPROXY_WRITE, 256);
    operation(handle, 0x23f8, REGPROXY_WRITE, 0);
    operation(handle, 0x23fc, REGPROXY_WRITE, 0);
    // Reset the MCU queue's internal DMA pointer/prefetch state only.
    // Writing DIDX alone does not reset this hardware-maintained counter.
    operation(handle, 0x220c, REGPROXY_WRITE, 0x8000);
    setupIndex = operation(handle, 0x23fc);
    setupBase = operation(handle, 0x23f0);
    if (setupIndex != 0 || setupBase != ringAddress)
      throw std::runtime_error("MCU ring setup did not confirm");

    // Disable all four MT7925 RX rings before enabling the shared RX
    // engine: none may retain an OEM buffer that has been released.
    for (unsigned queue = 0; queue < 4; ++queue) {
      operation(handle, 0x2504 + queue * 16, REGPROXY_WRITE, 0);
      operation(handle, 0x2508 + queue * 16, REGPROXY_WRITE, 0);
    }
    operation(handle, 0x2500, REGPROXY_WRITE, bufferAddress(2));
    operation(handle, 0x2504, REGPROXY_WRITE, 32);

    operation(handle, 0x2520, REGPROXY_WRITE, bufferAddress(35));
    operation(handle, 0x2524, REGPROXY_WRITE, 32);

    operation(handle, 0x2280, REGPROXY_WRITE, 15);
    operation(handle, 0x2508, REGPROXY_WRITE, 31);
    operation(handle, 0x2528, REGPROXY_WRITE, 31);
    for (unsigned queue = 0; queue < 4; ++queue)
      if (operation(handle, 0x250c + queue * 16) != 0 ||
          operation(handle, 0x2504 + queue * 16) !=
              (queue == 0 || (queue == 2) ? 32u : 0u))
        throw std::runtime_error("RX ring isolation did not confirm");

    MemoryBarrier();
    operation(handle, 0x2208, REGPROXY_SET_BITS, 5u);
    operation(handle, 0x23f8, REGPROXY_WRITE, 1);
    for (int attempt = 0; attempt < 1000; ++attempt) {
      dmaIndex = operation(handle, 0x23fc);
      MemoryBarrier();
      descriptorControl = ring[0].ctrl;
      if (dmaIndex == 1 && (descriptorControl & 0x80000000u)) {
        done = true;
        break;
      }
      Sleep(1);
    }
    if (!done) error = "MCU DMA completion was not observed";
    if (done) {

      unsigned tail = 0;
      unsigned dataTail = 0;
      std::deque<std::vector<uint8_t>> mcuFrames;
      auto drainControlData = [&] {
        if (!dataRing) return;
        // RX must keep advancing while we await a command reply, too.
        // Otherwise a full data ring can block the shared RX engine.
        for (unsigned drained = 0; drained < 32; ++drained) {
          MemoryBarrier();
          if (!(dataRing[dataTail].ctrl & 0x80000000u)) break;
          dataRing[dataTail].info = 0;
          dataRing[dataTail].ctrl = 2048u << 16;
          MemoryBarrier();
          operation(handle, 0x2528, REGPROXY_WRITE, dataTail);
          dataTail = (dataTail + 1) % 32;
          ++controlDataDrained;
        }
      };
      auto waitEvent = [&](auto predicate, unsigned waitMilliseconds = 3000) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(waitMilliseconds);
        while (std::chrono::steady_clock::now() < deadline) {
          MemoryBarrier();
          const auto control = rxRing[tail].ctrl;
          rxDmaIndex = operation(handle, 0x250c);
          if (!(control & 0x80000000u)) {
            // During the live AWDL loop the one-millisecond poll
            // leaves normal reception to its parser, not this drain.
            if (waitMilliseconds > 1) drainControlData();
            Sleep(1);
            continue;
          }
          const unsigned bytes = (control >> 16) & 0x3fff;
          if (!bytes || bytes > 2048 || !(control & 0x40000000u))
            throw std::runtime_error("Invalid or fragmented MCU RX descriptor");
          const auto *data = bufferPointer(3 + tail);

          ++receivedEvents;
          if (!mtkDot11Frame(data, bytes).empty()) {
            if (mcuFrames.size() == 32) mcuFrames.pop_front();
            mcuFrames.emplace_back(data, data + bytes);
            ++mcuForwardedFrames;
          }
          const auto broadcast = mtkTxStatus(data, bytes, 128),
                     unicast = mtkTxStatus(data, bytes, 129);
          if (broadcast.matched) broadcastStatus = broadcast;
          if (unicast.matched) unicastStatus = unicast;
          const auto alternate = mtkTxStatus(data, bytes, 130);
          if (alternate.matched) alternateStatus = alternate;
          const auto fwBroadcast = mtkFwTxStatus(data, bytes, 128, 1),
                     fwUnicast = mtkFwTxStatus(data, bytes, 129, 2),
                     fwAlternate = mtkFwTxStatus(data, bytes, 130, 3);
          if (fwBroadcast.matched) fwBroadcastStatus = fwBroadcast;
          if (fwUnicast.matched) fwUnicastStatus = fwUnicast;
          if (fwAlternate.matched) fwAlternateStatus = fwAlternate;

          for (unsigned slot = 0; slot < pendingActions.size(); ++slot) {
            auto &pending = pendingActions[slot];
            if (!pending.active) continue;
            const auto status = mtkFwTxStatus(data, bytes, uint8_t(128 + slot),
                                              pending.sequence);
            if (status.matched) {
              ++awdlTxFirmwareCompleted;
              if (pending.data) {
                ++dataTxFirmwareCompleted;
                if (pending.unicast) {
                  if (status.success) {
                    dataTxAck = true;
                    ++dataUnicastAckCount;
                  } else
                    ++dataUnicastFailedCount;
                }
              } else if (pending.unicast && status.success)
                awdlUnicastAck = true;
              pending.active = false;
            }
          }

          const bool matched = predicate(data, bytes);
          rxRing[tail].info = 0;
          rxRing[tail].ctrl = 2048u << 16;
          MemoryBarrier();
          operation(handle, 0x2508, REGPROXY_WRITE, tail);
          tail = (tail + 1) % 32;
          if (matched) return true;
        }
        return false;
      };
      if (!waitEvent([&](const uint8_t *data, size_t bytes) {
            const auto parsed = mtkParseNicCapability(data, bytes, 1);
            if (parsed.matched) capability = parsed;
            return capability.valid;
          }))
        throw std::runtime_error(
            "A complete matching NIC capability reply was not observed");

      radioAddress = capability.address;
      unsigned txSlot = 1;
      unsigned publications = 1;
      auto send = [&](const std::vector<uint8_t> &packet,
                      unsigned bufferIndex = 1) {
        if (publications >= 100000000u || packet.size() > 2048)
          throw std::runtime_error("MCU publication limit exceeded");
        MemoryBarrier();
        if (!(ring[(txSlot + 255) % 256].ctrl & 0x80000000u) ||
            operation(handle, 0x23fc) != txSlot)
          throw std::runtime_error(
              "The shared MCU command buffer is still owned by DMA");
        auto *buffer = bufferPointer(bufferIndex);
        ZeroMemory(buffer, 2048);
        CopyMemory(buffer, packet.data(), packet.size());
        ring[txSlot].buf0 = bufferAddress(bufferIndex);
        ring[txSlot].info = 0;
        ring[txSlot].ctrl =
            0x40000000u | (static_cast<unsigned>(packet.size()) << 16);
        const auto next = (txSlot + 1) % 256;
        MemoryBarrier();
        operation(handle, 0x23f8, REGPROXY_WRITE, next);
        ++publications;
        const auto deadline = GetTickCount64() + 1000;
        while (GetTickCount64() < deadline) {
          MemoryBarrier();
          if ((ring[txSlot].ctrl & 0x80000000u) &&
              operation(handle, 0x23fc) == next) {
            if (!next) ++ringWraps;
            txSlot = next;
            return;
          }
          Sleep(1);
        }
        throw std::runtime_error(
            "Subsequent MCU DMA completion was not observed");
      };
      auto commandReply = [&](uint8_t sequence, uint8_t cid) {
        MtkCommandResult result;
        waitEvent([&](const uint8_t *data, size_t bytes) {
          const auto reply = mtkCommandResult(data, bytes, sequence, cid);
          if (reply.matched) result = reply;
          return reply.matched;
        });
        return result.matched && result.status == 0;
      };
      try {

        clockAttempted = true;
        send(mtkClockRequest(true, 15));
        clockEnabled = commandReply(15, 0x0f);
        if (!clockEnabled)
          throw std::runtime_error(
              "Band-gap clock request was not acknowledged");

        send(mtkRadioRequest(true, 2));
        radioEnabled = commandReply(2, 8);
        if (!radioEnabled)
          throw std::runtime_error("Radio-enable command was not acknowledged");
        send(mtkSnifferRequest(true, 3));
        snifferEnabled = commandReply(3, 0x24);
        if (!snifferEnabled)
          throw std::runtime_error(
              "Sniffer-enable command was not acknowledged");

        if (!capability.addressValid)
          throw std::runtime_error(
              "A valid hardware MAC capability is required for the ACK test");

        awdlLink = std::make_unique<OwlLink>(stopEvent);
        awdlLink->start();
        radioAddress = awdlLink->address();

        send(mtkDeviceRequest(true, 6, radioAddress));
        deviceEnabled = commandReply(6, 1);
        if (!deviceEnabled)
          throw std::runtime_error("Own MAC activation was not acknowledged");
        send(mtkBssRequest(true, 7, mtkAwDlBssid));
        bssEnabled = commandReply(7, 2);
        if (!bssEnabled)
          throw std::runtime_error("Own BSS activation was not acknowledged");

        const unsigned requestedTenure = 86400000u;
        rocAttempted = true;
        send(mtkRocRequest(true, 13, requestedTenure));
        if (!waitEvent([&](const uint8_t *data, size_t length) {
              if (mtkCommandSucceeded(data, length, 13, 0x27))
                rocCommandAck = true;
              const auto grant = mtkRocGrant(data, length, requestedTenure);
              if (grant.matched) rocGrant = grant;
              return rocCommandAck && rocGrant.valid;
            }))
          throw std::runtime_error("AWDL channel 6 privilege was not granted");
        continuousRoc = rocGrant.duration >= 86400000u;
        if (!continuousRoc)
          throw std::runtime_error(
              "The required continuous channel privilege was not granted");

        std::vector<uint8_t> assembly;
        bool dropping = false;
        const auto captureStart = GetTickCount64();

        auto nextHealth = captureStart;
        const auto deadline = captureStart + 82800000ull;
        while (GetTickCount64() < deadline) {
          if (GetTickCount64() >= nextHealth) {
            std::ofstream health(statePath(L"native-health.json.tmp"),
                                 std::ios::trunc);
            health << "{\"running\":true,\"rocDuration\":" << rocGrant.duration
                   << ",\"transmitted\":" << awdlTxSubmitted
                   << ",\"ringWraps\":" << ringWraps
                   << ",\"dataAck\":" << dataUnicastAckCount
                   << ",\"peerSwitches\":" << peerSwitches
                   << ",\"otherPeerDropped\":" << hostOtherPeerDropped << "}";
            health.close();
            MoveFileExW(statePath(L"native-health.json.tmp").c_str(),
                        statePath(L"native-health.json").c_str(),
                        MOVEFILE_REPLACE_EXISTING);
            nextHealth = GetTickCount64() + 2000;
          }

          if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) break;
          if (awdlLink->stopping())
            throw std::runtime_error("The AWDL pipe ended early");
          waitEvent([](const uint8_t *, size_t) { return false; }, 1);

          if (!hostCandidate.empty() &&
              monotonicMicros() - hostCandidateAt > 2000000) {
            hostCandidate.clear();
            ++hostExpired;
          }
          if (hostCandidate.empty() && awdlLink->popData(hostCandidate))
            hostCandidateAt = monotonicMicros();
          if (!hostCandidate.empty() && !(hostCandidate[4] & 1)) {
            std::array<uint8_t, 6> target{};
            std::copy(hostCandidate.begin() + 4, hostCandidate.begin() + 10,
                      target.begin());
            const auto heard = peerReceived(target);
            if (target != awdlPeer && heard &&
                monotonicMicros() - heard <= 4000 &&
                std::none_of(
                    pendingActions.begin(), pendingActions.end(),
                    [](const PendingAction &item) { return item.active; })) {
              if (peerEnabled) {
                send(mtkPeerRequest(false, 9, awdlPeer));
                if (!commandReply(9, 3))
                  throw std::runtime_error(
                      "Previous peer record could not be retired");
                peerEnabled = false;
              }
              send(mtkPeerRequest(true, 8, target));
              if (!commandReply(8, 3))
                throw std::runtime_error(
                    "Requested host peer record was not acknowledged");
              peerEnabled = true;
              awdlPeer = target;
              awdlPeerLearned = true;
              peerLastRxUs = peerReceived(target);
              lastUnicastPeerRxUs = 0;
              unicastInWindow = 0;
              ++peerSwitches;
            }
          }

          for (unsigned slot = 0;
               slot < pendingActions.size() && awdlTxSubmitted < 100000000u;
               ++slot) {
            if (pendingActions[slot].active) continue;
            const auto nowUs = monotonicMicros();
            const bool peerWindow =
                peerEnabled &&
                (peerLastRxUs > lastUnicastPeerRxUs || unicastInWindow < 3) &&
                nowUs - peerLastRxUs <= 4000;

            std::vector<uint8_t> frame;
            bool hostData = false;

            if (peerWindow && !hostCandidate.empty() &&
                ((hostCandidate[4] & 1) ||
                 std::equal(awdlPeer.begin(), awdlPeer.end(),
                            hostCandidate.begin() + 4))) {
              frame = std::move(hostCandidate);
              hostCandidate.clear();
              hostData = true;
            }

            if (hostData && !(frame[4] & 1) &&
                !std::equal(awdlPeer.begin(), awdlPeer.end(),
                            frame.begin() + 4)) {
              ++hostOtherPeerDropped;
              continue;
            }
            const bool managementTrial =
                peerWindow && !hostData && peerLastRxUs > lastUnicastPeerRxUs &&
                nowUs - lastManagementTrialUs >= 200000 &&
                awdlUnicastSubmitted < 100000000u && !awdlUnicastAck;
            const bool unicast =
                (hostData && !(frame[4] & 1)) || managementTrial;

            if (!hostData && !awdlLink->pop(frame)) break;
            const auto sequence = uint16_t((awdlTxSubmitted + 1) & 4095);
            const auto packet =
                hostData ? mtkAwDlHostRequest(std::move(frame), sequence,
                                              uint8_t(128 + slot))
                         : mtkAwDlActionRequest(std::move(frame), sequence,
                                                uint8_t(128 + slot),
                                                unicast ? &awdlPeer : nullptr);
            pendingActions[slot] = {true, unicast, sequence, hostData};
            if (hostData) ++dataTxSubmitted;
            if (unicast) {
              if (!hostData) {
                awdlUnicastAttempted = true;
                ++awdlUnicastSubmitted;
                lastManagementTrialUs = nowUs;
              }
              if (peerLastRxUs != lastUnicastPeerRxUs) unicastInWindow = 0;
              lastUnicastPeerRxUs = peerLastRxUs;
              ++unicastInWindow;
              maxUnicastDequeueDelayUs =
                  (std::max)(maxUnicastDequeueDelayUs, nowUs - peerLastRxUs);
            }
            send(packet, 68 + slot);
            ++awdlTxSubmitted;
          }

          std::vector<uint8_t> record;
          if (!mcuFrames.empty()) {
            record = std::move(mcuFrames.front());
            mcuFrames.pop_front();
          } else {
            MemoryBarrier();
            const auto control = dataRing[dataTail].ctrl;
            if (!(control & 0x80000000u)) {
              Sleep(1);
              continue;
            }
            const unsigned bytes = (control >> 16) & 0x3fff;
            MemoryBarrier();
            const auto *data = bufferPointer(36 + dataTail);
            if (!bytes || bytes > 2048 || assembly.size() + bytes > 16384) {
              if (!dropping) ++oversizedDropped;
              dropping = true;
              assembly.clear();
            } else if (!dropping)
              assembly.insert(assembly.end(), data, data + bytes);
            const bool last = (control & 0x40000000u) != 0;
            if (!last) ++fragmentedDescriptors;
            dataRing[dataTail].info = 0;
            dataRing[dataTail].ctrl = 2048u << 16;
            MemoryBarrier();
            operation(handle, 0x2528, REGPROXY_WRITE, dataTail);
            dataTail = (dataTail + 1) % 32;
            if (!last) continue;
            if (dropping) {
              assembly.clear();
              dropping = false;
              continue;
            }
            record = std::move(assembly);
            assembly.clear();
          }
          ++dataPackets;

          const auto frame = mtkDot11Frame(record.data(), record.size());
          if (frame.empty())
            ++invalidFrames;
          else {
            ++rawFrames;
            if (frame.size() >= 29 &&
                std::equal(mtkAwDlBssid.begin(), mtkAwDlBssid.end(),
                           frame.begin() + 16) &&
                (((mtkLe16(frame.data()) & 0x0c) == 8) ||
                 (frame[0] == 0xd0 && frame[24] == 0x7f && frame[25] == 0 &&
                  frame[26] == 0x17 && frame[27] == 0xf2 && frame[28] == 8)))
              hearPeer(frame.data() + 10);
            // Addressed AWDL data proves the peer is awake on
            // our actual channel. Reply within that fresh window
            // rather than waiting for a later action frame.
            if (peerEnabled && frame.size() >= 24 &&
                (mtkLe16(frame.data()) & 0x0c) == 8 &&
                std::equal(radioAddress.begin(), radioAddress.end(),
                           frame.begin() + 4) &&
                std::equal(awdlPeer.begin(), awdlPeer.end(),
                           frame.begin() + 10) &&
                std::equal(mtkAwDlBssid.begin(), mtkAwDlBssid.end(),
                           frame.begin() + 16)) {
              peerLastRxUs = monotonicMicros();
              ++peerDataWindows;
            }

            if (radioTxSubmitted >= 2 &&
                (mtkLe16(frame.data()) & 0xfc) == 0xd4 && frame.size() == 10 &&
                std::equal(radioAddress.begin(), radioAddress.end(),
                           frame.begin() + 4))
              ++ackFrames;
            if (frame.size() >= 36 && (mtkLe16(frame.data()) & 0xfc) == 0x50 &&
                std::equal(radioAddress.begin(), radioAddress.end(),
                           frame.begin() + 4))
              ++probeResponses;
            if (frame.size() >= 29 && (mtkLe16(frame.data()) & 0xfc) == 0xd0 &&
                frame[24] == 0x7f && frame[25] == 0 && frame[26] == 0x17 &&
                frame[27] == 0xf2 && frame[28] == 8) {
              ++awdlFrames;
              if (!std::equal(radioAddress.begin(), radioAddress.end(),
                              frame.begin() + 10)) {
                if (!(frame[10] & 1) && awdlTxSubmitted && !awdlPeerLearned) {
                  std::copy(frame.begin() + 10, frame.begin() + 16,
                            awdlPeer.begin());
                  awdlPeerLearned = true;
                }
                if (awdlPeerLearned &&
                    std::equal(awdlPeer.begin(), awdlPeer.end(),
                               frame.begin() + 10)) {
                  if (!peerEnabled) {
                    send(mtkPeerRequest(true, 8, awdlPeer));
                    peerEnabled = commandReply(8, 3);
                    if (!peerEnabled)
                      throw std::runtime_error(
                          "AWDL peer record was not acknowledged");

                    // Wait for the next received peer action after setup.
                  } else
                    peerLastRxUs = monotonicMicros();
                }
                if (mtkAwDlPeerReferences(frame, radioAddress))
                  awdlPeerReferenced = true;
              }
            }
            if (!awdlLink->receive(frame))
              throw std::runtime_error("AWDL receive forwarding failed");
          }
        }

      } catch (const std::exception &failure) {
        error = failure.what();
      }
      if (awdlLink) {
        awdlLink->stop();
        hostInterfaceIndex = awdlLink->hostIndex();
        hostUnicastDropped = awdlLink->unicastDropped();
        hostMulticastDropped = awdlLink->multicastDropped();
        awdlLink.reset();

        if (!hostInterfaceIndex && error.empty())
          error = "The owned AWDL host interface was not verified";
      }
      // Firmware state is restored even if an enable reply was lost.

      try {
        send(mtkSnifferRequest(false, 4));
        snifferDisabled = commandReply(4, 0x24);
      } catch (const std::exception &failure) {
        if (error.empty()) error = failure.what();
      }
      if (rocAttempted) {
        try {
          send(mtkRocRequest(false, 14));
          rocAborted = commandReply(14, 0x27);
        } catch (const std::exception &failure) {
          if (error.empty()) error = failure.what();
        }
        if (!rocAborted && error.empty())
          error = "Channel privilege cleanup was not acknowledged";
      }
      if (peerEnabled) {
        try {
          send(mtkPeerRequest(false, 9, radioAddress));
          peerDisabled = commandReply(9, 3);
        } catch (const std::exception &failure) {
          if (error.empty()) error = failure.what();
        }
      }

      try {
        send(mtkBssRequest(false, 10, radioAddress));
        bssDisabled = commandReply(10, 2);
      } catch (const std::exception &failure) {
        if (error.empty()) error = failure.what();
      }
      try {
        send(mtkDeviceRequest(false, 11, radioAddress));
        deviceDisabled = commandReply(11, 1);
      } catch (const std::exception &failure) {
        if (error.empty()) error = failure.what();
      }

      try {
        send(mtkRadioRequest(false, 5));
        radioDisabled = commandReply(5, 8);
      } catch (const std::exception &failure) {
        if (error.empty()) error = failure.what();
      }
      if (clockAttempted) {
        try {
          send(mtkClockRequest(false, 12));
          clockDisabled = commandReply(12, 0x0f);
        } catch (const std::exception &failure) {
          if (error.empty()) error = failure.what();
        }
        if (!clockDisabled && error.empty())
          error = "Band-gap clock cleanup was not acknowledged";
      }
      if ((!snifferDisabled || !radioDisabled) && error.empty())
        error = "Firmware radio cleanup was not acknowledged";

      if ((!deviceDisabled || !bssDisabled || (peerEnabled && !peerDisabled)) &&
          error.empty())
        error = "AWDL MAC context cleanup was not acknowledged";
    }
  } catch (const std::exception &failure) {
    error = failure.what();
  }
  if (token) CloseHandle(token);
  if (handle != INVALID_HANDLE_VALUE) {
    stopped = programmed ? stopDma(handle) : true;
    CloseHandle(handle);
  }
  if (programmed && !stopped) {
    // Keep the pages allocated if hardware shutdown is uncertain. The
    // independent supervisor restores the OEM driver before allowing release.
    std::ofstream pending(reportPath, std::ios::trunc);
    pending << "{\"state\":\"waiting-for-oem-recovery\",\"pagesHeld\":true,"
               "\"ringAddress\":"
            << ringAddress << ",\"dmaValidated\":false}";
    pending.close();
    while (GetFileAttributesW(releasePath.c_str()) == INVALID_FILE_ATTRIBUTES)
      Sleep(500);
  }
  if (mapped && !MapUserPhysicalPages(view, count, nullptr)) freed = false;
  if (view && !VirtualFree(view, 0, MEM_RELEASE)) freed = false;
  if (count) {
    ULONG_PTR released = count;
    if (!FreeUserPhysicalPages(GetCurrentProcess(), &released, frames.data()) ||
        released != count)
      freed = false;
  }
  const bool transmitVerified =
      awdlPeerReferenced || awdlUnicastAck || dataTxAck;
  std::ofstream report(reportPath, std::ios::trunc);
  report << std::boolalpha
         << "{\"state\":\"completed\",\"hardwareProgrammed\":" << programmed
         << ",\"dmaValidated\":" << done << ",\"ringAddress\":" << ringAddress
         << ",\"setupBase\":" << setupBase << ",\"setupIndex\":" << setupIndex
         << ",\"dmaIndex\":" << dmaIndex
         << ",\"descriptorControl\":" << descriptorControl
         << ",\"dmaStopped\":" << stopped << ",\"allPagesReleased\":" << freed
         << ",\"allocationMiB\":" << allocationMiB
         << ",\"allocationFlags\":" << allocationFlags
         << ",\"allocationAttempts\":" << allocationAttempts
         << ",\"allocatedPages\":" << count
         << ",\"lowPagesAvailable\":" << lowPagesAvailable
         << ",\"lowPagesRequired\":" << lowPagesRequired
         << ",\"receivedEvents\":" << receivedEvents
         << ",\"rawFrames\":" << rawFrames << ",\"awdlFrames\":" << awdlFrames
         << ",\"invalidFrames\":" << invalidFrames
         << ",\"transmitted\":" << awdlTxSubmitted
         << ",\"firmwareCompleted\":" << awdlTxFirmwareCompleted
         << ",\"dataAck\":" << dataUnicastAckCount
         << ",\"dataFailed\":" << dataUnicastFailedCount
         << ",\"ringWraps\":" << ringWraps
         << ",\"peerSwitches\":" << peerSwitches
         << ",\"hostExpired\":" << hostExpired
         << ",\"hostUnicastDropped\":" << hostUnicastDropped
         << ",\"hostMulticastDropped\":" << hostMulticastDropped
         << ",\"maxUnicastDequeueDelayUs\":" << maxUnicastDequeueDelayUs
         << ",\"fragmentedDescriptors\":" << fragmentedDescriptors
         << ",\"oversizedDropped\":" << oversizedDropped
         << ",\"controlDataDrained\":" << controlDataDrained
         << ",\"mcuForwardedFrames\":" << mcuForwardedFrames
         << ",\"rawRadioTransmitVerified\":" << transmitVerified
         << ",\"error\":" << jsonString(error) << "}";
  report.close();

  std::ofstream health(statePath(L"native-health.json.tmp"), std::ios::trunc);
  health << std::boolalpha
         << "{\"running\":false,\"state\":\"ended\",\"dmaStopped\":" << stopped
         << ",\"allPagesReleased\":" << freed
         << ",\"transmitted\":" << awdlTxSubmitted << '}';
  health.close();
  MoveFileExW(statePath(L"native-health.json.tmp").c_str(),
              statePath(L"native-health.json").c_str(),
              MOVEFILE_REPLACE_EXISTING);

  return report.good() && done && stopped && freed && error.empty() ? 0 : 3;
}
