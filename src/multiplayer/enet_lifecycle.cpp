#include "enet_lifecycle.h"

#include <enet/enet.h>

#include <mutex>

namespace edf::net {
namespace {
std::mutex g_mutex;
unsigned g_users = 0;
}

bool AcquireENet() {
  std::lock_guard lock(g_mutex);
  if (!g_users && enet_initialize() != 0) return false;
  ++g_users;
  return true;
}

void ReleaseENet() {
  std::lock_guard lock(g_mutex);
  if (!g_users) return;
  if (--g_users == 0) enet_deinitialize();
}
}  // namespace edf::net
