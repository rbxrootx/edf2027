#pragma once

#include "client_session.h"
#include "discovery.h"

#include <rex/ui/imgui_dialog.h>
#include <rex/cvar.h>
#include <imgui.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace edf::net {

struct ServerBrowserScanState {
  std::mutex mutex;
  std::atomic<bool> complete{false};
  std::vector<DiscoveredServer> servers;
  std::string error;
};

class ServerBrowserScanCoordinator {
 public:
  bool Start(bool directory, std::string endpoint, uint16_t discovery_port,
             std::shared_ptr<ServerBrowserScanState> state, std::string* error = nullptr) {
    if (error) error->clear();
    if (worker_.joinable()) {
      if (active_state_ && !active_state_->complete.load(std::memory_order_acquire)) {
        if (error) *error = "A previous server scan is still stopping. Try again shortly.";
        return false;
      }
      worker_.join();
    }
    active_state_ = state;
    try {
      worker_ = std::jthread([state = std::move(state), directory, endpoint = std::move(endpoint), discovery_port](
                                 std::stop_token stop) {
        std::string scan_error;
        auto servers = directory ? QueryMasterDirectory(endpoint, std::chrono::milliseconds(2500), &scan_error, stop)
                                 : DiscoverServers(discovery_port, std::chrono::milliseconds(700), &scan_error, stop);
        {
          std::lock_guard lock(state->mutex);
          state->servers = std::move(servers);
          state->error = std::move(scan_error);
        }
        state->complete.store(true, std::memory_order_release);
      });
    } catch (const std::system_error& exception) {
      active_state_.reset();
      if (error) *error = "Could not start server scan: " + std::string(exception.what());
      return false;
    }
    return true;
  }

  void RequestStop() {
    if (worker_.joinable()) worker_.request_stop();
  }

  void StopAndJoin() {
    RequestStop();
    if (worker_.joinable()) worker_.join();
    active_state_.reset();
  }

 private:
  std::shared_ptr<ServerBrowserScanState> active_state_;
  std::jthread worker_;
};

class ServerBrowserDialog final : public rex::ui::ImGuiDialog {
 public:
  using HostLocalHandler = std::function<bool(std::string_view, std::string*)>;
  using LocalHostActive = std::function<bool()>;
  using StopLocalHostHandler = std::function<void()>;

  ServerBrowserDialog(rex::ui::ImGuiDrawer* drawer, ClientSession& session,
                      ServerBrowserScanCoordinator& scan_coordinator, HostLocalHandler host_local,
                      LocalHostActive local_host_active, StopLocalHostHandler stop_local_host,
                      std::function<void()> on_close)
      : ImGuiDialog(drawer), session_(session), scan_coordinator_(scan_coordinator),
        host_local_(std::move(host_local)), local_host_active_(std::move(local_host_active)),
        stop_local_host_(std::move(stop_local_host)), on_close_(std::move(on_close)) {
    const std::string_view default_name = "EDF Player";
    std::copy(default_name.begin(), default_name.end(), player_name_.begin());
    const std::string saved_directory = rex::cvar::GetFlagByName("edf_server_directory");
    const size_t directory_length = std::min(saved_directory.size(), directory_.size() - 1);
    std::copy_n(saved_directory.data(), directory_length, directory_.data());
    directory_[directory_length] = '\0';
    // Keep even hand-edited config values within the same bound as the UI buffer.
    rex::cvar::SetFlagByName("edf_server_directory", directory_.data());
    const std::string saved_discovery_port = rex::cvar::GetFlagByName("edf_discovery_port");
    int configured_port = kDefaultDiscoveryPort;
    const auto [end, error] = std::from_chars(saved_discovery_port.data(),
                                               saved_discovery_port.data() + saved_discovery_port.size(),
                                               configured_port);
    if (error != std::errc{} || end != saved_discovery_port.data() + saved_discovery_port.size())
      configured_port = kDefaultDiscoveryPort;
    discovery_port_ = ClampDiscoveryPort(configured_port);
    rex::cvar::SetFlagByName("edf_discovery_port", std::to_string(discovery_port_));
  }
  static ServerBrowserDialog*& Current() { static ServerBrowserDialog* current = nullptr; return current; }
  void Dismiss() { Close(); }
 protected:
  void OnClose() override {
    scan_coordinator_.RequestStop();
    CancelPendingJoin();
    if (Current() == this) Current() = nullptr;
    if (on_close_) { auto callback = std::move(on_close_); callback(); }
  }

  void OnDraw(ImGuiIO& io) override {
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(1050.0f, io.DisplaySize.x * 0.92f), std::min(700.0f, io.DisplaySize.y * 0.9f)), ImGuiCond_Always);
    ImGui::Begin("EDF2027 Server Browser", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
    ImGui::TextUnformatted("Self-hosted EDF2027 sessions");
    ImGui::Separator();
    ImGui::InputText("Player name", player_name_.data(), player_name_.size());
    ImGui::InputText("Address", address_.data(), address_.size());
    ImGui::SameLine();
    if (ImGui::Button("Join address")) Join(address_.data());
    ImGui::SameLine();
    if (local_host_active_ && local_host_active_()) {
      if (ImGui::Button("Stop local server") && stop_local_host_) stop_local_host_();
    } else if (ImGui::Button("Host local and join")) {
      std::string error;
      if (!host_local_ || !host_local_(player_name_.data(), &error)) {
        scan_error_ = error.empty() ? "Could not start the local server" : std::move(error);
      } else {
        constexpr std::string_view local_endpoint = "127.0.0.1:27717";
        std::copy(local_endpoint.begin(), local_endpoint.end(), address_.begin());
        address_[local_endpoint.size()] = '\0';
        if (Join(std::string(local_endpoint))) local_host_join_pending_ = true;
        else if (stop_local_host_) stop_local_host_();
      }
    }
    int edited_discovery_port = discovery_port_;
    if (ImGui::InputInt("LAN discovery port", &edited_discovery_port, 0, 0)) {
      discovery_port_ = ClampDiscoveryPort(edited_discovery_port);
      rex::cvar::SetFlagByName("edf_discovery_port", std::to_string(discovery_port_));
    }
    ImGui::SameLine();
    if (ImGui::Button(scanning_ ? "Scanning..." : "Refresh LAN")) Scan(false);
    ImGui::TextDisabled("LAN port must match the host server's --discovery-port.");
    if (ImGui::InputText("Directory host:port", directory_.data(), directory_.size()))
      rex::cvar::SetFlagByName("edf_server_directory", directory_.data());
    ImGui::SameLine();
    if (ImGui::Button(scanning_ ? "Scanning...##directory" : "Query directory")) Scan(true);

    const ClientView view = session_.View();
    if (local_host_join_pending_ && view.state == ClientState::kConnected) {
      local_host_join_pending_ = false;
    } else if (local_host_join_pending_ && view.state == ClientState::kFailed) {
      local_host_join_pending_ = false;
      const std::string failure = view.status;
      if (stop_local_host_) stop_local_host_();
      scan_error_ = "Local host connection failed: " + failure + ". The local server was stopped; you can try hosting again.";
    }
    ImGui::Text("Status: %s%s", StateText(view.state), view.status.empty() ? "" : (": " + view.status).c_str());
    if (view.state == ClientState::kConnected) {
      ImGui::SameLine();
      if (ImGui::Button("Disconnect")) session_.Disconnect();
      ImGui::Text("Server tick %u   Players visible %zu", view.snapshot.tick, view.snapshot.players.size());
    } else if (view.state == ClientState::kConnecting) {
      ImGui::SameLine();
      if (ImGui::Button("Cancel join")) CancelPendingJoin();
    }
    if (!scan_error_.empty()) ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.38f, 1.0f), "%s", scan_error_.c_str());

    ImGui::Separator();
    ImGui::TextUnformatted(scan_is_directory_ ? "Directory servers" : "LAN servers");
    if (scanning_ && scan_state_->complete.load(std::memory_order_acquire)) {
      std::lock_guard lock(scan_state_->mutex);
      scan_result_ = scan_state_->servers;
      scan_error_ = scan_state_->error;
      scanning_ = false;
    }
    if (scan_result_.empty()) {
      const char* empty_status = scanning_
          ? (scan_is_directory_ ? "Querying the server directory..." : "Searching the local network...")
          : (scan_is_directory_ ? "No directory servers found. Check the directory address and server registration."
                                : "No LAN servers found. Enter an address to join over the Internet.");
      ImGui::TextDisabled("%s", empty_status);
    } else {
      ImGui::BeginChild("##edf_server_list", ImVec2(0, 250), ImGuiChildFlags_Borders);
      for (size_t i = 0; i < scan_result_.size(); ++i) {
        const auto& server = scan_result_[i];
        const bool matching_build = std::string_view(server.info.build_tag) == kPortBuildTag;
        ImGui::PushID(static_cast<int>(i));
        ImGui::Text("%s", server.info.name.c_str());
        ImGui::SameLine(310.0f);
        ImGui::TextDisabled("%s", server.info.mission.c_str());
        ImGui::SameLine(450.0f);
        ImGui::TextDisabled("%s", server.info.server_authoritative_movement
            ? "Server movement" : "Client movement");
        ImGui::SameLine(560.0f);
        ImGui::Text("%u / %u", server.info.players, server.info.max_players);
        ImGui::SameLine(660.0f);
        ImGui::TextDisabled("%s:%u", server.address.c_str(), server.info.game_port);
        ImGui::SameLine();
        if (matching_build && ImGui::SmallButton("Join")) {
          const std::string endpoint = server.address + ":" + std::to_string(server.info.game_port);
          std::copy(endpoint.begin(), endpoint.end(), address_.begin());
          address_[std::min(endpoint.size(), address_.size() - 1)] = '\0';
          Join(endpoint);
        } else if (!matching_build) {
          ImGui::TextDisabled("Build mismatch");
        }
        ImGui::Separator();
        ImGui::PopID();
      }
      ImGui::EndChild();
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Connected / nearby players");
    for (const auto& player : view.snapshot.players)
      ImGui::BulletText("%s  (player %u)", player.name.c_str(), player.id);
    if (view.snapshot.players.empty()) ImGui::TextDisabled("Connect to a server to see its player list.");
    if (view.state == ClientState::kConnected) {
      const auto send_chat = [this] {
        if (session_.SendChat(chat_input_.data())) {
          chat_input_[0] = '\0';
          scan_error_.clear();
        } else {
          scan_error_ = "Chat queue full, or message exceeds 255 UTF-8 bytes.";
        }
      };
      if (ImGui::InputText("Chat message", chat_input_.data(), chat_input_.size(), ImGuiInputTextFlags_EnterReturnsTrue))
        send_chat();
      ImGui::SameLine();
      if (ImGui::Button("Send chat")) send_chat();
    }
    for (const auto& line : view.chat) ImGui::TextWrapped("%s", line.c_str());
    if (ImGui::Button("Close")) Close();
    ImGui::End();
  }

 private:
  static const char* StateText(ClientState state) {
    switch (state) {
      case ClientState::kConnecting: return "Connecting";
      case ClientState::kConnected: return "Connected";
      case ClientState::kFailed: return "Failed";
      default: return "Disconnected";
    }
  }
  static int ClampDiscoveryPort(int port) { return std::clamp(port, 1, 65535); }
  void CancelPendingJoin() {
    const bool cancelled = session_.CancelPendingConnect();
    if (!local_host_join_pending_) return;
    const bool connected = session_.View().state == ClientState::kConnected;
    local_host_join_pending_ = false;
    if ((cancelled || !connected) && stop_local_host_) stop_local_host_();
  }
  bool Join(std::string endpoint) {
    local_host_join_pending_ = false;
    std::string error;
    if (!session_.Connect(std::move(endpoint), player_name_.data(), &error)) {
      scan_error_ = std::move(error);
      return false;
    }
    scan_error_.clear();
    return true;
  }
  void Scan(bool directory) {
    if (scanning_) return;
    if (directory && directory_[0] == '\0') { scan_error_ = "Enter a self-hosted directory address, such as 203.0.113.5:27719"; return; }
    scan_error_.clear();
    scanning_ = true;
    scan_is_directory_ = directory;
    scan_result_.clear();
    auto state = std::make_shared<ServerBrowserScanState>();
    scan_state_ = state;
    const std::string endpoint = directory_.data();
    const uint16_t discovery_port = static_cast<uint16_t>(ClampDiscoveryPort(discovery_port_));
    if (!scan_coordinator_.Start(directory, endpoint, discovery_port, state, &scan_error_)) {
      scanning_ = false;
    }
  }

  ClientSession& session_;
  ServerBrowserScanCoordinator& scan_coordinator_;
  HostLocalHandler host_local_;
  LocalHostActive local_host_active_;
  StopLocalHostHandler stop_local_host_;
  std::function<void()> on_close_;
  std::array<char, kMaxPlayerNameBytes + 1> player_name_{};
  std::array<char, kMaxChatBytes + 1> chat_input_{};
  std::array<char, 256> address_{};
  std::array<char, 512> directory_{};
  int discovery_port_ = kDefaultDiscoveryPort;
  std::shared_ptr<ServerBrowserScanState> scan_state_ = std::make_shared<ServerBrowserScanState>();
  std::vector<DiscoveredServer> scan_result_;
  std::string scan_error_;
  bool scanning_ = false;
  bool scan_is_directory_ = false;
  bool local_host_join_pending_ = false;
};

}  // namespace edf::net
