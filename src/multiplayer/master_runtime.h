#pragma once

#include "protocol.h"

#include <enet/enet.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace edf::net {

class MasterRuntime {
 public:
  using LogSink = std::function<void(std::string)>;
  explicit MasterRuntime(uint16_t port = 27719, LogSink log = {}, std::string bind_host = {});
  ~MasterRuntime();
  bool Start(std::string* error = nullptr);
  int Run(const std::atomic<bool>& stop);
  void Stop();

 private:
  struct PeerState {
    uint16_t packets = 0;
    std::chrono::steady_clock::time_point window{};
    bool disconnecting = false;
  };
  struct Record { DirectoryServer server; std::chrono::steady_clock::time_point last_seen{}; };
  void Event(const ENetEvent& event);
  void Receive(ENetPeer* peer, ENetPacket* packet, uint8_t channel);
  void SendPage(ENetPeer* peer, uint16_t page);
  void WriteLog(std::string text) const;
  void Prune();

  uint16_t port_;
  LogSink log_;
  std::string bind_host_;
  ENetHost* host_ = nullptr;
  bool enet_started_ = false;
  std::unordered_map<ENetPeer*, PeerState> peers_;
  std::unordered_map<ENetPeer*, Record> servers_;
};

}  // namespace edf::net
