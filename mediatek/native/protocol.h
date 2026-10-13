// SPDX-License-Identifier: MIT
// MT7925 MCU commands, firmware events and packed DMA layout.
// mt76 reference layouts: Copyright (C) 2023 MediaTek Inc., BSD-3-Clause-Clear.
// See LICENSES/BSD-3-Clause-Clear.txt for the reference license.
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

struct MtkNicCapability {
  bool matched = false;
  bool valid = false;
  uint8_t event = 0;
  uint8_t sequence = 0;
  uint16_t packetId = 0;
  uint16_t elements = 0;
  std::vector<uint16_t> tags;
  std::array<uint8_t, 6> address{};
  bool addressValid = false;
};

inline uint16_t mtkLe16(const uint8_t *data) {
  return static_cast<uint16_t>(data[0] | (unsigned(data[1]) << 8));
}
inline uint32_t mtkLe32(const uint8_t *data) {
  return uint32_t(mtkLe16(data)) | (uint32_t(mtkLe16(data + 2)) << 16);
}
inline void mtkPut32(uint8_t *data, uint32_t value) {
  for (unsigned byte = 0; byte < 4; ++byte)
    data[byte] = uint8_t(value >> (8 * byte));
}

inline const std::array<uint8_t, 6> mtkProbeAddress{
    {0x02, 0x77, 0x69, 0x6e, 0xad, 0x01}};
inline const std::array<uint8_t, 6> mtkAwDlBssid{
    {0, 0x25, 0, 0xff, 0x94, 0x73}};

struct MtkDmaSlice {
  unsigned page;
  unsigned offset;
  unsigned capacity;
};
inline MtkDmaSlice mtkDmaSlice(unsigned logical) {
  if (logical == 0 || logical == 2) return {logical, 0, 4096};
  if (logical == 1) return {1, 0, 2048};
  if (logical >= 3 && logical <= 34)
    return {3 + (logical - 3) / 2, ((logical - 3) % 2) * 2048, 2048};
  if (logical == 35) return {19, 0, 4096};
  if (logical >= 36 && logical <= 67)
    return {20 + (logical - 36) / 2, ((logical - 36) % 2) * 2048, 2048};
  if (logical == 68 || logical == 69) return {36, (logical - 68) * 2048, 2048};
  if (logical == 70) return {1, 2048, 2048};
  throw std::out_of_range("Unknown owned DMA slice");
}

inline std::vector<uint8_t> mtkProbeRequest(
    uint16_t sequence, const std::array<uint8_t, 6> *destination = nullptr,
    const std::array<uint8_t, 6> *source = nullptr, bool alternate = false) {
  // OEM MT7925 MLME path: complete 32-byte TXD + MPDU on hardware ring 15.
  // Software queue 2 maps to context[d1]=15; context[d0]=16 is FW download.
  // Local test address, broadcast wildcard Probe Request, no association.
  std::vector<uint8_t> packet(32 + 35);
  auto *frame = packet.data() + 32;
  frame[0] = 0x40;
  std::fill(frame + 4, frame + 10, uint8_t(0xff));
  const auto &address = source ? *source : mtkProbeAddress;
  std::copy(address.begin(), address.end(), frame + 10);
  std::fill(frame + 16, frame + 22, uint8_t(0xff));
  if (destination) {
    std::copy(destination->begin(), destination->end(), frame + 4);
    std::copy(destination->begin(), destination->end(), frame + 16);
  }
  frame[22] = uint8_t(sequence << 4);
  frame[23] = uint8_t(sequence >> 4);
  const std::array<uint8_t, 11> elements{
      {0, 0, 1, 4, 0x8c, 0x12, 0x98, 0x24, 3, 1, 6}};
  std::copy(elements.begin(), elements.end(), frame + 24);
  mtkPut32(packet.data(), (alternate ? 0x21000000u : 0x03000000u) |
                              static_cast<uint32_t>(packet.size()));
  mtkPut32(packet.data() + 4,
           destination
               ? 0x800c8001u
               : 0x800c8000u);    // Fixed rate, own WTBL entry 1 for unicast.
  mtkPut32(packet.data() + 8, 4); // Management, Probe Request subtype.
  mtkPut32(packet.data() + 12, 0x90000000u | (destination ? 0x1800u : 0xf011u) |
                                   (uint32_t(sequence & 4095) << 16));
  // OEM probe subtype with a nonzero PID requests HOST and MCU completion.
  mtkPut32(packet.data() + 20, alternate     ? 0x682u
                               : destination ? 0x681u
                                             : 0x680u);
  mtkPut32(packet.data() + 24,
           0x0201001c); // OEM basic rate index 1, 20 MHz, 1 MSDU.
  return packet;
}

inline std::vector<uint8_t>
mtkAwDlActionRequest(std::vector<uint8_t> frame, uint16_t sequence,
                     uint8_t packetId,
                     const std::array<uint8_t, 6> *destination = nullptr) {
  if (frame.size() < 40 || frame.size() > 1500 || frame[0] != 0xd0 ||
      frame[1] || frame[24] != 0x7f || frame[25] || frame[26] != 0x17 ||
      frame[27] != 0xf2 || frame[28] != 8 || packetId < 128 || packetId > 130 ||
      sequence > 4095)
    throw std::invalid_argument("Invalid bounded AWDL action frame");
  if (destination)
    std::copy(destination->begin(), destination->end(), frame.begin() + 4);
  frame[22] = uint8_t(sequence << 4);
  frame[23] = uint8_t(sequence >> 4);
  std::vector<uint8_t> packet(32 + frame.size());
  std::copy(frame.begin(), frame.end(), packet.begin() + 32);
  mtkPut32(packet.data(), 0x03000000u | static_cast<uint32_t>(packet.size()));
  mtkPut32(packet.data() + 4, destination ? 0x800c8001u : 0x800c8000u);
  mtkPut32(packet.data() + 8, 13); // Management action subtype.
  mtkPut32(packet.data() + 12, 0x90000000u | (destination ? 0x1800u : 0xf011u) |
                                   (uint32_t(sequence) << 16));
  // The bounded bridge consumes MCU TX_DONE, not a second host TXS stream.
  mtkPut32(packet.data() + 20, 0x200u | packetId);
  mtkPut32(packet.data() + 24, 0x0201001c);
  return packet;
}

inline bool mtkAwDlPeerReferences(const std::vector<uint8_t> &frame,
                                  const std::array<uint8_t, 6> &own) {
  static constexpr std::array<uint8_t, 6> bssid{{0, 0x25, 0, 0xff, 0x94, 0x73}};
  if (frame.size() < 40 || (mtkLe16(frame.data()) & 0xfc) != 0xd0 ||
      frame[24] != 0x7f || frame[25] || frame[26] != 0x17 ||
      frame[27] != 0xf2 || frame[28] != 8 ||
      !std::equal(bssid.begin(), bssid.end(), frame.begin() + 16) ||
      std::equal(own.begin(), own.end(), frame.begin() + 10))
    return false;
  size_t offset = 40;
  while (offset + 3 <= frame.size()) {
    const unsigned tag = frame[offset],
                   length = mtkLe16(frame.data() + offset + 1);
    if (length > frame.size() - offset - 3) return false;
    if (tag == 24 && length >= 12 &&
        (std::equal(own.begin(), own.end(), frame.begin() + offset + 3) ||
         std::equal(own.begin(), own.end(), frame.begin() + offset + 9)))
      return true;
    if (tag == 20 && length && length % 6 == 0)
      for (unsigned member = 0; member < length; member += 6)
        if (std::equal(own.begin(), own.end(),
                       frame.begin() + offset + 3 + member))
          return true;
    offset += 3 + length;
  }
  return false;
}

inline std::array<uint8_t, 16>
mtkLinkLocalAddress(const std::array<uint8_t, 6> &mac) {
  std::array<uint8_t, 16> address{};
  address[0] = 0xfe;
  address[1] = 0x80;
  address[8] = mac[0] ^ 2;
  address[9] = mac[1];
  address[10] = mac[2];
  address[11] = 0xff;
  address[12] = 0xfe;
  std::copy(mac.begin() + 3, mac.end(), address.begin() + 13);
  return address;
}

inline std::vector<uint8_t> mtkAwDlHostRequest(std::vector<uint8_t> frame,
                                               uint16_t sequence,
                                               uint8_t packetId) {
  const std::array<uint8_t, 8> llc{{0xaa, 0xaa, 3, 0, 0x17, 0xf2, 8, 0}};
  if (frame.size() < 80 || frame.size() > 1536 || frame[0] != 8 || frame[1] ||
      !std::equal(mtkAwDlBssid.begin(), mtkAwDlBssid.end(),
                  frame.begin() + 16) ||
      !std::equal(llc.begin(), llc.end(), frame.begin() + 24) ||
      frame[32] != 3 || frame[33] != 4 || frame[38] != 0x86 ||
      frame[39] != 0xdd || (frame[40] >> 4) != 6 ||
      size_t(frame[44]) * 256 + frame[45] + 80 != frame.size() ||
      sequence > 4095 || packetId < 128 || packetId > 130)
    throw std::invalid_argument("Invalid bounded AWDL host data frame");
  const bool multicast = (frame[4] & 1) != 0;
  frame[22] = uint8_t(sequence << 4);
  frame[23] = uint8_t(sequence >> 4);
  std::vector<uint8_t> packet(32 + frame.size());
  std::copy(frame.begin(), frame.end(), packet.begin() + 32);
  mtkPut32(packet.data(), 0x03000000u | static_cast<uint32_t>(packet.size()));
  mtkPut32(packet.data() + 4, multicast ? 0x800c8000u : 0x800c8001u);
  mtkPut32(packet.data() + 8, 0x20);
  mtkPut32(packet.data() + 12, 0x90000000u | (multicast ? 0xf011u : 0x1800u) |
                                   (uint32_t(sequence) << 16));
  mtkPut32(packet.data() + 20, 0x200u | packetId);
  mtkPut32(packet.data() + 24, 0x0201001c);
  return packet;
}

inline uint16_t mtkIcmp6Checksum(const uint8_t *ipv6, size_t length) {
  if (length < 48 || ipv6[6] != 58 ||
      (size_t(ipv6[4]) << 8 | ipv6[5]) != length - 40)
    throw std::invalid_argument("Invalid bounded ICMPv6 packet");
  uint32_t sum = 58 + static_cast<uint32_t>(length - 40);
  auto add = [&](const uint8_t *data, size_t bytes) {
    for (size_t offset = 0; offset < bytes; offset += 2)
      sum += (uint32_t(data[offset]) << 8) |
             (offset + 1 < bytes ? data[offset + 1] : 0);
  };
  add(ipv6 + 8, 32);
  add(ipv6 + 40, length - 40);
  while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
  return static_cast<uint16_t>(~sum);
}

inline std::vector<uint8_t>
mtkAwDlEchoRequest(const std::array<uint8_t, 6> &own,
                   const std::array<uint8_t, 6> &peer, uint16_t sequence,
                   uint8_t packetId, uint64_t nonce) {
  if ((own[0] & 1) || (peer[0] & 1) || sequence > 4095 || packetId < 128 ||
      packetId > 130)
    throw std::invalid_argument("Invalid bounded AWDL data request");
  // Protocol bytes, independently serialized: native 802.11 Data, Apple
  // SNAP OUI/0800, eight-byte AWDL data header, IPv6 ICMP echo + 16-byte body.
  std::vector<uint8_t> packet(32 + 24 + 8 + 8 + 40 + 24);
  auto *frame = packet.data() + 32;
  frame[0] = 8;
  std::copy(peer.begin(), peer.end(), frame + 4);
  std::copy(own.begin(), own.end(), frame + 10);
  std::copy(mtkAwDlBssid.begin(), mtkAwDlBssid.end(), frame + 16);
  frame[22] = uint8_t(sequence << 4);
  frame[23] = uint8_t(sequence >> 4);
  const std::array<uint8_t, 8> llc{{0xaa, 0xaa, 3, 0, 0x17, 0xf2, 8, 0}};
  std::copy(llc.begin(), llc.end(), frame + 24);
  frame[32] = 3;
  frame[33] = 4;
  frame[34] = uint8_t(sequence);
  frame[35] = uint8_t(sequence >> 8);
  frame[38] = 0x86;
  frame[39] = 0xdd;
  auto *ip = frame + 40;
  ip[0] = 0x60;
  ip[5] = 24;
  ip[6] = 58;
  ip[7] = 64;
  const auto source = mtkLinkLocalAddress(own),
             destination = mtkLinkLocalAddress(peer);
  std::copy(source.begin(), source.end(), ip + 8);
  std::copy(destination.begin(), destination.end(), ip + 24);
  ip[40] = 128;
  ip[44] = 0x77;
  ip[45] = 0xad;
  ip[46] = uint8_t(sequence >> 8);
  ip[47] = uint8_t(sequence);
  for (unsigned byte = 0; byte < 8; ++byte)
    ip[48 + byte] = uint8_t(nonce >> (8 * byte));
  const std::array<uint8_t, 8> marker{{'w', 'i', 'n', 'a', 'w', 'd', 'l', 1}};
  std::copy(marker.begin(), marker.end(), ip + 56);
  const auto checksum = mtkIcmp6Checksum(ip, 64);
  ip[42] = uint8_t(checksum >> 8);
  ip[43] = uint8_t(checksum);
  mtkPut32(packet.data(), 0x03000000u | static_cast<uint32_t>(packet.size()));
  mtkPut32(packet.data() + 4, 0x800c8001u);
  mtkPut32(packet.data() + 8, 0x20); // Data type 2, non-QoS subtype 0.
  mtkPut32(packet.data() + 12, 0x90001800u | (uint32_t(sequence) << 16));
  mtkPut32(packet.data() + 20, 0x200u | packetId);
  mtkPut32(packet.data() + 24, 0x0201001c);
  return packet;
}

inline std::vector<uint8_t> mtkUniRequest(uint16_t cid, uint8_t sequence,
                                          uint8_t option,
                                          const std::vector<uint8_t> &payload) {
  if (!sequence || sequence > 15 || payload.size() > 2000)
    throw std::invalid_argument("Invalid MCU request size or sequence");
  std::vector<uint8_t> packet(48 + payload.size());
  const auto bytes = packet.size();
  packet[0] = uint8_t(bytes);
  packet[1] = uint8_t(bytes >> 8);
  packet[3] = 0x41;
  packet[5] = 0x40;
  packet[32] = uint8_t(bytes - 32);
  packet[33] = uint8_t((bytes - 32) >> 8);
  packet[34] = uint8_t(cid);
  packet[35] = uint8_t(cid >> 8);
  packet[37] = 0xa0;
  packet[39] = sequence;
  packet[43] = option;
  std::copy(payload.begin(), payload.end(), packet.begin() + 48);
  return packet;
}

inline std::vector<uint8_t> mtkRadioRequest(bool enable, uint8_t sequence) {
  std::vector<uint8_t> payload(12);
  payload[6] = 8;
  payload[8] = enable ? 1 : 0;
  return mtkUniRequest(8, sequence, 7, payload);
}

inline std::vector<uint8_t> mtkClockRequest(bool enable, uint8_t sequence) {
  // OEM nicUniCmdBandGapClockReq, RVA 0x1358a8: POWER_CTRL, tag 4, len 8.
  std::vector<uint8_t> payload(12);
  payload[4] = 4;
  payload[6] = 8;
  payload[8] = enable ? 1 : 0;
  return mtkUniRequest(0x0f, sequence, 7, payload);
}

inline std::vector<uint8_t> mtkRxFilterRequest(bool enable, uint8_t sequence) {
  // OEM nicUniCmdSetRxFilter, RVA 0x12ae40: BAND_CONFIG tag 2, len 8.
  // The 2024 firmware ABI differs from current mt76 tag 0x0c.
  // The OEM's all-band selector 0xff was rejected by this firmware with
  // C0000001 in v19. Target the one configured radio explicitly instead.
  std::vector<uint8_t> payload(12);
  payload[4] = 2;
  payload[6] = 8;
  // MtCmdSetCurrentPacketFilter (RVA 0xc1db8) always ORs BROADCAST (8)
  // and MULTICAST (2) before nicUniCmdSetRxFilter. Include ALL_MULTICAST
  // for AWDL in addition to PROMISCUOUS; 0x20 alone was ACKed but RX silent.
  // windot11.h defines separate RAW_DATA and MGMT flags at bits 16..22.
  // The low NDIS flags admit data only; AWDL vendor actions need MGMT too.
  mtkPut32(payload.data() + 8, enable ? 0x007f002fu : 0x0au);
  return mtkUniRequest(8, sequence, 7, payload);
}

inline std::vector<uint8_t> mtkMacRxFilterRequest(bool enable,
                                                  uint8_t sequence) {
  // Independent mt7925 MCU MAC80211 filter ABI: 4-byte band header and
  // 68-byte tag 0x0c. Hardware support is exploratory, not assumed from ACK.
  std::vector<uint8_t> payload(72);
  payload[4] = 0x0c;
  payload[6] = 68;
  mtkPut32(payload.data() + 12, enable ? 0x80000060u : 0x80000000u);
  return mtkUniRequest(8, sequence, 7, payload);
}

inline std::vector<uint8_t> mtkSnifferRequest(bool enable, uint8_t sequence) {
  // OEM nicUniCmdSetMonitor: band header + ENABLE(8) + CONFIG(16).
  std::vector<uint8_t> payload(28);
  payload[6] = 8;
  payload[8] = enable ? 1 : 0;
  payload[12] = 1;
  payload[14] = 16;
  payload[18] = 1;
  payload[20] = 6;
  payload[22] = 6;
  payload[24] = 1;
  return mtkUniRequest(0x24, sequence, 7, payload);
}

inline std::vector<uint8_t> mtkRocRequest(bool acquire, uint8_t sequence,
                                          uint32_t duration = 10000) {
  if (!duration || duration > 86400000)
    throw std::invalid_argument("Unbounded channel tenure request");
  // OEM nicUniCmdChReqPrivilege (0x135f74) and ChAbortPrivilege (0x135e40):
  // four reserved bytes, then ACQUIRE(24) or ABORT(12). BSS 0, token 0x4d.
  std::vector<uint8_t> payload(acquire ? 28u : 16u);
  payload[4] = acquire ? 0 : 1;
  payload[6] = acquire ? 24 : 12;
  payload[9] = 0x4d;
  if (acquire) {
    payload[10] = 6;
    payload[12] = 1;
    payload[14] = 6;
    payload[17] = 6;
    payload[19] = 1; // Remain-on-channel, 20 MHz.
    mtkPut32(payload.data() + 20,
             duration); // Finite privilege, always aborted during cleanup.
    payload[24] = 0xff;
  } else
    payload[10] = 0xff;
  return mtkUniRequest(0x27, sequence, 7, payload);
}

struct MtkRocGrant {
  bool matched = false;
  bool valid = false;
  uint8_t status = 0xff;
  uint8_t radio = 0xff;
  uint32_t duration = 0;
};
inline MtkRocGrant mtkRocGrant(const uint8_t *data, size_t length,
                               uint32_t maximumDuration = 10000) {
  if (!maximumDuration || maximumDuration > 86400000) return {};
  if (length < 68 || mtkLe32(data) >> 27 != 7 || data[36] != 0x27) return {};
  size_t offset = 48;
  while (offset + 4 <= length) {
    const unsigned tag = mtkLe16(data + offset),
                   bytes = mtkLe16(data + offset + 2);
    if (bytes < 4 || bytes > length - offset) return {};
    if (tag == 0 && bytes >= 20 && data[offset + 4] == 0 &&
        data[offset + 5] == 0x4d) {
      const auto status = data[offset + 6], radio = data[offset + 14];
      const auto duration = mtkLe32(data + offset + 16);
      // This bounded test configures only radio 0 and the 2.4 GHz channel 6.
      const bool valid = status == 0 && radio == 0 && data[offset + 7] == 6 &&
                         data[offset + 8] == 0 && data[offset + 9] == 1 &&
                         data[offset + 10] == 0 && data[offset + 11] == 6 &&
                         data[offset + 12] == 0 && data[offset + 13] == 1 &&
                         duration > 0 && duration <= maximumDuration;
      return {true, valid, status, radio, duration};
    }
    offset += bytes;
  }
  return {};
}

inline std::vector<uint8_t>
mtkDeviceRequest(bool enable, uint8_t sequence,
                 const std::array<uint8_t, 6> &address) {
  std::vector<uint8_t> payload(16);
  payload[6] = 12;
  payload[8] = enable ? 1 : 0;
  std::copy(address.begin(), address.end(), payload.begin() + 10);
  return mtkUniRequest(1, sequence, 7, payload);
}

inline std::vector<uint8_t>
mtkBssRequest(bool enable, uint8_t sequence,
              const std::array<uint8_t, 6> &address) {
  // BSS 0, OMAC 0, 2.4 GHz; monitor interface is an INFRA_AP context.
  std::vector<uint8_t> payload(enable ? 52u : 36u);
  payload[6] = 32;
  payload[8] = enable ? 1 : 0;
  mtkPut32(payload.data() + 12, 0x00010002);
  payload[16] = enable ? 0 : 1;
  std::copy(address.begin(), address.end(), payload.begin() + 18);
  payload[26] = 100;
  payload[28] = 1;
  payload[29] = 6;
  payload[32] = 1;
  if (enable) {
    // BSS RLM configures the transmit channel; sniffer config alone is RX.
    payload[36] = 2;
    payload[38] = 16;
    payload[40] = 6;
    payload[41] = 6;
    payload[44] = 2;
    payload[45] = 2;
    payload[46] = 4;
    payload[48] = 1;
  }
  return mtkUniRequest(2, sequence, 7, payload);
}

inline std::vector<uint8_t>
mtkPeerRequest(bool enable, uint8_t sequence,
               const std::array<uint8_t, 6> &address) {
  std::vector<uint8_t> payload(enable ? 36u : 16u);
  payload[1] = 1;
  payload[2] = enable ? 2 : 1;
  payload[4] = 1; // BSS 0, WTBL 1, append.
  if (enable) {
    payload[10] = 20;
    mtkPut32(payload.data() + 12, 0x00010001);
    payload[16] = 2; // Port secure, plaintext legacy test frame.
    std::copy(address.begin(), address.end(), payload.begin() + 20);
    payload[26] = 3; // Version + new record.
    payload[28] = 0x2b;
    payload[30] = 8;
    payload[34] = 1; // Raw 802.11 RX, no header translation.
  } else {
    payload[8] = 0x25;
    payload[10] = 8;
  }
  return mtkUniRequest(3, sequence, 7, payload);
}

struct MtkCommandResult {
  bool matched = false;
  uint32_t status = 0xffffffffu;
};
inline MtkCommandResult mtkCommandResult(const uint8_t *data, size_t length,
                                         uint8_t sequence, uint8_t cid) {
  if (length < 52 || (mtkLe32(data) >> 27) != 7 || data[36] != 1 ||
      data[37] != sequence || mtkLe32(data + 44) != cid)
    return {};
  return {true, mtkLe32(data + 48)};
}
inline bool mtkCommandSucceeded(const uint8_t *data, size_t length,
                                uint8_t sequence, uint8_t cid) {
  const auto result = mtkCommandResult(data, length, sequence, cid);
  return result.matched && result.status == 0;
}

struct MtkTxStatus {
  bool matched = false;
  bool success = false;
  uint32_t status = 0;
  uint16_t rate = 0;
  uint8_t attempts = 0;
  uint32_t delayTicks = 0;
};
inline MtkTxStatus mtkTxStatus(const uint8_t *data, size_t length,
                               uint8_t packetId) {
  // MT7925 PCI RX-check format: four header DWORDs + twelve TXS DWORDs.
  if (length != 64 || mtkLe32(data) >> 27 != 0 ||
      (mtkLe32(data + 28) >> 24) != packetId)
    return {};
  const auto status = mtkLe32(data + 16);
  if (!(status & (1u << 15))) return {}; // Host TX report.
  return {true, !(status & 0x007f0000u), status};
}

inline MtkTxStatus mtkFwTxStatus(const uint8_t *data, size_t length,
                                 uint8_t packetId, uint16_t sequence) {
  // OEM UNI_EVENT_TX_DONE MSG TLV: 32 bytes, PID/status/SN at payload 4/5/6.
  if (length < 80 || mtkLe32(data) >> 27 != 7 || data[36] != 0x2d) return {};
  size_t offset = 48;
  while (offset + 4 <= length) {
    const unsigned tag = mtkLe16(data + offset),
                   bytes = mtkLe16(data + offset + 2);
    if (bytes < 4 || bytes > length - offset) return {};
    if (tag == 0 && bytes >= 32 && data[offset + 4] == packetId &&
        mtkLe16(data + offset + 6) == sequence)
      return {true, data[offset + 5] == 0, data[offset + 5],
              mtkLe16(data + offset + 10), data[offset + 9],
              mtkLe32(data + offset + 20)};
    offset += bytes;
  }
  return {};
}

inline bool mtkBeaconOnSix(const std::vector<uint8_t> &frame) {
  if (frame.size() < 36 || (mtkLe16(frame.data()) & 0xfc) != 0x80 ||
      (frame[10] & 1) ||
      !std::equal(frame.begin() + 10, frame.begin() + 16, frame.begin() + 16))
    return false;
  size_t offset = 36;
  while (offset + 2 <= frame.size()) {
    const unsigned tag = frame[offset++], length = frame[offset++];
    if (length > frame.size() - offset) return false;
    if (tag == 3 && length == 1) return frame[offset] == 6;
    offset += length;
  }
  return false;
}

inline std::vector<uint8_t> mtkDot11Frame(const uint8_t *data, size_t length) {
  if (length < 32) return {};
  const auto word0 = mtkLe32(data);
  if ((word0 >> 27) != 2 && (((word0 >> 16) & 0x380fu) != 0x3801u)) return {};
  const auto rxd1 = mtkLe32(data + 4), rxd2 = mtkLe32(data + 8);
  if ((rxd2 & (1u << 7)) || (mtkLe32(data + 12) & (1u << 24)))
    return {}; // Translation / bad FCS.
  if ((rxd1 & (1u << 20)) && !(rxd1 & (1u << 18)))
    return {}; // Group 5 requires group 3.
  size_t offset = 32;
  for (unsigned group : {19u, 16u, 17u, 18u})
    if (rxd1 & (1u << group)) offset += 16;
  if (rxd1 & (1u << 20)) offset += 96; // MT7925 C-RXV is 24 DWORDs.
  offset += 2 * ((rxd2 >> 13) & 7);
  const size_t total = mtkLe32(data) & 0xffff;
  if (total > length || total < offset || total - offset < 10) return {};
  const auto control = mtkLe16(data + offset);
  if ((control & 3) != 0) return {};
  const unsigned type = (control >> 2) & 3, subtype = (control >> 4) & 15;
  size_t header =
      type == 1 ? ((subtype == 12 || subtype == 13) ? 10u : 16u) : 24u;
  if (type == 3) return {};
  if (type == 2) {
    if ((control & 0x300) == 0x300) header += 6;
    if (subtype & 8) {
      header += 2;
      if (control & 0x8000) header += 4;
    }
  }
  if (total - offset < header) return {};
  return {data + offset, data + total};
}

inline std::array<uint8_t, 56> mtkNicCapabilityRequest(uint8_t sequence) {
  if (!sequence || sequence > 15)
    throw std::invalid_argument("Invalid MCU command sequence");
  std::array<uint8_t, 56> packet{};
  // Connac3 CMD format is TXD1[15:14], not Connac2 TXD1[17:16].
  // Confirmed independently in OEM MtCmdSendSetQueryUniCmdAdv, RVA 0x134c20.
  packet[0] = 56;
  packet[3] = 0x41;
  packet[5] = 0x40;
  packet[32] = 24;
  packet[34] = 0x0e;
  packet[37] = 0xa0;
  packet[39] = sequence;
  packet[43] = 6;
  packet[52] = 3;
  packet[54] = 4;
  return packet;
}

inline MtkNicCapability mtkParseNicCapability(const uint8_t *data,
                                              size_t length, uint8_t sequence) {
  MtkNicCapability result;
  // mt7925_mcu_rxd is eight RXD DWORDs, then a 12-byte event header.
  if (length < 44) return result;
  result.packetId = mtkLe16(data + 34);
  result.event = data[36];
  result.sequence = data[37];
  result.matched = result.event == 0x43 && result.sequence == sequence;
  if (!result.matched || length < 48) return result;
  result.elements = mtkLe16(data + 44);
  if (!result.elements || result.elements > 256) return result;
  size_t offset = 48;
  for (unsigned index = 0; index < result.elements; ++index) {
    if (offset > length || length - offset < 4) return result;
    const auto tag = mtkLe16(data + offset);
    const auto bytes = mtkLe16(data + offset + 2);
    if (bytes < 4 || bytes > length - offset) return result;
    result.tags.push_back(tag);
    if (tag == 7 && bytes >= 10) {
      std::copy(data + offset + 4, data + offset + 10, result.address.begin());
      result.addressValid =
          !(result.address[0] & 1) &&
          std::any_of(result.address.begin(), result.address.end(),
                      [](uint8_t byte) { return byte != 0; });
    }
    offset += bytes;
  }
  result.valid = true;
  return result;
}
