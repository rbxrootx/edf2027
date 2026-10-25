#pragma once

#include "protocol.h"

#include <chrono>
#include <mutex>
#include <optional>

namespace edf::net {

struct RemoteGuestIdentity {
  uint16_t player_id = 0;
  uint64_t connection_generation = 0;
};

// Thread-safe input handoff between the authenticated server loop and EDF's
// game thread. The host stays on seat 0; at most one remote pilot owns seat 1.
class LocalHostGuestInputMailbox {
 public:
  using Clock = std::chrono::steady_clock;

  void Reset() {
    std::lock_guard lock(mutex_);
    local_player_id_ = 0;
    remote_player_id_ = 0;
    remote_guest_seat_ready_ = false;
    remote_input_ = {};
    remote_input_at_ = {};
  }

  void SetLocalPlayerId(uint16_t player_id) {
    std::lock_guard lock(mutex_);
    if (local_player_id_ != player_id) {
      if (local_player_id_ || remote_player_id_ == player_id)
        ClearRemoteGuestInput();
      local_player_id_ = player_id;
    }
  }

  void Publish(uint16_t player_id, std::optional<InputCommand> input,
               Clock::time_point now = Clock::now()) {
    if (!player_id) return;
    std::lock_guard lock(mutex_);
    if (player_id == local_player_id_) return;
    if (!input) {
      if (player_id == remote_player_id_) ClearRemoteGuestInput();
      return;
    }
    if (!remote_player_id_ && !input->gameplay_active) return;
    if (remote_player_id_ && remote_player_id_ != player_id) return;
    if (!remote_player_id_) {
      ++remote_connection_generation_;
      if (!remote_connection_generation_) ++remote_connection_generation_;
    }
    remote_player_id_ = player_id;
    remote_input_ = *input;
    remote_input_at_ = now;
  }

  bool HasRemoteGuestSeat() const {
    std::lock_guard lock(mutex_);
    return remote_player_id_ != 0;
  }

  uint16_t RemoteGuestPlayerId() const {
    std::lock_guard lock(mutex_);
    return remote_guest_seat_ready_ ? remote_player_id_ : 0;
  }

  RemoteGuestIdentity PendingRemoteGuestIdentity() const {
    std::lock_guard lock(mutex_);
    return {remote_player_id_, remote_connection_generation_};
  }

  bool SetRemoteGuestSeatReady(RemoteGuestIdentity expected, bool ready) {
    std::lock_guard lock(mutex_);
    if (!expected.player_id || expected.player_id != remote_player_id_ ||
        expected.connection_generation != remote_connection_generation_) return false;
    remote_guest_seat_ready_ = ready;
    return remote_guest_seat_ready_;
  }

  std::optional<InputCommand> Read(Clock::time_point now = Clock::now()) const {
    std::lock_guard lock(mutex_);
    if (!remote_player_id_ || !remote_guest_seat_ready_)
      return std::nullopt;
    constexpr auto kMaximumInputAge = std::chrono::milliseconds(300);
    if (remote_input_at_ == Clock::time_point{} || now < remote_input_at_ ||
        now - remote_input_at_ > kMaximumInputAge)
      return InputCommand{};
    return remote_input_;
  }

 private:
  void ClearRemoteGuestInput() {
    remote_player_id_ = 0;
    remote_guest_seat_ready_ = false;
    remote_input_ = {};
    remote_input_at_ = {};
  }

  mutable std::mutex mutex_;
  uint16_t local_player_id_ = 0;
  uint16_t remote_player_id_ = 0;
  uint64_t remote_connection_generation_ = 0;
  bool remote_guest_seat_ready_ = false;
  InputCommand remote_input_{};
  Clock::time_point remote_input_at_{};
};

}  // namespace edf::net
