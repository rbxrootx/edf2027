#pragma once

#include "protocol.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace edf::net {

struct PendingPositionReset {
  uint16_t player_id = 0;
  uint32_t reset_id = 0;
  int32_t x_cm = 0;
  int32_t y_cm = 0;
  int32_t z_cm = 0;
  int16_t yaw_mrad = 0;
};

class ServerWorld {
 public:
  struct JoinResult { uint16_t id = 0; std::string error; };

  explicit ServerWorld(uint16_t max_players = 8, int32_t spawn_x_cm = 0,
                       int32_t spawn_y_cm = 0, int32_t spawn_z_cm = 0,
                       bool require_authoritative_reset = false,
                       bool accept_client_transforms = true,
                       uint32_t max_client_transform_speed_cm_per_second = 6'000,
                       bool server_authoritative_vitals = false,
                       uint16_t player_max_health = 100,
                       uint32_t max_client_aim_turn_rate_mrad_per_second = 12'566);
  JoinResult AddPlayer(std::string name);
  bool RemovePlayer(uint16_t id);
  bool RebasePlayerPosition(uint16_t id, int32_t x_cm, int32_t y_cm, int32_t z_cm,
                            int16_t yaw_mrad);
  bool ApplyAuthoritativeTransform(uint16_t id, int32_t x_cm, int32_t y_cm, int32_t z_cm,
                                   int16_t yaw_mrad, bool owner_pose_authoritative = false);
  bool ApplyAuthoritativeVitals(uint16_t id, bool valid, bool alive,
                                uint16_t health, uint16_t max_health);
  bool DamagePlayer(uint16_t id, uint16_t damage_points);
  bool AcknowledgeAuthoritativePositionReset(uint16_t id, uint32_t reset_id);
  void CancelAuthoritativePositionResetRequirementsExcept(const std::vector<uint16_t>& keep_ids);
  bool MarkAuthoritativeUnavailable(uint16_t id);
  bool ReleaseAuthoritativeSimulation(uint16_t id);
  bool SubmitInput(uint16_t id, const InputCommand& command,
                   std::chrono::steady_clock::time_point received = std::chrono::steady_clock::now());
  void Tick();
  WorldSnapshot Snapshot() const;
  WorldSnapshot SnapshotFor(uint16_t observer_id, uint32_t interest_radius_cm,
                            uint32_t presence_sequence = 0) const;
  std::vector<ScriptPlayerView> ScriptPlayers() const;
  std::optional<PlayerState> FindPlayerState(uint16_t id) const;
  std::optional<InputCommand> FindPlayerInput(uint16_t id) const;
  std::optional<PendingPositionReset> FindPendingPositionReset(uint16_t id) const;
  uint16_t player_count() const { return static_cast<uint16_t>(players_.size()); }
  uint16_t max_players() const { return max_players_; }

 private:
  struct Player {
    PlayerState state;
    InputCommand input;
    uint32_t last_sequence = 0;
    bool has_sequence = false;
    bool externally_simulated = false;
    bool owner_pose_authoritative = false;
    bool authoritative_unavailable = false;
    bool client_reset_acknowledged = false;
    bool authoritative_reset_acknowledged = true;
    bool authoritative_reset_required = false;
    std::chrono::steady_clock::time_point last_input{};
    int16_t client_aim_yaw_mrad = 0;
    bool has_client_aim_yaw = false;
    std::chrono::steady_clock::time_point inactive_since{};
    std::chrono::steady_clock::time_point last_spawn_reset{};
    int32_t spawn_x_cm = 0;
    int32_t spawn_y_cm = 0;
    int32_t spawn_z_cm = 0;
    double movement_remainder_x_cm = 0.0;
    double movement_remainder_z_cm = 0.0;
    bool client_transform_anchor_valid = false;
    int32_t client_transform_anchor_x_cm = 0;
    int32_t client_transform_anchor_y_cm = 0;
    int32_t client_transform_anchor_z_cm = 0;
    std::chrono::steady_clock::time_point client_transform_anchor_at{};
  };
  uint16_t max_players_;
  bool require_authoritative_reset_ = false;
  bool accept_client_transforms_ = true;
  bool server_authoritative_vitals_ = false;
  uint32_t max_client_transform_speed_cm_per_second_ = 6'000;
  uint32_t max_client_aim_turn_rate_mrad_per_second_ = 12'566;
  uint16_t player_max_health_ = 100;
  int32_t spawn_x_cm_ = 0;
  int32_t spawn_y_cm_ = 0;
  int32_t spawn_z_cm_ = 0;
  uint16_t next_id_ = 1;
  uint32_t tick_ = 0;
  std::vector<Player> players_;
};

}  // namespace edf::net
