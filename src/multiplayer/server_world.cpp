#include "server_world.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace edf::net {
namespace {

bool IsNewerInputSequence(uint32_t sequence, uint32_t previous) {
  // Compare modulo 2^32. A distance in the forward half of the sequence space
  // is newer; the exact half-range is ambiguous and must not replace state.
  const uint32_t distance = sequence - previous;
  return distance != 0 && distance < (uint32_t{1} << 31);
}

}  // namespace

ServerWorld::ServerWorld(uint16_t max_players, int32_t spawn_x_cm,
                         int32_t spawn_y_cm, int32_t spawn_z_cm,
                         bool require_authoritative_reset,
                         bool accept_client_transforms,
                         uint32_t max_client_transform_speed_cm_per_second,
                         bool server_authoritative_vitals,
                         uint16_t player_max_health,
                         uint32_t max_client_aim_turn_rate_mrad_per_second)
    : max_players_(std::clamp<uint16_t>(max_players, 1, kMaxServerPlayers)),
      require_authoritative_reset_(require_authoritative_reset),
      accept_client_transforms_(accept_client_transforms),
      server_authoritative_vitals_(server_authoritative_vitals),
      max_client_transform_speed_cm_per_second_(
          std::clamp<uint32_t>(max_client_transform_speed_cm_per_second, 100, 100'000)),
      max_client_aim_turn_rate_mrad_per_second_(
          std::clamp<uint32_t>(max_client_aim_turn_rate_mrad_per_second, 1'570, 62'832)),
      player_max_health_(std::clamp<uint16_t>(player_max_health, 1, 65535)),
      spawn_x_cm_(std::clamp(spawn_x_cm, -1'000'000, 1'000'000)),
      spawn_y_cm_(std::clamp(spawn_y_cm, -1'000'000, 1'000'000)),
      spawn_z_cm_(std::clamp(spawn_z_cm, -1'000'000, 1'000'000)) {}

ServerWorld::JoinResult ServerWorld::AddPlayer(std::string name) {
  if (players_.size() >= max_players_) return {0, "server is full"};
  if (name.empty() || name.size() > kMaxPlayerNameBytes ||
      name.find_first_of("\r\n") != std::string::npos || name.find('\0') != std::string::npos)
    return {0, "invalid player name"};
  for (char& c : name) if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) == 0x7f) c = '_';
  uint16_t id = next_id_;
  for (;;) {
    if (!id) id = 1;
    const bool in_use = std::any_of(players_.begin(), players_.end(), [=](const Player& player) {
      return player.state.id == id;
    });
    next_id_ = static_cast<uint16_t>(id + 1);
    if (!next_id_) next_id_ = 1;
    if (!in_use) break;
    id = next_id_;
  }
  Player player;
  player.state.id = id;
  player.state.name = std::move(name);
  player.inactive_since = std::chrono::steady_clock::now();
  // Use the host-configured mission spawn and spread nearby joins slightly.
  player.spawn_x_cm = std::clamp(spawn_x_cm_ + int32_t(players_.size() % 4) * 150,
                                 -1'000'000, 1'000'000);
  player.spawn_y_cm = spawn_y_cm_;
  player.spawn_z_cm = std::clamp(spawn_z_cm_ + int32_t(players_.size() / 4) * 150,
                                 -1'000'000, 1'000'000);
  player.state.x_cm = player.spawn_x_cm;
  player.state.y_cm = player.spawn_y_cm;
  player.state.z_cm = player.spawn_z_cm;
  if (server_authoritative_vitals_) {
    // Health belongs to the server session, not to the client's claim that it
    // has entered gameplay. Otherwise an authenticated peer can stay alive
    // with unknown health simply by reporting itself inactive.
    player.state.health_valid = true;
    player.state.alive = true;
    player.state.health = player_max_health_;
    player.state.max_health = player_max_health_;
  }
  if (!accept_client_transforms_) {
    player.state.position_reset = true;
    player.state.position_reset_id = 1;
    player.client_reset_acknowledged = false;
  }
  players_.push_back(std::move(player));
  return {id, {}};
}

bool ServerWorld::RemovePlayer(uint16_t id) {
  const auto it = std::find_if(players_.begin(), players_.end(), [=](const Player& p) { return p.state.id == id; });
  if (it == players_.end()) return false;
  players_.erase(it);
  return true;
}

bool ServerWorld::RebasePlayerPosition(uint16_t id, int32_t x_cm, int32_t y_cm, int32_t z_cm,
                                       int16_t yaw_mrad) {
  if (x_cm < -1'000'000 || x_cm > 1'000'000 ||
      y_cm < -1'000'000 || y_cm > 1'000'000 ||
      z_cm < -1'000'000 || z_cm > 1'000'000 ||
      yaw_mrad < -3142 || yaw_mrad > 3142) return false;
  const auto it = std::find_if(players_.begin(), players_.end(), [=](const Player& p) { return p.state.id == id; });
  if (it == players_.end()) return false;

  // Only approved server coordinates can be applied here. Trusted Luau scripts
  // provide them directly; client spawn requests select the player's fixed
  // host-configured spawn. A guest must apply and acknowledge this generation
  // before becoming active again.
  it->state.x_cm = x_cm;
  it->state.y_cm = y_cm;
  it->state.z_cm = z_cm;
  it->state.yaw_mrad = yaw_mrad;
  it->state.active = false;
  it->state.position_reset = true;
  if (++it->state.position_reset_id == 0) ++it->state.position_reset_id;
  it->client_reset_acknowledged = false;
  it->authoritative_reset_required = require_authoritative_reset_;
  it->authoritative_reset_acknowledged = !it->authoritative_reset_required;
  it->inactive_since = std::chrono::steady_clock::now();
  it->state.buttons = 0;
  it->state.left_trigger = it->state.right_trigger = 0;
  it->input = {};
  it->movement_remainder_x_cm = 0.0;
  it->movement_remainder_z_cm = 0.0;
  it->client_transform_anchor_valid = true;
  it->client_transform_anchor_x_cm = x_cm;
  it->client_transform_anchor_y_cm = y_cm;
  it->client_transform_anchor_z_cm = z_cm;
  it->client_transform_anchor_at = std::chrono::steady_clock::now();
  return true;
}

bool ServerWorld::ApplyAuthoritativeTransform(uint16_t id, int32_t x_cm, int32_t y_cm, int32_t z_cm,
                                              int16_t yaw_mrad, bool owner_pose_authoritative) {
  if (x_cm < -1'000'000 || x_cm > 1'000'000 ||
      y_cm < -1'000'000 || y_cm > 1'000'000 ||
      z_cm < -1'000'000 || z_cm > 1'000'000 ||
      yaw_mrad < -3142 || yaw_mrad > 3142) return false;
  const auto it = std::find_if(players_.begin(), players_.end(), [=](const Player& player) {
    return player.state.id == id;
  });
  if (it == players_.end()) return false;
  if (!it->state.position_reset) {
    it->state.x_cm = x_cm;
    it->state.y_cm = y_cm;
    it->state.z_cm = z_cm;
    it->state.yaw_mrad = yaw_mrad;
    // Guest-backed host samples are already authoritative. Preserve the latest
    // host pose as the handoff anchor if guest simulation is later released.
    it->client_transform_anchor_valid = true;
    it->client_transform_anchor_x_cm = x_cm;
    it->client_transform_anchor_y_cm = y_cm;
    it->client_transform_anchor_z_cm = z_cm;
    it->client_transform_anchor_at = std::chrono::steady_clock::now();
  }
  it->externally_simulated = true;
  it->owner_pose_authoritative = owner_pose_authoritative;
  it->authoritative_unavailable = false;
  if (it->state.position_reset && require_authoritative_reset_ && !it->authoritative_reset_required) {
    it->authoritative_reset_required = true;
    it->authoritative_reset_acknowledged = false;
  }
  const auto now = std::chrono::steady_clock::now();
  const bool fresh_input = it->last_input != std::chrono::steady_clock::time_point{} &&
      now - it->last_input <= std::chrono::milliseconds(250);
  const bool trusted_vitals_allow_activity = !it->state.health_valid || it->state.alive;
  if (!it->state.position_reset && fresh_input && trusted_vitals_allow_activity) {
    it->state.active = true;
    it->inactive_since = {};
  } else {
    if (it->state.active || it->inactive_since == std::chrono::steady_clock::time_point{})
      it->inactive_since = now;
    it->state.active = false;
    it->state.buttons = 0;
    it->state.left_trigger = it->state.right_trigger = 0;
  }
  it->movement_remainder_x_cm = it->movement_remainder_z_cm = 0.0;
  return true;
}

bool ServerWorld::ApplyAuthoritativeVitals(uint16_t id, bool valid, bool alive,
                                           uint16_t health, uint16_t max_health) {
  if (valid && (!max_health || health > max_health || alive != (health > 0))) return false;
  const auto it = std::find_if(players_.begin(), players_.end(), [=](const Player& player) {
    return player.state.id == id;
  });
  if (it == players_.end()) return false;
  it->state.health_valid = valid;
  it->state.alive = valid && alive;
  it->state.health = valid ? health : 0;
  it->state.max_health = valid ? max_health : 0;
  if (valid && !alive) {
    if (it->state.active || it->inactive_since == std::chrono::steady_clock::time_point{})
      it->inactive_since = std::chrono::steady_clock::now();
    it->state.active = false;
    it->state.buttons = 0;
    it->state.left_trigger = it->state.right_trigger = 0;
    it->input.buttons = 0;
    it->input.left_trigger = it->input.right_trigger = 0;
    it->input.move_x = it->input.move_y = it->input.aim_x = it->input.aim_y = 0;
    it->input.transform_valid = false;
    it->input.x_cm = it->input.y_cm = it->input.z_cm = 0;
  }
  return true;
}

bool ServerWorld::DamagePlayer(uint16_t id, uint16_t damage_points) {
  if (!server_authoritative_vitals_ || !damage_points) return false;
  const auto it = std::find_if(players_.begin(), players_.end(), [=](const Player& player) {
    return player.state.id == id;
  });
  if (it == players_.end() || !it->state.health_valid || !it->state.alive || !it->state.health)
    return false;
  const uint16_t remaining = damage_points >= it->state.health
      ? 0 : static_cast<uint16_t>(it->state.health - damage_points);
  it->state.health = remaining;
  it->state.alive = remaining != 0;
  if (!remaining) {
    it->state.active = false;
    it->state.buttons = 0;
    it->state.left_trigger = it->state.right_trigger = 0;
    it->input = {};
    it->movement_remainder_x_cm = it->movement_remainder_z_cm = 0.0;
    it->inactive_since = std::chrono::steady_clock::now();
  }
  return true;
}

bool ServerWorld::AcknowledgeAuthoritativePositionReset(uint16_t id, uint32_t reset_id) {
  const auto it = std::find_if(players_.begin(), players_.end(), [=](const Player& player) {
    return player.state.id == id;
  });
  if (it == players_.end() || !reset_id || !it->state.position_reset ||
      it->state.position_reset_id != reset_id || !it->authoritative_reset_required) return false;
  it->authoritative_reset_acknowledged = true;
  if (it->client_reset_acknowledged) it->state.position_reset = false;
  return true;
}

void ServerWorld::CancelAuthoritativePositionResetRequirementsExcept(const std::vector<uint16_t>& keep_ids) {
  for (auto& player : players_) {
    if (std::find(keep_ids.begin(), keep_ids.end(), player.state.id) != keep_ids.end() ||
        !player.state.position_reset ||
        !player.authoritative_reset_required) continue;
    player.authoritative_reset_required = false;
    player.authoritative_reset_acknowledged = true;
    if (player.client_reset_acknowledged) player.state.position_reset = false;
  }
}

bool ServerWorld::MarkAuthoritativeUnavailable(uint16_t id) {
  const auto it = std::find_if(players_.begin(), players_.end(), [=](const Player& player) {
    return player.state.id == id;
  });
  if (it == players_.end()) return false;
  it->externally_simulated = true;
  if (it->state.position_reset && require_authoritative_reset_ && !it->authoritative_reset_required) {
    it->authoritative_reset_required = true;
    it->authoritative_reset_acknowledged = false;
  }
  if (!it->authoritative_unavailable)
    it->inactive_since = std::chrono::steady_clock::now();
  it->authoritative_unavailable = true;
  it->state.active = false;
  it->state.buttons = 0;
  it->state.left_trigger = it->state.right_trigger = 0;
  return true;
}

bool ServerWorld::ReleaseAuthoritativeSimulation(uint16_t id) {
  const auto it = std::find_if(players_.begin(), players_.end(), [=](const Player& player) {
    return player.state.id == id;
  });
  if (it == players_.end()) return false;
  const bool was_active = it->state.active;
  it->externally_simulated = false;
  it->owner_pose_authoritative = false;
  it->authoritative_unavailable = false;
  it->state.health_valid = false;
  it->state.alive = false;
  it->state.health = it->state.max_health = 0;
  it->state.active = false;
  it->input = {};
  it->state.buttons = 0;
  it->state.left_trigger = it->state.right_trigger = 0;
  if (was_active || it->inactive_since == std::chrono::steady_clock::time_point{})
    it->inactive_since = std::chrono::steady_clock::now();
  return true;
}

bool ServerWorld::SubmitInput(uint16_t id, const InputCommand& command,
                              std::chrono::steady_clock::time_point received) {
  constexpr int32_t kMaxClientCoordinateCm = 10'000'000;
  if (command.transform_valid &&
      (command.x_cm < -kMaxClientCoordinateCm || command.x_cm > kMaxClientCoordinateCm ||
       command.y_cm < -kMaxClientCoordinateCm || command.y_cm > kMaxClientCoordinateCm ||
       command.z_cm < -kMaxClientCoordinateCm || command.z_cm > kMaxClientCoordinateCm)) return false;
  const auto it = std::find_if(players_.begin(), players_.end(), [=](const Player& p) { return p.state.id == id; });
  if (it == players_.end()) return false;
  if (it->has_sequence && !IsNewerInputSequence(command.sequence, it->last_sequence)) return false;
  if (it->last_input != std::chrono::steady_clock::time_point{} && received - it->last_input < std::chrono::milliseconds(8)) return false;
  if (!accept_client_transforms_ && command.request_spawn && command.gameplay_active &&
      !it->state.position_reset) {
    // In server-owned health mode, spawning is also a health reset. Only the
    // server's recorded lethal state may authorize it; inactivity alone is a
    // client-controlled hint and must not grant a heal or teleport.
    if (!it->state.health_valid || it->state.alive || it->state.health != 0) return false;
    // A spawn request cannot teleport a currently active player. Require a
    // server-observed inactive interval before honoring a dead client's hint.
    if (it->state.active || it->inactive_since == std::chrono::steady_clock::time_point{} ||
        received - it->inactive_since < std::chrono::seconds(2)) return false;
    if (it->last_spawn_reset != std::chrono::steady_clock::time_point{} &&
        received - it->last_spawn_reset < std::chrono::seconds(5)) return false;
    it->last_spawn_reset = received;
    if (server_authoritative_vitals_) {
      it->state.health_valid = true;
      it->state.alive = true;
      it->state.health = player_max_health_;
      it->state.max_health = player_max_health_;
    }
    // Respawn requests select only the operator-configured spawn. The client
    // cannot submit replacement coordinates or heading; death state, an
    // inactive interval, and cooldown limit repeat teleports.
    RebasePlayerPosition(id, it->spawn_x_cm, it->spawn_y_cm, it->spawn_z_cm, 0);
    return false;
  }
  if (it->state.position_reset) {
    if (command.position_reset_ack) {
      if (command.position_reset_ack != it->state.position_reset_id) return false;
      it->client_reset_acknowledged = true;
      if (!it->authoritative_reset_required || it->authoritative_reset_acknowledged)
        it->state.position_reset = false;
    } else if (command.gameplay_active) {
      return false;
    }
  }
  // A transform is optional for compatibility: the native game client supplies
  // its guest pose, while input-only clients use the server's fixed-step fallback.
  const bool was_active = it->state.active;
  const bool host_unavailable = it->externally_simulated && it->authoritative_unavailable;
  const bool reset_pending = it->state.position_reset;
  const bool trusted_vitals_allow_activity = !it->state.health_valid || it->state.alive;
  const bool server_allows_activity = !host_unavailable && !reset_pending &&
      trusted_vitals_allow_activity;
  // Reset ACK policy is a server-wide capability, but transform authority is
  // per player. In an F5 host, only seats returned by the guest transform
  // source are externally simulated; other network peers still supply their
  // bounded client transforms and must be activated by accepted input.
  const bool trusted_transform_source = it->externally_simulated;
  // The client hint controls input delivery only. Server presence follows fresh
  // accepted packets, server-owned life/reset state, or a trusted host sample.
  if (trusted_transform_source) {
    if (!server_allows_activity) it->state.active = false;
  } else {
    it->state.active = server_allows_activity;
  }
  const bool will_accept_client_pose = command.gameplay_active && it->state.active &&
      !trusted_transform_source;
  const bool controls_allowed = command.gameplay_active && server_allows_activity;
  if (accept_client_transforms_ && will_accept_client_pose && command.transform_valid &&
      it->client_transform_anchor_valid && received > it->client_transform_anchor_at) {
    // Client-authoritative mode still needs an outlier guard: sequence and
    // coordinate bounds alone allow a modified client to teleport across the
    // world. Use server receive time and the last accepted pose, with 25 cm of
    // jitter allowance. This is not collision validation; the host can raise
    // the cap for unusual vehicles or movement mods.
    const double elapsed_seconds = std::chrono::duration<double>(received - it->client_transform_anchor_at).count();
    const double allowed_cm =
        double(max_client_transform_speed_cm_per_second_) * elapsed_seconds + 25.0;
    const double dx = double(command.x_cm) - it->client_transform_anchor_x_cm;
    const double dy = double(command.y_cm) - it->client_transform_anchor_y_cm;
    const double dz = double(command.z_cm) - it->client_transform_anchor_z_cm;
    if (dx * dx + dy * dy + dz * dz > allowed_cm * allowed_cm) return false;
  }
  int16_t accepted_aim_yaw_mrad = command.aim_yaw_mrad;
  if (controls_allowed && it->has_client_aim_yaw &&
      it->last_input != std::chrono::steady_clock::time_point{} && received > it->last_input) {
    constexpr int32_t kYawPeriodMrad = 6'284;
    constexpr int32_t kHalfTurnMrad = 3'142;
    int32_t delta_mrad = int32_t(command.aim_yaw_mrad) - it->client_aim_yaw_mrad;
    if (delta_mrad > kHalfTurnMrad) delta_mrad -= kYawPeriodMrad;
    else if (delta_mrad < -kHalfTurnMrad) delta_mrad += kYawPeriodMrad;
    constexpr auto kMaxCreditedAimInterval = std::chrono::duration_cast<
        std::chrono::steady_clock::duration>(std::chrono::milliseconds(100));
    const auto elapsed = std::min(received - it->last_input, kMaxCreditedAimInterval);
    const double elapsed_seconds = std::chrono::duration<double>(elapsed).count();
    const double allowed_delta_mrad =
        double(max_client_aim_turn_rate_mrad_per_second_) * elapsed_seconds;
    if (std::abs(double(delta_mrad)) > allowed_delta_mrad) {
      const int32_t limited_delta = static_cast<int32_t>(std::floor(allowed_delta_mrad));
      delta_mrad = delta_mrad < 0 ? -limited_delta : limited_delta;
    }
    int32_t yaw_mrad = int32_t(it->client_aim_yaw_mrad) + delta_mrad;
    if (yaw_mrad >= kHalfTurnMrad) yaw_mrad -= kYawPeriodMrad;
    else if (yaw_mrad < -kHalfTurnMrad) yaw_mrad += kYawPeriodMrad;
    accepted_aim_yaw_mrad = static_cast<int16_t>(yaw_mrad);
  }
  if (controls_allowed) {
    it->client_aim_yaw_mrad = accepted_aim_yaw_mrad;
    it->has_client_aim_yaw = true;
  } else {
    accepted_aim_yaw_mrad = it->has_client_aim_yaw
        ? it->client_aim_yaw_mrad : it->state.yaw_mrad;
  }
  if (accept_client_transforms_ && will_accept_client_pose && command.transform_valid) {
    // Explicit client-authoritative mode: preserve the local EDF guest's
    // collision/terrain movement instead of integrating a generic planar model.
    it->state.x_cm = command.x_cm;
    it->state.y_cm = command.y_cm;
    it->state.z_cm = command.z_cm;
    it->state.yaw_mrad = accepted_aim_yaw_mrad;
    it->client_transform_anchor_valid = true;
    it->client_transform_anchor_x_cm = command.x_cm;
    it->client_transform_anchor_y_cm = command.y_cm;
    it->client_transform_anchor_z_cm = command.z_cm;
    it->client_transform_anchor_at = received;
    it->movement_remainder_x_cm = it->movement_remainder_z_cm = 0.0;
  }
  // XInput uses all sixteen bits, including face buttons in bits 12-15. Preserve the full
  // controller word so fire/jump/weapon actions survive the server snapshot path.
  InputCommand accepted = command;
  accepted.aim_yaw_mrad = accepted_aim_yaw_mrad;
  if (!controls_allowed) {
    accepted.buttons = 0;
    accepted.left_trigger = accepted.right_trigger = 0;
    accepted.move_x = accepted.move_y = accepted.aim_x = accepted.aim_y = 0;
    accepted.request_spawn = false;
    accepted.transform_valid = false;
    accepted.x_cm = accepted.y_cm = accepted.z_cm = 0;
  }
  it->input = accepted;
  // Native EDF clients own their mission respawns while guest transforms are
  // accepted. Never retain a spawn hint that could later rebase them to origin.
  if (accept_client_transforms_) it->input.request_spawn = false;
  if (!accept_client_transforms_) {
    it->input.transform_valid = false;
    it->input.x_cm = it->input.y_cm = it->input.z_cm = 0;
  }
  if (it->state.active) {
    it->inactive_since = {};
  } else if (was_active || it->inactive_since == std::chrono::steady_clock::time_point{}) {
    it->inactive_since = received;
  }
  if (!controls_allowed) {
    it->state.buttons = 0;
    it->state.left_trigger = it->state.right_trigger = 0;
  }
  it->last_sequence = command.sequence;
  it->has_sequence = true;
  it->last_input = received;
  return true;
}

void ServerWorld::Tick() {
  ++tick_;
  constexpr double kTickSeconds = 1.0 / 60.0;
  constexpr double kSpeedCmPerSecond = 420.0;
  constexpr int32_t kWorldLimitCm = 1'000'000;
  for (auto& player : players_) {
    if (!player.state.active) {
      if (!player.authoritative_unavailable && !player.state.position_reset) player.input = {};
      player.state.buttons = 0;
      player.state.left_trigger = player.state.right_trigger = 0;
      continue;
    }
    // A lost client stops moving quickly; the server never trusts a held input forever.
    if (player.last_input == std::chrono::steady_clock::time_point{} ||
        std::chrono::steady_clock::now() - player.last_input > std::chrono::milliseconds(250)) {
      player.input = {};
      player.state.active = false;
      if (player.inactive_since == std::chrono::steady_clock::time_point{})
        player.inactive_since = std::chrono::steady_clock::now();
      player.state.buttons = 0;
      player.state.left_trigger = player.state.right_trigger = 0;
      continue;
    }
    if (player.externally_simulated) {
      // A host guest owns this pilot's movement. Keep accepted action state, but
      // do not integrate the fallback planar model over the sampled guest pose.
      player.state.buttons = player.input.buttons;
      player.state.left_trigger = player.input.left_trigger;
      player.state.right_trigger = player.input.right_trigger;
      continue;
    }
    if (player.input.transform_valid) {
      // The latest accepted guest transform was copied in SubmitInput. Keep it
      // intact until a newer input or the normal input timeout arrives.
      player.state.buttons = player.input.buttons;
      player.state.left_trigger = player.input.left_trigger;
      player.state.right_trigger = player.input.right_trigger;
      player.state.yaw_mrad = player.input.aim_yaw_mrad;
      continue;
    }
    const double local_x = double(player.input.move_x) / 32767.0;
    const double local_z = double(player.input.move_y) / 32767.0;
    const double yaw = double(player.input.aim_yaw_mrad) / 1000.0;
    const double ax = local_x * std::cos(yaw) + local_z * std::sin(yaw);
    const double az = -local_x * std::sin(yaw) + local_z * std::cos(yaw);
    const double magnitude = std::hypot(ax, az);
    const double scale = magnitude > 1.0 ? 1.0 / magnitude : 1.0;
    const auto whole_centimeters = [](double delta, double& remainder) {
      const double accumulated = delta + remainder;
      const double whole = std::trunc(accumulated);
      remainder = accumulated - whole;
      return static_cast<int32_t>(whole);
    };
    const int32_t next_x = player.state.x_cm + whole_centimeters(
        ax * scale * kSpeedCmPerSecond * kTickSeconds, player.movement_remainder_x_cm);
    const int32_t next_z = player.state.z_cm + whole_centimeters(
        az * scale * kSpeedCmPerSecond * kTickSeconds, player.movement_remainder_z_cm);
    player.state.x_cm = std::clamp(next_x, -kWorldLimitCm, kWorldLimitCm);
    player.state.z_cm = std::clamp(next_z, -kWorldLimitCm, kWorldLimitCm);
    if (player.state.x_cm != next_x) player.movement_remainder_x_cm = 0.0;
    if (player.state.z_cm != next_z) player.movement_remainder_z_cm = 0.0;
    player.state.buttons = player.input.buttons;
    player.state.left_trigger = player.input.left_trigger;
    player.state.right_trigger = player.input.right_trigger;
    player.state.yaw_mrad = player.input.aim_yaw_mrad;
  }
}

WorldSnapshot ServerWorld::Snapshot() const {
  WorldSnapshot snapshot;
  snapshot.tick = tick_;
  snapshot.players.reserve(players_.size());
  for (const auto& player : players_) snapshot.players.push_back(player.state);
  return snapshot;
}

std::vector<ScriptPlayerView> ServerWorld::ScriptPlayers() const {
  std::vector<ScriptPlayerView> players;
  players.reserve(players_.size());
  for (const auto& player : players_)
    players.push_back({player.state, player.input});
  return players;
}

std::optional<PlayerState> ServerWorld::FindPlayerState(uint16_t id) const {
  const auto it = std::find_if(players_.begin(), players_.end(), [=](const Player& player) {
    return player.state.id == id;
  });
  if (it == players_.end()) return std::nullopt;
  return it->state;
}

std::optional<InputCommand> ServerWorld::FindPlayerInput(uint16_t id) const {
  const auto it = std::find_if(players_.begin(), players_.end(), [=](const Player& player) {
    return player.state.id == id;
  });
  if (it == players_.end()) return std::nullopt;
  return it->input;
}

std::optional<PendingPositionReset> ServerWorld::FindPendingPositionReset(uint16_t id) const {
  const auto it = std::find_if(players_.begin(), players_.end(), [=](const Player& player) {
    return player.state.id == id;
  });
  if (it == players_.end() || !it->state.position_reset) return std::nullopt;
  return PendingPositionReset{it->state.id, it->state.position_reset_id, it->state.x_cm,
                              it->state.y_cm, it->state.z_cm, it->state.yaw_mrad};
}

WorldSnapshot ServerWorld::SnapshotFor(uint16_t observer_id, uint32_t interest_radius_cm,
                                       uint32_t presence_sequence) const {
  WorldSnapshot snapshot;
  snapshot.tick = tick_;
  const auto observer = std::find_if(players_.begin(), players_.end(), [=](const Player& player) {
    return player.state.id == observer_id;
  });
  if (observer == players_.end()) return snapshot;
  snapshot.owner_pose_authoritative = observer->owner_pose_authoritative ||
      (server_authoritative_vitals_ && observer->state.health_valid);

  snapshot.players.reserve(kMaxSnapshotPlayers);
  snapshot.players.push_back(observer->state);
  if (kMaxSnapshotPlayers == 1) return snapshot;

  const auto append_presence = [&](const Player& player) {
    if (snapshot.players.size() >= kMaxSnapshotPlayers) return;
    PlayerState presence = player.state;
    // Keep connected names visible in the browser without disclosing out-of-scope
    // transforms or making a presence-only player eligible for a guest replica.
    presence.active = false;
    presence.position_reset = false;
    presence.position_reset_id = 0;
    presence.x_cm = presence.y_cm = presence.z_cm = 0;
    presence.yaw_mrad = 0;
    presence.health_valid = false;
    presence.alive = false;
    presence.health = presence.max_health = 0;
    presence.buttons = 0;
    presence.left_trigger = presence.right_trigger = 0;
    snapshot.players.push_back(std::move(presence));
  };

  if (!observer->state.active) {
    // Menus and mission transitions have no active spatial observer. Still return
    // connected peers (as non-replicating presence records) so F5 can show joins.
    // Rotate the bounded wire roster; the client retains names between pages.
    std::vector<const Player*> presence;
    presence.reserve(players_.size() - 1);
    for (const auto& player : players_)
      if (player.state.id != observer_id) presence.push_back(&player);
    const size_t page_size = std::min<size_t>(kMaxSnapshotPlayers - 1, presence.size());
    if (page_size) {
      const size_t start = (uint64_t(presence_sequence) * page_size) % presence.size();
      for (size_t i = 0; i < page_size; ++i)
        append_presence(*presence[(start + i) % presence.size()]);
    }
    return snapshot;
  }

  struct Candidate {
    uint64_t distance_squared = 0;
    const PlayerState* state = nullptr;
  };
  // Reserve a few wire slots for a rotating presence page. This keeps remote
  // soldier replication focused on the nearest active peers while allowing the
  // browser roster to discover servers configured above the packet's 32-player cap.
  constexpr size_t kPresenceSlots = 4;
  constexpr size_t kNearestCapacity = kMaxSnapshotPlayers - 1 - kPresenceSlots;
  std::array<Candidate, kNearestCapacity> nearest{};
  size_t nearest_count = 0;
  const uint64_t radius = interest_radius_cm;
  const uint64_t radius_squared = radius * radius;
  const auto by_distance = [](const Candidate& left, const Candidate& right) {
    if (left.distance_squared != right.distance_squared) return left.distance_squared < right.distance_squared;
    return left.state->id < right.state->id;
  };
  for (const auto& player : players_) {
    if (player.state.id == observer_id || !player.state.active) continue;
    const int64_t dx = int64_t(player.state.x_cm) - observer->state.x_cm;
    const int64_t dy = int64_t(player.state.y_cm) - observer->state.y_cm;
    const int64_t dz = int64_t(player.state.z_cm) - observer->state.z_cm;
    const uint64_t distance_squared = uint64_t(dx * dx + dy * dy + dz * dz);
    if (distance_squared > radius_squared) continue;
    const Candidate candidate{distance_squared, &player.state};
    if (nearest_count < kNearestCapacity) {
      nearest[nearest_count++] = candidate;
      std::push_heap(nearest.begin(), nearest.begin() + nearest_count, by_distance);
    } else if (by_distance(candidate, nearest.front())) {
      // The heap root is the farthest selected player; replace it only when this
      // candidate is closer, keeping selection bounded without a temporary vector.
      std::pop_heap(nearest.begin(), nearest.begin() + nearest_count, by_distance);
      nearest[nearest_count - 1] = candidate;
      std::push_heap(nearest.begin(), nearest.begin() + nearest_count, by_distance);
    }
  }
  std::sort(nearest.begin(), nearest.begin() + nearest_count, by_distance);
  for (size_t i = 0; i < nearest_count; ++i) snapshot.players.push_back(*nearest[i].state);
  // Keep roster filtering bounded at high occupancy. The previous per-player
  // scan against snapshot.players cost O(players * snapshot slots) for every
  // observer at 20 Hz; sort this fixed-size ID set once and binary-search it.
  std::array<uint16_t, kMaxSnapshotPlayers> included_ids{};
  size_t included_count = 0;
  for (const PlayerState& included : snapshot.players)
    included_ids[included_count++] = included.id;
  std::sort(included_ids.begin(), included_ids.begin() + included_count);
  // Peers outside the active interest set remain visible by name only. Rotate
  // the remaining IDs so clients with more than one packet of peers can build a
  // complete roster without ever treating these records as world replicas.
  std::vector<const Player*> presence;
  presence.reserve(players_.size());
  for (const auto& player : players_) {
    if (player.state.id == observer_id) continue;
    const bool already_included = std::binary_search(
        included_ids.begin(), included_ids.begin() + included_count, player.state.id);
    if (!already_included) presence.push_back(&player);
  }
  const size_t page_size = std::min<size_t>(kPresenceSlots, presence.size());
  if (page_size) {
    const size_t start = (uint64_t(presence_sequence) * page_size) % presence.size();
    for (size_t i = 0; i < page_size; ++i)
      append_presence(*presence[(start + i) % presence.size()]);
  }
  return snapshot;
}

}  // namespace edf::net
