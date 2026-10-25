// Guest-side input hook: overrides the game's XInputGetState wrapper
// (sub_8212EA20: r3=user, r4=X_INPUT_STATE*), the game's only pad read (it imports
// XamInputGetState and XamInputSetState, no keystroke or capability calls). Every pad the
// game sees passes here, under either input backend: the F1 menu chord and gate, then the
// dead zones and the player's remap table (controller_logic.h). Keyboard and mouse do not
// pass through here; see native_kbm.cpp.
#include <chrono>
#include <algorithm>
#include <array>
#include <mutex>
#include <string>
#include <vector>
#include <thread>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/input/input.h>
#include <rex/types.h>
#include "frame_stats.h"
#include "frame_limiter.h"
#include "core_logic.h"
#include "native_graphics/native_renderer_preset.h"
#include "scripted_input_logic.h"
#include "pause_menu.h"
#include "controller_logic.h"
#include "manual_reload.h"
#if defined(EDF2027_BUILD_MULTIPLAYER)
#include "multiplayer/client_input.h"
#include "multiplayer/local_server_host.h"
#endif
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif
using rex::be;
REXCVAR_DECLARE(bool, edf_trace_input);
REXCVAR_DECLARE(bool, edf_rumble);
REXCVAR_DECLARE(int32_t, edf_frame_pacer_spin_us);
REXCVAR_DECLARE(std::string, edf_menu_pad_chord);
REXCVAR_DECLARE(int32_t, edf_pad_left_deadzone);
REXCVAR_DECLARE(int32_t, edf_pad_right_deadzone);
REXCVAR_DECLARE(int32_t, edf_pad_trigger_threshold);
REXCVAR_DECLARE(std::string, edf_pad_remap_p1);
REXCVAR_DECLARE(std::string, edf_pad_remap_p2);

namespace {
uint32_t g_calls = 0, g_last_rc = 0xFFFFFFFF;
uint16_t g_last_btn = 0xFFFF;
#if defined(EDF2027_BUILD_MULTIPLAYER)
uint32_t g_remote_input_packet = 0;

bool SafeRemoteInputStatePointer(uint32_t pointer, uint32_t stack_pointer) {
  constexpr uint32_t size = sizeof(rex::input::X_INPUT_STATE);
  if (!pointer || (pointer & 3u) || uint64_t(pointer) + size > uint64_t(UINT32_MAX) + 1)
    return false;
  const uint32_t distance = pointer > stack_pointer ? pointer - stack_pointer : stack_pointer - pointer;
  return distance <= 0x10000u;
}

void InjectRemoteGuestInput(rex::input::X_INPUT_STATE& state,
                            const edf::net::InputCommand& input) {
  state.packet_number = ++g_remote_input_packet;
  state.gamepad = {};
  // An inactive authenticated peer can retain seat ownership while its native
  // soldier respawns, but its stale buttons must never drive a live host seat.
  // The game's revive/respawn control mapping has not been verified yet.
  if (!input.gameplay_active) return;
  state.gamepad.buttons = input.buttons;
  state.gamepad.left_trigger = input.left_trigger;
  state.gamepad.right_trigger = input.right_trigger;
  state.gamepad.thumb_lx = input.move_x;
  state.gamepad.thumb_ly = input.move_y;
  state.gamepad.thumb_rx = input.aim_x;
  state.gamepad.thumb_ry = input.aim_y;
}
#endif

// The players' remap tables, parsed again only when their cvar text changes (the settings
// menu edits them live). Guest pad polls can come from more than one thread.
struct RemapTables {
  std::mutex mutex;
  std::array<std::string, edf::pad::kRemapPlayers> text;
  std::array<edf::pad::PadRemap, edf::pad::kRemapPlayers> table{edf::pad::PadRemap::Default(),
                                                                 edf::pad::PadRemap::Default()};
};
RemapTables& Remaps() { static RemapTables tables; return tables; }

// Dead zones, then the remap table of guest user `user`; users past the tables get the
// dead zones only. Returns the synthetic actions held; `reload_mapped` says whether the
// table binds a button to the Reload action at all (manual_reload.h).
uint32_t RemapPad(uint32_t user, rex::input::X_INPUT_GAMEPAD& pad, bool* reload_mapped) {
  const auto zones = edf::pad::Deadzones::FromPercent(
      REXCVAR_GET(edf_pad_left_deadzone), REXCVAR_GET(edf_pad_right_deadzone), REXCVAR_GET(edf_pad_trigger_threshold));
  edf::pad::PadRemap table;
  const bool has_table = user < uint32_t(edf::pad::kRemapPlayers);
  if (has_table) {
    const std::string text = user == 0 ? REXCVAR_GET(edf_pad_remap_p1) : REXCVAR_GET(edf_pad_remap_p2);
    auto& remaps = Remaps();
    std::lock_guard lock(remaps.mutex);
    if (text != remaps.text[user]) {
      remaps.text[user] = text;
      remaps.table[user] = edf::pad::ParseRemap(text);
      REXLOG_INFO("Controller remap, player {}: \"{}\"", user + 1, edf::pad::SerializeRemap(remaps.table[user]));
    }
    table = remaps.table[user];
  }
  // Manual reload off: a button mapped to Reload goes back to the game (manual_reload_logic.h).
  if (has_table && !edf::reload::Enabled()) table = edf::reload::WithoutReload(table);
  *reload_mapped = has_table && table.source[size_t(edf::pad::TargetOf(edf::pad::SyntheticAction::kReload))] !=
                                    edf::pad::kNoSource;
  const edf::menu::PadSnapshot raw{pad.buttons, pad.left_trigger, pad.right_trigger,
                                   pad.thumb_lx, pad.thumb_ly, pad.thumb_rx, pad.thumb_ry};
  const auto result = edf::pad::ProcessPad(raw, zones, has_table ? &table : nullptr);
  pad.buttons = result.game.buttons;
  pad.left_trigger = result.game.left_trigger;
  pad.right_trigger = result.game.right_trigger;
  pad.thumb_lx = result.game.lx;
  pad.thumb_ly = result.game.ly;
  pad.thumb_rx = result.game.rx;
  pad.thumb_ry = result.game.ry;
  return result.synthetic;
}

// F1 menu (pause_menu.h): the pad chord that opens it, and while it is open - and until
// the pad has been let go after it closes - the game sees an untouched pad. The chord and
// the drain look at the RAW pad, before dead zones and remapping, so no remap can make the
// menu unreachable. Then the player's dead zones and remap table. Last, manual reload
// (optional, off by default) is shown the poll; it never changes what the game is given.
void RoutePad(const uint8_t* base, uint32_t user, rex::input::X_INPUT_GAMEPAD& pad) {
  const edf::menu::PadSnapshot snapshot{pad.buttons, pad.left_trigger, pad.right_trigger,
                                        pad.thumb_lx, pad.thumb_ly, pad.thumb_rx, pad.thumb_ry};
  const auto routed = edf::menu::RouteGuestPad(snapshot, REXCVAR_GET(edf_menu_pad_chord), user == 0);
  edf::reload::PadPoll poll;
  poll.raw = snapshot.buttons;
  if (routed.blank) {
    pad = rex::input::X_INPUT_GAMEPAD{};
    edf::pad::PublishSynthetic(int(user), 0);
#if defined(EDF2027_BUILD_MULTIPLAYER)
    if (user == 0) edf::net::PublishLocalInput({});
#endif
    poll.blank = true;
    edf::reload::OnGuestPad(base, user, poll);
    return;
  }
  pad.buttons = routed.buttons;  // the chord's buttons taken out, as raw buttons
  const uint32_t synthetic = RemapPad(user, pad, &poll.reload_mapped);
  edf::pad::PublishSynthetic(int(user), synthetic);
  poll.game = pad.buttons;
  poll.reload_held = (synthetic >> unsigned(edf::pad::SyntheticAction::kReload)) & 1u;
  edf::reload::OnGuestPad(base, user, poll);
#if defined(EDF2027_BUILD_MULTIPLAYER)
  if (user == 0) {
    edf::net::InputCommand input;
    input.buttons = pad.buttons;
    input.left_trigger = pad.left_trigger;
    input.right_trigger = pad.right_trigger;
    input.move_x = pad.thumb_lx;
    input.move_y = pad.thumb_ly;
    input.aim_x = pad.thumb_rx;
    input.aim_y = pad.thumb_ry;
    edf::net::PublishLocalInput(input);
  }
#endif
}
}  // namespace

REX_EXTERN(__imp__sub_8212EA20);

REX_HOOK_RAW(sub_8212EA20) {
  const uint32_t user = ctx.r3.u32, ptr = ctx.r4.u32;
  __imp__sub_8212EA20(ctx, base);
  uint32_t rc = ctx.r3.u32;
#if defined(EDF2027_BUILD_MULTIPLAYER)
  // F5's authenticated remote pilot drives EDF's real second player object.
  // Keep the native poll first so the runtime still refreshes device state, then
  // replace the sample even when no physical controller is assigned to user 1.
  if (user == 1 && SafeRemoteInputStatePointer(ptr, ctx.r1.u32)) {
    std::optional<edf::net::InputCommand> remote_input;
    const auto bridge = edf::GetLocalHostEngineBridge();
    {
      std::lock_guard bridge_lock(bridge->mutex);
      if (bridge->host && bridge->host->running())
        remote_input = bridge->host->RemoteGuestInput();
    }
    if (remote_input) {
      auto* state = reinterpret_cast<rex::input::X_INPUT_STATE*>(base + ptr);
      InjectRemoteGuestInput(*state, *remote_input);
      edf::pad::PublishSynthetic(1, 0);
      ctx.r3.u32 = 0;
      return;
    }
  }
#endif
  if (!ptr || rc != 0) {
    if (REXCVAR_GET(edf_trace_input) && user == 0 && rc != g_last_rc)
      REXLOG_INFO("XInputGetState hook: user={} rc={:#x} state={:#x}", user, rc, ptr);
    g_last_rc = rc;
    edf::pad::PublishSynthetic(int(user), 0);  // no pad: nothing held
#if defined(EDF2027_BUILD_MULTIPLAYER)
    if (user == 0) edf::net::PublishLocalInput({});
#endif
    return;
  }
  auto* st = reinterpret_cast<rex::input::X_INPUT_STATE*>(base + ptr);
  uint16_t btn = st->gamepad.buttons;
  ++g_calls;
  if (REXCVAR_GET(edf_trace_input) && user == 0 &&
      (rc != g_last_rc || btn != g_last_btn || (g_calls % 6000) == 0)) {
    REXLOG_INFO("XInputGetState hook: user={} rc={:#x} buttons={:#06x} packet={} (call {})", user, rc, btn,
                (uint32_t)st->packet_number, g_calls);
    g_last_rc = rc; g_last_btn = btn;
  }
  RoutePad(base, user, st->gamepad);
}

// Gate the game's XInputSetState wrapper so the option applies equally to the
// SDL and XInput backends. Sending zero also stops an effect already in flight
// the next time the guest updates vibration.
REX_EXTERN(__imp__sub_8212EA30);
REX_HOOK_RAW(sub_8212EA30) {
  if (!REXCVAR_GET(edf_rumble) && ctx.r4.u32) {
    auto* vibration = reinterpret_cast<rex::input::X_INPUT_VIBRATION*>(base + ctx.r4.u32);
    vibration->left_motor_speed = 0;
    vibration->right_motor_speed = 0;
  }
  __imp__sub_8212EA30(ctx, base);
}

// ---- XAM sign-in / content / notification tracing (first 30 calls each) ----
#define TRACE_WRAPPER(sym, tag)                                                              \
  REX_EXTERN(__imp__##sym);                                                                  \
  REX_HOOK_RAW(sym) {                                                                        \
    static uint32_t n = 0;                                                                   \
    const uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32, a5 = ctx.r5.u32, a6 = ctx.r6.u32,       \
                   a7 = ctx.r7.u32;                                                          \
    __imp__##sym(ctx, base);                                                                 \
    if (REXCVAR_GET(edf_trace_input) && n++ < 30)                                             \
      REXLOG_INFO(tag " (" #sym "): args={:#x},{:#x},{:#x},{:#x},{:#x} -> r3={:#x}", a3, a4, \
                  a5, a6, a7, ctx.r3.u32);                                                   \
  }

TRACE_WRAPPER(sub_8212EA50, "XamShowSigninUI")
TRACE_WRAPPER(sub_8212EA58, "XamShowDeviceSelectorUI")
TRACE_WRAPPER(sub_821FAF78, "SigninState wrapper")
TRACE_WRAPPER(sub_821FB068, "XamContentCreateEx wrapper")
TRACE_WRAPPER(sub_8212EA48, "XamNotifyCreateListener")
TRACE_WRAPPER(sub_823E4F88, "XamUserReadProfileSettings wrapper")

REX_EXTERN(__imp__sub_82178568);
REX_HOOK_RAW(sub_82178568) {  // XNotifyGetNext wrapper: log only when it returns non-zero
  static uint32_t n = 0;
  const uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32, a5 = ctx.r5.u32, a6 = ctx.r6.u32;
  __imp__sub_82178568(ctx, base);
  if (REXCVAR_GET(edf_trace_input) && ctx.r3.u32 && n++ < 30) {
    uint32_t id = a5 ? (uint32_t)(*reinterpret_cast<be<uint32_t>*>(base + a5)) : 0u;
    REXLOG_INFO("XNotifyGetNext (sub_82178568): args={:#x},{:#x},{:#x},{:#x} -> r3={:#x} id={:#x}", a3, a4, a5, a6, ctx.r3.u32, id);
  }
}
REX_EXTERN(__imp__sub_820A6B10);
REX_HOOK_RAW(sub_820A6B10) {
  static uint32_t n = 0;
  const uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32, a5 = ctx.r5.u32, a6 = ctx.r6.u32;
  __imp__sub_820A6B10(ctx, base);
  if (REXCVAR_GET(edf_trace_input) && ctx.r3.u32 && n++ < 30) {
    uint32_t id = a5 ? (uint32_t)(*reinterpret_cast<be<uint32_t>*>(base + a5)) : 0u;
    REXLOG_INFO("XNotifyGetNext (sub_820A6B10): args={:#x},{:#x},{:#x},{:#x} -> r3={:#x} id={:#x}", a3, a4, a5, a6, ctx.r3.u32, id);
  }
}

// ---- frame pacer + frame-time statistics on the game's VdSwap wrapper ----
// Independent rendering cap (edf_fps_cap), see frame_limiter.h and
// docs/framerate-unlock.md "Render cap limiter". Frames are released on an
// absolute deadline grid; the wait is a high-resolution waitable timer until an
// adaptive margin before the deadline, then a QPC spin.
//
// Placement. The guest present wrapper (this hook) reaches edf_native_swap_wait,
// which submits the scene frame and publishes it to the host presenter; frame
// times (edf_native_frame_times) are sampled at that entry. With the frame-rate
// unlock active and VSync off the pacer therefore waits *before* the wrapper:
// the frame is complete, it is released on the grid, and submission/publish
// happens a fixed, short time after the deadline whatever the frame's own work
// cost (including the heavier frames that carry a 60 Hz simulation step).
// This costs up to one period minus the frame's work of extra latency;
// edf_frame_pacer_before_present=false restores the lower-latency placement.
// With VSync on, the unlock off or a movie pacing the swap (movie_pacing_active),
// the pacer keeps its previous placement after the wrapper, where the guest's
// own swap pacing has already run.
REXCVAR_DECLARE(int32_t, edf_fps_cap);
REXCVAR_DECLARE(bool, edf_frametime_log);
REXCVAR_DECLARE(bool, edf_native_memory_log);
REXCVAR_DECLARE(bool, edf_frame_pacer_before_present);
REXCVAR_DECLARE(bool, edf_show_fps);
#ifdef _WIN32
REXCVAR_DECLARE(bool, edf_native_vsync);
namespace edf::native { bool NativeFramerateUnlockActive(); }
#endif
namespace {
// One pacer for the guest's swap thread: the schedule, the sleeper and the
// 5 s window's statistics for the FRAMETIME log.
struct FramePacer {
  edf::FrameDeadlineSchedule schedule;
  edf::PreciseSleeper sleeper;
  uint64_t paced = 0, waited = 0, overruns = 0, rebases = 0;
  int64_t spin_ns = 0, oversleep_ns = 0, oversleep_max_ns = 0, late_max_ns = 0;

  void Pace(int cap) {
    sleeper.SetMinimumMarginNs(int64_t(REXCVAR_GET(edf_frame_pacer_spin_us)) * 1000);
    const int64_t now = edf::PacerClock::NowNs();
    const auto release = schedule.Next(now, cap);
    if (cap <= 0) return;
    ++paced;
    if (release.rebased) ++rebases;
    if (release.deadline_ns <= now) {
      if (!release.reset && !release.rebased) ++overruns;
      return;
    }
    const auto result = sleeper.WaitUntil(release.deadline_ns);
    ++waited;
    spin_ns += result.spin_ns;
    oversleep_ns += result.oversleep_ns;
    oversleep_max_ns = std::max(oversleep_max_ns, result.oversleep_ns);
    late_max_ns = std::max(late_max_ns, result.late_ns);
  }
  // One FRAMEPACER line per report window (with edf_frametime_log): how many
  // releases waited, the spin cost, the timer's oversleep, the worst exit past
  // a deadline, overruns kept on the grid and hitch rebases.
  void Report(bool log, bool before_present) {
    if (log && paced)
      REXLOG_INFO("FRAMEPACER: placement={} timer={} margin_us={:.0f} waited={}/{} spin_avg_us={:.0f} "
                  "oversleep_avg_us={:.0f} oversleep_max_us={:.0f} late_max_us={:.1f} overruns={} rebases={}",
                  before_present ? "before_present" : "after_present",
                  sleeper.high_resolution() ? "high_resolution" : "standard", sleeper.margin_ns() / 1e3,
                  waited, paced, waited ? double(spin_ns) / waited / 1e3 : 0.0,
                  waited ? double(oversleep_ns) / waited / 1e3 : 0.0, oversleep_max_ns / 1e3,
                  late_max_ns / 1e3, overruns, rebases);
    paced = waited = overruns = rebases = 0;
    spin_ns = oversleep_ns = oversleep_max_ns = late_max_ns = 0;
  }
};
FramePacer& Pacer() { static FramePacer pacer; return pacer; }
bool PaceBeforePresent(int cap) {
#ifdef _WIN32
  return cap > 0 && REXCVAR_GET(edf_frame_pacer_before_present) && !REXCVAR_GET(edf_native_vsync) &&
         edf::native::NativeFramerateUnlockActive();
#else
  (void)cap;
  return false;
#endif
}
// One "Native memory" line for tools/soak-report.py: process private bytes
// (commit charge), working set and its peak, handle count, and the engine's
// simulation ticks (the scripted pad's game clock, so a soak window can be
// placed on the input script's timeline).
void LogProcessMemory(double t_s) {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS_EX counters{};
  counters.cb = sizeof(counters);
  DWORD handles = 0;
  const HANDLE process = GetCurrentProcess();
  if (!K32GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
    return;
  GetProcessHandleCount(process, &handles);
  constexpr double kMiB = 1024.0 * 1024.0;
  REXLOG_INFO("Native memory: private_mb={:.1f} working_set_mb={:.1f} peak_working_set_mb={:.1f} "
              "pagefile_mb={:.1f} handles={} sim_ticks={} t={:.0f} s",
              counters.PrivateUsage / kMiB, counters.WorkingSetSize / kMiB, counters.PeakWorkingSetSize / kMiB,
              counters.PagefileUsage / kMiB, handles,
              edf::SimulationTicks().load(std::memory_order_relaxed), t_s);
#else
  (void)t_s;
#endif
}
}  // namespace
namespace edf {
FrameStats& CurrentFrameStats() { static FrameStats s; return s; }
}
namespace {
// One frame into the overlay's ring (frame_stats.h). CPU time is the interval minus the
// heartbeat's wait for the next simulation tick and the render cap's wait: the engine
// thread's busy time, including the present's own work.
void RecordOverlayFrame(std::chrono::steady_clock::duration interval, std::chrono::steady_clock::duration cap_wait) {
  auto& st = edf::CurrentFrameStats();
  const double ms = std::chrono::duration<double, std::milli>(interval).count();
  const uint32_t index = st.frame_count.load(std::memory_order_relaxed);
  st.frame_ms[index % edf::FrameStats::kHistory].store(float(ms), std::memory_order_relaxed);
  st.frame_count.store(index + 1, std::memory_order_release);
  const double engine_wait_ms = double(st.engine_wait_ns.exchange(0, std::memory_order_relaxed)) / 1e6;
  const double cap_wait_ms = std::chrono::duration<double, std::milli>(cap_wait).count();
  const double cpu = std::clamp(ms - engine_wait_ms - cap_wait_ms, 0.0, ms);
  const float previous = st.cpu_ms.load(std::memory_order_relaxed);
  st.cpu_ms.store(previous > 0 ? float(previous * 0.9 + cpu * 0.1) : float(cpu), std::memory_order_relaxed);
}
}  // namespace
REX_EXTERN(__imp__sub_82151460);
REX_EXTERN(__imp__edf_native_present_cpu_tail);
REXCVAR_DECLARE(bool, edf_native_host);
REX_EXTERN(__imp__sub_82151168);
REX_EXTERN(__imp__edf_native_color_cpu_tail);
REX_HOOK_RAW(sub_82151168) {
  if(EDF_NATIVE_FLAG(host)) __imp__edf_native_color_cpu_tail(ctx,base);
  else __imp__sub_82151168(ctx,base);
}
REX_HOOK_RAW(sub_82151460) {
  using clock = std::chrono::steady_clock;
  static auto t0 = clock::now();
  static auto last_frame = t0, last_report = t0;
  static std::vector<double> frame_ms;
  static uint32_t frames = 0, total = 0;
  static bool previous_before_present = false;
  auto& pacer = Pacer();
  const int cap = REXCVAR_GET(edf_fps_cap);
  // A placement switch moves the release point by the frame's work: restart
  // the grid rather than count that as an overrun or a wait.
  const bool before_present = PaceBeforePresent(cap);
  if (before_present != previous_before_present) pacer.schedule.Reset();
  previous_before_present = before_present;
  // Performance overlay: time spent in the render cap's wait, so the frame's CPU time
  // can leave it out. Two clock reads, only while the overlay is shown.
  const bool overlay = REXCVAR_GET(edf_show_fps);
  clock::duration cap_wait{};
  const auto pace = [&] {
    if (!overlay) { pacer.Pace(cap); return; }
    const auto begin = clock::now();
    pacer.Pace(cap);
    cap_wait += clock::now() - begin;
  };
  if (before_present) pace();
  if(EDF_NATIVE_FLAG(host)) __imp__edf_native_present_cpu_tail(ctx, base);
  else __imp__sub_82151460(ctx, base);
  if (!before_present) pace();  // cap 0: releases at once and resets the grid
  auto now = clock::now();
  if (total != 0) frame_ms.push_back(std::chrono::duration<double, std::milli>(now - last_frame).count());
  if (overlay && total != 0) RecordOverlayFrame(now - last_frame, cap_wait);
  last_frame = now; ++frames; ++total;
  double dt = std::chrono::duration<double>(now - last_report).count();
  if (dt >= 5.0) {
    auto& st = edf::CurrentFrameStats();
    const auto summary = edf::SummarizeFrameTimes(frame_ms);
    const double average_ms = summary.average_ms;
    const double minimum_ms = summary.minimum_ms;
    const double maximum_ms = summary.maximum_ms;
    const double low_1_percent_ms = summary.low_1_percent_ms;
    st.fps.store(frames / dt, std::memory_order_relaxed);
    st.average_ms.store(average_ms, std::memory_order_relaxed);
    st.minimum_ms.store(minimum_ms, std::memory_order_relaxed);
    st.maximum_ms.store(maximum_ms, std::memory_order_relaxed);
    st.low_1_percent_ms.store(low_1_percent_ms, std::memory_order_relaxed);
    st.total_frames.store(total, std::memory_order_relaxed);
    REXLOG_INFO("FPS: {:.1f} (frames {} over {:.1f} s, t={:.0f} s)", frames / dt, frames, dt, std::chrono::duration<double>(now - t0).count());
    if (REXCVAR_GET(edf_native_memory_log)) LogProcessMemory(std::chrono::duration<double>(now - t0).count());
    if (REXCVAR_GET(edf_frametime_log))
      REXLOG_INFO("FRAMETIME: avg {:.2f} ms min {:.2f} max {:.2f} 1%-low {:.2f} ms (cap {})", average_ms, minimum_ms, maximum_ms, low_1_percent_ms, cap);
    pacer.Report(REXCVAR_GET(edf_frametime_log), before_present);
    frame_ms.clear(); frames = 0; last_report = now;
  }
}
