#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "discovery.h"
#include "enet_lifecycle.h"
#include "resolve_ipv4.h"

#include <enet/enet.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <algorithm>
#include <charconv>
#include <chrono>

namespace edf::net {
namespace {
bool ParseDirectoryEndpoint(std::string_view text, std::string& host, uint16_t& port) {
  if (text.empty() || text.size() > 255) return false;
  const auto colon = text.rfind(':');
  if (colon == std::string_view::npos || colon == 0 || colon + 1 >= text.size()) return false;
  const auto port_text = text.substr(colon + 1);
  unsigned parsed = 0;
  const auto [end, error] = std::from_chars(port_text.data(), port_text.data() + port_text.size(), parsed);
  if (error != std::errc{} || end != port_text.data() + port_text.size() || parsed < 1 || parsed > 65535) return false;
  host.assign(text.substr(0, colon));
  if (host.find_first_of(" \t/\\") != std::string::npos) return false;
  port = static_cast<uint16_t>(parsed);
  return true;
}
void SetError(std::string* error, std::string text) { if (error) *error = std::move(text); }
}

std::vector<DiscoveredServer> QueryMasterDirectory(std::string_view endpoint, std::chrono::milliseconds timeout,
                                                   std::string* error, std::stop_token stop) {
  std::vector<DiscoveredServer> servers;
  if (error) error->clear();
  if (stop.stop_requested()) return servers;
  std::string host_name;
  uint16_t port = 0;
  if (!ParseDirectoryEndpoint(endpoint, host_name, port)) { SetError(error, "Directory address must be host:port"); return servers; }
  using Clock = std::chrono::steady_clock;
  const auto deadline = Clock::now() + std::max(timeout, std::chrono::milliseconds(0));
  if (!AcquireENet()) { SetError(error, "ENet initialization failed"); return servers; }
  ENetHost* host = enet_host_create(nullptr, 1, 1, 0, 0);
  if (!host) { ReleaseENet(); SetError(error, "Could not create directory client"); return servers; }
  host->maximumPacketSize = kMaxPacketBytes;
  host->maximumWaitingData = kMaxPacketBytes * 8;
  ENetAddress address{};
  std::string resolve_error;
  const auto resolve_timeout = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::max(Clock::duration::zero(), deadline - Clock::now()));
  if (!ResolveIPv4(host_name, port, resolve_timeout,
                   [&stop] { return stop.stop_requested(); }, address, &resolve_error)) {
    enet_host_destroy(host);
    ReleaseENet();
    if (!stop.stop_requested())
      SetError(error, resolve_error.empty() ? "Could not resolve directory address"
                                            : std::move(resolve_error));
    return servers;
  }
  if (stop.stop_requested()) { enet_host_destroy(host); ReleaseENet(); return servers; }
  ENetPeer* peer = enet_host_connect(host, &address, 1, kProtocolVersion);
  if (!peer) { enet_host_destroy(host); ReleaseENet(); SetError(error, "No ENet connection slots"); return servers; }
  bool connected = false;
  bool complete = false;
  uint16_t requested_page = 0;
  while (!complete && !stop.stop_requested() && Clock::now() < deadline) {
    ENetEvent event{};
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    const int serviced = enet_host_service(host, &event, static_cast<enet_uint32>(std::clamp<int64_t>(remaining.count(), 0, 20)));
    if (serviced < 0) {
      SetError(error, "Directory transport failed while waiting for a response");
      break;
    }
    if (serviced == 0) continue;
    if (event.type == ENET_EVENT_TYPE_CONNECT) {
      connected = true;
      const auto request = EncodeDirectoryQuery(0);
      ENetPacket* packet = enet_packet_create(request.data(), request.size(), ENET_PACKET_FLAG_RELIABLE);
      if (!packet || enet_peer_send(peer, 0, packet) < 0) {
        if (packet) enet_packet_destroy(packet);
        SetError(error, "Could not query directory");
        break;
      }
      enet_host_flush(host);
    } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
      DecodedMessage message;
      const bool valid = event.packet->dataLength <= kMaxPacketBytes &&
          Decode(std::span(event.packet->data, event.packet->dataLength), message) &&
          event.channelID == 0 && message.type == Message::kDirectoryList && message.directory_page == requested_page;
      if (!valid) {
        enet_packet_destroy(event.packet);
        SetError(error, "Directory sent an invalid response");
        break;
      }
      for (auto& server : message.directory_servers) {
        in_addr ip{};
        ip.s_addr = server.ipv4;
        char text[INET_ADDRSTRLEN]{};
        if (!inet_ntop(AF_INET, &ip, text, sizeof(text))) continue;
        const std::string address_text(text);
        const auto existing = std::find_if(servers.begin(), servers.end(), [&](const auto& item) {
          return item.address == address_text && item.info.game_port == server.info.game_port;
        });
        if (existing == servers.end()) servers.push_back({address_text, std::move(server.info)});
      }
      const bool more = message.directory_more;
      enet_packet_destroy(event.packet);
      if (more && requested_page < 255) {
        ++requested_page;
        const auto next = EncodeDirectoryQuery(requested_page);
        ENetPacket* packet = enet_packet_create(next.data(), next.size(), ENET_PACKET_FLAG_RELIABLE);
        if (!packet || enet_peer_send(peer, 0, packet) < 0) {
          if (packet) enet_packet_destroy(packet);
          SetError(error, "Could not continue directory query");
          break;
        }
        enet_host_flush(host);
      } else {
        complete = true;
      }
    } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
      SetError(error, connected ? "Directory disconnected" : "Directory connection failed");
      break;
    }
  }
  if (!complete && !stop.stop_requested() && error && error->empty()) SetError(error, "Directory query timed out");
  if (peer->state != ENET_PEER_STATE_DISCONNECTED) enet_peer_disconnect_now(peer, 0);
  enet_host_destroy(host);
  ReleaseENet();
  std::sort(servers.begin(), servers.end(), [](const auto& a, const auto& b) {
    return a.info.name == b.info.name ? a.address < b.address : a.info.name < b.info.name;
  });
  return servers;
}

}  // namespace edf::net
