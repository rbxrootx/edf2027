#include "master_runtime.h"

#include <atomic>
#include <charconv>
#include <csignal>
#include <iostream>
#include <string_view>

namespace {
std::atomic<bool> g_stop{false};
void Stop(int) { g_stop.store(true, std::memory_order_relaxed); }
}

int main(int argc, char** argv) {
  uint16_t port = 27719;
  std::string bind_host;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--help" || arg == "-h") {
      std::cout << "EDF2027 server directory\nUsage: edf2027_master [--port PORT] [--bind ADDRESS]\n";
      return 0;
    }
    if ((arg != "--port" && arg != "--bind") || i + 1 >= argc) { std::cerr << "Unknown or incomplete option: " << arg << '\n'; return 2; }
    const std::string_view value(argv[++i]);
    if (arg == "--bind") { bind_host = value; continue; }
    unsigned parsed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size() || parsed < 1 || parsed > 65535) {
      std::cerr << "Invalid directory port\n";
      return 2;
    }
    port = static_cast<uint16_t>(parsed);
  }
  std::signal(SIGINT, Stop);
  std::signal(SIGTERM, Stop);
  edf::net::MasterRuntime directory(port, [](std::string line) { std::cout << line << '\n'; }, std::move(bind_host));
  return directory.Run(g_stop);
}
