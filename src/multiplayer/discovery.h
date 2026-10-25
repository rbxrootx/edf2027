#pragma once

#include "protocol.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <stop_token>
#include <vector>

namespace edf::net {

struct DiscoveredServer {
  std::string address;
  ServerInfo info;
};

// The LAN directory uses a small, separate UDP port. Requests and replies are
// versioned EDF packets; the responder never reflects arbitrary input payloads.
class DiscoveryResponder {
 public:
  DiscoveryResponder() = default;
  ~DiscoveryResponder();
  DiscoveryResponder(const DiscoveryResponder&) = delete;
  DiscoveryResponder& operator=(const DiscoveryResponder&) = delete;
  bool Open(uint16_t port, std::string* error = nullptr);
  void Poll(const ServerInfo& info);
  void Close();

 private:
  struct SourceRate {
    bool used = false;
    uint32_t ipv4 = 0;
    std::chrono::steady_clock::time_point last_seen{};
    std::chrono::steady_clock::time_point last_response{};
  };
  intptr_t socket_ = -1;
  bool winsock_started_ = false;
  std::array<SourceRate, 64> source_rates_{};
  double response_tokens_ = 32.0;
  std::chrono::steady_clock::time_point response_rate_updated_{};
};

std::vector<DiscoveredServer> DiscoverServers(
    uint16_t discovery_port = kDefaultDiscoveryPort,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(600),
    std::string* error = nullptr,
    std::stop_token stop = {});

// Queries an optional self-hosted Internet directory (host:port, default port 27719).
std::vector<DiscoveredServer> QueryMasterDirectory(
    std::string_view endpoint,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(2500),
    std::string* error = nullptr,
    std::stop_token stop = {});

}  // namespace edf::net
