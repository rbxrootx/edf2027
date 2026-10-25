// EDF2027 - ReXGlue Recompiled Project
//
// Adds a first-run setup screen (ISO -> extracted game folder), a per-user
// config file, an F1 settings screen and display-mode handling on top of ReXApp.
#pragma once

#include <rex/rex_app.h>
#include <rex/audio/audio_system.h>
#include <rex/audio/sdl/sdl_audio_system.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
#include "console/console_dialog.h"
#include "console/console.h"
#include "diagnostics.h"
#include "kernel_tick_timer.h"
#include "launcher.h"
#if defined(EDF2027_BUILD_MULTIPLAYER)
#include "multiplayer/client_input.h"
#include "multiplayer/client_session.h"
#include "multiplayer/local_server_host.h"
#include "multiplayer/mission_actor_trace.h"
#include "multiplayer/server_browser_dialog.h"
#endif
#include "pause_menu.h"
#include "perf_overlay.h"
#include "scripted_input.h"
#include "settings_dialog.h"
#include "setup_dialog.h"
#include "version.h"
#if defined(_WIN32)
#include "native_graphics/guest_shader_bridge.h"
#include "native_graphics/native_renderer_preset.h"
#include "native_graphics/guest_mesh_watch_audit.h"
#include "native_graphics/native_preview_window.h"
#include "native_graphics/native_host_surface.h"
#include "native_graphics/native_immediate_drawer.h"
#include "native_graphics/native_backend_immediate_drawer.h"
#include "native_graphics/native_backend_host.h"
#include "native_graphics/d3d12_backend.h"
#include "native_graphics/native_ffx.h"
#include "native_graphics/native_fsr.h"
#include "shader_prep_overlay.h"
#endif
// edf_native_preview_window, edf_native_scene_backend, edf_native_untiled_scene, edf_native_mesh_watch_audit.
#include "native_graphics/bridge/native_cvars.h"

REXCVAR_DECLARE(std::string, game_data_root);
REXCVAR_DECLARE(int32_t, window_width);
REXCVAR_DECLARE(int32_t, window_height);
REXCVAR_DECLARE(bool, edf_native_host);
REXCVAR_DECLARE(bool, audio_mute);
REXCVAR_DECLARE(bool, edf_menu_pause);
REXCVAR_DECLARE(bool, edf_menu_mute_audio);
REXCVAR_DECLARE(bool, edf_menu_pause_audio_engine);
REXCVAR_DECLARE(std::string, hid_mappings_file);
REXCVAR_DECLARE(bool, edf_console_pause);
#if defined(EDF2027_BUILD_MULTIPLAYER)
REXCVAR_DECLARE(bool, edf_mp_actor_trace);
#endif

namespace edf {

// Suspends and resumes the SDK audio engine (AudioSystem::Pause/Resume) for the F1
// pause, on a thread of its own: Pause waits for the audio worker to acknowledge, and
// the UI thread must never wait on anything the game owns. Requests run in order, so a
// resume always follows the pause it undoes. Only used with edf_menu_pause_audio_engine.
class AudioSuspender {
 public:
  ~AudioSuspender() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    wake_.notify_all();
    // A Pause that never returns must not hang the exit.
    if (worker_.joinable()) worker_.detach();
  }
  void Request(rex::audio::AudioSystem* audio, bool suspend) {
    if (!audio) return;
    {
      std::lock_guard lock(mutex_);
      if (suspend == suspended_) return;
      suspended_ = suspend;
      queue_.push_back({audio, suspend});
      if (!worker_.joinable()) worker_ = std::thread([this] { Run(); });
    }
    wake_.notify_all();
  }
  bool suspended() const { std::lock_guard lock(mutex_); return suspended_; }

 private:
  struct Job { rex::audio::AudioSystem* audio; bool suspend; };
  void Run() {
    std::unique_lock lock(mutex_);
    while (true) {
      wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (queue_.empty()) return;
      const Job job = queue_.front();
      queue_.pop_front();
      lock.unlock();
      if (job.suspend) job.audio->Pause();
      else job.audio->Resume();
      lock.lock();
    }
  }
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Job> queue_;
  std::thread worker_;
  bool suspended_ = false, stopping_ = false;
};
}  // namespace edf

class Edf2017App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    // First thing the game itself runs: from here on a crash leaves a dump and says where.
    edf::diag::InstallCrashHandler();
    return std::unique_ptr<Edf2017App>(new Edf2017App(ctx, "edf2027", PPCImageConfig));
  }

  // Runs before the config file is loaded: pick a per-user config path and make
  // "windowed" the default display mode (the SDK defaults to fullscreen).
  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (!rex::cvar::HasNonDefaultValue("user_data_root")) {
      auto pref = edf::PrefPath();
      if (!pref.empty()) {
        paths.user_data_root = pref;
        if (!rex::cvar::HasNonDefaultValue("cache_root")) paths.cache_root = pref / "cache";
      }
    }
    paths.config_path = paths.user_data_root / "edf2027.toml";
    user_data_root_ = paths.user_data_root;
    config_path_ = paths.config_path;
    // Before the SDK opens the log: <exe>/logs, or <user data>/logs when the game folder
    // is read-only; old runs pruned (diagnostics.h).
    edf::diag::PrepareLogFile(paths.user_data_root);
    if (!rex::cvar::HasNonDefaultValue("fullscreen")) rex::cvar::SetFlagByName("fullscreen", "false");
    rex::cvar::SetFlagByName("present_letterbox", REXCVAR_GET(edf_aspect) == "stretch" ? "false" : "true");
    // The guest is always told a 16:9 video mode (core_logic.h GuestVideoMode); a
    // config saved by an older build may still hold the window's own shape.
    if (REXCVAR_GET(window_width) > 0 &&
        (edf::ForcesConsoleAspect(REXCVAR_GET(edf_aspect)) ||
         !edf::native::IsConsoleAspect(REXCVAR_GET(window_width), REXCVAR_GET(window_height))))
      edf::SettingsDialog::ApplyVideoMode(REXCVAR_GET(window_width), REXCVAR_GET(window_height));
    edf::ApplyControllerDbDefault();  // before the input backend loads gamecontrollerdb.txt
  }

  // The log is open (and the config loaded): start it with what a bug report needs.
  void OnPostInitLogging() override { edf::diag::LogStartupReport(user_data_root_, config_path_); }

  // Setup failures that would otherwise end the process with only a log line (or, for
  // an exception, a crash report about the wrong thing) get a message box naming the log.
  bool SetupPresentation() override {
    try {
      if (rex::ReXApp::SetupPresentation()) return true;
      edf::diag::ReportFatalError("EDF2027 could not start",
                                  "The game window or the renderer could not be created." + RendererAdvice());
    } catch (const std::exception& error) {
      edf::diag::ReportFatalError("EDF2027 could not start",
                                  std::string("The game window or the renderer could not be created:\n\n") +
                                      error.what() + RendererAdvice());
    }
    return false;
  }
  bool ConstructRuntime(const rex::PathConfig& paths) override {
    try {
      if (rex::ReXApp::ConstructRuntime(paths)) return true;
      edf::diag::ReportFatalError("EDF2027 could not start",
                                  "The game could not be started from:\n" + paths.game_data_root.string() +
                                      "\n\nThe log says why. If the folder is wrong, start the game with --settings "
                                      "and choose the extracted game folder again.");
    } catch (const std::exception& error) {
      edf::diag::ReportFatalError("EDF2027 could not start", std::string("The game could not be started:\n\n") + error.what());
    }
    return false;
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    // This port has no GPU-emulation fallback. Migrate old saved settings too,
    // before SDK setup and immediate-drawer creation.
    config.gpu_plugin.clear();
#if defined(_WIN32)
    if(!REXCVAR_GET(edf_native_host))
      REXLOG_INFO("EDF2027: ignoring legacy edf_native_host=false; the port uses native rendering");
    rex::cvar::SetFlagByName("edf_native_host","true");
    if(!REXCVAR_GET(edf_native_untiled_scene))
      REXLOG_INFO("EDF2027: ignoring legacy edf_native_untiled_scene=false; Xbox tile recording is not supported");
    rex::cvar::SetFlagByName("edf_native_untiled_scene","true");
    if(REXCVAR_GET(edf_native_preview_window)) throw std::runtime_error("native host and preview modes are mutually exclusive");
    rex::cvar::SetFlagByName("edf_native_shader_bridge","true");
    rex::cvar::SetFlagByName("edf_native_publish_frames","true");
    edf::native::ResolveNativeRendererPreset();  // once, after the migrations above; logs the effective flags
#endif
    config.audio_factory = REX_AUDIO_BACKEND(rex::audio::sdl::SDLAudioSystem);
    config.input_factory = REX_INPUT_BACKEND(CreateEdfInputSystem);  // SDL + NOP + optional scripted pad (EDF_INPUT_SCRIPT)
  }

  // The F1 menu's fonts (menu_ui.h), baked next to the SDK's default font.
  void OnConfigureFonts(ImFontAtlas* atlas) override {
    edf::ui::LoadMenuFonts(atlas);
    edf::console::LoadConsoleFonts(atlas);  // the console's monospace face (console_dialog.h)
  }

  std::unique_ptr<rex::ui::ImmediateDrawer> OnCreateImmediateDrawer() override {
#if defined(_WIN32)
    if(EDF_NATIVE_FLAG(host)) {
      // The render size follows the window's shape (native_display_layout.h); it is
      // resolved when the engine initializes its renderer, after the display mode
      // (windowed or borderless) has sized the window.
      edf::native::SetNativeDisplaySizeProvider(
          [window=static_cast<HWND>(window()->GetNativeWindowHandle())]() -> std::array<int32_t, 2> {
            RECT rect{};
            if (!IsWindow(window) || IsIconic(window) || !GetClientRect(window, &rect)) return {0, 0};
            return {int32_t(rect.right - rect.left), int32_t(rect.bottom - rect.top)};
          });
      edf::native::InitializeGuestShaderBridge({});
      // The presenting host stops on a lost GPU or a device it cannot create; say so and
      // close instead of leaving a frozen window.
      edf::native::NativeBackendHost::SetFailureHandler([this, context = &app_context()](const std::string& error) {
        context->CallInUIThreadDeferred([this, error] {
          edf::diag::ReportFatalError("EDF2027 - the renderer stopped",
                                      "The renderer stopped because of an error:\n\n" + error + RendererAdvice() +
                                          "\n\nThe game will now close.");
          if (window()) window()->RequestClose();
        });
      });
      if(REXCVAR_GET(edf_native_scene_backend).starts_with("d3d12")) {
        edf::native::RegisterNativeD3D12Backend();
        auto backend=std::shared_ptr<edf::native::NativeRenderBackend>(
          edf::native::CreateNativeRenderBackend(REXCVAR_GET(edf_native_scene_backend)));
        auto drawer=std::make_unique<edf::native::NativeBackendImmediateDrawer>(backend);
        native_backend_immediate_=drawer.get();
        native_backend_host_=edf::native::NativeBackendHost::Create(
          static_cast<HWND>(window()->GetNativeWindowHandle()),backend,
          [this](edf::native::NativeBackendRenderTarget& target) {
            if(!imgui_drawer()) return;
            native_backend_immediate_->SetTarget(&target);
            rex::ui::AppUIDrawContext context(target.width(),target.height());
            try { imgui_drawer()->Draw(context); }
            catch(...) {
              if(native_backend_immediate_) { native_backend_immediate_->End(); native_backend_immediate_->SetTarget(nullptr); }
              throw;
            }
            if(native_backend_immediate_) native_backend_immediate_->SetTarget(nullptr);
          },[context=&app_context()](std::function<void()> callback) {
            return context->CallInUIThreadDeferred(std::move(callback));
          });
        return drawer;
      }
      std::unique_ptr<edf::native::NativeImmediateDrawer> drawer;
      edf::native::VisitNativePresentationContext([&](auto& device,auto& context) {
        drawer=std::make_unique<edf::native::NativeImmediateDrawer>(device,context);
      });
      if(!drawer) throw std::runtime_error("native host has no rendering context");
      native_immediate_=drawer.get();
      native_host_=edf::native::NativeHostSurface::Create(
        static_cast<HWND>(window()->GetNativeWindowHandle()),[this](UINT width,UINT height) {
          if(!imgui_drawer()) return;
          rex::ui::AppUIDrawContext context(width,height);
          try { imgui_drawer()->Draw(context); }
          catch(...) { if(native_immediate_) native_immediate_->End(); throw; }
        },[context=&app_context()](std::function<void()> callback) {
          return context->CallInUIThreadDeferred(std::move(callback));
        });
      return drawer;
    }
#endif
    return nullptr;
  }

  void OnPostSetup() override {
    edf::StartKernelTickTimer(runtime());  // before the guest runs (kernel_tick_timer.cpp)
#if defined(_WIN32)
    edf::native::InitializeGuestShaderBridge(runtime()->game_data_root());
    if(REXCVAR_GET(edf_native_mesh_watch_audit)) {
      native_mesh_audit_=std::make_shared<edf::native::GuestMeshWatchAudit>(*runtime()->memory());
      edf::native::SetNativeMeshWatchAudit(native_mesh_audit_);
    }
    if(REXCVAR_GET(edf_native_preview_window))
      native_preview_=std::make_unique<edf::native::NativePreviewWindow>();
#endif
    // ReXApp's default mouse-look gate only knows about its built-in dialogs.
    // EDF's setup and F1 settings are custom dialogs, so use ImGui's capture
    // state directly, and the F1 menu's own state: while it is open the SDK's
    // drivers report an untouched pad and the native K/M driver lets go of the
    // mouse (pause_menu.h has the whole routing). The performance overlay uses
    // NoInputs, so it does not interrupt mouse-look.
    auto* input_system = static_cast<rex::input::InputSystem*>(runtime()->input_system());
    if (input_system && imgui_drawer()) {
      input_system->SetActiveCallback(
          [this]() { return !edf::menu::MenuOpen() && !imgui_drawer()->GetIO().WantCaptureMouse; });
    }
    booted_ = true;
    edf::diag::RunCrashTest();  // edf_crash_test, off by default
  }

  // Runs after config load and window creation, before the runtime boots.
  std::optional<rex::PathConfig> OnFinalizePaths(const rex::PathConfig& defaults,
                                                 std::function<void(rex::PathConfig)> resume) override {
    config_path_ = defaults.config_path;
    ApplyDisplayMode();
    edf::ApplyKeyboardDefaults();

    // gamecontrollerdb.txt is found beside the exe at startup, and the saved config keeps
    // that absolute path: after the game folder moves, look beside the exe again.
    if (const std::string db = REXCVAR_GET(hid_mappings_file); !db.empty() && !std::filesystem::exists(db)) {
      rex::cvar::ResetToDefault("hid_mappings_file");
      edf::ApplyControllerDbDefault();
      REXLOG_INFO("EDF2027: controller database {} is missing; using {}", db, REXCVAR_GET(hid_mappings_file));
    }

    rex::PathConfig paths = defaults;
    // Priority: --game_data_root on the command line, then the saved edf_game_path,
    // then the per-user extraction folder.
    std::filesystem::path candidates[] = {
        std::filesystem::path(REXCVAR_GET(game_data_root)),
        std::filesystem::path(REXCVAR_GET(edf_game_path)),
        edf::GameDir(defaults.user_data_root)};
    for (auto& c : candidates) {
      if (!c.empty() && edf::HasXex(c)) {
        paths.game_data_root = c;
        break;
      }
    }

    if (!paths.game_data_root.empty() && !ShowSettingsRequested()) {
      REXLOG_INFO("EDF2027: game folder {}", paths.game_data_root.string());
      return paths;
    }
    if (!paths.game_data_root.empty()) {
      // --settings: show the settings screen first, boot when it closes.
      pending_resume_ = [resume, paths]() { resume(paths); };
      OpenSettings();
      return std::nullopt;
    }

    // No usable game folder: first-run setup, boot when it completes.
    auto* dlg = new edf::SetupDialog(
        imgui_drawer(), edf::GameDir(defaults.user_data_root),
        [this, resume, paths](const std::filesystem::path& dir) mutable {
          paths.game_data_root = dir;
          rex::cvar::SetFlagByName("edf_game_path", dir.generic_string());  // forward slashes: TOML strings treat backslash as escape
          rex::cvar::SetFlagByName("edf_setup_done", "true");
          SaveConfig();
          REXLOG_INFO("EDF2027: setup complete, game folder {}", dir.string());
          resume(paths);
        },
        [this]() { window()->RequestClose(); });
    imgui_drawer()->AddDialog(dlg);
    return std::nullopt;
  }

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    // The SDK titles the window "<name> <SDK build stamp>"; show only the game's name.
    if (window()) window()->SetTitle("EDF2027");
    auto* overlay = new edf::PerfOverlay(drawer, [this] { return PhysicalHeight(); });
    edf::PerfOverlay::Current() = overlay;
#if defined(_WIN32)
    new edf::ShaderPrepOverlay(drawer, [this] { return PhysicalHeight(); });
#endif
    rex::ui::RegisterBind("bind_edf_settings", "F1", "EDF2027 settings", [this]() { ToggleSettings(); });
    rex::ui::RegisterBind("bind_edf_fps", "F2", "Toggle performance overlay", []() {
      rex::cvar::SetFlagByName("edf_show_fps", REXCVAR_GET(edf_show_fps) ? "false" : "true");
    });
#if defined(EDF2027_BUILD_MULTIPLAYER)
    rex::ui::RegisterBind("bind_edf_servers", "F5", "Open server browser", [this]() { OpenServerBrowser(); });
    const auto host_bridge = edf::GetLocalHostEngineBridge();
    {
      std::lock_guard bridge_lock(host_bridge->mutex);
      host_bridge->host = &local_server_host_;
    }
    static std::once_flag network_tick;
    std::call_once(network_tick, [host_bridge] {
      static uint64_t snapshot_generation = 0;
      static uint64_t actor_snapshot_generation = 0;
      static uint64_t mission_actor_diagnostic_generation = 0;
      static uint64_t mission_actor_identity_generation = 0;
      static uint64_t mission_actor_state_generation = 0;
      static uint64_t mission_actor_removal_generation = 0;
      static uint64_t local_host_run_generation = 0;
      static uint64_t connection_epoch = 0;
      static uint16_t local_player_id = 0;
      static uint32_t last_mission_actor_diagnostic_log_tick = 0;
      static std::vector<uint32_t> last_mission_actor_diagnostic_ids;
      static edf::net::WorldSnapshot latest_snapshot;
      static edf::net::ActorSnapshot latest_actor_snapshot;
      static edf::net::ActorSnapshot latest_mission_actor_diagnostic_snapshot;
      static edf::net::MissionActorIdentitySnapshot latest_mission_actor_identity_snapshot;
      static edf::net::MissionActorStateSnapshot latest_mission_actor_state_snapshot;
      static edf::net::MissionActorRemovalSnapshot latest_mission_actor_removal_snapshot;
      edf::console::AddEngineTick([host_bridge, generation = &snapshot_generation, local_id = &local_player_id,
                                   session_epoch = &connection_epoch,
                                   snapshot = &latest_snapshot, actor_generation = &actor_snapshot_generation,
                                   actor_snapshot = &latest_actor_snapshot,
                                   diagnostic_generation = &mission_actor_diagnostic_generation,
                                   diagnostic_log_tick = &last_mission_actor_diagnostic_log_tick,
                                   diagnostic_ids = &last_mission_actor_diagnostic_ids,
                                   diagnostic_snapshot = &latest_mission_actor_diagnostic_snapshot,
                                   identity_generation = &mission_actor_identity_generation,
                                   identity_snapshot = &latest_mission_actor_identity_snapshot,
                                   state_generation = &mission_actor_state_generation,
                                   state_snapshot = &latest_mission_actor_state_snapshot,
                                   removal_generation = &mission_actor_removal_generation,
                                   removal_snapshot = &latest_mission_actor_removal_snapshot,
                                   host_run_generation = &local_host_run_generation](edf::console::EngineFrame& frame) {
        std::lock_guard bridge_lock(host_bridge->mutex);
        auto* local_host = host_bridge->host;
        auto& session = edf::net::ClientSession::Global();
        uint64_t current_epoch = 0;
        const bool connected = session.connected(current_epoch);
        const bool local_host_running = local_host && local_host->running();
        const uint64_t current_host_generation = local_host ? local_host->run_generation() : 0;
        const bool network_mission_active = connected || local_host_running;
        if (local_host) local_host->SetLocalPlayerId(connected ? session.local_player_id() : 0);
        const edf::net::RemoteGuestIdentity remote_guest_identity = local_host
            ? local_host->PendingRemoteGuestIdentity() : edf::net::RemoteGuestIdentity{};
        const bool remote_guest_seat_ready = edf::console::SyncLocalHostGuestSeat(
            frame, local_host_running && remote_guest_identity.player_id != 0,
            remote_guest_identity.player_id, remote_guest_identity.connection_generation);
        if (local_host) {
          local_host->SetRemoteGuestSeatReady(remote_guest_identity, remote_guest_seat_ready);
        }
        const uint16_t host_guest_player_id =
            local_host_running ? local_host->RemoteGuestPlayerId() : 0;
        if (!network_mission_active &&
            edf::multiplayer::MissionActorNetworkReplicationActive()) {
          // Restore any local replica flags while the trace keys are still
          // available, then stop collecting them for ordinary single-player.
          edf::console::ReconcileMissionActorStates(frame, nullptr, true);
        }
        edf::multiplayer::SetMissionActorNetworkReplicationActive(network_mission_active);
        if (local_host_running) {
          if (*host_run_generation != current_host_generation) {
            // A host restart can reuse server actor IDs while the game mission
            // remains loaded. Retire the previous run's native actor map first.
            edf::console::ClearHostServerActors(frame);
            *host_run_generation = current_host_generation;
          }
          // Mission trace state may remain nonzero after leaving a mission.
          // Do not let Luau reserve actor IDs while the menu/loading scene is
          // active: the native game-thread bridge cannot create those actors.
          const uint64_t trace_generation = edf::console::MissionWorldActive(frame)
              ? edf::multiplayer::CurrentMissionAntTraceGeneration() : 0;
          local_host->SetServerActorWorldGeneration(trace_generation);
          local_host->PublishMissionActors(
              trace_generation, edf::console::SampleHostMissionActors(frame));
          const auto spawn_requests = local_host->TakeServerActorSpawnRequests();
          std::vector<edf::net::ActorState> server_actors;
          const auto spawned_ids = edf::console::UpdateHostServerActors(
              frame, spawn_requests, server_actors);
          local_host->PublishServerActors(trace_generation, std::move(server_actors));
          local_host->CompleteServerActorSpawns(spawn_requests, spawned_ids);
          const auto server_damage_requests = local_host->TakeServerActorDamageRequests();
          (void)edf::console::ApplyHostServerActorDamage(frame, server_damage_requests);
          const auto damage_requests = local_host->TakeMissionActorDamageRequests();
          (void)edf::console::ApplyHostMissionActorDamage(frame, damage_requests);
        } else {
          edf::console::ClearHostServerActors(frame);
          *host_run_generation = 0;
        }
        if (current_epoch != *session_epoch) {
          *session_epoch = current_epoch;
          *snapshot = {};
          *actor_snapshot = {};
          *diagnostic_snapshot = {};
          *identity_snapshot = {};
          *state_snapshot = {};
          *removal_snapshot = {};
          *generation = 0;
          *actor_generation = 0;
          *diagnostic_generation = 0;
          *identity_generation = 0;
          *state_generation = 0;
          *removal_generation = 0;
          *diagnostic_log_tick = 0;
          diagnostic_ids->clear();
          *local_id = 0;
          edf::console::TraceMissionActorShadowMatches(frame, nullptr);
          if (connected) {
            // A reconnect may complete between engine ticks. Run the normal
            // disconnect cleanup before using any state from the new server.
            edf::net::InputCommand cleanup;
            edf::console::MultiplayerEngineTick(frame, false, 0, false, cleanup, nullptr, nullptr);
          }
        }
        edf::console::TrustedGuestPositionResetAck host_guest_reset_ack;
        if (connected) {
          auto input = edf::net::PublishedLocalInput();
          (void)session.CopySnapshotIfNew(*generation, *snapshot, *local_id);
          (void)session.CopyActorSnapshotIfNew(*actor_generation, *actor_snapshot);
          const bool got_diagnostic_snapshot = session.CopyMissionActorDiagnosticSnapshotIfNew(
              *diagnostic_generation, *diagnostic_snapshot);
          const bool got_identity_snapshot = session.CopyMissionActorIdentitySnapshotIfNew(
              *identity_generation, *identity_snapshot);
          (void)session.CopyMissionActorStateSnapshotIfNew(
              *state_generation, *state_snapshot);
          (void)session.CopyMissionActorRemovalSnapshotIfNew(
              *removal_generation, *removal_snapshot);
          edf::console::TrustedGuestPose host_guest_pose;
          edf::console::TrustedGuestPositionReset host_guest_reset;
          std::optional<edf::net::AuthoritativePositionReset> pending_host_guest_reset;
          if (local_host_running) {
            pending_host_guest_reset = local_host->PendingRemoteGuestReset();
            if (pending_host_guest_reset) {
              host_guest_reset = {pending_host_guest_reset->player_id,
                  pending_host_guest_reset->reset_id, pending_host_guest_reset->x_cm,
                  pending_host_guest_reset->y_cm, pending_host_guest_reset->z_cm,
                  pending_host_guest_reset->yaw_mrad};
            }
          }
          const bool actor_trace_enabled = REXCVAR_GET(edf_mp_actor_trace);
          if (got_diagnostic_snapshot && actor_trace_enabled) {
            std::vector<uint32_t> current_ids;
            current_ids.reserve(diagnostic_snapshot->actors.size());
            for (const auto& actor : diagnostic_snapshot->actors) current_ids.push_back(actor.id);
            std::sort(current_ids.begin(), current_ids.end());
            const bool actor_set_changed = current_ids != *diagnostic_ids;
            if (actor_set_changed || *diagnostic_log_tick == 0 ||
                static_cast<int32_t>(diagnostic_snapshot->tick - *diagnostic_log_tick) >= 60) {
              *diagnostic_ids = std::move(current_ids);
              *diagnostic_log_tick = diagnostic_snapshot->tick;
              REXLOG_INFO("Mission actor network trace: side=client_remote tick={} count={}",
                          diagnostic_snapshot->tick, diagnostic_snapshot->actors.size());
              for (const auto& actor : diagnostic_snapshot->actors)
                REXLOG_INFO("Mission actor network trace: side=client_remote tick={} id={} health={} x_cm={} y_cm={} z_cm={} yaw_mrad={}",
                            diagnostic_snapshot->tick, actor.id, actor.health, actor.x_cm, actor.y_cm,
                            actor.z_cm, actor.yaw_mrad);
            }
          }
          if (!actor_trace_enabled || !edf::console::MissionWorldActive(frame)) {
            edf::console::TraceMissionActorShadowMatches(frame, nullptr);
          } else if (got_diagnostic_snapshot) {
            edf::console::TraceMissionActorShadowMatches(
                frame, diagnostic_snapshot->tick ? diagnostic_snapshot : nullptr);
          }
          if (!actor_trace_enabled || !edf::console::MissionWorldActive(frame)) {
            edf::console::TraceMissionActorIdentityMatches(frame, nullptr);
          } else if (got_identity_snapshot && identity_snapshot->host_trace_generation) {
            edf::console::TraceMissionActorIdentityMatches(frame, identity_snapshot);
          } else if (got_identity_snapshot) {
            edf::console::TraceMissionActorIdentityMatches(frame, nullptr);
          }
          edf::console::ReconcileMissionActorStates(
              frame, state_snapshot->tick ? state_snapshot : nullptr,
              local_host_running);
          edf::console::ReconcileMissionActorRemovals(
              frame, removal_snapshot->tick ? removal_snapshot : nullptr);
          edf::console::MultiplayerEngineTick(frame, true, *local_id,
              session.server_authoritative_movement(), input,
              snapshot->tick ? snapshot : nullptr,
              !local_host_running && actor_snapshot->tick ? actor_snapshot : nullptr,
              local_host_running ? &host_guest_pose : nullptr, host_guest_player_id,
              pending_host_guest_reset ? &host_guest_reset : nullptr, &host_guest_reset_ack);
          session.SetInput(input);
          if (local_host_running && *local_id) {
            edf::net::AuthoritativePlayerTransform host_transform;
            host_transform.player_id = *local_id;
            host_transform.available = input.gameplay_active && host_guest_pose.available;
            host_transform.acknowledged_reset_player_id = input.position_reset_ack ? *local_id : 0;
            host_transform.acknowledged_reset_id = input.position_reset_ack;
            host_transform.x_cm = host_guest_pose.available ? host_guest_pose.x_cm : 0;
            host_transform.y_cm = host_guest_pose.available ? host_guest_pose.y_cm : 0;
            host_transform.z_cm = host_guest_pose.available ? host_guest_pose.z_cm : 0;
            host_transform.yaw_mrad = host_guest_pose.yaw_mrad;
            host_transform.health_valid = host_guest_pose.health_valid;
            host_transform.alive = host_guest_pose.alive;
            host_transform.health = host_guest_pose.health;
            host_transform.max_health = host_guest_pose.max_health;
            local_host->PublishHostTransform(host_transform);
          } else if (local_host_running) {
            local_host->ClearHostTransform();
          }
        } else {
          if (local_host_running) local_host->ClearHostTransform();
          *snapshot = {};
          *actor_snapshot = {};
          *diagnostic_snapshot = {};
          *identity_snapshot = {};
          *state_snapshot = {};
          *removal_snapshot = {};
          diagnostic_ids->clear();
          *local_id = 0;
          edf::console::TraceMissionActorShadowMatches(frame, nullptr);
          edf::console::TraceMissionActorIdentityMatches(frame, nullptr);
          edf::console::ReconcileMissionActorStates(frame, nullptr);
          edf::console::ReconcileMissionActorRemovals(frame, nullptr);
          edf::net::InputCommand input;
          edf::console::MultiplayerEngineTick(frame, false, 0, false, input, nullptr, nullptr);
        }
        if (local_host_running) {
          const uint16_t remote_player_id = local_host->RemoteGuestPlayerId();
          if (remote_player_id) {
            const auto guest_pose = edf::console::SampleGuestPlayerPose(frame, 1);
            edf::net::AuthoritativePlayerTransform remote_transform;
            remote_transform.player_id = remote_player_id;
            remote_transform.available = guest_pose.available;
            remote_transform.x_cm = guest_pose.x_cm;
            remote_transform.y_cm = guest_pose.y_cm;
            remote_transform.z_cm = guest_pose.z_cm;
            remote_transform.yaw_mrad = guest_pose.yaw_mrad;
            remote_transform.health_valid = guest_pose.health_valid;
            remote_transform.alive = guest_pose.alive;
            remote_transform.health = guest_pose.health;
            remote_transform.max_health = guest_pose.max_health;
            if (host_guest_reset_ack.player_id == remote_player_id) {
              remote_transform.acknowledged_reset_player_id = host_guest_reset_ack.player_id;
              remote_transform.acknowledged_reset_id = host_guest_reset_ack.reset_id;
            }
            local_host->PublishRemoteGuestTransform(remote_transform);
          } else {
            local_host->ClearRemoteGuestTransform();
          }
        }
      });
    });
#endif
    CreateConsole(drawer);
    // The pad chord (edf_menu_pad_chord) is seen on the guest's pad poll; open the menu
    // from the UI thread.
    edf::menu::SetOpenRequestHandler([context = &app_context(), this] {
      context->CallInUIThreadDeferred([this] { OpenSettings(true); });
    });
  }

  void OnKeyDown(rex::ui::KeyEvent& event) override {
    // While the settings dialog is capturing a rebind, every key belongs to it: Escape
    // cancels the capture instead of quitting, and F1/F2 can be bound like any other key.
    if (auto* settings = edf::SettingsDialog::Current(); settings && settings->capturing()) {
      const bool cancel = event.virtual_key() == rex::ui::VirtualKey::kEscape;
      settings->FeedCapturedKey(rex::ui::VirtualKeyToString(event.virtual_key()),
                                event.is_shift_pressed(), event.is_ctrl_pressed(),
                                event.is_alt_pressed(), cancel);
      event.set_handled(true);
      return;
    }
    if (event.virtual_key() == rex::ui::VirtualKey::kEscape) {
      event.set_handled(true);
      // The console closes first (it never quits the game).
      if (auto* console = edf::console::ConsoleDialog::Current(); console && console->open()) {
        console->SetOpen(false);
        return;
      }
      // With the menu open, Escape resumes (unless ImGui has a popup or an edit to
      // cancel first); otherwise it quits, as before.
      if (auto* settings = edf::SettingsDialog::Current()) {
        if (settings->WantsEscapeToClose()) settings->Dismiss();
        return;
      }
      window()->RequestClose();
      return;
    }
    rex::ui::ProcessKeyEvent(event);
  }

  void OnShutdown() override {
    SaveConfig();
#if defined(EDF2027_BUILD_MULTIPLAYER)
    server_browser_scans_.StopAndJoin();
    edf::net::ClientSession::Global().Disconnect();
    DetachLocalServerHostFromEngine();
    local_server_host_.Stop();
    edf::multiplayer::SetMissionActorNetworkReplicationActive(false);
#endif
    ReleasePause();
    edf::StopKernelTickTimer();  // while the runtime and guest memory are still up
#if defined(_WIN32)
    edf::native::LogNativeCoverageCensusFinal();
    edf::native::SetNativeMeshWatchAudit({});
    native_mesh_audit_.reset();
    if(native_backend_host_) native_backend_host_->Stop();
    native_backend_host_.reset(); native_backend_immediate_=nullptr;
    if(native_host_) native_host_->Stop();
    native_host_.reset(); native_immediate_=nullptr;
    native_preview_.reset();
#endif
  }
  bool OnWindowCloseRequested() override {
    // The engine thread must not stay held in the heartbeat while the runtime stops.
#if defined(EDF2027_BUILD_MULTIPLAYER)
    edf::net::ClientSession::Global().Disconnect();
    DetachLocalServerHostFromEngine();
    local_server_host_.Stop();
    edf::multiplayer::SetMissionActorNetworkReplicationActive(false);
#endif
    ReleasePause();
#if defined(_WIN32)
    edf::native::LogNativeCoverageCensusFinal();
    if(native_backend_host_) native_backend_host_->Stop();
    native_backend_host_.reset(); native_backend_immediate_=nullptr;
    if(native_host_) native_host_->Stop();
    native_host_.reset(); native_immediate_=nullptr;
    native_preview_.reset();
#endif
    return true;
  }
 private:
#if defined(EDF2027_BUILD_MULTIPLAYER)
  edf::net::ServerBrowserScanCoordinator server_browser_scans_;
  edf::net::LocalServerHost local_server_host_;

  void DetachLocalServerHostFromEngine() {
    const auto bridge = edf::GetLocalHostEngineBridge();
    std::lock_guard bridge_lock(bridge->mutex);
    if (bridge->host == &local_server_host_) bridge->host = nullptr;
  }
#endif
#if defined(_WIN32)
  std::unique_ptr<edf::native::NativePreviewWindow> native_preview_;
  std::shared_ptr<edf::native::GuestMeshWatchAudit> native_mesh_audit_;
  std::shared_ptr<edf::native::NativeBackendHost> native_backend_host_;
  edf::native::NativeBackendImmediateDrawer* native_backend_immediate_=nullptr;
  std::shared_ptr<edf::native::NativeHostSurface> native_host_;
  edf::native::NativeImmediateDrawer* native_immediate_=nullptr; // Owned by SDK.
#endif
  void ApplyDisplayMode() {
    bool borderless = edf::DisplayMode() == "borderless";
    rex::cvar::SetFlagByName("fullscreen", borderless ? "true" : "false");  // change callback resizes the window
  }
  void SaveConfig() { edf::SaveUserConfig(config_path_); }
  void ToggleSettings() {
    if (auto* cur = edf::SettingsDialog::Current()) {
      cur->Dismiss();
      return;
    }
    OpenSettings();
  }
#if defined(EDF2027_BUILD_MULTIPLAYER)
  void OpenServerBrowser() {
    if (auto* current = edf::net::ServerBrowserDialog::Current()) {
      current->Dismiss();
      return;
    }
    if (!imgui_drawer() || edf::net::ServerBrowserDialog::Current()) return;
    if (auto* settings = edf::SettingsDialog::Current()) settings->Dismiss();
    if (auto* console = edf::console::ConsoleDialog::Current(); console && console->open()) console->SetOpen(false);
    if (booted_) BeginPause();
    auto* dialog = new edf::net::ServerBrowserDialog(imgui_drawer(), edf::net::ClientSession::Global(),
        server_browser_scans_, [this](std::string_view, std::string* error) {
      const auto scripts_directory = rex::filesystem::GetExecutableFolder() / "server-data" / "scripts";
      if (!local_server_host_.Start(scripts_directory, error)) return false;
      return true;
    }, [this] { return local_server_host_.running(); }, [this] {
      local_server_host_.Stop();
    }, [this] {
      SaveConfig();
      if (booted_) EndPause();
    });
    edf::net::ServerBrowserDialog::Current() = dialog;
    imgui_drawer()->AddDialog(dialog);
  }
#endif
  void OpenSettings(bool by_pad = false) {
    if (edf::SettingsDialog::Current() || !imgui_drawer()) return;
#if defined(EDF2027_BUILD_MULTIPLAYER)
    if (auto* browser = edf::net::ServerBrowserDialog::Current()) browser->Dismiss();
#endif
    // One owner of the input gate at a time: the menu replaces the console.
    if (auto* console = edf::console::ConsoleDialog::Current(); console && console->open()) console->SetOpen(false);
    BeginPause();
    edf::SettingsDialog::Hooks hooks;
    hooks.on_close = [this]() {
      EndPause();
      if (pending_resume_) {
        auto r = std::move(pending_resume_);
        pending_resume_ = nullptr;
        r();
      }
    };
    hooks.on_restart = [this]() { return RestartApplication(); };
    hooks.physical_height = [this]() { return PhysicalHeight(); };
    hooks.resize_window = [this](int width, int height) { ResizeWindow(width, height); };
    hooks.game_running = [this]() { return booted_; };
    hooks.user_mute = [this]() { return pause_.PersistentMute(REXCVAR_GET(audio_mute)); };
    hooks.set_user_mute = [this](bool mute) {
      // While the menu forces silence, this is the setting to give back on close.
      if (pause_.forcing_mute()) pause_.SetUserMute(mute);
      else rex::cvar::SetFlagByName("audio_mute", mute ? "true" : "false");
    };
    hooks.paused = [this]() { return pause_.holding() && edf::menu::Engine().holding.load(); };
    hooks.version = edf::version::Full();
#if defined(_WIN32)
    hooks.fsr_unavailable = []() -> std::string {
      const auto& library = edf::native::NativeFsrLibrary();
      return library.available() ? edf::native::NativeFsrRuntimeError() : library.error();
    };
#endif
    auto* dlg = new edf::SettingsDialog(imgui_drawer(), config_path_, std::move(hooks), by_pad);
    edf::SettingsDialog::Current() = dlg;
    imgui_drawer()->AddDialog(dlg);
  }

  // ---- Console (console/console_dialog.h, docs/console.md) ------------------------------
  // The console takes the backtick key. The SDK's own log console defaulted to it too; it
  // moves to F9 unless the player bound it elsewhere.
  void CreateConsole(rex::ui::ImGuiDrawer* drawer) {
    edf::console::Service::Get().Initialize({
        .quit = [context = &app_context(), this] {
          context->CallInUIThreadDeferred([this] { if (window()) window()->RequestClose(); });
        },
        .clear_view = {},
        .show = [context = &app_context(), this](int state) {
          context->CallInUIThreadDeferred([this, state] {
            if (edf::SettingsDialog::Current()) return;
            if (auto* current = edf::console::ConsoleDialog::Current())
              current->SetOpen(state < 0 ? !current->open() : state > 0);
          });
        }});
    auto* console = new edf::console::ConsoleDialog(
        drawer, {.on_toggle = [this](bool open) { open ? BeginConsoleCapture() : EndConsoleCapture(); },
                 .physical_height = [this] { return PhysicalHeight(); }});
    edf::console::ConsoleDialog::Current() = console;
    if (rex::cvar::GetFlagByName("bind_console") == "Backtick") {
      rex::cvar::SetFlagByName("bind_console", "F9");
      REXLOG_INFO("EDF2027: the SDK log console moved from ` to F9 (bind_console); ` opens the EDF2027 console");
    }
    rex::ui::RegisterBind("bind_edf_console", "Backtick", "EDF2027 console (docs/console.md)", [] {
      if (edf::SettingsDialog::Current()) return;  // the menu owns input while it is open
      if (auto* current = edf::console::ConsoleDialog::Current()) current->Toggle();
    });
  }
  // Like the F1 menu (BeginPause), without the mute: the input gate closes so keyboard,
  // mouse and pad stop reaching the game, the mouse is freed, and with edf_console_pause
  // the engine heartbeat holds.
  void BeginConsoleCapture() {
    edf::menu::OpenGate();
    NativeKbmDriver::ReleaseForMenu();
    if (window()) {
      cursor_before_console_ = window()->GetCursorVisibility();
      window()->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
    }
    console_paused_ = booted_ && REXCVAR_GET(edf_console_pause);
    if (console_paused_) edf::menu::Engine().requested.store(true, std::memory_order_release);
  }
  void EndConsoleCapture() {
    if (console_paused_) edf::menu::Engine().requested.store(false, std::memory_order_release);
    console_paused_ = false;
    edf::menu::CloseGate();
    if (window()) window()->SetCursorVisibility(cursor_before_console_);
  }

  // ---- F1 pause (pause_menu.h) -------------------------------------------------------
  // Opening: input goes to the menu only (gate closed, mouse capture dropped, cursor
  // shown), then the engine heartbeat is asked to hold and the game is silenced.
  void BeginPause() {
    edf::menu::OpenGate();
    NativeKbmDriver::ReleaseForMenu();
    if (window()) {
      cursor_before_menu_ = window()->GetCursorVisibility();
      window()->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
    }
    const auto effects = pause_.Open(REXCVAR_GET(edf_menu_pause), REXCVAR_GET(edf_menu_mute_audio), booted_,
                                     REXCVAR_GET(audio_mute));
    if (effects.set_mute) rex::cvar::SetFlagByName("audio_mute", *effects.set_mute ? "true" : "false");
    edf::menu::Engine().requested.store(effects.hold_engine, std::memory_order_release);
    if (effects.hold_engine && REXCVAR_GET(edf_menu_pause_audio_engine)) audio_suspender_.Request(AudioEngine(), true);
  }
  // Closing: the engine resumes (its clock rebased, no catch-up), sound comes back as the
  // player had it, and input drains back to the game (pause_menu.h MenuInputGate). The
  // mouse is captured again by the first guest poll.
  void EndPause() {
    const auto effects = pause_.Close();
    edf::menu::Engine().requested.store(false, std::memory_order_release);
    if (audio_suspender_.suspended()) audio_suspender_.Request(AudioEngine(), false);
    if (effects.set_mute) rex::cvar::SetFlagByName("audio_mute", *effects.set_mute ? "true" : "false");
    edf::menu::CloseGate();
    if (window()) window()->SetCursorVisibility(cursor_before_menu_);
  }
  void ReleasePause() {
    edf::menu::SetOpenRequestHandler({});
    edf::menu::Engine().requested.store(false, std::memory_order_release);
    if (audio_suspender_.suspended()) audio_suspender_.Request(AudioEngine(), false);
  }
  rex::audio::AudioSystem* AudioEngine() {
    return runtime() ? dynamic_cast<rex::audio::AudioSystem*>(runtime()->audio_system()) : nullptr;
  }
  float PhysicalHeight() { return window() ? float(window()->GetActualPhysicalHeight()) : 0.0f; }
  // Settings > Display > Window size, windowed mode: resize the client area now. The
  // game's render size follows only after a restart.
  void ResizeWindow(int width, int height) {
#if defined(_WIN32)
    if (!window() || width <= 0 || height <= 0) return;
    auto hwnd = static_cast<HWND>(window()->GetNativeWindowHandle());
    if (!hwnd || IsZoomed(hwnd) || IsIconic(hwnd)) return;
    RECT rect{0, 0, width, height};
    const DWORD style = DWORD(GetWindowLongPtrW(hwnd, GWL_STYLE)), ex_style = DWORD(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    AdjustWindowRectEx(&rect, style, FALSE, ex_style);
    SetWindowPos(hwnd, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
#else
    (void)width; (void)height;
#endif
  }

  bool RestartApplication() {
    const char* base = SDL_GetBasePath();
    if (!base) return false;
#if defined(_WIN32)
    const std::filesystem::path executable = std::filesystem::path(base) / "edf2027.exe";
#else
    const std::filesystem::path executable = std::filesystem::path(base) / "edf2027";
#endif
    const std::string executable_string = executable.string();
    const char* arguments[] = {executable_string.c_str(), nullptr};
    SDL_PropertiesID properties = SDL_CreateProperties();
    if (!properties) return false;
    SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER,
                           const_cast<char**>(arguments));
    SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetBooleanProperty(properties, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    SDL_Process* process = SDL_CreateProcessWithProperties(properties);
    SDL_DestroyProperties(properties);
    if (!process) return false;
    SDL_DestroyProcess(process);
    window()->RequestClose();
    return true;
  }

  // What a player should try when the renderer fails.
  static std::string RendererAdvice() {
    return "\n\nThis port needs a GPU and driver with Direct3D 12 (feature level 11_0). Update the graphics driver "
           "and try again; if the GPU was lost or hung, closing other GPU-heavy programs or lowering the frame-rate "
           "cap can help.";
  }

  std::filesystem::path config_path_;
  std::filesystem::path user_data_root_;
  std::function<void()> pending_resume_;
  bool booted_ = false;  // the runtime is up (OnPostSetup); before it, --settings has nothing to pause
  edf::menu::PauseController pause_;
  edf::AudioSuspender audio_suspender_;
  rex::ui::Window::CursorVisibility cursor_before_menu_ = rex::ui::Window::CursorVisibility::kVisible;
  rex::ui::Window::CursorVisibility cursor_before_console_ = rex::ui::Window::CursorVisibility::kVisible;
  bool console_paused_ = false;
};
