#pragma once

#include "protocol.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace edf::net {

enum class ClientState { kDisconnected, kConnecting, kConnected, kFailed };

struct ClientView {
  ClientState state = ClientState::kDisconnected;
  std::string endpoint;
  std::string status;
  uint16_t local_player_id = 0;
  bool server_authoritative_movement = false;
  WorldSnapshot snapshot;
  ActorSnapshot actor_snapshot;
  ActorSnapshot mission_actor_diagnostic_snapshot;
  MissionActorIdentitySnapshot mission_actor_identity_snapshot;
  MissionActorStateSnapshot mission_actor_state_snapshot;
  MissionActorRemovalSnapshot mission_actor_removal_snapshot;
  std::vector<std::string> chat;
};

class ClientSession {
 public:
  static ClientSession& Global();
  ClientSession() = default;
  ~ClientSession();
  ClientSession(const ClientSession&) = delete;
  ClientSession& operator=(const ClientSession&) = delete;

  bool Connect(std::string endpoint, std::string player_name, std::string* error = nullptr);
  void Disconnect();
  // Cancel only a connection that is still pending. If the handshake has
  // already completed, leave that established session untouched.
  bool CancelPendingConnect();
  // Refresh periodically while connected; the client sends a neutral inactive
  // sample if this is not called for 250 ms.
  void SetInput(InputCommand input);
  bool SendChat(std::string message);
  ClientView View() const;
  bool CopySnapshotIfNew(uint64_t& generation, WorldSnapshot& snapshot, uint16_t& local_player_id) const;
  bool CopyActorSnapshotIfNew(uint64_t& generation, ActorSnapshot& snapshot) const;
  bool CopyMissionActorDiagnosticSnapshotIfNew(uint64_t& generation, ActorSnapshot& snapshot) const;
  bool CopyMissionActorIdentitySnapshotIfNew(uint64_t& generation,
                                             MissionActorIdentitySnapshot& snapshot) const;
  bool CopyMissionActorStateSnapshotIfNew(uint64_t& generation,
                                          MissionActorStateSnapshot& snapshot) const;
  bool CopyMissionActorRemovalSnapshotIfNew(uint64_t& generation,
                                            MissionActorRemovalSnapshot& snapshot) const;
  bool connected() const;
  bool connected(uint64_t& connection_epoch) const;
  uint16_t local_player_id() const;
  bool server_authoritative_movement() const;

 private:
  void Run(std::string host, uint16_t port, std::string player_name, std::string pin_endpoint);
  void SetStatus(ClientState state, std::string text);
  void DisconnectLocked();

  // Serializes Connect/Disconnect/CancelPendingConnect and access to worker_.
  std::mutex lifecycle_mutex_;
  mutable std::mutex mutex_;
  ClientView view_;
  uint64_t connection_epoch_ = 0;
  uint64_t snapshot_generation_ = 0;
  uint64_t actor_snapshot_generation_ = 0;
  uint64_t mission_actor_diagnostic_generation_ = 0;
  uint64_t mission_actor_identity_generation_ = 0;
  uint64_t mission_actor_state_generation_ = 0;
  uint64_t mission_actor_removal_generation_ = 0;
  struct CachedPresence {
    PlayerState state;
    std::chrono::steady_clock::time_point last_seen{};
  };
  std::unordered_map<uint16_t, CachedPresence> roster_cache_;
  // Reliable removal events carry the server tick so delayed snapshots cannot
  // resurrect a player that has already left this session.
  std::unordered_map<uint16_t, uint32_t> departed_players_;
  InputCommand latest_input_;
  std::chrono::steady_clock::time_point latest_input_at_{};
  std::deque<std::string> outgoing_chat_;
  uint32_t next_sequence_ = 0;
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> worker_finished_{true};
  bool thread_running_ = false;
  std::thread worker_;
};

}  // namespace edf::net
