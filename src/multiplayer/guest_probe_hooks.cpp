#include "guest_probe.h"

#include "generated/default/edf2017_init.h"

#include "console/console_guest.h"
#include "native_graphics/native_pacing.h"

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

REXCVAR_DEFINE_BOOL(edf_mp_actor_trace,false,"EDF2027",
    "Trace mission ant spawns/removals and received host samples (development; read-only)");

namespace {
std::atomic<uint64_t> g_title_router_call_ids{0};
std::atomic<uint64_t> g_decoder_opcode_call_order{0};
std::atomic<uint64_t> g_raw_vm_dispatch_order{0};
std::atomic<uint64_t> g_mission_start_dispatch_order{0};
std::atomic<uint32_t> g_decoder_trace_begin{0};
std::atomic<uint32_t> g_decoder_trace_end{0};
std::atomic<uint64_t> g_decoder_trace_dropped{0};
std::atomic<uint16_t> g_decoder_trace_last_buttons{0};
std::atomic<uint32_t> g_decoder_trace_capture_until_ms{0};
std::atomic<bool> g_decoder_trace_has_trigger{false};
thread_local uint64_t g_active_title_router_call_id = 0;
thread_local uint32_t g_active_title_router_compare_calls = 0;
thread_local uint32_t g_active_title_router_compare_result = UINT32_MAX;
thread_local uint32_t g_active_title_router_compare_object = 0;
thread_local uint32_t g_active_title_router_compare_value_20 = 0;
thread_local uint32_t g_active_title_router_compare_value_32 = 0;
thread_local uint32_t g_active_decoder_context = 0;
thread_local uint32_t g_active_decoder_callsite = 0;
thread_local uint64_t g_active_decoder_opcode_sample_token = 0;
std::atomic<uint64_t> g_headless_fence_waits{0};
std::atomic<uint64_t> g_headless_allocator_waits{0};
std::atomic<uint64_t> g_mission_sequence_begins{0};
std::atomic<uint32_t> g_input_script_elapsed_ms{0};
std::atomic<uint16_t> g_input_script_buttons{0};
std::atomic<bool> g_trace_input_consumers{false};
std::array<std::array<std::array<std::atomic<uint64_t>, 4>, 3>, 4> g_input_consumer_mask_polls{};
std::array<std::atomic<uint64_t>, 4> g_xpad_action_poll_calls{};
std::array<std::atomic<uint32_t>, 4> g_xpad_action_targets_seen{};
std::array<std::atomic<uint32_t>, 4> g_xpad_action_channels_seen{};
std::array<std::array<std::atomic<uint32_t>, 32>, 4> g_xpad_action_targets{};
std::array<std::atomic<uint64_t>, 4> g_xpad_action_state_samples{};
std::array<std::atomic<uint32_t>, 4> g_xpad_action_held_seen{};
std::array<std::atomic<uint32_t>, 4> g_xpad_action_edges_seen{};
std::array<std::atomic<uint32_t>, 4> g_xpad_action_pulses_seen{};
std::array<std::atomic<uint32_t>, 4> g_xpad_action_last_held{};
std::array<std::atomic<uint32_t>, 4> g_xpad_action_last_edges{};
std::array<std::atomic<uint32_t>, 4> g_xpad_action_last_pulses{};
std::array<std::atomic<uint64_t>, 4> g_xpad_action_invalid_samples{};
std::atomic<uint64_t> g_title_action_handler_calls{0};
std::atomic<uint32_t> g_title_action_handler_last_r31{0};
std::atomic<uint64_t> g_title_action_default_calls{0};
std::atomic<uint32_t> g_title_action_default_last_r31{0};
std::atomic<uint64_t> g_title_action_consumer_reads{0};
std::atomic<uint32_t> g_title_action_consumer_event_mask{0};
std::atomic<uint32_t> g_title_action_consumer_last_event{0};
std::atomic<uint32_t> g_title_action_consumer_last_object{0};
std::atomic<uint32_t> g_title_action_consumer_last_slot{0};
std::atomic<uint32_t> g_title_action_consumer_last_count{0};
std::atomic<uint32_t> g_title_action_consumer_last_callsite{0};
std::array<std::atomic<uint64_t>, 4> g_title_ui_action_slot_reads{};
std::array<std::atomic<uint32_t>, 4> g_title_ui_action_slot_event_values{};
std::array<std::atomic<uint32_t>, 4> g_title_ui_action_slot_input_flags{};
std::array<std::atomic<uint32_t>, 4> g_title_ui_action_slot_last_event{};
std::array<std::atomic<uint64_t>, 4> g_input_event_update_slot_calls{};
std::array<std::atomic<uint32_t>, 4> g_input_event_update_slot_callbacks{};
std::array<std::atomic<uint32_t>, 4> g_input_event_update_slot_last_event{};
std::array<std::atomic<uint32_t>, 4> g_input_event_update_slot_nonzero_seen{};
std::atomic<uint64_t> g_input_event_update_calls{0};
std::atomic<uint64_t> g_input_event_update_nonzero_calls{0};
std::atomic<uint32_t> g_input_event_update_last_object{0};
std::atomic<uint32_t> g_input_event_update_last_callback{0};
std::atomic<uint32_t> g_input_event_update_last_event_before{0};
std::atomic<uint32_t> g_input_event_update_last_event_after{0};
std::atomic<uint32_t> g_input_event_update_last_callsite{0};
std::atomic<uint64_t> g_title_action_slot_scan_calls{0};
std::atomic<uint32_t> g_title_action_slot_scan_manager{0};
std::atomic<uint32_t> g_title_action_slot_scan_slot{0};
std::atomic<uint32_t> g_title_action_slot_scan_count{0};
std::atomic<uint32_t> g_title_action_slot_scan_object{0};
std::atomic<uint32_t> g_title_action_slot_scan_event{0};
std::atomic<uint64_t> g_title_ui_handoff_calls{0};
std::atomic<uint64_t> g_title_scene_update_calls{0};
std::atomic<uint16_t> g_title_ui_trace_last_buttons{0};
std::atomic<uint32_t> g_title_ui_handoff_window{0};
std::atomic<uint32_t> g_title_scene_update_window{0};
std::atomic<uint64_t> g_title_sequence_request_calls{0};
std::atomic<uint32_t> g_title_sequence_request_callsite{0};
std::atomic<uint32_t> g_title_sequence_request_receiver{0};
std::atomic<uint32_t> g_title_sequence_request_object{0};
std::atomic<uint32_t> g_title_sequence_request_state_72_before{0};
std::atomic<uint32_t> g_title_sequence_request_state_60_before{0};
std::atomic<uint32_t> g_title_sequence_request_state_60_after{0};
std::atomic<uint32_t> g_title_sequence_request_sequence_object{0};
std::atomic<uint32_t> g_title_sequence_request_sequence_696_before{0};
std::atomic<uint32_t> g_title_sequence_request_sequence_696_after{0};
std::atomic<uint32_t> g_title_sequence_request_sequence_708_before{0};
std::atomic<uint32_t> g_title_sequence_request_sequence_708_after{0};
std::atomic<uint32_t> g_title_sequence_request_sequence_712_before{0};
std::atomic<uint32_t> g_title_sequence_request_sequence_712_after{0};
std::array<std::array<std::atomic<uint64_t>, 32>, 3> g_title_ui_router_event_calls{};
std::atomic<uint64_t> g_title_action_gate_samples{0};
std::atomic<uint32_t> g_title_action_gate_2260_last{0};
std::atomic<uint32_t> g_title_action_gate_2261_last{0};
std::atomic<uint32_t> g_title_action_gate_2262_last{0};
std::atomic<uint32_t> g_title_action_gate_a_object{0};
std::atomic<uint32_t> g_title_action_gate_a_link{0};
std::atomic<uint32_t> g_title_action_gate_a_state{0};
std::atomic<uint32_t> g_title_action_gate_a_result{0};
std::atomic<uint32_t> g_title_action_gate_b_object{0};
std::atomic<uint32_t> g_title_action_gate_b_712{0};
std::atomic<uint32_t> g_title_action_gate_b_696{0};
std::atomic<uint32_t> g_title_action_gate_b_708{0};
std::atomic<uint32_t> g_title_action_gate_b_result{0};
std::atomic<uint32_t> g_title_action_gate_sequence_pointer{0};
std::atomic<uint32_t> g_title_action_gate_sequence_708{0};
std::atomic<uint32_t> g_title_action_gate_sequence_result{0};
std::atomic<uint32_t> g_title_action_gate_state_object{0};
std::atomic<uint32_t> g_title_action_gate_state_vtable{0};
std::atomic<uint32_t> g_title_action_gate_input_manager{0};
std::atomic<uint32_t> g_title_action_gate_input_count{0};
std::atomic<uint64_t> g_title_action_gate_a_function_calls{0};
std::atomic<uint32_t> g_title_action_gate_a_function_callsite{0};
std::atomic<uint32_t> g_title_action_gate_a_function_return{0};
std::atomic<uint64_t> g_title_action_gate_b_function_calls{0};
std::atomic<uint32_t> g_title_action_gate_b_function_callsite{0};
std::atomic<uint32_t> g_title_action_gate_b_function_return{0};
std::atomic<uint32_t> g_title_action_gate_r31{0};
std::atomic<uint32_t> g_title_action_gate_r25{0};
std::atomic<bool> g_title_action_gate_estimates_valid{false};
std::atomic<bool> g_title_action_gate_timer_valid{false};
std::atomic<uint32_t> g_title_action_gate_timer_a{0};
std::atomic<uint32_t> g_title_action_gate_timer_b{0};
std::atomic<uint32_t> g_title_action_gate_timer_branch_taken{0};
std::atomic<uint64_t> g_title_action_user_samples{0};
std::atomic<uint32_t> g_title_action_user_count{0};
std::atomic<uint32_t> g_title_action_user_object_mask{0};
std::atomic<uint32_t> g_title_action_slot_gate_mask{0};
std::atomic<uint32_t> g_title_action_slot_gate_seen{0};
std::atomic<uint64_t> g_input_dispatch_gate_calls{0};
std::atomic<uint64_t> g_input_dispatch_gate_slot_matches{0};
std::array<std::atomic<uint32_t>, 4> g_input_dispatch_gate_watched_addresses{};
std::atomic<uint32_t> g_title_action_registry_pointer{0};
std::atomic<uint32_t> g_title_action_user_invalid_stage{0};
std::atomic<uint64_t> g_title_action_user_invalid_samples{0};
struct HeadlessPacingState {
  std::mutex mutex;
  edf::native::NativePacingClock clock;
  bool initialized = false;
};
HeadlessPacingState g_headless_pacing;
std::atomic<uint64_t> g_scheduler_calls{0};
std::atomic<uint64_t> g_guest_wait_calls{0};
std::atomic<uint64_t> g_dispatches{0};
std::atomic<uint64_t> g_steps{0};
std::array<std::atomic<uint64_t>, 4> g_xinput_user_wrapper_calls{};
std::array<std::atomic<uint64_t>, 4> g_xinput_user_calls_last_sampled_ready{};
std::atomic<uint64_t> g_other_xinput_wrapper_calls{0};
std::atomic<uint64_t> g_other_xinput_calls_last_sampled_ready{0};
std::array<std::atomic<uint64_t>, 4> g_xinput_user_native_forwards{};
std::array<std::atomic<uint64_t>, 4> g_xinput_user_native_forwards_last_sampled_ready{};
std::atomic<uint64_t> g_other_xinput_native_forwards{0};
std::atomic<uint64_t> g_other_xinput_native_forwards_last_sampled_ready{0};
std::atomic<bool> g_last_sampled_mission_ready{false};
std::atomic<uint32_t> g_input_user{0};
std::vector<edf::ScriptedInputEvent> g_input_events;
edf::ScriptedClock g_input_clock = edf::ScriptedClock::Wall;
bool g_mission_probe_enabled = false;
std::mutex g_input_clock_mutex;
std::chrono::steady_clock::time_point g_input_wall_origin{};
uint64_t g_input_tick_origin = 0;
std::atomic<uint32_t> g_input_packet{0};
bool g_input_clock_started = false;
std::mutex g_network_input_mutex;
edf::net::probe::NetworkSeatMailbox g_network_input;
#if defined(EDF2027_AUTHORITATIVE_HOST)
std::atomic<uint32_t> g_player_spawn_probe_index{0};
std::atomic<bool> g_player_spawn_probe_attempted{false};
#endif
std::mutex g_state_mutex;
edf::net::probe::GuestProbeState g_state;
std::mutex g_actor_spawn_mutex;
struct QueuedActorSpawn {
  edf::net::ActorSpawnRequest request;
  uint64_t mission_generation = 0;
};
struct QueuedActorDamage {
  uint32_t actor_id = 0;
  float damage_points = 0.0f;
  uint64_t mission_generation = 0;
  uint64_t request_id = 0;
};

void CountNativeXInputForward(uint32_t user, bool last_sampled_mission_ready) {
  if (user < g_xinput_user_native_forwards.size()) {
    const size_t index = static_cast<size_t>(user);
    g_xinput_user_native_forwards[index].fetch_add(1, std::memory_order_relaxed);
    if (last_sampled_mission_ready)
      g_xinput_user_native_forwards_last_sampled_ready[index].fetch_add(1, std::memory_order_relaxed);
  } else {
    g_other_xinput_native_forwards.fetch_add(1, std::memory_order_relaxed);
    if (last_sampled_mission_ready)
      g_other_xinput_native_forwards_last_sampled_ready.fetch_add(1, std::memory_order_relaxed);
  }
}

void ObserveXpadActionChannels(uint8_t* base, uint32_t device) {
  if (!g_trace_input_consumers.load(std::memory_order_relaxed)) return;
  using edf::console::GuestMemory;
  if (!GuestMemory::Heap(device, 80)) return;

  const GuestMemory memory(base);
  const uint32_t user = memory.U32(device + 64);
  if (user >= g_xpad_action_poll_calls.size()) return;
  g_xpad_action_poll_calls[user].fetch_add(1, std::memory_order_relaxed);

  const uint32_t count = memory.U32(device + 76);
  const uint32_t targets = memory.U32(device + 68);
  if (count == 0) {
    for (uint32_t index = 0; index < 32; ++index) {
      g_xpad_action_targets[user][index].store(0, std::memory_order_relaxed);
    }
    return;
  }
  if (count > 32 || !GuestMemory::Heap(targets, count * 4)) {
    g_xpad_action_invalid_samples[user].fetch_add(1, std::memory_order_relaxed);
    for (uint32_t index = 0; index < 32; ++index) {
      g_xpad_action_targets[user][index].store(0, std::memory_order_relaxed);
    }
    return;
  }

  uint32_t seen = 0;
  for (uint32_t index = 0; index < count; ++index) {
    const uint32_t target = memory.U32(targets + index * 4);
    if (!GuestMemory::Heap(target, 36)) {
      g_xpad_action_invalid_samples[user].fetch_add(1, std::memory_order_relaxed);
      g_xpad_action_targets[user][index].store(0, std::memory_order_relaxed);
      continue;
    }
    g_xpad_action_targets[user][index].store(target, std::memory_order_relaxed);
    const float value = memory.F32(target + 32);
    if (!std::isfinite(value)) {
      g_xpad_action_invalid_samples[user].fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    if (std::abs(value) > 0.0001f) seen |= uint32_t{1} << index;
  }

  g_xpad_action_channels_seen[user].fetch_or(seen, std::memory_order_relaxed);
  uint32_t previous = g_xpad_action_targets_seen[user].load(std::memory_order_relaxed);
  while (previous < count && !g_xpad_action_targets_seen[user].compare_exchange_weak(
             previous, count, std::memory_order_relaxed, std::memory_order_relaxed)) {}
  for (uint32_t index = count; index < 32; ++index) {
    g_xpad_action_targets[user][index].store(0, std::memory_order_relaxed);
  }
}

void ObserveXpadActionStates(uint8_t* base, uint32_t callsite) {
  if (!g_trace_input_consumers.load(std::memory_order_relaxed)) return;
  using edf::console::GuestMemory;
  const GuestMemory memory(base);
  for (std::size_t user = 0; user < g_xpad_action_targets.size(); ++user) {
    const uint32_t count = (std::min)(
        g_xpad_action_targets_seen[user].load(std::memory_order_relaxed), uint32_t{32});
    if (!count) continue;
    uint32_t held = 0, edges = 0, pulses = 0;
    bool sampled = false;
    for (uint32_t index = 0; index < count; ++index) {
      const uint32_t target = g_xpad_action_targets[user][index].load(std::memory_order_relaxed);
      if (!target) continue;
      if (!GuestMemory::Heap(target, 40)) {
        g_xpad_action_invalid_samples[user].fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      sampled = true;
      if (memory.U8(target + 36)) held |= uint32_t{1} << index;
      if (memory.U8(target + 38)) edges |= uint32_t{1} << index;
      if (memory.U8(target + 39)) pulses |= uint32_t{1} << index;
    }
    if (!sampled) continue;
    g_xpad_action_state_samples[user].fetch_add(1, std::memory_order_relaxed);
    g_xpad_action_held_seen[user].fetch_or(held, std::memory_order_relaxed);
    g_xpad_action_edges_seen[user].fetch_or(edges, std::memory_order_relaxed);
    g_xpad_action_pulses_seen[user].fetch_or(pulses, std::memory_order_relaxed);
    const bool held_changed =
        g_xpad_action_last_held[user].exchange(held, std::memory_order_relaxed) != held;
    const bool edges_changed =
        g_xpad_action_last_edges[user].exchange(edges, std::memory_order_relaxed) != edges;
    const bool pulses_changed =
        g_xpad_action_last_pulses[user].exchange(pulses, std::memory_order_relaxed) != pulses;
    const bool changed = held_changed || edges_changed || pulses_changed;
    if (changed) {
      edf::net::probe::InputActionTransitionSample sample;
      sample.elapsed_ms = g_input_script_elapsed_ms.load(std::memory_order_relaxed);
      sample.callsite = callsite;
      sample.held = held;
      sample.edges = edges;
      sample.pulses = pulses;
      sample.injected_buttons = g_input_script_buttons.load(std::memory_order_relaxed);
      sample.user = static_cast<uint8_t>(user);
      std::lock_guard lock(g_state_mutex);
      constexpr std::size_t kMaximumInputActionTransitions = 48;
      if (g_state.input_action_transitions.size() < kMaximumInputActionTransitions)
        g_state.input_action_transitions.push_back(sample);
    }
  }
}

void ObserveTitleActionSlots(uint8_t* base) {
  if (!g_trace_input_consumers.load(std::memory_order_relaxed)) return;
  using edf::console::GuestMemory;
  // Dx::clInputDevice_XPad's manager global, also read by the native input
  // bindings. sub_820A6B10 loads this exact address before walking user slots.
  constexpr uint32_t kRegistryGlobal = 0x82578640;
  constexpr uint32_t kRegistryBytes = 2356;
  const GuestMemory memory(base);
  if (!GuestMemory::Image(kRegistryGlobal, 4)) {
    g_title_action_user_invalid_stage.store(1, std::memory_order_relaxed);
    g_title_action_user_invalid_samples.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const uint32_t registry = memory.U32(kRegistryGlobal);
  g_title_action_registry_pointer.store(registry, std::memory_order_relaxed);
  if (!GuestMemory::Heap(registry, kRegistryBytes) && !GuestMemory::Image(registry, kRegistryBytes)) {
    g_title_action_user_invalid_stage.store(2, std::memory_order_relaxed);
    g_title_action_user_invalid_samples.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  const uint32_t count = memory.U32(registry + 2336);
  g_title_action_user_count.store(count, std::memory_order_relaxed);
  if (count > 4) {
    g_title_action_user_invalid_stage.store(3, std::memory_order_relaxed);
    g_title_action_user_invalid_samples.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  uint32_t object_mask = 0;
  uint32_t slot_gate_mask = 0;
  for (auto& address : g_input_dispatch_gate_watched_addresses)
    address.store(0, std::memory_order_relaxed);
  for (uint32_t index = 0; index < count; ++index) {
    const uint32_t object_index = memory.U32(registry + 2340 + index * 4);
    const uint64_t object_wide = uint64_t(registry) + uint64_t(object_index) * 368;
    if (object_wide > UINT32_MAX) {
      g_title_action_user_invalid_stage.store(4, std::memory_order_relaxed);
      g_title_action_user_invalid_samples.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    const uint32_t object = static_cast<uint32_t>(object_wide);
    if (!GuestMemory::Heap(object, 154) && !GuestMemory::Image(object, 154)) {
      g_title_action_user_invalid_stage.store(4, std::memory_order_relaxed);
      g_title_action_user_invalid_samples.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    object_mask |= uint32_t{1} << index;
    g_input_dispatch_gate_watched_addresses[index].store(object + 153,
                                                          std::memory_order_relaxed);
    if (memory.U8(object + 153)) slot_gate_mask |= uint32_t{1} << index;
  }

  g_title_action_user_samples.fetch_add(1, std::memory_order_relaxed);
  g_title_action_user_count.store(count, std::memory_order_relaxed);
  g_title_action_user_object_mask.store(object_mask, std::memory_order_relaxed);
  g_title_action_slot_gate_mask.store(slot_gate_mask, std::memory_order_relaxed);
  g_title_action_slot_gate_seen.fetch_or(slot_gate_mask, std::memory_order_relaxed);
  g_title_action_user_invalid_stage.store(0, std::memory_order_relaxed);
}

void ObserveInputDispatchGate(uint8_t* base, uint32_t function, uint32_t phase,
                             uint32_t callsite, const std::array<uint32_t, 6>& arguments,
                             uint64_t fp1_bits, uint64_t fp2_bits,
                             const char* source_file = nullptr, uint32_t source_line = 0) {
  if (!g_trace_input_consumers.load(std::memory_order_relaxed)) return;
  g_input_dispatch_gate_calls.fetch_add(1, std::memory_order_relaxed);
  using edf::console::GuestMemory;
  const uint32_t receiver = arguments[0];
  if (!GuestMemory::Heap(receiver, 154) && !GuestMemory::Image(receiver, 154)) return;

  constexpr uint32_t kRegistryGlobal = 0x82578640;
  const GuestMemory memory(base);
  edf::net::probe::InputDispatchGateSample sample;
  sample.function = function;
  sample.phase = phase;
  sample.callsite = callsite;
  sample.receiver = receiver;
  sample.arguments = arguments;
  sample.fp1_bits = fp1_bits;
  sample.fp2_bits = fp2_bits;
  sample.vtable = memory.U32(receiver);
  sample.field96_bits = memory.U32(receiver + 96);
  sample.field140 = memory.U32(receiver + 140);
  sample.field144 = memory.U32(receiver + 144);
  sample.field148 = memory.U32(receiver + 148);
  sample.field152 = memory.U8(receiver + 152);
  sample.field153 = memory.U8(receiver + 153);
  sample.class_name = memory.ClassName(receiver);
  if (source_file) {
    sample.source_file = source_file;
    sample.source_line = source_line;
  }

  if (GuestMemory::Image(kRegistryGlobal, 4)) {
    const uint32_t registry = memory.U32(kRegistryGlobal);
    if ((GuestMemory::Heap(registry, 2356) || GuestMemory::Image(registry, 2356)) &&
        memory.U32(registry + 2336) <= 4) {
      const uint32_t count = memory.U32(registry + 2336);
      for (uint32_t index = 0; index < count; ++index) {
        const uint32_t slot_index = memory.U32(registry + 2340 + index * 4);
        const uint64_t slot_wide = uint64_t(registry) + uint64_t(slot_index) * 368;
        if (slot_wide <= UINT32_MAX && static_cast<uint32_t>(slot_wide) == receiver) {
          sample.slot_index = static_cast<int32_t>(index);
          break;
        }
      }
    }
  }

  if (sample.slot_index >= 0)
    g_input_dispatch_gate_slot_matches.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard lock(g_state_mutex);
  constexpr std::size_t kMaximumDispatchGateSamples = 64;
  auto existing = std::find_if(g_state.input_dispatch_gate_trace.begin(),
      g_state.input_dispatch_gate_trace.end(), [&sample](const auto& prior) {
        return prior.function == sample.function && prior.phase == sample.phase &&
               prior.callsite == sample.callsite && prior.receiver == sample.receiver &&
               prior.slot_index == sample.slot_index &&
               prior.field96_bits == sample.field96_bits &&
               prior.field140 == sample.field140 && prior.field144 == sample.field144 &&
               prior.field148 == sample.field148 && prior.field152 == sample.field152 &&
               prior.field153 == sample.field153 && prior.source_file == sample.source_file &&
               prior.source_line == sample.source_line;
      });
  if (existing != g_state.input_dispatch_gate_trace.end()) {
    if (existing->samples < UINT32_MAX) ++existing->samples;
  } else if (g_state.input_dispatch_gate_trace.size() < kMaximumDispatchGateSamples) {
    sample.samples = 1;
    g_state.input_dispatch_gate_trace.push_back(std::move(sample));
  }
}

// Sample the user-action objects and their virtual slot-0 callbacks after the
// title's input-manager dispatch. This read-only correlation trace helps show
// which callback owns the action records; it never invokes guest code.
void ObserveInputManagerCallbacks(uint8_t* base, uint32_t manager, uint32_t callsite) {
  if (!g_trace_input_consumers.load(std::memory_order_relaxed) ||
      callsite != 0x820A6B98) return;
  using edf::console::GuestMemory;
  const GuestMemory memory(base);
  if (!GuestMemory::Heap(manager, 8) && !GuestMemory::Image(manager, 8)) return;
  const uint32_t sentinel = memory.U32(manager + 4);
  if ((!GuestMemory::Heap(sentinel, 16) && !GuestMemory::Image(sentinel, 16))) return;

  uint32_t node = memory.U32(sentinel);
  for (uint32_t count = 0; count < 16 && node != sentinel; ++count) {
    if (!GuestMemory::Heap(node, 16) && !GuestMemory::Image(node, 16)) break;
    const uint32_t object = memory.U32(node + 12);
    if ((GuestMemory::Heap(object, 4) || GuestMemory::Image(object, 4))) {
      const uint32_t vtable = memory.U32(object);
      if (GuestMemory::Image(vtable, 4)) {
        edf::net::probe::InputManagerCallbackSample sample;
        sample.callsite = callsite;
        sample.manager = manager;
        sample.record = node;
        sample.object = object;
        sample.vtable = vtable;
        sample.slot0 = memory.U32(vtable);
        sample.class_name = memory.ClassName(object);
        std::lock_guard lock(g_state_mutex);
        auto existing = std::find_if(g_state.input_manager_callbacks.begin(),
            g_state.input_manager_callbacks.end(), [&sample](const auto& prior) {
              return prior.object == sample.object && prior.slot0 == sample.slot0;
            });
        if (existing != g_state.input_manager_callbacks.end()) {
          if (existing->samples < UINT32_MAX) ++existing->samples;
        } else if (g_state.input_manager_callbacks.size() < 16) {
          sample.samples = 1;
          g_state.input_manager_callbacks.push_back(std::move(sample));
        }
      }
    }
    node = memory.U32(node);
  }
}

void ObserveTitleActionGates(uint8_t* base) {
  if (!g_trace_input_consumers.load(std::memory_order_relaxed)) return;
  using edf::console::GuestMemory;
  constexpr uint32_t kTitleStateGlobal = 0x8257C030;
  constexpr uint32_t kInputManagerGlobal = 0x82578640;
  // Match sub_820A6B10's loads at -31068 and -31060 from 0x82580000.
  constexpr uint32_t kGateAObjectGlobal = 0x825786A4;
  constexpr uint32_t kGateBObjectGlobal = 0x825786AC;
  constexpr uint32_t kTimerGlobal = 0x8257869C;
  const GuestMemory memory(base);
  bool gate_a_valid = false;
  bool gate_b_valid = false;
  bool timer_valid = false;
  g_title_action_gate_2260_last.store(0, std::memory_order_relaxed);
  g_title_action_gate_2261_last.store(0, std::memory_order_relaxed);
  g_title_action_gate_2262_last.store(0, std::memory_order_relaxed);
  g_title_action_gate_a_link.store(0, std::memory_order_relaxed);
  g_title_action_gate_a_state.store(0, std::memory_order_relaxed);
  g_title_action_gate_a_result.store(0, std::memory_order_relaxed);
  g_title_action_gate_b_712.store(0, std::memory_order_relaxed);
  g_title_action_gate_b_696.store(0, std::memory_order_relaxed);
  g_title_action_gate_b_708.store(0, std::memory_order_relaxed);
  g_title_action_gate_b_result.store(0, std::memory_order_relaxed);
  g_title_action_gate_sequence_pointer.store(0, std::memory_order_relaxed);
  g_title_action_gate_sequence_708.store(0, std::memory_order_relaxed);
  g_title_action_gate_sequence_result.store(0, std::memory_order_relaxed);
  g_title_action_gate_input_manager.store(0, std::memory_order_relaxed);
  g_title_action_gate_input_count.store(0, std::memory_order_relaxed);
  g_title_action_gate_r31.store(0, std::memory_order_relaxed);
  g_title_action_gate_r25.store(0, std::memory_order_relaxed);
  g_title_action_gate_estimates_valid.store(false, std::memory_order_relaxed);
  g_title_action_gate_timer_valid.store(false, std::memory_order_relaxed);
  g_title_action_gate_timer_a.store(0, std::memory_order_relaxed);
  g_title_action_gate_timer_b.store(0, std::memory_order_relaxed);
  g_title_action_gate_timer_branch_taken.store(0, std::memory_order_relaxed);
  if (!GuestMemory::Image(kTitleStateGlobal, 4)) {
    g_title_action_gate_state_object.store(0, std::memory_order_relaxed);
    g_title_action_gate_state_vtable.store(0, std::memory_order_relaxed);
    g_title_action_gate_a_object.store(0, std::memory_order_relaxed);
    g_title_action_gate_b_object.store(0, std::memory_order_relaxed);
    std::lock_guard lock(g_state_mutex);
    g_state.title_action_gate_b_class_name.clear();
    g_state.title_action_gate_state_class_name.clear();
    return;
  }
  const uint32_t state = memory.U32(kTitleStateGlobal);
  if (!GuestMemory::Heap(state, 2263) && !GuestMemory::Image(state, 2263)) {
    g_title_action_gate_state_object.store(0, std::memory_order_relaxed);
    g_title_action_gate_state_vtable.store(0, std::memory_order_relaxed);
    g_title_action_gate_a_object.store(0, std::memory_order_relaxed);
    g_title_action_gate_b_object.store(0, std::memory_order_relaxed);
    std::lock_guard lock(g_state_mutex);
    g_state.title_action_gate_b_class_name.clear();
    g_state.title_action_gate_state_class_name.clear();
    return;
  }
  g_title_action_gate_samples.fetch_add(1, std::memory_order_relaxed);
  g_title_action_gate_2260_last.store(memory.U8(state + 2260), std::memory_order_relaxed);
  g_title_action_gate_2261_last.store(memory.U8(state + 2261), std::memory_order_relaxed);
  g_title_action_gate_2262_last.store(memory.U8(state + 2262), std::memory_order_relaxed);

  const uint32_t state_vtable = memory.U32(state);
  const uint32_t previous_state =
      g_title_action_gate_state_object.exchange(state, std::memory_order_relaxed);
  const uint32_t previous_vtable =
      g_title_action_gate_state_vtable.exchange(state_vtable, std::memory_order_relaxed);
  if (previous_state != state || previous_vtable != state_vtable) {
    const std::string class_name = memory.ClassName(state);
    std::lock_guard lock(g_state_mutex);
    g_state.title_action_gate_state_class_name = class_name;
  }

  if (GuestMemory::Image(kInputManagerGlobal, 4)) {
    const uint32_t manager = memory.U32(kInputManagerGlobal);
    if (GuestMemory::Heap(manager, 2340) || GuestMemory::Image(manager, 2340)) {
      g_title_action_gate_input_manager.store(manager, std::memory_order_relaxed);
      g_title_action_gate_input_count.store(memory.U32(manager + 2336), std::memory_order_relaxed);
    }
  }

  if (GuestMemory::Image(kGateAObjectGlobal, 4)) {
    const uint32_t object = memory.U32(kGateAObjectGlobal);
    if (GuestMemory::Heap(object, 664) || GuestMemory::Image(object, 664)) {
      g_title_action_gate_a_object.store(object, std::memory_order_relaxed);
      const uint32_t link = memory.U32(object + 660);
      g_title_action_gate_a_link.store(link, std::memory_order_relaxed);
      if (GuestMemory::Heap(link, 180) || GuestMemory::Image(link, 180)) {
        gate_a_valid = true;
        const uint32_t linked_state = memory.U32(link + 176);
        g_title_action_gate_a_state.store(linked_state, std::memory_order_relaxed);
        g_title_action_gate_a_result.store(linked_state == 1 || linked_state == 2,
                                           std::memory_order_relaxed);
      }
    }
    else {
      g_title_action_gate_a_object.store(0, std::memory_order_relaxed);
    }
  } else {
    g_title_action_gate_a_object.store(0, std::memory_order_relaxed);
  }

  if (GuestMemory::Image(kGateBObjectGlobal, 4)) {
    const uint32_t object = memory.U32(kGateBObjectGlobal);
    if (GuestMemory::Heap(object, 716) || GuestMemory::Image(object, 716)) {
      gate_b_valid = true;
      const uint32_t previous_object =
          g_title_action_gate_b_object.exchange(object, std::memory_order_relaxed);
      if (previous_object != object) {
        const std::string class_name = memory.ClassName(object);
        std::lock_guard lock(g_state_mutex);
        g_state.title_action_gate_b_class_name = class_name;
      }
      g_title_action_gate_sequence_pointer.store(object, std::memory_order_relaxed);
      const uint32_t state712 = memory.U32(object + 712);
      const uint32_t state696 = memory.U8(object + 696);
      const uint32_t state708 = memory.U32(object + 708);
      g_title_action_gate_b_712.store(state712, std::memory_order_relaxed);
      g_title_action_gate_b_696.store(state696, std::memory_order_relaxed);
      g_title_action_gate_b_708.store(state708, std::memory_order_relaxed);
      g_title_action_gate_b_result.store(state712 == 1 && state696 != 0,
                                         std::memory_order_relaxed);
      g_title_action_gate_sequence_708.store(state708, std::memory_order_relaxed);
      g_title_action_gate_sequence_result.store(state712 == 1 && state696 != 0,
                                                std::memory_order_relaxed);
    } else {
      const uint32_t previous_object =
          g_title_action_gate_b_object.exchange(0, std::memory_order_relaxed);
      if (previous_object != 0) {
        std::lock_guard lock(g_state_mutex);
        g_state.title_action_gate_b_class_name.clear();
      }
    }
  } else {
    const uint32_t previous_object =
        g_title_action_gate_b_object.exchange(0, std::memory_order_relaxed);
    if (previous_object != 0) {
      std::lock_guard lock(g_state_mutex);
      g_state.title_action_gate_b_class_name.clear();
    }
  }

  if (GuestMemory::Image(kTimerGlobal, 4)) {
    const uint32_t timer = memory.U32(kTimerGlobal);
    if (GuestMemory::Heap(timer, 36) || GuestMemory::Image(timer, 36)) {
      g_title_action_gate_timer_a.store(memory.U32(timer + 20), std::memory_order_relaxed);
      g_title_action_gate_timer_b.store(memory.U32(timer + 32), std::memory_order_relaxed);
      timer_valid = true;
    }
  }

  const bool title_action_early_return = memory.U8(state + 2261) != 0 ||
                                          memory.U8(state + 2262) != 0;
  if (!title_action_early_return && gate_a_valid && gate_b_valid) {
    const bool gate_a = g_title_action_gate_a_result.load(std::memory_order_relaxed) != 0;
    const bool gate_b = g_title_action_gate_b_result.load(std::memory_order_relaxed) != 0;
    const uint32_t sequence_phase = g_title_action_gate_b_708.load(std::memory_order_relaxed);
    g_title_action_gate_r31.store(gate_a && !gate_b ? 0 : 1, std::memory_order_relaxed);
    g_title_action_gate_r25.store(gate_b && sequence_phase != 1 ? 0 : 1,
                                  std::memory_order_relaxed);
    g_title_action_gate_estimates_valid.store(true, std::memory_order_relaxed);
  }
  g_title_action_gate_timer_valid.store(timer_valid, std::memory_order_relaxed);
  g_title_action_gate_timer_branch_taken.store(
      timer_valid && g_title_action_gate_timer_a.load(std::memory_order_relaxed) !=
                         g_title_action_gate_timer_b.load(std::memory_order_relaxed),
      std::memory_order_relaxed);
}

std::deque<QueuedActorSpawn> g_actor_spawn_queue;
std::deque<QueuedActorDamage> g_actor_damage_queue;
constexpr size_t kMaxPendingActorDamage = 64;
constexpr size_t kMaxActorDamageApplicationsPerSample = 16;
uint64_t g_actor_mission_generation = 1;
uint64_t g_next_actor_damage_request_id = 0;
bool g_actor_world_ready = false;
struct AuthoritativeActorTrack {
  uint32_t id = 0;
  uint32_t vtable = 0;
};
std::unordered_map<uint32_t, AuthoritativeActorTrack> g_authoritative_actor_ids;
size_t g_actor_spawn_reservations = 0;  // queued, in-flight, or live guest actors
constexpr uint32_t kMissionActorIdNamespace = 0x80000000u;
#if defined(EDF2027_AUTHORITATIVE_HOST)
constexpr uint32_t kMissionActorIdSerialMask = 0x7fffffffu;
#endif
struct MissionActorTrack {
  uint32_t id = 0;
  uint32_t vtable = 0;
  uint64_t mission_generation = 0;
};
std::unordered_map<uint32_t, MissionActorTrack> g_mission_actor_ids;
constexpr uint32_t kMissionActorTraceCallLimit = 4096;
constexpr uint32_t kMissionActorTraceAntLimit = 64;
std::atomic<uint32_t> g_mission_actor_spawn_calls{0},g_mission_ant_spawn_samples{0};
std::atomic<uint32_t> g_mission_actor_trace_removals{0};
std::atomic<uint64_t> g_mission_actor_trace_generation{0};
std::atomic<bool> g_mission_actor_trace_pending{false};
struct MissionAntTraceObject {
  uint64_t generation = 0;
  uint32_t call_ordinal = 0, ant_ordinal = 0, vtable = 0;
};
std::unordered_map<uint32_t, MissionAntTraceObject> g_mission_actor_trace_objects;
struct MissionAntSpawnTraceRecord {
  uint64_t generation = 0, mission_generation = 0;
  uint32_t call_ordinal = 0, ant_ordinal = 0, caller = 0;
  uint32_t r3 = 0, r4 = 0, r5 = 0, r6 = 0, r7 = 0;
  uint64_t f1_bits = 0;
  uint32_t object = 0, vtable = 0, faction = 0;
  uint8_t awaken_latch_after = 0;
  float health = 0, x = 0, y = 0, z = 0, yaw = 0;
  bool yaw_valid = false;
};
std::mutex g_mission_actor_trace_mutex;
std::vector<MissionAntSpawnTraceRecord> g_mission_actor_trace_records;
std::mutex g_mission_actor_trace_log_mutex;
uint64_t g_mission_actor_trace_log_generation = UINT64_MAX;
uint32_t g_mission_actor_trace_log_count = 0;
constexpr uint32_t kMissionActorTraceLogLimit = 512;
#if defined(EDF2027_AUTHORITATIVE_HOST)
constexpr const char* kMissionActorTraceSide = "authoritative_host";
#else
constexpr const char* kMissionActorTraceSide = "headless_probe";
#endif

uint64_t ActorMissionGeneration() {
  std::lock_guard lock(g_actor_spawn_mutex);
  return g_actor_mission_generation;
}

bool ReserveMissionActorTraceLog(uint64_t generation) {
  if (!REXCVAR_GET(edf_mp_actor_trace) ||
      generation != g_mission_actor_trace_generation.load(std::memory_order_relaxed)) return false;
  std::lock_guard lock(g_mission_actor_trace_log_mutex);
  if (generation != g_mission_actor_trace_generation.load(std::memory_order_relaxed)) return false;
  if (g_mission_actor_trace_log_generation != generation) {
    g_mission_actor_trace_log_generation = generation;
    g_mission_actor_trace_log_count = 0;
  }
  if (g_mission_actor_trace_log_count >= kMissionActorTraceLogLimit) return false;
  ++g_mission_actor_trace_log_count;
  return true;
}

void LogMissionActorLifecycle(const char* event, uint64_t trace_generation,
                              uint64_t mission_generation, uint32_t actor_id,
                              uint32_t object, uint32_t expected_vtable,
                              uint32_t current_vtable, uint32_t call_ordinal,
                              uint32_t ant_ordinal, const char* reason = "-",
                              float damage = 0.0f, uint64_t request_id = 0,
                              uint32_t guest_call_return = 0,
                              bool guest_call_return_observed = false) {
  if (!ReserveMissionActorTraceLog(trace_generation)) return;
  if (guest_call_return_observed) {
    REXLOG_INFO(
        "Mission actor lifecycle: side=authoritative_host event={} trace_generation={} mission_generation={} actor_id={} object={:#x} expected_vtable={:#x} current_vtable={:#x} call_ordinal={} ant_ordinal={} reason={} damage={} request_id={} guest_call_return_observed={:#x}",
        event, trace_generation, mission_generation, actor_id, object, expected_vtable,
        current_vtable, call_ordinal, ant_ordinal, reason, damage, request_id,
        guest_call_return);
  } else {
    REXLOG_INFO(
        "Mission actor lifecycle: side=authoritative_host event={} trace_generation={} mission_generation={} actor_id={} object={:#x} expected_vtable={:#x} current_vtable={:#x} call_ordinal={} ant_ordinal={} reason={} damage={} request_id={}",
        event, trace_generation, mission_generation, actor_id, object, expected_vtable,
        current_vtable, call_ordinal, ant_ordinal, reason, damage, request_id);
  }
}

void ResetMissionActorTrace() {
  g_mission_actor_trace_generation.fetch_add(1, std::memory_order_relaxed);
  g_mission_actor_spawn_calls.store(0, std::memory_order_relaxed);
  g_mission_ant_spawn_samples.store(0, std::memory_order_relaxed);
  g_mission_actor_trace_removals.store(0, std::memory_order_relaxed);
  g_mission_actor_trace_objects.clear();
  std::lock_guard lock(g_mission_actor_trace_mutex);
  g_mission_actor_trace_records.clear();
  g_mission_actor_trace_pending.store(false, std::memory_order_relaxed);
}

void QueueMissionAntSpawnTrace(MissionAntSpawnTraceRecord record) {
  std::lock_guard lock(g_mission_actor_trace_mutex);
  g_mission_actor_trace_records.push_back(record);
  g_mission_actor_trace_pending.store(true, std::memory_order_release);
}

void FlushMissionActorTrace() {
  if (!g_mission_actor_trace_pending.exchange(false, std::memory_order_acq_rel)) return;
  std::vector<MissionAntSpawnTraceRecord> records;
  {
    std::lock_guard lock(g_mission_actor_trace_mutex);
    records.swap(g_mission_actor_trace_records);
  }
  for (const auto& record : records) {
    if (!ReserveMissionActorTraceLog(record.generation)) continue;
    REXLOG_INFO(
        "Mission ant identity trace: side={} event=ant_spawn_sample trace_generation={} mission_generation={} actor_id=0 call_ordinal={} ant_ordinal={} caller={:#x} args=[{:#x},{:#x},{:#x},{:#x},{:#x}] f1_bits={:#x} r7_awaken_gate={} object={:#x} expected_vtable={:#x} current_vtable={:#x} class=clGiantAnt faction={} awaken_latch_after={} health={} x={} y={} z={} yaw_valid={} yaw_rad={}",
        kMissionActorTraceSide, record.generation, record.mission_generation,
        record.call_ordinal, record.ant_ordinal, record.caller, record.r3, record.r4,
        record.r5, record.r6, record.r7, record.f1_bits, record.r7 != 0,
        record.object, record.vtable, record.vtable, record.faction,
        record.awaken_latch_after, record.health, record.x, record.y, record.z,
        record.yaw_valid, record.yaw);
  }
}
// Never recycle mission IDs, even after the mission registry is cleared. That
// keeps a stale Luau token harmless if guest memory later reuses an object address.
#if defined(EDF2027_AUTHORITATIVE_HOST)
uint32_t g_next_mission_actor_serial = 1;
#endif

void ReleaseActorSpawnReservations(size_t count) {
  std::lock_guard lock(g_actor_spawn_mutex);
  g_actor_spawn_reservations = count > g_actor_spawn_reservations
      ? 0 : g_actor_spawn_reservations - count;
}

void ReleaseActorSpawnReservation() {
  ReleaseActorSpawnReservations(1);
}

void ForgetAuthoritativeActor(uint32_t object) {
  if (!g_authoritative_actor_ids.erase(object)) return;
  ReleaseActorSpawnReservation();
}

void ClearAuthoritativeActors() {
  const size_t tracked = g_authoritative_actor_ids.size();
  g_authoritative_actor_ids.clear();
  std::lock_guard lock(g_actor_spawn_mutex);
  g_actor_spawn_reservations = tracked > g_actor_spawn_reservations
      ? 0 : g_actor_spawn_reservations - tracked;
}

void ForgetMissionActor(uint32_t object) {
  g_mission_actor_ids.erase(object);
  g_mission_actor_trace_objects.erase(object);
}

void ClearMissionActors() {
  g_mission_actor_ids.clear();
  g_mission_actor_trace_objects.clear();
}

struct ActorSpawnReservationBatch {
  explicit ActorSpawnReservationBatch(size_t count) : remaining(count) {}
  size_t remaining = 0;
  void TransferOneToLiveActor() {
    if (remaining) --remaining;
  }
  void ReleaseOne() {
    if (!remaining) return;
    --remaining;
    ReleaseActorSpawnReservation();
  }
  ~ActorSpawnReservationBatch() {
    ReleaseActorSpawnReservations(remaining);
  }
};

REX_EXTERN(sub_820A0D10);
REX_EXTERN(sub_820A0858);
REX_EXTERN(sub_820AFAC0);
REX_EXTERN(sub_821A6E08);
REX_EXTERN(sub_821E9348);
REX_EXTERN(sub_820D5430);
REX_EXTERN(sub_820D5ED0);
REX_EXTERN(sub_820D4A00);
REX_EXTERN(sub_820D59D0);
REX_EXTERN(__imp__sub_820CD448);
REX_EXTERN(__imp__sub_820C59F0);
REX_EXTERN(__imp__sub_820C5A70);
REX_EXTERN(__imp__sub_820C5B28);
REX_EXTERN(__imp__sub_820CEBD0);
REX_EXTERN(__imp__sub_821C0ED8);

struct Vec3 { float x = 0, y = 0, z = 0; };
constexpr std::size_t kTrackedHostileSlots = 128;
struct ActorPosition { uint32_t object = 0, vtable = 0; Vec3 position{}; };
struct ActorTrack { uint32_t object = 0, vtable = 0, ready_samples = 0; Vec3 origin{}; };
std::array<ActorTrack, kTrackedHostileSlots> g_hostile_tracks{};
struct PlayerMotionTrack {
  uint32_t object = 0;
  uint32_t ready_samples = 0;
  Vec3 origin{};
};
PlayerMotionTrack g_input_user_player_track{};
bool g_input_user_player_motion_seen = false;

void UpdateActorSpawnWorldReadiness(bool ready) {
  std::deque<QueuedActorDamage> cancelled_damage;
  {
    std::lock_guard lock(g_actor_spawn_mutex);
    if (!ready) {
      if (g_actor_world_ready) {
        ++g_actor_mission_generation;
        if (!g_actor_mission_generation) ++g_actor_mission_generation;
      }
      const size_t canceled = g_actor_spawn_queue.size();
      g_actor_spawn_queue.clear();
      cancelled_damage.swap(g_actor_damage_queue);
      g_actor_spawn_reservations = canceled > g_actor_spawn_reservations
          ? 0 : g_actor_spawn_reservations - canceled;
    }
    g_actor_world_ready = ready;
  }
#if defined(EDF2027_AUTHORITATIVE_HOST)
  const uint64_t trace_generation =
      g_mission_actor_trace_generation.load(std::memory_order_relaxed);
  for (const QueuedActorDamage& queued : cancelled_damage) {
    if ((queued.actor_id & kMissionActorIdNamespace) == 0) continue;
    LogMissionActorLifecycle("damage_dropped", trace_generation,
        queued.mission_generation, queued.actor_id, 0, 0, 0, 0, 0,
        "world_not_ready", queued.damage_points, queued.request_id);
  }
#endif
}

constexpr uint32_t kCorePointer = 0x8257C030;
constexpr uint32_t kFactionManagerPointer = 0x82578720;
constexpr uint32_t kFactionCount = 5, kFactionRecordBytes = 40, kFactionNodeOffset = 608;
constexpr uint32_t kMaxFactionNodes = 4096;
constexpr uint64_t kMissionSampleInterval = 8;

struct WorldSample {
  bool loading = true;
  bool has_player = false;
  bool player_alive = false;
  bool primary_player_available = false;
  bool primary_player_alive = false;
  uint32_t hostile_actors = 0;
  uint32_t player_object = 0;
  Vec3 player_position{};
  int16_t player_yaw_mrad = 0;
  std::vector<edf::net::probe::GuestPlayerObjectSample> player_objects;
  std::array<ActorPosition, kTrackedHostileSlots> hostiles{};
  std::size_t tracked_hostile_count = 0;
  std::vector<edf::net::ActorState> authoritative_actors;
  std::vector<edf::net::ActorState> mission_actors;
};

bool Finite(Vec3 p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }

uint32_t FindPlayerCamera(const edf::console::GuestMemory& memory) {
  const uint32_t core = memory.U32(kCorePointer);
  if (!edf::console::GuestMemory::Heap(core, 16)) return 0;
  const uint32_t end = memory.U32(core + 12);
  uint32_t node = memory.U32(core);
  for (uint32_t count = 0; node && node != end && count < 64; ++count) {
    if (!edf::console::GuestMemory::Heap(node, 12)) break;
    const uint32_t object = memory.U32(node + 8);
    if (memory.ClassName(object) == "clPlayerCamera") return object;
    node = memory.U32(node);
  }
  return 0;
}

int16_t SamplePlayerYawMrad(const edf::console::GuestMemory& memory) {
  const uint32_t camera = FindPlayerCamera(memory);
  if (!edf::console::GuestMemory::Heap(camera, 480)) return 0;
  const float forward_x = memory.F32(camera + 416 + 32);
  const float forward_z = memory.F32(camera + 416 + 40);
  if (!std::isfinite(forward_x) || !std::isfinite(forward_z) ||
      (std::abs(forward_x) < 1e-6f && std::abs(forward_z) < 1e-6f)) return 0;
  return static_cast<int16_t>(std::lround(std::atan2(forward_x, forward_z) * 1000.0));
}

bool ApplyPositionReset(const edf::console::GuestMemory& memory, uint32_t object,
                        const edf::net::probe::NetworkPositionReset& reset, bool update_camera) {
  if (!edf::console::GuestMemory::Heap(object, 1452)) return false;
  const float x = float(reset.x_cm) * 0.01f;
  const float y = float(reset.y_cm) * 0.01f;
  const float z = float(reset.z_cm) * 0.01f;
  const float yaw = float(reset.yaw_mrad) * 0.001f;
  const float c = std::cos(yaw), s = std::sin(yaw);
  const float rows[16] = {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, x, y, z, 1};
  for (uint32_t i = 0; i < 16; ++i) memory.WF32(object + 224 + i * 4, rows[i]);
  memory.WF32(object + 1440, x);
  memory.WF32(object + 1444, y);
  memory.WF32(object + 1448, z);
  if (!update_camera) return true;
  const uint32_t camera = FindPlayerCamera(memory);
  if (!edf::console::GuestMemory::Heap(camera, 480)) return false;
  const float forward_x = memory.F32(camera + 416 + 32);
  const float forward_z = memory.F32(camera + 416 + 40);
  if (!std::isfinite(forward_x) || !std::isfinite(forward_z) ||
      (std::abs(forward_x) < 1e-6f && std::abs(forward_z) < 1e-6f)) return false;
  constexpr float kTwoPi = 6.2831853071795864769f;
  const float delta = std::remainder(yaw - std::atan2(forward_x, forward_z), kTwoPi);
  const float rotate_c = std::cos(delta), rotate_s = std::sin(delta);
  for (uint32_t row = 0; row < 3; ++row) {
    const uint32_t address = camera + 416 + row * 16;
    const float row_x = memory.F32(address);
    const float row_z = memory.F32(address + 8);
    if (!std::isfinite(row_x) || !std::isfinite(row_z)) return false;
    memory.WF32(address, rotate_c * row_x + rotate_s * row_z);
    memory.WF32(address + 8, -rotate_s * row_x + rotate_c * row_z);
  }
  return true;
}

#if defined(EDF2027_AUTHORITATIVE_HOST)
bool SpawnPlayerObjectForProbe(PPCContext& parent, uint8_t* base, uint32_t player_index,
                               float x, float y, float z, float yaw) {
  if (player_index < 1 || player_index > 3 || !std::isfinite(x) || !std::isfinite(y) ||
      !std::isfinite(z) || !std::isfinite(yaw)) return false;
  const edf::console::GuestMemory memory(base);
  edf::console::GuestCalls guest(parent, base);
  const uint32_t mark = guest.Mark();
  uint32_t object = 0;
  uint32_t path = 0;
  bool path_initialized = false;
  bool path_destroyed = false;
  bool accepted = false;
  try {
    const std::string path_text = "game:\\Object\\Player01.Sgo";
    const uint32_t literal = guest.WideString(path_text);
    path = guest.Alloc(32);
    memory.W32(path + 24, 7);
    guest.CallAddress(0x820A0D10, edf::console::GuestCalls::R(
        {path, literal, static_cast<uint32_t>(path_text.size())}));
    path_initialized = true;
    guest.CallAddress(0x820AFAC0, edf::console::GuestCalls::R(
        {memory.U32(0x82578648), path, 1}));

    const uint32_t matrix = guest.Alloc(64);
    const uint32_t params = guest.Alloc(80);
    const float c = std::cos(yaw), s = std::sin(yaw);
    const float rows[16] = {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, x, y, z, 1};
    for (uint32_t i = 0; i < 16; ++i) memory.WF32(matrix + i * 4, rows[i]);
    for (uint32_t i = 0; i < 4; ++i) memory.WF32(params + i * 20, 1.0f);
    object = guest.CallAddress(0x821A6E08, edf::console::GuestCalls::R(
        {memory.U32(0x8257C030), matrix, path, params}));
    const uint32_t game_object = object ? guest.CallAddress(0x821E9348,
        edf::console::GuestCalls::R({object, 0, 0x82550D14, 0x82551058, 0})) : 0;
    if (game_object && edf::console::GuestMemory::Heap(game_object, 1452) &&
        memory.ClassName(game_object) == "clPlayerObject") {
      guest.CallAddress(0x820D5430, edf::console::GuestCalls::R({game_object}, {1.0}));
      guest.CallAddress(0x820D5ED0, edf::console::GuestCalls::R({game_object, 0, 1}));
      memory.W32(game_object + 492, player_index);
      guest.CallAddress(0x820D4A00, edf::console::GuestCalls::R({game_object}));
      accepted = memory.ClassName(game_object) == "clPlayerObject" &&
          std::bit_cast<int32_t>(memory.U32(game_object + 492)) == static_cast<int32_t>(player_index);
    }
    guest.CallAddress(0x820A0858, edf::console::GuestCalls::R({path}));
    path_destroyed = true;
  } catch (...) {
    if (path_initialized && !path_destroyed) {
      try { guest.CallAddress(0x820A0858, edf::console::GuestCalls::R({path})); } catch (...) {}
    }
    if (object && edf::console::GuestMemory::Heap(object, 4)) {
      try { guest.CallAddress(0x821C0ED8, edf::console::GuestCalls::R({object})); } catch (...) {}
    }
    guest.Release(mark);
    return false;
  }
  if (!accepted && object && edf::console::GuestMemory::Heap(object, 4)) {
    try { guest.CallAddress(0x821C0ED8, edf::console::GuestCalls::R({object})); } catch (...) {}
  }
  guest.Release(mark);
  return accepted;
}

void RunPlayerSpawnProbe(PPCContext& ctx, uint8_t* base, const WorldSample& world) {
  const uint32_t player_index = g_player_spawn_probe_index.load(std::memory_order_relaxed);
  if (!player_index || g_player_spawn_probe_attempted.load(std::memory_order_relaxed) ||
      world.loading || !world.primary_player_available || !world.primary_player_alive) return;
  const auto existing = std::find_if(world.player_objects.begin(), world.player_objects.end(),
      [player_index](const auto& player) { return player.player_index == static_cast<int32_t>(player_index); });
  if (existing != world.player_objects.end()) {
    g_player_spawn_probe_attempted.store(true, std::memory_order_relaxed);
    REXLOG_INFO("Player seat probe: requested index already exists in the live guest");
    return;
  }
  const auto primary = std::find_if(world.player_objects.begin(), world.player_objects.end(),
      [](const auto& player) { return player.player_index == 0; });
  if (primary == world.player_objects.end() || primary->ambiguous || !primary->object || !primary->alive) return;
  g_player_spawn_probe_attempted.store(true, std::memory_order_relaxed);
  const bool created = SpawnPlayerObjectForProbe(ctx, base, player_index,
      primary->x + 2.0f, primary->y, primary->z, float(primary->yaw_mrad) * 0.001f);
  REXLOG_INFO("{}", created
      ? "Player seat probe: Player01.Sgo created a unique clPlayerObject at the requested input index"
      : "Player seat probe: Player01.Sgo did not create a validated clPlayerObject at the requested input index");
}
#endif

bool PositionResetMatches(const edf::console::GuestMemory& memory, uint32_t object,
                          const edf::net::probe::NetworkPositionReset& reset, bool verify_camera) {
  if (!edf::console::GuestMemory::Heap(object, 1452)) return false;
  constexpr float kPositionToleranceMeters = 0.25f;
  constexpr float kYawToleranceRadians = 0.02f;
  constexpr float kTwoPi = 6.2831853071795864769f;
  const float x = float(reset.x_cm) * 0.01f;
  const float y = float(reset.y_cm) * 0.01f;
  const float z = float(reset.z_cm) * 0.01f;
  const auto near_position = [](float actual, float expected) {
    return std::isfinite(actual) && std::isfinite(expected) &&
           std::abs(actual - expected) <= kPositionToleranceMeters;
  };
  if (!near_position(memory.F32(object + 272), x) ||
      !near_position(memory.F32(object + 276), y) ||
      !near_position(memory.F32(object + 280), z) ||
      !near_position(memory.F32(object + 1440), x) ||
      !near_position(memory.F32(object + 1444), y) ||
      !near_position(memory.F32(object + 1448), z)) return false;
  const float body_x = memory.F32(object + 224 + 32);
  const float body_z = memory.F32(object + 224 + 40);
  if (!std::isfinite(body_x) || !std::isfinite(body_z) ||
      (std::abs(body_x) < 1e-6f && std::abs(body_z) < 1e-6f)) return false;
  const float body_yaw = std::atan2(body_x, body_z);
  const float expected_yaw = float(reset.yaw_mrad) * 0.001f;
  if (std::abs(std::remainder(body_yaw - expected_yaw, kTwoPi)) > kYawToleranceRadians) return false;
  if (!verify_camera) return true;
  const uint32_t camera = FindPlayerCamera(memory);
  if (!edf::console::GuestMemory::Heap(camera, 480)) return false;
  const float camera_x = memory.F32(camera + 416 + 32);
  const float camera_z = memory.F32(camera + 416 + 40);
  if (!std::isfinite(camera_x) || !std::isfinite(camera_z) ||
      (std::abs(camera_x) < 1e-6f && std::abs(camera_z) < 1e-6f)) return false;
  const float camera_yaw = std::atan2(camera_x, camera_z);
  return std::abs(std::remainder(camera_yaw - expected_yaw, kTwoPi)) <= kYawToleranceRadians;
}

bool SafeInputStatePointer(uint32_t pointer, uint32_t stack_pointer) {
  constexpr uint32_t size = sizeof(rex::input::X_INPUT_STATE);
  if (!pointer || (pointer & 3u) || uint64_t(pointer) + size > uint64_t(UINT32_MAX) + 1) return false;
  const uint32_t distance = pointer > stack_pointer ? pointer - stack_pointer : stack_pointer - pointer;
  // The XInput wrapper's out parameter is a guest stack local in this title.
  return distance <= 0x10000u;
}

void InjectInput(rex::input::X_INPUT_STATE* state, const edf::net::InputCommand& input) {
  state->packet_number = ++g_input_packet;
  state->gamepad = {};
  state->gamepad.buttons = input.buttons;
  state->gamepad.left_trigger = input.left_trigger;
  state->gamepad.right_trigger = input.right_trigger;
  state->gamepad.thumb_lx = input.move_x;
  state->gamepad.thumb_ly = input.move_y;
  state->gamepad.thumb_rx = input.aim_x;
  state->gamepad.thumb_ry = input.aim_y;
}

WorldSample SampleWorld(uint8_t* base) {
  using edf::console::GuestMemory;
  const GuestMemory memory(base);
  WorldSample sample;
#if defined(EDF2027_AUTHORITATIVE_HOST)
  const uint64_t mission_generation = ActorMissionGeneration();
#endif

  const uint32_t core = memory.U32(kCorePointer);
  if (GuestMemory::Heap(core, 2264))
    sample.loading = memory.U8(core + 2260) || memory.U8(core + 2261) || memory.U8(core + 2262);
  if (sample.loading) {
    ClearAuthoritativeActors();
    ClearMissionActors();
    return sample;
  }

  const uint32_t manager = memory.U32(kFactionManagerPointer);
  if (!GuestMemory::Heap(manager)) { ClearAuthoritativeActors(); ClearMissionActors(); return sample; }
  const uint32_t records = memory.U32(manager);
  if (!GuestMemory::Heap(records, kFactionCount * kFactionRecordBytes)) {
    ClearAuthoritativeActors();
    ClearMissionActors();
    return sample;
  }

  std::vector<uint32_t> live_authoritative_objects;
#if defined(EDF2027_AUTHORITATIVE_HOST)
  std::vector<uint32_t> live_mission_objects;
#endif
  for (uint32_t faction : {0u, 1u}) {
    uint32_t node = memory.U32(records + faction * kFactionRecordBytes);
    for (uint32_t count = 0; node && count < kMaxFactionNodes; ++count) {
      if (!GuestMemory::Heap(node, 8) || node < kFactionNodeOffset) break;
      const uint32_t object = node - kFactionNodeOffset;
      if (!GuestMemory::Heap(object, 628)) break;
      const uint32_t next = memory.U32(node);
      const std::string class_name = memory.ClassName(object);
      const float health = memory.F32(object + 592);
      const Vec3 position{memory.F32(object + 272), memory.F32(object + 276), memory.F32(object + 280)};
      if (faction == 0 && class_name == "clPlayerObject") {
        const int32_t player_index = std::bit_cast<int32_t>(memory.U32(object + 492));
        const auto existing = std::find_if(sample.player_objects.begin(), sample.player_objects.end(),
            [=](const auto& player) { return player.player_index == player_index; });
        if (existing == sample.player_objects.end()) {
          const float body_yaw = std::atan2(memory.F32(object + 256), memory.F32(object + 264));
          const long body_yaw_mrad = std::isfinite(body_yaw)
              ? std::clamp(std::lround(body_yaw * 1000.0f), -3142l, 3142l) : 0l;
          sample.player_objects.push_back({player_index, object,
              std::isfinite(health) && health > 0.0f, false, position.x, position.y, position.z,
              static_cast<int16_t>(body_yaw_mrad)});
        } else {
          // A duplicate index cannot safely identify which soldier owns that input slot.
          existing->object = 0;
          existing->alive = false;
          existing->ambiguous = true;
        }
        sample.has_player = true;
        sample.player_alive = sample.player_alive || (std::isfinite(health) && health > 0.0f);
      } else if (faction == 1 && !class_name.empty() && std::isfinite(health) && health > 0.0f && Finite(position)) {
        ++sample.hostile_actors;
        if (sample.tracked_hostile_count < sample.hostiles.size()) {
          sample.hostiles[sample.tracked_hostile_count++] = {object, memory.U32(object), position};
        }
        if (const auto found = g_authoritative_actor_ids.find(object); found != g_authoritative_actor_ids.end() &&
            found->second.vtable == memory.U32(object)) {
          const auto centimeters = [](float meters) -> int32_t {
            const double value = std::clamp(double(meters) * 100.0, -10'000'000.0, 10'000'000.0);
            return static_cast<int32_t>(std::lround(value));
          };
          const float bounded_health = std::clamp(health, 0.0f, 65535.0f);
          const long rounded_health = std::lround(bounded_health);
          const float yaw = std::atan2(memory.F32(object + 256), memory.F32(object + 264));
          const long yaw_mrad = std::isfinite(yaw)
              ? std::clamp(std::lround(yaw * 1000.0f), -3142l, 3142l) : 0l;
          sample.authoritative_actors.push_back({found->second.id, edf::net::ActorArchetype::kAnt,
              centimeters(position.x), centimeters(position.y), centimeters(position.z),
              static_cast<int16_t>(yaw_mrad),
              static_cast<uint16_t>(rounded_health)});
          live_authoritative_objects.push_back(object);
        }
#if defined(EDF2027_AUTHORITATIVE_HOST)
        if (class_name == "clGiantAnt" && memory.U8(object + 624) == 0 &&
            !g_authoritative_actor_ids.contains(object)) {
          bool id_assigned = false;
          auto found = g_mission_actor_ids.find(object);
          if (found != g_mission_actor_ids.end() && found->second.vtable != memory.U32(object)) {
            const MissionActorTrack old_actor = found->second;
            const uint64_t trace_generation =
                g_mission_actor_trace_generation.load(std::memory_order_relaxed);
            uint32_t call_ordinal = 0, ant_ordinal = 0;
            MissionAntTraceObject trace_metadata{};
            bool has_trace_metadata = false;
            const uint32_t current_vtable = memory.U32(object);
            {
              const auto trace = g_mission_actor_trace_objects.find(object);
              if (trace != g_mission_actor_trace_objects.end()) {
                if (trace->second.vtable == old_actor.vtable) {
                  trace_metadata = trace->second;
                  has_trace_metadata = true;
                  g_mission_actor_trace_objects.erase(trace);
                } else if (trace->second.vtable != current_vtable) {
                  g_mission_actor_trace_objects.erase(trace);
                }
              }
            }
            if (has_trace_metadata && trace_metadata.generation == trace_generation) {
              call_ordinal = trace_metadata.call_ordinal;
              ant_ordinal = trace_metadata.ant_ordinal;
            }
            g_mission_actor_ids.erase(object);
            LogMissionActorLifecycle("vtable_mismatch", trace_generation,
                old_actor.mission_generation, old_actor.id, object,
                old_actor.vtable, current_vtable, call_ordinal, ant_ordinal,
                "identity_reused");
            found = g_mission_actor_ids.end();
          }
          if (found == g_mission_actor_ids.end() &&
              g_mission_actor_ids.size() < edf::net::kMaxSnapshotActors &&
              g_next_mission_actor_serial &&
              g_next_mission_actor_serial <= kMissionActorIdSerialMask) {
            const uint32_t id = kMissionActorIdNamespace | g_next_mission_actor_serial++;
            found = g_mission_actor_ids.emplace(object,
                MissionActorTrack{id, memory.U32(object), mission_generation}).first;
            id_assigned = true;
          }
          if (found != g_mission_actor_ids.end()) {
            if (id_assigned && REXCVAR_GET(edf_mp_actor_trace)) {
              const auto trace = g_mission_actor_trace_objects.find(object);
              const uint64_t trace_generation =
                  g_mission_actor_trace_generation.load(std::memory_order_relaxed);
              const bool trace_matches = trace != g_mission_actor_trace_objects.end() &&
                  trace->second.vtable == found->second.vtable &&
                  trace->second.generation == trace_generation;
              LogMissionActorLifecycle(trace_matches ? "identity_assigned" : "identity_assigned_untraced",
                  trace_generation,
                  found->second.mission_generation, found->second.id, object,
                  found->second.vtable, memory.U32(object),
                  trace_matches ? trace->second.call_ordinal : 0,
                  trace_matches ? trace->second.ant_ordinal : 0);
            }
            const auto centimeters = [](float meters) -> int32_t {
              const double value = std::clamp(double(meters) * 100.0, -10'000'000.0, 10'000'000.0);
              return static_cast<int32_t>(std::lround(value));
            };
            const float bounded_health = std::clamp(health, 0.0f, 65535.0f);
            const long rounded_health = std::lround(bounded_health);
            const float yaw = std::atan2(memory.F32(object + 256), memory.F32(object + 264));
            const long yaw_mrad = std::isfinite(yaw)
                ? std::clamp(std::lround(yaw * 1000.0f), -3142l, 3142l) : 0l;
            edf::net::ActorState actor{found->second.id, edf::net::ActorArchetype::kAnt,
                centimeters(position.x), centimeters(position.y), centimeters(position.z),
                static_cast<int16_t>(yaw_mrad), static_cast<uint16_t>(rounded_health)};
            const auto trace = g_mission_actor_trace_objects.find(object);
            const uint64_t trace_generation =
                g_mission_actor_trace_generation.load(std::memory_order_relaxed);
            if (trace != g_mission_actor_trace_objects.end() &&
                trace->second.generation == trace_generation &&
                trace->second.vtable == found->second.vtable &&
                trace->second.call_ordinal < kMissionActorTraceCallLimit &&
                trace->second.ant_ordinal < kMissionActorTraceAntLimit) {
              actor.trace_identity_valid = true;
              actor.trace_generation = trace->second.generation;
              actor.trace_call_ordinal = trace->second.call_ordinal;
              actor.trace_ant_ordinal = trace->second.ant_ordinal;
            }
            sample.mission_actors.push_back(std::move(actor));
            live_mission_objects.push_back(object);
          }
        }
#endif
      }
      node = next;
    }
  }
  const auto primary_player = std::find_if(sample.player_objects.begin(), sample.player_objects.end(),
      [](const auto& player) { return player.player_index == 0; });
  if (primary_player != sample.player_objects.end() && !primary_player->ambiguous && primary_player->object) {
    sample.primary_player_available = true;
    sample.primary_player_alive = primary_player->alive;
    sample.player_object = primary_player->object;
    sample.player_position = {primary_player->x, primary_player->y, primary_player->z};
    sample.player_yaw_mrad = SamplePlayerYawMrad(memory);
  }
  for (auto it = g_authoritative_actor_ids.begin(); it != g_authoritative_actor_ids.end();) {
    if (std::find(live_authoritative_objects.begin(), live_authoritative_objects.end(), it->first) !=
        live_authoritative_objects.end()) {
      ++it;
      continue;
    }
    const uint32_t object = it->first;
    ++it;
    ForgetAuthoritativeActor(object);
  }
#if defined(EDF2027_AUTHORITATIVE_HOST)
  for (auto it = g_mission_actor_ids.begin(); it != g_mission_actor_ids.end();) {
    if (std::find(live_mission_objects.begin(), live_mission_objects.end(), it->first) !=
        live_mission_objects.end()) {
      ++it;
    } else {
#if defined(EDF2027_AUTHORITATIVE_HOST)
      const uint32_t object = it->first;
      const MissionActorTrack actor = it->second;
      const uint64_t trace_generation =
          g_mission_actor_trace_generation.load(std::memory_order_relaxed);
      uint32_t call_ordinal = 0, ant_ordinal = 0;
      MissionAntTraceObject trace_metadata{};
      bool has_trace_metadata = false;
      {
        const auto trace = g_mission_actor_trace_objects.find(object);
        if (trace != g_mission_actor_trace_objects.end()) {
          trace_metadata = trace->second;
          has_trace_metadata = true;
          g_mission_actor_trace_objects.erase(trace);
        }
      }
      if (has_trace_metadata && trace_metadata.generation == trace_generation) {
        call_ordinal = trace_metadata.call_ordinal;
        ant_ordinal = trace_metadata.ant_ordinal;
      }
      LogMissionActorLifecycle("sample_disappearance", trace_generation,
          actor.mission_generation, actor.id, object, actor.vtable, 0,
          call_ordinal, ant_ordinal, "not_in_current_sample");
#endif
      ++it;
      ForgetMissionActor(object);
    }
  }
#endif
  std::sort(sample.authoritative_actors.begin(), sample.authoritative_actors.end(),
            [](const auto& a, const auto& b) { return a.id < b.id; });
  std::sort(sample.mission_actors.begin(), sample.mission_actors.end(),
            [](const auto& a, const auto& b) { return a.id < b.id; });
  return sample;
}

template <typename Context>
void DrainAuthoritativeActorSpawns(Context& parent, uint8_t* base, const WorldSample& world) {
  if (world.loading || !world.primary_player_available || !world.primary_player_alive) return;
  std::deque<QueuedActorSpawn> pending;
  uint64_t mission_generation = 0;
  {
    std::lock_guard lock(g_actor_spawn_mutex);
    mission_generation = g_actor_mission_generation;
    pending.swap(g_actor_spawn_queue);
  }
  if (pending.empty()) return;
  ActorSpawnReservationBatch reservations(pending.size());
  const edf::console::GuestMemory memory(base);
  edf::console::GuestCalls guest(parent, base);
  for (const auto& queued : pending) {
    if (queued.mission_generation != mission_generation) {
      reservations.ReleaseOne();
      continue;
    }
    const auto& spawn = queued.request;
    if (spawn.archetype != edf::net::ActorArchetype::kAnt || !spawn.id) {
      reservations.ReleaseOne();
      continue;
    }
    bool reservation_transferred = false;
    const uint32_t mark = guest.Mark();
    constexpr std::string_view path_text = "game:\\Object\\GiantAnt01.Sgo";
    const uint32_t literal = guest.WideString(path_text);
    const uint32_t path = guest.Alloc(32);
    memory.W32(path + 24, 7);
    guest.Call(sub_820A0D10, edf::console::GuestCalls::R({path, literal, uint32_t(path_text.size())}));
    guest.Call(sub_820AFAC0, edf::console::GuestCalls::R({memory.U32(0x82578648), path, 1}));
    const uint32_t matrix = guest.Alloc(64), params = guest.Alloc(80);
    const float yaw = float(spawn.yaw_mrad) * 0.001f;
    const float c = std::cos(yaw), s = std::sin(yaw);
    const float x = float(spawn.x_cm) * 0.01f, y = float(spawn.y_cm) * 0.01f, z = float(spawn.z_cm) * 0.01f;
    const float rows[16] = {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, x, y, z, 1};
    for (int i = 0; i < 16; ++i) memory.WF32(matrix + uint32_t(i) * 4, rows[i]);
    for (int i = 0; i < 4; ++i) memory.WF32(params + uint32_t(i) * 20, 1.0f);
    const uint32_t object = guest.Call(sub_821A6E08,
        edf::console::GuestCalls::R({memory.U32(0x8257C030), matrix, path, params}));
    uint32_t game_object = 0;
    if (object)
      game_object = guest.Call(sub_821E9348,
          edf::console::GuestCalls::R({object, 0, 0x82550D14, 0x82551058, 0}));
    if (game_object) {
      guest.Call(sub_820D5430, edf::console::GuestCalls::R({game_object}, {1.0}));
      guest.Call(sub_820D5ED0, edf::console::GuestCalls::R({game_object, 1, 1}));
      guest.Call(sub_820D4A00, edf::console::GuestCalls::R({game_object}));
      if (memory.ClassName(game_object) == "clGiantAnt") {
        const auto [it, inserted] = g_authoritative_actor_ids.emplace(
            game_object, AuthoritativeActorTrack{spawn.id, memory.U32(game_object)});
        (void)it;
        if (inserted) {
          reservations.TransferOneToLiveActor();
          reservation_transferred = true;
        }
        else guest.Call(sub_821C0ED8, edf::console::GuestCalls::R({game_object}));
      } else {
        guest.Call(sub_821C0ED8, edf::console::GuestCalls::R({game_object}));
      }
    }
    guest.Call(sub_820A0858, edf::console::GuestCalls::R({path}));
    guest.Release(mark);
    // A successful insertion transferred this slot to the live actor map.
    // Otherwise the request failed and no longer consumes guest capacity.
    if (!reservation_transferred) reservations.ReleaseOne();
  }
}

template <typename Context>
void DrainAuthoritativeActorDamage(Context& parent, uint8_t* base, const WorldSample& world) {
  if (world.loading || !world.primary_player_available || !world.primary_player_alive) return;
  std::array<QueuedActorDamage, kMaxActorDamageApplicationsPerSample> pending{};
  size_t pending_count = 0;
  uint64_t mission_generation = 0;
  {
    std::lock_guard lock(g_actor_spawn_mutex);
    mission_generation = g_actor_mission_generation;
    while (pending_count < pending.size() && !g_actor_damage_queue.empty()) {
      pending[pending_count++] = g_actor_damage_queue.front();
      g_actor_damage_queue.pop_front();
    }
  }
  if (!pending_count) return;

  const edf::console::GuestMemory memory(base);
  edf::console::GuestCalls guest(parent, base);
  for (size_t index = 0; index < pending_count; ++index) {
    const QueuedActorDamage& queued = pending[index];
    const bool mission_actor_id = (queued.actor_id & kMissionActorIdNamespace) != 0;
#if defined(EDF2027_AUTHORITATIVE_HOST)
    const uint64_t trace_generation =
        g_mission_actor_trace_generation.load(std::memory_order_relaxed);
    const auto log_damage_drop = [&](const char* reason, uint32_t object,
                                     uint32_t expected_vtable, uint32_t current_vtable,
                                     uint32_t call_ordinal, uint32_t ant_ordinal) {
      LogMissionActorLifecycle("damage_dropped", trace_generation,
          queued.mission_generation, queued.actor_id, object, expected_vtable,
          current_vtable, call_ordinal, ant_ordinal, reason, queued.damage_points,
          queued.request_id);
    };
#endif
    if (queued.mission_generation != mission_generation) {
#if defined(EDF2027_AUTHORITATIVE_HOST)
      if (mission_actor_id)
        log_damage_drop("stale_mission_generation", 0, 0, 0, 0, 0);
#endif
      continue;
    }
    if (!queued.actor_id || !std::isfinite(queued.damage_points) ||
        queued.damage_points <= 0.0f || queued.damage_points > 65535.0f) {
#if defined(EDF2027_AUTHORITATIVE_HOST)
      if (mission_actor_id)
        log_damage_drop("invalid_actor_id_or_damage", 0, 0, 0, 0, 0);
#endif
      continue;
    }
    const auto& sampled_actors = mission_actor_id ? world.mission_actors : world.authoritative_actors;
    const auto sampled_actor = std::find_if(sampled_actors.begin(), sampled_actors.end(),
        [&](const edf::net::ActorState& actor) {
          return actor.id == queued.actor_id && actor.health > 0 &&
              actor.archetype == edf::net::ActorArchetype::kAnt;
        });
    if (sampled_actor == sampled_actors.end()) {
#if defined(EDF2027_AUTHORITATIVE_HOST)
      if (mission_actor_id)
        log_damage_drop("no_sampled_live_actor", 0, 0, 0, 0, 0);
#endif
      continue;
    }

    if (mission_actor_id) {
#if defined(EDF2027_AUTHORITATIVE_HOST)
      MissionActorTrack actor_track;
      uint32_t object = 0;
      bool has_actor_track = false;
      {
        const auto tracked = std::find_if(g_mission_actor_ids.begin(), g_mission_actor_ids.end(),
            [&](const auto& entry) { return entry.second.id == queued.actor_id; });
        if (tracked != g_mission_actor_ids.end()) {
          object = tracked->first;
          actor_track = tracked->second;
          has_actor_track = true;
        }
      }
      if (!has_actor_track) {
        log_damage_drop("no_registry_identity", 0, 0, 0, 0, 0);
        continue;
      }
      const uint32_t expected_vtable = actor_track.vtable;
      uint32_t call_ordinal = 0, ant_ordinal = 0;
      {
        const auto trace = g_mission_actor_trace_objects.find(object);
        if (trace != g_mission_actor_trace_objects.end() &&
            trace->second.generation == trace_generation &&
            trace->second.vtable == expected_vtable) {
          call_ordinal = trace->second.call_ordinal;
          ant_ordinal = trace->second.ant_ordinal;
        }
      }
      if (!edf::console::GuestMemory::Heap(object, 628)) {
        log_damage_drop("guest_object_unreadable", object, expected_vtable, 0,
                        call_ordinal, ant_ordinal);
        continue;
      }
      const uint32_t current_vtable = memory.U32(object);
      if (current_vtable != expected_vtable) {
        log_damage_drop("vtable_mismatch", object, expected_vtable, current_vtable,
                        call_ordinal, ant_ordinal);
        continue;
      }
      if (memory.ClassName(object) != "clGiantAnt") {
        log_damage_drop("wrong_guest_class", object, expected_vtable, current_vtable,
                        call_ordinal, ant_ordinal);
        continue;
      }
      if (memory.U32(object + 604) != 1 || memory.U8(object + 624) != 0) {
        log_damage_drop("actor_inactive_or_removed", object, expected_vtable,
                        current_vtable, call_ordinal, ant_ordinal);
        continue;
      }
      const float health = memory.F32(object + 592);
      if (!std::isfinite(health) || health <= 0.0f) {
        log_damage_drop("actor_dead_or_invalid_health", object, expected_vtable,
                        current_vtable, call_ordinal, ant_ordinal);
        continue;
      }
      const uint32_t mark = guest.Mark();
      uint32_t guest_call_return = 0;
      try {
        const uint32_t damage = guest.Alloc(128);
        memory.W32(damage + 20, 0xFFFFFFFFu);
        memory.WF32(damage + 24, queued.damage_points);
        guest_call_return = guest.Call(sub_820D59D0,
            edf::console::GuestCalls::R({object, damage, 1}));
      } catch (...) {
        guest.Release(mark);
        log_damage_drop("guest_damage_call_exception", object, expected_vtable,
                        current_vtable, call_ordinal, ant_ordinal);
        continue;
      }
      guest.Release(mark);
      LogMissionActorLifecycle("damage_dispatch", trace_generation,
          queued.mission_generation, queued.actor_id, object,
          expected_vtable, current_vtable, call_ordinal, ant_ordinal,
          "guest_call_return_observed", queued.damage_points, queued.request_id,
          guest_call_return, true);
#endif
      continue;
    }

    const auto tracked = std::find_if(g_authoritative_actor_ids.begin(), g_authoritative_actor_ids.end(),
        [&](const auto& entry) { return entry.second.id == queued.actor_id; });
    if (tracked == g_authoritative_actor_ids.end()) continue;
    const uint32_t object = tracked->first;
    const uint32_t expected_vtable = tracked->second.vtable;
    if (!edf::console::GuestMemory::Heap(object, 628) || memory.U32(object) != expected_vtable ||
        memory.ClassName(object) != "clGiantAnt" || memory.U32(object + 604) != 1 ||
        memory.U8(object + 624) != 0) continue;
    const float health = memory.F32(object + 592);
    if (!std::isfinite(health) || health <= 0.0f) continue;

    const uint32_t mark = guest.Mark();
    try {
      const uint32_t damage = guest.Alloc(128);
      memory.W32(damage + 20, 0xFFFFFFFFu);  // no faction
      memory.WF32(damage + 24, queued.damage_points);
      (void)guest.Call(sub_820D59D0, edf::console::GuestCalls::R({object, damage, 1}));
    } catch (...) {
      guest.Release(mark);
      continue;
    }
    guest.Release(mark);
  }
}

uint32_t InputElapsedMs() {
  const auto now = std::chrono::steady_clock::now();
  const uint64_t ticks = g_steps.load(std::memory_order_relaxed);
  std::lock_guard lock(g_input_clock_mutex);
  if (!g_input_clock_started) {
    g_input_clock_started = true;
    g_input_tick_origin = ticks;
    g_input_wall_origin = now;
  }
  if (g_input_clock == edf::ScriptedClock::Game) {
    return edf::GameTicksToMs(ticks >= g_input_tick_origin ? ticks - g_input_tick_origin : 0);
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_input_wall_origin).count();
  return elapsed <= 0 ? 0 : elapsed >= UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(elapsed);
}
}  // namespace

#if defined(EDF2027_HEADLESS_GUEST)
extern "C" void edf_native_probe_input_dispatch_gate(
    uint8_t* base, uint32_t function, uint32_t phase, uint32_t callsite,
    uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7, uint32_t r8,
    uint64_t fp1_bits, uint64_t fp2_bits) {
  ObserveInputDispatchGate(base, function, phase, callsite,
      {r3, r4, r5, r6, r7, r8}, fp1_bits, fp2_bits);
}
#endif

extern "C" void edf_native_probe_title_action_event(
    uint32_t object, uint32_t event, uint32_t slot, uint32_t count, uint32_t callsite) {
  if (!g_trace_input_consumers.load(std::memory_order_relaxed)) return;
  g_title_action_consumer_reads.fetch_add(1, std::memory_order_relaxed);
  if (event && slot < 32)
    g_title_action_consumer_event_mask.fetch_or(uint32_t{1} << slot, std::memory_order_relaxed);
  g_title_action_consumer_last_event.store(event, std::memory_order_relaxed);
  g_title_action_consumer_last_object.store(object, std::memory_order_relaxed);
  g_title_action_consumer_last_slot.store(slot, std::memory_order_relaxed);
  g_title_action_consumer_last_count.store(count, std::memory_order_relaxed);
  g_title_action_consumer_last_callsite.store(callsite, std::memory_order_relaxed);
}

namespace edf::net::probe {

void ConfigureInputScript(std::vector<edf::ScriptedInputEvent> events, edf::ScriptedClock clock) {
  g_input_events = std::move(events);
  g_input_clock = clock;
  g_mission_probe_enabled = true;
}

void ConfigureDecoderTraceRange(uint32_t begin, uint32_t end) {
  if (begin >= end) {
    begin = 0;
    end = 0;
  }
  g_decoder_trace_begin.store(begin, std::memory_order_relaxed);
  g_decoder_trace_end.store(end, std::memory_order_relaxed);
}

void ConfigureInputConsumerTrace(bool enabled) {
  g_trace_input_consumers.store(enabled, std::memory_order_relaxed);
  g_decoder_opcode_call_order.store(0, std::memory_order_relaxed);
  g_raw_vm_dispatch_order.store(0, std::memory_order_relaxed);
  g_mission_start_dispatch_order.store(0, std::memory_order_relaxed);
  g_decoder_trace_dropped.store(0, std::memory_order_relaxed);
  g_decoder_trace_last_buttons.store(0, std::memory_order_relaxed);
  g_decoder_trace_capture_until_ms.store(0, std::memory_order_relaxed);
  g_decoder_trace_has_trigger.store(false, std::memory_order_relaxed);
  {
    std::lock_guard lock(g_state_mutex);
    g_state.mission_sequence_lifecycle.clear();
    g_state.mission_sequence_callbacks.clear();
    g_state.title_router_transitions.clear();
    g_state.decoder_opcode_samples.clear();
    g_state.raw_vm_dispatch_samples.clear();
    g_state.mission_start_dispatch_samples.clear();
    g_state.mission_start_dispatch_dropped = 0;
    g_state.mission_sequence_gate_trace.clear();
    g_state.input_action_transitions.clear();
    g_state.input_manager_callbacks.clear();
    g_state.input_dispatch_gate_trace.clear();
    g_state.title_ui_handoffs.clear();
    g_state.title_scene_updates.clear();
    g_state.guest_wait_samples.clear();
    g_state.title_sequence_request = {};
  }
  g_guest_wait_calls.store(0, std::memory_order_relaxed);
  g_mission_sequence_begins.store(0, std::memory_order_relaxed);
  for (auto& phase : g_title_ui_router_event_calls)
    for (auto& event : phase) event.store(0, std::memory_order_relaxed);
  for (auto& user : g_input_consumer_mask_polls)
    for (auto& site : user)
      for (auto& mask : site) mask.store(0, std::memory_order_relaxed);
  for (auto& value : g_xpad_action_poll_calls) value.store(0, std::memory_order_relaxed);
  for (auto& value : g_xpad_action_targets_seen) value.store(0, std::memory_order_relaxed);
  for (auto& value : g_xpad_action_channels_seen) value.store(0, std::memory_order_relaxed);
  for (auto& user : g_xpad_action_targets)
    for (auto& value : user) value.store(0, std::memory_order_relaxed);
  for (auto& value : g_xpad_action_state_samples) value.store(0, std::memory_order_relaxed);
  for (auto& value : g_xpad_action_held_seen) value.store(0, std::memory_order_relaxed);
  for (auto& value : g_xpad_action_edges_seen) value.store(0, std::memory_order_relaxed);
  for (auto& value : g_xpad_action_pulses_seen) value.store(0, std::memory_order_relaxed);
  for (auto& value : g_xpad_action_last_held) value.store(0, std::memory_order_relaxed);
  for (auto& value : g_xpad_action_last_edges) value.store(0, std::memory_order_relaxed);
  for (auto& value : g_xpad_action_last_pulses) value.store(0, std::memory_order_relaxed);
  for (auto& value : g_xpad_action_invalid_samples) value.store(0, std::memory_order_relaxed);
  g_title_action_handler_calls.store(0, std::memory_order_relaxed);
  g_title_action_handler_last_r31.store(0, std::memory_order_relaxed);
  g_title_action_default_calls.store(0, std::memory_order_relaxed);
  g_title_action_default_last_r31.store(0, std::memory_order_relaxed);
  g_title_action_consumer_reads.store(0, std::memory_order_relaxed);
  g_title_action_consumer_event_mask.store(0, std::memory_order_relaxed);
  g_title_action_consumer_last_event.store(0, std::memory_order_relaxed);
  g_title_action_consumer_last_object.store(0, std::memory_order_relaxed);
  g_title_action_consumer_last_slot.store(0, std::memory_order_relaxed);
  g_title_action_consumer_last_count.store(0, std::memory_order_relaxed);
  g_title_action_consumer_last_callsite.store(0, std::memory_order_relaxed);
  for (auto& value : g_title_ui_action_slot_reads) value.store(0, std::memory_order_relaxed);
  for (auto& value : g_title_ui_action_slot_event_values) value.store(0, std::memory_order_relaxed);
  for (auto& value : g_title_ui_action_slot_input_flags) value.store(0, std::memory_order_relaxed);
  for (auto& value : g_title_ui_action_slot_last_event) value.store(0, std::memory_order_relaxed);
  for (auto& value : g_input_event_update_slot_calls) value.store(0, std::memory_order_relaxed);
  for (auto& value : g_input_event_update_slot_callbacks) value.store(0, std::memory_order_relaxed);
  for (auto& value : g_input_event_update_slot_last_event) value.store(0, std::memory_order_relaxed);
  for (auto& value : g_input_event_update_slot_nonzero_seen) value.store(0, std::memory_order_relaxed);
  g_input_event_update_calls.store(0, std::memory_order_relaxed);
  g_input_event_update_nonzero_calls.store(0, std::memory_order_relaxed);
  g_input_event_update_last_object.store(0, std::memory_order_relaxed);
  g_input_event_update_last_callback.store(0, std::memory_order_relaxed);
  g_input_event_update_last_event_before.store(0, std::memory_order_relaxed);
  g_input_event_update_last_event_after.store(0, std::memory_order_relaxed);
  g_input_event_update_last_callsite.store(0, std::memory_order_relaxed);
  g_title_action_slot_scan_calls.store(0, std::memory_order_relaxed);
  g_title_action_slot_scan_manager.store(0, std::memory_order_relaxed);
  g_title_action_slot_scan_slot.store(0, std::memory_order_relaxed);
  g_title_action_slot_scan_count.store(0, std::memory_order_relaxed);
  g_title_action_slot_scan_object.store(0, std::memory_order_relaxed);
  g_title_action_slot_scan_event.store(0, std::memory_order_relaxed);
  g_title_ui_handoff_calls.store(0, std::memory_order_relaxed);
  g_title_scene_update_calls.store(0, std::memory_order_relaxed);
  g_title_ui_trace_last_buttons.store(0, std::memory_order_relaxed);
  g_title_ui_handoff_window.store(0, std::memory_order_relaxed);
  g_title_scene_update_window.store(0, std::memory_order_relaxed);
  g_title_sequence_request_calls.store(0, std::memory_order_relaxed);
  g_title_sequence_request_callsite.store(0, std::memory_order_relaxed);
  g_title_sequence_request_receiver.store(0, std::memory_order_relaxed);
  g_title_sequence_request_object.store(0, std::memory_order_relaxed);
  g_title_sequence_request_state_72_before.store(0, std::memory_order_relaxed);
  g_title_sequence_request_state_60_before.store(0, std::memory_order_relaxed);
  g_title_sequence_request_state_60_after.store(0, std::memory_order_relaxed);
  g_title_sequence_request_sequence_object.store(0, std::memory_order_relaxed);
  g_title_sequence_request_sequence_696_before.store(0, std::memory_order_relaxed);
  g_title_sequence_request_sequence_696_after.store(0, std::memory_order_relaxed);
  g_title_sequence_request_sequence_708_before.store(0, std::memory_order_relaxed);
  g_title_sequence_request_sequence_708_after.store(0, std::memory_order_relaxed);
  g_title_sequence_request_sequence_712_before.store(0, std::memory_order_relaxed);
  g_title_sequence_request_sequence_712_after.store(0, std::memory_order_relaxed);
  g_title_action_gate_samples.store(0, std::memory_order_relaxed);
  g_title_action_gate_a_function_calls.store(0, std::memory_order_relaxed);
  g_title_action_gate_a_function_callsite.store(0, std::memory_order_relaxed);
  g_title_action_gate_a_function_return.store(0, std::memory_order_relaxed);
  g_title_action_gate_b_function_calls.store(0, std::memory_order_relaxed);
  g_title_action_gate_b_function_callsite.store(0, std::memory_order_relaxed);
  g_title_action_gate_b_function_return.store(0, std::memory_order_relaxed);
  g_title_action_gate_2260_last.store(0, std::memory_order_relaxed);
  g_title_action_gate_2261_last.store(0, std::memory_order_relaxed);
  g_title_action_gate_2262_last.store(0, std::memory_order_relaxed);
  g_title_action_gate_a_object.store(0, std::memory_order_relaxed);
  g_title_action_gate_a_link.store(0, std::memory_order_relaxed);
  g_title_action_gate_a_state.store(0, std::memory_order_relaxed);
  g_title_action_gate_a_result.store(0, std::memory_order_relaxed);
  g_title_action_gate_b_object.store(0, std::memory_order_relaxed);
  g_title_action_gate_b_712.store(0, std::memory_order_relaxed);
  g_title_action_gate_b_696.store(0, std::memory_order_relaxed);
  g_title_action_gate_b_708.store(0, std::memory_order_relaxed);
  g_title_action_gate_b_result.store(0, std::memory_order_relaxed);
  g_title_action_gate_sequence_pointer.store(0, std::memory_order_relaxed);
  g_title_action_gate_sequence_708.store(0, std::memory_order_relaxed);
  g_title_action_gate_sequence_result.store(0, std::memory_order_relaxed);
  g_title_action_gate_state_object.store(0, std::memory_order_relaxed);
  g_title_action_gate_state_vtable.store(0, std::memory_order_relaxed);
  g_title_action_gate_input_manager.store(0, std::memory_order_relaxed);
  g_title_action_gate_input_count.store(0, std::memory_order_relaxed);
  g_title_action_gate_r31.store(0, std::memory_order_relaxed);
  g_title_action_gate_r25.store(0, std::memory_order_relaxed);
  g_title_action_gate_estimates_valid.store(false, std::memory_order_relaxed);
  g_title_action_gate_timer_valid.store(false, std::memory_order_relaxed);
  g_title_action_gate_timer_a.store(0, std::memory_order_relaxed);
  g_title_action_gate_timer_b.store(0, std::memory_order_relaxed);
  g_title_action_gate_timer_branch_taken.store(0, std::memory_order_relaxed);
  {
    std::lock_guard lock(g_state_mutex);
    g_state.title_action_gate_b_class_name.clear();
    g_state.title_action_gate_state_class_name.clear();
  }
  g_title_action_user_samples.store(0, std::memory_order_relaxed);
  g_title_action_user_count.store(0, std::memory_order_relaxed);
  g_title_action_user_object_mask.store(0, std::memory_order_relaxed);
  g_title_action_slot_gate_mask.store(0, std::memory_order_relaxed);
  g_title_action_slot_gate_seen.store(0, std::memory_order_relaxed);
  g_input_dispatch_gate_calls.store(0, std::memory_order_relaxed);
  g_input_dispatch_gate_slot_matches.store(0, std::memory_order_relaxed);
  g_title_action_registry_pointer.store(0, std::memory_order_relaxed);
  g_title_action_user_invalid_stage.store(0, std::memory_order_relaxed);
  g_title_action_user_invalid_samples.store(0, std::memory_order_relaxed);
}

void RecordMissionSequenceLifecycle(uint8_t* base, uint32_t phase, uint32_t object,
                                    uint32_t related) {
  if (!g_trace_input_consumers.load(std::memory_order_relaxed)) return;
  using edf::console::GuestMemory;
  constexpr uint32_t kMissionSequenceGlobal = 0x825786AC;
  const GuestMemory memory(base);
  MissionSequenceLifecycleSample sample;
  sample.phase = phase;
  sample.object = object;
  sample.related = related;
  if (GuestMemory::Image(kMissionSequenceGlobal, 4))
    sample.current_object = memory.U32(kMissionSequenceGlobal);
  if (object && (GuestMemory::Heap(object, 716) || GuestMemory::Image(object, 716))) {
    sample.vtable = memory.U32(object);
    if (GuestMemory::Image(sample.vtable, 32 * 4)) {
      for (std::size_t index = 0; index < sample.vtable_entries.size(); ++index)
        sample.vtable_entries[index] = memory.U32(sample.vtable + uint32_t(index * 4));
    }
    sample.state_696 = memory.U8(object + 696);
    sample.state_708 = memory.U32(object + 708);
    sample.state_712 = memory.U32(object + 712);
  }
  std::lock_guard lock(g_state_mutex);
  constexpr std::size_t kMaximumLifecycleSamples = 32;
  if (g_state.mission_sequence_lifecycle.size() < kMaximumLifecycleSamples)
    g_state.mission_sequence_lifecycle.push_back(sample);
}

void RecordMissionSequenceCallback(uint8_t* base, uint32_t function, uint32_t phase,
                                   uint32_t callsite,
                                   const std::array<uint32_t, 6>& arguments) {
  if (!g_trace_input_consumers.load(std::memory_order_relaxed)) return;
  using edf::console::GuestMemory;
  constexpr uint32_t kMissionSequenceGlobal = 0x825786AC;
  const GuestMemory memory(base);
  MissionSequenceCallbackSample sample;
  sample.function = function;
  sample.phase = phase;
  sample.callsite = callsite;
  sample.arguments = arguments;
  if (GuestMemory::Image(kMissionSequenceGlobal, 4)) {
    sample.sequence = memory.U32(kMissionSequenceGlobal);
    if (sample.sequence &&
        (GuestMemory::Heap(sample.sequence, 716) || GuestMemory::Image(sample.sequence, 716))) {
      sample.state_696 = memory.U8(sample.sequence + 696);
      sample.state_708 = memory.U32(sample.sequence + 708);
      sample.state_712 = memory.U32(sample.sequence + 712);
    }
  }
  if (function == 0x820C7220) {
    sample.router_object = arguments[0];
    if (sample.router_object &&
        (GuestMemory::Heap(sample.router_object, 260) ||
         GuestMemory::Image(sample.router_object, 260))) {
      sample.router_vtable = memory.U32(sample.router_object);
      sample.router_valid_mask |= 1u << 2;
      sample.router_related = memory.U32(sample.router_object + 256);
      sample.router_valid_mask |= 1u << 0;
      if (sample.router_related &&
          (GuestMemory::Heap(sample.router_related, 76) ||
           GuestMemory::Image(sample.router_related, 76))) {
        sample.router_related_72 = memory.U32(sample.router_related + 72);
        sample.router_valid_mask |= 1u << 1;
      }
        if (GuestMemory::Heap(sample.router_related, 64) ||
            GuestMemory::Image(sample.router_related, 64)) {
          if (phase == 1) {
            sample.router_status_before = memory.U8(sample.router_related + 60);
            sample.router_valid_mask |= 1u << 3;
          } else {
            sample.router_status_after = memory.U8(sample.router_related + 60);
            sample.router_valid_mask |= 1u << 4;
          }
        }
      }
    if (arguments[3] == 1 && (arguments[4] & 0x1F) == 16) {
      const uint32_t value_stack = arguments[2];
      if (value_stack &&
          (GuestMemory::Heap(value_stack, 4) || GuestMemory::Image(value_stack, 4))) {
        const uint32_t text = memory.U32(value_stack);
        sample.router_text_pointer = text;
        const auto readable_byte = [](uint32_t address) {
          return (address >= 0x40000000u && address < 0x50000000u) ||
                 GuestMemory::Image(address, 1);
        };
        if (text && readable_byte(text))
          sample.router_valid_mask |= 1u << 5;
        constexpr uint32_t kMaximumTextBytes = 96;
        for (uint32_t i = 0; text && i < kMaximumTextBytes; ++i) {
          const uint32_t address = text + i;
          if (!readable_byte(address)) break;
          const uint8_t character = memory.U8(address);
          if (!character) break;
          sample.text_argument.push_back(character >= 0x20 && character < 0x7F
                                             ? static_cast<char>(character)
                                             : '?');
        }
      }
    }
  } else if (function == 0x820C5A70 || function == 0x820C5B28) {
    sample.router_object = arguments[0];
    if (sample.router_object &&
        (GuestMemory::Heap(sample.router_object, 260) ||
         GuestMemory::Image(sample.router_object, 260))) {
      sample.router_vtable = memory.U32(sample.router_object);
      sample.router_valid_mask |= 1u << 2;
      sample.router_related = memory.U32(sample.router_object + 256);
      sample.router_valid_mask |= 1u << 0;
      if (sample.router_related &&
          (GuestMemory::Heap(sample.router_related, 76) ||
           GuestMemory::Image(sample.router_related, 76))) {
        sample.router_related_72 = memory.U32(sample.router_related + 72);
        sample.router_valid_mask |= 1u << 1;
        if (GuestMemory::Heap(sample.router_related, 64) ||
            GuestMemory::Image(sample.router_related, 64)) {
          if (phase == 1) {
            sample.router_status_before = memory.U8(sample.router_related + 60);
            sample.router_valid_mask |= 1u << 3;
          } else {
            sample.router_status_after = memory.U8(sample.router_related + 60);
            sample.router_valid_mask |= 1u << 4;
          }
        }
      }
    }
  }
  std::lock_guard lock(g_state_mutex);
  if (function == 0x820C7220) {
    const auto found = std::find_if(
        g_state.mission_sequence_callbacks.begin(), g_state.mission_sequence_callbacks.end(),
        [&sample](const auto& existing) {
          return existing.function == sample.function && existing.callsite == sample.callsite &&
                 existing.arguments[3] == sample.arguments[3] &&
                 (existing.arguments[4] & 0x1F) == (sample.arguments[4] & 0x1F) &&
                 ((sample.arguments[3] != 1 || (sample.arguments[4] & 0x1F) != 16) ||
                  existing.text_argument == sample.text_argument);
        });
    if (found != g_state.mission_sequence_callbacks.end()) {
      if (phase == 2) {
        sample.router_status_before = found->router_status_before;
        sample.router_valid_mask |= found->router_valid_mask & (1u << 3);
      }
      *found = sample;
    } else if (g_state.mission_sequence_callbacks.size() < 64) {
      g_state.mission_sequence_callbacks.push_back(sample);
    }
  } else if (g_state.mission_sequence_callbacks.size() < 48) {
    g_state.mission_sequence_callbacks.push_back(sample);
  }
}

TitleRouterTransitionSample CaptureTitleRouterTransition(
    uint8_t* base, uint32_t callsite, const std::array<uint32_t, 6>& arguments,
    const std::array<uint32_t, 3>& callback_registers) {
  using edf::console::GuestMemory;
  constexpr uint32_t kMissionSequenceGlobal = 0x825786AC;
  constexpr uint32_t kTitleEventOwnerGlobal = 0x82578E80;
  constexpr uint32_t kTitleStateGlobal = 0x8257C030;
  const GuestMemory memory(base);
  TitleRouterTransitionSample sample;
  sample.script_elapsed_ms = g_input_script_elapsed_ms.load(std::memory_order_relaxed);
  sample.script_buttons = g_input_script_buttons.load(std::memory_order_relaxed);
  sample.callsite = callsite;
  sample.route_phase = arguments[3];
  sample.event = arguments[4] & 0x1Fu;
  sample.callback_arguments = arguments;
  sample.decoder_context = callback_registers[0];
  sample.decoder_cursor_slot = callback_registers[1];
  sample.decoder_output = callback_registers[2];
  if (GuestMemory::Heap(sample.decoder_context, 176) ||
      GuestMemory::Image(sample.decoder_context, 176)) {
    sample.decoder_context_160 = memory.U32(sample.decoder_context + 160);
    sample.decoder_context_164 = memory.U32(sample.decoder_context + 164);
    sample.decoder_context_168 = memory.U32(sample.decoder_context + 168);
    sample.decoder_context_172 = memory.U32(sample.decoder_context + 172);
    sample.callback_valid_mask |= 1u << 0;
    // The VM stack pointer and its preceding 32-bit slot are independent reads.
    if (GuestMemory::Heap(sample.decoder_context_168, 4) ||
        GuestMemory::Image(sample.decoder_context_168, 4)) {
      sample.decoder_stack_top = memory.U32(sample.decoder_context_168);
      sample.callback_valid_mask |= 1u << 6;
    }
    if (sample.decoder_context_168 >= 4 &&
        (GuestMemory::Heap(sample.decoder_context_168 - 4, 4) ||
         GuestMemory::Image(sample.decoder_context_168 - 4, 4))) {
      sample.decoder_stack_previous = memory.U32(sample.decoder_context_168 - 4);
      sample.callback_valid_mask |= 1u << 7;
    }
    if (sample.decoder_context == arguments[1]) sample.callback_valid_mask |= 1u << 1;
    if (sample.decoder_cursor_slot == sample.decoder_context + 164)
      sample.callback_valid_mask |= 1u << 2;
    if (sample.decoder_context_160 == arguments[0]) sample.callback_valid_mask |= 1u << 3;
    if (sample.decoder_output == arguments[2]) sample.callback_valid_mask |= 1u << 4;
  }
  if (GuestMemory::Image(kTitleStateGlobal, 4)) {
    sample.title_state_pointer_valid = true;
    sample.title_state = memory.U32(kTitleStateGlobal);
    if (GuestMemory::Heap(sample.title_state, 2264) ||
        GuestMemory::Image(sample.title_state, 2264)) {
      sample.title_state_valid = true;
      for (std::size_t i = 0; i < sample.title_state_flags.size(); ++i) {
        sample.title_state_flags[i] = memory.U8(
            sample.title_state + 2260 + static_cast<uint32_t>(i));
        sample.title_state_flags_valid_mask |= static_cast<uint8_t>(1u << i);
      }
    }
  }
  sample.router = arguments[0];
  sample.latch_owner = arguments[1];
  if (GuestMemory::Heap(sample.latch_owner, 61) ||
      GuestMemory::Image(sample.latch_owner, 61)) {
    sample.latch_owner_state_60_before = memory.U8(sample.latch_owner + 60);
    sample.latch_owner_state_60_after = sample.latch_owner_state_60_before;
    sample.valid_mask |= 1u << 5;
  }
  sample.route_output = arguments[2];
  if (GuestMemory::Heap(sample.route_output, 8) ||
      GuestMemory::Image(sample.route_output, 8)) {
    sample.route_output_value_4_before = memory.U32(sample.route_output + 4);
    sample.route_output_value_4_after = sample.route_output_value_4_before;
    sample.valid_mask |= 1u << 6;
  }
  if (GuestMemory::Heap(sample.router, 260) || GuestMemory::Image(sample.router, 260)) {
    sample.request = memory.U32(sample.router + 256);
    sample.valid_mask |= 1u << 0;
  }
  if (sample.request &&
      (GuestMemory::Heap(sample.request, 76) || GuestMemory::Image(sample.request, 76))) {
    sample.request_state_72 = memory.U32(sample.request + 72);
    sample.request_state_60_before = memory.U8(sample.request + 60);
    sample.request_state_60_after = sample.request_state_60_before;
    sample.valid_mask |= 1u << 1;
  }
  if (sample.request &&
      (GuestMemory::Heap(sample.request, 172) || GuestMemory::Image(sample.request, 172))) {
    sample.request_cursor_164_before = memory.U32(sample.request + 164);
    sample.request_cursor_164_after = sample.request_cursor_164_before;
    sample.request_stack_168_before = memory.U32(sample.request + 168);
    sample.request_stack_168_after = sample.request_stack_168_before;
    if (sample.request_cursor_164_before > 0 &&
        (GuestMemory::Heap(sample.request_cursor_164_before - 1, 1) ||
         GuestMemory::Image(sample.request_cursor_164_before - 1, 1)))
      sample.request_opcode_before = memory.U8(sample.request_cursor_164_before - 1);
    sample.request_opcode_after = sample.request_opcode_before;
    sample.valid_mask |= 1u << 7;
  }
  if (GuestMemory::Image(kMissionSequenceGlobal, 4)) {
    sample.sequence = memory.U32(kMissionSequenceGlobal);
    if (sample.sequence &&
        (GuestMemory::Heap(sample.sequence, 716) || GuestMemory::Image(sample.sequence, 716))) {
      sample.sequence_696_before = memory.U8(sample.sequence + 696);
      sample.sequence_696_after = sample.sequence_696_before;
      sample.sequence_708_before = memory.U32(sample.sequence + 708);
      sample.sequence_708_after = sample.sequence_708_before;
      sample.sequence_712_before = memory.U32(sample.sequence + 712);
      sample.sequence_712_after = sample.sequence_712_before;
      sample.valid_mask |= 1u << 2;
    }
  }
  if (GuestMemory::Image(kTitleEventOwnerGlobal, 4)) {
    sample.event_owner_before = memory.U32(kTitleEventOwnerGlobal);
    sample.event_owner_after = sample.event_owner_before;
    sample.valid_mask |= 1u << 3;
    if (sample.event_owner_before &&
        (GuestMemory::Heap(sample.event_owner_before, 133) ||
         GuestMemory::Image(sample.event_owner_before, 133))) {
      sample.event_owner_marker_before = memory.U8(sample.event_owner_before + 132);
      sample.event_owner_marker_after = sample.event_owner_marker_before;
      sample.valid_mask |= 1u << 4;
    }
  }
  return sample;
}

uint64_t RecordDecoderOpcodeSample(uint8_t* base, uint32_t caller, uint32_t context) {
  if (!g_trace_input_consumers.load(std::memory_order_relaxed)) return 0;
  using edf::console::GuestMemory;
  if (!context || (!GuestMemory::Heap(context, 176) && !GuestMemory::Image(context, 176)))
    return 0;

  const GuestMemory memory(base);
  DecoderOpcodeSample sample;
  sample.call_order = g_decoder_opcode_call_order.fetch_add(1, std::memory_order_relaxed) + 1;
  sample.title_router_call_id = g_active_title_router_call_id;
  sample.caller = caller;
  sample.context = context;
  sample.cursor = memory.U32(context + 164);
  sample.context_160 = memory.U32(context + 160);
  sample.context_168 = memory.U32(context + 168);
  sample.context_172 = memory.U32(context + 172);
  sample.context_60_before = memory.U8(context + 60);
  sample.context_72_before = memory.U32(context + 72);
  if (GuestMemory::Heap(sample.context_168, 4) || GuestMemory::Image(sample.context_168, 4))
    sample.stack_top_before = memory.U32(sample.context_168);
  if (sample.context_168 >= 4 &&
      (GuestMemory::Heap(sample.context_168 - 4, 4) || GuestMemory::Image(sample.context_168 - 4, 4)))
    sample.stack_previous_before = memory.U32(sample.context_168 - 4);
  sample.elapsed_ms = g_input_script_elapsed_ms.load(std::memory_order_relaxed);
  sample.script_buttons = g_input_script_buttons.load(std::memory_order_relaxed);
  const uint16_t previous_buttons =
      g_decoder_trace_last_buttons.exchange(sample.script_buttons, std::memory_order_relaxed);
  if (previous_buttons != sample.script_buttons) {
    g_decoder_trace_capture_until_ms.store(sample.elapsed_ms + 500, std::memory_order_relaxed);
    g_decoder_trace_has_trigger.store(true, std::memory_order_relaxed);
  }
  const uint32_t decoder_trace_begin = g_decoder_trace_begin.load(std::memory_order_relaxed);
  const uint32_t decoder_trace_end = g_decoder_trace_end.load(std::memory_order_relaxed);
  const bool ordered_trace = decoder_trace_begin < decoder_trace_end;
  const bool in_trace_window = sample.script_buttons != 0 ||
      (g_decoder_trace_has_trigger.load(std::memory_order_relaxed) &&
       sample.elapsed_ms <= g_decoder_trace_capture_until_ms.load(std::memory_order_relaxed));
  const uint64_t byte_range_end = uint64_t(sample.cursor) + sample.bytes.size();
  const bool cursor_range_valid = (sample.cursor >= 0x40000000u &&
          byte_range_end <= 0x50000000ull) ||
      GuestMemory::Image(sample.cursor, static_cast<uint32_t>(sample.bytes.size()));
  if (!sample.cursor || !cursor_range_valid ||
      (ordered_trace && (sample.cursor < decoder_trace_begin ||
                         sample.cursor >= decoder_trace_end || !in_trace_window)))
    return 0;

  for (uint8_t i = 0; i < sample.bytes.size(); ++i)
    sample.bytes[i] = memory.U8(sample.cursor + i);
  sample.opcode = sample.bytes[0] & 0x3Fu;
  // Capture dispatch metadata for opcodes 44/45 and retain every opcode
  // boundary so the BVM control-flow path to a dispatch can be reconstructed.
  if (sample.opcode == 44 || sample.opcode == 45) {
    const uint8_t tag = sample.bytes[0] >> 6;
    const uint8_t operand_bytes = tag == 0 ? 0 : tag == 1 ? 1 : tag == 2 ? 2 : 4;
    uint32_t operand = 0;
    for (uint8_t i = 0; i < operand_bytes; ++i)
      operand |= uint32_t(sample.bytes[1 + i]) << (8 * i);
    sample.dispatch_phase = sample.opcode == 44 ? 0 : 1;
    sample.dispatch_event = operand & 0x1Fu;
  }
  sample.bytes_read = static_cast<uint8_t>(sample.bytes.size());

  std::lock_guard lock(g_state_mutex);
  constexpr std::size_t kMaximumDecoderOpcodeSamples = 65536;
  if (!ordered_trace) {
    const auto duplicate = std::find_if(g_state.decoder_opcode_samples.begin(),
        g_state.decoder_opcode_samples.end(), [&sample](const auto& prior) {
          return prior.caller == sample.caller && prior.context == sample.context &&
              prior.cursor == sample.cursor && prior.opcode == sample.opcode &&
              prior.bytes == sample.bytes && prior.bytes_read == sample.bytes_read &&
              prior.elapsed_ms == sample.elapsed_ms &&
              prior.script_buttons == sample.script_buttons;
        });
    if (duplicate != g_state.decoder_opcode_samples.end()) return 0;
    if (g_state.decoder_opcode_samples.size() >= 2048) return 0;
  } else if (g_state.decoder_opcode_samples.size() >= kMaximumDecoderOpcodeSamples) {
    g_decoder_trace_dropped.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  const uint64_t sample_index_token = g_state.decoder_opcode_samples.size() + 1;
  g_state.decoder_opcode_samples.push_back(std::move(sample));
  return sample_index_token;
}

void RecordRawVmDispatch(uint8_t* base, uint32_t context, uint32_t opcode,
                         uint32_t phase, uint32_t event, uint32_t target,
                         uint32_t callsite) {
  if (!g_trace_input_consumers.load(std::memory_order_relaxed)) return;
  using edf::console::GuestMemory;
  if (!context || (!GuestMemory::Heap(context, 176) && !GuestMemory::Image(context, 176)))
    return;

  const uint32_t elapsed_ms = g_input_script_elapsed_ms.load(std::memory_order_relaxed);
  const uint16_t script_buttons = g_input_script_buttons.load(std::memory_order_relaxed);
  const bool in_input_window = script_buttons != 0 ||
      (g_decoder_trace_has_trigger.load(std::memory_order_relaxed) &&
       elapsed_ms <= g_decoder_trace_capture_until_ms.load(std::memory_order_relaxed));
  if ((event & 0x1Fu) != 0x12 && !in_input_window) return;

  const GuestMemory memory(base);
  RawVmDispatchSample sample;
  sample.order = g_raw_vm_dispatch_order.fetch_add(1, std::memory_order_relaxed) + 1;
  sample.callsite = callsite;
  sample.context = context;
  sample.opcode = opcode;
  sample.phase = phase;
  sample.event = event & 0x1Fu;
  sample.cursor = memory.U32(context + 164);
  sample.stack = memory.U32(context + 168);
  if (sample.stack &&
      (GuestMemory::Heap(sample.stack, 4) || GuestMemory::Image(sample.stack, 4)))
    sample.stack_top = memory.U32(sample.stack);
  sample.receiver = memory.U32(context + 160);
  if (sample.receiver &&
      (GuestMemory::Heap(sample.receiver, 4) || GuestMemory::Image(sample.receiver, 4)))
    sample.receiver_vtable = memory.U32(sample.receiver);
  sample.target = target;
  sample.elapsed_ms = elapsed_ms;
  sample.script_buttons = script_buttons;

  std::lock_guard lock(g_state_mutex);
  constexpr std::size_t kMaximumRawVmDispatchSamples = 256;
  if (g_state.raw_vm_dispatch_samples.size() >= kMaximumRawVmDispatchSamples)
    g_state.raw_vm_dispatch_samples.erase(g_state.raw_vm_dispatch_samples.begin());
  g_state.raw_vm_dispatch_samples.push_back(sample);
}

void RecordMissionStartDispatch(uint8_t* base, uint32_t context, uint32_t opcode_pc,
                                uint32_t fallthrough_pc, uint32_t event,
                                uint32_t callback_target, uint32_t retry_byte_before,
                                uint32_t retry_count_before, bool retry_branch_taken) {
  if (!g_trace_input_consumers.load(std::memory_order_relaxed)) return;
  const uint32_t elapsed_ms = g_input_script_elapsed_ms.load(std::memory_order_relaxed);
  const uint16_t script_buttons = g_input_script_buttons.load(std::memory_order_relaxed);
  const bool in_input_window = script_buttons != 0 ||
      (g_decoder_trace_has_trigger.load(std::memory_order_relaxed) &&
       elapsed_ms <= g_decoder_trace_capture_until_ms.load(std::memory_order_relaxed));
  if ((event & 0x1Fu) != 0x12 && !in_input_window) return;

  using edf::console::GuestMemory;
  if (!context || (!GuestMemory::Heap(context, 176) && !GuestMemory::Image(context, 176)))
    return;

  const GuestMemory memory(base);
  MissionStartDispatchSample sample;
  sample.order = g_mission_start_dispatch_order.fetch_add(1, std::memory_order_relaxed) + 1;
  sample.context = context;
  sample.opcode_pc = opcode_pc;
  sample.fallthrough_pc = fallthrough_pc;
  sample.retry_byte_before = retry_byte_before;
  sample.retry_byte_after = memory.U8(context + 60);
  sample.retry_count_before = retry_count_before;
  sample.retry_count_after = memory.U32(context + 72);
  sample.event = event;
  sample.callback_target = callback_target;
  sample.elapsed_ms = elapsed_ms;
  sample.script_buttons = script_buttons;
  sample.input_window = in_input_window;
  sample.retry_branch_taken = retry_branch_taken;

  constexpr uint32_t kTimerGlobal = 0x8257869C;
  if (GuestMemory::Image(kTimerGlobal, 4)) {
    const uint32_t timer = memory.U32(kTimerGlobal);
    if (GuestMemory::Heap(timer, 36) || GuestMemory::Image(timer, 36)) {
      sample.guest_time_a = memory.U32(timer + 20);
      sample.guest_time_b = memory.U32(timer + 32);
      sample.guest_time_valid = true;
    }
  }

  std::lock_guard lock(g_state_mutex);
  constexpr std::size_t kMaximumMissionStartDispatchSamples = 64;
  if (g_state.mission_start_dispatch_samples.size() >= kMaximumMissionStartDispatchSamples) {
    // Only input-window evidence may displace an older sample. Non-input event
    // 0x12 records are useful baseline context, but should not churn that set.
    if (!sample.input_window) {
      ++g_state.mission_start_dispatch_dropped;
      return;
    }
    const auto earliest_non_input = std::find_if(
        g_state.mission_start_dispatch_samples.begin(),
        g_state.mission_start_dispatch_samples.end(),
        [](const auto& prior) { return !prior.input_window; });
    if (earliest_non_input == g_state.mission_start_dispatch_samples.end()) {
      ++g_state.mission_start_dispatch_dropped;
      return;
    }
    g_state.mission_start_dispatch_samples.erase(earliest_non_input);
  }
  g_state.mission_start_dispatch_samples.push_back(sample);
}

void RecordTitleConfirmCompare(uint8_t* base, uint32_t object, uint32_t result) {
  if (!g_trace_input_consumers.load(std::memory_order_relaxed) ||
      !g_active_title_router_call_id)
    return;
  using edf::console::GuestMemory;
  if (!GuestMemory::Heap(object, 36) && !GuestMemory::Image(object, 36)) return;
  const GuestMemory memory(base);
  ++g_active_title_router_compare_calls;
  g_active_title_router_compare_result = result;
  g_active_title_router_compare_object = object;
  g_active_title_router_compare_value_20 = memory.U32(object + 20);
  g_active_title_router_compare_value_32 = memory.U32(object + 32);
}

void RecordDecoderOpcodeResult(uint8_t* base, uint32_t context, uint64_t sample_index_token) {
  if (!sample_index_token || !g_trace_input_consumers.load(std::memory_order_relaxed)) return;
  using edf::console::GuestMemory;
  if (!context || (!GuestMemory::Heap(context, 176) && !GuestMemory::Image(context, 176))) return;

  const GuestMemory memory(base);
  const uint32_t cursor_after = memory.U32(context + 164);
  const uint32_t stack_after = memory.U32(context + 168);
  uint32_t stack_top_after = 0;
  uint32_t stack_previous_after = 0;
  if (GuestMemory::Heap(stack_after, 4) || GuestMemory::Image(stack_after, 4))
    stack_top_after = memory.U32(stack_after);
  if (stack_after >= 4 &&
      (GuestMemory::Heap(stack_after - 4, 4) || GuestMemory::Image(stack_after - 4, 4)))
    stack_previous_after = memory.U32(stack_after - 4);
  const uint32_t status_after = memory.U8(context + 60);
  const uint32_t state_72_after = memory.U32(context + 72);

  std::lock_guard lock(g_state_mutex);
  const std::size_t sample_index = static_cast<std::size_t>(sample_index_token - 1);
  if (sample_index >= g_state.decoder_opcode_samples.size()) return;
  auto* sample = &g_state.decoder_opcode_samples[sample_index];
  sample->cursor_after = cursor_after;
  sample->context_168_after = stack_after;
  sample->stack_top_after = stack_top_after;
  sample->stack_previous_after = stack_previous_after;
  sample->context_60_after = status_after;
  sample->context_72_after = state_72_after;
}

void AssociateDecoderOpcodeTitleRouterCall(uint64_t sample_index_token,
                                           uint64_t title_router_call_id) {
  if (!sample_index_token || !title_router_call_id) return;
  std::lock_guard lock(g_state_mutex);
  const std::size_t sample_index = static_cast<std::size_t>(sample_index_token - 1);
  if (sample_index >= g_state.decoder_opcode_samples.size()) return;
  g_state.decoder_opcode_samples[sample_index].title_router_call_id = title_router_call_id;
}

void RecordTitleRouterTransition(uint8_t* base, TitleRouterTransitionSample sample) {
  if (!g_trace_input_consumers.load(std::memory_order_relaxed)) return;
  using edf::console::GuestMemory;
  constexpr uint32_t kMissionSequenceGlobal = 0x825786AC;
  constexpr uint32_t kTitleEventOwnerGlobal = 0x82578E80;
  constexpr uint32_t kTitleStateGlobal = 0x8257C030;
  const GuestMemory memory(base);
  if ((sample.valid_mask & (1u << 1)) &&
      (GuestMemory::Heap(sample.request, 76) || GuestMemory::Image(sample.request, 76)))
    sample.request_state_60_after = memory.U8(sample.request + 60);
  if ((sample.valid_mask & (1u << 7)) &&
      (GuestMemory::Heap(sample.request, 172) || GuestMemory::Image(sample.request, 172))) {
    sample.request_cursor_164_after = memory.U32(sample.request + 164);
    sample.request_stack_168_after = memory.U32(sample.request + 168);
    sample.request_opcode_after = UINT32_MAX;
    if (sample.request_cursor_164_after > 0 &&
        (GuestMemory::Heap(sample.request_cursor_164_after - 1, 1) ||
         GuestMemory::Image(sample.request_cursor_164_after - 1, 1)))
      sample.request_opcode_after = memory.U8(sample.request_cursor_164_after - 1);
  }
  if ((sample.valid_mask & (1u << 2)) &&
      (GuestMemory::Heap(sample.sequence, 716) || GuestMemory::Image(sample.sequence, 716))) {
    sample.sequence_696_after = memory.U8(sample.sequence + 696);
    sample.sequence_708_after = memory.U32(sample.sequence + 708);
    sample.sequence_712_after = memory.U32(sample.sequence + 712);
  }
  if ((sample.valid_mask & (1u << 5)) &&
      (GuestMemory::Heap(sample.latch_owner, 61) ||
       GuestMemory::Image(sample.latch_owner, 61)))
    sample.latch_owner_state_60_after = memory.U8(sample.latch_owner + 60);
  if ((sample.valid_mask & (1u << 6)) &&
      (GuestMemory::Heap(sample.route_output, 8) ||
       GuestMemory::Image(sample.route_output, 8)))
    sample.route_output_value_4_after = memory.U32(sample.route_output + 4);
  if (GuestMemory::Image(kTitleStateGlobal, 4)) {
    sample.title_state_pointer_valid_after = true;
    sample.title_state_after = memory.U32(kTitleStateGlobal);
    if (GuestMemory::Heap(sample.title_state_after, 2264) ||
        GuestMemory::Image(sample.title_state_after, 2264)) {
      sample.title_state_valid_after = true;
      for (std::size_t i = 0; i < sample.title_state_flags_after.size(); ++i) {
        sample.title_state_flags_after[i] = memory.U8(
            sample.title_state_after + 2260 + static_cast<uint32_t>(i));
        sample.title_state_flags_valid_mask_after |= static_cast<uint8_t>(1u << i);
      }
    }
  }
  if (GuestMemory::Image(kTitleEventOwnerGlobal, 4)) {
    sample.event_owner_after = memory.U32(kTitleEventOwnerGlobal);
    if (sample.event_owner_after &&
        (GuestMemory::Heap(sample.event_owner_after, 133) ||
         GuestMemory::Image(sample.event_owner_after, 133))) {
      sample.event_owner_marker_after = memory.U8(sample.event_owner_after + 132);
      sample.valid_mask |= 1u << 4;
    }
  }
  const bool noisy_confirm_poll = sample.route_phase == 0 && sample.event == 6 &&
      sample.script_buttons == 0;
  const bool request_interpreter_changed =
      sample.request_state_60_before != sample.request_state_60_after ||
      sample.latch_owner_state_60_before != sample.latch_owner_state_60_after ||
      sample.request_cursor_164_before != sample.request_cursor_164_after ||
      sample.request_stack_168_before != sample.request_stack_168_after ||
      sample.request_opcode_before != sample.request_opcode_after;
  const bool state_changed = (!noisy_confirm_poll && request_interpreter_changed) ||
      sample.event_owner_before != sample.event_owner_after ||
      sample.event_owner_marker_before != sample.event_owner_marker_after ||
      sample.route_output_value_4_before != sample.route_output_value_4_after ||
      sample.title_state_pointer_valid != sample.title_state_pointer_valid_after ||
      sample.title_state != sample.title_state_after ||
      sample.title_state_valid != sample.title_state_valid_after ||
      sample.title_state_flags != sample.title_state_flags_after ||
      sample.title_state_flags_valid_mask != sample.title_state_flags_valid_mask_after ||
      sample.sequence_696_before != sample.sequence_696_after ||
      sample.sequence_708_before != sample.sequence_708_after ||
      sample.sequence_712_before != sample.sequence_712_after;
  // Phase 0's decimal event 18 dispatches sub_820C59F0, whose request+72 /
  // sequence+712 guard decides whether the mission sequence can begin. Preserve
  // event 18 from every phase so the bounded trace retains its producer context even
  // when a phase-1 route has no visible state change. Phase 0/decimal event 24
  // is also retained without a state change to capture its route sample.
  const bool title_event_18 = sample.event == 18;
  const bool title_event_24 = sample.route_phase == 0 && sample.event == 24;
  if (!sample.script_buttons && !state_changed && !title_event_18 && !title_event_24) return;

  std::lock_guard lock(g_state_mutex);
  const auto duplicate = std::find_if(g_state.title_router_transitions.begin(),
      g_state.title_router_transitions.end(), [&sample](const auto& prior) {
       return prior.script_elapsed_ms == sample.script_elapsed_ms &&
             prior.callsite == sample.callsite && prior.route_phase == sample.route_phase &&
            prior.call_id == sample.call_id &&
            prior.event == sample.event && prior.decoder_context == sample.decoder_context &&
            prior.decoder_cursor_slot == sample.decoder_cursor_slot &&
            prior.decoder_output == sample.decoder_output &&
            prior.decoder_context_160 == sample.decoder_context_160 &&
            prior.decoder_context_164 == sample.decoder_context_164 &&
             prior.decoder_context_168 == sample.decoder_context_168 &&
             prior.decoder_context_172 == sample.decoder_context_172 &&
             prior.decoder_stack_top == sample.decoder_stack_top &&
             prior.decoder_stack_previous == sample.decoder_stack_previous &&
             prior.decoder_entry_callsite == sample.decoder_entry_callsite &&
             prior.callback_arguments == sample.callback_arguments &&
             prior.callback_valid_mask == sample.callback_valid_mask &&
             prior.title_state == sample.title_state &&
             prior.title_state_flags == sample.title_state_flags &&
             prior.title_state_pointer_valid == sample.title_state_pointer_valid &&
             prior.title_state_valid == sample.title_state_valid &&
             prior.title_state_flags_valid_mask == sample.title_state_flags_valid_mask &&
             prior.title_state_after == sample.title_state_after &&
             prior.title_state_flags_after == sample.title_state_flags_after &&
             prior.title_state_pointer_valid_after == sample.title_state_pointer_valid_after &&
             prior.title_state_valid_after == sample.title_state_valid_after &&
             prior.title_state_flags_valid_mask_after ==
                 sample.title_state_flags_valid_mask_after &&
             prior.router == sample.router &&
            prior.latch_owner == sample.latch_owner &&
            prior.latch_owner_state_60_before == sample.latch_owner_state_60_before &&
            prior.latch_owner_state_60_after == sample.latch_owner_state_60_after &&
            prior.route_output == sample.route_output &&
            prior.route_output_value_4_before == sample.route_output_value_4_before &&
            prior.route_output_value_4_after == sample.route_output_value_4_after &&
            prior.request == sample.request && prior.request_state_72 == sample.request_state_72 &&
            prior.request_cursor_164_before == sample.request_cursor_164_before &&
            prior.request_cursor_164_after == sample.request_cursor_164_after &&
            prior.request_stack_168_before == sample.request_stack_168_before &&
            prior.request_stack_168_after == sample.request_stack_168_after &&
            prior.request_opcode_before == sample.request_opcode_before &&
            prior.request_opcode_after == sample.request_opcode_after &&
            prior.request_state_60_before == sample.request_state_60_before &&
            prior.request_state_60_after == sample.request_state_60_after &&
            prior.event_owner_before == sample.event_owner_before &&
            prior.event_owner_after == sample.event_owner_after &&
            prior.event_owner_marker_before == sample.event_owner_marker_before &&
            prior.event_owner_marker_after == sample.event_owner_marker_after &&
            prior.sequence == sample.sequence &&
            prior.sequence_696_before == sample.sequence_696_before &&
            prior.sequence_696_after == sample.sequence_696_after &&
            prior.sequence_708_before == sample.sequence_708_before &&
            prior.sequence_708_after == sample.sequence_708_after &&
            prior.sequence_712_before == sample.sequence_712_before &&
            prior.sequence_712_after == sample.sequence_712_after &&
            prior.compare_calls == sample.compare_calls &&
            prior.compare_result == sample.compare_result &&
            prior.compare_object == sample.compare_object &&
            prior.compare_value_20 == sample.compare_value_20 &&
            prior.compare_value_32 == sample.compare_value_32 &&
            prior.script_buttons == sample.script_buttons;
      });
  if (duplicate != g_state.title_router_transitions.end()) return;
  constexpr std::size_t kMaximumTitleRouterTransitions = 96;
  if (g_state.title_router_transitions.size() >= kMaximumTitleRouterTransitions) {
    // These are decimal event IDs: 6 is noisiest; phase-0 24 ranks above
    // ordinary samples (including non-phase-0 18), while phase-0 18 is highest.
    const auto priority = [](const auto& transition) {
      if (transition.route_phase == 0 && transition.event == 18) return 3;
      if (transition.route_phase == 0 && transition.event == 24) return 2;
      if (transition.event == 6) return 0;
      return 1;
    };
    const int incoming_priority = priority(sample);
    auto lowest = g_state.title_router_transitions.end();
    int lowest_priority = incoming_priority;
    for (auto prior = g_state.title_router_transitions.begin();
         prior != g_state.title_router_transitions.end(); ++prior) {
      const int prior_priority = priority(*prior);
      if (prior_priority < lowest_priority) {
        lowest = prior;
        lowest_priority = prior_priority;
      }
    }
    if (lowest == g_state.title_router_transitions.end()) return;
    g_state.title_router_transitions.erase(lowest);
  }
  g_state.title_router_transitions.push_back(std::move(sample));
}

void RecordMissionSequenceGate(uint8_t* base, uint32_t callsite, uint32_t sequence_object,
                               uint32_t helper_result, uint32_t actual_r31,
                               uint32_t actual_r25, uint32_t branch_value) {
  if (!g_trace_input_consumers.load(std::memory_order_relaxed)) return;
  using edf::console::GuestMemory;
  constexpr uint32_t kTitleStateGlobal = 0x8257C030;
  constexpr uint32_t kInputManagerGlobal = 0x82578640;
  constexpr uint32_t kTimerGlobal = 0x8257869C;
  const GuestMemory memory(base);
  MissionSequenceGateSample sample;
  sample.callsite = callsite;
  sample.sequence_object = sequence_object;
  sample.helper_result = helper_result;
  sample.actual_r31 = actual_r31;
  sample.actual_r25 = actual_r25;
  sample.branch_value = branch_value;

  if (sequence_object && (GuestMemory::Heap(sequence_object, 716) ||
                          GuestMemory::Image(sequence_object, 716))) {
    sample.sequence_696 = memory.U8(sequence_object + 696);
    sample.sequence_708 = memory.U32(sequence_object + 708);
    sample.sequence_712 = memory.U32(sequence_object + 712);
    sample.valid_mask |= 1u << 0;
  }
  if (GuestMemory::Image(kTitleStateGlobal, 4)) {
    const uint32_t title_state = memory.U32(kTitleStateGlobal);
    if (GuestMemory::Heap(title_state, 2263) || GuestMemory::Image(title_state, 2263)) {
      sample.title_state = title_state;
      sample.title_flag_2260 = memory.U8(title_state + 2260);
      sample.valid_mask |= 1u << 1;
    }
  }
  if (GuestMemory::Image(kInputManagerGlobal, 4)) {
    const uint32_t manager = memory.U32(kInputManagerGlobal);
    if (GuestMemory::Heap(manager, 2340) || GuestMemory::Image(manager, 2340)) {
      sample.input_manager = manager;
      sample.input_count = memory.U32(manager + 2336);
      sample.valid_mask |= 1u << 2;
    }
  }
  if (GuestMemory::Image(kTimerGlobal, 4)) {
    const uint32_t timer = memory.U32(kTimerGlobal);
    if (GuestMemory::Heap(timer, 36) || GuestMemory::Image(timer, 36)) {
      sample.timer_a = memory.U32(timer + 20);
      sample.timer_b = memory.U32(timer + 32);
      sample.valid_mask |= 1u << 3;
    }
  }

  std::lock_guard lock(g_state_mutex);
  const auto found = std::find_if(g_state.mission_sequence_gate_trace.begin(),
                                  g_state.mission_sequence_gate_trace.end(),
                                  [callsite](const auto& item) {
                                    return item.callsite == callsite;
                                  });
  if (found != g_state.mission_sequence_gate_trace.end()) {
    sample.calls = found->calls + 1;
    *found = sample;
  } else if (g_state.mission_sequence_gate_trace.size() < 16) {
    sample.calls = 1;
    g_state.mission_sequence_gate_trace.push_back(sample);
  }
}

#if !defined(EDF2027_AUTHORITATIVE_HOST)
void ConfigureInputUser(uint32_t user) {
  g_input_user.store(user, std::memory_order_relaxed);
}
#endif

void ConfigureNetworkInput() {
#if defined(EDF2027_AUTHORITATIVE_HOST)
  {
    std::lock_guard lock(g_state_mutex);
    g_state.mission_actor_removals.removals.reserve(kMaxMissionActorRemovalHistory);
  }
#endif
  std::lock_guard lock(g_network_input_mutex);
  g_network_input.Configure();
  g_mission_probe_enabled = true;
}

#if defined(EDF2027_AUTHORITATIVE_HOST)
void ConfigurePlayerSpawnProbe(uint32_t player_index) {
  if (player_index < 1 || player_index > 3) player_index = 0;
  g_player_spawn_probe_attempted.store(false, std::memory_order_relaxed);
  g_player_spawn_probe_index.store(player_index, std::memory_order_relaxed);
}
#endif

void MarkNetworkMissionReady() {
  std::lock_guard lock(g_network_input_mutex);
  g_network_input.MarkMissionReady();
}

void PublishNetworkInput(uint16_t player_id, std::optional<InputCommand> command,
                         std::optional<NetworkPositionReset> position_reset) {
  std::array<bool, kNetworkGuestSeatCount> validated_seats{};
  if (command && command->gameplay_active) {
    const auto guest = ReadGuestProbeState();
    const auto now = std::chrono::steady_clock::now();
    const bool fresh = guest.player_pose_sampled_at != std::chrono::steady_clock::time_point{} &&
        now >= guest.player_pose_sampled_at && now - guest.player_pose_sampled_at <= std::chrono::milliseconds(500);
    for (const auto& player : guest.player_objects) {
      if (player.player_index < 0 || player.player_index >= static_cast<int32_t>(validated_seats.size())) continue;
      const auto index = static_cast<std::size_t>(player.player_index);
      validated_seats[index] = !guest.loading && fresh && !player.ambiguous && player.object && player.alive;
    }
  }
  std::lock_guard lock(g_network_input_mutex);
  g_network_input.Publish(player_id, std::move(command), std::move(position_reset), validated_seats);
}

bool PublishNetworkPositionReset(const NetworkPositionReset& reset) {
  std::lock_guard lock(g_network_input_mutex);
  return g_network_input.PublishPositionReset(reset);
}

void AcknowledgeNetworkPositionReset(uint16_t player_id, uint32_t reset_id) {
  std::lock_guard lock(g_network_input_mutex);
  g_network_input.AcknowledgePositionReset(player_id, reset_id);
}

bool WithPendingNetworkPositionReset(
    uint16_t player_id, uint32_t reset_id,
  const std::function<void(NetworkPositionReset&)>& callback) {
  std::lock_guard lock(g_network_input_mutex);
  return g_network_input.WithPendingPositionReset(player_id, reset_id, callback);
}

NetworkInputView ReadNetworkInput() {
  std::lock_guard lock(g_network_input_mutex);
  return g_network_input.Read();
}

GuestProbeState ReadGuestProbeState() {
  GuestProbeState state;
  {
    std::lock_guard lock(g_state_mutex);
    state = g_state;
  }
  state.headless_fence_waits = g_headless_fence_waits.load(std::memory_order_relaxed);
  state.headless_allocator_waits = g_headless_allocator_waits.load(std::memory_order_relaxed);
  state.mission_sequence_begins = g_mission_sequence_begins.load(std::memory_order_relaxed);
  state.input_script_elapsed_ms = g_input_script_elapsed_ms.load(std::memory_order_relaxed);
  state.input_script_buttons = g_input_script_buttons.load(std::memory_order_relaxed);
  state.input_consumer_trace_enabled = g_trace_input_consumers.load(std::memory_order_relaxed);
  state.decoder_trace_begin = g_decoder_trace_begin.load(std::memory_order_relaxed);
  state.decoder_trace_end = g_decoder_trace_end.load(std::memory_order_relaxed);
  state.decoder_trace_dropped = g_decoder_trace_dropped.load(std::memory_order_relaxed);
  for (std::size_t user = 0; user < g_input_consumer_mask_polls.size(); ++user)
    for (std::size_t site = 0; site < g_input_consumer_mask_polls[user].size(); ++site)
      for (std::size_t mask = 0; mask < g_input_consumer_mask_polls[user][site].size(); ++mask)
        state.input_consumer_mask_polls[user][site][mask] =
            g_input_consumer_mask_polls[user][site][mask].load(std::memory_order_relaxed);
  for (std::size_t user = 0; user < state.xpad_action_poll_calls.size(); ++user) {
    state.xpad_action_poll_calls[user] = g_xpad_action_poll_calls[user].load(std::memory_order_relaxed);
    state.xpad_action_targets_seen[user] = g_xpad_action_targets_seen[user].load(std::memory_order_relaxed);
    state.xpad_action_channels_seen[user] = g_xpad_action_channels_seen[user].load(std::memory_order_relaxed);
    state.xpad_action_state_samples[user] = g_xpad_action_state_samples[user].load(std::memory_order_relaxed);
    state.xpad_action_held_seen[user] = g_xpad_action_held_seen[user].load(std::memory_order_relaxed);
    state.xpad_action_edges_seen[user] = g_xpad_action_edges_seen[user].load(std::memory_order_relaxed);
    state.xpad_action_pulses_seen[user] = g_xpad_action_pulses_seen[user].load(std::memory_order_relaxed);
    state.xpad_action_invalid_samples[user] = g_xpad_action_invalid_samples[user].load(std::memory_order_relaxed);
  }
  for (std::size_t phase = 0; phase < state.title_ui_router_event_calls.size(); ++phase)
    for (std::size_t event = 0; event < state.title_ui_router_event_calls[phase].size(); ++event)
      state.title_ui_router_event_calls[phase][event] =
          g_title_ui_router_event_calls[phase][event].load(std::memory_order_relaxed);
  state.title_action_handler_calls = g_title_action_handler_calls.load(std::memory_order_relaxed);
  state.title_action_handler_last_r31 = g_title_action_handler_last_r31.load(std::memory_order_relaxed);
  state.title_action_default_calls = g_title_action_default_calls.load(std::memory_order_relaxed);
  state.title_action_default_last_r31 = g_title_action_default_last_r31.load(std::memory_order_relaxed);
  state.title_action_consumer_reads = g_title_action_consumer_reads.load(std::memory_order_relaxed);
  state.title_action_consumer_event_mask = g_title_action_consumer_event_mask.load(std::memory_order_relaxed);
  state.title_action_consumer_last_event = g_title_action_consumer_last_event.load(std::memory_order_relaxed);
  state.title_action_consumer_last_object = g_title_action_consumer_last_object.load(std::memory_order_relaxed);
  state.title_action_consumer_last_slot = g_title_action_consumer_last_slot.load(std::memory_order_relaxed);
  state.title_action_consumer_last_count = g_title_action_consumer_last_count.load(std::memory_order_relaxed);
  state.title_action_consumer_last_callsite = g_title_action_consumer_last_callsite.load(std::memory_order_relaxed);
  for (std::size_t slot = 0; slot < state.title_ui_action_slot_reads.size(); ++slot) {
    state.title_ui_action_slot_reads[slot] = g_title_ui_action_slot_reads[slot].load(std::memory_order_relaxed);
    state.title_ui_action_slot_event_values[slot] =
        g_title_ui_action_slot_event_values[slot].load(std::memory_order_relaxed);
    state.title_ui_action_slot_input_flags[slot] =
        g_title_ui_action_slot_input_flags[slot].load(std::memory_order_relaxed);
    state.title_ui_action_slot_last_event[slot] =
        g_title_ui_action_slot_last_event[slot].load(std::memory_order_relaxed);
    state.input_event_update_slot_calls[slot] =
        g_input_event_update_slot_calls[slot].load(std::memory_order_relaxed);
    state.input_event_update_slot_callbacks[slot] =
        g_input_event_update_slot_callbacks[slot].load(std::memory_order_relaxed);
    state.input_event_update_slot_last_event[slot] =
        g_input_event_update_slot_last_event[slot].load(std::memory_order_relaxed);
    state.input_event_update_slot_nonzero_seen[slot] =
        g_input_event_update_slot_nonzero_seen[slot].load(std::memory_order_relaxed);
  }
  state.input_event_update_calls = g_input_event_update_calls.load(std::memory_order_relaxed);
  state.input_event_update_nonzero_calls =
      g_input_event_update_nonzero_calls.load(std::memory_order_relaxed);
  state.input_event_update_last_object =
      g_input_event_update_last_object.load(std::memory_order_relaxed);
  state.input_event_update_last_callback =
      g_input_event_update_last_callback.load(std::memory_order_relaxed);
  state.input_event_update_last_event_before =
      g_input_event_update_last_event_before.load(std::memory_order_relaxed);
  state.input_event_update_last_event_after =
      g_input_event_update_last_event_after.load(std::memory_order_relaxed);
  state.input_event_update_last_callsite =
      g_input_event_update_last_callsite.load(std::memory_order_relaxed);
  state.title_action_slot_scan_calls = g_title_action_slot_scan_calls.load(std::memory_order_relaxed);
  state.title_action_slot_scan_manager = g_title_action_slot_scan_manager.load(std::memory_order_relaxed);
  state.title_action_slot_scan_slot = g_title_action_slot_scan_slot.load(std::memory_order_relaxed);
  state.title_action_slot_scan_count = g_title_action_slot_scan_count.load(std::memory_order_relaxed);
  state.title_action_slot_scan_object = g_title_action_slot_scan_object.load(std::memory_order_relaxed);
  state.title_action_slot_scan_event = g_title_action_slot_scan_event.load(std::memory_order_relaxed);
  state.title_ui_handoff_calls = g_title_ui_handoff_calls.load(std::memory_order_relaxed);
  state.title_scene_update_calls = g_title_scene_update_calls.load(std::memory_order_relaxed);
  state.title_sequence_request.calls = g_title_sequence_request_calls.load(std::memory_order_relaxed);
  state.title_sequence_request.callsite = g_title_sequence_request_callsite.load(std::memory_order_relaxed);
  state.title_sequence_request.receiver = g_title_sequence_request_receiver.load(std::memory_order_relaxed);
  state.title_sequence_request.request_object = g_title_sequence_request_object.load(std::memory_order_relaxed);
  state.title_sequence_request.request_state_72_before = g_title_sequence_request_state_72_before.load(std::memory_order_relaxed);
  state.title_sequence_request.request_state_60_before = g_title_sequence_request_state_60_before.load(std::memory_order_relaxed);
  state.title_sequence_request.request_state_60_after = g_title_sequence_request_state_60_after.load(std::memory_order_relaxed);
  state.title_sequence_request.sequence_object = g_title_sequence_request_sequence_object.load(std::memory_order_relaxed);
  state.title_sequence_request.sequence_state_696_before = g_title_sequence_request_sequence_696_before.load(std::memory_order_relaxed);
  state.title_sequence_request.sequence_state_696_after = g_title_sequence_request_sequence_696_after.load(std::memory_order_relaxed);
  state.title_sequence_request.sequence_state_708_before = g_title_sequence_request_sequence_708_before.load(std::memory_order_relaxed);
  state.title_sequence_request.sequence_state_708_after = g_title_sequence_request_sequence_708_after.load(std::memory_order_relaxed);
  state.title_sequence_request.sequence_state_712_before = g_title_sequence_request_sequence_712_before.load(std::memory_order_relaxed);
  state.title_sequence_request.sequence_state_712_after = g_title_sequence_request_sequence_712_after.load(std::memory_order_relaxed);
  state.title_action_gate_samples = g_title_action_gate_samples.load(std::memory_order_relaxed);
  state.title_action_gate_2260_last = g_title_action_gate_2260_last.load(std::memory_order_relaxed);
  state.title_action_gate_2261_last = g_title_action_gate_2261_last.load(std::memory_order_relaxed);
  state.title_action_gate_2262_last = g_title_action_gate_2262_last.load(std::memory_order_relaxed);
  state.title_action_gate_a_object = g_title_action_gate_a_object.load(std::memory_order_relaxed);
  state.title_action_gate_a_link = g_title_action_gate_a_link.load(std::memory_order_relaxed);
  state.title_action_gate_a_state = g_title_action_gate_a_state.load(std::memory_order_relaxed);
  state.title_action_gate_a_result = g_title_action_gate_a_result.load(std::memory_order_relaxed);
  state.title_action_gate_b_object = g_title_action_gate_b_object.load(std::memory_order_relaxed);
  state.title_action_gate_b_712 = g_title_action_gate_b_712.load(std::memory_order_relaxed);
  state.title_action_gate_b_696 = g_title_action_gate_b_696.load(std::memory_order_relaxed);
  state.title_action_gate_b_708 = g_title_action_gate_b_708.load(std::memory_order_relaxed);
  state.title_action_gate_b_result = g_title_action_gate_b_result.load(std::memory_order_relaxed);
  state.title_action_gate_sequence_pointer =
      g_title_action_gate_sequence_pointer.load(std::memory_order_relaxed);
  state.title_action_gate_sequence_708 =
      g_title_action_gate_sequence_708.load(std::memory_order_relaxed);
  state.title_action_gate_sequence_result =
      g_title_action_gate_sequence_result.load(std::memory_order_relaxed);
  state.title_action_gate_state_object =
      g_title_action_gate_state_object.load(std::memory_order_relaxed);
  state.title_action_gate_state_vtable =
      g_title_action_gate_state_vtable.load(std::memory_order_relaxed);
  {
    std::lock_guard lock(g_state_mutex);
    state.title_action_gate_state_class_name =
        g_state.title_action_gate_state_class_name;
  }
  state.title_action_gate_input_manager =
      g_title_action_gate_input_manager.load(std::memory_order_relaxed);
  state.title_action_gate_input_count =
      g_title_action_gate_input_count.load(std::memory_order_relaxed);
  state.title_action_gate_a_function_calls =
      g_title_action_gate_a_function_calls.load(std::memory_order_relaxed);
  state.title_action_gate_a_function_callsite =
      g_title_action_gate_a_function_callsite.load(std::memory_order_relaxed);
  state.title_action_gate_a_function_return =
      g_title_action_gate_a_function_return.load(std::memory_order_relaxed);
  state.title_action_gate_b_function_calls =
      g_title_action_gate_b_function_calls.load(std::memory_order_relaxed);
  state.title_action_gate_b_function_callsite =
      g_title_action_gate_b_function_callsite.load(std::memory_order_relaxed);
  state.title_action_gate_b_function_return =
      g_title_action_gate_b_function_return.load(std::memory_order_relaxed);
  state.title_action_gate_r31 = g_title_action_gate_r31.load(std::memory_order_relaxed);
  state.title_action_gate_r25 = g_title_action_gate_r25.load(std::memory_order_relaxed);
  state.title_action_gate_estimates_valid =
      g_title_action_gate_estimates_valid.load(std::memory_order_relaxed);
  state.title_action_gate_timer_valid =
      g_title_action_gate_timer_valid.load(std::memory_order_relaxed);
  state.title_action_gate_timer_a = g_title_action_gate_timer_a.load(std::memory_order_relaxed);
  state.title_action_gate_timer_b = g_title_action_gate_timer_b.load(std::memory_order_relaxed);
  state.title_action_gate_timer_branch_taken =
      g_title_action_gate_timer_branch_taken.load(std::memory_order_relaxed);
  state.title_action_user_samples = g_title_action_user_samples.load(std::memory_order_relaxed);
  state.title_action_slot_gate_count = g_title_action_user_count.load(std::memory_order_relaxed);
  state.title_action_slot_gate_object_mask = g_title_action_user_object_mask.load(std::memory_order_relaxed);
  state.title_action_slot_gate_mask = g_title_action_slot_gate_mask.load(std::memory_order_relaxed);
  state.title_action_slot_gate_seen = g_title_action_slot_gate_seen.load(std::memory_order_relaxed);
  state.input_dispatch_gate_calls = g_input_dispatch_gate_calls.load(std::memory_order_relaxed);
  state.input_dispatch_gate_slot_matches =
      g_input_dispatch_gate_slot_matches.load(std::memory_order_relaxed);
  state.title_action_registry_pointer = g_title_action_registry_pointer.load(std::memory_order_relaxed);
  state.title_action_user_invalid_stage = g_title_action_user_invalid_stage.load(std::memory_order_relaxed);
  state.title_action_user_invalid_samples =
      g_title_action_user_invalid_samples.load(std::memory_order_relaxed);
  state.scheduler_calls = g_scheduler_calls.load(std::memory_order_relaxed);
  state.guest_wait_calls = g_guest_wait_calls.load(std::memory_order_relaxed);
  state.input_user = g_input_user.load(std::memory_order_relaxed);
  for (std::size_t user = 0; user < state.xinput_user_wrapper_calls.size(); ++user) {
    state.xinput_user_wrapper_calls[user] = g_xinput_user_wrapper_calls[user].load(std::memory_order_relaxed);
    state.xinput_user_calls_last_sampled_ready[user] =
        g_xinput_user_calls_last_sampled_ready[user].load(std::memory_order_relaxed);
  }
  state.other_xinput_wrapper_calls = g_other_xinput_wrapper_calls.load(std::memory_order_relaxed);
  state.other_xinput_calls_last_sampled_ready =
      g_other_xinput_calls_last_sampled_ready.load(std::memory_order_relaxed);
  for (std::size_t user = 0; user < state.xinput_user_native_forwards.size(); ++user) {
    state.xinput_user_native_forwards[user] = g_xinput_user_native_forwards[user].load(std::memory_order_relaxed);
    state.xinput_user_native_forwards_last_sampled_ready[user] =
        g_xinput_user_native_forwards_last_sampled_ready[user].load(std::memory_order_relaxed);
  }
  state.other_xinput_native_forwards = g_other_xinput_native_forwards.load(std::memory_order_relaxed);
  state.other_xinput_native_forwards_last_sampled_ready =
      g_other_xinput_native_forwards_last_sampled_ready.load(std::memory_order_relaxed);
  return state;
}

GuestPlayerSample ReadGuestPlayerSample(uint32_t player_index) {
  std::lock_guard lock(g_state_mutex);
  const auto primary = std::find_if(g_state.player_objects.begin(), g_state.player_objects.end(),
      [player_index](const auto& player) { return player.player_index == static_cast<int32_t>(player_index); });
  const bool available = primary != g_state.player_objects.end() && !primary->ambiguous && primary->object;
  if (!available)
    return {g_state.loading, false, false, -1, g_state.mission_ready, 0, 0, 0, 0,
            g_state.player_pose_sampled_at};
  return {g_state.loading, true, primary->alive, static_cast<int32_t>(player_index), g_state.mission_ready,
          primary->x, primary->y, primary->z, primary->yaw_mrad, g_state.player_pose_sampled_at};
}

std::vector<ActorState> ReadAuthoritativeActors() {
  std::lock_guard lock(g_state_mutex);
  return g_state.authoritative_actors;
}

std::vector<ActorState> ReadHostMissionActors() {
#if defined(EDF2027_AUTHORITATIVE_HOST)
  std::lock_guard lock(g_state_mutex);
  return g_state.mission_actors;
#else
  return {};
#endif
}

MissionActorRemovalSnapshot ReadHostMissionActorRemovals() {
#if defined(EDF2027_AUTHORITATIVE_HOST)
  MissionActorRemovalSnapshot snapshot;
  {
    std::lock_guard lock(g_state_mutex);
    snapshot = g_state.mission_actor_removals;
  }
  const uint64_t current_generation =
      g_mission_actor_trace_generation.load(std::memory_order_relaxed);
  if (snapshot.host_trace_generation != current_generation) {
    snapshot = {};
    snapshot.host_trace_generation = current_generation;
  }
  return snapshot;
#else
  return {};
#endif
}

bool GuestMissionReady() {
  std::lock_guard lock(g_state_mutex);
  return g_state.mission_ready;
}

bool QueueAuthoritativeActorSpawn(const ActorSpawnRequest& spawn) {
  if (!spawn.id || spawn.archetype != ActorArchetype::kAnt ||
      spawn.x_cm < -1'000'000 || spawn.x_cm > 1'000'000 ||
      spawn.y_cm < -1'000'000 || spawn.y_cm > 1'000'000 ||
      spawn.z_cm < -1'000'000 || spawn.z_cm > 1'000'000 ||
      spawn.yaw_mrad < -3142 || spawn.yaw_mrad > 3142) return false;
  std::lock_guard lock(g_actor_spawn_mutex);
  if (!g_actor_world_ready || g_actor_spawn_reservations >= edf::net::kMaxSnapshotActors) return false;
  g_actor_spawn_queue.push_back({spawn, g_actor_mission_generation});
  ++g_actor_spawn_reservations;
  return true;
}

bool QueueAuthoritativeActorDamage(uint32_t actor_id, float damage_points) {
  const bool valid_damage = actor_id && std::isfinite(damage_points) &&
      damage_points > 0.0f && damage_points <= 65535.0f;
  uint64_t request_id = 0;
  uint64_t mission_generation = 0;
  const char* rejection_reason = nullptr;
  {
    std::lock_guard lock(g_actor_spawn_mutex);
    request_id = ++g_next_actor_damage_request_id;
    mission_generation = g_actor_mission_generation;
    if (!valid_damage) {
      rejection_reason = "invalid_damage_input";
    } else if (!g_actor_world_ready) {
      rejection_reason = "queue_world_not_ready";
    } else if (g_actor_damage_queue.size() >= kMaxPendingActorDamage) {
      rejection_reason = "queue_full";
    } else {
      g_actor_damage_queue.push_back(
          {actor_id, damage_points, mission_generation, request_id});
    }
  }
  if (actor_id & kMissionActorIdNamespace) {
#if defined(EDF2027_AUTHORITATIVE_HOST)
    if (rejection_reason) {
      LogMissionActorLifecycle("damage_dropped",
          g_mission_actor_trace_generation.load(std::memory_order_relaxed),
          mission_generation, actor_id, 0, 0, 0, 0, 0, rejection_reason,
          damage_points, request_id);
    } else {
      LogMissionActorLifecycle("damage_queued",
          g_mission_actor_trace_generation.load(std::memory_order_relaxed),
          mission_generation, actor_id, 0, 0, 0, 0, 0, "accepted",
          damage_points, request_id);
    }
#endif
  }
  return rejection_reason == nullptr;
}

}  // namespace edf::net::probe

REX_EXTERN(__imp__sub_820A1D08);
REX_HOOK_RAW(sub_820A1D08) {
  const uint32_t callsite = uint32_t(ctx.lr);
  if (g_trace_input_consumers.load(std::memory_order_relaxed) && callsite == 0x820A6D58) {
    g_title_action_handler_calls.fetch_add(1, std::memory_order_relaxed);
    g_title_action_handler_last_r31.store(ctx.r31.u32, std::memory_order_relaxed);
  } else if (g_trace_input_consumers.load(std::memory_order_relaxed) && callsite == 0x820A6DD0) {
    g_title_action_default_calls.fetch_add(1, std::memory_order_relaxed);
    g_title_action_default_last_r31.store(ctx.r31.u32, std::memory_order_relaxed);
  }
  __imp__sub_820A1D08(ctx, base);
}

REX_EXTERN(__imp__sub_820C5740);
REX_HOOK_RAW(sub_820C5740) {
  const uint32_t callsite = uint32_t(ctx.lr);
  __imp__sub_820C5740(ctx, base);
  if (g_trace_input_consumers.load(std::memory_order_relaxed) &&
      callsite == 0x820A6C34) {
    g_title_action_gate_a_function_calls.fetch_add(1, std::memory_order_relaxed);
    g_title_action_gate_a_function_callsite.store(callsite, std::memory_order_relaxed);
    g_title_action_gate_a_function_return.store(ctx.r3.u32, std::memory_order_relaxed);
  }
}

REX_EXTERN(__imp__sub_820C8710);
REX_HOOK_RAW(sub_820C8710) {
  const uint32_t callsite = uint32_t(ctx.lr);
  __imp__sub_820C8710(ctx, base);
  if (g_trace_input_consumers.load(std::memory_order_relaxed) &&
      (callsite == 0x820A6C50 || callsite == 0x820A6C68)) {
    g_title_action_gate_b_function_calls.fetch_add(1, std::memory_order_relaxed);
    g_title_action_gate_b_function_callsite.store(callsite, std::memory_order_relaxed);
    g_title_action_gate_b_function_return.store(ctx.r3.u32, std::memory_order_relaxed);
  }
}

REX_HOOK_RAW(sub_820CD448) {
  const uint32_t callsite = uint32_t(ctx.lr);
  const std::array<uint32_t, 6> args_before = {
      ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};
  edf::net::probe::RecordMissionSequenceCallback(base, 0x820CD448, 1, callsite,
                                                  args_before);
  g_mission_sequence_begins.fetch_add(1, std::memory_order_relaxed);
  if (REXCVAR_GET(edf_mp_actor_trace)) ResetMissionActorTrace();
  __imp__sub_820CD448(ctx, base);
  const std::array<uint32_t, 6> args_after = {
      ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};
  edf::net::probe::RecordMissionSequenceCallback(base, 0x820CD448, 2, callsite,
                                                  args_after);
  FlushMissionActorTrace();
}

REX_EXTERN(__imp__sub_820A6B10);
REX_HOOK_RAW(sub_820A6B10) {
  ObserveTitleActionGates(base);
  __imp__sub_820A6B10(ctx, base);
  // Observe after the title has copied the XPad action edges into its
  // per-slot dispatch records and performed its own event handling.
  ObserveTitleActionSlots(base);
}

REX_EXTERN(__imp__sub_820A5130);
REX_HOOK_RAW(sub_820A5130) {
  using edf::console::GuestMemory;
  constexpr uint32_t kMissionSequenceGlobal = 0x825786AC;
  const GuestMemory memory(base);
  const uint32_t previous = GuestMemory::Image(kMissionSequenceGlobal, 4)
                                ? memory.U32(kMissionSequenceGlobal)
                                : 0;
  edf::net::probe::RecordMissionSequenceLifecycle(base, 1, previous, uint32_t(ctx.lr));
  __imp__sub_820A5130(ctx, base);
  const uint32_t current = GuestMemory::Image(kMissionSequenceGlobal, 4)
                               ? memory.U32(kMissionSequenceGlobal)
                               : 0;
  edf::net::probe::RecordMissionSequenceLifecycle(base, 2, current, previous);
}

REX_EXTERN(__imp__sub_820CD298);
REX_HOOK_RAW(sub_820CD298) {
  const uint32_t object = ctx.r3.u32;
  edf::net::probe::RecordMissionSequenceLifecycle(base, 3, object, uint32_t(ctx.lr));
  __imp__sub_820CD298(ctx, base);
  edf::net::probe::RecordMissionSequenceLifecycle(base, 4, object, uint32_t(ctx.lr));
}

REX_EXTERN(__imp__sub_820D1F50);
REX_HOOK_RAW(sub_820D1F50) {
  const uint32_t object = ctx.r3.u32;
  edf::net::probe::RecordMissionSequenceLifecycle(base, 5, object, uint32_t(ctx.lr));
  __imp__sub_820D1F50(ctx, base);
  edf::net::probe::RecordMissionSequenceLifecycle(base, 6, object, uint32_t(ctx.lr));
}

REX_EXTERN(__imp__sub_820BDC68);
REX_HOOK_RAW(sub_820BDC68) {
  const uint32_t callsite = uint32_t(ctx.lr);
  const std::array<uint32_t, 6> args_before = {
      ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};
  edf::net::probe::RecordMissionSequenceCallback(base, 0x820BDC68, 1, callsite,
                                                  args_before);
  __imp__sub_820BDC68(ctx, base);
  const std::array<uint32_t, 6> args_after = {
      ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};
  edf::net::probe::RecordMissionSequenceCallback(base, 0x820BDC68, 2, callsite,
                                                  args_after);
}

REX_EXTERN(__imp__sub_820D1518);
REX_HOOK_RAW(sub_820D1518) {
  const uint32_t callsite = uint32_t(ctx.lr);
  const std::array<uint32_t, 6> args_before = {
      ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};
  edf::net::probe::RecordMissionSequenceCallback(base, 0x820D1518, 1, callsite,
                                                  args_before);
  __imp__sub_820D1518(ctx, base);
  const std::array<uint32_t, 6> args_after = {
      ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};
  edf::net::probe::RecordMissionSequenceCallback(base, 0x820D1518, 2, callsite,
                                                  args_after);
}

REX_EXTERN(__imp__sub_820CBC08);
REX_HOOK_RAW(sub_820CBC08) {
  const uint32_t callsite = uint32_t(ctx.lr);
  const std::array<uint32_t, 6> args_before = {
      ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};
  edf::net::probe::RecordMissionSequenceCallback(base, 0x820CBC08, 1, callsite,
                                                  args_before);
  __imp__sub_820CBC08(ctx, base);
  const std::array<uint32_t, 6> args_after = {
      ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};
  edf::net::probe::RecordMissionSequenceCallback(base, 0x820CBC08, 2, callsite,
                                                  args_after);
}

REX_EXTERN(__imp__sub_820C7220);
REX_EXTERN(__imp__sub_821DE3B0);
REX_HOOK_RAW(sub_821DE3B0) {
  if (!g_trace_input_consumers.load(std::memory_order_relaxed)) {
    __imp__sub_821DE3B0(ctx, base);
    return;
  }

  const uint32_t previous_context = g_active_decoder_context;
  const uint32_t previous_callsite = g_active_decoder_callsite;
  const uint32_t decoder_context = ctx.r3.u32;
  g_active_decoder_context = decoder_context;
  g_active_decoder_callsite = static_cast<uint32_t>(ctx.lr);
  const uint64_t sample_call_order = edf::net::probe::RecordDecoderOpcodeSample(
      base, static_cast<uint32_t>(ctx.lr), decoder_context);
  const uint64_t previous_sample_token = g_active_decoder_opcode_sample_token;
  g_active_decoder_opcode_sample_token = sample_call_order;
  __imp__sub_821DE3B0(ctx, base);
  edf::net::probe::RecordDecoderOpcodeResult(base, decoder_context, sample_call_order);
  g_active_decoder_opcode_sample_token = previous_sample_token;
  g_active_decoder_context = previous_context;
  g_active_decoder_callsite = previous_callsite;
}

REX_HOOK_RAW(sub_820C7220) {
  const uint32_t callsite = uint32_t(ctx.lr);
  const uint32_t phase = ctx.r6.u32;
  const uint32_t event = ctx.r7.u32 & 0x1Fu;
  if (g_trace_input_consumers.load(std::memory_order_relaxed) &&
      phase < g_title_ui_router_event_calls.size())
    g_title_ui_router_event_calls[phase][event].fetch_add(1, std::memory_order_relaxed);
  const std::array<uint32_t, 6> args_before = {
      ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};
  const bool trace_route_transition =
      g_trace_input_consumers.load(std::memory_order_relaxed) &&
      (event == 6 || event == 18 || event == 21 || event == 22 || event == 24);
  const bool correlate_confirm = trace_route_transition && phase == 0 && event == 6;
  const uint64_t previous_call_id = g_active_title_router_call_id;
  const uint32_t previous_compare_calls = g_active_title_router_compare_calls;
  const uint32_t previous_compare_result = g_active_title_router_compare_result;
  const uint32_t previous_compare_object = g_active_title_router_compare_object;
  const uint32_t previous_compare_value_20 = g_active_title_router_compare_value_20;
  const uint32_t previous_compare_value_32 = g_active_title_router_compare_value_32;
  const uint64_t active_call_id = correlate_confirm
      ? g_title_router_call_ids.fetch_add(1, std::memory_order_relaxed) + 1
      : 0;
  if (active_call_id && g_active_decoder_opcode_sample_token)
    edf::net::probe::AssociateDecoderOpcodeTitleRouterCall(
        g_active_decoder_opcode_sample_token, active_call_id);
  // Isolate each router invocation so nested route events cannot be mistaken
  // for comparisons made by their parent event.
  g_active_title_router_call_id = active_call_id;
  g_active_title_router_compare_calls = 0;
  g_active_title_router_compare_result = UINT32_MAX;
  g_active_title_router_compare_object = 0;
  g_active_title_router_compare_value_20 = 0;
  g_active_title_router_compare_value_32 = 0;
  edf::net::probe::TitleRouterTransitionSample route_before;
  if (trace_route_transition) {
    route_before = edf::net::probe::CaptureTitleRouterTransition(
        base, callsite, args_before,
        {ctx.r28.u32, ctx.r29.u32, ctx.r31.u32});
    if (g_active_decoder_context == ctx.r28.u32) {
      route_before.decoder_entry_callsite = g_active_decoder_callsite;
      route_before.callback_valid_mask |= 1u << 5;
    }
    route_before.call_id = active_call_id;
  }
  edf::net::probe::RecordMissionSequenceCallback(base, 0x820C7220, 1, callsite,
                                                  args_before);
  __imp__sub_820C7220(ctx, base);
  if (trace_route_transition) {
    if (correlate_confirm) {
      route_before.compare_calls = g_active_title_router_compare_calls;
      route_before.compare_result = g_active_title_router_compare_result;
      route_before.compare_object = g_active_title_router_compare_object;
      route_before.compare_value_20 = g_active_title_router_compare_value_20;
      route_before.compare_value_32 = g_active_title_router_compare_value_32;
    }
    edf::net::probe::RecordTitleRouterTransition(base, std::move(route_before));
  }
  g_active_title_router_call_id = previous_call_id;
  g_active_title_router_compare_calls = previous_compare_calls;
  g_active_title_router_compare_result = previous_compare_result;
  g_active_title_router_compare_object = previous_compare_object;
  g_active_title_router_compare_value_20 = previous_compare_value_20;
  g_active_title_router_compare_value_32 = previous_compare_value_32;
  edf::net::probe::RecordMissionSequenceCallback(base, 0x820C7220, 2, callsite,
                                                  args_before);
}

// The confirm comparison is sampled inline at sub_820C59A0 because optimized
// generated code calls the fast helper directly and bypasses this wrapper.
REX_EXTERN(__imp__sub_820C2640);
REX_HOOK_RAW(sub_820C2640) {
  __imp__sub_820C2640(ctx, base);
}

REX_EXTERN(__imp__sub_820C7070);
REX_HOOK_RAW(sub_820C7070) {
  const uint32_t callsite = uint32_t(ctx.lr);
  const std::array<uint32_t, 6> args_before = {
      ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};
  edf::net::probe::RecordMissionSequenceCallback(base, 0x820C7070, 1, callsite,
                                                  args_before);
  __imp__sub_820C7070(ctx, base);
  const std::array<uint32_t, 6> args_after = {
      ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};
  edf::net::probe::RecordMissionSequenceCallback(base, 0x820C7070, 2, callsite,
                                                  args_after);
}

// Observe the generated action-slot update before/after its native callback.
// The slot's +153 byte is the event consumed later by the title UI reader.
REX_EXTERN(__imp__sub_821CE3A0);
REX_HOOK_RAW(sub_821CE3A0) {
  if (!g_trace_input_consumers.load(std::memory_order_relaxed)) {
    __imp__sub_821CE3A0(ctx, base);
    return;
  }
  using edf::console::GuestMemory;
  const uint32_t object = ctx.r3.u32;
  const uint32_t callsite = uint32_t(ctx.lr);
  const GuestMemory memory(base);
  uint32_t slot = UINT32_MAX;
  uint32_t callback = 0;
  uint32_t event_before = 0;
  const bool object_valid = GuestMemory::Heap(object, 154) || GuestMemory::Image(object, 154);
  constexpr uint32_t kRegistryGlobal = 0x82578640;
  if (object_valid) {
    callback = memory.U32(object + 144);
    event_before = memory.U8(object + 153);
  }
  if (object_valid && GuestMemory::Image(kRegistryGlobal, 4)) {
    const uint32_t registry = memory.U32(kRegistryGlobal);
    if (GuestMemory::Heap(registry, 2356) || GuestMemory::Image(registry, 2356)) {
      const uint32_t count = (std::min)(memory.U32(registry + 2336), uint32_t{4});
      for (uint32_t index = 0; index < count; ++index) {
        const uint32_t object_index = memory.U32(registry + 2340 + index * 4);
        const uint64_t candidate = uint64_t(registry) + uint64_t(object_index) * 368;
        if (candidate == object) {
          slot = index;
          break;
        }
      }
    }
  }
  __imp__sub_821CE3A0(ctx, base);
  if (!object_valid) return;
  const uint32_t event = memory.U8(object + 153);
  g_input_event_update_calls.fetch_add(1, std::memory_order_relaxed);
  if (event != 0) g_input_event_update_nonzero_calls.fetch_add(1, std::memory_order_relaxed);
  g_input_event_update_last_object.store(object, std::memory_order_relaxed);
  g_input_event_update_last_callback.store(callback, std::memory_order_relaxed);
  g_input_event_update_last_event_before.store(event_before, std::memory_order_relaxed);
  g_input_event_update_last_event_after.store(event, std::memory_order_relaxed);
  g_input_event_update_last_callsite.store(callsite, std::memory_order_relaxed);
  if (slot < g_input_event_update_slot_calls.size()) {
    g_input_event_update_slot_calls[slot].fetch_add(1, std::memory_order_relaxed);
    g_input_event_update_slot_callbacks[slot].store(callback, std::memory_order_relaxed);
    g_input_event_update_slot_last_event[slot].store(event, std::memory_order_relaxed);
    if (event != 0 && event < 32)
      g_input_event_update_slot_nonzero_seen[slot].fetch_or(uint32_t{1} << event,
                                                            std::memory_order_relaxed);
  }
}

REX_EXTERN(__imp__sub_820A0268);
REX_HOOK_RAW(sub_820A0268) {
  const uint32_t callsite = uint32_t(ctx.lr);
  const uint32_t manager = ctx.r3.u32;
  const uint32_t slot = ctx.r4.u32;
  __imp__sub_820A0268(ctx, base);
  if (!g_trace_input_consumers.load(std::memory_order_relaxed)) return;

  const uint32_t object = ctx.r3.u32;
  using edf::console::GuestMemory;
  const GuestMemory memory(base);
  if (!GuestMemory::Heap(object, 154) && !GuestMemory::Image(object, 154)) return;

  uint32_t count = 0;
  constexpr uint32_t kRegistryGlobal = 0x82578640;
  if (GuestMemory::Image(kRegistryGlobal, 4)) {
    const uint32_t registry = memory.U32(kRegistryGlobal);
    if (GuestMemory::Heap(registry, 2356) || GuestMemory::Image(registry, 2356))
      count = memory.U32(registry + 2336);
  }
  const uint32_t event = memory.U8(object + 153);
  edf_native_probe_title_action_event(object, event, slot, count, callsite);
  if (callsite == 0x8218DBEC && slot < g_title_ui_action_slot_reads.size()) {
    g_title_ui_action_slot_reads[slot].fetch_add(1, std::memory_order_relaxed);
    if (event != 0 && event < 32)
      g_title_ui_action_slot_event_values[slot].fetch_or(uint32_t{1} << event,
                                                        std::memory_order_relaxed);
    const uint32_t input_flags = (memory.U8(object + 9) ? 0x01 : 0) |
        (memory.U8(object + 21) ? 0x02 : 0) |
        (memory.U8(object + 33) ? 0x04 : 0) |
        (memory.U8(object + 45) ? 0x08 : 0);
    g_title_ui_action_slot_input_flags[slot].fetch_or(input_flags, std::memory_order_relaxed);
    g_title_ui_action_slot_last_event[slot].store(event, std::memory_order_relaxed);
  }
  if (callsite == 0x820A6CFC) {
    g_title_action_slot_scan_calls.fetch_add(1, std::memory_order_relaxed);
    g_title_action_slot_scan_manager.store(manager, std::memory_order_relaxed);
    g_title_action_slot_scan_slot.store(slot, std::memory_order_relaxed);
    g_title_action_slot_scan_count.store(count, std::memory_order_relaxed);
    g_title_action_slot_scan_object.store(object, std::memory_order_relaxed);
    g_title_action_slot_scan_event.store(event, std::memory_order_relaxed);
  }
}

REX_EXTERN(__imp__sub_8218DF08);
REX_HOOK_RAW(sub_8218DF08) {
  if (!g_trace_input_consumers.load(std::memory_order_relaxed)) {
    __imp__sub_8218DF08(ctx, base);
    return;
  }

  constexpr uint32_t kTitleEventOwnerGlobal = 0x82578E80;
  const uint32_t receiver = ctx.r3.u32;
  const uint32_t call = static_cast<uint32_t>(
      g_title_ui_handoff_calls.fetch_add(1, std::memory_order_relaxed) + 1);
  uint32_t handoff_window = g_title_ui_handoff_window.load(std::memory_order_relaxed);
  while (handoff_window && !g_title_ui_handoff_window.compare_exchange_weak(
      handoff_window, handoff_window - 1, std::memory_order_relaxed)) {}
  const bool capture_input_window = handoff_window != 0;
  const uint32_t callsite = static_cast<uint32_t>(ctx.lr);
  const edf::console::GuestMemory memory(base);
  const bool receiver_valid = edf::console::GuestMemory::Heap(receiver, 117) ||
      edf::console::GuestMemory::Image(receiver, 117);
  const uint32_t vtable = receiver_valid ? memory.U32(receiver) : 0;
  const uint32_t dispatch_method =
      edf::console::GuestMemory::Image(vtable, 20) ? memory.U32(vtable + 16) : 0;
  const auto read_flags = [&](uint32_t object) -> uint8_t {
    if (!receiver_valid) return 0;
    return static_cast<uint8_t>((memory.U8(object + 104) ? 0x40 : 0) |
        (memory.U8(object + 105) ? 0x01 : 0) |
        (memory.U8(object + 106) ? 0x02 : 0) |
        (memory.U8(object + 107) ? 0x80 : 0) |
        (memory.U8(object + 108) ? 0x04 : 0) |
        (memory.U8(object + 116) ? 0x08 : 0) |
        (memory.U8(object + 119) ? 0x10 : 0) |
        (memory.U8(object + 120) ? 0x20 : 0));
  };
  const auto read_marker = [&](uint32_t& event_object) -> uint8_t {
    event_object = 0;
    if (!edf::console::GuestMemory::Image(kTitleEventOwnerGlobal, 4)) return 0;
    event_object = memory.U32(kTitleEventOwnerGlobal);
    if (!edf::console::GuestMemory::Heap(event_object, 133) &&
        !edf::console::GuestMemory::Image(event_object, 133)) return 0;
    return memory.U8(event_object + 132);
  };

  const uint8_t flags_before = read_flags(receiver);
  uint32_t event_object_before = 0;
  const uint8_t marker_before = read_marker(event_object_before);
  __imp__sub_8218DF08(ctx, base);
  const uint32_t r3_after = ctx.r3.u32;
  const uint8_t flags_after = read_flags(receiver);
  uint32_t event_object_after = 0;
  const uint8_t marker_after = read_marker(event_object_after);
  if (!receiver_valid) return;

  if (call <= 8 || capture_input_window || flags_before != flags_after ||
      marker_before != marker_after || event_object_before != event_object_after) {
    std::lock_guard lock(g_state_mutex);
    constexpr std::size_t kTitleUiHandoffTraceLimit = 320;
    if (g_state.title_ui_handoffs.size() >= kTitleUiHandoffTraceLimit)
      g_state.title_ui_handoffs.erase(g_state.title_ui_handoffs.begin());
    edf::net::probe::TitleUiHandoffSample sample;
    sample.call = call;
    sample.callsite = callsite;
    sample.receiver = receiver;
    sample.vtable = vtable;
    sample.dispatch_method = dispatch_method;
    sample.r3_after = r3_after;
    sample.class_name = memory.ClassName(receiver);
    sample.receiver_words_valid = edf::console::GuestMemory::Heap(receiver, 128) ||
        edf::console::GuestMemory::Image(receiver, 128);
    if (sample.receiver_words_valid)
      for (std::size_t word = 0; word < sample.receiver_words.size(); ++word)
        sample.receiver_words[word] = memory.U32(receiver + uint32_t(word * 4));
    sample.event_object_before = event_object_before;
    sample.event_object_after = event_object_after;
    sample.flags_before = flags_before;
    sample.flags_after = flags_after;
    sample.marker_before = marker_before;
    sample.marker_after = marker_after;
    sample.script_elapsed_ms = g_input_script_elapsed_ms.load(std::memory_order_relaxed);
    sample.script_buttons = g_input_script_buttons.load(std::memory_order_relaxed);
    g_state.title_ui_handoffs.push_back(std::move(sample));
  }
}

// The scene's vtable +16 update method. It consumes r3 only; r4-r8 below are
// captured as ambient registers for context, not as event or input arguments.
REX_EXTERN(__imp__sub_82178CD8);
REX_HOOK_RAW(sub_82178CD8) {
  if (!g_trace_input_consumers.load(std::memory_order_relaxed)) {
    __imp__sub_82178CD8(ctx, base);
    return;
  }

  constexpr uint32_t kTitleEventOwnerGlobal = 0x82578E80;
  const uint32_t receiver = ctx.r3.u32;
  const uint64_t call = g_title_scene_update_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t update_window = g_title_scene_update_window.load(std::memory_order_relaxed);
  while (update_window && !g_title_scene_update_window.compare_exchange_weak(
      update_window, update_window - 1, std::memory_order_relaxed)) {}
  const bool capture_input_window = update_window != 0;
  const uint32_t callsite = static_cast<uint32_t>(ctx.lr);
  const edf::console::GuestMemory memory(base);
  const bool receiver_valid = edf::console::GuestMemory::Heap(receiver, 121) ||
      edf::console::GuestMemory::Image(receiver, 121);
  const uint32_t vtable = receiver_valid ? memory.U32(receiver) : 0;
  const auto read_flags = [&](uint32_t object) -> uint8_t {
    if (!receiver_valid) return 0;
    return static_cast<uint8_t>((memory.U8(object + 104) ? 0x40 : 0) |
        (memory.U8(object + 105) ? 0x01 : 0) |
        (memory.U8(object + 106) ? 0x02 : 0) |
        (memory.U8(object + 107) ? 0x80 : 0) |
        (memory.U8(object + 108) ? 0x04 : 0) |
        (memory.U8(object + 116) ? 0x08 : 0) |
        (memory.U8(object + 119) ? 0x10 : 0) |
        (memory.U8(object + 120) ? 0x20 : 0));
  };
  const auto read_marker = [&](uint32_t& event_object) -> uint8_t {
    event_object = 0;
    if (!edf::console::GuestMemory::Image(kTitleEventOwnerGlobal, 4)) return 0;
    event_object = memory.U32(kTitleEventOwnerGlobal);
    if (!edf::console::GuestMemory::Heap(event_object, 133) &&
        !edf::console::GuestMemory::Image(event_object, 133)) return 0;
    return memory.U8(event_object + 132);
  };

  const uint8_t flags_before = read_flags(receiver);
  uint32_t event_object_before = 0;
  const uint8_t marker_before = read_marker(event_object_before);
  const uint32_t ambient_registers[] = {
      ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};
  const uint32_t script_buttons = g_input_script_buttons.load(std::memory_order_relaxed);
  const uint32_t script_elapsed_ms = g_input_script_elapsed_ms.load(std::memory_order_relaxed);
  __imp__sub_82178CD8(ctx, base);
  const uint32_t result = ctx.r3.u32;
  const uint8_t flags_after = read_flags(receiver);
  uint32_t event_object_after = 0;
  const uint8_t marker_after = read_marker(event_object_after);
  if (!receiver_valid) return;

  if (call <= 8 || flags_before != flags_after || marker_before != marker_after ||
      event_object_before != event_object_after || capture_input_window) {
    edf::net::probe::TitleSceneUpdateSample sample;
    sample.call = static_cast<uint32_t>(call);
    sample.receiver = receiver;
    sample.vtable = vtable;
    sample.class_name = memory.ClassName(receiver);
    std::copy(ambient_registers, ambient_registers + 5, sample.ambient_registers);
    sample.result = result;
    sample.script_elapsed_ms = script_elapsed_ms;
    sample.script_buttons = static_cast<uint16_t>(script_buttons);
    sample.flags_before = flags_before;
    sample.flags_after = flags_after;
    sample.marker_before = marker_before;
    sample.marker_after = marker_after;
    std::lock_guard lock(g_state_mutex);
    constexpr std::size_t kTitleSceneUpdateTraceLimit = 320;
    if (g_state.title_scene_updates.size() >= kTitleSceneUpdateTraceLimit)
      g_state.title_scene_updates.erase(g_state.title_scene_updates.begin());
    sample.callsite = callsite;
    sample.receiver_words_valid = edf::console::GuestMemory::Heap(receiver, 128) ||
        edf::console::GuestMemory::Image(receiver, 128);
    if (sample.receiver_words_valid)
      for (std::size_t word = 0; word < sample.receiver_words.size(); ++word)
        sample.receiver_words[word] = memory.U32(receiver + uint32_t(word * 4));
    g_state.title_scene_updates.push_back(std::move(sample));
  }
}

// This callback can call sub_820CD448 to start the sequence, then set the UI
// object's +60 latch. Keep its observations distinct from the global begin count.
REX_HOOK_RAW(sub_820C59F0) {
  if (!g_trace_input_consumers.load(std::memory_order_relaxed)) {
    __imp__sub_820C59F0(ctx, base);
    return;
  }

  using edf::console::GuestMemory;
  constexpr uint32_t kMissionSequenceGlobal = 0x825786AC;
  const GuestMemory memory(base);
  const uint32_t callsite = uint32_t(ctx.lr);
  const uint32_t receiver = ctx.r3.u32;
  const bool receiver_valid = GuestMemory::Heap(receiver, 260) || GuestMemory::Image(receiver, 260);
  const uint32_t request_object = receiver_valid ? memory.U32(receiver + 256) : 0;
  const bool request_valid = GuestMemory::Heap(request_object, 76) || GuestMemory::Image(request_object, 76);
  const uint32_t sequence_object = GuestMemory::Image(kMissionSequenceGlobal, 4)
      ? memory.U32(kMissionSequenceGlobal) : 0;
  const bool sequence_valid = GuestMemory::Heap(sequence_object, 716) ||
      GuestMemory::Image(sequence_object, 716);
  const uint32_t request_state_72 = request_valid ? memory.U32(request_object + 72) : 0;
  const uint32_t request_state_60 = request_valid ? memory.U8(request_object + 60) : 0;
  const uint32_t sequence_state_696 = sequence_valid ? memory.U8(sequence_object + 696) : 0;
  const uint32_t sequence_state_708 = sequence_valid ? memory.U32(sequence_object + 708) : 0;
  const uint32_t sequence_state_712 = sequence_valid ? memory.U32(sequence_object + 712) : 0;

  __imp__sub_820C59F0(ctx, base);

  const uint32_t request_state_60_after = request_valid ? memory.U8(request_object + 60) : 0;
  const uint32_t sequence_state_696_after = sequence_valid ? memory.U8(sequence_object + 696) : 0;
  const uint32_t sequence_state_708_after = sequence_valid ? memory.U32(sequence_object + 708) : 0;
  const uint32_t sequence_state_712_after = sequence_valid ? memory.U32(sequence_object + 712) : 0;
  g_title_sequence_request_calls.fetch_add(1, std::memory_order_relaxed);
  g_title_sequence_request_callsite.store(callsite, std::memory_order_relaxed);
  g_title_sequence_request_receiver.store(receiver, std::memory_order_relaxed);
  g_title_sequence_request_object.store(request_object, std::memory_order_relaxed);
  g_title_sequence_request_state_72_before.store(request_state_72, std::memory_order_relaxed);
  g_title_sequence_request_state_60_before.store(request_state_60, std::memory_order_relaxed);
  g_title_sequence_request_state_60_after.store(request_state_60_after, std::memory_order_relaxed);
  g_title_sequence_request_sequence_object.store(sequence_object, std::memory_order_relaxed);
  g_title_sequence_request_sequence_696_before.store(sequence_state_696, std::memory_order_relaxed);
  g_title_sequence_request_sequence_696_after.store(sequence_state_696_after, std::memory_order_relaxed);
  g_title_sequence_request_sequence_708_before.store(sequence_state_708, std::memory_order_relaxed);
  g_title_sequence_request_sequence_708_after.store(sequence_state_708_after, std::memory_order_relaxed);
  g_title_sequence_request_sequence_712_before.store(sequence_state_712, std::memory_order_relaxed);
  g_title_sequence_request_sequence_712_after.store(sequence_state_712_after, std::memory_order_relaxed);
}

REX_HOOK_RAW(sub_820C5A70) {
  if (!g_trace_input_consumers.load(std::memory_order_relaxed)) {
    __imp__sub_820C5A70(ctx, base);
    return;
  }
  const uint32_t callsite = uint32_t(ctx.lr);
  const std::array<uint32_t, 6> args_before = {
      ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};
  edf::net::probe::RecordMissionSequenceCallback(base, 0x820C5A70, 1, callsite,
                                                  args_before);
  __imp__sub_820C5A70(ctx, base);
  const std::array<uint32_t, 6> args_after = {
      ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};
  edf::net::probe::RecordMissionSequenceCallback(base, 0x820C5A70, 2, callsite,
                                                  args_after);
}

REX_HOOK_RAW(sub_820C5B28) {
  if (!g_trace_input_consumers.load(std::memory_order_relaxed)) {
    __imp__sub_820C5B28(ctx, base);
    return;
  }
  const uint32_t callsite = uint32_t(ctx.lr);
  const std::array<uint32_t, 6> args_before = {
      ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};
  edf::net::probe::RecordMissionSequenceCallback(base, 0x820C5B28, 1, callsite,
                                                  args_before);
  __imp__sub_820C5B28(ctx, base);
  const std::array<uint32_t, 6> args_after = {
      ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};
  edf::net::probe::RecordMissionSequenceCallback(base, 0x820C5B28, 2, callsite,
                                                  args_after);
}

REX_HOOK_RAW(sub_820CEBD0) {
  if (!REXCVAR_GET(edf_mp_actor_trace) ||
      g_mission_ant_spawn_samples.load(std::memory_order_relaxed) >= kMissionActorTraceAntLimit) {
    __imp__sub_820CEBD0(ctx, base);
    return;
  }
  const uint32_t call_ordinal = g_mission_actor_spawn_calls.fetch_add(1, std::memory_order_relaxed);
  const uint32_t caller = uint32_t(ctx.lr);
  const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32, r6 = ctx.r6.u32, r7 = ctx.r7.u32;
  const uint64_t f1_bits = ctx.f1.u64;
  __imp__sub_820CEBD0(ctx, base);
  if (call_ordinal >= kMissionActorTraceCallLimit) return;

  const uint32_t object = ctx.r3.u32;
  if (!edf::console::GuestMemory::Heap(object, 738)) return;
  const edf::console::GuestMemory memory(base);
  if (memory.ClassName(object) != "clGiantAnt") return;
  const uint32_t faction = memory.U32(object + 604);
  if (faction != 1) return;
  const uint32_t ant_ordinal = g_mission_ant_spawn_samples.fetch_add(1, std::memory_order_relaxed);
  if (ant_ordinal >= kMissionActorTraceAntLimit) return;

  const float health = memory.F32(object + 592);
  const float x = memory.F32(object + 272), y = memory.F32(object + 276), z = memory.F32(object + 280);
  const float forward_x = memory.F32(object + 256), forward_z = memory.F32(object + 264);
  if (!std::isfinite(health) || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
      !std::isfinite(forward_x) || !std::isfinite(forward_z)) return;
  const bool yaw_valid = std::abs(forward_x) >= 1e-6f || std::abs(forward_z) >= 1e-6f;
  const float yaw = yaw_valid ? std::atan2(forward_x, forward_z) : 0.0f;
  const uint8_t awaken_latch_after = memory.U8(object + 737);
  const uint64_t generation = g_mission_actor_trace_generation.load(std::memory_order_relaxed);
  const uint64_t mission_generation = ActorMissionGeneration();
  const uint32_t vtable = memory.U32(object);
  g_mission_actor_trace_objects.insert_or_assign(object,
      MissionAntTraceObject{generation, call_ordinal, ant_ordinal, vtable});
  QueueMissionAntSpawnTrace({generation, mission_generation,
      call_ordinal, ant_ordinal, caller, r3, r4, r5, r6, r7, f1_bits,
      object, vtable, faction, awaken_latch_after, health, x, y, z, yaw, yaw_valid});
}

REX_HOOK_RAW(sub_821C0ED8) {
  // Generated removal call sites route through this scenegraph path. Invalidate
  // both identities before the original routine can release/recycle the object.
  const uint32_t object = ctx.r3.u32;
  const auto traced = g_mission_actor_trace_objects.find(object);
  const uint64_t current_trace_generation =
      g_mission_actor_trace_generation.load(std::memory_order_relaxed);
  const bool has_current_trace = traced != g_mission_actor_trace_objects.end() &&
      traced->second.generation == current_trace_generation;
  const uint32_t call_ordinal = has_current_trace ? traced->second.call_ordinal : 0;
  const uint32_t ant_ordinal = has_current_trace ? traced->second.ant_ordinal : 0;
  const uint32_t current_vtable = edf::console::GuestMemory::Heap(object, 4)
      ? edf::console::GuestMemory(base).U32(object) : 0;
  uint32_t lifecycle_actor_id = 0;
  uint64_t lifecycle_mission_generation = 0;
#if defined(EDF2027_AUTHORITATIVE_HOST)
  MissionActorTrack invalidated_mission_actor{};
  bool had_mission_actor = false;
  const auto mission_actor = g_mission_actor_ids.find(object);
  if (mission_actor != g_mission_actor_ids.end()) {
    invalidated_mission_actor = mission_actor->second;
    had_mission_actor = true;
    lifecycle_actor_id = mission_actor->second.id;
    lifecycle_mission_generation = mission_actor->second.mission_generation;
  }
#endif
  if (traced != g_mission_actor_trace_objects.end()) {
    const MissionAntTraceObject trace = traced->second;
    g_mission_actor_trace_objects.erase(traced);
#if defined(EDF2027_AUTHORITATIVE_HOST)
    if (had_mission_actor && has_current_trace &&
        (lifecycle_actor_id & kMissionActorIdNamespace) != 0 &&
        trace.vtable == current_vtable &&
        edf::console::GuestMemory::Heap(object, 738)) {
      const edf::console::GuestMemory memory(base);
      if (memory.ClassName(object) == "clGiantAnt") {
        std::lock_guard lock(g_state_mutex);
        auto& removals = g_state.mission_actor_removals;
        if (removals.host_trace_generation != trace.generation) {
          removals = {};
          removals.host_trace_generation = trace.generation;
        }
        const auto duplicate = std::find_if(removals.removals.begin(), removals.removals.end(),
            [&](const auto& prior) {
              return prior.id == lifecycle_actor_id ||
                  (prior.call_ordinal == trace.call_ordinal &&
                   prior.ant_ordinal == trace.ant_ordinal);
            });
        if (duplicate == removals.removals.end() &&
            removals.removals.size() < edf::net::kMaxMissionActorRemovalHistory) {
          removals.removals.push_back(
              {lifecycle_actor_id, trace.call_ordinal, trace.ant_ordinal});
          removals.revision = static_cast<uint32_t>(removals.removals.size());
        }
      }
    }
#endif
    if (REXCVAR_GET(edf_mp_actor_trace) &&
        trace.generation == g_mission_actor_trace_generation.load(std::memory_order_relaxed) &&
        edf::console::GuestMemory::Heap(object, 738)) {
      const edf::console::GuestMemory memory(base);
      if (memory.U32(object) == trace.vtable && memory.ClassName(object) == "clGiantAnt") {
        const uint32_t removal_ordinal = g_mission_actor_trace_removals.fetch_add(1, std::memory_order_relaxed);
        if (ReserveMissionActorTraceLog(trace.generation)) {
          REXLOG_INFO(
              "Mission ant lifecycle trace: side={} event=removal_hook_observed trace_generation={} mission_generation={} actor_id={} removal_ordinal={} call_ordinal={} ant_ordinal={} caller={:#x} object={:#x} expected_vtable={:#x} current_vtable={:#x} class=clGiantAnt faction={} health={} x={} y={} z={}",
              kMissionActorTraceSide, trace.generation, lifecycle_mission_generation,
              lifecycle_actor_id, removal_ordinal,
              trace.call_ordinal, trace.ant_ordinal, uint32_t(ctx.lr), object,
              trace.vtable, memory.U32(object), memory.U32(object + 604),
              memory.F32(object + 592), memory.F32(object + 272),
              memory.F32(object + 276), memory.F32(object + 280));
        }
      }
    }
  }
  ForgetAuthoritativeActor(object);
  ForgetMissionActor(object);
#if defined(EDF2027_AUTHORITATIVE_HOST)
  if (had_mission_actor) {
    LogMissionActorLifecycle(has_current_trace ? "removal_hook_invalidated" : "removal_hook_invalidated_untraced",
        current_trace_generation,
        invalidated_mission_actor.mission_generation, invalidated_mission_actor.id,
        object, invalidated_mission_actor.vtable, current_vtable, call_ordinal,
        ant_ordinal, "scenegraph_removal_hook");
  }
#endif
  __imp__sub_821C0ED8(ctx, base);
}

REX_EXTERN(__imp__sub_821A6508);
REX_HOOK_RAW(sub_821A6508) {
  g_scheduler_calls.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_821A6508(ctx, base);
}

REX_EXTERN(__imp__sub_82132ED0);
REX_HOOK_RAW(sub_82132ED0) {
  if (g_trace_input_consumers.load(std::memory_order_relaxed)) {
    const uint64_t call = g_guest_wait_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (call <= 32 || ctx.r3.u32 >= 0xF0000000) {
      edf::net::probe::GuestWaitSample sample;
      sample.call = static_cast<uint32_t>(call);
      sample.callsite = static_cast<uint32_t>(ctx.lr);
      sample.handle = ctx.r3.u32;
      sample.alertable = ctx.r4.u32;
      sample.wait_mode = ctx.r5.u32;
      sample.timeout = ctx.r6.u32;
      sample.stack = ctx.r1.u32;
      sample.r31 = ctx.r31.u32;
      sample.simulation_ticks = static_cast<uint32_t>(
          edf::SimulationTicks().load(std::memory_order_relaxed));
      std::lock_guard lock(g_state_mutex);
      constexpr std::size_t kGuestWaitTraceLimit = 64;
      if (g_state.guest_wait_samples.size() >= kGuestWaitTraceLimit)
        g_state.guest_wait_samples.erase(g_state.guest_wait_samples.begin());
      g_state.guest_wait_samples.push_back(sample);
    }
  }
  __imp__sub_82132ED0(ctx, base);
}

// The headless probe/host intentionally has no graphics queue. Retaining the
// guest resource-fence wait therefore spins forever on work no renderer can
// submit or complete. In this no-GPU runtime, resolve that wait immediately.
REX_EXTERN(__imp__sub_8213C928);
REX_HOOK_RAW(sub_8213C928) {
  (void)ctx;
  (void)base;
  g_headless_fence_waits.fetch_add(1, std::memory_order_relaxed);
}

// The two allocator waits below poll GPU ring/generation state. With tool_mode
// enabled no renderer submits or retires those allocations, so the retained
// polling loop can never complete. Treat those renderer-only waits as complete
// in the dedicated headless runtime.
REX_EXTERN(__imp__sub_8213BC48);
REX_HOOK_RAW(sub_8213BC48) {
  (void)ctx;
  (void)base;
  g_headless_allocator_waits.fetch_add(1, std::memory_order_relaxed);
}

REX_EXTERN(__imp__sub_8213BCE0);
REX_HOOK_RAW(sub_8213BCE0) {
  (void)ctx;
  (void)base;
  g_headless_allocator_waits.fetch_add(1, std::memory_order_relaxed);
}

// Native desktop rendering replaces the retail vblank-backed ticker with a
// host-clock heartbeat. A dedicated headless host has no renderer to install
// that hook, so use the same fixed 60 Hz clock here and let simulation advance
// without waiting on the title's never-changing retail tick globals.
REX_EXTERN(__imp__sub_821BEBF0);
REX_HOOK_RAW(sub_821BEBF0) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t divisor = ctx.r4.u32;
  (void)edf::native::NativePacingSteps(0, 0, divisor);
  std::lock_guard lock(g_headless_pacing.mutex);
  REX_STORE_U32(object, 0x82019918);
  REX_STORE_U32(object + 4, divisor);
  REX_STORE_U64(object + 16, 1);
  g_headless_pacing.clock.Reset(edf::native::NativePacingClock::Clock::now(),
                                REX_LOAD_U64(0x8257C300));
  g_headless_pacing.initialized = true;
}

REX_EXTERN(__imp__sub_821BEAB0);
REX_EXTERN(sub_821FAC28);
REX_HOOK_RAW(sub_821BEAB0) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t divisor = REX_LOAD_U32(object + 4);
  (void)edf::native::NativePacingSteps(0, 0, divisor);
  std::lock_guard lock(g_headless_pacing.mutex);
  auto sampled_at = edf::native::NativePacingClock::Clock::now();
  const uint64_t previous = REX_LOAD_U64(0x8257C308);
  if (!g_headless_pacing.initialized) {
    g_headless_pacing.clock.Reset(sampled_at, previous);
    g_headless_pacing.initialized = true;
  }
  uint64_t current = g_headless_pacing.clock.Sample(sampled_at);
  uint64_t steps = edf::native::NativePacingSteps(current, previous, divisor);
  while (edf::native::NativePacingPending(steps)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    sampled_at = edf::native::NativePacingClock::Clock::now();
    current = g_headless_pacing.clock.Sample(sampled_at);
    steps = edf::native::NativePacingSteps(current, previous, divisor);
  }
  REX_STORE_U64(0x8257C300, current);
  REX_STORE_U64(0x8257C308, current);
  ctx.r3.u64 = object + 8;
  sub_821FAC28(ctx, base);
  ctx.r3.u64 = edf::native::NativePacingResult(steps);
}

REX_EXTERN(__imp__sub_821BEB38);
REX_HOOK_RAW(sub_821BEB38) {
  {
    std::lock_guard lock(g_headless_pacing.mutex);
    if (g_headless_pacing.initialized) {
      REX_STORE_U64(0x8257C300,
                    g_headless_pacing.clock.Sample(edf::native::NativePacingClock::Clock::now()));
    }
  }
  __imp__sub_821BEB38(ctx, base);
}

REX_EXTERN(__imp__sub_821A4BA0);
REX_HOOK_RAW(sub_821A4BA0) {
  const uint32_t dispatched_steps = ctx.r4.u32;
  __imp__sub_821A4BA0(ctx, base);
  FlushMissionActorTrace();
  const uint64_t dispatches = g_dispatches.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint64_t steps = g_steps.fetch_add(dispatched_steps, std::memory_order_relaxed) + dispatched_steps;
  edf::SimulationTicks().fetch_add(dispatched_steps, std::memory_order_relaxed);
  if (!g_mission_probe_enabled || dispatches % kMissionSampleInterval != 0) {
    std::lock_guard lock(g_state_mutex);
    g_state.dispatches = dispatches;
    g_state.steps = steps;
    return;
  }

  WorldSample world = SampleWorld(base);
#if defined(EDF2027_AUTHORITATIVE_HOST)
  RunPlayerSpawnProbe(ctx, base, world);
  if (g_player_spawn_probe_attempted.load(std::memory_order_relaxed)) world = SampleWorld(base);
#endif
  UpdateActorSpawnWorldReadiness(!world.loading && world.primary_player_available && world.primary_player_alive);
  DrainAuthoritativeActorSpawns(ctx, base, world);
  DrainAuthoritativeActorDamage(ctx, base, world);
  const auto network = edf::net::probe::ReadNetworkInput();
  std::array<edf::net::probe::NetworkPositionReset, edf::net::probe::kNetworkGuestSeatCount> confirmed_resets{};
  std::array<bool, edf::net::probe::kNetworkGuestSeatCount> has_confirmed_reset{};
  if (!world.loading) {
    const edf::console::GuestMemory memory(base);
    for (const auto& seat : network.seats) {
      if (!seat.player_id || !seat.position_reset || seat.position_reset->guest_acknowledged ||
          seat.position_reset->player_id != seat.player_id ||
          seat.player_index >= edf::net::probe::kNetworkGuestSeatCount) continue;
      const auto player = std::find_if(world.player_objects.begin(), world.player_objects.end(),
          [&](const auto& sample) { return sample.player_index == static_cast<int32_t>(seat.player_index); });
      if (player == world.player_objects.end() || player->ambiguous || !player->object || !player->alive) continue;
      const std::size_t index = seat.player_index;
      edf::net::probe::WithPendingNetworkPositionReset(
          seat.player_id, seat.position_reset->reset_id,
          [&](edf::net::probe::NetworkPositionReset& reset) {
            if (!reset.guest_applied) {
              reset.guest_applied = ApplyPositionReset(memory, player->object, reset, index == 0);
            } else if (PositionResetMatches(memory, player->object, reset, index == 0)) {
              // The readback follows a later completed guest dispatch; each seat
              // retains its own reset until guest and network ACKs agree.
              confirmed_resets[index] = reset;
              has_confirmed_reset[index] = true;
            } else {
              ApplyPositionReset(memory, player->object, reset, index == 0);
            }
          });
    }
  }
  const bool mission_ready = !world.loading && world.primary_player_available &&
      world.primary_player_alive && world.hostile_actors > 0;
  if (mission_ready) edf::net::probe::MarkNetworkMissionReady();
  bool hostile_motion = false;
  std::array<ActorTrack, kTrackedHostileSlots> next_hostile_tracks{};
  if (mission_ready) {
    for (std::size_t i = 0; i < world.tracked_hostile_count; ++i) {
      const ActorPosition& actor = world.hostiles[i];
      ActorTrack track{actor.object, actor.vtable, 1, actor.position};
      for (const ActorTrack& previous : g_hostile_tracks) {
        if (previous.object == actor.object && previous.vtable == actor.vtable) {
          track = previous;
          ++track.ready_samples;
          break;
        }
      }
      // Require a stable object and vtable across eight ready dispatch samples,
      // then require at least one world unit of displacement from its first sample.
      const float dx = actor.position.x - track.origin.x;
      const float dy = actor.position.y - track.origin.y;
      const float dz = actor.position.z - track.origin.z;
      if (track.ready_samples >= 8 && dx * dx + dy * dy + dz * dz > 1.0f) hostile_motion = true;
      next_hostile_tracks[i] = track;
    }
  }
  g_hostile_tracks = next_hostile_tracks;
  const uint32_t selected_user = g_input_user.load(std::memory_order_relaxed);
  const auto selected_player = std::find_if(world.player_objects.begin(), world.player_objects.end(),
      [selected_user](const auto& player) { return player.player_index == static_cast<int32_t>(selected_user); });
  const bool selected_player_present = selected_player != world.player_objects.end();
  const bool selected_player_ambiguous = selected_player_present && selected_player->ambiguous;
  const bool selected_player_alive = selected_player_present && !selected_player_ambiguous &&
      selected_player->object && selected_player->alive;
  if (mission_ready && selected_player_alive &&
      Finite({selected_player->x, selected_player->y, selected_player->z})) {
    if (g_input_user_player_track.object != selected_player->object) {
      g_input_user_player_track = {selected_player->object, 1,
                                   {selected_player->x, selected_player->y, selected_player->z}};
    } else if (g_input_user_player_track.ready_samples < UINT32_MAX) {
      ++g_input_user_player_track.ready_samples;
    }
    const float dx = selected_player->x - g_input_user_player_track.origin.x;
    const float dy = selected_player->y - g_input_user_player_track.origin.y;
    const float dz = selected_player->z - g_input_user_player_track.origin.z;
    if (g_input_user_player_track.ready_samples >= 8 && dx * dx + dy * dy + dz * dz >= 1.0f)
      g_input_user_player_motion_seen = true;
  } else {
    g_input_user_player_track = {};
  }
  g_last_sampled_mission_ready.store(mission_ready, std::memory_order_relaxed);

  std::lock_guard lock(g_state_mutex);
  g_state.hostile_motion_seen = g_state.hostile_motion_seen || (mission_ready && hostile_motion);
  g_state.input_user = selected_user;
  g_state.input_user_player_present = selected_player_present;
  g_state.input_user_player_ambiguous = selected_player_ambiguous;
  g_state.input_user_player_alive = selected_player_alive;
  g_state.input_user_player_ready_samples = g_input_user_player_track.ready_samples;
  g_state.input_user_player_motion_seen = g_input_user_player_motion_seen;
  g_state.dispatches = dispatches;
  g_state.steps = steps;
  g_state.loading = world.loading;
  g_state.has_player = world.has_player;
  g_state.player_alive = world.player_alive;
  g_state.player_index = world.primary_player_available ? 0 : -1;
  g_state.hostile_actors = world.hostile_actors;
  g_state.mission_ready = mission_ready;
  if (mission_ready && !g_state.mission_ready_seen) {
    g_state.mission_ready_seen = true;
    g_state.mission_ready_dispatch = dispatches;
  }
  g_state.player_x = world.player_position.x;
  g_state.player_y = world.player_position.y;
  g_state.player_z = world.player_position.z;
  g_state.player_yaw_mrad = world.player_yaw_mrad;
  g_state.player_pose_sampled_at = std::chrono::steady_clock::now();
  std::sort(world.player_objects.begin(), world.player_objects.end(),
            [](const auto& a, const auto& b) { return a.player_index < b.player_index; });
  g_state.player_objects = std::move(world.player_objects);
  g_state.authoritative_actors = std::move(world.authoritative_actors);
  g_state.mission_actors = std::move(world.mission_actors);
  for (std::size_t index = 0; index < has_confirmed_reset.size(); ++index)
    if (has_confirmed_reset[index])
      edf::net::probe::AcknowledgeNetworkPositionReset(
          confirmed_resets[index].player_id, confirmed_resets[index].reset_id);
}

REX_EXTERN(__imp__sub_8212EA20);
REX_HOOK_RAW(sub_8212EA20) {
  const uint32_t user = ctx.r3.u32, ptr = ctx.r4.u32, callsite = ctx.lr;
  const bool last_sampled_mission_ready = g_last_sampled_mission_ready.load(std::memory_order_relaxed);
  if (user < g_xinput_user_wrapper_calls.size()) {
    const size_t index = static_cast<size_t>(user);
    g_xinput_user_wrapper_calls[index].fetch_add(1, std::memory_order_relaxed);
    if (last_sampled_mission_ready)
      g_xinput_user_calls_last_sampled_ready[index].fetch_add(1, std::memory_order_relaxed);
  } else {
    g_other_xinput_wrapper_calls.fetch_add(1, std::memory_order_relaxed);
    if (last_sampled_mission_ready)
      g_other_xinput_calls_last_sampled_ready.fetch_add(1, std::memory_order_relaxed);
  }
  const uint32_t scripted_user = g_input_user.load(std::memory_order_relaxed);
  if (SafeInputStatePointer(ptr, ctx.r1.u32) && (user < edf::net::probe::kNetworkGuestSeatCount || user == scripted_user)) {
    // Preserve the title's XamInputGetState path before replacing its pad
    // sample. The desktop hook routes input after this call; doing the same
    // keeps the runtime's assigned-device refresh and active-device tracking
    // in place while the probe supplies its synthetic controller state.
    __imp__sub_8212EA20(ctx, base);
    auto* state = reinterpret_cast<rex::input::X_INPUT_STATE*>(base + ptr);
    const auto network = edf::net::probe::ReadNetworkInput();
    if (network.enabled && user < network.seats.size() && network.seats[user].player_id) {
      const auto probe = edf::net::probe::ReadGuestProbeState();
      const auto player_seat = std::find_if(probe.player_objects.begin(), probe.player_objects.end(),
          [user](const auto& player) { return player.player_index == static_cast<int32_t>(user); });
      const auto now = std::chrono::steady_clock::now();
      const bool player_ready = !probe.loading && player_seat != probe.player_objects.end() &&
          !player_seat->ambiguous && player_seat->object && player_seat->alive &&
          probe.player_pose_sampled_at != std::chrono::steady_clock::time_point{} &&
          now >= probe.player_pose_sampled_at && now - probe.player_pose_sampled_at <= std::chrono::milliseconds(500);
      auto command = player_ready && network.seats[user].engaged
          ? network.seats[user].command.value_or(edf::net::InputCommand{})
          : edf::net::InputCommand{};
      if (network.seats[user].position_reset &&
          network.seats[user].position_reset->player_id == network.seats[user].player_id)
        command = {};
      InjectInput(state, command);
      ctx.r3.u32 = 0;
      return;
    }
    // Keep the title/menu path on user 0 while the selected secondary user is
    // also driven by the same route. Without this, a user-1 diagnostic leaves
    // all primary menu input native and never reaches mission gameplay.
    if ((user != 0 && user != scripted_user) || g_input_events.empty()) {
      CountNativeXInputForward(user, last_sampled_mission_ready);
      return;
    }
    const uint32_t elapsed_ms = InputElapsedMs();
    const auto analog = edf::AnalogAt(g_input_events, elapsed_ms);
    edf::net::InputCommand script_input;
    script_input.buttons = edf::ButtonsAt(g_input_events, elapsed_ms);
    const uint16_t previous_script_buttons = g_title_ui_trace_last_buttons.exchange(
        script_input.buttons, std::memory_order_relaxed);
    if (previous_script_buttons != script_input.buttons) {
      // Arm both consumer traces from the actual injected-input transition,
      // rather than relying on the UI callbacks to observe the same clock tick.
      g_title_ui_handoff_window.store(16, std::memory_order_relaxed);
      g_title_scene_update_window.store(16, std::memory_order_relaxed);
    }
    g_input_script_elapsed_ms.store(elapsed_ms, std::memory_order_relaxed);
    g_input_script_buttons.store(script_input.buttons, std::memory_order_relaxed);
    script_input.left_trigger = static_cast<uint8_t>(analog[0]);
    script_input.right_trigger = static_cast<uint8_t>(analog[1]);
    script_input.move_x = static_cast<int16_t>(analog[2]);
    script_input.move_y = static_cast<int16_t>(analog[3]);
    script_input.aim_x = static_cast<int16_t>(analog[4]);
    script_input.aim_y = static_cast<int16_t>(analog[5]);
    InjectInput(state, script_input);
    if (g_trace_input_consumers.load(std::memory_order_relaxed) &&
        user < g_input_consumer_mask_polls.size()) {
      const size_t site = callsite == 0x820A6C04 ? 0 : callsite == 0x821B09C0 ? 1 : 2;
      const size_t mask = script_input.buttons == 0 ? 0 : script_input.buttons == 0x0010 ? 1 :
                          script_input.buttons == 0x1000 ? 2 : 3;
      g_input_consumer_mask_polls[user][site][mask].fetch_add(1, std::memory_order_relaxed);
    }
    ctx.r3.u32 = 0;
    return;
  }
  CountNativeXInputForward(user, last_sampled_mission_ready);
  __imp__sub_8212EA20(ctx, base);
}

// Observe the guest's post-XInput action channels in the headless diagnostic
// targets. This hook is deliberately read-only and is enabled only with
// --trace-input-consumers; it helps distinguish XInput injection from later
// title/menu handling without treating a channel value as proof of a selection.
REX_EXTERN(__imp__sub_821B09A0);
REX_HOOK_RAW(sub_821B09A0) {
  const uint32_t device = ctx.r3.u32;
  __imp__sub_821B09A0(ctx, base);
  ObserveXpadActionChannels(base, device);
}

REX_EXTERN(__imp__sub_8219E6A8);
REX_HOOK_RAW(sub_8219E6A8) {
  const uint32_t manager = ctx.r3.u32;
  const uint32_t callsite = uint32_t(ctx.lr);
  __imp__sub_8219E6A8(ctx, base);
  ObserveInputManagerCallbacks(base, manager, callsite);
  ObserveXpadActionStates(base, callsite);
}

// The generated objects use these two host adapters in the regular renderer
// build. The probe keeps their original runtime behavior without linking the
// D3D-dependent buffer tracking implementation.
REX_EXTERN(__imp__NtReadFile);
REX_HOOK_RAW(edf_native_NtReadFile) { __imp__NtReadFile(ctx, base); }

REX_EXTERN(__imp__RtlFillMemoryUlong);
REX_HOOK_RAW(edf_native_RtlFillMemoryUlong) { __imp__RtlFillMemoryUlong(ctx, base); }

extern "C" void edf_native_observe_guest_scalar_store(
    uint8_t* base, uint32_t address, uint32_t bytes, const char* file, uint32_t line) {
  if (!g_trace_input_consumers.load(std::memory_order_relaxed) || bytes == 0) return;
  const uint64_t store_end = uint64_t(address) + bytes;
  for (uint32_t index = 0; index < g_input_dispatch_gate_watched_addresses.size(); ++index) {
    const uint32_t watched =
        g_input_dispatch_gate_watched_addresses[index].load(std::memory_order_relaxed);
    if (!watched || address > watched || store_end <= watched) continue;
    ObserveInputDispatchGate(base, 0, 3, line,
        {watched - 153, 0, 0, 0, 0, 0}, 0, 0, file, line);
  }
}
