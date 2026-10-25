// EDF2027 - native keyboard and mouse input: the guest side.
//
// Three hooks, all on the game's own input path (addresses are EDF 2017 default.xex):
//   sub_821B09A0  Dx::clInputDevice_XPad's poll. After it has turned the XInput state into
//                 named float channels, held keys are merged into those channels. The game
//                 then derives held / pressed / auto-repeat itself (sub_821D4A28), so
//                 gameplay, vehicles, menus and pause all see keys with no further work,
//                 and whatever a physical pad supplied is kept.
//   sub_820DC890  the soldier's aim update. Mouse motion since the last tick is added to
//                 the turn/pitch inputs as the exact angle it stands for.
//   sub_820A0F70  the control-slot binder. Native input is built for the Technical control
//                 type, so player 0's profile is held at that type.
// See native_kbm_logic.h for the arithmetic and why each hook sits where it does.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/types.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/virtual_key.h>

#include "input_latency.h"
#include "manual_reload.h"
#include "native_kbm.h"
#include "native_kbm_logic.h"
#include "pause_menu.h"
#if defined(EDF2027_BUILD_MULTIPLAYER)
#include "multiplayer/client_input.h"
#endif

REXCVAR_DEFINE_BOOL(edf_kbm, true, "EDF2027", "Native keyboard and mouse input (not pad emulation)");
REXCVAR_DEFINE_BOOL(edf_kbm_mouse_look, true, "EDF2027", "Aim with the mouse");
REXCVAR_DEFINE_DOUBLE(edf_kbm_sensitivity, 1.0, "EDF2027",
                      "Mouse sensitivity; 1.0 turns 0.05 degrees per count, divided by weapon zoom")
    .range(0.05, 20.0);
REXCVAR_DEFINE_BOOL(edf_kbm_invert_y, false, "EDF2027", "Invert vertical mouse aim");
REXCVAR_DEFINE_INT32(edf_kbm_trace, 0, "EDF2027",
                     "Log this many native input events of each kind (diagnostic)")
    .range(0, 100000);
// Diagnostics for runs with no one at the keyboard (the scripted-pad validation runs): they
// stand in for a moving mouse and a held key, and ignore window focus.
REXCVAR_DEFINE_INT32(edf_kbm_test_counts, 0, "EDF2027",
                     "Pretend the mouse moved this many counts to the right every poll (diagnostic)")
    .range(-1000, 1000);
REXCVAR_DEFINE_BOOL(edf_kbm_test_forward, false, "EDF2027", "Pretend the move-forward key is held (diagnostic)");
REXCVAR_DEFINE_INT32(edf_kbm_test_sweep, 0, "EDF2027",
                     "Pretend the mouse sweeps right, stops, sweeps left and stops (500 ms each) at this many counts "
                     "per millisecond, from a 1 kHz thread, while the soldier is on foot: timed input for "
                     "edf_native_input_latency_trace with nobody at the mouse (diagnostic)")
    .range(0, 50);
REXCVAR_DECLARE(std::string, edf_mouse_left);
REXCVAR_DECLARE(std::string, edf_mouse_right);
REXCVAR_DECLARE(std::string, edf_mouse_middle);

namespace edf::kbm {
namespace {

struct SharedInput {
  std::mutex mutex;
  KeyState keys{};
  std::array<bool, 3> mouse_buttons{};
  float dx = 0.0f, dy = 0.0f;
  bool game_owns_input = true;
};
SharedInput& Shared() {
  static SharedInput shared;
  return shared;
}

// Guest-thread state. The pad poll and the aim update can run on different guest threads.
struct GuestSide {
  std::mutex mutex;
  MouseRouter router;
  std::array<std::string, std::size(kKeyActions)> bind_text;
  std::array<std::vector<BindAlternative>, std::size(kKeyActions)> binds;
  bool binds_parsed = false;
};
GuestSide& Guest() {
  static GuestSide guest;
  return guest;
}

uint16_t ResolveKey(std::string_view name) {
  return static_cast<uint16_t>(rex::ui::ParseVirtualKey(name));
}

// Guest memory is big-endian.
uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}
void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, sizeof(value));
}
float LoadFloat(const uint8_t* base, uint32_t address) {
  const uint32_t bits = Load32(base, address);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}
void StoreFloat(uint8_t* base, uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  Store32(base, address, bits);
}

bool Trace(std::atomic<int32_t>& used) {
  return used.load(std::memory_order_relaxed) < REXCVAR_GET(edf_kbm_trace) &&
         used.fetch_add(1, std::memory_order_relaxed) < REXCVAR_GET(edf_kbm_trace);
}

// Dx::clInputDevice_XPad, as laid out by its constructor sub_821B0CC8.
constexpr uint32_t kDeviceUser = 64, kDeviceButtons = 68, kDeviceButtonCount = 76;
constexpr uint32_t kTriggerLeft = 80, kTriggerRight = 84, kTriggerLeftRight = 88, kTriggerRightLeft = 92;
constexpr uint32_t kLeftX = 96, kLeftY = 100, kLeftXReverse = 104, kLeftYReverse = 108;
constexpr uint32_t kRightX = 112, kRightY = 116, kRightXReverse = 120, kRightYReverse = 124;
constexpr uint32_t kDirectionX = 128, kDirectionY = 132, kDirectionXReverse = 136, kDirectionYReverse = 140;
constexpr uint32_t kChannelValue = 32;
// Order of the button table at 0x82556170, which the channel-pointer array follows.
enum ButtonIndex : uint32_t {
  kButtonA, kButtonB, kButtonX, kButtonY, kButtonUp, kButtonDown, kButtonLeft, kButtonRight,
  kButtonLeftThumb, kButtonRightThumb, kButtonLeftShoulder, kButtonRightShoulder, kButtonStart, kButtonBack,
  kButtonCount
};

// The input manager (global at 0x82578640) keeps 64-byte controller profiles from +2356;
// player 0's is the first.
constexpr uint32_t kInputManagerGlobal = 0x82578640, kProfiles = 2356;
constexpr uint32_t kProfileControlType = 0, kProfileSensitivity = 60, kControlTypeTechnical = 1;

void WriteChannel(uint8_t* base, uint32_t channel, float value) {
  if (channel) StoreFloat(base, channel + kChannelValue, value);
}
float ReadChannel(const uint8_t* base, uint32_t channel) {
  return channel ? LoadFloat(base, channel + kChannelValue) : 0.0f;
}
void WriteAxisPair(uint8_t* base, uint32_t device, uint32_t positive, uint32_t negative, float value) {
  WriteChannel(base, Load32(base, device + positive), value);
  WriteChannel(base, Load32(base, device + negative), -value);
}

TechnicalBindings ReadBindings(const uint8_t* base) {
  TechnicalBindings bindings;
  const uint32_t manager = Load32(base, kInputManagerGlobal);
  if (!manager) return bindings;
  const uint32_t profile = manager + kProfiles;  // player 0
  bindings.fire = ResolvePadControl(Load32(base, profile + 12), bindings.fire);
  bindings.zoom = ResolvePadControl(Load32(base, profile + 16), bindings.zoom);
  bindings.jump = ResolvePadControl(Load32(base, profile + 20), bindings.jump);
  bindings.next_weapon = ResolvePadControl(Load32(base, profile + 24), bindings.next_weapon);
  return bindings;
}

#if defined(EDF2027_BUILD_MULTIPLAYER)
void SetNetworkButton(edf::net::InputCommand& input, PadControl control) {
  switch (control) {
    case PadControl::kA: input.buttons |= edf::menu::kPadA; break;
    case PadControl::kB: input.buttons |= edf::menu::kPadB; break;
    case PadControl::kX: input.buttons |= edf::menu::kPadX; break;
    case PadControl::kY: input.buttons |= edf::menu::kPadY; break;
    case PadControl::kLeftShoulder: input.buttons |= edf::menu::kPadLeftShoulder; break;
    case PadControl::kRightShoulder: input.buttons |= edf::menu::kPadRightShoulder; break;
    case PadControl::kLeftTrigger: input.left_trigger = 255; break;
    case PadControl::kRightTrigger: input.right_trigger = 255; break;
    case PadControl::kLeftThumb: input.buttons |= edf::menu::kPadLeftThumb; break;
    case PadControl::kRightThumb: input.buttons |= edf::menu::kPadRightThumb; break;
    case PadControl::kCount: break;
  }
}

int16_t NetworkAxis(float value) {
  const float bounded = std::clamp(value, -1.0f, 1.0f);
  return static_cast<int16_t>(std::lround(bounded * 32767.0f));
}

void PublishKeyboardInput(const ChannelFrame& frame, const MouseRouter::Stick& stick) {
  edf::net::InputCommand input;
  input.move_x = static_cast<int16_t>(frame.move_x * 32767);
  input.move_y = static_cast<int16_t>(frame.move_y * 32767);
  input.aim_x = NetworkAxis(stick.x);
  input.aim_y = NetworkAxis(REXCVAR_GET(edf_kbm_invert_y) ? -stick.y : stick.y);
  for (size_t i = 0; i < frame.pad.size(); ++i) {
    if (frame.pad[i]) SetNetworkButton(input, static_cast<PadControl>(i));
  }
  if (frame.start) input.buttons |= edf::menu::kPadStart;
  if (frame.back) input.buttons |= edf::menu::kPadBack;
  if (frame.menu_x > 0) input.buttons |= edf::menu::kPadRight;
  if (frame.menu_x < 0) input.buttons |= edf::menu::kPadLeft;
  if (frame.menu_y > 0) input.buttons |= edf::menu::kPadUp;
  if (frame.menu_y < 0) input.buttons |= edf::menu::kPadDown;
  edf::net::PublishLocalKeyboardInput(input);
}
#endif

// Caller holds Guest().mutex. Re-parses a bind only when its cvar text changed, so the
// settings screen's edits apply on the next poll.
ActionState ReadActions(GuestSide& guest, const KeyState& keys, const std::array<bool, 3>& mouse_buttons) {
  ActionState actions{};
  for (size_t i = 0; i < std::size(kKeyActions); ++i) {
    std::string text = rex::cvar::GetFlagByName(kKeyActions[i].cvar);
    if (!guest.binds_parsed || text != guest.bind_text[i]) {
      guest.binds[i] = ParseBind(text, &ResolveKey);
      guest.bind_text[i] = std::move(text);
    }
    actions[i] = BindPressed(guest.binds[i], keys);
  }
  guest.binds_parsed = true;
  const std::string mouse_cvars[3] = {REXCVAR_GET(edf_mouse_left), REXCVAR_GET(edf_mouse_right),
                                      REXCVAR_GET(edf_mouse_middle)};
  for (size_t button = 0; button < 3; ++button) {
    if (!mouse_buttons[button]) continue;
    const int action = MouseTargetAt(MouseTargetIndex(mouse_cvars[button])).action;
    if (action >= 0 && static_cast<size_t>(action) < actions.size()) actions[static_cast<size_t>(action)] = true;
  }
  return actions;
}

// The player's on-foot aim update last ran (steady-clock ns), for the diagnostic sweep.
std::atomic<int64_t> g_last_player_aim_ns{0};
int64_t SteadyNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
void PlayerAimed() { g_last_player_aim_ns.store(SteadyNowNs(), std::memory_order_relaxed); }

// edf_kbm_test_sweep: a host thread standing in for a mouse. It adds its counts through
// AddMouseDelta like the window does, so the latency trace times them as input events. It
// emits only while the player's on-foot aim update keeps running (menus and vehicles would
// read it as a stick) and sleeps otherwise; started on the first poll that finds the cvar set.
void AddMouseDeltaFromSweep(float dx);
void EnsureSweepThread() {
  if (!REXCVAR_GET(edf_kbm_test_sweep)) return;
  static std::once_flag started;
  static std::atomic<bool> stop{false};
  std::call_once(started, [] {
    std::atexit([] { stop.store(true, std::memory_order_relaxed); });
    std::thread([] {
      const auto begin = std::chrono::steady_clock::now();
      double last_ms = 0.0;
      while (!stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const double now_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        const int32_t rate = REXCVAR_GET(edf_kbm_test_sweep);
        const bool on_foot = SteadyNowNs() - g_last_player_aim_ns.load(std::memory_order_relaxed) < 100'000'000;
        if (rate > 0 && on_foot) {
          const float counts = SweepCounts(last_ms, now_ms, float(rate));
          if (counts != 0.0f && !stop.load(std::memory_order_relaxed)) AddMouseDeltaFromSweep(counts);
        }
        last_ms = now_ms;
      }
    }).detach();
    REXLOG_INFO("Native K/M: diagnostic mouse sweep thread started (edf_kbm_test_sweep)");
  });
}

void MergeIntoDevice(uint8_t* base, uint32_t device) {
  KeyState keys;
  std::array<bool, 3> mouse_buttons;
  float dx, dy;
  bool game_owns_input;
  {
    SharedInput& shared = Shared();
    std::lock_guard lock(shared.mutex);
    keys = shared.keys;
    mouse_buttons = shared.mouse_buttons;
    dx = shared.dx;
    dy = shared.dy;
    shared.dx = shared.dy = 0.0f;
    game_owns_input = shared.game_owns_input;
  }
  EnsureSweepThread();  // after Shared() exists, so its exit handler runs before Shared() is destroyed
  const int32_t test_counts = REXCVAR_GET(edf_kbm_test_counts);
  const bool test_forward = REXCVAR_GET(edf_kbm_test_forward);
  if (test_counts || test_forward || REXCVAR_GET(edf_kbm_test_sweep)) game_owns_input = true;
  dx += static_cast<float>(test_counts);

  GuestSide& guest = Guest();
  std::lock_guard lock(guest.mutex);
  if (!game_owns_input) {
    guest.router.Reset();
    edf::reload::ResetKeyboard();
    edf::latency::OnInputDiscarded();
#if defined(EDF2027_BUILD_MULTIPLAYER)
    edf::net::ClearLocalKeyboardInput();
#endif
    return;
  }
  // F1 menu (pause_menu.h): nothing reaches the game while it is open, nor after it
  // closes until every key and button then held has been let go - the key or click
  // that closed it must not land in the game. Motion in that window is dropped.
  const bool idle = std::none_of(keys.begin(), keys.end(), [](bool down) { return down; }) &&
                    std::none_of(mouse_buttons.begin(), mouse_buttons.end(), [](bool down) { return down; });
  if (edf::menu::BlockGameInput(edf::menu::MenuInputGate::kKeyboardMouse, idle)) {
    guest.router.Reset();
    edf::reload::ResetKeyboard();
    edf::latency::OnInputDiscarded();
#if defined(EDF2027_BUILD_MULTIPLAYER)
    edf::net::ClearLocalKeyboardInput();
#endif
    return;
  }
  // Input latency trace: this poll gives the game everything received so far.
  edf::latency::OnInputConsumed();
  ActionState actions = ReadActions(guest, keys, mouse_buttons);
  // Manual reload (optional): a request on the press edge and nothing written to the pad
  // channels, so an action sharing the key keeps it. A no-op with edf_manual_reload off.
  edf::reload::OnKeyboardAction(actions[static_cast<size_t>(Action::kReload)]);
  if (test_forward) actions[static_cast<size_t>(Action::kMoveForward)] = true;
  const ChannelFrame frame = BuildChannelFrame(actions, ReadBindings(base));

  const uint32_t buttons = Load32(base, device + kDeviceButtons);
  const uint32_t button_count = Load32(base, device + kDeviceButtonCount);
  const auto button = [&](uint32_t index) -> uint32_t {
    return buttons && index < button_count ? Load32(base, buttons + index * 4) : 0;
  };
  const auto press = [&](uint32_t index, bool down) {
    if (down) WriteChannel(base, button(index), 1.0f);
  };
  const auto pad = [&](PadControl control) { return frame.pad[static_cast<size_t>(control)]; };

  press(kButtonA, pad(PadControl::kA));
  press(kButtonB, pad(PadControl::kB));
  press(kButtonX, pad(PadControl::kX));
  press(kButtonY, pad(PadControl::kY));
  press(kButtonLeftShoulder, pad(PadControl::kLeftShoulder));
  press(kButtonRightShoulder, pad(PadControl::kRightShoulder));
  press(kButtonLeftThumb, pad(PadControl::kLeftThumb));
  press(kButtonRightThumb, pad(PadControl::kRightThumb));
  press(kButtonStart, frame.start);
  press(kButtonBack, frame.back);

  if (pad(PadControl::kLeftTrigger) || pad(PadControl::kRightTrigger)) {
    const uint32_t left = Load32(base, device + kTriggerLeft), right = Load32(base, device + kTriggerRight);
    const float lt = MergeButton(ReadChannel(base, left), pad(PadControl::kLeftTrigger));
    const float rt = MergeButton(ReadChannel(base, right), pad(PadControl::kRightTrigger));
    WriteChannel(base, left, lt);
    WriteChannel(base, right, rt);
    WriteChannel(base, Load32(base, device + kTriggerLeftRight), lt - rt);
    WriteChannel(base, Load32(base, device + kTriggerRightLeft), rt - lt);
  }

  // The *_Direction channels are the left stick with the d-pad laid over it, which is what
  // both movement and menu navigation bind to, so the move keys feed them as well.
  if (frame.move_x) {
    WriteAxisPair(base, device, kLeftX, kLeftXReverse, static_cast<float>(frame.move_x));
    WriteAxisPair(base, device, kDirectionX, kDirectionXReverse, static_cast<float>(frame.move_x));
  }
  if (frame.move_y) {
    WriteAxisPair(base, device, kLeftY, kLeftYReverse, static_cast<float>(frame.move_y));
    WriteAxisPair(base, device, kDirectionY, kDirectionYReverse, static_cast<float>(frame.move_y));
  }
  if (frame.menu_x) {
    WriteAxisPair(base, device, kDirectionX, kDirectionXReverse, static_cast<float>(frame.menu_x));
    press(frame.menu_x > 0 ? kButtonRight : kButtonLeft, true);
  }
  if (frame.menu_y) {
    WriteAxisPair(base, device, kDirectionY, kDirectionYReverse, static_cast<float>(frame.menu_y));
    press(frame.menu_y > 0 ? kButtonUp : kButtonDown, true);
  }

  if (!REXCVAR_GET(edf_kbm_mouse_look)) {
    guest.router.Reset();
#if defined(EDF2027_BUILD_MULTIPLAYER)
    PublishKeyboardInput(frame, {});
#endif
    return;
  }
  const MouseRouter::Stick stick =
      guest.router.OnPoll(dx, dy, static_cast<float>(REXCVAR_GET(edf_kbm_sensitivity)));
  if (stick.x != 0.0f) WriteAxisPair(base, device, kRightX, kRightXReverse, stick.x);
  if (stick.y != 0.0f)
    WriteAxisPair(base, device, kRightY, kRightYReverse, REXCVAR_GET(edf_kbm_invert_y) ? -stick.y : stick.y);
#if defined(EDF2027_BUILD_MULTIPLAYER)
  PublishKeyboardInput(frame, stick);
#endif

  static std::atomic<int32_t> traced{0};
  if ((dx != 0.0f || dy != 0.0f || frame.move_x || frame.move_y) && Trace(traced))
    REXLOG_INFO("Native K/M poll: device={:#x} move=({},{}) mouse=({:.1f},{:.1f}) stick=({:.3f},{:.3f})", device,
                frame.move_x, frame.move_y, dx, dy, stick.x, stick.y);
}

// The unit input snapshot (object +928..+1011) and the soldier fields the aim update reads.
constexpr uint32_t kUnitProfile = 400, kUnitPlayer = 492, kUnitInputDisabled = 624;
constexpr uint32_t kUnitTurnInput = 936, kUnitPitchInput = 940, kUnitYaw = 1316, kUnitPitch = 1312;

void InjectAim(uint8_t* base, uint32_t unit) {
  if (!unit || !REXCVAR_GET(edf_kbm_mouse_look)) return;
  // Diagnostic: every distinct kind of unit that reaches the aim update, once each. Allies
  // run it too, many per tick, so a plain call budget would be spent before the player's
  // unit ever showed up.
  if (REXCVAR_GET(edf_kbm_trace) > 0) {
    static std::mutex seen_mutex;
    static std::vector<uint64_t> seen;
    const uint32_t p = Load32(base, unit + kUnitProfile);
    const uint64_t shape = (uint64_t(Load32(base, unit)) << 32) | Load32(base, unit + kUnitPlayer);
    std::lock_guard lock(seen_mutex);
    if (seen.size() < 32 && std::find(seen.begin(), seen.end(), shape) == seen.end()) {
      seen.push_back(shape);
      REXLOG_INFO("Native K/M aim candidate: unit={:#x} vtable={:#x} player={} input_disabled={} profile={:#x} type={}",
                  unit, Load32(base, unit), static_cast<int32_t>(Load32(base, unit + kUnitPlayer)),
                  base[unit + kUnitInputDisabled], p, p ? Load32(base, p) : 0xFFFFFFFFu);
    }
  }
  // Allied soldiers run the same update with a negative player index; K/M is player 0's.
  if (static_cast<int32_t>(Load32(base, unit + kUnitPlayer)) != 0) return;
  if (base[unit + kUnitInputDisabled]) return;
  const uint32_t profile = Load32(base, unit + kUnitProfile);
  if (!profile || Load32(base, profile + kProfileControlType) != kControlTypeTechnical) return;

  PlayerAimed();
  float dx, dy;
  {
    GuestSide& guest = Guest();
    std::lock_guard lock(guest.mutex);
    if (edf::latency::LowLatencySetting() && REXCVAR_GET(edf_kbm_mouse_look)) {
      // edf_low_latency: the motion that arrived since this step's pad poll too (no smoothing,
      // no acceleration: the counts as they came, turned into an exact angle below).
      guest.router.OnAim(dx, dy, [](float& late_x, float& late_y) {
        SharedInput& shared = Shared();
        std::lock_guard shared_lock(shared.mutex);
        if (!shared.game_owns_input) return;
        late_x = shared.dx;
        late_y = shared.dy;
        shared.dx = shared.dy = 0.0f;
      });
      if (guest.router.late_allowed()) edf::latency::OnInputConsumed();
    } else {
      guest.router.OnAim(dx, dy);
    }
  }
  if (dx == 0.0f && dy == 0.0f) return;
  const float sensitivity = static_cast<float>(REXCVAR_GET(edf_kbm_sensitivity));
  const float profile_sensitivity = LoadFloat(base, profile + kProfileSensitivity);
  const float turn = AimInputForCounts(dx, sensitivity, profile_sensitivity);
  float pitch = AimInputForCounts(dy, sensitivity, profile_sensitivity);
  if (REXCVAR_GET(edf_kbm_invert_y)) pitch = -pitch;
  // The snapshot stores the right stick negated (sub_820D4EC8), so stick-right and
  // stick-up are both negative here; mouse-right is +dx and mouse-up is -dy.
  StoreFloat(base, unit + kUnitTurnInput, LoadFloat(base, unit + kUnitTurnInput) - turn);
  StoreFloat(base, unit + kUnitPitchInput, LoadFloat(base, unit + kUnitPitchInput) + pitch);

  static std::atomic<int32_t> traced{0};
  if (Trace(traced))
    REXLOG_INFO("Native K/M aim: unit={:#x} counts=({:.1f},{:.1f}) input=({:.4f},{:.4f}) profile_sens={:.3f} "
                "zoom_divisor={:.3f} yaw={:.5f} pitch={:.5f} strafe={:.3f} forward={:.3f}",
                unit, dx, dy, -turn, pitch, profile_sensitivity, LoadFloat(base, unit + 896),
                LoadFloat(base, unit + kUnitYaw), LoadFloat(base, unit + kUnitPitch),
                LoadFloat(base, unit + 928), LoadFloat(base, unit + 932));
}

}  // namespace

bool Enabled() { return REXCVAR_GET(edf_kbm); }
bool MouseLookWanted() { return REXCVAR_GET(edf_kbm) && REXCVAR_GET(edf_kbm_mouse_look); }

void SetKey(uint16_t vk, bool down) {
  if (vk >= 256) return;
  bool pressed = false;
  {
    SharedInput& shared = Shared();
    std::lock_guard lock(shared.mutex);
    pressed = down && !shared.keys[vk];
    shared.keys[vk] = down;
  }
  if (pressed) edf::latency::OnInput(edf::latency::InputKind::kKey);
}
void SetMouseButton(int button, bool down) {
  if (button < 0 || button >= 3) return;
  SharedInput& shared = Shared();
  std::lock_guard lock(shared.mutex);
  shared.mouse_buttons[static_cast<size_t>(button)] = down;
}
void AddMouseDelta(float dx, float dy) {
  {
    SharedInput& shared = Shared();
    std::lock_guard lock(shared.mutex);
    shared.dx += dx;
    shared.dy += dy;
  }
  if (dx != 0.0f || dy != 0.0f) edf::latency::OnInput(edf::latency::InputKind::kMouse);
}
namespace {
void AddMouseDeltaFromSweep(float dx) { AddMouseDelta(dx, 0.0f); }
}  // namespace
void ClearInput() {
  SharedInput& shared = Shared();
  std::lock_guard lock(shared.mutex);
  shared.keys.fill(false);
  shared.mouse_buttons.fill(false);
  shared.dx = shared.dy = 0.0f;
}
void SetGameOwnsInput(bool owns) {
  SharedInput& shared = Shared();
  std::lock_guard lock(shared.mutex);
  if (shared.game_owns_input && !owns) {
    shared.keys.fill(false);
    shared.mouse_buttons.fill(false);
    shared.dx = shared.dy = 0.0f;
  }
  shared.game_owns_input = owns;
}

}  // namespace edf::kbm

REX_EXTERN(__imp__sub_821B09A0);
REX_HOOK_RAW(sub_821B09A0) {
  const uint32_t device = ctx.r3.u32;
  __imp__sub_821B09A0(ctx, base);
  // Player 0's pad only: that is where the window-side driver keeps a device connected.
  if (!edf::kbm::Enabled()) {
#if defined(EDF2027_BUILD_MULTIPLAYER)
    edf::net::ClearLocalKeyboardInput();
#endif
  } else if (!device) {
#if defined(EDF2027_BUILD_MULTIPLAYER)
    edf::net::ClearLocalKeyboardInput();
#endif
  } else if (edf::kbm::Load32(base, device + edf::kbm::kDeviceUser) == 0) {
    edf::kbm::MergeIntoDevice(base, device);
  }
}

REX_EXTERN(__imp__sub_820DC890);
REX_HOOK_RAW(sub_820DC890) {
  if (edf::kbm::Enabled()) edf::kbm::InjectAim(base, ctx.r3.u32);
  __imp__sub_820DC890(ctx, base);
}

REX_EXTERN(__imp__sub_820A0F70);
REX_HOOK_RAW(sub_820A0F70) {
  // r3 = input manager, r4 = player. Hold player 0 at the Technical control type before the
  // binder reads it; the Normal type maps turning to the left stick and pitch to a stick
  // position, neither of which a mouse can drive.
  if (edf::kbm::Enabled() && ctx.r3.u32 && ctx.r4.u32 == 0) {
    const uint32_t profile = ctx.r3.u32 + edf::kbm::kProfiles;
    const uint32_t type = edf::kbm::Load32(base, profile + edf::kbm::kProfileControlType);
    if (type != edf::kbm::kControlTypeTechnical) {
      REXLOG_INFO("Native K/M: control type {} -> Technical for player 0", type);
      edf::kbm::Store32(base, profile + edf::kbm::kProfileControlType, edf::kbm::kControlTypeTechnical);
    }
  }
  __imp__sub_820A0F70(ctx, base);
}
