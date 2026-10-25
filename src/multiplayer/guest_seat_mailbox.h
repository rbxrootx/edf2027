#pragma once

#include "protocol.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>

namespace edf::net::probe {

struct NetworkPositionReset {
  uint16_t player_id = 0;
  uint32_t reset_id = 0;
  int32_t x_cm = 0, y_cm = 0, z_cm = 0;
  int16_t yaw_mrad = 0;
  bool guest_applied = false;
  bool guest_acknowledged = false;
};

inline constexpr std::size_t kNetworkGuestSeatCount = 2;

struct NetworkSeatInput {
  uint32_t player_index = 0;
  uint16_t player_id = 0;
  bool engaged = false;
  std::optional<InputCommand> command;
  std::optional<NetworkPositionReset> position_reset;
  uint16_t acknowledged_reset_player_id = 0;
  uint32_t acknowledged_reset_id = 0;
};

struct NetworkInputView {
  bool enabled = false;
  bool mission_ready = false;
  std::array<NetworkSeatInput, kNetworkGuestSeatCount> seats{};
};

// Thread-confined mailbox logic used by the guest bridge. The caller supplies
// sampled object validation before a new network peer can claim either seat.
class NetworkSeatMailbox {
 public:
  void Configure() {
    state_ = {};
    state_.enabled = true;
    for (std::size_t index = 0; index < state_.seats.size(); ++index)
      state_.seats[index].player_index = static_cast<uint32_t>(index);
  }

  void MarkMissionReady() {
    if (!state_.enabled || state_.mission_ready) return;
    state_.mission_ready = true;
    for (auto& seat : state_.seats)
      if (seat.player_id && seat.command) seat.engaged = true;
  }

  bool Publish(uint16_t player_id, std::optional<InputCommand> command,
               std::optional<NetworkPositionReset> position_reset,
               const std::array<bool, kNetworkGuestSeatCount>& validated_seats) {
    if (!state_.enabled || !player_id) return false;
    auto* seat = FindPlayer(player_id);
    if (!command) {
      if (!seat) return true;
      if (position_reset && position_reset->player_id == player_id)
        StoreReset(*seat, *position_reset);
      const uint32_t player_index = seat->player_index;
      *seat = {};
      seat->player_index = player_index;
      return true;
    }
    if (!command->gameplay_active) {
      if (!seat) return true;
      if (position_reset && position_reset->player_id == player_id && !StoreReset(*seat, *position_reset))
        return false;
      seat->command.reset();
      seat->engaged = false;
      return true;
    }
    if (!seat) {
      for (std::size_t index = 0; index < state_.seats.size(); ++index) {
        auto& candidate = state_.seats[index];
        if (!candidate.player_id && validated_seats[index]) {
          seat = &candidate;
          seat->player_id = player_id;
          break;
        }
      }
    }
    if (!seat) return false;
    if (position_reset && position_reset->player_id == player_id && !StoreReset(*seat, *position_reset))
      return false;
    seat->command = *command;
    seat->engaged = state_.mission_ready;
    if (seat->position_reset && seat->position_reset->player_id == player_id &&
        seat->position_reset->guest_acknowledged &&
        command->position_reset_ack == seat->position_reset->reset_id)
      seat->position_reset.reset();
    return true;
  }

  bool PublishPositionReset(const NetworkPositionReset& reset) {
    auto* seat = FindPlayer(reset.player_id);
    return state_.enabled && seat && reset.reset_id && StoreReset(*seat, reset);
  }

  void AcknowledgePositionReset(uint16_t player_id, uint32_t reset_id) {
    auto* seat = FindPlayer(player_id);
    if (!seat || !seat->position_reset || seat->position_reset->player_id != player_id ||
        seat->position_reset->reset_id != reset_id || !seat->position_reset->guest_applied) return;
    seat->acknowledged_reset_player_id = player_id;
    seat->acknowledged_reset_id = reset_id;
    seat->position_reset->guest_acknowledged = true;
    if (seat->command && seat->command->gameplay_active &&
        seat->command->position_reset_ack == reset_id)
      seat->position_reset.reset();
  }

  bool WithPendingPositionReset(uint16_t player_id, uint32_t reset_id,
                               const std::function<void(NetworkPositionReset&)>& callback) {
    auto* seat = FindPlayer(player_id);
    if (!callback || !state_.enabled || !seat || !seat->position_reset ||
        seat->position_reset->player_id != player_id || seat->position_reset->reset_id != reset_id ||
        seat->position_reset->guest_acknowledged) return false;
    callback(*seat->position_reset);
    return true;
  }

  NetworkInputView Read() const { return state_; }

 private:
  NetworkSeatInput* FindPlayer(uint16_t player_id) {
    for (auto& seat : state_.seats)
      if (seat.player_id == player_id) return &seat;
    return nullptr;
  }

  bool StoreReset(NetworkSeatInput& seat, const NetworkPositionReset& reset) {
    if (seat.position_reset && seat.position_reset->player_id == reset.player_id &&
        seat.position_reset->reset_id == reset.reset_id) {
      return seat.position_reset->x_cm == reset.x_cm && seat.position_reset->y_cm == reset.y_cm &&
          seat.position_reset->z_cm == reset.z_cm && seat.position_reset->yaw_mrad == reset.yaw_mrad;
    }
    seat.position_reset = reset;
    seat.position_reset->guest_applied = false;
    seat.position_reset->guest_acknowledged = false;
    seat.acknowledged_reset_player_id = 0;
    seat.acknowledged_reset_id = 0;
    return true;
  }

  NetworkInputView state_;
};

}  // namespace edf::net::probe
