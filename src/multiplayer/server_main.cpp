#include "server_runtime.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <csignal>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <array>
#include <unistd.h>
#endif

namespace {
std::atomic<bool> g_stop{false};
void Stop(int) { g_stop.store(true, std::memory_order_relaxed); }

std::filesystem::path ExecutableDirectory(const char* argv0) {
#if defined(_WIN32)
  std::vector<wchar_t> buffer(512);
  for (;;) {
    const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!size) break;
    if (size < buffer.size() - 1) return std::filesystem::path(std::wstring(buffer.data(), size)).parent_path();
    if (buffer.size() >= 32768) break;
    buffer.resize(std::min<std::size_t>(buffer.size() * 2, 32768));
  }
#elif defined(__linux__)
  std::array<char, 4096> buffer{};
  const ssize_t size = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (size > 0) return std::filesystem::path(std::string(buffer.data(), static_cast<std::size_t>(size))).parent_path();
#endif
  if (argv0 && *argv0) {
    std::error_code ec;
    const auto path = std::filesystem::absolute(argv0, ec);
    if (!ec && path.has_parent_path()) return path.parent_path();
  }
  std::error_code ec;
  return std::filesystem::current_path(ec);
}

void Help() {
  std::cout << "EDF2027 dedicated server\n"
               "Usage: edf2027_server [options]\n"
               "  --name NAME              Server name (max 48 bytes)\n"
               "  --mission NAME           Displayed mission name (max 48 bytes)\n"
               "  --port PORT              ENet gameplay UDP port (default 27717)\n"
               "  --discovery-port PORT    LAN browser UDP port (default 27718)\n"
               "  --no-discovery           Disable LAN broadcast discovery\n"
               "  --bind ADDRESS           IPv4 interface (default 0.0.0.0)\n"
               "  --master HOST:PORT       Register in a self-hosted Internet directory\n"
               "  --advertise-ip IPv4      Optional reachable IPv4 for directory listings\n"
               "  --max-players COUNT      Player slots, 1-512 (default 8)\n"
               "  --player-health COUNT    Server-owned health, 1-65535 (default 100; authority mode only)\n"
               "  --interest-radius METERS Player replication radius (default 500)\n"
               "  --max-client-speed MPS   Client-pose outlier ceiling, 1-1000 m/s (default 60)\n"
               "  --max-client-aim-rate DEG_PER_SEC  Client aim turn ceiling, 90-3600 deg/s (default 720)\n"
               "  --spawn-x METERS         Initial player spawn X (default 0)\n"
               "  --spawn-y METERS         Initial player spawn Y (default 0)\n"
               "  --spawn-z METERS         Initial player spawn Z (default 0)\n"
               "  --server-authoritative-movement  Ignore client poses; simulate horizontal input on the server\n"
               "  --data-dir DIRECTORY     Server data folder; scripts load from its scripts/ subfolder\n"
               "  --identity-file FILE     Persistent signing key (default: identity.edfkey beside server data)\n"
               "  --scripts DIRECTORY      Trusted server-side .luau scripts\n"
               "  --help                   Show this help\n\n"
               "Open/forward the game UDP port and, when configured, the directory UDP port for Internet play.\n"
               "The discovery UDP port is for LAN browser scans.\n";
}
bool ParsePort(std::string_view text, uint16_t& output) {
  unsigned value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() || value < 1 || value > 65535) return false;
  output = static_cast<uint16_t>(value);
  return true;
}
bool ParseCount(std::string_view text, uint16_t& output) {
  unsigned value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() || value < 1 || value > edf::net::kMaxServerPlayers) return false;
  output = static_cast<uint16_t>(value);
  return true;
}
bool ParsePlayerHealth(std::string_view text, uint16_t& output) {
  unsigned value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() || value < 1 || value > 65535) return false;
  output = static_cast<uint16_t>(value);
  return true;
}
bool ParseInterestRadius(std::string_view text, uint32_t& output) {
  unsigned meters = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), meters);
  if (error != std::errc{} || end != text.data() + text.size() || meters < 1 || meters > 10'000) return false;
  output = meters * 100;
  return true;
}
bool ParseMaxClientSpeed(std::string_view text, uint32_t& output) {
  unsigned meters_per_second = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), meters_per_second);
  if (error != std::errc{} || end != text.data() + text.size() ||
      meters_per_second < 1 || meters_per_second > 1'000) return false;
  output = meters_per_second * 100;
  return true;
}
bool ParseMaxClientAimRate(std::string_view text, uint32_t& output) {
  unsigned degrees_per_second = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), degrees_per_second);
  if (error != std::errc{} || end != text.data() + text.size() ||
      degrees_per_second < 90 || degrees_per_second > 3'600) return false;
  output = static_cast<uint32_t>(std::lround(double(degrees_per_second) * 3.14159265358979323846 * 1'000.0 / 180.0));
  return true;
}
bool ParseSpawnMeters(std::string_view text, int32_t& output) {
  double meters = 0.0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), meters);
  if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(meters) ||
      meters < -10'000.0 || meters > 10'000.0) return false;
  output = static_cast<int32_t>(std::lround(meters * 100.0));
  return true;
}
}

int main(int argc, char** argv) {
  edf::net::ServerConfig config;
  const auto executable_directory = ExecutableDirectory(argc > 0 ? argv[0] : nullptr);
  std::optional<std::filesystem::path> data_directory;
  std::optional<std::filesystem::path> scripts_directory;
  std::optional<std::filesystem::path> identity_file;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--help" || arg == "-h") { Help(); return 0; }
    if (arg == "--no-discovery") { config.discovery_port = 0; continue; }
    if (arg == "--server-authoritative-movement") {
      config.server_authoritative_movement = true;
      continue;
    }
    if (i + 1 >= argc) { std::cerr << "Missing value for " << arg << '\n'; Help(); return 2; }
    const std::string_view value(argv[++i]);
    if (arg == "--name") config.name = value;
    else if (arg == "--mission") config.mission = value;
    else if (arg == "--bind") config.bind_host = value;
    else if (arg == "--master") config.master_endpoint = value;
    else if (arg == "--advertise-ip") config.advertise_ip = value;
    else if (arg == "--interest-radius") { if (!ParseInterestRadius(value, config.interest_radius_cm)) { std::cerr << "Invalid interest radius\n"; return 2; } }
    else if (arg == "--max-client-speed") { if (!ParseMaxClientSpeed(value, config.max_client_transform_speed_cm_per_second)) { std::cerr << "Invalid client speed ceiling; use 1-1000 meters per second\n"; return 2; } }
    else if (arg == "--max-client-aim-rate") { if (!ParseMaxClientAimRate(value, config.max_client_aim_turn_rate_mrad_per_second)) { std::cerr << "Invalid client aim turn rate; use 90-3600 degrees per second\n"; return 2; } }
    else if (arg == "--spawn-x") { if (!ParseSpawnMeters(value, config.spawn_x_cm)) { std::cerr << "Invalid spawn X; use meters from -10000 to 10000\n"; return 2; } }
    else if (arg == "--spawn-y") { if (!ParseSpawnMeters(value, config.spawn_y_cm)) { std::cerr << "Invalid spawn Y; use meters from -10000 to 10000\n"; return 2; } }
    else if (arg == "--spawn-z") { if (!ParseSpawnMeters(value, config.spawn_z_cm)) { std::cerr << "Invalid spawn Z; use meters from -10000 to 10000\n"; return 2; } }
    else if (arg == "--data-dir") data_directory = std::filesystem::path(std::string(value));
    else if (arg == "--scripts") scripts_directory = std::filesystem::path(std::string(value));
    else if (arg == "--identity-file") identity_file = std::filesystem::path(std::string(value));
    else if (arg == "--port") { if (!ParsePort(value, config.game_port)) { std::cerr << "Invalid game port\n"; return 2; } }
    else if (arg == "--discovery-port") { if (!ParsePort(value, config.discovery_port)) { std::cerr << "Invalid discovery port\n"; return 2; } }
    else if (arg == "--max-players") { if (!ParseCount(value, config.max_players)) { std::cerr << "Invalid player count\n"; return 2; } }
    else if (arg == "--player-health") { if (!ParsePlayerHealth(value, config.player_max_health)) { std::cerr << "Invalid player health; use 1-65535\n"; return 2; } }
    else { std::cerr << "Unknown option: " << arg << '\n'; Help(); return 2; }
  }
  if (config.advertise_ip.empty()) {
    if (const char* advertised_ip = std::getenv("EDF_ADVERTISE_IP")) config.advertise_ip = advertised_ip;
  }
  if (scripts_directory) config.scripts_directory = *scripts_directory;
  else if (data_directory) config.scripts_directory = *data_directory / "scripts";
  else {
    auto installed_data = executable_directory / "server-data";
    std::error_code ec;
    if (!std::filesystem::is_directory(installed_data, ec)) {
      const auto working_data = std::filesystem::current_path(ec) / "server-data";
      if (!ec && std::filesystem::is_directory(working_data, ec)) installed_data = working_data;
    }
    config.scripts_directory = installed_data / "scripts";
  }
  if (identity_file) config.identity_file = *identity_file;
  else if (data_directory) config.identity_file = *data_directory / "identity.edfkey";
  else if (scripts_directory) config.identity_file = scripts_directory->parent_path() / "identity.edfkey";
  else config.identity_file = config.scripts_directory.parent_path() / "identity.edfkey";
  if (config.name.empty() || config.name.size() > edf::net::kMaxServerNameBytes || config.mission.size() > edf::net::kMaxMissionNameBytes) {
    std::cerr << "Name/mission text exceeds protocol limit\n";
    return 2;
  }
  std::signal(SIGINT, Stop);
  std::signal(SIGTERM, Stop);
  edf::net::ServerRuntime server(std::move(config), [](std::string line) { std::cout << line << '\n'; });
  return server.Run(g_stop);
}
