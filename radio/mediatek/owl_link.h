// SPDX-License-Identifier: GPL-2.0-only
// Owns OWL and carries AWDL frames over radio_link v3.
#pragma once
#include "../../mediatek/native/paths.h"
#include "../pipe_server.h"
#include <array>
#include <deque>
#include <stdexcept>
#include <thread>
// Windows networking headers have an ordering dependency.
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
// clang-format on

class OwlLink {
public:
  explicit OwlLink(HANDLE stop)
      : externalStop_(stop),
        pipe_(RADIO_LINK_PIPE_NAME, [this] { return stopping(); }) {}
  ~OwlLink() { stop(); }
  OwlLink(const OwlLink &) = delete;
  OwlLink &operator=(const OwlLink &) = delete;
  void start() {
    if (!pipe_.valid())
      throw std::runtime_error("The AWDL pipe could not be created");
    const auto executable = mtkRoot() / L"build\\awdl\\awdld.exe";
    std::wstring command = L"\"" + executable.wstring() + L"\" -c 6 -r";
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    HANDLE log =
        CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ, &attributes,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE input =
        CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE) {
      if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
      if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
      throw std::runtime_error("AWDL diagnostic logging could not be opened");
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input;
    startup.hStdOutput = log;
    startup.hStdError = log;
    job_ = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job_ ||
        !SetInformationJobObject(job_, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits))) {
      CloseHandle(log);
      CloseHandle(input);
      throw std::runtime_error("AWDL child lifetime could not be bounded");
    }
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(
        executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
        (mtkRoot() / L"build\\awdl").c_str(), &startup, &process);
    CloseHandle(log);
    CloseHandle(input);
    if (!created)
      throw std::runtime_error("The local AWDL daemon could not be started");
    child_ = process.hProcess;
    if (!AssignProcessToJobObject(job_, child_) ||
        ResumeThread(process.hThread) == DWORD(-1)) {
      CloseHandle(process.hThread);
      throw std::runtime_error("AWDL child could not enter its bounded job");
    }
    CloseHandle(process.hThread);
    if (!pipe_.wait_for_client())
      throw std::runtime_error("The local AWDL daemon did not connect");
    if (pipe_.client_process_id() != process.dwProcessId)
      throw std::runtime_error("The AWDL pipe client is not the owned daemon");
    const radio_link_hello hello{RADIO_LINK_VERSION, 6, 20, {0, 0}};
    if (!pipe_.send(RADIO_LINK_HELLO,
                    {reinterpret_cast<const uint8_t *>(&hello), sizeof(hello)}))
      throw std::runtime_error("AWDL pipe handshake failed");
    reader_ = std::thread([this] { read(); });
    const auto addressDeadline = GetTickCount64() + 3000;
    while (GetTickCount64() < addressDeadline && !stopping()) {
      bool ready = false;
      {
        std::lock_guard<std::mutex> guard(mutex_);
        ready = addressReady_;
      }
      if (ready) {
        verifyHostInterface();
        return;
      }
      Sleep(1);
    }
    throw std::runtime_error("A valid local AWDL address was not received");
  }
  std::array<uint8_t, 6> address() {
    std::lock_guard<std::mutex> guard(mutex_);
    return address_;
  }
  bool pop(std::vector<uint8_t> &frame) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (queue_.empty()) return false;
    frame = std::move(queue_.front());
    queue_.pop_front();
    return true;
  }
  bool popData(std::vector<uint8_t> &frame) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto &queue = unicastQueue_.empty() ? multicastQueue_ : unicastQueue_;
    if (queue.empty()) return false;
    frame = std::move(queue.front());
    queue.pop_front();
    return true;
  }
  unsigned unicastDropped() const { return unicastDropped_; }
  unsigned multicastDropped() const { return multicastDropped_; }
  unsigned hostIndex() const { return hostIndex_; }
  bool receive(const std::vector<uint8_t> &frame) {
    static constexpr std::array<uint8_t, 6> bssid{
        {0, 0x25, 0, 0xff, 0x94, 0x73}};
    if (frame.size() < 24 ||
        !std::equal(bssid.begin(), bssid.end(), frame.begin() + 16))
      return true;
    auto packet = std::vector<uint8_t>(9);
    packet[2] = uint8_t(packet.size());
    packet[4] = 2; // Explicit radiotap FLAGS=0: DMA frames contain no FCS.
    packet.insert(packet.end(), frame.begin(), frame.end());
    return pipe_.send(RADIO_LINK_RX, packet);
  }
  bool stopping() const {
    return stopped_ || (externalStop_ &&
                        WaitForSingleObject(externalStop_, 0) == WAIT_OBJECT_0);
  }
  void stop() {
    stopped_ = true;
    if (reader_.joinable()) reader_.join();
    // Disconnect lets OWL run awdl_free/host_if_close and remove Wintun.
    // A forced stop remains bounded and targets only our owned handle.
    pipe_.drop_client();
    if (child_ && WaitForSingleObject(child_, 3000) == WAIT_TIMEOUT)
      TerminateProcess(child_, 0);
    if (child_) {
      WaitForSingleObject(child_, 1000);
      CloseHandle(child_);
      child_ = nullptr;
    }
    if (job_) {
      CloseHandle(job_);
      job_ = nullptr;
    }
  }

private:
  void verifyHostInterface() {
    const auto own = address();
    std::array<uint8_t, 16> source{{0xfe, 0x80}};
    source[8] = own[0] ^ 2;
    source[9] = own[1];
    source[10] = own[2];
    source[11] = 0xff;
    source[12] = 0xfe;
    std::copy(own.begin() + 3, own.end(), source.begin() + 13);
    const auto deadline = GetTickCount64() + 4000;
    do {
      std::vector<uint64_t> storage(8192);
      ULONG bytes = static_cast<ULONG>(storage.size() * sizeof(uint64_t));
      auto *adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(storage.data());
      if (GetAdaptersAddresses(AF_INET6,
                               GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                   GAA_FLAG_SKIP_DNS_SERVER,
                               nullptr, adapters, &bytes) == NO_ERROR)
        for (auto *item = adapters; item; item = item->Next)
          if (item->FriendlyName &&
              std::wcscmp(item->FriendlyName, L"awdl0") == 0 &&
              item->OperStatus == IfOperStatusUp)
            for (auto *unicast = item->FirstUnicastAddress; unicast;
                 unicast = unicast->Next)
              if (unicast->Address.lpSockaddr &&
                  unicast->Address.lpSockaddr->sa_family == AF_INET6 &&
                  std::memcmp(&reinterpret_cast<sockaddr_in6 *>(
                                   unicast->Address.lpSockaddr)
                                   ->sin6_addr,
                              source.data(), source.size()) == 0) {
                hostIndex_ = item->Ipv6IfIndex;
                return;
              }
      Sleep(50);
    } while (GetTickCount64() < deadline && !stopping());
    throw std::runtime_error(
        "The owned IPv6 host interface did not become ready");
  }
  void read() {
    try {
      while (!stopping()) {
        radio_link_header header{};
        if (!pipe_.read_exact(&header, sizeof(header)) || header.reserved ||
            header.length > 2048)
          break;
        std::vector<uint8_t> bytes(header.length);
        if (header.length && !pipe_.read_exact(bytes.data(), header.length))
          break;
        if (header.type == RADIO_LINK_SET_ADDRESS) {
          if (bytes.size() != 6 || (bytes[0] & 3) != 2) break;
          std::lock_guard<std::mutex> guard(mutex_);
          if (addressReady_) break;
          std::copy(bytes.begin(), bytes.end(), address_.begin());
          addressReady_ = true;
        } else if (header.type == RADIO_LINK_SET_BSSID_FILTER) {
          const std::vector<uint8_t> expected{0, 0x25, 0, 0xff, 0x94, 0x73};
          if (bytes != expected) break;
        } else if (header.type == RADIO_LINK_SET_CHANNEL) {
          if (bytes.size() != 1 || (bytes[0] != 6)) break;
        } else if (header.type == RADIO_LINK_TX) {
          if (bytes.size() < 8) break;
          const size_t radiotap = size_t(bytes[2]) | (size_t(bytes[3]) << 8);
          if (bytes[0] || radiotap < 8 || radiotap > bytes.size() ||
              bytes.size() - radiotap < 40)
            break;
          std::vector<uint8_t> frame(bytes.begin() + radiotap, bytes.end());
          std::lock_guard<std::mutex> guard(mutex_);
          static constexpr std::array<uint8_t, 6> bssid{
              {0, 0x25, 0, 0xff, 0x94, 0x73}};
          if (!addressReady_ || frame[1] || frame.size() > 1536 ||
              !std::equal(address_.begin(), address_.end(),
                          frame.begin() + 10) ||
              !std::equal(bssid.begin(), bssid.end(), frame.begin() + 16))
            break;
          if (frame[0] == 8) {
            static constexpr std::array<uint8_t, 8> llc{
                {0xaa, 0xaa, 3, 0, 0x17, 0xf2, 8, 0}};
            if (frame.size() < 80 ||
                !std::equal(llc.begin(), llc.end(), frame.begin() + 24) ||
                frame[32] != 3 || frame[33] != 4 || frame[38] != 0x86 ||
                frame[39] != 0xdd || (frame[40] >> 4) != 6 ||
                size_t(frame[44]) * 256 + frame[45] + 80 != frame.size())
              break;
            // TLS and TCP acknowledgements must not wait behind
            // repeated multicast service announcements. Preserve
            // unicast ordering and keep both queues bounded.
            if (frame[4] & 1) {
              if (multicastQueue_.size() == 8) {
                multicastQueue_.pop_front();
                ++multicastDropped_;
              }
              multicastQueue_.push_back(std::move(frame));
            } else if (unicastQueue_.size() == 64)
              ++unicastDropped_;
            else
              unicastQueue_.push_back(std::move(frame));
            continue;
          }
          // The action path accepts only OWL's broadcast AWDL actions.
          if (frame[0] != 0xd0 || frame[24] != 0x7f || frame[25] ||
              frame[26] != 0x17 || frame[27] != 0xf2 || frame[28] != 8 ||
              !std::equal(address_.begin(), address_.end(),
                          frame.begin() + 10) ||
              !std::all_of(frame.begin() + 4, frame.begin() + 10,
                           [](uint8_t byte) { return byte == 0xff; }))
            break;
          // Keep the newest scheduled action when the hardware cannot keep up.
          if (queue_.size() == 3) queue_.pop_front();
          queue_.push_back(std::move(frame));
        } else
          break;
      }
    } catch (...) {
    }
    stopped_ = true;
  }
  std::atomic<bool> stopped_{false};
  HANDLE externalStop_;
  PipeServer pipe_;
  HANDLE child_ = nullptr;
  HANDLE job_ = nullptr;
  std::thread reader_;
  std::atomic<unsigned> hostIndex_{0};
  std::mutex mutex_;
  std::array<uint8_t, 6> address_{};
  bool addressReady_ = false;
  std::deque<std::vector<uint8_t>> queue_;
  std::deque<std::vector<uint8_t>> unicastQueue_, multicastQueue_;
  std::atomic<unsigned> unicastDropped_{0}, multicastDropped_{0};
};
