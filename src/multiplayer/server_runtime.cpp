#include "server_runtime.h"
#include "enet_lifecycle.h"
#include "resolve_ipv4.h"

#include <enet/enet.h>
#include <sodium.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <exception>
#include <fstream>
#include <mutex>
#include <thread>

namespace edf::net {

struct MasterResolution {
  std::atomic<bool> cancelled{false};
  std::atomic<bool> completed{false};
  std::mutex mutex;
  bool succeeded = false;
  ENetAddress address{};
  std::string error;
};

namespace {
constexpr auto kMinimumChatBroadcastInterval = std::chrono::milliseconds(50);
constexpr size_t kMaxPendingHandshakesPerServer = 32;
constexpr size_t kMaxEventsPerServicePass = 512;
constexpr uint8_t kMaxActorDamageRequestsPerActorPerTick = 1;
constexpr uint8_t kMaxPlayerDamageRequestsPerPlayerPerTick = 1;

bool LoadOrCreateServerIdentity(const std::filesystem::path& path,
                                CryptoSigningKeyPair& identity, std::string* error) {
  if (path.empty()) {
    if (error) *error = "server identity file path is empty";
    return false;
  }
  std::error_code ec;
  if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    if (error) *error = "could not create server identity directory: " + ec.message();
    return false;
  }
  CryptoNonce seed{};
  if (std::filesystem::exists(path, ec)) {
    if (ec || std::filesystem::file_size(path, ec) != seed.size() || ec) {
      if (error) *error = "server identity file must contain exactly 32 seed bytes; move it aside to regenerate";
      return false;
    }
    std::ifstream input(path, std::ios::binary);
    input.read(reinterpret_cast<char*>(seed.data()), static_cast<std::streamsize>(seed.size()));
    if (!input || input.gcount() != static_cast<std::streamsize>(seed.size())) {
      if (error) *error = "could not read server identity seed";
      sodium_memzero(seed.data(), seed.size());
      return false;
    }
  } else {
    if (ec || !GenerateCryptoNonce(seed)) {
      if (error) *error = "could not create server signing identity";
      return false;
    }
    const auto temporary = path.string() + ".tmp";
    {
      std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
      output.write(reinterpret_cast<const char*>(seed.data()), static_cast<std::streamsize>(seed.size()));
      output.flush();
      if (!output) {
        if (error) *error = "could not write server identity seed";
        sodium_memzero(seed.data(), seed.size());
        return false;
      }
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
      std::filesystem::remove(temporary);
      if (error) *error = "could not commit server identity seed: " + ec.message();
      sodium_memzero(seed.data(), seed.size());
      return false;
    }
  }
  std::filesystem::permissions(path,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace, ec);
#if !defined(_WIN32)
  if (ec) {
    if (error) *error = "could not restrict server identity file permissions: " + ec.message();
    sodium_memzero(seed.data(), seed.size());
    return false;
  }
#else
  ec.clear();
#endif
  const bool derived = DeriveSigningKeyPair(seed, identity);
  sodium_memzero(seed.data(), seed.size());
  if (!derived && error) *error = "could not derive the server signing key from its identity seed";
  return derived;
}

uint64_t RandomSessionId() {
  CryptoNonce random{};
  if (!GenerateCryptoNonce(random)) return 0;
  uint64_t value = 0;
  for (size_t i = 0; i < sizeof(value); ++i) value |= uint64_t(random[i]) << (i * 8);
  sodium_memzero(random.data(), random.size());
  return value ? value : 1;
}
}

ServerRuntime::ServerRuntime(ServerConfig config, LogSink log, InputSink input_sink,
                             TransformSource transform_source, RebaseSink rebase_sink,
                             ActorSource actor_source, ActorSpawnSink actor_spawn_sink,
                             ActorDamageSink actor_damage_sink,
                             MissionActorSource mission_actor_source,
                             MissionActorRemovalSource mission_actor_removal_source,
                             AuthoritativeResetAckSink authoritative_reset_ack_sink)
    : config_(std::move(config)), log_(std::move(log)), input_sink_(std::move(input_sink)),
      transform_source_(std::move(transform_source)), rebase_sink_(std::move(rebase_sink)),
      authoritative_reset_ack_sink_(std::move(authoritative_reset_ack_sink)),
      actor_source_(std::move(actor_source)), mission_actor_source_(std::move(mission_actor_source)),
      mission_actor_removal_source_(std::move(mission_actor_removal_source)),
      actor_spawn_sink_(std::move(actor_spawn_sink)),
      actor_damage_sink_(std::move(actor_damage_sink)),
      world_(config_.max_players, config_.spawn_x_cm, config_.spawn_y_cm, config_.spawn_z_cm,
             static_cast<bool>(transform_source_) && static_cast<bool>(rebase_sink_),
             !config_.server_authoritative_movement,
             config_.max_client_transform_speed_cm_per_second,
             config_.server_authoritative_movement && !transform_source_,
             config_.player_max_health,
             config_.max_client_aim_turn_rate_mrad_per_second),
      scripts_([this](std::string line) { WriteLog(std::move(line)); },
               [this](std::string message) { BroadcastChat(0, std::move(message)); },
               [this](uint16_t id, int32_t x_cm, int32_t y_cm, int32_t z_cm, int16_t yaw_mrad) {
                 if (!world_.RebasePlayerPosition(id, x_cm, y_cm, z_cm, yaw_mrad)) return false;
                 InputCommand neutral;
                 neutral.gameplay_active = false;
                 NotifyInput(id, neutral);
                 return true;
                },
                [this] { return world_.ScriptPlayers(); },
                [this](std::string_view archetype, int32_t x_cm, int32_t y_cm, int32_t z_cm,
                       int16_t yaw_mrad) -> std::optional<uint32_t> {
                  const bool headless_actor_mode = HeadlessActorMode();
                  if (archetype != "ant" ||
                      (headless_actor_mode && headless_actor_spawns_ >= kMaxSnapshotActors))
                    return std::nullopt;
                  ActorSpawnRequest spawn{next_actor_id_, ActorArchetype::kAnt, x_cm, y_cm, z_cm, yaw_mrad};
                  if (!spawn.id) return std::nullopt;
                  if (headless_actor_mode) {
                    ActorState actor;
                    actor.id = spawn.id;
                    actor.archetype = spawn.archetype;
                    actor.x_cm = spawn.x_cm;
                    actor.y_cm = spawn.y_cm;
                    actor.z_cm = spawn.z_cm;
                    actor.yaw_mrad = spawn.yaw_mrad;
                    actor.health = 60;  // GiantAnt01 go_Durability on the game disc.
                    actor.server_owned = true;
                    authoritative_actors_.push_back(actor);
                  } else if (!actor_spawn_sink_ || actor_source_disabled_ || !actor_spawn_sink_(spawn)) {
                    return std::nullopt;
                  }
                  const uint32_t id = spawn.id;
                  ++next_actor_id_;
                  if (!next_actor_id_) next_actor_id_ = 1;
                  if (headless_actor_mode) ++headless_actor_spawns_;
                  return id;
                },
                [this](uint32_t actor_id, float damage_points) {
                  const bool headless = HeadlessActorMode();
                  if ((!headless && (!actor_damage_sink_ || actor_damage_sink_disabled_ ||
                      (!actor_source_ && !mission_actor_source_))) || !std::isfinite(damage_points) ||
                      damage_points <= 0.0f || damage_points > 65535.0f)
                    return false;
                  if (actor_damage_limit_tick_ != tick_) {
                    actor_damage_limit_tick_ = tick_;
                    actor_damage_requests_this_tick_.clear();
                  }
                  const auto is_current_ant = [actor_id](const std::vector<ActorState>& actors) {
                    return std::find_if(actors.begin(), actors.end(), [actor_id](const ActorState& state) {
                      return state.id == actor_id && state.health > 0 &&
                          state.archetype == ActorArchetype::kAnt;
                    }) != actors.end();
                  };
                  const bool current_server_actor = (headless || (actor_source_ && !actor_source_disabled_)) &&
                      is_current_ant(authoritative_actors_);
                  const bool current_mission_actor = mission_actor_source_ && !mission_actor_source_disabled_ &&
                      is_current_ant(mission_actors_);
                  if (!current_server_actor && !current_mission_actor) return false;
                  const auto [count, inserted] = actor_damage_requests_this_tick_.try_emplace(actor_id, 0);
                  if (count->second >= kMaxActorDamageRequestsPerActorPerTick) return false;
                  if (headless) {
                    const auto actor = std::find_if(authoritative_actors_.begin(), authoritative_actors_.end(),
                        [actor_id](const ActorState& state) {
                          return state.id == actor_id && state.health > 0 &&
                              state.archetype == ActorArchetype::kAnt;
                        });
                    if (actor == authoritative_actors_.end()) {
                      if (inserted) actor_damage_requests_this_tick_.erase(count);
                      return false;
                    }
                    const float remaining = std::max(0.0f, static_cast<float>(actor->health) - damage_points);
                    const long rounded_remaining = std::lround(remaining);
                    actor->health = static_cast<uint16_t>(rounded_remaining);
                  } else {
                    try {
                      if (!actor_damage_sink_(actor_id, damage_points)) {
                        if (inserted) actor_damage_requests_this_tick_.erase(count);
                        return false;
                      }
                    } catch (...) {
                      if (inserted) actor_damage_requests_this_tick_.erase(count);
                      actor_damage_sink_disabled_ = true;
                      WriteLog("Guest actor damage sink threw; sink disabled");
                      return false;
                    }
                  }
                  ++count->second;
                  return true;
                },
                [this] {
                  std::vector<ActorState> actors;
                  if (HeadlessActorMode() || (actor_source_ && !actor_source_disabled_)) {
                    for (ActorState actor : authoritative_actors_) {
                      actor.server_owned = true;
                      actors.push_back(actor);
                    }
                  }
                  if (mission_actor_source_ && !mission_actor_source_disabled_) {
                    for (ActorState actor : mission_actors_) {
                      actor.server_owned = false;
                      actors.push_back(actor);
                    }
                  }
                  return actors;
                },
                [this](uint32_t actor_id, int32_t x_cm, int32_t y_cm, int32_t z_cm,
                       int16_t yaw_mrad) {
                  // The standalone server simulates script-owned records itself.
                  // The guest-backed host leaves native EDF actors to the guest AI.
                  if (!HeadlessActorMode() ||
                      x_cm < -1'000'000 || x_cm > 1'000'000 ||
                      y_cm < -1'000'000 || y_cm > 1'000'000 ||
                      z_cm < -1'000'000 || z_cm > 1'000'000 ||
                      yaw_mrad < -3142 || yaw_mrad > 3142)
                    return false;
                  const auto actor = std::find_if(authoritative_actors_.begin(), authoritative_actors_.end(),
                      [actor_id](const ActorState& candidate) {
                        return candidate.id == actor_id && candidate.health > 0 &&
                            candidate.archetype == ActorArchetype::kAnt && candidate.server_owned;
                      });
                  if (actor == authoritative_actors_.end()) return false;
                  actor->x_cm = x_cm;
                  actor->y_cm = y_cm;
                  actor->z_cm = z_cm;
                  actor->yaw_mrad = yaw_mrad;
                  return true;
                },
                [this](uint16_t player_id, uint16_t damage_points) {
                  if (!config_.server_authoritative_movement || transform_source_ || !damage_points)
                    return false;
                  if (player_damage_limit_tick_ != tick_) {
                    player_damage_limit_tick_ = tick_;
                    player_damage_requests_this_tick_.clear();
                  }
                  const auto [count, inserted] =
                      player_damage_requests_this_tick_.try_emplace(player_id, 0);
                  if (count->second >= kMaxPlayerDamageRequestsPerPlayerPerTick) return false;
                  if (!world_.DamagePlayer(player_id, damage_points)) {
                    if (inserted) player_damage_requests_this_tick_.erase(count);
                    return false;
                  }
                  ++count->second;
                  return true;
                }) {}

ServerRuntime::~ServerRuntime() {
  Stop();
  if (master_resolution_worker_.joinable()) {
    try {
      master_resolution_worker_.detach();
    } catch (...) {
      try { master_resolution_worker_.join(); } catch (...) {}
    }
  }
  ClearCryptoSigningKeyPair(identity_);
}

bool ServerRuntime::Start(std::string* error) {
  if (started_) return true;
  if (static_cast<bool>(transform_source_) != static_cast<bool>(rebase_sink_)) {
    if (error) *error = "guest transform and position reset callbacks must be supplied together";
    return false;
  }
  if (static_cast<bool>(actor_source_) != static_cast<bool>(actor_spawn_sink_)) {
    if (error) *error = "guest actor snapshot and spawn callbacks must be supplied together";
    return false;
  }
  if (config_.game_port == 0 || (config_.discovery_port && config_.game_port == config_.discovery_port) ||
      config_.max_players == 0 || config_.max_players > kMaxServerPlayers ||
      config_.max_client_transform_speed_cm_per_second < 100 ||
      config_.max_client_transform_speed_cm_per_second > 100'000 ||
      config_.max_client_aim_turn_rate_mrad_per_second < 1'570 ||
      config_.max_client_aim_turn_rate_mrad_per_second > 62'832 ||
      config_.interest_radius_cm == 0 || config_.interest_radius_cm > 1'000'000) {
    if (error) *error = "invalid server ports or player limit";
    return false;
  }
  if (config_.identity_file.empty()) config_.identity_file = config_.scripts_directory.parent_path() / "identity.edfkey";
  if (!LoadOrCreateServerIdentity(config_.identity_file, identity_, error)) return false;
  // A restart gets a fresh set of callback bridges and per-run server state.
  // A callback that failed in a previous run should not stay disabled forever.
  input_sink_disabled_ = false;
  transform_source_disabled_ = false;
  rebase_sink_disabled_ = false;
  actor_source_disabled_ = false;
  actor_cap_logged_ = false;
  mission_actor_source_disabled_ = false;
  mission_actor_cap_logged_ = false;
  actor_damage_sink_disabled_ = false;
  actor_damage_limit_tick_ = 0;
  actor_damage_requests_this_tick_.clear();
  authoritative_player_ids_.clear();
  last_guest_rebase_ids_.clear();
  authoritative_actors_.clear();
  mission_actors_.clear();
  headless_actor_spawns_ = 0;
  last_chat_broadcast_ = {};
  // ServerWorld survives Stop()/Start() on this runtime object and keeps its
  // tick. Keep wire event timestamps in the same monotonic tick domain.
  presence_sequence_ = 0;
  if (!AcquireENet()) { if (error) *error = "ENet initialization failed"; return false; }
  enet_started_ = true;
  advertised_ipv4_ = 0;
  if (!config_.advertise_ip.empty()) {
    ENetAddress advertised{};
    if (enet_address_set_host_ip(&advertised, config_.advertise_ip.c_str()) != 0 || advertised.host == ENET_HOST_ANY) {
      if (error) *error = "advertised IP must be a numeric IPv4 address other than 0.0.0.0";
      Stop();
      return false;
    }
    advertised_ipv4_ = advertised.host;
  }
  ENetAddress address{};
  address.host = ENET_HOST_ANY;
  if (!config_.bind_host.empty() && config_.bind_host != "0.0.0.0" && enet_address_set_host(&address, config_.bind_host.c_str()) != 0) {
    if (error) *error = "could not resolve bind address";
    Stop();
    return false;
  }
  address.port = config_.game_port;
  const size_t pending_handshake_limit =
      std::min<size_t>(config_.max_players, kMaxPendingHandshakesPerServer);
  host_ = enet_host_create(&address, size_t(config_.max_players) + pending_handshake_limit, 2, 0, 0);
  if (!host_) { if (error) *error = "could not bind the gameplay port"; Stop(); return false; }
  host_->maximumPacketSize = kMaxPacketBytes;
  host_->maximumWaitingData = kMaxPacketBytes * 8;
  if (config_.discovery_port && !discovery_.Open(config_.discovery_port, error)) { Stop(); return false; }

  // Scripts belong to one server run. Rebuild the Luau state on reuse so
  // globals and event hooks from a previous run cannot leak or stack up.
  if (scripts_started_) scripts_.Reset();

  std::error_code ec;
  std::filesystem::create_directories(config_.scripts_directory, ec);
  size_t loaded_scripts = 0;
  if (ec) {
    WriteLog("Luau: cannot prepare scripts directory " + config_.scripts_directory.string() + ": " + ec.message());
  } else {
    std::vector<std::filesystem::path> scripts;
    std::filesystem::directory_iterator entry(config_.scripts_directory, ec), end;
    while (!ec && entry != end) {
      const bool regular_file = entry->is_regular_file(ec);
      if (ec) break;
      if (regular_file && entry->path().extension() == ".luau") scripts.push_back(entry->path());
      entry.increment(ec);
    }
    if (ec) WriteLog("Luau: cannot scan scripts directory " + config_.scripts_directory.string() + ": " + ec.message());
    std::sort(scripts.begin(), scripts.end());
    for (const auto& path : scripts) loaded_scripts += scripts_.LoadFile(path) ? 1u : 0u;
  }
  scripts_started_ = true;
  started_ = true;
  WriteLog("EDF2027 multiplayer server listening on UDP " + std::to_string(config_.game_port) +
      (config_.discovery_port ? " (LAN discovery " + std::to_string(config_.discovery_port) + ")" : " (LAN discovery disabled)"));
  WriteLog("Capacity " + std::to_string(config_.max_players) + "; Luau scripts " +
      std::to_string(loaded_scripts) + " loaded from " + config_.scripts_directory.string());
  WriteLog(std::string("Player movement authority: ") +
      (config_.server_authoritative_movement ? "server input simulation" : "client transforms"));
  if (config_.server_authoritative_movement && !transform_source_)
    WriteLog("Player vitals authority: server Luau policy; max health " +
             std::to_string(config_.player_max_health) +
             "; at most one scripted hit per player per tick");
  if (!config_.server_authoritative_movement) {
    WriteLog("Client transform speed ceiling: " +
             std::to_string(config_.max_client_transform_speed_cm_per_second / 100) + " m/s");
  }
  WriteLog("Client aim turn-rate ceiling: " +
           std::to_string(config_.max_client_aim_turn_rate_mrad_per_second) + " mrad/s");
  if (!config_.master_endpoint.empty()) {
    master_host_ = enet_host_create(nullptr, 1, 1, 0, 0);
    if (!master_host_) {
      WriteLog("Could not create master-directory client; direct and LAN joining still work; retrying shortly");
      master_retry_ = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    }
    else {
      master_host_->maximumPacketSize = kMaxPacketBytes;
      master_host_->maximumWaitingData = kMaxPacketBytes * 8;
      master_retry_ = std::chrono::steady_clock::now();
    }
  }
  return true;
}

int ServerRuntime::Run(const std::atomic<bool>& stop) {
  std::string error;
  if (!Start(&error)) { WriteLog("Server startup failed: " + error); return 2; }
  using Clock = std::chrono::steady_clock;
  constexpr auto step = std::chrono::microseconds(1'000'000 / 60);
  auto next_tick = Clock::now();
  auto snapshot_deadline = next_tick;
  int result = 0;
  while (!stop.load(std::memory_order_relaxed)) {
    ENetEvent event{};
    for (size_t serviced = 0; serviced < kMaxEventsPerServicePass; ++serviced) {
      const int service_result = enet_host_service(host_, &event, 0);
      if (service_result < 0) {
        WriteLog("Gameplay transport failed while servicing ENet events");
        result = 3;
        break;
      }
      if (service_result == 0) break;
      ServiceEvent(event);
    }
    if (result) break;
    if (config_.discovery_port) discovery_.Poll(Info());
    ServiceMaster();
    const auto now = Clock::now();
    PruneHandshakeTimeouts(now);
    if (now >= next_tick) {
      Tick();
      next_tick += step;
      if (now - next_tick > step * 4) next_tick = now + step;
      if (now >= snapshot_deadline) {
        const uint32_t presence_sequence = ++presence_sequence_;
        MissionActorRemovalSnapshot removal_snapshot;
        bool have_removal_snapshot = false;
        if (config_.mission_actor_reconciliation && mission_actor_removal_source_ &&
            !mission_actor_removal_source_disabled_) {
          try {
            removal_snapshot = mission_actor_removal_source_();
            removal_snapshot.tick = tick_;
            if (removal_snapshot.revision == removal_snapshot.removals.size() &&
                removal_snapshot.revision <= kMaxMissionActorRemovalHistory) {
              have_removal_snapshot = true;
            } else {
              mission_actor_removal_source_disabled_ = true;
              WriteLog("Host mission actor removal source returned an invalid bounded history; source disabled");
            }
          } catch (...) {
            mission_actor_removal_source_disabled_ = true;
            WriteLog("Host mission actor removal source threw; source disabled");
          }
        }
        for (auto& [peer, state] : peers_) {
          if (state.stage != PeerStage::kAuthenticated || state.disconnecting) continue;
          const auto player_snapshot = world_.SnapshotFor(
              state.player_id, config_.interest_radius_cm, presence_sequence);
          const auto bytes = EncodeSnapshot(player_snapshot);
          if (!bytes.empty()) Send(peer, bytes, 1, ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
          if (actor_source_ || HeadlessActorMode()) {
            ActorSnapshot actor_snapshot;
            actor_snapshot.tick = tick_;
            if (!player_snapshot.players.empty() && player_snapshot.players.front().active) {
              const PlayerState& observer = player_snapshot.players.front();
              const int64_t radius = config_.interest_radius_cm;
              const int64_t radius_squared = radius * radius;
              actor_snapshot.actors.reserve(authoritative_actors_.size());
              for (const ActorState& actor : authoritative_actors_) {
                const int64_t dx = int64_t(actor.x_cm) - observer.x_cm;
                const int64_t dy = int64_t(actor.y_cm) - observer.y_cm;
                const int64_t dz = int64_t(actor.z_cm) - observer.z_cm;
                if (dx * dx + dy * dy + dz * dz <= radius_squared)
                  actor_snapshot.actors.push_back(actor);
              }
            }
            const auto actor_bytes = EncodeActorSnapshot(actor_snapshot);
            if (!actor_bytes.empty()) Send(peer, actor_bytes, 1, ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
          }
          // Mission actor samples share the existing 50 ms snapshot deadline.
          // A tick-modulo gate here phase-locks against the 60 Hz simulation
          // step and can skip otherwise due samples.
          if ((config_.mission_actor_diagnostics || config_.mission_actor_reconciliation) &&
              mission_actor_source_ && !mission_actor_source_disabled_) {
            ActorSnapshot diagnostic_snapshot;
            diagnostic_snapshot.tick = tick_;
            if (!player_snapshot.players.empty() && player_snapshot.players.front().active) {
              const PlayerState& observer = player_snapshot.players.front();
              const int64_t radius = config_.interest_radius_cm;
              const int64_t radius_squared = radius * radius;
              diagnostic_snapshot.actors.reserve(mission_actors_.size());
              for (const ActorState& actor : mission_actors_) {
                const int64_t dx = int64_t(actor.x_cm) - observer.x_cm;
                const int64_t dy = int64_t(actor.y_cm) - observer.y_cm;
                const int64_t dz = int64_t(actor.z_cm) - observer.z_cm;
                if (dx * dx + dy * dy + dz * dz <= radius_squared)
                  diagnostic_snapshot.actors.push_back(actor);
              }
            }
            if (config_.mission_actor_diagnostics) {
              const auto diagnostic_bytes = EncodeMissionActorDiagnosticSnapshot(diagnostic_snapshot);
              if (!diagnostic_bytes.empty())
                Send(peer, diagnostic_bytes, 1, ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);

              MissionActorIdentitySnapshot identity_snapshot;
              identity_snapshot.tick = tick_;
              identity_snapshot.actors.reserve(diagnostic_snapshot.actors.size());
              for (const ActorState& actor : diagnostic_snapshot.actors) {
                if (!actor.trace_identity_valid || !actor.trace_generation) continue;
                if (!identity_snapshot.host_trace_generation)
                  identity_snapshot.host_trace_generation = actor.trace_generation;
                if (actor.trace_generation != identity_snapshot.host_trace_generation) continue;
                identity_snapshot.actors.push_back(
                    {actor.id, actor.trace_call_ordinal, actor.trace_ant_ordinal});
              }
              auto identity_bytes = EncodeMissionActorIdentitySnapshot(identity_snapshot);
              if (identity_bytes.empty()) {
                // Invalid or ambiguous metadata clears the remote candidate set
                // instead of leaving an older identity sample cached by clients.
                identity_snapshot.actors.clear();
                identity_bytes = EncodeMissionActorIdentitySnapshot(identity_snapshot);
              }
              if (!identity_bytes.empty())
                Send(peer, identity_bytes, 1, ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
            }

            if (config_.mission_actor_reconciliation) {
              MissionActorStateSnapshot state_snapshot;
              state_snapshot.tick = tick_;
              state_snapshot.actors.reserve(diagnostic_snapshot.actors.size());
              bool mixed_generations = false;
              for (const ActorState& actor : diagnostic_snapshot.actors) {
                if (!actor.trace_identity_valid || !actor.trace_generation) continue;
                if (!state_snapshot.host_trace_generation)
                  state_snapshot.host_trace_generation = actor.trace_generation;
                if (actor.trace_generation != state_snapshot.host_trace_generation) {
                  mixed_generations = true;
                  break;
                }
                state_snapshot.actors.push_back(actor);
              }
              if (mixed_generations) {
                // One atomic snapshot must describe one trace generation. Send
                // an empty clear instead of a partial set with stale associations.
                state_snapshot.host_trace_generation = 0;
                state_snapshot.actors.clear();
              }
              auto state_bytes = EncodeMissionActorStateSnapshot(state_snapshot);
              if (state_bytes.empty()) {
                state_snapshot.host_trace_generation = 0;
                state_snapshot.actors.clear();
                state_bytes = EncodeMissionActorStateSnapshot(state_snapshot);
              }
              if (!state_bytes.empty())
                Send(peer, state_bytes, 1, ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
            }
          }
          if (have_removal_snapshot) {
            const bool new_generation = !state.mission_removal_snapshot_sent ||
                state.mission_removal_generation_sent != removal_snapshot.host_trace_generation;
            const bool history_shrank = !new_generation &&
                state.mission_removal_page_cursor > removal_snapshot.removals.size();
            const uint32_t page_offset = (new_generation || history_shrank)
                ? 0 : state.mission_removal_page_cursor;
            if (new_generation || history_shrank || page_offset < removal_snapshot.removals.size()) {
              MissionActorRemovalSnapshot page;
              page.tick = tick_;
              page.host_trace_generation = removal_snapshot.host_trace_generation;
              page.revision = removal_snapshot.revision;
              page.page_offset = page_offset;
              const size_t page_end = std::min(removal_snapshot.removals.size(),
                  size_t(page_offset) + kMaxMissionActorRemovals);
              page.removals.insert(page.removals.end(),
                  removal_snapshot.removals.begin() + page_offset,
                  removal_snapshot.removals.begin() + page_end);
              const auto removal_bytes = EncodeMissionActorRemovalSnapshot(page);
              if (!removal_bytes.empty() &&
                  Send(peer, removal_bytes, 1, ENET_PACKET_FLAG_RELIABLE)) {
                state.mission_removal_generation_sent = removal_snapshot.host_trace_generation;
                state.mission_removal_page_cursor = static_cast<uint32_t>(page_end);
                state.mission_removal_snapshot_sent = true;
              }
            }
          }
        }
        snapshot_deadline = now + std::chrono::milliseconds(50);
        enet_host_flush(host_);
      }
    } else {
      std::this_thread::sleep_until(next_tick);
    }
  }
  Stop();
  return result;
}

void ServerRuntime::Tick() {
  ++tick_;
  const auto now = std::chrono::steady_clock::now();
  for (auto& [peer, state] : peers_) {
    (void)peer;
    if (state.stage != PeerStage::kAuthenticated || state.input_timeout_notified ||
        state.last_input_at == std::chrono::steady_clock::time_point{} ||
        now - state.last_input_at <= std::chrono::milliseconds(250)) continue;
    // Stop stale controls without releasing the guest-backed seat. A short
    // packet stall should recover in place; only an actual disconnect tears
    // down the native EDF player object.
    InputCommand neutral;
    NotifyInput(state.player_id, neutral);
    state.input_timeout_notified = true;
  }
  world_.Tick();
  if (transform_source_ && !transform_source_disabled_) {
    try {
      const auto transforms = transform_source_();
      std::vector<uint16_t> current_player_ids;
      current_player_ids.reserve(transforms.size());
      for (const auto& transform : transforms) {
        if (!transform.player_id ||
            std::find(current_player_ids.begin(), current_player_ids.end(), transform.player_id) != current_player_ids.end())
          continue;
        current_player_ids.push_back(transform.player_id);
        if (std::find(authoritative_player_ids_.begin(), authoritative_player_ids_.end(), transform.player_id) ==
            authoritative_player_ids_.end())
          last_guest_rebase_ids_.erase(transform.player_id);
        if (transform.acknowledged_reset_player_id == transform.player_id && transform.acknowledged_reset_id &&
            world_.AcknowledgeAuthoritativePositionReset(transform.player_id, transform.acknowledged_reset_id) &&
            authoritative_reset_ack_sink_) {
          try {
            authoritative_reset_ack_sink_(transform.player_id, transform.acknowledged_reset_id);
          } catch (...) {
            WriteLog("Authoritative position-reset acknowledgment sink threw");
          }
        }
        if (!world_.ApplyAuthoritativeVitals(transform.player_id, transform.health_valid,
                                             transform.alive, transform.health,
                                             transform.max_health))
          (void)world_.ApplyAuthoritativeVitals(transform.player_id, false, false, 0, 0);
        if (!transform.available ||
            !world_.ApplyAuthoritativeTransform(transform.player_id, transform.x_cm, transform.y_cm,
                                                transform.z_cm, transform.yaw_mrad,
                                                transform.owner_pose_authoritative))
          world_.MarkAuthoritativeUnavailable(transform.player_id);
      }
      for (uint16_t old_player_id : authoritative_player_ids_) {
        if (std::find(current_player_ids.begin(), current_player_ids.end(), old_player_id) != current_player_ids.end())
          continue;
        world_.ReleaseAuthoritativeSimulation(old_player_id);
        last_guest_rebase_ids_.erase(old_player_id);
      }
      authoritative_player_ids_ = std::move(current_player_ids);
    } catch (...) {
      transform_source_disabled_ = true;
      for (uint16_t player_id : authoritative_player_ids_) {
        (void)world_.ApplyAuthoritativeVitals(player_id, false, false, 0, 0);
        world_.MarkAuthoritativeUnavailable(player_id);
      }
      WriteLog("Guest transform source threw; source disabled");
    }
  }
  if (transform_source_disabled_)
    for (uint16_t player_id : authoritative_player_ids_) {
      (void)world_.ApplyAuthoritativeVitals(player_id, false, false, 0, 0);
      world_.MarkAuthoritativeUnavailable(player_id);
    }
  if (actor_source_ && !actor_source_disabled_) {
    try {
      authoritative_actors_ = actor_source_();
      if (authoritative_actors_.size() > kMaxSnapshotActors) {
        authoritative_actors_.resize(kMaxSnapshotActors);
        if (!actor_cap_logged_) {
          actor_cap_logged_ = true;
          WriteLog("Guest actor source exceeded the snapshot cap; excess actors are omitted");
        }
      }
    } catch (...) {
      actor_source_disabled_ = true;
      authoritative_actors_.clear();
      WriteLog("Guest actor source threw; source disabled");
    }
  }
  if (mission_actor_source_ && !mission_actor_source_disabled_) {
    try {
      mission_actors_ = mission_actor_source_();
      if (mission_actors_.size() > kMaxSnapshotActors) {
        mission_actors_.resize(kMaxSnapshotActors);
        if (!mission_actor_cap_logged_) {
          mission_actor_cap_logged_ = true;
          WriteLog("Host mission actor source exceeded its cap; excess actors are omitted from Luau");
        }
      }
    } catch (...) {
      mission_actor_source_disabled_ = true;
      mission_actors_.clear();
      WriteLog("Host mission actor source threw; source disabled");
    }
  }
  scripts_.Tick(tick_);
  // Only seats currently mapped to guest player objects require guest reset ACKs.
  // Other connected clients keep the normal client ACK path.
  world_.CancelAuthoritativePositionResetRequirementsExcept(authoritative_player_ids_);
  if (rebase_sink_ && !rebase_sink_disabled_) {
    for (uint16_t player_id : authoritative_player_ids_) {
      if (const auto reset = world_.FindPendingPositionReset(player_id)) {
        const auto last_reset = last_guest_rebase_ids_.find(player_id);
        if (last_reset == last_guest_rebase_ids_.end() || last_reset->second != reset->reset_id) {
        try {
          if (rebase_sink_(reset->player_id, reset->reset_id, reset->x_cm, reset->y_cm,
                           reset->z_cm, reset->yaw_mrad)) {
            last_guest_rebase_ids_[player_id] = reset->reset_id;
          }
        } catch (...) {
          rebase_sink_disabled_ = true;
          WriteLog("Guest position reset sink threw; sink disabled");
        }
        }
      }
    }
  }
}

void ServerRuntime::Stop() {
  if (master_resolution_)
    master_resolution_->cancelled.store(true, std::memory_order_relaxed);
  authoritative_actors_.clear();
  mission_actors_.clear();
  for (uint16_t player_id : authoritative_player_ids_)
    world_.ReleaseAuthoritativeSimulation(player_id);
  authoritative_player_ids_.clear();
  last_guest_rebase_ids_.clear();
  for (auto& [peer, state] : peers_) {
    (void)peer;
    state.disconnecting = true;
  }
  for (auto& [peer, state] : peers_) {
    (void)peer;
    if (!state.player_id) continue;
    const uint16_t id = state.player_id;
    world_.RemovePlayer(id);
    NotifyInput(id, std::nullopt);
    state.player_id = 0;
    scripts_.PlayerLeft(id, state.name);
    WriteLog(state.name + " disconnected because the server stopped");
  }
  if (master_host_) {
    if (master_peer_ && master_peer_->state != ENET_PEER_STATE_DISCONNECTED) enet_peer_disconnect_now(master_peer_, 0);
    master_peer_ = nullptr;
    enet_host_destroy(master_host_);
    master_host_ = nullptr;
  }
  if (host_) {
    for (auto& [peer, state] : peers_) { (void)state; enet_peer_disconnect_now(peer, 0); }
    enet_host_destroy(host_);
    host_ = nullptr;
  }
  peers_.clear();
  discovery_.Close();
  if (enet_started_) { ReleaseENet(); enet_started_ = false; }
  started_ = false;
}

bool ServerRuntime::HeadlessActorMode() const {
  return !actor_source_ && !actor_spawn_sink_ && !actor_damage_sink_ &&
      !mission_actor_source_ && !mission_actor_removal_source_;
}

void ServerRuntime::ServiceMaster() {
  if (config_.master_endpoint.empty()) return;
  const auto now = std::chrono::steady_clock::now();
  if (!master_host_) {
    if (now < master_retry_) return;
    master_host_ = enet_host_create(nullptr, 1, 1, 0, 0);
    if (!master_host_) {
      WriteLog("Could not recreate directory client; retrying");
      master_retry_ = now + std::chrono::seconds(10);
      return;
    }
    master_host_->maximumPacketSize = kMaxPacketBytes;
    master_host_->maximumWaitingData = kMaxPacketBytes * 8;
  }
  // Connect before servicing a freshly created client host. On Windows, the
  // first receive on an unbound UDP socket can fail with WSAEINVAL; ENet binds
  // the socket when it sends the initial connection request.
  if (master_resolution_ && master_resolution_->completed.load(std::memory_order_acquire)) {
    const auto resolution = master_resolution_;
    const bool cancelled = resolution->cancelled.load(std::memory_order_relaxed);
    bool succeeded = false;
    ENetAddress address{};
    std::string resolution_error;
    {
      std::lock_guard lock(resolution->mutex);
      succeeded = resolution->succeeded;
      address = resolution->address;
      resolution_error = resolution->error;
    }
    if (master_resolution_worker_.joinable()) master_resolution_worker_.join();
    master_resolution_.reset();
    if (cancelled) {
      master_retry_ = now + std::chrono::seconds(1);
    } else if (!succeeded) {
      WriteLog("Could not resolve directory host: " + resolution_error);
      master_retry_ = now + std::chrono::seconds(30);
    } else {
      master_peer_ = enet_host_connect(master_host_, &address, 1, kProtocolVersion);
      if (!master_peer_) {
        WriteLog("Could not connect to directory; retrying");
        master_retry_ = now + std::chrono::seconds(10);
      }
    }
  }
  if (!master_peer_ && !master_resolution_ && now >= master_retry_) {
    const std::string endpoint = config_.master_endpoint;
    const auto colon = endpoint.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= endpoint.size()) {
      WriteLog("Invalid directory address; expected host:port");
      master_retry_ = now + std::chrono::seconds(60);
      return;
    }
    const auto port_text = std::string_view(endpoint).substr(colon + 1);
    unsigned port = 0;
    const auto [end, ec] = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    if (ec != std::errc{} || end != port_text.data() + port_text.size() || port < 1 || port > 65535) {
      WriteLog("Invalid directory port; expected host:port");
      master_retry_ = now + std::chrono::seconds(60);
      return;
    }
    const std::string host_name = endpoint.substr(0, colon);
    auto resolution = std::make_shared<MasterResolution>();
    master_resolution_ = resolution;
    try {
      std::thread worker([resolution, host_name, port] {
        ENetAddress address{};
        std::string error;
        bool succeeded = false;
        bool enet_acquired = false;
        try {
          enet_acquired = AcquireENet();
          if (!enet_acquired) {
            error = "could not acquire Winsock/ENet lifetime for directory lookup";
          } else {
            succeeded = ResolveIPv4(host_name, static_cast<uint16_t>(port),
                std::chrono::seconds(5), [resolution] {
                  return resolution->cancelled.load(std::memory_order_relaxed);
                }, address, &error);
          }
        } catch (const std::exception& exception) {
          try { error = std::string("directory lookup failed: ") + exception.what(); } catch (...) {}
        } catch (...) {
          try { error = "directory lookup failed with an unknown error"; } catch (...) {}
        }
        if (enet_acquired) {
          try { ReleaseENet(); } catch (...) {}
        }
        try {
          std::lock_guard lock(resolution->mutex);
          resolution->succeeded = succeeded;
          resolution->address = address;
          resolution->error = std::move(error);
        } catch (...) {}
        resolution->completed.store(true, std::memory_order_release);
      });
      try {
        worker.detach();
      } catch (...) {
        // Retain a joinable worker so the completed result can reap it on a
        // later server-loop pass instead of destroying a joinable thread.
        master_resolution_worker_ = std::move(worker);
      }
      WriteLog("Resolving directory host " + host_name);
    } catch (const std::exception& error) {
      master_resolution_.reset();
      WriteLog(std::string("Could not start directory host lookup: ") + error.what());
      master_retry_ = now + std::chrono::seconds(10);
    }
  }
  ENetEvent event{};
  for (size_t serviced = 0; serviced < 32; ++serviced) {
    const int service_result = enet_host_service(master_host_, &event, 0);
    if (service_result < 0) {
#if defined(_WIN32)
      const int system_socket_error = WSAGetLastError();
#else
      const int system_socket_error = 0;
#endif
      int socket_error = 0;
      const int socket_error_result = enet_socket_get_option(master_host_->socket, ENET_SOCKOPT_ERROR, &socket_error);
      enet_host_destroy(master_host_);
      master_host_ = nullptr;
      master_peer_ = nullptr;
      master_register_ = {};
      master_retry_ = now + std::chrono::seconds(10);
      WriteLog("Directory transport failed" +
          (socket_error_result == 0 ? " (socket error " + std::to_string(socket_error) + ")" : "") +
          (system_socket_error ? " (system error " + std::to_string(system_socket_error) + ")" : "") +
          "; will recreate and retry");
      return;
    }
    if (service_result == 0) break;
    if (event.type == ENET_EVENT_TYPE_CONNECT) {
      master_peer_ = event.peer;
      master_register_ = {};
      RegisterWithMaster();
      WriteLog("Connected to directory " + config_.master_endpoint);
    } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
      DecodedMessage message;
      if (event.packet->dataLength <= kMaxPacketBytes &&
          Decode(std::span(event.packet->data, event.packet->dataLength), message) && message.type == Message::kError)
        WriteLog("Directory: " + message.text);
      enet_packet_destroy(event.packet);
    } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
      if (event.peer == master_peer_) master_peer_ = nullptr;
      master_retry_ = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      WriteLog("Directory connection lost; will retry");
    }
  }
  const auto serviced_at = std::chrono::steady_clock::now();
  if (master_peer_ && master_peer_->state == ENET_PEER_STATE_CONNECTED &&
      (master_register_ == std::chrono::steady_clock::time_point{} || serviced_at - master_register_ >= std::chrono::seconds(20)))
    RegisterWithMaster();
}

void ServerRuntime::RegisterWithMaster() {
  if (!master_host_ || !master_peer_ || master_peer_->state != ENET_PEER_STATE_CONNECTED) return;
  const auto bytes = EncodeDirectoryRegister(Info(), advertised_ipv4_);
  ENetPacket* packet = enet_packet_create(bytes.data(), bytes.size(), ENET_PACKET_FLAG_RELIABLE);
  if (!packet || enet_peer_send(master_peer_, 0, packet) < 0) {
    if (packet) enet_packet_destroy(packet);
    enet_peer_disconnect(master_peer_, 1);
    master_peer_ = nullptr;
    master_retry_ = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  } else {
    enet_host_flush(master_host_);
    master_register_ = std::chrono::steady_clock::now();
    WriteLog("Sent directory registration to " + config_.master_endpoint);
  }
}

ServerInfo ServerRuntime::Info() const {
  ServerInfo info;
  info.name = config_.name;
  info.mission = config_.mission;
  info.game_port = config_.game_port;
  info.players = world_.player_count();
  info.max_players = config_.max_players;
  info.server_authoritative_movement = config_.server_authoritative_movement;
  return info;
}

void ServerRuntime::ServiceEvent(const ENetEvent& event) {
  if (event.type == ENET_EVENT_TYPE_CONNECT) {
    if (event.data != kProtocolVersion) {
      WriteLog("Rejected client with incompatible protocol version");
      enet_peer_disconnect_now(event.peer, 1);
      return;
    }
    const size_t pending_handshakes = std::count_if(peers_.begin(), peers_.end(), [](const auto& entry) {
      return entry.second.stage != PeerStage::kAuthenticated;
    });
    if (pending_handshakes >=
        std::min<size_t>(config_.max_players, kMaxPendingHandshakesPerServer)) {
      WriteLog("Rejected client: pending handshake pool is full");
      enet_peer_disconnect_now(event.peer, 1);
      return;
    }
    auto [it, inserted] = peers_.try_emplace(event.peer);
    (void)inserted;
    it->second.connected_at = std::chrono::steady_clock::now();
    enet_peer_timeout(event.peer, 8, 5'000, 15'000);
      WriteLog("Connection opened; awaiting protocol handshake");
  } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
    Receive(event.peer, event.packet, event.channelID);
    enet_packet_destroy(event.packet);
  } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
    const auto it = peers_.find(event.peer);
    if (it != peers_.end()) {
      if (it->second.player_id) {
        const uint16_t id = it->second.player_id;
        const auto left = EncodePlayerLeft(id, tick_);
        for (const auto& [other_peer, other_state] : peers_) {
          if (other_peer == event.peer || other_state.stage != PeerStage::kAuthenticated ||
              other_state.disconnecting) continue;
          Send(other_peer, left, 0, ENET_PACKET_FLAG_RELIABLE);
        }
        world_.RemovePlayer(id);
        NotifyInput(id, std::nullopt);
        scripts_.PlayerLeft(id, it->second.name);
        WriteLog(it->second.name + " disconnected");
      }
      ClearCryptoKeyPair(it->second.ephemeral_key_pair);
      if (it->second.secure_channel) it->second.secure_channel->Clear();
      peers_.erase(it);
    }
    event.peer->data = nullptr;
  }
}

void ServerRuntime::PruneHandshakeTimeouts(std::chrono::steady_clock::time_point now) {
  for (auto& [peer, state] : peers_) {
    if (state.stage == PeerStage::kAuthenticated || state.disconnecting ||
        now - state.connected_at < std::chrono::seconds(5)) continue;
    state.disconnecting = true;
    WriteLog("Rejected client: handshake timed out");
    enet_peer_disconnect(peer, 9);
  }
}

void ServerRuntime::Receive(ENetPeer* peer, ENetPacket* packet, uint8_t channel) {
  const auto it = peers_.find(peer);
  if (it == peers_.end()) { enet_peer_disconnect(peer, 1); return; }
  auto& state = it->second;
  if (!packet || packet->dataLength > kMaxPacketBytes) {
    state.disconnecting = true;
    if (state.player_id) {
      NotifyInput(state.player_id, std::nullopt);
      state.input_timeout_notified = true;
    }
    enet_peer_disconnect(peer, 1);
    return;
  }
  // Once teardown starts, ignore queued datagrams from that peer. In particular,
  // an unauthenticated client must not revive its handshake while a timeout or
  // protocol rejection is already in progress.
  if (state.disconnecting) return;
  const auto now = std::chrono::steady_clock::now();
  if (state.rate_window == std::chrono::steady_clock::time_point{} || now - state.rate_window >= std::chrono::seconds(1)) {
    state.rate_window = now;
    state.packet_count = 0;
  }
  if (++state.packet_count > 120) { Disconnect(peer, "packet rate limit exceeded"); return; }
  DecodedMessage message;
  std::string error;
  std::vector<uint8_t> plaintext;
  std::span<const uint8_t> message_bytes(packet->data, packet->dataLength);
  if (state.stage != PeerStage::kConnected) {
    if (!state.secure_channel || !state.secure_channel->Open(channel, message_bytes, plaintext)) {
      Disconnect(peer, "encrypted session packet failed authentication");
      return;
    }
    message_bytes = plaintext;
  }
  if (!Decode(message_bytes, message, &error)) {
    Disconnect(peer, "invalid protocol packet");
    return;
  }

  if (channel == 0 && state.stage == PeerStage::kConnected && message.type == Message::kHello) {
    if (message.build_fingerprint != kPortBuildFingerprint) {
      Disconnect(peer, "port build fingerprint mismatch");
      return;
    }
    state.name = message.text;
    state.nonce = RandomSessionId();
    if (!state.nonce) {
      Disconnect(peer, "could not create a handshake challenge nonce");
      return;
    }
    state.client_nonce = message.client_nonce;
    if (!GenerateCryptoKeyPair(state.ephemeral_key_pair) ||
        !GenerateCryptoNonce(state.server_nonce) || !(state.session_id = RandomSessionId())) {
      Disconnect(peer, "could not create authenticated session keys");
      return;
    }
    CryptoSessionKeys keys;
    CryptoNoncePrefix nonce_prefix{};
    CryptoSigningSignature signature{};
    if (!DeriveServerSessionKeys(state.ephemeral_key_pair, message.client_public_key, keys) ||
        !DeriveNoncePrefix(state.client_nonce, state.server_nonce, state.session_id, nonce_prefix) ||
        !SignHandshakeTranscript(identity_, message.client_public_key,
                                 state.ephemeral_key_pair.public_key, state.client_nonce,
                                 state.server_nonce, state.session_id, state.nonce, signature)) {
      ClearCryptoSessionKeys(keys);
      ClearCryptoKeyPair(state.ephemeral_key_pair);
      Disconnect(peer, "server could not authenticate the session handshake");
      return;
    }
    keys.session_id = state.session_id;
    state.secure_channel.emplace(keys, nonce_prefix);
    ClearCryptoSessionKeys(keys);
    const CryptoPublicKey server_public_key = state.ephemeral_key_pair.public_key;
    ClearCryptoKeyPair(state.ephemeral_key_pair);
    state.stage = PeerStage::kChallenged;
    const auto challenge = EncodeChallenge(state.nonce, state.session_id, state.client_nonce,
                                           server_public_key, state.server_nonce,
                                           identity_.public_key, signature);
    sodium_memzero(nonce_prefix.data(), nonce_prefix.size());
    sodium_memzero(signature.data(), signature.size());
    if (challenge.empty() || !SendPlain(peer, challenge, 0, ENET_PACKET_FLAG_RELIABLE)) {
      Disconnect(peer, "could not send authenticated session challenge");
    }
    return;
  }
  if (channel == 0 && state.stage == PeerStage::kChallenged && message.type == Message::kAuthenticate) {
    if (!state.nonce || message.nonce != state.nonce) { Disconnect(peer, "challenge response failed"); return; }
    auto joined = world_.AddPlayer(state.name);
    if (!joined.id) { Send(peer, EncodeError(joined.error), 0, ENET_PACKET_FLAG_RELIABLE); Disconnect(peer, joined.error); return; }
    state.player_id = joined.id;
    state.stage = PeerStage::kAuthenticated;
    state.nonce = 0;
    Send(peer, EncodeWelcome(state.player_id, tick_, config_.server_authoritative_movement),
         0, ENET_PACKET_FLAG_RELIABLE);
    scripts_.PlayerJoined(state.player_id, state.name);
    WriteLog(state.name + " joined as player " + std::to_string(state.player_id));
    return;
  }
  if (state.stage != PeerStage::kAuthenticated) { Disconnect(peer, "handshake required"); return; }
  if (message.type == Message::kInput && channel == 1) {
    const auto previous_state = world_.FindPlayerState(state.player_id);
    if (!world_.SubmitInput(state.player_id, message.input, now)) {
      const auto current_state = world_.FindPlayerState(state.player_id);
      const bool spawn_reset_created = message.input.request_spawn && current_state &&
          current_state->position_reset &&
          (!previous_state || !previous_state->position_reset ||
           previous_state->position_reset_id != current_state->position_reset_id);
      if (spawn_reset_created) {
        state.last_input_at = now;
        state.input_timeout_notified = false;
        NotifyInput(state.player_id, world_.FindPlayerInput(state.player_id));
      }
      return;  // stale or too-frequent input is dropped
    }
    state.last_input_at = now;
    state.input_timeout_notified = false;
    NotifyInput(state.player_id, world_.FindPlayerInput(state.player_id));
  } else if (message.type == Message::kPing && channel == 0) {
    Send(peer, EncodePong(message.tick), 0, ENET_PACKET_FLAG_RELIABLE);
  } else if (message.type == Message::kChat && channel == 0) {
    if (message.text.empty() ||
        (state.last_chat != std::chrono::steady_clock::time_point{} && now - state.last_chat < std::chrono::milliseconds(500)))
      return;
    state.last_chat = now;
    BroadcastChat(state.player_id, std::move(message.text));
  } else if (message.type == Message::kGoodbye && channel == 0) {
    state.disconnecting = true;
    if (state.player_id) NotifyInput(state.player_id, std::nullopt);
    state.input_timeout_notified = true;
    enet_peer_disconnect(peer, 0);
  } else {
    Disconnect(peer, "message is not valid in this state");
  }
}

bool ServerRuntime::Send(ENetPeer* peer, const std::vector<uint8_t>& bytes, uint8_t channel, uint32_t flags) {
  if (!peer || bytes.empty()) return false;
  auto it = peers_.find(peer);
  if (it != peers_.end() && it->second.secure_channel && it->second.secure_channel->ready()) {
    if (bytes.size() > kMaxSecurePayloadBytes) return false;
    const auto encrypted = it->second.secure_channel->Seal(channel, bytes);
    if (encrypted.empty()) return false;
    return SendPlain(peer, encrypted, channel, flags);
  }
  return SendPlain(peer, bytes, channel, flags);
}

bool ServerRuntime::SendPlain(ENetPeer* peer, const std::vector<uint8_t>& bytes,
                              uint8_t channel, uint32_t flags) {
  if (!peer || bytes.empty() || bytes.size() > kMaxPacketBytes) return false;
  ENetPacket* packet = enet_packet_create(bytes.data(), bytes.size(), flags);
  if (!packet || enet_peer_send(peer, channel, packet) < 0) {
    if (packet) enet_packet_destroy(packet);
    if (const auto it = peers_.find(peer); it != peers_.end()) {
      it->second.disconnecting = true;
      if (it->second.player_id) {
        NotifyInput(it->second.player_id, std::nullopt);
        it->second.input_timeout_notified = true;
      }
    }
    enet_peer_disconnect(peer, 2);
    return false;
  }
  return true;
}

void ServerRuntime::BroadcastChat(uint16_t sender, std::string text) {
  const auto now = std::chrono::steady_clock::now();
  // One message fans out reliably to every authenticated peer. Bound aggregate
  // fan-out too: per-peer limits alone still let a full server saturate channel 0.
  if (last_chat_broadcast_ != std::chrono::steady_clock::time_point{} &&
      now - last_chat_broadcast_ < kMinimumChatBroadcastInterval) return;
  const auto bytes = EncodeChat(sender, text);
  if (bytes.empty()) return;
  last_chat_broadcast_ = now;
  for (const auto& [peer, state] : peers_) {
    if (state.stage == PeerStage::kAuthenticated && !state.disconnecting)
      Send(peer, bytes, 0, ENET_PACKET_FLAG_RELIABLE);
  }
  enet_host_flush(host_);
  WriteLog(sender ? "[chat] Player " + std::to_string(sender) + ": " + text : "[script] " + text);
}

void ServerRuntime::NotifyInput(uint16_t player_id, std::optional<InputCommand> input) {
  if (!input_sink_ || input_sink_disabled_) return;
  std::optional<AuthoritativePositionReset> position_reset;
  if (const auto reset = world_.FindPendingPositionReset(player_id)) {
    position_reset = AuthoritativePositionReset{reset->player_id, reset->reset_id,
        reset->x_cm, reset->y_cm, reset->z_cm, reset->yaw_mrad};
  }
  try {
    input_sink_(player_id, std::move(input), position_reset);
  } catch (...) {
    input_sink_disabled_ = true;
    // Best-effort neutral delivery before disabling a broken observer. In
    // particular, do not leave a guest-side held command alive until teardown.
    for (const auto& [peer, state] : peers_) {
      (void)peer;
      if (!state.player_id) continue;
      try { input_sink_(state.player_id, std::nullopt, std::nullopt); } catch (...) {}
    }
    WriteLog("Input observer threw; observer disabled");
  }
}

void ServerRuntime::Disconnect(ENetPeer* peer, std::string_view reason) {
  if (const auto it = peers_.find(peer); it != peers_.end()) {
    it->second.disconnecting = true;
    if (it->second.player_id) {
      NotifyInput(it->second.player_id, std::nullopt);
      it->second.input_timeout_notified = true;
    }
  }
  WriteLog("Rejected client: " + std::string(reason));
  Send(peer, EncodeError(reason), 0, ENET_PACKET_FLAG_RELIABLE);
  enet_peer_disconnect_later(peer, 3);
}

void ServerRuntime::WriteLog(std::string text) const { if (log_) log_(std::move(text)); }

}  // namespace edf::net
