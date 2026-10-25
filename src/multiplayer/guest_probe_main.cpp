#include "generated/default/edf2017_init.h"
#include "guest_probe.h"
#include "kernel_tick_timer.h"
#include "../scripted_input.h"
#if defined(EDF2027_AUTHORITATIVE_HOST)
#include "server_runtime.h"
#endif

#include <rex/audio/audio_driver.h>
#include <rex/audio/audio_system.h>
#include <rex/cvar.h>
#include <rex/kernel/init.h>
#include <rex/memory/utils.h>
#include <rex/input/input_system.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

REXCVAR_DECLARE(bool, edf_mp_actor_trace);

namespace {
volatile std::sig_atomic_t g_stop = 0;

void Stop(int) { g_stop = 1; }

bool SetScriptedInputEnvironment(const std::filesystem::path& input_script) {
  if (input_script.empty()) return true;
#if defined(_WIN32)
  return _wputenv_s(L"EDF_INPUT_SCRIPT", input_script.c_str()) == 0;
#else
  const std::string path = input_script.string();
  return ::setenv("EDF_INPUT_SCRIPT", path.c_str(), 1) == 0;
#endif
}

std::unique_ptr<rex::system::IInputSystem> CreateHeadlessScriptedInputSystem(bool tool_mode) {
  auto input = rex::input::CreateDefaultInputSystem(tool_mode);
  input->AddDriver(std::make_unique<ScriptedInputDriver>());
  input->SetDeviceAssignment(std::make_unique<rex::input::SharedAssignment>());
  return input;
}

// The game registers an XAudio render client during startup. A headless host
// still needs ReX's AudioSystem HLE to exist, but must not require a physical
// audio device or play sound. This driver keeps XAudio registration valid and
// drops submitted samples.
class HeadlessAudioDriver final : public rex::audio::AudioDriver {
 public:
  explicit HeadlessAudioDriver(rex::memory::Memory* memory) : AudioDriver(memory) {}
  void SubmitFrame(uint32_t) override {}
};

class HeadlessAudioSystem final : public rex::audio::AudioSystem {
 public:
  explicit HeadlessAudioSystem(rex::runtime::FunctionDispatcher* dispatcher)
      : AudioSystem(dispatcher) {}

  static std::unique_ptr<rex::audio::AudioSystem> Create(
      rex::runtime::FunctionDispatcher* dispatcher) {
    return std::make_unique<HeadlessAudioSystem>(dispatcher);
  }

 protected:
  rex::X_STATUS CreateDriver(size_t, rex::thread::Semaphore*, rex::audio::AudioDriver** out_driver) override {
    if (!out_driver) return rex::X_STATUS{0xC000000D};
    *out_driver = new HeadlessAudioDriver(memory_);
    return rex::X_STATUS{0};
  }

  void DestroyDriver(rex::audio::AudioDriver* driver) override { delete driver; }
};

class KernelTickTimerGuard {
 public:
  explicit KernelTickTimerGuard(rex::Runtime* runtime) { edf::StartKernelTickTimer(runtime); }
  ~KernelTickTimerGuard() { edf::StopKernelTickTimer(); }
  KernelTickTimerGuard(const KernelTickTimerGuard&) = delete;
  KernelTickTimerGuard& operator=(const KernelTickTimerGuard&) = delete;
};

void PrintPlayerObjectSummary(std::ostream& output, const edf::net::probe::GuestProbeState& state) {
  output << ", observed_faction_player_objects=[";
  for (std::size_t i = 0; i < state.player_objects.size(); ++i) {
    if (i) output << ',';
    const auto& player = state.player_objects[i];
    output << "index=" << player.player_index << ':';
    if (player.ambiguous) output << "ambiguous";
    else output << (player.alive ? "alive" : "dead");
  }
  output << ']';
}

void PrintInputCallSummary(std::ostream& output, const edf::net::probe::GuestProbeState& state) {
  output << ", xinput_wrapper_calls=[";
  for (std::size_t user = 0; user < state.xinput_user_wrapper_calls.size(); ++user) {
    if (user) output << ',';
    output << 'u' << user << ':' << state.xinput_user_wrapper_calls[user]
           << '/' << state.xinput_user_calls_last_sampled_ready[user];
  }
  output << ",other:" << state.other_xinput_wrapper_calls
         << '/' << state.other_xinput_calls_last_sampled_ready << ']';
  output << ", xinput_native_forwards=[";
  for (std::size_t user = 0; user < state.xinput_user_native_forwards.size(); ++user) {
    if (user) output << ',';
    output << 'u' << user << ':' << state.xinput_user_native_forwards[user]
           << '/' << state.xinput_user_native_forwards_last_sampled_ready[user];
  }
  output << ",other:" << state.other_xinput_native_forwards
         << '/' << state.other_xinput_native_forwards_last_sampled_ready << ']';
  if (state.input_consumer_trace_enabled) {
    output << ", input_consumer_mask_polls=[";
    for (std::size_t user = 0; user < state.input_consumer_mask_polls.size(); ++user) {
      if (user) output << ',';
      output << 'u' << user << "{status=";
      for (std::size_t mask = 0; mask < 4; ++mask) {
        if (mask) output << '/';
        output << state.input_consumer_mask_polls[user][0][mask];
      }
      output << ",action=";
      for (std::size_t mask = 0; mask < 4; ++mask) {
        if (mask) output << '/';
        output << state.input_consumer_mask_polls[user][1][mask];
      }
      output << ",other=";
      for (std::size_t mask = 0; mask < 4; ++mask) {
        if (mask) output << '/';
        output << state.input_consumer_mask_polls[user][2][mask];
      }
      output << ",xpad_action_polls=" << state.xpad_action_poll_calls[user]
             << ",targets=" << state.xpad_action_targets_seen[user]
             << ",ever_nonzero=0x" << std::hex << state.xpad_action_channels_seen[user]
             << ",held_seen=0x" << state.xpad_action_held_seen[user]
             << ",edge_seen=0x" << state.xpad_action_edges_seen[user]
             << ",pulse_seen=0x" << state.xpad_action_pulses_seen[user]
             << std::dec << ",state_samples=" << state.xpad_action_state_samples[user]
             << ",invalid=" << state.xpad_action_invalid_samples[user] << '}';
    }
    output << "],input_action_transitions=[";
    for (std::size_t i = 0; i < state.input_action_transitions.size(); ++i) {
      if (i) output << ',';
      const auto& sample = state.input_action_transitions[i];
      output << "{ms=" << sample.elapsed_ms << ",user=" << unsigned(sample.user)
             << ",buttons=0x" << std::hex << sample.injected_buttons
             << ",held=0x" << sample.held << ",edges=0x" << sample.edges
             << ",pulses=0x" << sample.pulses << ",callsite=0x" << sample.callsite
             << std::dec << '}';
    }
    output << "],input_manager_callbacks=[";
    for (std::size_t i = 0; i < state.input_manager_callbacks.size(); ++i) {
      if (i) output << ',';
      const auto& sample = state.input_manager_callbacks[i];
      output << "{record=0x" << std::hex << sample.record
             << ",object=0x" << sample.object << ",vtable=0x" << sample.vtable
             << ",slot0=0x" << sample.slot0 << ",class=" << sample.class_name
             << std::dec << ",samples=" << sample.samples << '}';
    }
    output << "],title_action_handler={calls=" << state.title_action_handler_calls
           << ",entry_r31=0x" << std::hex << state.title_action_handler_last_r31
           << "},title_action_default={calls=" << std::dec << state.title_action_default_calls
           << ",entry_r31=0x" << std::hex << state.title_action_default_last_r31
           << std::dec << "},title_action_consumer={reads=" << state.title_action_consumer_reads
           << ",event_mask=0x" << std::hex << state.title_action_consumer_event_mask
           << ",last_event=0x" << state.title_action_consumer_last_event
           << ",object=0x" << state.title_action_consumer_last_object << std::dec
           << ",slot=" << state.title_action_consumer_last_slot
           << ",count=" << state.title_action_consumer_last_count
           << ",callsite=0x" << std::hex << state.title_action_consumer_last_callsite
           << "},title_ui_action_slots=[";
    for (std::size_t slot = 0; slot < state.title_ui_action_slot_reads.size(); ++slot) {
      if (slot) output << ',';
      output << "{slot=" << slot << ",reads=" << state.title_ui_action_slot_reads[slot]
             << ",event_values=0x" << std::hex << state.title_ui_action_slot_event_values[slot]
             << ",input_flags=0x" << state.title_ui_action_slot_input_flags[slot]
             << ",last_event=0x" << state.title_ui_action_slot_last_event[slot] << std::dec << '}';
    }
    output << "],input_event_update_slots=[";
    for (std::size_t slot = 0; slot < state.input_event_update_slot_calls.size(); ++slot) {
      if (slot) output << ',';
      output << "{slot=" << slot << ",calls=" << state.input_event_update_slot_calls[slot]
             << ",callback=0x" << std::hex << state.input_event_update_slot_callbacks[slot]
             << ",event_values=0x" << state.input_event_update_slot_nonzero_seen[slot]
             << ",last_event=0x" << state.input_event_update_slot_last_event[slot]
             << std::dec << '}';
    }
    output << "],input_event_update={calls=" << state.input_event_update_calls
           << ",nonzero_calls=" << state.input_event_update_nonzero_calls
           << ",object=0x" << std::hex << state.input_event_update_last_object
           << ",callback=0x" << state.input_event_update_last_callback
           << ",event=" << std::dec << state.input_event_update_last_event_before << "->"
           << state.input_event_update_last_event_after << ",callsite=0x" << std::hex
           << state.input_event_update_last_callsite
           << "},title_action_slot_scan={calls=" << std::dec << state.title_action_slot_scan_calls
           << ",manager=0x" << std::hex << state.title_action_slot_scan_manager
           << ",slot=" << std::dec << state.title_action_slot_scan_slot
           << ",count=" << state.title_action_slot_scan_count
           << ",object=0x" << std::hex << state.title_action_slot_scan_object
           << ",event=" << std::dec << state.title_action_slot_scan_event
           << "},title_action_gate={samples=" << state.title_action_gate_samples
           << ",byte2260=" << state.title_action_gate_2260_last
           << ",byte2261=" << state.title_action_gate_2261_last
           << ",byte2262=" << state.title_action_gate_2262_last
           << ",a={object=0x" << state.title_action_gate_a_object
           << ",link=0x" << state.title_action_gate_a_link
           << ",state=" << state.title_action_gate_a_state
           << ",result=" << state.title_action_gate_a_result
           << "},b={object=0x" << state.title_action_gate_b_object
           << ",class=" << state.title_action_gate_b_class_name
           << ",712=" << state.title_action_gate_b_712
           << ",696=" << state.title_action_gate_b_696
           << ",708=" << state.title_action_gate_b_708
           << ",result=" << state.title_action_gate_b_result
           << "},estimated_valid=" << state.title_action_gate_estimates_valid
           << ",estimated_r31=" << state.title_action_gate_r31
           << ",estimated_r25=" << state.title_action_gate_r25
           << ",gate_functions={a_calls=" << state.title_action_gate_a_function_calls
           << ",a_callsite=0x" << std::hex << state.title_action_gate_a_function_callsite
           << ",a_return=0x" << state.title_action_gate_a_function_return
           << ",b_calls=" << std::dec << state.title_action_gate_b_function_calls
           << ",b_callsite=0x" << std::hex << state.title_action_gate_b_function_callsite
           << ",b_return=0x" << state.title_action_gate_b_function_return << std::dec << "}"
           << ",title_state={object=0x" << state.title_action_gate_state_object
           << ",vtable=0x" << state.title_action_gate_state_vtable << std::dec
           << ",class=" << state.title_action_gate_state_class_name << "}"
           << std::hex
           << ",input_manager=0x" << state.title_action_gate_input_manager
           << ",input_count=" << state.title_action_gate_input_count
           << ",timer_valid=" << state.title_action_gate_timer_valid
           << ",timer_a=0x" << state.title_action_gate_timer_a
           << ",timer_b=0x" << state.title_action_gate_timer_b
           << ",timer_branch=" << state.title_action_gate_timer_branch_taken
           << std::dec << "},title_ui_handoff_calls=" << state.title_ui_handoff_calls
           << ",title_ui_handoffs=[";
    for (std::size_t i = 0; i < state.title_ui_handoffs.size(); ++i) {
      if (i) output << ',';
      const auto& sample = state.title_ui_handoffs[i];
      output << "{call=" << sample.call << ",callsite=0x" << std::hex << sample.callsite
             << ",receiver=0x" << sample.receiver
             << ",vtable=0x" << sample.vtable << ",dispatch_method=0x"
             << sample.dispatch_method << ",r3_after=0x" << sample.r3_after << std::dec
             << ",class=" << sample.class_name << std::hex << ",event_before=0x"
             << sample.event_object_before << ",event_after=0x" << sample.event_object_after
             << ",flags_before=0x" << static_cast<uint32_t>(sample.flags_before)
             << ",flags_after=0x" << static_cast<uint32_t>(sample.flags_after)
             << ",marker_before=" << std::dec << static_cast<uint32_t>(sample.marker_before)
             << ",marker_after=" << static_cast<uint32_t>(sample.marker_after)
             << ",script_ms=" << sample.script_elapsed_ms << ",script_buttons=0x" << std::hex
             << sample.script_buttons;
      if (sample.receiver_words_valid) {
        output << ",receiver_words=[";
        for (std::size_t word = 0; word < sample.receiver_words.size(); ++word) {
          if (word) output << ',';
          output << "0x" << sample.receiver_words[word];
        }
        output << ']';
      }
      output << std::dec << '}';
    }
    output << "],title_scene_update_calls=" << state.title_scene_update_calls
           << ",title_scene_updates=[";
    for (std::size_t i = 0; i < state.title_scene_updates.size(); ++i) {
      if (i) output << ',';
      const auto& sample = state.title_scene_updates[i];
      output << "{call=" << sample.call << ",callsite=0x" << std::hex << sample.callsite
             << ",receiver=0x" << sample.receiver
             << ",vtable=0x" << sample.vtable << std::dec << ",class=" << sample.class_name
             << std::hex << ",ambient_regs=[0x" << sample.ambient_registers[0]
             << ",0x" << sample.ambient_registers[1] << ",0x" << sample.ambient_registers[2]
             << ",0x" << sample.ambient_registers[3] << ",0x" << sample.ambient_registers[4]
             << "],result=0x" << sample.result << std::dec
             << ",script_ms=" << sample.script_elapsed_ms << ",script_buttons=0x" << std::hex
             << sample.script_buttons << ",flags=0x" << static_cast<uint32_t>(sample.flags_before)
             << "->0x" << static_cast<uint32_t>(sample.flags_after) << std::dec
             << ",marker=" << static_cast<uint32_t>(sample.marker_before) << "->"
             << static_cast<uint32_t>(sample.marker_after);
      if (sample.receiver_words_valid) {
        output << ",receiver_words=[";
        for (std::size_t word = 0; word < sample.receiver_words.size(); ++word) {
          if (word) output << ',';
          output << "0x" << std::hex << sample.receiver_words[word];
        }
        output << std::dec << ']';
      }
      output << '}';
    }
    const auto& action_latch = state.title_sequence_request;
    output << "],title_action_callback_820c59f0={calls=" << action_latch.calls
           << ",callsite=0x" << std::hex << action_latch.callsite
           << ",receiver=0x" << action_latch.receiver
           << ",request_object=0x" << action_latch.request_object << std::dec
           << ",request_72=" << action_latch.request_state_72_before
           << ",request_60=" << action_latch.request_state_60_before << "->"
           << action_latch.request_state_60_after
           << ",sequence=0x" << std::hex << action_latch.sequence_object << std::dec
           << ",seq_696=" << action_latch.sequence_state_696_before << "->"
           << action_latch.sequence_state_696_after
           << ",seq_708=" << action_latch.sequence_state_708_before << "->"
           << action_latch.sequence_state_708_after
           << ",seq_712=" << action_latch.sequence_state_712_before << "->"
           << action_latch.sequence_state_712_after << "},mission_sequence_gate_trace=[";
    for (std::size_t i = 0; i < state.mission_sequence_gate_trace.size(); ++i) {
      if (i) output << ',';
      const auto& sample = state.mission_sequence_gate_trace[i];
      output << "{site=0x" << std::hex << sample.callsite << ",object=0x"
             << sample.sequence_object << std::dec << ",calls=" << sample.calls
             << ",result=" << sample.helper_result << ",actual_r31=" << sample.actual_r31
             << ",actual_r25=" << sample.actual_r25 << ",branch=" << sample.branch_value
             << ",valid=0x" << std::hex << sample.valid_mask << ",696="
             << sample.sequence_696 << ",708=" << sample.sequence_708 << ",712="
             << sample.sequence_712 << ",title=0x" << sample.title_state
             << ",title2260=" << sample.title_flag_2260 << ",input=0x"
             << sample.input_manager << ",input_count=" << sample.input_count
             << ",timer_a=0x" << sample.timer_a << ",timer_b=0x" << sample.timer_b
             << std::dec << '}';
    }
    output << "],mission_sequence_lifecycle=[";
    for (std::size_t i = 0; i < state.mission_sequence_lifecycle.size(); ++i) {
      if (i) output << ',';
      const auto& sample = state.mission_sequence_lifecycle[i];
      output << "{phase=" << sample.phase << ",object=0x" << std::hex << sample.object
             << ",related=0x" << sample.related << ",current=0x" << sample.current_object
             << ",vtable=0x" << sample.vtable << std::dec << ",696=" << sample.state_696
             << ",708=" << sample.state_708 << ",712=" << sample.state_712;
      if (sample.phase == 6) {
        output << ",vt=[";
        for (std::size_t entry = 0; entry < sample.vtable_entries.size(); ++entry) {
          if (entry) output << ',';
          output << "0x" << std::hex << sample.vtable_entries[entry];
        }
        output << std::dec << ']';
      }
      output << '}';
    }
    output << "],mission_sequence_callbacks=[";
    for (std::size_t i = 0; i < state.mission_sequence_callbacks.size(); ++i) {
      if (i) output << ',';
      const auto& sample = state.mission_sequence_callbacks[i];
      output << "{fn=0x" << std::hex << sample.function << ",phase=" << std::dec
             << sample.phase << ",lr=0x" << std::hex << sample.callsite << ",args=[";
      for (std::size_t arg = 0; arg < sample.arguments.size(); ++arg) {
        if (arg) output << ',';
        output << "0x" << sample.arguments[arg];
      }
      output << "],sequence=0x" << sample.sequence << std::dec
             << ",696=" << sample.state_696 << ",708=" << sample.state_708
             << ",712=" << sample.state_712;
      if (sample.function == 0x820C7220 || sample.function == 0x820C5A70 ||
          sample.function == 0x820C5B28) {
        if (sample.function == 0x820C7220)
          output << ",event=0x" << std::hex << (sample.arguments[4] & 0x1F);
        output << ",request_owner=0x" << sample.router_object
               << ",owner_vtable=0x" << sample.router_vtable
               << ",request=0x" << sample.router_related
               << ",request72=0x" << sample.router_related_72
               << ",text_ptr=0x" << sample.router_text_pointer
               << ",status_before=" << std::dec << sample.router_status_before
               << ",status_after=" << sample.router_status_after
               << ",request_valid=0x" << std::hex << sample.router_valid_mask;
        if (!sample.text_argument.empty())
          output << ",text=\"" << sample.text_argument << '\"';
      }
      output << '}';
    }
    output << "],title_router_transitions=[";
    for (std::size_t i = 0; i < state.title_router_transitions.size(); ++i) {
      if (i) output << ',';
      const auto& sample = state.title_router_transitions[i];
      output << "{id=" << std::dec << sample.call_id << ",ms=" << sample.script_elapsed_ms
             << ",buttons=0x" << std::hex
             << sample.script_buttons << ",callsite=0x" << sample.callsite
             << ",phase=" << std::dec << sample.route_phase << ",event=0x" << std::hex
             << sample.event << ",args=[";
      for (std::size_t arg = 0; arg < sample.callback_arguments.size(); ++arg) {
        if (arg) output << ',';
        output << "0x" << sample.callback_arguments[arg];
      }
      output << "],decoder={context=0x" << sample.decoder_context
             << ",cursor_slot=0x" << sample.decoder_cursor_slot << ",output=0x"
             << sample.decoder_output << ",160=0x" << sample.decoder_context_160
             << ",164=0x" << sample.decoder_context_164 << ",168=0x"
             << sample.decoder_context_168 << ",172=0x" << sample.decoder_context_172
             << ",stack_top=0x" << sample.decoder_stack_top << ",stack_top_valid="
             << ((sample.callback_valid_mask & (1u << 6)) != 0)
             << ",stack_previous=0x" << sample.decoder_stack_previous
             << ",stack_previous_valid="
             << ((sample.callback_valid_mask & (1u << 7)) != 0)
             << ",entry=0x" << sample.decoder_entry_callsite << ",valid=0x"
             << sample.callback_valid_mask
             << "},title_state_before={global_valid=" << std::dec
             << sample.title_state_pointer_valid << ",object=0x" << std::hex
             << sample.title_state << ",object_valid=" << std::dec << sample.title_state_valid
             << ",flags=[" << std::hex
             << static_cast<uint32_t>(sample.title_state_flags[0]) << ','
             << static_cast<uint32_t>(sample.title_state_flags[1]) << ','
             << static_cast<uint32_t>(sample.title_state_flags[2]) << ','
             << static_cast<uint32_t>(sample.title_state_flags[3])
             << "],flags_valid=0x" << static_cast<uint32_t>(sample.title_state_flags_valid_mask)
             << "},title_state_after={global_valid=" << std::dec
             << sample.title_state_pointer_valid_after << ",object=0x" << std::hex
             << sample.title_state_after << ",object_valid=" << std::dec
             << sample.title_state_valid_after << ",flags=[" << std::hex
             << static_cast<uint32_t>(sample.title_state_flags_after[0]) << ','
             << static_cast<uint32_t>(sample.title_state_flags_after[1]) << ','
             << static_cast<uint32_t>(sample.title_state_flags_after[2]) << ','
             << static_cast<uint32_t>(sample.title_state_flags_after[3])
             << "],flags_valid=0x"
             << static_cast<uint32_t>(sample.title_state_flags_valid_mask_after)
             << "},router=0x" << sample.router << ",request=0x" << sample.request
             << ",latch_owner=0x" << sample.latch_owner << ",owner60=" << std::dec
             << sample.latch_owner_state_60_before << "->" << sample.latch_owner_state_60_after
             << ",output=0x" << std::hex << sample.route_output << ",output4=0x"
             << sample.route_output_value_4_before << "->0x"
             << sample.route_output_value_4_after
             << ",request72=0x" << sample.request_state_72 << ",request164=0x"
             << sample.request_cursor_164_before << "->0x" << sample.request_cursor_164_after
             << ",request168=0x" << sample.request_stack_168_before << "->0x"
             << sample.request_stack_168_after << ",opcode=0x" << sample.request_opcode_before
             << "->0x" << sample.request_opcode_after << ",request60=" << std::dec
             << sample.request_state_60_before << "->" << sample.request_state_60_after
             << ",event_owner=0x" << std::hex << sample.event_owner_before << "->0x"
             << sample.event_owner_after << ",owner132=" << std::dec
             << sample.event_owner_marker_before << "->" << sample.event_owner_marker_after
             << ",sequence=0x" << std::hex << sample.sequence << std::dec
             << ",696=" << sample.sequence_696_before << "->" << sample.sequence_696_after
             << ",708=" << sample.sequence_708_before << "->" << sample.sequence_708_after
             << ",712=" << sample.sequence_712_before << "->" << sample.sequence_712_after
             << ",compare_calls=" << sample.compare_calls << ",compare_result=0x" << std::hex
             << sample.compare_result << ",compare_object=0x" << sample.compare_object
             << ",compare20=0x" << sample.compare_value_20 << ",compare32=0x"
             << sample.compare_value_32 << std::dec
             << ",valid=0x" << std::hex << sample.valid_mask << std::dec << '}';
    }
    output << "],decoder_trace_range=[0x" << std::hex << state.decoder_trace_begin
           << ",0x" << state.decoder_trace_end << "],decoder_trace_dropped="
           << std::dec << state.decoder_trace_dropped << ",decoder_opcode_samples=[";
    for (std::size_t i = 0; i < state.decoder_opcode_samples.size(); ++i) {
      if (i) output << ',';
      const auto& sample = state.decoder_opcode_samples[i];
      output << "{order=" << std::dec << sample.call_order << ",router_call="
             << sample.title_router_call_id << ",ms=" << sample.elapsed_ms
             << ",buttons=0x" << std::hex << sample.script_buttons
             << ",caller=0x" << sample.caller << ",context=0x"
             << sample.context << ",cursor=0x" << sample.cursor << ",op=0x"
             << static_cast<unsigned>(sample.opcode);
      output << ",cursor_after=0x" << std::hex << sample.cursor_after
             << ",stack=0x" << sample.context_168 << "->0x" << sample.context_168_after
             << ",stack_top=0x" << sample.stack_top_before << "->0x" << sample.stack_top_after
             << ",stack_previous=0x" << sample.stack_previous_before << "->0x"
             << sample.stack_previous_after
             << ",request60=0x" << static_cast<unsigned>(sample.context_60_before)
             << "->0x" << static_cast<unsigned>(sample.context_60_after)
             << ",request72=0x" << std::hex << sample.context_72_before
             << "->0x" << sample.context_72_after << std::dec;
      if (sample.dispatch_phase != 0xFF)
        output << ",phase=" << std::dec << static_cast<unsigned>(sample.dispatch_phase)
               << ",event=0x" << std::hex << sample.dispatch_event;
      output << ",bytes=[";
      for (std::size_t byte = 0; byte < sample.bytes_read; ++byte) {
        if (byte) output << ',';
        output << "0x" << std::hex << static_cast<unsigned>(sample.bytes[byte]);
      }
      output << "],context160=0x" << sample.context_160 << ",context168=0x"
             << sample.context_168 << ",context172=0x" << sample.context_172 << '}';
    }
    output << "],raw_vm_dispatches=[";
    for (std::size_t i = 0; i < state.raw_vm_dispatch_samples.size(); ++i) {
      if (i) output << ',';
      const auto& sample = state.raw_vm_dispatch_samples[i];
      output << "{order=" << std::dec << sample.order << ",ms=" << sample.elapsed_ms
             << ",buttons=0x" << std::hex << sample.script_buttons
             << ",callsite=0x" << sample.callsite << ",context=0x" << sample.context
             << ",opcode=0x" << sample.opcode << ",phase=" << std::dec << sample.phase
             << ",event=0x" << std::hex << sample.event << ",cursor=0x" << sample.cursor
             << ",stack=0x" << sample.stack << ",stack_top=0x" << sample.stack_top
             << ",receiver=0x" << sample.receiver << ",vtable=0x"
              << sample.receiver_vtable << ",target=0x" << sample.target << '}';
    }
    output << "],mission_start_dispatch_dropped=" << std::dec
           << state.mission_start_dispatch_dropped << ",mission_start_dispatches=[";
    for (std::size_t i = 0; i < state.mission_start_dispatch_samples.size(); ++i) {
      if (i) output << ',';
      const auto& sample = state.mission_start_dispatch_samples[i];
      output << "{order=" << std::dec << sample.order << ",ms=" << sample.elapsed_ms
             << ",buttons=0x" << std::hex << sample.script_buttons
             << ",input_window=" << std::dec << sample.input_window
             << ",guest_time_valid=" << std::dec << sample.guest_time_valid << ",guest_time=["
             << sample.guest_time_a << ',' << sample.guest_time_b << "],context=0x" << std::hex
             << sample.context << ",opcode_pc=0x" << sample.opcode_pc << ",fallthrough_pc=0x"
             << sample.fallthrough_pc << ",retry_byte=" << std::dec
             << sample.retry_byte_before << "->" << sample.retry_byte_after
             << ",retry_count=" << sample.retry_count_before << "->"
             << sample.retry_count_after << ",branch="
             << (sample.retry_branch_taken ? "retry" : "saved-state") << ",event=0x"
             << std::hex << sample.event << ",callback_target=0x"
             << sample.callback_target << '}';
    }
    output << "],title_ui_router_events=[";
    bool first_router_event = true;
    for (std::size_t phase = 0; phase < state.title_ui_router_event_calls.size(); ++phase) {
      for (std::size_t event = 0; event < state.title_ui_router_event_calls[phase].size(); ++event) {
        const uint64_t calls = state.title_ui_router_event_calls[phase][event];
        if (!calls) continue;
        if (!first_router_event) output << ',';
        first_router_event = false;
        output << "{phase=" << std::dec << phase << ",event=0x" << std::hex << event
               << ",calls=" << std::dec << calls << '}';
      }
    }
    output << "],title_action_slots={samples=" << state.title_action_user_samples
           << ",count=" << state.title_action_slot_gate_count << ",objects=0x" << std::hex
           << state.title_action_slot_gate_object_mask << ",slot_gate=0x" << state.title_action_slot_gate_mask
           << ",slot_gate_seen=0x" << state.title_action_slot_gate_seen
           << ",registry=0x" << state.title_action_registry_pointer << std::dec
           << ",invalid_stage=" << state.title_action_user_invalid_stage
           << ",invalid=" << state.title_action_user_invalid_samples << '}';
    output << ",dispatch_gate={calls=" << state.input_dispatch_gate_calls
           << ",slot_matches=" << state.input_dispatch_gate_slot_matches << ",trace=[";
    for (std::size_t i = 0; i < state.input_dispatch_gate_trace.size(); ++i) {
      if (i) output << ',';
      const auto& sample = state.input_dispatch_gate_trace[i];
      output << "{fn=0x" << std::hex << sample.function << ",phase=" << std::dec
             << (sample.phase == 1 ? "before" : sample.phase == 3 ? "store" : "after")
             << ",lr=0x" << std::hex
             << sample.callsite << ",receiver=0x" << sample.receiver << ",slot="
             << std::dec << sample.slot_index << ",class=" << sample.class_name
             << ",vtable=0x" << std::hex << sample.vtable << ",args=[";
      for (std::size_t arg = 0; arg < sample.arguments.size(); ++arg) {
        if (arg) output << ',';
        output << "0x" << sample.arguments[arg];
      }
      output << "],f1=0x" << sample.fp1_bits << ",f2=0x" << sample.fp2_bits
             << ",96=0x" << sample.field96_bits << ",140=0x" << sample.field140
             << ",144=0x" << sample.field144 << ",148=0x" << sample.field148
             << ",152=" << std::dec << static_cast<uint32_t>(sample.field152)
             << ",153=" << static_cast<uint32_t>(sample.field153)
             << ",samples=" << sample.samples;
      if (!sample.source_file.empty())
        output << ",source=" << sample.source_file << ':' << sample.source_line;
      output << '}';
    }
    output << "]}";
  }
}

void PrintGuestContextSummary(std::ostream& output, rex::system::XThread* thread,
                              rex::Runtime* runtime) {
  if (thread && thread->thread_state() && thread->thread_state()->context()) {
    const auto* context = thread->thread_state()->context();
    output << ", guest_context_lr=0x" << std::hex << context->lr
           << ", guest_context_sp=0x" << context->r1.u32
           << ", guest_context_indirect=0x" << context->last_indirect_target
           << ", guest_context_ctr=0x" << context->ctr.u32
           << ", guest_context_r3=0x" << context->r3.u32
           << ", guest_context_r31=0x" << context->r31.u32 << std::dec;
  }
  uint32_t guest_tick = 0;
  uint32_t kernel_tick_address = 0;
  if (runtime && runtime->export_resolver() && runtime->memory()) {
    const auto* bundle = runtime->export_resolver()->GetExportByOrdinal("xboxkrnl.exe", 0x00AD);
    if (bundle && bundle->variable_ptr) {
      kernel_tick_address = bundle->variable_ptr + 16;
      const auto* tick_count = runtime->memory()->TranslateVirtual<const uint8_t*>(kernel_tick_address);
      guest_tick = rex::memory::load_and_swap<uint32_t>(tick_count);
    }
  }
  uint64_t title_tick = 0, title_previous_tick = 0;
  if (runtime && runtime->memory()) {
    const auto* ticker = runtime->memory()->TranslateVirtual<const uint8_t*>(0x8257C300);
    title_tick = rex::memory::load_and_swap<uint64_t>(ticker);
    title_previous_tick = rex::memory::load_and_swap<uint64_t>(ticker + 8);
  }
  output << ", kernel_tick_timer_running=" << edf::KernelTickTimerRunning()
         << ", kernel_tick_addr=0x" << std::hex << kernel_tick_address
         << ", kernel_tick=" << std::dec << guest_tick
         << ", title_tick=0x" << std::hex << title_tick
         << ", title_previous_tick=0x" << title_previous_tick << std::dec;
}

void PrintSelectedPlayerMotion(std::ostream& output, const edf::net::probe::GuestProbeState& state) {
  output << ", input_user=" << state.input_user
         << ", input_user_player_present=" << state.input_user_player_present
         << ", input_user_player_ambiguous=" << state.input_user_player_ambiguous
         << ", input_user_player_alive=" << state.input_user_player_alive
         << ", input_user_player_ready_samples=" << state.input_user_player_ready_samples
         << ", input_user_player_motion_seen=" << state.input_user_player_motion_seen;
}

void PrintUsage() {
#if defined(EDF2027_AUTHORITATIVE_HOST)
  std::cout << "EDF2027 experimental headless guest host\n"
               "Usage: edf2027_authoritative_host --game-root DIRECTORY --input-script FILE [options]\n"
               "  --user-root DIRECTORY  User/profile data (default: sibling user-data)\n"
               "  --xex VFS-PATH       Guest XEX path (default game:\\default.xex)\n"
               "  --input-script FILE  Controller route used to boot the headless mission\n"
               "  --duration SECONDS   Host duration, 0 until stopped or 1-420 seconds (default 0)\n"
               "  --name NAME          Server browser name (max 48 bytes)\n"
               "  --mission NAME       Server browser mission label\n"
               "  --port PORT          Gameplay UDP port (default 27717)\n"
               "  --discovery-port PORT LAN discovery UDP port (default 27718)\n"
               "  --no-discovery       Disable LAN server-browser discovery\n"
               "  --max-players COUNT  Advertised slots, 1-512 (default 8)\n"
               "  --master HOST:PORT   Register with a self-hosted Internet directory\n"
               "  --advertise-ip IPv4  Optional reachable IPv4 for directory listings\n"
               "  --mission-actor-diagnostics  Send read-only mission-ant samples to clients\n"
               "  --mission-actor-reconciliation  Send keyed host ant state for matched local ants\n"
               "  --trace-input-consumers  Report raw XPad channels, action edges, and title transition gates\n"
               "  --trace-mission-actors  Log native mission actor spawn/removal lifecycle\n"
               "  --probe-player-index INDEX  Probe creating a Player01.Sgo seat (1-3; default 1)\n"
               "  --scripts DIRECTORY  Trusted server-side .luau scripts\n"
               "  --help               Show this help\n\n"
                "The guest is booted from the supplied controller route. The first two active remote pilots\n"
                "can drive live guest player indices 0 and 1; overflow players use server input simulation.\n"
                "Mission actor replication remains experimental.\n";
#else
  std::cout << "EDF2027 headless guest probe\n"
                "Usage: edf2027_headless_probe --game-root DIRECTORY [options]\n"
                "  --user-root DIRECTORY  User/profile data (default: sibling user-data)\n"
                "  --xex VFS-PATH       Guest XEX path (default game:\\default.xex)\n"
               "  --duration SECONDS   Probe duration, 1-300 (default 30)\n"
               "  --input-script FILE  Inject a controller route and require moving live mission actors\n"
               "  --input-user INDEX  XInput user for the diagnostic route, 0-3 (default 0)\n"
               "  --trace-input-consumers  Report raw XPad channels, action edges, and title transition gates\n"
               "  --trace-decoder-range START END  Record ordered VM opcodes in this guest-address range\n"
                "  --help               Show this help\n";
#endif
}

bool ParseDuration(std::string_view value, uint32_t& seconds) {
  unsigned parsed = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
#if defined(EDF2027_AUTHORITATIVE_HOST)
  if (error != std::errc{} || end != value.data() + value.size() || parsed > 420) return false;
#else
  if (error != std::errc{} || end != value.data() + value.size() || parsed < 1 || parsed > 300) return false;
#endif
  seconds = parsed;
  return true;
}

bool ParseGuestAddress(std::string_view value, uint32_t& address) {
  int base = 10;
  if (value.starts_with("0x") || value.starts_with("0X")) {
    value.remove_prefix(2);
    base = 16;
  }
  if (value.empty()) return false;
  uint32_t parsed = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed, base);
  if (error != std::errc{} || end != value.data() + value.size()) return false;
  address = parsed;
  return true;
}

#if !defined(EDF2027_AUTHORITATIVE_HOST)
bool ParseInputUser(std::string_view value, uint32_t& user) {
  unsigned parsed = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size() || parsed > 3) return false;
  user = parsed;
  return true;
}
#endif

#if defined(EDF2027_AUTHORITATIVE_HOST)
bool ParsePort(std::string_view text, uint16_t& output) {
  unsigned parsed = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (error != std::errc{} || end != text.data() + text.size() || parsed < 1 || parsed > 65535) return false;
  output = static_cast<uint16_t>(parsed);
  return true;
}

bool ParsePlayerCount(std::string_view text, uint16_t& output) {
  unsigned parsed = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (error != std::errc{} || end != text.data() + text.size() || parsed < 1 ||
      parsed > edf::net::kMaxServerPlayers) return false;
  output = static_cast<uint16_t>(parsed);
  return true;
}

bool ParsePlayerIndex(std::string_view text, uint32_t& output) {
  unsigned parsed = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (error != std::errc{} || end != text.data() + text.size() || parsed < 1 || parsed > 3) return false;
  output = parsed;
  return true;
}
#endif
}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path game_root;
  std::filesystem::path user_root;
  std::filesystem::path input_script;
  std::string xex_path = "game:\\default.xex";
#if defined(EDF2027_AUTHORITATIVE_HOST)
  uint32_t duration_seconds = 0;
#else
  uint32_t duration_seconds = 30;
#endif
  bool trace_input_consumers = false;
  bool trace_mission_actors = false;
  uint32_t decoder_trace_begin = 0;
  uint32_t decoder_trace_end = 0;
#if !defined(EDF2027_AUTHORITATIVE_HOST)
  uint32_t input_user = 0;
#endif
#if defined(EDF2027_AUTHORITATIVE_HOST)
  edf::net::ServerConfig server_config;
  // Never trust owner transforms on the guest-backed host. The first two
  // pilots are driven by live EDF player seats; overflow peers use the server's
  // fixed-step input fallback until more guest seats are validated.
  server_config.server_authoritative_movement = true;
  uint32_t probe_player_index = 0;
#endif

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--help" || arg == "-h") {
      PrintUsage();
      return 0;
    }
#if defined(EDF2027_AUTHORITATIVE_HOST)
    if (arg == "--no-discovery") {
      server_config.discovery_port = 0;
      continue;
    }
    if (arg == "--mission-actor-diagnostics") {
      server_config.mission_actor_diagnostics = true;
      continue;
    }
    if (arg == "--mission-actor-reconciliation") {
      server_config.mission_actor_reconciliation = true;
      trace_mission_actors = true;
      continue;
    }
    if (arg == "--trace-mission-actors") {
      trace_mission_actors = true;
      continue;
    }
    if (arg == "--probe-player-index") {
      if (i + 1 >= argc || !ParsePlayerIndex(argv[++i], probe_player_index)) {
        std::cerr << "Invalid player probe index (expected 1..3)\n";
        return 2;
      }
      continue;
    }
#endif
    if (arg == "--trace-input-consumers") {
      trace_input_consumers = true;
      continue;
    }
    if (arg == "--trace-decoder-range") {
      if (i + 2 >= argc || !ParseGuestAddress(argv[i + 1], decoder_trace_begin) ||
          !ParseGuestAddress(argv[i + 2], decoder_trace_end) ||
          decoder_trace_begin >= decoder_trace_end) {
        std::cerr << "Invalid decoder trace range (expected START < END)\n";
        return 2;
      }
      i += 2;
      trace_input_consumers = true;
      continue;
    }
#if !defined(EDF2027_AUTHORITATIVE_HOST)
    if (arg == "--input-user") {
      if (i + 1 >= argc || !ParseInputUser(argv[++i], input_user)) {
        std::cerr << "Invalid input user (expected 0..3)\n";
        return 2;
      }
      continue;
    }
#endif
    if (i + 1 >= argc) {
      std::cerr << "Missing value for " << arg << '\n';
      PrintUsage();
      return 2;
    }
    const std::string_view value(argv[++i]);
    if (arg == "--game-root") game_root = std::filesystem::path(std::string(value));
    else if (arg == "--user-root") user_root = std::filesystem::path(std::string(value));
    else if (arg == "--xex") xex_path = value;
    else if (arg == "--input-script") input_script = std::filesystem::path(std::string(value));
#if defined(EDF2027_AUTHORITATIVE_HOST)
    else if (arg == "--name") server_config.name = value;
    else if (arg == "--mission") server_config.mission = value;
    else if (arg == "--master") server_config.master_endpoint = value;
    else if (arg == "--advertise-ip") server_config.advertise_ip = value;
    else if (arg == "--scripts") server_config.scripts_directory = std::filesystem::path(std::string(value));
    else if (arg == "--port") {
      if (!ParsePort(value, server_config.game_port)) { std::cerr << "Invalid gameplay port\n"; return 2; }
    } else if (arg == "--discovery-port") {
      if (!ParsePort(value, server_config.discovery_port)) { std::cerr << "Invalid discovery port\n"; return 2; }
    } else if (arg == "--max-players") {
      if (!ParsePlayerCount(value, server_config.max_players)) { std::cerr << "Invalid player count\n"; return 2; }
    }
#endif
    else if (arg == "--duration") {
      if (!ParseDuration(value, duration_seconds)) {
        std::cerr << "Invalid probe duration\n";
        return 2;
      }
    } else {
      std::cerr << "Unknown option: " << arg << '\n';
      PrintUsage();
      return 2;
    }
  }

#if !defined(EDF2027_AUTHORITATIVE_HOST)
  if (input_user > 0 && input_script.empty()) {
    std::cerr << "--input-user values greater than 0 require --input-script\n";
    PrintUsage();
    return 2;
  }
#endif

  if (game_root.empty() || !std::filesystem::is_directory(game_root)) {
    std::cerr << "--game-root must name an extracted EDF game-data directory\n";
    return 2;
  }
  if (user_root.empty()) user_root = game_root.parent_path() / "user-data";
  std::error_code user_root_error;
  std::filesystem::create_directories(user_root, user_root_error);
  if (user_root_error) {
    std::cerr << "Could not create user/profile directory " << user_root << ": "
              << user_root_error.message() << '\n';
    return 2;
  }

#if defined(EDF2027_AUTHORITATIVE_HOST)
  if (input_script.empty()) {
    std::cerr << "--input-script is required to boot the guest mission headlessly\n";
    return 2;
  }
  if (server_config.name.empty() || server_config.name.size() > edf::net::kMaxServerNameBytes ||
      server_config.mission.empty() || server_config.mission.size() > edf::net::kMaxMissionNameBytes) {
    std::cerr << "Server name and mission label must fit the multiplayer protocol limits\n";
    return 2;
  }
#endif

  if (!input_script.empty()) {
    std::ifstream script(input_script);
    if (!script) {
      std::cerr << "Could not open input script: " << input_script << '\n';
      return 2;
    }
    const auto clock = edf::ParseInputClock(script);
    script.clear();
    script.seekg(0);
    auto events = edf::ParseInputEvents(script);
    if (script.bad() || events.empty()) {
      std::cerr << "Input script contains no valid controller events: " << input_script << '\n';
      return 2;
    }
    edf::net::probe::ConfigureInputScript(std::move(events), clock);
  }

  edf::net::probe::ConfigureDecoderTraceRange(decoder_trace_begin, decoder_trace_end);
  edf::net::probe::ConfigureInputConsumerTrace(trace_input_consumers);
#if !defined(EDF2027_AUTHORITATIVE_HOST)
  edf::net::probe::ConfigureInputUser(input_user);
#endif

#if defined(EDF2027_AUTHORITATIVE_HOST)
  edf::net::probe::ConfigurePlayerSpawnProbe(probe_player_index ? probe_player_index : 1);
  edf::net::probe::ConfigureNetworkInput();
#endif

  std::signal(SIGINT, Stop);
  std::signal(SIGTERM, Stop);

  if (!SetScriptedInputEnvironment(input_script)) {
    std::cerr << "Could not configure the scripted headless input driver\n";
    return 2;
  }
  rex::Runtime runtime(game_root, user_root);
  rex::RuntimeConfig config{
      .audio_factory = REX_AUDIO_BACKEND(HeadlessAudioSystem),
      .input_factory = [scripted_input = !input_script.empty()](bool tool_mode) {
        return scripted_input ? CreateHeadlessScriptedInputSystem(tool_mode)
                              : rex::input::CreateDefaultInputSystem(tool_mode);
      },
      .kernel_init = rex::kernel::InitializeKernel,
      .tool_mode = true,
  };
  const rex::X_STATUS setup_status = runtime.Setup(PPCImageConfig, std::move(config));
  if (XFAILED(setup_status)) {
    std::cerr << "ReX headless setup failed: " << std::hex << static_cast<uint32_t>(setup_status) << '\n';
    return 1;
  }

#if defined(EDF2027_AUTHORITATIVE_HOST)
  REXCVAR_SET(edf_mp_actor_trace, trace_mission_actors);
#endif

  const rex::X_STATUS load_status = runtime.LoadXexImage(xex_path);
  if (XFAILED(load_status)) {
    std::cerr << "XEX load failed for " << xex_path << ": " << std::hex << static_cast<uint32_t>(load_status) << '\n';
    return 1;
  }

  // Match the desktop app's OnPostSetup path. The EDF guest scheduler waits on
  // KeTimeStampBundle.TickCount before its first engine dispatch.
  KernelTickTimerGuard tick_timer(&runtime);

#if defined(EDF2027_AUTHORITATIVE_HOST)
  edf::net::ServerRuntime server(
      std::move(server_config),
      [](std::string line) { std::cout << "[server] " << line << '\n' << std::flush; },
      [](uint16_t player_id, std::optional<edf::net::InputCommand> input,
         std::optional<edf::net::AuthoritativePositionReset> reset) {
        std::optional<edf::net::probe::NetworkPositionReset> network_reset;
        if (reset) {
          network_reset = edf::net::probe::NetworkPositionReset{
              reset->player_id, reset->reset_id, reset->x_cm, reset->y_cm,
              reset->z_cm, reset->yaw_mrad};
        }
        edf::net::probe::PublishNetworkInput(player_id, std::move(input), network_reset);
      },
      []() -> std::vector<edf::net::AuthoritativePlayerTransform> {
        const auto network = edf::net::probe::ReadNetworkInput();
        std::vector<edf::net::AuthoritativePlayerTransform> transforms;
        if (!network.enabled) return transforms;
        const auto now = std::chrono::steady_clock::now();
        constexpr auto kMaximumPoseAge = std::chrono::milliseconds(500);
        const auto to_centimeters = [](float meters) -> std::optional<int32_t> {
          if (!std::isfinite(meters)) return std::nullopt;
          const double centimeters = double(meters) * 100.0;
          if (centimeters < -1'000'000.0 || centimeters > 1'000'000.0) return std::nullopt;
          return static_cast<int32_t>(std::lround(centimeters));
        };
        for (const auto& seat : network.seats) {
          if (!seat.player_id) continue;
          edf::net::AuthoritativePlayerTransform transform;
          transform.player_id = seat.player_id;
          transform.acknowledged_reset_player_id = seat.acknowledged_reset_player_id;
          transform.acknowledged_reset_id = seat.acknowledged_reset_id;
          if (!network.mission_ready || !seat.engaged || !seat.command ||
              (seat.position_reset && seat.position_reset->player_id == seat.player_id)) {
            transforms.push_back(transform);
            continue;
          }
          const auto guest = edf::net::probe::ReadGuestPlayerSample(seat.player_index);
          if (guest.loading || !guest.has_player || !guest.player_alive ||
              guest.player_index != static_cast<int32_t>(seat.player_index) ||
              guest.player_pose_sampled_at == std::chrono::steady_clock::time_point{} ||
              now < guest.player_pose_sampled_at || now - guest.player_pose_sampled_at > kMaximumPoseAge) {
            transforms.push_back(transform);
            continue;
          }
          const auto x = to_centimeters(guest.player_x);
          const auto y = to_centimeters(guest.player_y);
          const auto z = to_centimeters(guest.player_z);
          if (x && y && z) {
            transform.available = true;
            transform.x_cm = *x;
            transform.y_cm = *y;
            transform.z_cm = *z;
            transform.yaw_mrad = guest.player_yaw_mrad;
          }
          transforms.push_back(transform);
        }
        return transforms;
      },
      [](uint16_t player_id, uint32_t reset_id, int32_t x_cm, int32_t y_cm,
         int32_t z_cm, int16_t yaw_mrad) {
        return edf::net::probe::PublishNetworkPositionReset(
            {player_id, reset_id, x_cm, y_cm, z_cm, yaw_mrad});
      },
      [] { return edf::net::probe::ReadAuthoritativeActors(); },
      [](const edf::net::ActorSpawnRequest& spawn) {
        if (!edf::net::probe::GuestMissionReady()) return false;
        return edf::net::probe::QueueAuthoritativeActorSpawn(spawn);
      },
      [](uint32_t actor_id, float damage_points) {
        return edf::net::probe::QueueAuthoritativeActorDamage(actor_id, damage_points);
      },
      [] { return edf::net::probe::ReadHostMissionActors(); },
      [] { return edf::net::probe::ReadHostMissionActorRemovals(); });
  std::string server_error;
  if (!server.Start(&server_error)) {
    std::cerr << "Server startup failed: " << server_error << '\n';
    return 1;
  }
  std::atomic<bool> stop_server{false};
  std::atomic<bool> server_finished{false};
  std::atomic<int> server_result{0};
  std::thread server_thread([&] {
    server_result.store(server.Run(stop_server), std::memory_order_release);
    server_finished.store(true, std::memory_order_release);
  });
#endif

  auto guest_thread = runtime.LaunchModule();
  if (!guest_thread) {
    std::cerr << "ReX did not create the guest main thread\n";
#if defined(EDF2027_AUTHORITATIVE_HOST)
    stop_server.store(true, std::memory_order_relaxed);
    server_thread.join();
    server.Stop();
#endif
    return 1;
  }

  std::atomic<bool> guest_finished{false};
  std::thread guest_waiter([&] {
    guest_thread->Wait(0, 0, 0, nullptr);
    guest_finished.store(true, std::memory_order_release);
  });

#if defined(EDF2027_AUTHORITATIVE_HOST)
  std::cout << "Headless EDF guest and multiplayer server started";
  if (duration_seconds) std::cout << " for " << duration_seconds << " seconds";
  else std::cout << "; press Ctrl+C to stop";
  std::cout << "; waiting for one remote pilot and a live guest player index 0.\n";
#else
  std::cout << "Headless guest started. Watching completed sub_821A4BA0 dispatches for "
            << duration_seconds << " seconds";
  if (!input_script.empty()) std::cout << " with scripted input and mission actor state sampling";
  std::cout << ".\n" << std::flush;
#endif
  const auto deadline = duration_seconds
      ? std::chrono::steady_clock::now() + std::chrono::seconds(duration_seconds)
      : std::chrono::steady_clock::time_point::max();
  auto last_report = std::chrono::steady_clock::now();
  while (!g_stop &&
         !guest_finished.load(std::memory_order_acquire) &&
#if defined(EDF2027_AUTHORITATIVE_HOST)
         !server_finished.load(std::memory_order_acquire) &&
#endif
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const auto now = std::chrono::steady_clock::now();
    if (now - last_report >= std::chrono::seconds(1)) {
      const auto state = edf::net::probe::ReadGuestProbeState();
  std::cout << "Scheduler calls: " << state.scheduler_calls
            << ", headless fence waits: " << state.headless_fence_waits
            << ", headless allocator waits: " << state.headless_allocator_waits
           << ", mission sequence begin callback calls: " << state.mission_sequence_begins
            << ", input_script_ms=" << state.input_script_elapsed_ms
            << ", input_script_buttons=0x" << std::hex << state.input_script_buttons << std::dec
                << ", completed dispatches: " << state.dispatches
                << ", engine steps: " << state.steps;
      if (!input_script.empty()) {
        std::cout << ", loading=" << state.loading
                  << ", player=" << state.has_player
                  << ", player_alive=" << state.player_alive
                  << ", hostile_actors=" << state.hostile_actors
                  << ", mission_ready=" << state.mission_ready
                  << ", mission_ready_seen=" << state.mission_ready_seen
                  << ", hostile_motion_seen=" << state.hostile_motion_seen;
      }
      PrintPlayerObjectSummary(std::cout, state);
      PrintSelectedPlayerMotion(std::cout, state);
      PrintInputCallSummary(std::cout, state);
      PrintGuestContextSummary(std::cout, guest_thread.get(), &runtime);
      std::cout << '\n' << std::flush;
      last_report = now;
    }
  }

#if defined(EDF2027_AUTHORITATIVE_HOST)
  stop_server.store(true, std::memory_order_relaxed);
#endif
  if (!guest_finished.load(std::memory_order_acquire)) runtime.kernel_state()->TerminateTitle();
  // TerminateTitle signals the guest; the waiter observes thread completion
  // asynchronously. Give a cooperative guest time to unwind before treating
  // it as a stalled thread and taking the isolated-process fallback below.
  if (!guest_finished.load(std::memory_order_acquire)) {
    const auto termination_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!guest_finished.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < termination_deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  if (!guest_finished.load(std::memory_order_acquire)) {
    // ReX deliberately leaves non-cooperative guest threads alive rather than
    // force-killing a host thread that may own CRT or runtime locks. This probe
    // runs as its own process, so report the stalled guest and let the OS reap
    // it instead of hanging forever in the waiter or destroying live runtime state.
    const auto state = edf::net::probe::ReadGuestProbeState();
    std::cerr << "Guest thread did not stop after title termination; exiting the probe process "
                 "without tearing down live guest runtime state. Scheduler calls: "
              << state.scheduler_calls << ", completed dispatches: " << state.dispatches
              << ", engine steps: " << state.steps << ".\n" << std::flush;
#if defined(EDF2027_AUTHORITATIVE_HOST)
    server_thread.join();
    server.Stop();
#endif
    std::_Exit(1);
  }
  guest_waiter.join();
#if defined(EDF2027_AUTHORITATIVE_HOST)
  server_thread.join();
  server.Stop();
#endif
  const auto state = edf::net::probe::ReadGuestProbeState();
  std::cout << "Final scheduler calls: " << state.scheduler_calls
            << ", headless fence waits: " << state.headless_fence_waits
            << ", headless allocator waits: " << state.headless_allocator_waits
           << ", mission sequence begin callback calls: " << state.mission_sequence_begins
            << ", input_script_ms=" << state.input_script_elapsed_ms
            << ", input_script_buttons=0x" << std::hex << state.input_script_buttons << std::dec
            << ", completed dispatches: " << state.dispatches
            << ", engine steps: " << state.steps;
  if (!input_script.empty()) {
    std::cout << ", mission_ready_seen=" << state.mission_ready_seen
              << " at dispatch " << state.mission_ready_dispatch
              << ", hostile_motion_seen=" << state.hostile_motion_seen;
  }
  std::cout << ", guest_wait_calls=" << state.guest_wait_calls << ", guest_wait_samples=[";
  for (std::size_t i = 0; i < state.guest_wait_samples.size(); ++i) {
    if (i) std::cout << ',';
    const auto& sample = state.guest_wait_samples[i];
    std::cout << "{call=" << sample.call << ",site=0x" << std::hex << sample.callsite
              << ",handle=0x" << sample.handle << ",alertable=0x" << sample.alertable
              << ",mode=0x" << sample.wait_mode << ",timeout=0x" << sample.timeout
              << ",sp=0x" << sample.stack << ",r31=0x" << sample.r31 << std::dec
              << ",ticks=" << sample.simulation_ticks << '}';
  }
  std::cout << ']';
  PrintPlayerObjectSummary(std::cout, state);
  PrintSelectedPlayerMotion(std::cout, state);
  PrintInputCallSummary(std::cout, state);
  PrintGuestContextSummary(std::cout, guest_thread.get(), &runtime);
  std::cout << '\n' << std::flush;
#if !defined(EDF2027_AUTHORITATIVE_HOST)
  bool selected_user_diagnostic_failed = false;
  if (!input_script.empty() && input_user > 0) {
    const bool wrapper_called = state.xinput_user_wrapper_calls[input_user] > 0;
    if (!wrapper_called) {
      std::cerr << "No XInput wrapper calls were observed for selected user " << input_user
                << "; the secondary-player diagnostic failed.\n";
      selected_user_diagnostic_failed = true;
    }
    if (!state.input_user_player_motion_seen) {
      std::cerr << "No unique living clPlayerObject with +492=" << input_user
                << " moved at least 1 meter after 8 consecutive ready samples; "
                   "the secondary-player diagnostic failed.\n";
      selected_user_diagnostic_failed = true;
    }
  }
#endif
  if (state.steps == 0) {
    std::cerr << "No engine steps completed; the guest did not demonstrate headless dispatcher progress.\n";
    return 1;
  }
  if (!input_script.empty() && (!state.mission_ready_seen || !state.hostile_motion_seen)) {
    std::cerr << "The scripted run did not demonstrate a live player and moving hostile actors after loading.\n";
    return 1;
  }
#if !defined(EDF2027_AUTHORITATIVE_HOST)
  if (selected_user_diagnostic_failed) return 1;
#endif
#if defined(EDF2027_AUTHORITATIVE_HOST)
  if (server_result.load(std::memory_order_acquire) != 0)
    return server_result.load(std::memory_order_relaxed);
#endif
  return 0;
}
