#if defined(_WIN32)
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include "resolve_ipv4.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <string>
#include <vector>

namespace edf::net {
namespace {
void SetError(std::string* error, std::string text) {
  if (error) *error = std::move(text);
}

bool IsStopped(const std::function<bool()>& stop_requested) {
  return stop_requested && stop_requested();
}

#if defined(_WIN32)
std::wstring ToWide(std::string_view value) {
  if (value.empty() || value.size() > static_cast<size_t>(INT_MAX)) return {};
  const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        value.data(), static_cast<int>(value.size()),
                                        nullptr, 0);
  if (count <= 0) return {};
  std::wstring result(static_cast<size_t>(count), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                          value.data(), static_cast<int>(value.size()),
                          result.data(), count) != count)
    return {};
  return result;
}

std::string WinsockError(int result) {
  return "Name lookup failed (Winsock error " + std::to_string(result) + ")";
}
#endif
}  // namespace

bool ResolveIPv4(std::string_view host, uint16_t port,
                 std::chrono::milliseconds timeout,
                 const std::function<bool()>& stop_requested,
                 ENetAddress& address, std::string* error) {
  if (error) error->clear();
  address = {};
  if (IsStopped(stop_requested)) return false;
  if (host.empty()) {
    SetError(error, "Host name is empty");
    return false;
  }
  timeout = std::max(timeout, std::chrono::milliseconds(0));
  if (timeout.count() == 0) {
    SetError(error, "Name lookup timed out");
    return false;
  }

#if defined(_WIN32)
  const std::wstring wide_host = ToWide(host);
  if (wide_host.empty()) {
    SetError(error, "Host name is not valid UTF-8");
    return false;
  }

  HANDLE complete_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!complete_event) {
    SetError(error, "Could not create the host lookup event (Windows error " +
                     std::to_string(GetLastError()) + ")");
    return false;
  }

  ADDRINFOEXW hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;
  ADDRINFOEXW* results = nullptr;
  OVERLAPPED overlapped{};
  overlapped.hEvent = complete_event;
  HANDLE cancel_handle = nullptr;
  const auto timeout_ms = std::clamp<int64_t>(timeout.count(), 1, INT_MAX);
  timeval provider_timeout{};
  provider_timeout.tv_sec = static_cast<long>(timeout_ms / 1000);
  provider_timeout.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  int lookup_result = GetAddrInfoExW(wide_host.c_str(), nullptr, NS_ALL, nullptr,
                                     &hints, &results, &provider_timeout,
                                     &overlapped, nullptr, &cancel_handle);
  if (lookup_result == WSA_IO_PENDING) {
    bool canceled = false;
    bool timed_out = false;
    bool wait_failed = false;
    for (;;) {
      const DWORD wait = WaitForSingleObject(complete_event, 25);
      if (wait == WAIT_OBJECT_0) break;
      if (wait == WAIT_FAILED) {
        canceled = true;
        wait_failed = true;
        break;
      }
      if (IsStopped(stop_requested)) {
        canceled = true;
        break;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        canceled = true;
        timed_out = true;
        break;
      }
    }

    if (canceled) {
      if (cancel_handle) GetAddrInfoExCancel(&cancel_handle);
      // Cancellation is asynchronous. Reap the completion before releasing
      // the OVERLAPPED event or Winsock state.
      WaitForSingleObject(complete_event, INFINITE);
      lookup_result = GetAddrInfoExOverlappedResult(&overlapped);
      if (results) FreeAddrInfoExW(results);
      CloseHandle(complete_event);
      if (!IsStopped(stop_requested)) {
        SetError(error, wait_failed ? "Could not wait for the host lookup to finish"
                                   : (timed_out ? "Name lookup timed out"
                                                : WinsockError(lookup_result)));
      }
      return false;
    }

    if (lookup_result == WSA_IO_PENDING) {
      lookup_result = GetAddrInfoExOverlappedResult(&overlapped);
    }
  }

  if (lookup_result != 0) {
    if (results) FreeAddrInfoExW(results);
    CloseHandle(complete_event);
    if (!IsStopped(stop_requested)) SetError(error, WinsockError(lookup_result));
    return false;
  }

  bool found = false;
  for (const ADDRINFOEXW* entry = results; entry; entry = entry->ai_next) {
    if (entry->ai_family != AF_INET || !entry->ai_addr ||
        entry->ai_addrlen < sizeof(sockaddr_in))
      continue;
    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(entry->ai_addr);
    address.host = ipv4->sin_addr.s_addr;
    address.port = port;
    found = true;
    break;
  }
  if (results) FreeAddrInfoExW(results);
  CloseHandle(complete_event);
  if (!found) {
    SetError(error, "Host name did not resolve to an IPv4 address");
    return false;
  }
  return !IsStopped(stop_requested);
#else
  const std::string host_name(host);
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;
  addrinfo* results = nullptr;
  const int lookup_result = getaddrinfo(host_name.c_str(), nullptr, &hints, &results);
  if (lookup_result != 0) {
    if (!IsStopped(stop_requested)) SetError(error, gai_strerror(lookup_result));
    return false;
  }
  if (IsStopped(stop_requested)) {
    freeaddrinfo(results);
    return false;
  }
  if (std::chrono::steady_clock::now() >= deadline) {
    freeaddrinfo(results);
    SetError(error, "Name lookup timed out");
    return false;
  }
  bool found = false;
  for (const addrinfo* entry = results; entry; entry = entry->ai_next) {
    if (entry->ai_family != AF_INET || !entry->ai_addr ||
        entry->ai_addrlen < sizeof(sockaddr_in))
      continue;
    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(entry->ai_addr);
    address.host = ipv4->sin_addr.s_addr;
    address.port = port;
    found = true;
    break;
  }
  freeaddrinfo(results);
  if (!found) SetError(error, "Host name did not resolve to an IPv4 address");
  return found && !IsStopped(stop_requested);
#endif
}

}  // namespace edf::net
