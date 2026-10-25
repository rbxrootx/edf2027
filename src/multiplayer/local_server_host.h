#pragma once

#include "local_guest_input.h"
#include "server_runtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <exception>
#include <set>
#include <string>
#include <thread>
#include <utility>

namespace edf::net {

// Runs the normal server runtime in the game process for one-click LAN hosting.
// The game still connects through ClientSession. The host's own sampled guest
// pose is treated as trusted server state; other clients use the normal server
// validation path.
class LocalServerHost final {
 public:
  using SpawnKey = std::pair<uint32_t, uint32_t>;

  LocalServerHost() = default;
  ~LocalServerHost() { Stop(); }
  LocalServerHost(const LocalServerHost&) = delete;
  LocalServerHost& operator=(const LocalServerHost&) = delete;

  bool Start(std::filesystem::path scripts_directory, std::string* error = nullptr) {
    std::unique_lock lock(mutex_);
    if (running_) {
      if (error) *error = "A local server is already running";
      return false;
    }
    run_generation_.fetch_add(1, std::memory_order_release);
    if (worker_.joinable()) {
      lock.unlock();
      worker_.join();
      lock.lock();
      runtime_.reset();
    }
    guest_transforms_.clear();
    host_player_id_ = remote_guest_transform_id_ = 0;
    pending_remote_guest_reset_.reset();
    guest_input_.Reset();
    ResetServerActors();

    ServerConfig config;
    config.name = "EDF2027 Local Host";
    config.mission = "Mission 1";
    // Until mission-specific EDF spawn points and collision are mapped on the
    // server, preserve each guest's real spawn and movement physics.
    config.server_authoritative_movement = false;
    config.scripts_directory = std::move(scripts_directory);
    config.mission_actor_reconciliation = true;
    stop_requested_.store(false, std::memory_order_relaxed);
    startup_complete_ = false;
    startup_error_.clear();
    runtime_ = std::make_unique<ServerRuntime>(
        std::move(config),
        [this](std::string line) {
          std::lock_guard callback_lock(mutex_);
          if (line.starts_with("EDF2027 multiplayer server listening")) {
            running_ = true;
            startup_complete_ = true;
          } else if (line.starts_with("Server startup failed:")) {
            startup_error_ = std::move(line);
            startup_complete_ = true;
          }
          startup_changed_.notify_all();
      }, [this](uint16_t player_id, std::optional<InputCommand> input,
                std::optional<AuthoritativePositionReset> reset) {
        (void)reset;
        PublishRemoteGuestInput(player_id, std::move(input));
      },
        [this] { return CopyHostTransforms(); },
        [this](uint16_t player_id, uint32_t reset_id, int32_t x_cm, int32_t y_cm,
               int32_t z_cm, int16_t yaw_mrad) {
          return ConfirmHostPositionReset(player_id, reset_id, x_cm, y_cm, z_cm, yaw_mrad);
        }, [this] { return CopyServerActors(); },
        [this](const ActorSpawnRequest& spawn) { return QueueServerActorSpawn(spawn); },
        [this](uint32_t actor_id, float damage) {
          return actor_id & kMissionActorIdNamespace
              ? QueueMissionActorDamage(actor_id, damage)
              : QueueServerActorDamage(actor_id, damage);
        },
        [this] { return CopyMissionActors(); },
        [this] { return CopyMissionActorRemovals(); },
        [this](uint16_t player_id, uint32_t reset_id) {
          AcknowledgeRemoteGuestPositionReset(player_id, reset_id);
        });

    try {
      worker_ = std::thread([this] {
        int result = 0;
        try {
          result = runtime_->Run(stop_requested_);
        } catch (const std::exception& failure) {
          std::lock_guard worker_lock(mutex_);
          running_ = false;
          startup_error_ = std::string("Local server crashed: ") + failure.what();
          startup_complete_ = true;
          startup_changed_.notify_all();
          return;
        } catch (...) {
          std::lock_guard worker_lock(mutex_);
          running_ = false;
          startup_error_ = "Local server crashed with an unknown exception";
          startup_complete_ = true;
          startup_changed_.notify_all();
          return;
        }
        std::lock_guard worker_lock(mutex_);
        running_ = false;
        if (!startup_complete_) {
          startup_error_ = result ? "Local server exited during startup" : "Local server stopped during startup";
          startup_complete_ = true;
        }
        startup_changed_.notify_all();
      });
    } catch (const std::exception& failure) {
      runtime_.reset();
      if (error) *error = std::string("Could not start local server thread: ") + failure.what();
      return false;
    }

    if (!startup_changed_.wait_for(lock, std::chrono::seconds(5), [this] { return startup_complete_; })) {
      stop_requested_.store(true, std::memory_order_relaxed);
      lock.unlock();
      if (worker_.joinable()) worker_.join();
      lock.lock();
      runtime_.reset();
      if (error) *error = "Timed out while starting the local server";
      return false;
    }
    if (!running_) {
      const std::string failure = startup_error_.empty() ? "Local server failed to start" : startup_error_;
      lock.unlock();
      if (worker_.joinable()) worker_.join();
      lock.lock();
      runtime_.reset();
      if (error) *error = failure;
      return false;
    }
    return true;
  }

  void Stop() {
    stop_requested_.store(true, std::memory_order_relaxed);
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) worker_.join();
    std::lock_guard lock(mutex_);
    runtime_.reset();
    guest_transforms_.clear();
    host_player_id_ = remote_guest_transform_id_ = 0;
    pending_remote_guest_reset_.reset();
    running_ = false;
    startup_complete_ = false;
    guest_input_.Reset();
    ResetServerActors();
    {
      std::lock_guard mission_lock(mission_mutex_);
      published_trace_generation_ = 0;
      published_mission_actors_.clear();
      mission_actor_ids_.clear();
      mission_actor_keys_by_id_.clear();
      missing_samples_.clear();
      removed_keys_.clear();
      mission_actor_removals_ = {};
      pending_mission_damage_.clear();
    }
  }

  bool running() const {
    std::lock_guard lock(mutex_);
    return running_;
  }

  uint64_t run_generation() const {
    return run_generation_.load(std::memory_order_acquire);
  }

  // The in-process client uses EDF's first player seat. Keep its authenticated
  // network input out of the second-seat mailbox when F5 runs a local server.
  void SetLocalPlayerId(uint16_t player_id) { guest_input_.SetLocalPlayerId(player_id); }

  bool HasRemoteGuestSeat() const { return guest_input_.HasRemoteGuestSeat(); }

  uint16_t RemoteGuestPlayerId() const { return guest_input_.RemoteGuestPlayerId(); }

  RemoteGuestIdentity PendingRemoteGuestIdentity() const {
    return guest_input_.PendingRemoteGuestIdentity();
  }

  bool SetRemoteGuestSeatReady(RemoteGuestIdentity expected, bool ready) {
    return guest_input_.SetRemoteGuestSeatReady(expected, ready);
  }

  // The guest input hook reads a copied command only. Network callbacks never
  // touch guest memory, and stale input is neutralized after 300 ms.
  std::optional<InputCommand> RemoteGuestInput() const { return guest_input_.Read(); }

  std::optional<AuthoritativePositionReset> PendingRemoteGuestReset() const {
    std::lock_guard lock(mutex_);
    return pending_remote_guest_reset_;
  }

  // Called only on the game thread. ServerRuntime receives this copied pose on
  // its worker thread, never a guest pointer or GuestMemory reference.
  void PublishHostTransform(AuthoritativePlayerTransform transform) {
    std::lock_guard lock(mutex_);
    if (!running_ || !transform.player_id) {
      if (host_player_id_) guest_transforms_.erase(host_player_id_);
      host_player_id_ = 0;
      return;
    }
    if (host_player_id_ && host_player_id_ != transform.player_id)
      guest_transforms_.erase(host_player_id_);
    host_player_id_ = transform.player_id;
    guest_transforms_[host_player_id_] = transform;
  }

  void ClearHostTransform() {
    std::lock_guard lock(mutex_);
    if (host_player_id_) guest_transforms_.erase(host_player_id_);
    host_player_id_ = 0;
  }

  void PublishRemoteGuestTransform(AuthoritativePlayerTransform transform) {
    std::lock_guard lock(mutex_);
    if (!running_ || !transform.player_id) {
      if (remote_guest_transform_id_) guest_transforms_.erase(remote_guest_transform_id_);
      remote_guest_transform_id_ = 0;
      pending_remote_guest_reset_.reset();
      return;
    }
    if (remote_guest_transform_id_ && remote_guest_transform_id_ != transform.player_id)
      guest_transforms_.erase(remote_guest_transform_id_);
    if (remote_guest_transform_id_ && remote_guest_transform_id_ != transform.player_id)
      pending_remote_guest_reset_.reset();
    remote_guest_transform_id_ = transform.player_id;
    transform.owner_pose_authoritative = true;
    guest_transforms_[remote_guest_transform_id_] = transform;
  }

  void ClearRemoteGuestTransform() {
    std::lock_guard lock(mutex_);
    if (remote_guest_transform_id_) guest_transforms_.erase(remote_guest_transform_id_);
    remote_guest_transform_id_ = 0;
    pending_remote_guest_reset_.reset();
  }

  // Called only on the game thread. Copies contain no guest pointers and are
  // consumed independently by the server's network worker.
  void PublishMissionActors(uint64_t trace_generation, std::vector<ActorState> actors) {
    std::lock_guard lock(mission_mutex_);
    if (trace_generation != published_trace_generation_) {
      published_trace_generation_ = trace_generation;
      published_mission_actors_.clear();
      mission_actor_ids_.clear();
      mission_actor_keys_by_id_.clear();
      missing_samples_.clear();
      removed_keys_.clear();
      pending_mission_damage_.clear();
      mission_actor_removals_ = {};
      mission_actor_removals_.host_trace_generation = trace_generation;
    }

    if (!trace_generation) {
      published_mission_actors_.clear();
      return;
    }

    std::map<SpawnKey, ActorState> current;
    for (ActorState& actor : actors) {
      if (!actor.trace_identity_valid || actor.trace_generation != trace_generation ||
          actor.archetype != ActorArchetype::kAnt || !actor.health ||
          actor.trace_call_ordinal >= 4096 || actor.trace_ant_ordinal >= 64) continue;
      const SpawnKey key{actor.trace_call_ordinal, actor.trace_ant_ordinal};
      current.try_emplace(key, std::move(actor));
    }

    published_mission_actors_.clear();
    published_mission_actors_.reserve(std::min(current.size(), kMaxSnapshotActors));
    std::set<SpawnKey> live_keys;
    for (auto& [key, actor] : current) {
      if (removed_keys_.contains(key)) continue;
      auto identity = mission_actor_ids_.find(key);
      if (identity == mission_actor_ids_.end()) {
        constexpr size_t kMaximumMissionIdentities = 4096;
        if (mission_actor_ids_.size() >= kMaximumMissionIdentities ||
            !next_mission_actor_serial_ ||
            next_mission_actor_serial_ >= kMissionActorIdNamespace) continue;
        const uint32_t id = kMissionActorIdNamespace | next_mission_actor_serial_++;
        identity = mission_actor_ids_.emplace(key, id).first;
        mission_actor_keys_by_id_.emplace(id, key);
      }
      actor.id = identity->second;
      live_keys.insert(key);
      missing_samples_.erase(key);
      if (published_mission_actors_.size() < kMaxSnapshotActors)
        published_mission_actors_.push_back(std::move(actor));
    }

    // A rotating 48-actor sample revisits every identity within 86 game ticks
    // at the 4,096-identity bound. Leave a margin so an omitted live actor is
    // not mistaken for a removal before its next sample arrives.
    constexpr uint8_t kMissingSamplesBeforeRemoval = 120;
    for (const auto& [key, id] : mission_actor_ids_) {
      if (live_keys.contains(key) || removed_keys_.contains(key)) continue;
      uint8_t& missing = missing_samples_[key];
      if (missing < kMissingSamplesBeforeRemoval) ++missing;
      if (missing < kMissingSamplesBeforeRemoval ||
          mission_actor_removals_.removals.size() >= kMaxMissionActorRemovalHistory) continue;
      mission_actor_removals_.removals.push_back(
          {id, key.first, key.second});
      mission_actor_removals_.revision =
          static_cast<uint32_t>(mission_actor_removals_.removals.size());
      removed_keys_.insert(key);
      for (auto request = pending_mission_damage_.begin();
           request != pending_mission_damage_.end();) {
        if (request->trace_generation == trace_generation &&
            request->call_ordinal == key.first && request->ant_ordinal == key.second)
          request = pending_mission_damage_.erase(request);
        else
          ++request;
      }
    }
  }

  std::vector<MissionActorDamageRequest> TakeMissionActorDamageRequests() {
    std::lock_guard lock(mission_mutex_);
    std::vector<MissionActorDamageRequest> requests;
    requests.reserve(pending_mission_damage_.size());
    while (!pending_mission_damage_.empty()) {
      requests.push_back(pending_mission_damage_.front());
      pending_mission_damage_.pop_front();
    }
    return requests;
  }

  // Called on the game thread; snapshots contain copied state, never guest pointers.
  void SetServerActorWorldGeneration(uint64_t mission_generation) {
    std::lock_guard lock(actor_mutex_);
    actor_world_ready_ = mission_generation != 0;
    if (mission_generation != server_actor_generation_) {
      server_actor_generation_ = mission_generation;
      published_server_actors_.clear();
      server_actor_live_ids_.clear();
      pending_server_actor_damage_.clear();
      pending_server_actor_spawns_.clear();
      inflight_server_actor_spawns_.clear();
    }
  }

  void PublishServerActors(uint64_t mission_generation, std::vector<ActorState> actors) {
    SetServerActorWorldGeneration(mission_generation);
    std::lock_guard lock(actor_mutex_);
    std::set<uint32_t> live;
    for (ActorState& actor : actors) {
      if (!actor.id || (actor.id & kMissionActorIdNamespace) ||
          actor.archetype != ActorArchetype::kAnt || !actor.health) continue;
      actor.server_owned = true;
      live.insert(actor.id);
    }
    published_server_actors_ = std::move(actors);
    server_actor_live_ids_ = std::move(live);
    for (auto it = pending_server_actor_damage_.begin(); it != pending_server_actor_damage_.end();) {
      if (!server_actor_live_ids_.contains(it->actor_id)) it = pending_server_actor_damage_.erase(it);
      else ++it;
    }
  }

  std::vector<ActorSpawnRequest> TakeServerActorSpawnRequests(size_t maximum = 4) {
    std::lock_guard lock(actor_mutex_);
    std::vector<ActorSpawnRequest> requests;
    const size_t count = std::min(maximum, pending_server_actor_spawns_.size());
    requests.reserve(count);
    while (requests.size() < count) {
      requests.push_back(pending_server_actor_spawns_.front());
      inflight_server_actor_spawns_.insert(pending_server_actor_spawns_.front().id);
      pending_server_actor_spawns_.pop_front();
    }
    return requests;
  }

  void CompleteServerActorSpawns(const std::vector<ActorSpawnRequest>& requests,
                                 const std::vector<uint32_t>& successful_ids) {
    const std::set<uint32_t> successful(successful_ids.begin(), successful_ids.end());
    std::lock_guard lock(actor_mutex_);
    for (const auto& request : requests) {
      inflight_server_actor_spawns_.erase(request.id);
      if (successful.contains(request.id) && std::any_of(published_server_actors_.begin(),
          published_server_actors_.end(), [&request](const ActorState& actor) {
            return actor.id == request.id && actor.health > 0;
          })) server_actor_live_ids_.insert(request.id);
    }
  }

  std::vector<ActorDamageRequest> TakeServerActorDamageRequests(size_t maximum = 16) {
    std::lock_guard lock(actor_mutex_);
    std::vector<ActorDamageRequest> requests;
    const size_t count = std::min(maximum, pending_server_actor_damage_.size());
    requests.reserve(count);
    while (requests.size() < count) {
      requests.push_back(pending_server_actor_damage_.front());
      pending_server_actor_damage_.pop_front();
    }
    return requests;
  }

 private:
  void PublishRemoteGuestInput(uint16_t player_id, std::optional<InputCommand> input) {
    guest_input_.Publish(player_id, std::move(input));
  }

  std::vector<AuthoritativePlayerTransform> CopyHostTransforms() const {
    std::lock_guard lock(mutex_);
    if (!running_) return {};
    std::vector<AuthoritativePlayerTransform> transforms;
    transforms.reserve(guest_transforms_.size());
    for (const auto& [id, transform] : guest_transforms_) {
      (void)id;
      transforms.push_back(transform);
    }
    return transforms;
  }

  bool ConfirmHostPositionReset(uint16_t player_id, uint32_t reset_id,
                                int32_t x_cm, int32_t y_cm, int32_t z_cm,
                                int16_t yaw_mrad) {
    if (!player_id || !reset_id || x_cm < -1'000'000 || x_cm > 1'000'000 ||
        y_cm < -1'000'000 || y_cm > 1'000'000 ||
        z_cm < -1'000'000 || z_cm > 1'000'000 ||
        yaw_mrad < -3142 || yaw_mrad > 3142) return false;
    std::lock_guard lock(mutex_);
    if (!running_ || !guest_transforms_.contains(player_id)) return false;
    // The owner receives its reset in its own snapshot. Route a second copy
    // through this mailbox so the F5 host can apply it to EDF's native seat 1
    // and acknowledge only after the game thread confirms the pose.
    if (player_id == remote_guest_transform_id_) {
      pending_remote_guest_reset_ = AuthoritativePositionReset{
          player_id, reset_id, x_cm, y_cm, z_cm, yaw_mrad};
    }
    return true;
  }

  void AcknowledgeRemoteGuestPositionReset(uint16_t player_id, uint32_t reset_id) {
    std::lock_guard lock(mutex_);
    if (pending_remote_guest_reset_ && pending_remote_guest_reset_->player_id == player_id &&
        pending_remote_guest_reset_->reset_id == reset_id)
      pending_remote_guest_reset_.reset();
  }

  std::vector<ActorState> CopyMissionActors() const {
    std::lock_guard lock(mission_mutex_);
    return published_mission_actors_;
  }

  std::vector<ActorState> CopyServerActors() const {
    std::lock_guard lock(actor_mutex_);
    return published_server_actors_;
  }

  bool QueueServerActorSpawn(const ActorSpawnRequest& spawn) {
    if (!spawn.id || (spawn.id & kMissionActorIdNamespace) ||
        spawn.archetype != ActorArchetype::kAnt ||
        spawn.x_cm < -1'000'000 || spawn.x_cm > 1'000'000 ||
        spawn.y_cm < -1'000'000 || spawn.y_cm > 1'000'000 ||
        spawn.z_cm < -1'000'000 || spawn.z_cm > 1'000'000 ||
        spawn.yaw_mrad < -3142 || spawn.yaw_mrad > 3142) return false;
    std::lock_guard lock(actor_mutex_);
    const size_t reserved_live = std::max(published_server_actors_.size(), server_actor_live_ids_.size());
    if (!actor_world_ready_ || reserved_live +
            pending_server_actor_spawns_.size() + inflight_server_actor_spawns_.size() >=
                kMaxSnapshotActors || server_actor_live_ids_.contains(spawn.id) ||
        inflight_server_actor_spawns_.contains(spawn.id) ||
        std::any_of(pending_server_actor_spawns_.begin(), pending_server_actor_spawns_.end(),
            [&spawn](const ActorSpawnRequest& queued) { return queued.id == spawn.id; })) return false;
    pending_server_actor_spawns_.push_back(spawn);
    return true;
  }

  bool QueueServerActorDamage(uint32_t actor_id, float damage_points) {
    if (!actor_id || !std::isfinite(damage_points) || damage_points <= 0.0f ||
        damage_points > 65535.0f) return false;
    std::lock_guard lock(actor_mutex_);
    if (pending_server_actor_damage_.size() >= 64 ||
        !server_actor_live_ids_.contains(actor_id)) return false;
    const auto live = std::find_if(published_server_actors_.begin(), published_server_actors_.end(),
        [actor_id](const ActorState& actor) { return actor.id == actor_id && actor.health > 0; });
    if (live == published_server_actors_.end()) return false;
    pending_server_actor_damage_.push_back({actor_id, damage_points});
    return true;
  }

  void ResetServerActors() {
    std::lock_guard lock(actor_mutex_);
    actor_world_ready_ = false;
    server_actor_generation_ = 0;
    published_server_actors_.clear();
    server_actor_live_ids_.clear();
    pending_server_actor_spawns_.clear();
    inflight_server_actor_spawns_.clear();
    pending_server_actor_damage_.clear();
  }

  MissionActorRemovalSnapshot CopyMissionActorRemovals() const {
    std::lock_guard lock(mission_mutex_);
    return mission_actor_removals_;
  }

  bool QueueMissionActorDamage(uint32_t actor_id, float damage_points) {
    if (!actor_id || !std::isfinite(damage_points) || damage_points <= 0.0f ||
        damage_points > 65535.0f) return false;
    std::lock_guard lock(mission_mutex_);
    if (pending_mission_damage_.size() >= 64) return false;
    const auto key = mission_actor_keys_by_id_.find(actor_id);
    if (key == mission_actor_keys_by_id_.end() || removed_keys_.contains(key->second)) return false;
    const auto live = std::find_if(published_mission_actors_.begin(), published_mission_actors_.end(),
        [actor_id](const ActorState& actor) { return actor.id == actor_id && actor.health > 0; });
    if (live == published_mission_actors_.end() ||
        live->trace_generation != published_trace_generation_) return false;
    pending_mission_damage_.push_back({published_trace_generation_, key->second.first,
        key->second.second, damage_points});
    return true;
  }

  mutable std::mutex mutex_;
  mutable std::mutex mission_mutex_;
  mutable std::mutex actor_mutex_;
  std::condition_variable startup_changed_;
  std::unique_ptr<ServerRuntime> runtime_;
  std::thread worker_;
  std::atomic<bool> stop_requested_{false};
  std::atomic<uint64_t> run_generation_{0};
  bool startup_complete_ = false;
  bool running_ = false;
  std::string startup_error_;
  std::map<uint16_t, AuthoritativePlayerTransform> guest_transforms_;
  uint16_t host_player_id_ = 0;
  uint16_t remote_guest_transform_id_ = 0;
  std::optional<AuthoritativePositionReset> pending_remote_guest_reset_;
  LocalHostGuestInputMailbox guest_input_;
  uint64_t published_trace_generation_ = 0;
  uint32_t next_mission_actor_serial_ = 1;
  std::vector<ActorState> published_mission_actors_;
  std::map<SpawnKey, uint32_t> mission_actor_ids_;
  std::map<uint32_t, SpawnKey> mission_actor_keys_by_id_;
  std::map<SpawnKey, uint8_t> missing_samples_;
  std::set<SpawnKey> removed_keys_;
  MissionActorRemovalSnapshot mission_actor_removals_;
  std::deque<MissionActorDamageRequest> pending_mission_damage_;
  uint64_t server_actor_generation_ = 0;
  bool actor_world_ready_ = false;
  std::vector<ActorState> published_server_actors_;
  std::set<uint32_t> server_actor_live_ids_;
  std::deque<ActorSpawnRequest> pending_server_actor_spawns_;
  std::set<uint32_t> inflight_server_actor_spawns_;
  std::deque<ActorDamageRequest> pending_server_actor_damage_;
};

}  // namespace edf::net

namespace edf {
struct LocalHostEngineBridge {
  std::mutex mutex;
  net::LocalServerHost* host = nullptr;
};

inline std::shared_ptr<LocalHostEngineBridge>& GetLocalHostEngineBridge() {
  static auto bridge = std::make_shared<LocalHostEngineBridge>();
  return bridge;
}
}  // namespace edf
