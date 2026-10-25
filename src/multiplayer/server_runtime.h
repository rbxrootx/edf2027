#pragma once

#include "discovery.h"
#include "secure_channel.h"
#include "script_runtime.h"
#include "server_world.h"

#include <enet/enet.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace edf::net {

struct MasterResolution;

struct ServerConfig {
  std::string name = "EDF2027 Server";
  std::string mission = "Mission 1";
  std::string bind_host;  // empty binds all IPv4 interfaces
  uint16_t game_port = kDefaultGamePort;
  uint16_t discovery_port = kDefaultDiscoveryPort;
  uint16_t max_players = 8;
  uint16_t player_max_health = 100;
  uint32_t interest_radius_cm = 50'000;
  uint32_t max_client_transform_speed_cm_per_second = 6'000;
  uint32_t max_client_aim_turn_rate_mrad_per_second = 12'566;
  int32_t spawn_x_cm = 0;
  int32_t spawn_y_cm = 0;
  int32_t spawn_z_cm = 0;
  std::string master_endpoint;
  std::filesystem::path scripts_directory = "server-data/scripts";
  std::filesystem::path identity_file;
  std::string advertise_ip;  // Optional numeric IPv4 published to the directory; empty uses the observed source IP.
  bool mission_actor_diagnostics = false;  // Opt-in host-only sample stream; clients never spawn these records.
  bool mission_actor_reconciliation = false;  // Opt-in keyed host ant state for matched local objects.
  // Experimental input-only movement. The server ignores client poses and
  // advances coordinates from accepted input at its fixed simulation tick.
  bool server_authoritative_movement = false;
};

struct AuthoritativePlayerTransform {
  uint16_t player_id = 0;
  bool available = false;
  bool owner_pose_authoritative = false;
  uint16_t acknowledged_reset_player_id = 0;
  uint32_t acknowledged_reset_id = 0;
  int32_t x_cm = 0;
  int32_t y_cm = 0;
  int32_t z_cm = 0;
  int16_t yaw_mrad = 0;
  bool health_valid = false;
  bool alive = false;
  uint16_t health = 0;
  uint16_t max_health = 0;
};

struct AuthoritativePositionReset {
  uint16_t player_id = 0;
  uint32_t reset_id = 0;
  int32_t x_cm = 0;
  int32_t y_cm = 0;
  int32_t z_cm = 0;
  int16_t yaw_mrad = 0;
};

class ServerRuntime {
 public:
  using LogSink = std::function<void(std::string)>;
  // Called on the server loop after the command passes protocol and world
  // sequence/rate validation. A disengaged command means neutral input. Keep
  // this callback short and publish to a mailbox; guest memory belongs to the
  // guest thread.
  using InputSink = std::function<void(uint16_t, std::optional<InputCommand>,
                                       std::optional<AuthoritativePositionReset>)>;
  // Queried on the server loop after its fixed-step update; returns poses
  // sampled from the host's simulation thread. It must not read guest memory.
  using TransformSource = std::function<std::vector<AuthoritativePlayerTransform>()>;
  // Queues a server position reset for application on the guest simulation
  // thread. This callback must not access guest memory.
  using RebaseSink = std::function<bool(uint16_t, uint32_t, int32_t, int32_t, int32_t, int16_t)>;
  using AuthoritativeResetAckSink = std::function<void(uint16_t, uint32_t)>;
  using ActorSource = std::function<std::vector<ActorState>()>;
  // Queried on the server loop for trusted scripting only. It must return a
  // thread-safe sampled copy and never access GuestMemory; host mission actors
  // are deliberately kept out of ActorSnapshot serialization.
  using MissionActorSource = std::function<std::vector<ActorState>()>;
  // Cumulative removals sampled from the host simulation. This callback must
  // not access GuestMemory; its generation/revision pair is sent reliably.
  using MissionActorRemovalSource = std::function<MissionActorRemovalSnapshot()>;
  // Must bound its queued, in-flight, and live actor pool. The guest bridge
  // counts all three against kMaxSnapshotActors and releases slots on removal.
  using ActorSpawnSink = std::function<bool(const ActorSpawnRequest&)>;
  using ActorDamageSink = std::function<bool(uint32_t, float)>;
  explicit ServerRuntime(ServerConfig config, LogSink log = {}, InputSink input_sink = {},
                         TransformSource transform_source = {}, RebaseSink rebase_sink = {},
                             ActorSource actor_source = {}, ActorSpawnSink actor_spawn_sink = {},
                             ActorDamageSink actor_damage_sink = {},
                             MissionActorSource mission_actor_source = {},
                             MissionActorRemovalSource mission_actor_removal_source = {},
                             AuthoritativeResetAckSink authoritative_reset_ack_sink = {});
  ~ServerRuntime();
  ServerRuntime(const ServerRuntime&) = delete;
  ServerRuntime& operator=(const ServerRuntime&) = delete;

  bool Start(std::string* error = nullptr);
  int Run(const std::atomic<bool>& stop);
  void Stop();
  ServerInfo Info() const;
  void Tick();

 private:
  enum class PeerStage { kConnected, kChallenged, kAuthenticated };
  struct PeerState {
    PeerStage stage = PeerStage::kConnected;
    std::string name;
    uint64_t nonce = 0;
    uint64_t session_id = 0;
    CryptoKeyPair ephemeral_key_pair;
    CryptoNonce client_nonce{};
    CryptoNonce server_nonce{};
    std::optional<SecureChannel> secure_channel;
    uint16_t player_id = 0;
    std::chrono::steady_clock::time_point connected_at{};
    std::chrono::steady_clock::time_point rate_window{};
    std::chrono::steady_clock::time_point last_chat{};
    std::chrono::steady_clock::time_point last_input_at{};
    uint16_t packet_count = 0;
    bool disconnecting = false;
    bool input_timeout_notified = false;
    uint64_t mission_removal_generation_sent = 0;
    uint32_t mission_removal_page_cursor = 0;
    bool mission_removal_snapshot_sent = false;
  };
  void ServiceEvent(const ENetEvent& event);
  void Receive(ENetPeer* peer, ENetPacket* packet, uint8_t channel);
  void ServiceMaster();
  void RegisterWithMaster();
  void PruneHandshakeTimeouts(std::chrono::steady_clock::time_point now);
  void NotifyInput(uint16_t player_id, std::optional<InputCommand> input);
  bool Send(ENetPeer* peer, const std::vector<uint8_t>& bytes, uint8_t channel, uint32_t flags);
  bool SendPlain(ENetPeer* peer, const std::vector<uint8_t>& bytes, uint8_t channel, uint32_t flags);
  void BroadcastChat(uint16_t sender, std::string text);
  void Disconnect(ENetPeer* peer, std::string_view reason);
  void WriteLog(std::string text) const;
  bool HeadlessActorMode() const;

  ServerConfig config_;
  CryptoSigningKeyPair identity_;
  LogSink log_;
  InputSink input_sink_;
  TransformSource transform_source_;
  RebaseSink rebase_sink_;
  AuthoritativeResetAckSink authoritative_reset_ack_sink_;
  ActorSource actor_source_;
  MissionActorSource mission_actor_source_;
  MissionActorRemovalSource mission_actor_removal_source_;
  ActorSpawnSink actor_spawn_sink_;
  ActorDamageSink actor_damage_sink_;
  bool input_sink_disabled_ = false;
  bool transform_source_disabled_ = false;
  bool rebase_sink_disabled_ = false;
  std::vector<uint16_t> authoritative_player_ids_;
  std::unordered_map<uint16_t, uint32_t> last_guest_rebase_ids_;
  uint32_t next_actor_id_ = 1;
  uint32_t headless_actor_spawns_ = 0;
  uint32_t actor_damage_limit_tick_ = 0;
  std::unordered_map<uint32_t, uint8_t> actor_damage_requests_this_tick_;
  uint32_t player_damage_limit_tick_ = 0;
  std::unordered_map<uint16_t, uint8_t> player_damage_requests_this_tick_;
  // Callback-free servers keep their static abstract actors in this snapshot source.
  std::vector<ActorState> authoritative_actors_;
  std::vector<ActorState> mission_actors_;
  bool actor_source_disabled_ = false;
  bool actor_cap_logged_ = false;
  bool mission_actor_source_disabled_ = false;
  bool mission_actor_removal_source_disabled_ = false;
  bool mission_actor_cap_logged_ = false;
  bool actor_damage_sink_disabled_ = false;
  ENetHost* host_ = nullptr;
  ENetHost* master_host_ = nullptr;
  ENetPeer* master_peer_ = nullptr;
  std::shared_ptr<MasterResolution> master_resolution_;
  std::thread master_resolution_worker_;
  uint32_t advertised_ipv4_ = 0;
  std::chrono::steady_clock::time_point master_retry_{};
  std::chrono::steady_clock::time_point master_register_{};
  std::chrono::steady_clock::time_point last_chat_broadcast_{};
  bool enet_started_ = false;
  bool started_ = false;
  DiscoveryResponder discovery_;
  ServerWorld world_;
  ScriptRuntime scripts_;
  bool scripts_started_ = false;
  std::unordered_map<ENetPeer*, PeerState> peers_;
  uint32_t tick_ = 0;
  uint32_t presence_sequence_ = 0;
};

}  // namespace edf::net
