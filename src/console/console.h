// EDF2027 - the in-game console service: command registry, executor and output.
//
// Threads. The console dialog (console_dialog.h) submits lines on the UI thread. A line
// whose commands are all "immediate" (help, echo, cvars, alias, exec, ...) runs there at
// once. Anything that touches game state (flag kEngine: spawn, killall, god, ...) and
// anything that waits goes to a runner, and runners only advance on the engine thread, at
// the start of a simulation step: the step dispatcher hook (frame.cpp, sub_821A4BA0) calls
// EngineStepBegin before the steps run and EngineStepEnd after. That is the point where the
// previous step has finished and the next has not started, the same thread and boundary
// the mission scripts' own spawns use. Nothing game-side ever runs on the UI thread.
//
// Runners. Each script (exec, --edf_exec, a typed line with a wait) is a runner: a queue of
// commands, run in order, that a "wait" pauses for a number of simulation ticks. Typed
// engine commands share one "console" runner, so they run in the order typed.
//
// Clock. Every executed command is logged with the simulation tick (SimulationTicks(), the
// engine heartbeat's count) and, with a scripted pad, the pad's game-clock tick, so a run is
// reproducible from its log. "at <time> cmd" schedules on the scripted-pad clock: the same
// clock "clock game" input scripts use (ticks since the pad's first poll).
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include "console_logic.h"

struct PPCContext;
#if defined(EDF2027_BUILD_MULTIPLAYER)
namespace edf::net {
struct InputCommand;
struct WorldSnapshot;
struct ActorSnapshot;
struct ActorState;
struct ActorSpawnRequest;
struct ActorDamageRequest;
struct MissionActorIdentitySnapshot;
struct MissionActorStateSnapshot;
struct MissionActorRemovalSnapshot;
struct MissionActorDamageRequest;
}
#endif

namespace edf::console {

// Where a command is running, for handlers (Invocation::host()).
struct Runner;
struct EngineFrame {
  PPCContext* ctx = nullptr;
  uint8_t* base = nullptr;
  uint64_t tick = 0;  // SimulationTicks() at this step
};
struct ExecContext {
  bool engine = false;           // on the engine thread, inside EngineStepBegin
  EngineFrame* frame = nullptr;  // engine only
  Runner* runner = nullptr;      // the runner executing this command, if any
  std::string source;            // "console", "exec:<file>", "edf_exec", "at"
};

struct Runner {
  std::string source;
  std::deque<std::string> queue;
  uint64_t wait_until = 0;          // absolute simulation tick
  bool wait_mission = false;
  uint64_t mission_deadline = 0;    // absolute tick; 0 = no timeout
  uint32_t mission_settle = 0;      // ticks to wait after the mission is detected
  uint32_t execs = 0;               // nested exec count (loop guard)
};

struct OutputLine {
  Severity severity;
  std::string text;
};

struct Status {
  uint64_t tick = 0;
  bool engine_seen = false;  // the engine has pumped at least once
  bool in_mission = false;
  bool cheats = false;
  size_t queued = 0;         // commands waiting in runners
  size_t runners = 0;
};

class Service {
 public:
  static Service& Get();

  // Registers every command (built-in and game). Called once, on the UI thread, before
  // the dialog opens. `quit` asks the app to close (UI thread).
  struct Hooks {
    std::function<void()> quit;
    std::function<void()> clear_view;
    // Show (1), hide (0) or toggle (-1) the console window, from any thread (the app posts
    // it to the UI thread): lets an unattended script show the console for a screenshot.
    std::function<void(int)> show;
  };
  void Initialize(Hooks hooks);
  bool initialized() const { return initialized_; }

  // UI thread (or any non-engine thread): run a typed line.
  void Submit(std::string_view line, std::string_view source = "console");
  // Queue a script file as a new runner. Returns false (and prints why) if unreadable.
  bool ExecFile(std::string_view path, std::string_view source);

  Completion Complete(std::string_view line);
  History& history() { return history_; }

  // Output. The dialog copies lines newer than its last generation.
  void Print(Severity severity, std::string text);
  uint64_t CopyOutput(uint64_t since, std::vector<OutputLine>& out, size_t limit = 4000);
  void ClearOutput();
  Status status();

  // Engine thread (frame.cpp's step dispatcher hook).
  void EngineStepBegin(PPCContext& ctx, uint8_t* base, uint32_t steps);
  void EngineStepEnd();

  // Game-state helpers for commands (console_game.cpp), engine thread only.
  void MarkCheat(std::string_view command);
  bool cheats_used() const { return cheats_.load(std::memory_order_relaxed); }
  uint64_t ScriptTick(uint64_t simulation_tick) const;
  // Step timing for "stats" (engine thread writes, anyone reads).
  struct StepTiming {
    std::atomic<double> dispatch_ms_avg{0}, dispatch_ms_max{0}, steps_per_second{0};
    std::atomic<uint64_t> dispatches{0};
  };
  StepTiming& timing() { return timing_; }

  // Registry access for command implementations (under the service lock).
  template <typename F> auto WithRegistry(F&& f) { std::lock_guard lock(mutex_); return f(registry_); }

 private:
  Service() = default;
  void RegisterBuiltins();
  // Runs one command. `ctx` says which thread and runner; engine commands on the UI thread
  // are forwarded to the console runner.
  void Execute(const std::string& text, ExecContext& ctx, int depth = 0);
  bool NeedsEngine(const std::string& line, int depth = 0);
  void PumpRunners(ExecContext& base_ctx);
  void Log(const ExecContext& ctx, const std::string& text);
  CvarAccess& Cvars();

  std::mutex mutex_;           // registry, runners, at-queue
  std::mutex output_mutex_;
  Registry registry_;
  History history_;
  std::deque<std::unique_ptr<Runner>> runners_;
  std::multimap<uint64_t, std::pair<std::string, std::string>> at_;  // script tick -> (command, source)
  std::deque<OutputLine> output_;
  uint64_t output_generation_ = 0;
  Hooks hooks_;
  bool initialized_ = false;
  std::atomic<bool> cheats_{false};
  std::atomic<uint64_t> tick_{0};
  std::atomic<bool> engine_seen_{false};
  std::atomic<bool> in_mission_{false};
  std::thread::id engine_thread_{};
  StepTiming timing_;
  // EngineStepBegin/End bookkeeping.
  std::chrono::steady_clock::time_point step_begin_{}, rate_window_{};
  uint64_t rate_steps_ = 0;
  double window_max_ms_ = 0;
  bool was_in_mission_ = false;
};

// Tick functions the game layer adds to every engine step (god mode, infinite ammo, spread
// spawns). Registered by console_game.cpp.
using EngineTick = std::function<void(EngineFrame&)>;
void AddEngineTick(EngineTick tick);
// console_game.cpp: registers the game commands and reports whether a mission is running.
void RegisterGameCommands(Registry& registry);
// True while EDF's mission world is loaded, including the interval where the
// primary soldier is dead but another local co-op seat can still be active.
bool MissionWorldActive(EngineFrame& frame);
bool GameInMission(EngineFrame& frame);
#if defined(EDF2027_BUILD_MULTIPLAYER)
struct TrustedGuestPose {
  bool available = false;
  int32_t x_cm = 0;
  int32_t y_cm = 0;
  int32_t z_cm = 0;
  int16_t yaw_mrad = 0;
  bool health_valid = false;
  bool alive = false;
  uint16_t health = 0;
  uint16_t max_health = 0;
};
struct TrustedGuestPositionReset {
  uint16_t player_id = 0;
  uint32_t reset_id = 0;
  int32_t x_cm = 0;
  int32_t y_cm = 0;
  int32_t z_cm = 0;
  int16_t yaw_mrad = 0;
};
struct TrustedGuestPositionResetAck {
  uint16_t player_id = 0;
  uint32_t reset_id = 0;
};
// Creates and validates EDF's native second player object for F5's remote seat.
// Returns true only while a port-owned seat is available to receive network input.
bool SyncLocalHostGuestSeat(EngineFrame& frame, bool remote_active,
                            uint16_t remote_player_id, uint64_t connection_generation);
TrustedGuestPose SampleGuestPlayerPose(EngineFrame& frame, uint32_t player_index);
void MultiplayerEngineTick(EngineFrame& frame, bool connected, uint16_t local_player_id,
                           bool server_authoritative_movement,
                           edf::net::InputCommand& input, const edf::net::WorldSnapshot* snapshot,
                           const edf::net::ActorSnapshot* actor_snapshot,
                           TrustedGuestPose* trusted_guest_pose = nullptr,
                           uint16_t host_guest_player_id = 0,
                           const TrustedGuestPositionReset* host_guest_reset = nullptr,
                           TrustedGuestPositionResetAck* host_guest_reset_ack = nullptr);
// Diagnostic only: compare host ant samples with local mission ants without
// creating, removing, or mutating guest objects.
void TraceMissionActorShadowMatches(EngineFrame& frame, const edf::net::ActorSnapshot* snapshot);
// Online spawn-key correlation is observational only. Local trace generations
// are process-local and are never treated as shared mission identity.
void TraceMissionActorIdentityMatches(
    EngineFrame& frame, const edf::net::MissionActorIdentitySnapshot* snapshot);
// Experimental: reconcile only exact, unique local mission-ant identities.
void ReconcileMissionActorStates(
    EngineFrame& frame, const edf::net::MissionActorStateSnapshot* snapshot,
    bool host_owns_mission_simulation = false);
// Applies only host-reported removals to exact, unique local mission-ant keys.
void ReconcileMissionActorRemovals(
    EngineFrame& frame, const edf::net::MissionActorRemovalSnapshot* snapshot);
// Samples and damages exact mission ants on the game thread for an in-process
// local server. The server worker only exchanges copied ActorState records and
// spawn-key damage requests with these functions.
std::vector<edf::net::ActorState> SampleHostMissionActors(EngineFrame& frame);
size_t ApplyHostMissionActorDamage(
    EngineFrame& frame, const std::vector<edf::net::MissionActorDamageRequest>& requests);
// Creates Luau server-owned actors and applies their damage on the game thread.
// Spawn requests contain only copied coordinates and IDs; guest pointers stay local.
std::vector<uint32_t> UpdateHostServerActors(
    EngineFrame& frame, const std::vector<edf::net::ActorSpawnRequest>& spawns,
    std::vector<edf::net::ActorState>& actors_out);
size_t ApplyHostServerActorDamage(
    EngineFrame& frame, const std::vector<edf::net::ActorDamageRequest>& requests);
void ClearHostServerActors(EngineFrame& frame);
#endif

}  // namespace edf::console
