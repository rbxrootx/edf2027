#pragma once

#include <enet/enet.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace edf::net {

// Resolve an IPv4 host without making the browser or game thread own the
// operating system's synchronous DNS call. Windows uses cancellable
// GetAddrInfoExW; the stop callback is checked while the lookup is pending.
bool ResolveIPv4(std::string_view host, uint16_t port,
                 std::chrono::milliseconds timeout,
                 const std::function<bool()>& stop_requested,
                 ENetAddress& address, std::string* error = nullptr);

}  // namespace edf::net
