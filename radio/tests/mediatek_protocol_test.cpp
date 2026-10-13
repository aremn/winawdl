// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// clang-format off
#include <windows.h>
#include <winioctl.h>
// clang-format on
#include "../../driver/mediatek/register_access.h"
#include "../../mediatek/native/protocol.h"
#include "../mediatek/json_string.h"
#include <algorithm>
#include <iostream>
inline void mtkDecodeSelfTest() {
  std::vector<std::pair<unsigned, unsigned>> intervals;
  for (unsigned slot = 0; slot <= 70; ++slot) {
    const auto slice = mtkDmaSlice(slot);
    if (slice.page >= 37 || slice.offset + slice.capacity > 4096 ||
        (slice.offset & 15))
      throw std::runtime_error(
          "DMA slice crosses an owned page or is not aligned");
    intervals.emplace_back(slice.page * 4096 + slice.offset,
                           slice.page * 4096 + slice.offset + slice.capacity);
  }
  std::sort(intervals.begin(), intervals.end());
  for (size_t index = 1; index < intervals.size(); ++index)
    if (intervals[index].first < intervals[index - 1].second)
      throw std::runtime_error("Owned DMA buffers overlap");
  const auto echo = mtkAwDlEchoRequest(mtkProbeAddress, {{2, 3, 4, 5, 6, 7}},
                                       12, 129, 0x0123456789abcdefULL);
  if (echo.size() != 136 || mtkLe32(echo.data() + 8) != 0x20 ||
      (mtkLe32(echo.data() + 12) & 1) || echo[56] != 0xaa || echo[64] != 3 ||
      echo[65] != 4 || echo[114] != 0xfd || echo[115] != 0x2c ||
      mtkIcmp6Checksum(echo.data() + 72, 64) != 0)
    throw std::runtime_error("Bounded AWDL IPv6 data fixture rejected");
  auto hostFrame = std::vector<uint8_t>(echo.begin() + 32, echo.end());
  const auto host = mtkAwDlHostRequest(hostFrame, 13, 130);
  if (host.size() != echo.size() || mtkLe32(host.data() + 8) != 0x20 ||
      (mtkLe32(host.data() + 12) & 1))
    throw std::runtime_error("Unicast host frame descriptor rejected");
  hostFrame[4] = 0x33;
  if (!(mtkLe32(mtkAwDlHostRequest(hostFrame, 14, 128).data() + 12) & 1))
    throw std::runtime_error("Multicast host frame requires an ACK");
  hostFrame[45]++;
  bool rejected = false;
  try {
    mtkAwDlHostRequest(hostFrame, 14, 128);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  if (!rejected)
    throw std::runtime_error("Invalid IPv6 payload length accepted from host");
  const auto filter = mtkRxFilterRequest(true, 4);
  if (filter.size() != 60 || filter[34] != 8 || filter[48] != 0 ||
      filter[52] != 2 || filter[54] != 8 ||
      mtkLe32(filter.data() + 56) != 0x007f002f)
    throw std::runtime_error(
        "Single-band NDIS RX filter request layout differs");
  const auto macFilter = mtkMacRxFilterRequest(true, 5);
  if (macFilter.size() != 120 || macFilter[52] != 0x0c || macFilter[54] != 68 ||
      mtkLe32(macFilter.data() + 60) != 0x80000060u)
    throw std::runtime_error(
        "MT7925 MAC80211 RX filter request layout differs");
  const auto clock = mtkClockRequest(true, 15);
  if (clock.size() != 60 || clock[34] != 0x0f || clock[52] != 4 ||
      clock[54] != 8 || clock[56] != 1)
    throw std::runtime_error("OEM band-gap clock request layout differs");
  const auto request = mtkNicCapabilityRequest(1);
  // The OEM builder sets TXD0 bits 30/24, TXD1 bit 14, UNI header at 0x20.
  if (request[0] != 56 || request[3] != 0x41 || request[4] != 0 ||
      request[5] != 0x40 || request[6] != 0 || request[32] != 24 ||
      request[34] != 14 || request[37] != 0xa0 || request[39] != 1 ||
      request[43] != 6 || request[52] != 3 || request[54] != 4)
    throw std::runtime_error("OEM MCU request layout differs");
  // Event header + two complete TLVs, independent of live DMA/register code.
  std::vector<uint8_t> fixture(62);
  fixture[36] = 0x43;
  fixture[37] = 1;
  fixture[44] = 2;
  fixture[48] = 4;
  fixture[50] = 6;
  fixture[54] = 9;
  fixture[56] = 8;
  if (!mtkParseNicCapability(fixture.data(), fixture.size(), 1).valid)
    throw std::runtime_error("NIC event fixture rejected");
  if (mtkParseNicCapability(fixture.data(), fixture.size(), 2).matched)
    throw std::runtime_error("Wrong event sequence accepted");
  if (mtkParseNicCapability(fixture.data(), 61, 1).valid)
    throw std::runtime_error("Truncated TLV accepted");
  fixture[50] = 3;
  if (mtkParseNicCapability(fixture.data(), fixture.size(), 1).valid)
    throw std::runtime_error("Short TLV accepted");
  std::vector<uint8_t> frame(56);
  frame[0] = 56;
  frame[3] = 0x10;
  frame[32] = 0x80;
  if (mtkDot11Frame(frame.data(), frame.size()).size() != 24)
    throw std::runtime_error("802.11 RX fixture rejected");
  frame[8] = 0x80;
  if (!mtkDot11Frame(frame.data(), frame.size()).empty())
    throw std::runtime_error("Ethernet-translated RX accepted as 802.11");
  frame[8] = 0;
  frame[6] = 0x08;
  if (!mtkDot11Frame(frame.data(), frame.size()).empty())
    throw std::runtime_error("Truncated RX group accepted");
  frame[6] = 0;
  frame[2] = 1;
  frame[3] = 0x38;
  if (mtkDot11Frame(frame.data(), frame.size()).size() != 24)
    throw std::runtime_error("Firmware-forwarded 802.11 RX fixture rejected");
  frame[2] = 0;
  if (!mtkDot11Frame(frame.data(), frame.size()).empty())
    throw std::runtime_error("Ordinary MCU event accepted as raw 802.11");
  // A command acknowledgement cannot be confused with a hardware TXS packet.
  std::vector<uint8_t> ack(52);
  ack[3] = 0x38;
  ack[36] = 1;
  ack[37] = 6;
  ack[44] = 1;
  if (!mtkCommandSucceeded(ack.data(), ack.size(), 6, 1))
    throw std::runtime_error("Command reply fixture rejected");
  mtkPut32(ack.data() + 48, 0xc0000001);
  if (!mtkCommandResult(ack.data(), ack.size(), 6, 1).matched ||
      mtkCommandSucceeded(ack.data(), ack.size(), 6, 1) ||
      mtkCommandResult(ack.data(), ack.size(), 7, 1).matched)
    throw std::runtime_error(
        "Rejected or unrelated MCU command reply accepted");
  mtkPut32(ack.data() + 48, 0);
  ack[3] = 0;
  if (mtkCommandSucceeded(ack.data(), ack.size(), 6, 1))
    throw std::runtime_error(
        "Wrong RX packet type accepted as MCU acknowledgement");
  std::vector<uint8_t> txDone(80);
  txDone[3] = 0x38;
  txDone[36] = 0x2d;
  txDone[50] = 32;
  txDone[52] = 129;
  txDone[54] = 2;
  if (!mtkFwTxStatus(txDone.data(), txDone.size(), 129, 2).success)
    throw std::runtime_error("Successful TX-done fixture rejected");
  txDone[53] = 3;
  if (mtkFwTxStatus(txDone.data(), txDone.size(), 129, 2).success)
    throw std::runtime_error("MPDU failure accepted as an ACK");
  if (mtkFwTxStatus(txDone.data(), 79, 129, 2).matched ||
      mtkFwTxStatus(txDone.data(), 80, 129, 3).matched)
    throw std::runtime_error("Truncated or mismatched TX-done event accepted");
  const auto roc = mtkRocRequest(true, 13), abort = mtkRocRequest(false, 14);
  if (roc.size() != 76 || roc[34] != 0x27 || roc[54] != 24 || roc[57] != 0x4d ||
      roc[58] != 6 || roc[60] != 1 || roc[62] != 6 || roc[67] != 1 ||
      mtkLe32(roc.data() + 68) != 10000 || roc[72] != 0xff ||
      abort.size() != 64 || abort[52] != 1 || abort[54] != 12 ||
      abort[57] != 0x4d || abort[58] != 0xff)
    throw std::runtime_error("OEM channel privilege request layout differs");
  std::vector<uint8_t> grant(68);
  grant[3] = 0x38;
  grant[36] = 0x27;
  grant[50] = 20;
  grant[53] = 0x4d;
  grant[55] = 6;
  grant[57] = 1;
  grant[59] = 6;
  grant[61] = 1;
  mtkPut32(grant.data() + 64, 10000);
  if (!mtkRocGrant(grant.data(), grant.size()).valid)
    throw std::runtime_error("Channel grant fixture rejected");
  const auto day = mtkRocRequest(true, 13, 86400000);
  if (mtkLe32(day.data() + 68) != 86400000)
    throw std::runtime_error("Service tenure serialization differs");
  const auto extended = mtkRocRequest(true, 13, 190000);
  if (mtkLe32(extended.data() + 68) != 190000)
    throw std::runtime_error("Extended channel tenure serializer differs");
  mtkPut32(grant.data() + 64, 190000);
  if (mtkRocGrant(grant.data(), grant.size()).valid ||
      !mtkRocGrant(grant.data(), grant.size(), 190000).valid)
    throw std::runtime_error("Channel tenure bounds are not enforced");
  mtkPut32(grant.data() + 64, 190001);
  if (mtkRocGrant(grant.data(), grant.size(), 190000).valid)
    throw std::runtime_error("Unbounded channel grant accepted");
  mtkPut32(grant.data() + 64, 10000);
  if (mtkRocGrant(grant.data(), 67).matched)
    throw std::runtime_error("Truncated channel grant accepted");
  grant[54] = 1;
  if (mtkRocGrant(grant.data(), grant.size()).valid)
    throw std::runtime_error("Denied channel grant accepted");
  grant[54] = 0;
  grant[62] = 1;
  if (mtkRocGrant(grant.data(), grant.size()).valid)
    throw std::runtime_error("Unconfigured radio grant accepted");
  grant[62] = 0;
  grant[53] = 0x4e;
  if (mtkRocGrant(grant.data(), grant.size()).matched)
    throw std::runtime_error("Wrong channel token accepted");
  std::vector<uint8_t> action(55);
  action[0] = 0xd0;
  const std::array<uint8_t, 6> bssid{{0, 0x25, 0, 0xff, 0x94, 0x73}};
  std::copy(bssid.begin(), bssid.end(), action.begin() + 16);
  action[24] = 0x7f;
  action[26] = 0x17;
  action[27] = 0xf2;
  action[28] = 8;
  action[40] = 24;
  action[41] = 12;
  std::copy(mtkProbeAddress.begin(), mtkProbeAddress.end(),
            action.begin() + 43);
  if (!mtkAwDlPeerReferences(action, mtkProbeAddress))
    throw std::runtime_error("AWDL election reference fixture rejected");
  const auto rawAction = mtkAwDlActionRequest(action, 4, 128, &mtkProbeAddress);
  if (mtkLe32(rawAction.data() + 8) != 13 ||
      (mtkLe32(rawAction.data() + 12) & 1) ||
      mtkLe16(rawAction.data() + 54) != 64 || rawAction.size() != 87)
    throw std::runtime_error("Unicast AWDL action descriptor rejected");
  action.resize(54);
  if (mtkAwDlPeerReferences(action, mtkProbeAddress))
    throw std::runtime_error("Truncated AWDL reference accepted");
  action.resize(55);
  action[40] = 11;
  if (mtkAwDlPeerReferences(action, mtkProbeAddress))
    throw std::runtime_error(
        "Unrelated AWDL field accepted as a peer response");
  action[40] = 24;
  std::copy(mtkProbeAddress.begin(), mtkProbeAddress.end(),
            action.begin() + 10);
  if (mtkAwDlPeerReferences(action, mtkProbeAddress))
    throw std::runtime_error("Own AWDL loopback accepted as RF proof");
}

int main() {
  try {
    mtkDecodeSelfTest();
    auto allowed = [](unsigned offset, unsigned value, unsigned char kind) {
      REGPROXY_OP op{offset, value, 4, kind, 0};
      return mtkRegisterOperationIsValid(&op) != FALSE;
    };
    if (!allowed(0x23f0, 0x1000, REGPROXY_WRITE) ||
        allowed(0x23f0, 0x1001, REGPROXY_WRITE) ||
        allowed(0x2400, 0x1000, REGPROXY_WRITE))
      throw std::runtime_error("DMA base alignment or queue isolation failed");
    if (!allowed(0x2208, 5, REGPROXY_CLEAR_BITS) ||
        allowed(0x2208, 7, REGPROXY_SET_BITS) ||
        allowed(0x2208, 0, REGPROXY_WRITE))
      throw std::runtime_error("DMA control bit restriction failed");
    if (!allowed(0x23f4, 256, REGPROXY_WRITE) ||
        allowed(0x23f4, 255, REGPROXY_WRITE) ||
        allowed(0x2508, 32, REGPROXY_WRITE) ||
        allowed(0x2209, 0, REGPROXY_READ))
      throw std::runtime_error("DMA descriptor bounds check failed");
    if (jsonString("quoted \"text\" and \\path") !=
        "\"quoted \\\"text\\\" and \\\\path\"")
      throw std::runtime_error("JSON quote/backslash escaping failed");
    if (jsonString(std::string(1, char(31))) != "\"\\u001f\"")
      throw std::runtime_error("JSON control-byte escaping failed");
    std::cout << "MediaTek protocol checks passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
