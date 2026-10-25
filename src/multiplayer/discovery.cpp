#include "discovery.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
#endif

namespace edf::net {
namespace {
constexpr size_t kMaxDiscoveryDatagramsPerPoll = 64;
constexpr double kDiscoveryResponsesPerSecond = 32.0;
constexpr double kDiscoveryResponseBurst = 32.0;
constexpr auto kDiscoverySourceCooldown = std::chrono::milliseconds(250);
NativeSocket AsSocket(intptr_t socket) { return static_cast<NativeSocket>(socket); }
intptr_t StoreSocket(NativeSocket socket) { return static_cast<intptr_t>(socket); }
void CloseSocket(NativeSocket socket) {
#if defined(_WIN32)
  closesocket(socket);
#else
  close(socket);
#endif
}
bool StartWinsock() {
#if defined(_WIN32)
  WSADATA data{};
  return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
  return true;
#endif
}
void StopWinsock() {
#if defined(_WIN32)
  WSACleanup();
#endif
}
bool SetReuseAndBroadcast(NativeSocket socket) {
  const int enabled = 1;
  return setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&enabled), sizeof(enabled)) == 0 &&
         setsockopt(socket, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&enabled), sizeof(enabled)) == 0;
}
bool WaitReadable(NativeSocket socket, std::chrono::milliseconds duration) {
  fd_set reads;
  FD_ZERO(&reads);
  FD_SET(socket, &reads);
  timeval timeout{};
  timeout.tv_sec = static_cast<long>(duration.count() / 1000);
  timeout.tv_usec = static_cast<long>((duration.count() % 1000) * 1000);
#if defined(_WIN32)
  return select(0, &reads, nullptr, nullptr, &timeout) > 0;
#else
  return select(socket + 1, &reads, nullptr, nullptr, &timeout) > 0;
#endif
}
void Error(std::string* out, std::string message) { if (out) *out = std::move(message); }
}  // namespace

DiscoveryResponder::~DiscoveryResponder() { Close(); }

bool DiscoveryResponder::Open(uint16_t port, std::string* error) {
  Close();
  if (!StartWinsock()) { Error(error, "Winsock initialization failed"); return false; }
  winsock_started_ = true;
  const NativeSocket socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (socket == kInvalidSocket) { Error(error, "could not create LAN discovery socket"); Close(); return false; }
  if (!SetReuseAndBroadcast(socket)) { Error(error, "could not configure LAN discovery socket"); CloseSocket(socket); Close(); return false; }
  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  local.sin_port = htons(port);
  if (bind(socket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
    Error(error, "could not bind LAN discovery port " + std::to_string(port));
    CloseSocket(socket);
    Close();
    return false;
  }
  socket_ = StoreSocket(socket);
  return true;
}

void DiscoveryResponder::Poll(const ServerInfo& info) {
  if (socket_ == -1) return;
  const auto socket = AsSocket(socket_);
  for (size_t processed = 0; processed < kMaxDiscoveryDatagramsPerPoll &&
       WaitReadable(socket, std::chrono::milliseconds(0)); ++processed) {
    std::array<uint8_t, kMaxPacketBytes + 1> packet{};
    sockaddr_in from{};
#if defined(_WIN32)
    int from_size = sizeof(from);
    const int received = recvfrom(socket, reinterpret_cast<char*>(packet.data()), int(packet.size()), 0,
                                  reinterpret_cast<sockaddr*>(&from), &from_size);
#else
    socklen_t from_size = sizeof(from);
    const auto received = recvfrom(socket, packet.data(), packet.size(), 0,
                                   reinterpret_cast<sockaddr*>(&from), &from_size);
#endif
    if (received <= 0 || static_cast<size_t>(received) > kMaxPacketBytes) continue;
    DecodedMessage request;
    if (!Decode(std::span(packet.data(), static_cast<size_t>(received)), request) || request.type != Message::kDiscoveryRequest)
      continue;
    const auto now = std::chrono::steady_clock::now();
    auto source = std::find_if(source_rates_.begin(), source_rates_.end(), [&](const SourceRate& rate) {
      return rate.used && rate.ipv4 == from.sin_addr.s_addr;
    });
    if (source == source_rates_.end()) {
      source = std::find_if(source_rates_.begin(), source_rates_.end(), [](const SourceRate& rate) {
        return !rate.used;
      });
      if (source == source_rates_.end()) {
        source = std::min_element(source_rates_.begin(), source_rates_.end(),
            [](const SourceRate& left, const SourceRate& right) { return left.last_seen < right.last_seen; });
      }
      *source = {true, from.sin_addr.s_addr, now, {}};
    } else {
      source->last_seen = now;
    }
    if (source->last_response != std::chrono::steady_clock::time_point{} &&
        now - source->last_response < kDiscoverySourceCooldown) continue;
    if (response_rate_updated_ == std::chrono::steady_clock::time_point{}) {
      response_rate_updated_ = now;
    } else {
      const double elapsed = std::chrono::duration<double>(now - response_rate_updated_).count();
      response_tokens_ = std::min(kDiscoveryResponseBurst,
          response_tokens_ + elapsed * kDiscoveryResponsesPerSecond);
      response_rate_updated_ = now;
    }
    if (response_tokens_ < 1.0) continue;
    const auto response = EncodeServerInfo(info);
    if (response.empty()) continue;
    const auto sent = sendto(socket, reinterpret_cast<const char*>(response.data()), static_cast<int>(response.size()), 0,
                             reinterpret_cast<const sockaddr*>(&from), from_size);
    if (sent == static_cast<decltype(sent)>(response.size())) {
      response_tokens_ -= 1.0;
      source->last_response = now;
    }
  }
}

void DiscoveryResponder::Close() {
  if (socket_ != -1) { CloseSocket(AsSocket(socket_)); socket_ = -1; }
  if (winsock_started_) { StopWinsock(); winsock_started_ = false; }
  source_rates_ = {};
  response_tokens_ = kDiscoveryResponseBurst;
  response_rate_updated_ = {};
}

std::vector<DiscoveredServer> DiscoverServers(uint16_t discovery_port, std::chrono::milliseconds timeout,
                                               std::string* error, std::stop_token stop) {
  std::vector<DiscoveredServer> servers;
  if (error) error->clear();
  if (stop.stop_requested()) return servers;
  if (!StartWinsock()) { Error(error, "Winsock initialization failed"); return servers; }
  const NativeSocket socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (socket == kInvalidSocket) { Error(error, "could not create LAN scan socket"); StopWinsock(); return servers; }
  if (!SetReuseAndBroadcast(socket)) { Error(error, "could not configure LAN scan socket"); CloseSocket(socket); StopWinsock(); return servers; }
  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  local.sin_port = 0;
  if (bind(socket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
    Error(error, "could not bind LAN scan socket"); CloseSocket(socket); StopWinsock(); return servers;
  }
  sockaddr_in target{};
  target.sin_family = AF_INET;
  target.sin_addr.s_addr = htonl(INADDR_BROADCAST);
  target.sin_port = htons(discovery_port);
  const auto request = EncodeDiscoveryRequest();
  if (sendto(socket, reinterpret_cast<const char*>(request.data()), static_cast<int>(request.size()), 0,
             reinterpret_cast<const sockaddr*>(&target), sizeof(target)) < 0) {
    Error(error, "LAN broadcast failed"); CloseSocket(socket); StopWinsock(); return servers;
  }
  const auto deadline = std::chrono::steady_clock::now() + std::max(timeout, std::chrono::milliseconds(0));
  while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
    const auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (!WaitReadable(socket, std::min(remain, std::chrono::milliseconds(50)))) continue;
    std::array<uint8_t, kMaxPacketBytes + 1> packet{};
    sockaddr_in from{};
#if defined(_WIN32)
    int from_size = sizeof(from);
    const int received = recvfrom(socket, reinterpret_cast<char*>(packet.data()), int(packet.size()), 0,
                                  reinterpret_cast<sockaddr*>(&from), &from_size);
#else
    socklen_t from_size = sizeof(from);
    const auto received = recvfrom(socket, packet.data(), packet.size(), 0,
                                   reinterpret_cast<sockaddr*>(&from), &from_size);
#endif
    if (received <= 0 || static_cast<size_t>(received) > kMaxPacketBytes) continue;
    DecodedMessage response;
    if (!Decode(std::span(packet.data(), static_cast<size_t>(received)), response) || response.type != Message::kServerInfo ||
        response.server_info.protocol != kProtocolVersion) continue;
    char address[INET_ADDRSTRLEN]{};
    if (!inet_ntop(AF_INET, &from.sin_addr, address, sizeof(address))) continue;
    const std::string ip(address);
    auto it = std::find_if(servers.begin(), servers.end(), [&](const auto& found) { return found.address == ip && found.info.game_port == response.server_info.game_port; });
    if (it == servers.end()) servers.push_back({ip, std::move(response.server_info)});
    else it->info = std::move(response.server_info);
  }
  CloseSocket(socket);
  StopWinsock();
  std::sort(servers.begin(), servers.end(), [](const auto& a, const auto& b) {
    return a.info.name == b.info.name ? a.address < b.address : a.info.name < b.info.name;
  });
  return servers;
}

}  // namespace edf::net
