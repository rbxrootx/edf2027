#pragma once

#if __has_include("edf_port_build_fingerprint.h")
#include "edf_port_build_fingerprint.h"
#else
#include <string_view>
namespace edf::net {
// Type-only probe builds can disable multiplayer and therefore have no generated
// build fingerprint. The zero tag is never accepted by a built multiplayer server.
inline constexpr std::string_view kPortBuildFingerprint =
    "0000000000000000000000000000000000000000000000000000000000000000";
}
#endif

#include <cstdint>
#include <cstddef>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace edf::net {

inline constexpr uint16_t kProtocolVersion = 20;
// ENet fragments reliable cumulative mission-removal snapshots larger than a
// typical UDP MTU; the total reassembled application packet remains bounded.
inline constexpr size_t kMaxPacketBytes = 4096;
inline constexpr size_t kMaxSnapshotPlayers = 32;
inline constexpr size_t kMaxPlayers = kMaxSnapshotPlayers;  // Maximum player records in one snapshot.
inline constexpr size_t kMaxSnapshotActors = 48;
// Removal history is retained for one bounded mission generation and sent in
// reliable pages whose one-byte count fits the wire format.
inline constexpr size_t kMaxMissionActorRemovals = 255;
inline constexpr size_t kMaxMissionActorRemovalHistory = 4096;
inline constexpr uint32_t kMissionActorIdNamespace = 0x80000000u;
inline constexpr size_t kMaxServerPlayers = 512;
inline constexpr size_t kMaxPlayerNameBytes = 16;
inline constexpr size_t kMaxChatBytes = 255;
inline constexpr size_t kMaxServerNameBytes = 48;
inline constexpr size_t kMaxMissionNameBytes = 48;
inline constexpr size_t kBuildFingerprintBytes = 64;
inline constexpr size_t kServerBuildTagBytes = 12;
inline constexpr std::string_view kPortBuildTag = kPortBuildFingerprint.substr(0, kServerBuildTagBytes);
inline constexpr uint16_t kDefaultGamePort = 27717;
inline constexpr uint16_t kDefaultDiscoveryPort = 27718;

enum class Message : uint8_t {
  kDiscoveryRequest = 1,
  kServerInfo = 2,
  kHello = 3,
  kChallenge = 4,
  kAuthenticate = 5,
  kWelcome = 6,
  kInput = 7,
  kSnapshot = 8,
  kPing = 9,
  kPong = 10,
  kError = 11,
  kGoodbye = 12,
  kChat = 13,
  kDirectoryRegister = 14,
  kDirectoryQuery = 15,
  kDirectoryList = 16,
  kActorSnapshot = 17,
  // Read-only host mission samples. Clients must never feed this into replica creation.
  kMissionActorDiagnosticSnapshot = 18,
  kPlayerLeft = 19,
  // Bounded, opt-in identity metadata for read-only mission-ant diagnostics.
  kMissionActorIdentitySnapshot = 20,
  // Atomic host pose/health plus exact mission spawn identity for opt-in reconciliation.
  kMissionActorStateSnapshot = 21,
  // Reliable, cumulative host entity removals for exact-key mission reconciliation.
  kMissionActorRemovalSnapshot = 22,
};

struct ServerInfo {
  std::string name = "EDF2027 Server";
  std::string mission = "Mission 1";
  uint16_t game_port = kDefaultGamePort;
  uint16_t players = 0;
  uint16_t max_players = 8;
  uint16_t protocol = kProtocolVersion;
  std::string build_tag = std::string(kPortBuildTag);
  bool server_authoritative_movement = false;
};

struct InputCommand {
  uint32_t sequence = 0;
  uint16_t buttons = 0;
  uint8_t left_trigger = 0;
  uint8_t right_trigger = 0;
  int16_t move_x = 0;
  int16_t move_y = 0;
  int16_t aim_x = 0;
  int16_t aim_y = 0;
  // Camera/view direction for local movement; position is server-owned only in
  // input-only fallback mode and comes from the guest transform for native clients.
  int16_t aim_yaw_mrad = 0;
  // Client-reported hint for whether controls/pose should be forwarded. This
  // is not authoritative server-presence or health state.
  bool gameplay_active = false;
  // Ask the host to respawn this player at its configured spawn, never at a
  // client-provided coordinate.
  bool request_spawn = false;
  // Client acknowledgment for a specific server position reset after applying its snapshot.
  uint32_t position_reset_ack = 0;
  // Optional client-authoritative local guest pose, in centimeters. The server
  // validates bounds and sequences it, but does not verify the client's physics.
  bool transform_valid = false;
  int32_t x_cm = 0;
  int32_t y_cm = 0;
  int32_t z_cm = 0;
};

struct PlayerState {
  uint16_t id = 0;
  std::string name;
  bool active = false;
  // Explicit host-authorized reset for this player's own local mission position.
  bool position_reset = false;
  // Monotonically advances for each reset so stale acknowledgments cannot clear a newer one.
  uint32_t position_reset_id = 0;
  int32_t x_cm = 0;
  int32_t y_cm = 0;
  int32_t z_cm = 0;
  int16_t yaw_mrad = 0;
  // Vitals are present when copied from a trusted native host simulation or
  // owned by the dedicated server's configured health model. Worlds without
  // either trusted source leave these fields unset.
  bool health_valid = false;
  bool alive = false;
  uint16_t health = 0;
  uint16_t max_health = 0;
  uint16_t buttons = 0;
  uint8_t left_trigger = 0;
  uint8_t right_trigger = 0;
};

// Server-only view for trusted Luau scripts. This is never encoded in network
// snapshots; it pairs the server's public player state with that player's last
// accepted input command.
struct ScriptPlayerView {
  PlayerState state;
  InputCommand input;
};

struct WorldSnapshot {
  uint32_t tick = 0;
  // The server's transform source simulates this observer's pilot on its host.
  // The owner should reconcile its local soldier to the same sampled pose.
  bool owner_pose_authoritative = false;
  // Server network snapshots put the observer first so reset metadata targets its owner.
  std::vector<PlayerState> players;
};

enum class ActorArchetype : uint8_t { kAnt = 1 };

struct ActorState {
  uint32_t id = 0;
  ActorArchetype archetype = ActorArchetype::kAnt;
  int32_t x_cm = 0;
  int32_t y_cm = 0;
  int32_t z_cm = 0;
  int16_t yaw_mrad = 0;
  uint16_t health = 0;
  // Script-view provenance; intentionally omitted from ActorSnapshot wire encoding.
  bool server_owned = false;
  // Host-only trace metadata. It is carried through sampled copies, never in gameplay snapshots.
  bool trace_identity_valid = false;
  uint64_t trace_generation = 0;
  uint32_t trace_call_ordinal = 0;
  uint32_t trace_ant_ordinal = 0;
};

struct ActorSnapshot {
  uint32_t tick = 0;
  std::vector<ActorState> actors;
};

struct MissionActorIdentity {
  uint32_t id = 0;
  uint32_t call_ordinal = 0;
  uint32_t ant_ordinal = 0;
};

struct MissionActorIdentitySnapshot {
  uint32_t tick = 0;
  // Process-local host trace generation; useful only to reject stale host samples.
  uint64_t host_trace_generation = 0;
  std::vector<MissionActorIdentity> actors;
};

struct MissionActorStateSnapshot {
  uint32_t tick = 0;
  uint64_t host_trace_generation = 0;
  // ActorState trace fields carry the unique mission spawn key for each state.
  std::vector<ActorState> actors;
};

// Internal request passed from the server worker to the host game's engine
// thread. It intentionally carries the spawn key, never a guest pointer.
struct MissionActorDamageRequest {
  uint64_t trace_generation = 0;
  uint32_t call_ordinal = 0;
  uint32_t ant_ordinal = 0;
  float damage_points = 0.0f;
};

struct MissionActorRemoval {
  uint32_t id = 0;
  uint32_t call_ordinal = 0;
  uint32_t ant_ordinal = 0;
};

struct MissionActorRemovalSnapshot {
  uint32_t tick = 0;
  uint64_t host_trace_generation = 0;
  // Total host removal count for this trace generation, including entries in
  // earlier pages. ClientView stores the fully merged history with offset zero.
  uint32_t revision = 0;
  uint32_t page_offset = 0;
  std::vector<MissionActorRemoval> removals;
};

struct ActorSpawnRequest {
  uint32_t id = 0;
  ActorArchetype archetype = ActorArchetype::kAnt;
  int32_t x_cm = 0;
  int32_t y_cm = 0;
  int32_t z_cm = 0;
  int16_t yaw_mrad = 0;
};

// Server worker to game-thread damage request for a server-owned native actor.
// Actor IDs are validated against the current copied actor snapshot before queueing.
struct ActorDamageRequest {
  uint32_t actor_id = 0;
  float damage_points = 0.0f;
};

struct DirectoryServer {
  uint32_t ipv4 = 0;  // ENet's IPv4 host value, in network byte order.
  ServerInfo info;
};

struct DecodedMessage {
  Message type{};
  ServerInfo server_info;
  uint32_t directory_ipv4 = 0;  // Optional server-advertised address for directory registration.
  InputCommand input;
  WorldSnapshot snapshot;
  ActorSnapshot actor_snapshot;
  ActorSnapshot mission_actor_diagnostic_snapshot;
  MissionActorIdentitySnapshot mission_actor_identity_snapshot;
  MissionActorStateSnapshot mission_actor_state_snapshot;
  MissionActorRemovalSnapshot mission_actor_removal_snapshot;
  uint16_t directory_page = 0;
  bool directory_more = false;
  std::vector<DirectoryServer> directory_servers;
  uint64_t nonce = 0;
  uint64_t session_id = 0;
  std::array<uint8_t, 32> client_public_key{};
  std::array<uint8_t, 32> server_public_key{};
  std::array<uint8_t, 32> client_nonce{};
  std::array<uint8_t, 32> server_nonce{};
  std::array<uint8_t, 32> server_identity_key{};
  std::array<uint8_t, 64> server_signature{};
  std::string build_fingerprint;
  uint16_t player_id = 0;
  uint32_t tick = 0;
  bool server_authoritative_movement = false;
  std::string text;
};

// All network inputs are length-checked, versioned, and encoded explicitly. No C++
// struct layout or guest pointers cross the wire.
std::vector<uint8_t> EncodeDiscoveryRequest();
std::vector<uint8_t> EncodeServerInfo(const ServerInfo& info);
std::vector<uint8_t> EncodeHello(std::string_view name);
std::vector<uint8_t> EncodeHello(std::string_view name,
                                const std::array<uint8_t, 32>& public_key,
                                const std::array<uint8_t, 32>& client_nonce);
std::vector<uint8_t> EncodeChallenge(uint64_t nonce, uint64_t session_id,
                                    const std::array<uint8_t, 32>& client_nonce,
                                    const std::array<uint8_t, 32>& server_public_key,
                                    const std::array<uint8_t, 32>& server_nonce,
                                    const std::array<uint8_t, 32>& server_identity_key,
                                    const std::array<uint8_t, 64>& signature);
std::vector<uint8_t> EncodeAuthenticate(uint64_t nonce);
std::vector<uint8_t> EncodeWelcome(uint16_t player_id, uint32_t tick,
                                   bool server_authoritative_movement);
std::vector<uint8_t> EncodeInput(const InputCommand& input);
std::vector<uint8_t> EncodeSnapshot(const WorldSnapshot& snapshot);
std::vector<uint8_t> EncodeActorSnapshot(const ActorSnapshot& snapshot);
std::vector<uint8_t> EncodeMissionActorDiagnosticSnapshot(const ActorSnapshot& snapshot);
std::vector<uint8_t> EncodeMissionActorIdentitySnapshot(const MissionActorIdentitySnapshot& snapshot);
std::vector<uint8_t> EncodeMissionActorStateSnapshot(const MissionActorStateSnapshot& snapshot);
bool IsMissionActorStateSnapshotNewer(const MissionActorStateSnapshot& incoming,
                                      const MissionActorStateSnapshot& current);
std::vector<uint8_t> EncodeMissionActorRemovalSnapshot(const MissionActorRemovalSnapshot& snapshot);
bool IsMissionActorRemovalSnapshotNewer(const MissionActorRemovalSnapshot& incoming,
                                       const MissionActorRemovalSnapshot& current);
bool MergeMissionActorRemovalPage(const MissionActorRemovalSnapshot& page,
                                  MissionActorRemovalSnapshot& accumulated);
std::vector<uint8_t> EncodePing(uint32_t tick);
std::vector<uint8_t> EncodePong(uint32_t tick);
std::vector<uint8_t> EncodeError(std::string_view message);
std::vector<uint8_t> EncodeGoodbye();
std::vector<uint8_t> EncodePlayerLeft(uint16_t player_id, uint32_t tick);
std::vector<uint8_t> EncodeChat(uint16_t sender, std::string_view message);
std::vector<uint8_t> EncodeDirectoryRegister(const ServerInfo& info, uint32_t advertised_ipv4 = 0);
std::vector<uint8_t> EncodeDirectoryQuery(uint16_t page);
std::vector<uint8_t> EncodeDirectoryList(uint16_t page, bool more, std::span<const DirectoryServer> servers);
bool Decode(std::span<const uint8_t> bytes, DecodedMessage& out, std::string* error = nullptr);

}  // namespace edf::net
