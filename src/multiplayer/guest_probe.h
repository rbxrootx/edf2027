#pragma once

#include "guest_seat_mailbox.h"
#include "protocol.h"
#include "scripted_input_logic.h"

#include <array>
#include <cstdint>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace edf::net::probe {

struct GuestPlayerObjectSample {
  int32_t player_index = -1;
  uint32_t object = 0;
  bool alive = false;
  bool ambiguous = false;
  float x = 0, y = 0, z = 0;
  int16_t yaw_mrad = 0;
};

struct MissionSequenceLifecycleSample {
  uint32_t phase = 0;
  uint32_t object = 0;
  uint32_t related = 0;
  uint32_t current_object = 0;
  uint32_t vtable = 0;
  std::array<uint32_t, 32> vtable_entries{};
  uint32_t state_696 = 0;
  uint32_t state_708 = 0;
  uint32_t state_712 = 0;
};

struct MissionSequenceCallbackSample {
  uint32_t function = 0;
  uint32_t phase = 0;
  uint32_t callsite = 0;
  std::array<uint32_t, 6> arguments{};
  uint32_t sequence = 0;
  uint32_t state_696 = 0;
  uint32_t state_708 = 0;
  uint32_t state_712 = 0;
  uint32_t router_object = 0;
  uint32_t router_vtable = 0;
  uint32_t router_related = 0;
  uint32_t router_related_72 = 0;
  uint32_t router_status_before = 0;
  uint32_t router_status_after = 0;
  uint32_t router_text_pointer = 0;
  uint32_t router_valid_mask = 0;
  std::string text_argument;
};

// Bounded snapshots around title-router events that can set or consume the
// mission-confirm latch. Keep before/after state together so repeated router
// events do not erase the transition that explains a failed title route.
struct TitleRouterTransitionSample {
  uint64_t call_id = 0;
  uint32_t script_elapsed_ms = 0;
  uint16_t script_buttons = 0;
  uint32_t callsite = 0;
  uint32_t route_phase = 0;
  uint32_t event = 0;
  // Preserve the callback's raw r3-r8 values alongside the interpreted fields.
  std::array<uint32_t, 6> callback_arguments{};
  uint32_t decoder_context = 0;
  uint32_t decoder_cursor_slot = 0;
  uint32_t decoder_output = 0;
  uint32_t decoder_context_160 = 0;
  uint32_t decoder_context_164 = 0;
  uint32_t decoder_context_168 = 0;
  uint32_t decoder_context_172 = 0;
  uint32_t decoder_stack_top = 0;
  uint32_t decoder_stack_previous = 0;
  uint32_t decoder_entry_callsite = 0;
  // Bits 6/7 mark the VM stack-top and previous-slot reads, respectively.
  uint32_t callback_valid_mask = 0;
  // Entry snapshot; serialized as title_state_before.
  uint32_t title_state = 0;
  std::array<uint8_t, 4> title_state_flags{};
  bool title_state_pointer_valid = false;
  bool title_state_valid = false;
  // Bit i validates the byte at title_state + 2260 + i.
  uint8_t title_state_flags_valid_mask = 0;
  // Re-read after the router dispatch with the same validity semantics.
  uint32_t title_state_after = 0;
  std::array<uint8_t, 4> title_state_flags_after{};
  bool title_state_pointer_valid_after = false;
  bool title_state_valid_after = false;
  uint8_t title_state_flags_valid_mask_after = 0;
  uint32_t router = 0;
  uint32_t latch_owner = 0;
  uint32_t latch_owner_state_60_before = 0;
  uint32_t latch_owner_state_60_after = 0;
  uint32_t route_output = 0;
  uint32_t route_output_value_4_before = 0;
  uint32_t route_output_value_4_after = 0;
  uint32_t request = 0;
  uint32_t request_state_72 = 0;
  uint32_t request_cursor_164_before = 0;
  uint32_t request_cursor_164_after = 0;
  uint32_t request_stack_168_before = 0;
  uint32_t request_stack_168_after = 0;
  uint32_t request_opcode_before = UINT32_MAX;
  uint32_t request_opcode_after = UINT32_MAX;
  uint32_t request_state_60_before = 0;
  uint32_t request_state_60_after = 0;
  uint32_t event_owner_before = 0;
  uint32_t event_owner_after = 0;
  uint32_t event_owner_marker_before = 0;
  uint32_t event_owner_marker_after = 0;
  uint32_t sequence = 0;
  uint32_t sequence_696_before = 0;
  uint32_t sequence_696_after = 0;
  uint32_t sequence_708_before = 0;
  uint32_t sequence_708_after = 0;
  uint32_t sequence_712_before = 0;
  uint32_t sequence_712_after = 0;
  uint32_t compare_calls = 0;
  uint32_t compare_result = UINT32_MAX;
  uint32_t compare_object = 0;
  uint32_t compare_value_20 = 0;
  uint32_t compare_value_32 = 0;
  uint32_t valid_mask = 0;
};

struct DecoderOpcodeSample {
  uint64_t call_order = 0;
  uint64_t title_router_call_id = 0;
  uint32_t caller = 0;
  uint32_t context = 0;
  uint32_t cursor = 0;
  uint32_t cursor_after = 0;
  uint32_t context_160 = 0;
  uint32_t context_168 = 0;
  uint32_t context_168_after = 0;
  uint32_t context_172 = 0;
  uint32_t stack_top_before = 0;
  uint32_t stack_top_after = 0;
  uint32_t stack_previous_before = 0;
  uint32_t stack_previous_after = 0;
  uint32_t context_60_before = 0;
  uint32_t context_60_after = 0;
  uint32_t context_72_before = 0;
  uint32_t context_72_after = 0;
  uint32_t elapsed_ms = 0;
  uint32_t dispatch_event = 0;
  std::array<uint8_t, 12> bytes{};
  uint16_t script_buttons = 0;
  uint8_t opcode = 0;
  uint8_t dispatch_phase = 0xFF;
  uint8_t bytes_read = 0;
};

// Read-only trace of the VM's shared indirect message dispatch. This records
// the actual runtime target selected by the guest's virtual call.
struct RawVmDispatchSample {
  uint64_t order = 0;
  uint32_t callsite = 0;
  uint32_t context = 0;
  uint32_t opcode = 0;
  uint32_t phase = 0;
  uint32_t event = 0;
  uint32_t cursor = 0;
  uint32_t stack = 0;
  uint32_t stack_top = 0;
  uint32_t receiver = 0;
  uint32_t receiver_vtable = 0;
  uint32_t target = 0;
  uint32_t elapsed_ms = 0;
  uint16_t script_buttons = 0;
};

// Bounded read-only snapshot of the retry decision at 0x821DF39C.
struct MissionStartDispatchSample {
  uint64_t order = 0;
  uint32_t guest_time_a = 0;
  uint32_t guest_time_b = 0;
  uint32_t context = 0;
  uint32_t opcode_pc = 0;
  uint32_t fallthrough_pc = 0;
  uint32_t retry_byte_before = 0;
  uint32_t retry_byte_after = 0;
  uint32_t retry_count_before = 0;
  uint32_t retry_count_after = 0;
  uint32_t event = UINT32_MAX;
  uint32_t callback_target = 0;
  uint32_t elapsed_ms = 0;
  uint16_t script_buttons = 0;
  bool input_window = false;
  bool retry_branch_taken = false;
  bool guest_time_valid = false;
};

struct MissionSequenceGateSample {
  uint32_t callsite = 0;
  uint32_t sequence_object = 0;
  uint32_t helper_result = 0;
  uint32_t actual_r31 = 0;
  uint32_t actual_r25 = 0;
  uint32_t branch_value = 0;
  uint32_t sequence_696 = 0;
  uint32_t sequence_708 = 0;
  uint32_t sequence_712 = 0;
  uint32_t title_state = 0;
  uint32_t title_flag_2260 = 0;
  uint32_t input_manager = 0;
  uint32_t input_count = 0;
  uint32_t timer_a = 0;
  uint32_t timer_b = 0;
  uint32_t valid_mask = 0;
  uint64_t calls = 0;
};

struct InputActionTransitionSample {
  uint32_t elapsed_ms = 0;
  uint32_t callsite = 0;
  uint32_t held = 0;
  uint32_t edges = 0;
  uint32_t pulses = 0;
  uint16_t injected_buttons = 0;
  uint8_t user = 0;
};

struct InputManagerCallbackSample {
  uint32_t callsite = 0;
  uint32_t manager = 0;
  uint32_t record = 0;
  uint32_t object = 0;
  uint32_t vtable = 0;
  uint32_t slot0 = 0;
  uint32_t samples = 0;
  std::string class_name;
};

struct TitleUiHandoffSample {
  uint32_t call = 0;
  uint32_t callsite = 0;
  uint32_t receiver = 0;
  uint32_t vtable = 0;
  uint32_t dispatch_method = 0;
  uint32_t r3_after = 0;
  std::string class_name;
  bool receiver_words_valid = false;
  std::array<uint32_t, 32> receiver_words{};
  uint32_t event_object_before = 0;
  uint32_t event_object_after = 0;
  uint8_t flags_before = 0;
  uint8_t flags_after = 0;
  uint8_t marker_before = 0;
  uint8_t marker_after = 0;
  uint32_t script_elapsed_ms = 0;
  uint16_t script_buttons = 0;
};

struct TitleSceneUpdateSample {
  uint32_t call = 0;
  uint32_t callsite = 0;
  uint32_t receiver = 0;
  uint32_t vtable = 0;
  std::string class_name;
  // sub_82178CD8 consumes only r3 (the receiver); these are ambient registers.
  uint32_t ambient_registers[5]{};
  uint32_t result = 0;
  bool receiver_words_valid = false;
  std::array<uint32_t, 32> receiver_words{};
  uint32_t script_elapsed_ms = 0;
  uint16_t script_buttons = 0;
  uint8_t flags_before = 0;
  uint8_t flags_after = 0;
  uint8_t marker_before = 0;
  uint8_t marker_after = 0;
};

struct GuestWaitSample {
  uint32_t call = 0;
  uint32_t callsite = 0;
  uint32_t handle = 0;
  uint32_t alertable = 0;
  uint32_t wait_mode = 0;
  uint32_t timeout = 0;
  uint32_t stack = 0;
  uint32_t r31 = 0;
  uint32_t simulation_ticks = 0;
};

struct TitleSequenceRequestSample {
  uint64_t calls = 0;
  uint32_t callsite = 0;
  uint32_t receiver = 0;
  uint32_t request_object = 0;
  uint32_t request_state_72_before = 0;
  uint32_t request_state_60_before = 0;
  uint32_t request_state_60_after = 0;
  uint32_t sequence_object = 0;
  uint32_t sequence_state_696_before = 0;
  uint32_t sequence_state_696_after = 0;
  uint32_t sequence_state_708_before = 0;
  uint32_t sequence_state_708_after = 0;
  uint32_t sequence_state_712_before = 0;
  uint32_t sequence_state_712_after = 0;
};

// Read-only snapshots of the transient per-slot dispatch object used by
// sub_821CE4C0/sub_821CE3A0. The field at +153 is intentionally called a
// dispatch gate here; its relationship to a gameplay action is unproven.
struct InputDispatchGateSample {
  uint32_t function = 0;
  uint32_t phase = 0;
  uint32_t callsite = 0;
  uint32_t receiver = 0;
  uint32_t vtable = 0;
  int32_t slot_index = -1;
  std::array<uint32_t, 6> arguments{};
  uint64_t fp1_bits = 0;
  uint64_t fp2_bits = 0;
  uint32_t field96_bits = 0;
  uint32_t field140 = 0;
  uint32_t field144 = 0;
  uint32_t field148 = 0;
  uint8_t field152 = 0;
  uint8_t field153 = 0;
  uint32_t samples = 0;
  std::string class_name;
  std::string source_file;
  uint32_t source_line = 0;
};

struct GuestProbeState {
  uint64_t headless_fence_waits = 0;
  uint64_t headless_allocator_waits = 0;
  uint64_t mission_sequence_begins = 0;
  uint32_t input_script_elapsed_ms = 0;
  uint16_t input_script_buttons = 0;
  uint32_t decoder_trace_begin = 0;
  uint32_t decoder_trace_end = 0;
  uint64_t decoder_trace_dropped = 0;
  uint64_t mission_start_dispatch_dropped = 0;
  uint64_t scheduler_calls = 0;
  uint64_t guest_wait_calls = 0;
  std::vector<GuestWaitSample> guest_wait_samples;
  uint64_t dispatches = 0;
  uint64_t steps = 0;
  uint64_t mission_ready_dispatch = 0;
  uint32_t hostile_actors = 0;
  bool loading = true;
  bool has_player = false;
  bool player_alive = false;
  int32_t player_index = -1;
  bool mission_ready = false;
  bool mission_ready_seen = false;
  bool hostile_motion_seen = false;
  uint32_t input_user = 0;
  bool input_user_player_present = false;
  bool input_user_player_ambiguous = false;
  bool input_user_player_alive = false;
  uint32_t input_user_player_ready_samples = 0;
  bool input_user_player_motion_seen = false;
  float player_x = 0, player_y = 0, player_z = 0;
  int16_t player_yaw_mrad = 0;
  std::chrono::steady_clock::time_point player_pose_sampled_at{};
  // Wrapper-call counts, not native XInput polls. Readiness uses the latest world sample.
  std::array<uint64_t, 4> xinput_user_wrapper_calls{};
  std::array<uint64_t, 4> xinput_user_calls_last_sampled_ready{};
  uint64_t other_xinput_wrapper_calls = 0;
  uint64_t other_xinput_calls_last_sampled_ready = 0;
  // Calls forwarded to XamInputGetState; the return status is not observed.
  std::array<uint64_t, 4> xinput_user_native_forwards{};
  std::array<uint64_t, 4> xinput_user_native_forwards_last_sampled_ready{};
  uint64_t other_xinput_native_forwards = 0;
  uint64_t other_xinput_native_forwards_last_sampled_ready = 0;
  bool input_consumer_trace_enabled = false;
  // Per XInput caller: status poll (0x820A6C04), mapped game action poll
  // (0x821B09C0), and any other generated caller. Mask classes are none,
  // Start, A, and other/combined.
  std::array<std::array<std::array<uint64_t, 4>, 3>, 4> input_consumer_mask_polls{};
  // Read-only results from the generated XPad action poll. The bitset records
  // action targets that were ever nonzero during this probe run.
  std::array<uint64_t, 4> xpad_action_poll_calls{};
  std::array<uint32_t, 4> xpad_action_targets_seen{};
  std::array<uint32_t, 4> xpad_action_channels_seen{};
  std::array<uint64_t, 4> xpad_action_state_samples{};
  std::array<uint32_t, 4> xpad_action_held_seen{};
  std::array<uint32_t, 4> xpad_action_edges_seen{};
  std::array<uint32_t, 4> xpad_action_pulses_seen{};
  std::array<uint64_t, 4> xpad_action_invalid_samples{};
  uint64_t title_action_handler_calls = 0;
  uint32_t title_action_handler_last_r31 = 0;
  uint64_t title_action_default_calls = 0;
  uint32_t title_action_default_last_r31 = 0;
  uint64_t title_action_consumer_reads = 0;
  uint32_t title_action_consumer_event_mask = 0;
  uint32_t title_action_consumer_last_event = 0;
  uint32_t title_action_consumer_last_object = 0;
  uint32_t title_action_consumer_last_slot = 0;
  uint32_t title_action_consumer_last_count = 0;
  uint32_t title_action_consumer_last_callsite = 0;
  // The title UI reader has its own callsite and must be isolated from the
  // gameplay/action scanner when diagnosing menu input routing.
  std::array<uint64_t, 4> title_ui_action_slot_reads{};
  std::array<uint32_t, 4> title_ui_action_slot_event_values{};
  std::array<uint32_t, 4> title_ui_action_slot_input_flags{};
  std::array<uint32_t, 4> title_ui_action_slot_last_event{};
  std::array<uint64_t, 4> input_event_update_slot_calls{};
  std::array<uint32_t, 4> input_event_update_slot_callbacks{};
  std::array<uint32_t, 4> input_event_update_slot_last_event{};
  std::array<uint32_t, 4> input_event_update_slot_nonzero_seen{};
  uint64_t input_event_update_calls = 0;
  uint64_t input_event_update_nonzero_calls = 0;
  uint32_t input_event_update_last_object = 0;
  uint32_t input_event_update_last_callback = 0;
  uint32_t input_event_update_last_event_before = 0;
  uint32_t input_event_update_last_event_after = 0;
  uint32_t input_event_update_last_callsite = 0;
  uint64_t title_action_slot_scan_calls = 0;
  uint32_t title_action_slot_scan_manager = 0;
  uint32_t title_action_slot_scan_slot = 0;
  uint32_t title_action_slot_scan_count = 0;
  uint32_t title_action_slot_scan_object = 0;
  uint32_t title_action_slot_scan_event = 0;
  uint64_t title_ui_handoff_calls = 0;
  std::vector<TitleUiHandoffSample> title_ui_handoffs;
  uint64_t title_scene_update_calls = 0;
  std::vector<TitleSceneUpdateSample> title_scene_updates;
  TitleSequenceRequestSample title_sequence_request;
  uint64_t title_action_gate_samples = 0;
  uint32_t title_action_gate_2260_last = 0;
  uint32_t title_action_gate_2261_last = 0;
  uint32_t title_action_gate_2262_last = 0;
  uint32_t title_action_gate_a_object = 0;
  uint32_t title_action_gate_a_link = 0;
  uint32_t title_action_gate_a_state = 0;
  uint32_t title_action_gate_a_result = 0;
  uint32_t title_action_gate_b_object = 0;
  std::string title_action_gate_b_class_name;
  uint32_t title_action_gate_b_712 = 0;
  uint32_t title_action_gate_b_696 = 0;
  uint32_t title_action_gate_b_708 = 0;
  uint32_t title_action_gate_b_result = 0;
  uint32_t title_action_gate_sequence_pointer = 0;
  uint32_t title_action_gate_sequence_708 = 0;
  uint32_t title_action_gate_sequence_result = 0;
  uint32_t title_action_gate_state_object = 0;
  uint32_t title_action_gate_state_vtable = 0;
  std::string title_action_gate_state_class_name;
  uint32_t title_action_gate_input_manager = 0;
  uint32_t title_action_gate_input_count = 0;
  uint64_t title_action_gate_a_function_calls = 0;
  uint32_t title_action_gate_a_function_callsite = 0;
  uint32_t title_action_gate_a_function_return = 0;
  uint64_t title_action_gate_b_function_calls = 0;
  uint32_t title_action_gate_b_function_callsite = 0;
  uint32_t title_action_gate_b_function_return = 0;
  uint32_t title_action_gate_r31 = 0;
  uint32_t title_action_gate_r25 = 0;
  bool title_action_gate_estimates_valid = false;
  bool title_action_gate_timer_valid = false;
  uint32_t title_action_gate_timer_a = 0;
  uint32_t title_action_gate_timer_b = 0;
  uint32_t title_action_gate_timer_branch_taken = 0;
  std::vector<MissionSequenceLifecycleSample> mission_sequence_lifecycle;
  std::vector<MissionSequenceCallbackSample> mission_sequence_callbacks;
  std::vector<TitleRouterTransitionSample> title_router_transitions;
  std::vector<DecoderOpcodeSample> decoder_opcode_samples;
  std::vector<RawVmDispatchSample> raw_vm_dispatch_samples;
  std::vector<MissionStartDispatchSample> mission_start_dispatch_samples;
  std::array<std::array<uint64_t, 32>, 3> title_ui_router_event_calls{};
  std::vector<MissionSequenceGateSample> mission_sequence_gate_trace;
  std::vector<InputActionTransitionSample> input_action_transitions;
  std::vector<InputManagerCallbackSample> input_manager_callbacks;
  uint64_t input_dispatch_gate_calls = 0;
  uint64_t input_dispatch_gate_slot_matches = 0;
  std::vector<InputDispatchGateSample> input_dispatch_gate_trace;
  uint64_t title_action_user_samples = 0;
  uint32_t title_action_slot_gate_count = 0;
  uint32_t title_action_slot_gate_object_mask = 0;
  uint32_t title_action_slot_gate_mask = 0;
  uint32_t title_action_slot_gate_seen = 0;
  uint32_t title_action_registry_pointer = 0;
  uint32_t title_action_user_invalid_stage = 0;
  uint64_t title_action_user_invalid_samples = 0;
  std::vector<GuestPlayerObjectSample> player_objects;
  std::vector<ActorState> authoritative_actors;
  std::vector<ActorState> mission_actors;
  MissionActorRemovalSnapshot mission_actor_removals;
};

struct GuestPlayerSample {
  bool loading = true;
  bool has_player = false;
  bool player_alive = false;
  int32_t player_index = -1;
  bool mission_ready = false;
  float player_x = 0, player_y = 0, player_z = 0;
  int16_t player_yaw_mrad = 0;
  std::chrono::steady_clock::time_point player_pose_sampled_at{};
};

void ConfigureInputScript(std::vector<edf::ScriptedInputEvent> events, edf::ScriptedClock clock);
void ConfigureInputConsumerTrace(bool enabled);
void ConfigureDecoderTraceRange(uint32_t begin, uint32_t end);
void RecordMissionSequenceLifecycle(uint8_t* base, uint32_t phase, uint32_t object,
                                    uint32_t related);
void RecordMissionSequenceCallback(uint8_t* base, uint32_t function, uint32_t phase,
                                   uint32_t callsite, const std::array<uint32_t, 6>& arguments);
TitleRouterTransitionSample CaptureTitleRouterTransition(
    uint8_t* base, uint32_t callsite, const std::array<uint32_t, 6>& arguments,
    const std::array<uint32_t, 3>& callback_registers);
uint64_t RecordDecoderOpcodeSample(uint8_t* base, uint32_t caller, uint32_t context);
void RecordRawVmDispatch(uint8_t* base, uint32_t context, uint32_t opcode,
                         uint32_t phase, uint32_t event, uint32_t target,
                         uint32_t callsite);
void RecordMissionStartDispatch(uint8_t* base, uint32_t context, uint32_t opcode_pc,
                                uint32_t fallthrough_pc, uint32_t event,
                                uint32_t callback_target, uint32_t retry_byte_before,
                                uint32_t retry_count_before, bool retry_branch_taken);
void RecordTitleConfirmCompare(uint8_t* base, uint32_t object, uint32_t result);
void RecordDecoderOpcodeResult(uint8_t* base, uint32_t context, uint64_t call_order);
void AssociateDecoderOpcodeTitleRouterCall(uint64_t sample_index_token,
                                           uint64_t title_router_call_id);
void RecordTitleRouterTransition(uint8_t* base, TitleRouterTransitionSample sample);
void RecordMissionSequenceGate(uint8_t* base, uint32_t callsite, uint32_t sequence_object,
                               uint32_t helper_result, uint32_t actual_r31,
                               uint32_t actual_r25, uint32_t branch_value);
#if !defined(EDF2027_AUTHORITATIVE_HOST)
void ConfigureInputUser(uint32_t user);
#endif
void ConfigureNetworkInput();
#if defined(EDF2027_AUTHORITATIVE_HOST)
void ConfigurePlayerSpawnProbe(uint32_t player_index);
#endif
void MarkNetworkMissionReady();
void PublishNetworkInput(uint16_t player_id, std::optional<InputCommand> command,
                        std::optional<NetworkPositionReset> position_reset = std::nullopt);
bool PublishNetworkPositionReset(const NetworkPositionReset& reset);
void AcknowledgeNetworkPositionReset(uint16_t player_id, uint32_t reset_id);
bool WithPendingNetworkPositionReset(
    uint16_t player_id, uint32_t reset_id,
    const std::function<void(NetworkPositionReset&)>& callback);
NetworkInputView ReadNetworkInput();
GuestProbeState ReadGuestProbeState();
GuestPlayerSample ReadGuestPlayerSample(uint32_t player_index = 0);
std::vector<ActorState> ReadAuthoritativeActors();
std::vector<ActorState> ReadHostMissionActors();
MissionActorRemovalSnapshot ReadHostMissionActorRemovals();
bool GuestMissionReady();
bool QueueAuthoritativeActorSpawn(const ActorSpawnRequest& spawn);
bool QueueAuthoritativeActorDamage(uint32_t actor_id, float damage_points);

}  // namespace edf::net::probe
