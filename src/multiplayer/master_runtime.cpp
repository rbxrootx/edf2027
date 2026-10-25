#include "master_runtime.h"
#include "enet_lifecycle.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <thread>

namespace edf::net {
namespace {

constexpr std::size_t kMaxEventsPerServicePass = 512;

}  // namespace

MasterRuntime::MasterRuntime(uint16_t port, LogSink log, std::string bind_host)
    : port_(port), log_(std::move(log)), bind_host_(std::move(bind_host)) {}
MasterRuntime::~MasterRuntime() { Stop(); }

bool MasterRuntime::Start(std::string* error) {
  if (host_) return true;
  if (!port_) { if (error) *error = "directory port must be nonzero"; return false; }
  if (!AcquireENet()) { if (error) *error = "ENet initialization failed"; return false; }
  enet_started_ = true;
  ENetAddress address{};
  address.host = ENET_HOST_ANY;
  if (!bind_host_.empty() && bind_host_ != "0.0.0.0" && enet_address_set_host(&address, bind_host_.c_str()) != 0) {
    if (error) *error = "could not resolve directory bind address";
    Stop();
    return false;
  }
  address.port = port_;
  host_ = enet_host_create(&address, 1024, 1, 0, 0);
  if (!host_) { if (error) *error = "could not bind directory port"; Stop(); return false; }
  host_->maximumPacketSize = kMaxPacketBytes;
  host_->maximumWaitingData = kMaxPacketBytes * 16;
  WriteLog("EDF2027 self-hosted directory listening on UDP " + std::to_string(port_));
  return true;
}

int MasterRuntime::Run(const std::atomic<bool>& stop) {
  std::string error;
  if (!Start(&error)) { WriteLog("Directory startup failed: " + error); return 2; }
  auto prune_at = std::chrono::steady_clock::now();
  int result = 0;
  while (!stop.load(std::memory_order_relaxed)) {
    ENetEvent event{};
    for (std::size_t serviced = 0; serviced < kMaxEventsPerServicePass; ++serviced) {
      const int service_result = enet_host_service(host_, &event, serviced == 0 ? 50 : 0);
      if (service_result < 0) {
        WriteLog("Directory transport failed while servicing ENet events");
        result = 3;
        break;
      }
      if (service_result == 0) break;
      Event(event);
    }
    if (result) break;
    if (std::chrono::steady_clock::now() >= prune_at) {
      Prune();
      prune_at = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    }
  }
  Stop();
  return result;
}

void MasterRuntime::Stop() {
  if (host_) {
    for (const auto& [peer, state] : peers_) { (void)state; enet_peer_disconnect_now(peer, 0); }
    peers_.clear();
    servers_.clear();
    enet_host_destroy(host_);
    host_ = nullptr;
  }
  if (enet_started_) { ReleaseENet(); enet_started_ = false; }
}

void MasterRuntime::Event(const ENetEvent& event) {
  if (event.type == ENET_EVENT_TYPE_CONNECT) {
    if (event.data != kProtocolVersion) {
      WriteLog("Rejected directory peer with incompatible protocol version");
      enet_peer_disconnect_now(event.peer, 1);
      return;
    }
    peers_.try_emplace(event.peer);
    enet_peer_timeout(event.peer, 8, 5'000, 15'000);
  } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
    Receive(event.peer, event.packet, event.channelID);
    enet_packet_destroy(event.packet);
  } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
    if (const auto found = servers_.find(event.peer); found != servers_.end()) {
      WriteLog("Server left directory: " + found->second.server.info.name);
      servers_.erase(found);
    }
    peers_.erase(event.peer);
  }
}

void MasterRuntime::Receive(ENetPeer* peer, ENetPacket* packet, uint8_t channel) {
  const auto peer_state = peers_.find(peer);
  if (peer_state == peers_.end()) {
    enet_peer_disconnect_later(peer, 1);
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  auto& rate = peer_state->second;
  if (rate.disconnecting) return;
  if (!packet || channel != 0 || packet->dataLength > kMaxPacketBytes) {
    rate.disconnecting = true;
    enet_peer_disconnect_later(peer, 1);
    return;
  }
  if (rate.window == std::chrono::steady_clock::time_point{} || now - rate.window >= std::chrono::seconds(1)) {
    rate.window = now;
    rate.packets = 0;
  }
  if (++rate.packets > 30) { rate.disconnecting = true; enet_peer_disconnect_later(peer, 2); return; }
  DecodedMessage message;
  if (!Decode(std::span(packet->data, packet->dataLength), message)) {
    rate.disconnecting = true;
    enet_peer_disconnect_later(peer, 3);
    return;
  }
  if (message.type == Message::kDirectoryRegister) {
    if (servers_.size() >= 512 && !servers_.contains(peer)) {
      const auto response = EncodeError("directory is full");
      ENetPacket* reply = enet_packet_create(response.data(), response.size(), ENET_PACKET_FLAG_RELIABLE);
      if (reply) enet_peer_send(peer, 0, reply);
      return;
    }
    auto& record = servers_[peer];
    record.server.ipv4 = message.directory_ipv4 ? message.directory_ipv4 : peer->address.host;
    record.server.info = std::move(message.server_info);
    record.last_seen = now;
    WriteLog("Registered server " + record.server.info.name + " at " +
        std::to_string(record.server.info.game_port));
    return;
  }
  if (message.type == Message::kDirectoryQuery) {
    SendPage(peer, message.directory_page);
    return;
  }
  rate.disconnecting = true;
  enet_peer_disconnect_later(peer, 4);
}

void MasterRuntime::SendPage(ENetPeer* peer, uint16_t page) {
  std::vector<DirectoryServer> active;
  active.reserve(servers_.size());
  for (const auto& [server_peer, record] : servers_) { (void)server_peer; active.push_back(record.server); }
  std::sort(active.begin(), active.end(), [](const auto& a, const auto& b) {
    return a.info.name == b.info.name ? a.ipv4 < b.ipv4 : a.info.name < b.info.name;
  });
  constexpr size_t kPerPage = 8;
  const size_t start = size_t(page) * kPerPage;
  if (start > active.size()) {
    if (const auto it = peers_.find(peer); it != peers_.end()) it->second.disconnecting = true;
    enet_peer_disconnect_later(peer, 5);
    return;
  }
  const size_t end = std::min(start + kPerPage, active.size());
  const auto payload = EncodeDirectoryList(page, end < active.size(), std::span(active).subspan(start, end - start));
  if (payload.empty()) {
    if (const auto it = peers_.find(peer); it != peers_.end()) it->second.disconnecting = true;
    enet_peer_disconnect_later(peer, 6);
    return;
  }
  ENetPacket* packet = enet_packet_create(payload.data(), payload.size(), ENET_PACKET_FLAG_RELIABLE);
  if (!packet || enet_peer_send(peer, 0, packet) < 0) {
    if (packet) enet_packet_destroy(packet);
    if (const auto it = peers_.find(peer); it != peers_.end()) it->second.disconnecting = true;
    enet_peer_disconnect_later(peer, 7);
  }
  enet_host_flush(host_);
}

void MasterRuntime::Prune() {
  const auto stale = std::chrono::steady_clock::now() - std::chrono::seconds(90);
  for (auto it = servers_.begin(); it != servers_.end();) {
    if (it->second.last_seen >= stale) { ++it; continue; }
    WriteLog("Expired directory entry: " + it->second.server.info.name);
    if (const auto peer = peers_.find(it->first); peer != peers_.end()) peer->second.disconnecting = true;
    enet_peer_disconnect(it->first, 8);
    it = servers_.erase(it);
  }
}

void MasterRuntime::WriteLog(std::string text) const { if (log_) log_(std::move(text)); }

}  // namespace edf::net
