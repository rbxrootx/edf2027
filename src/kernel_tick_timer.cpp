// EDF2027 - KeTimeStampBundle tick updater.
//
// The SDK keeps the guest's KeTimeStampBundle.TickCount (the xboxkrnl variable
// the title's GetTickCount reads, bundle + 16) current with a 1 ms recurring
// rex::thread::HighResolutionTimer. That timer is the only user, on Windows, of
// the SDK's rex::thread::TimerQueue, whose dispatch thread waits for the next due
// time in a disruptorplus spin_wait: ~4000 pause instructions after every fire,
// then SwitchToThread in a loop with a 1 ms Sleep every twentieth pass. With a
// 1 ms period it never gets further than that, so it holds ~14% of a core in every
// phase of the game (measured on the Mission 1 intro) - wasted power on
// a handheld whose CPU and GPU share one budget.
//
// This disarms that timer (TimerQueueWaitItem::Disarm: no callback runs after it
// returns) and writes the same value, Clock::QueryGuestUptimeMillis(), from a
// thread that sleeps on a 1 ms high-resolution waitable timer. The value is
// computed at the time of the store in both, so the tick the guest reads is as
// correct; only how stale it can get between updates changes (the waitable timer
// wakes within a fraction of a millisecond, like the spin). With the queue empty
// the SDK's dispatch thread falls back to its yield/Sleep(1) loop.
//
// The SDK keeps the timer in a private member, reached here through the explicit
// instantiation rule ([temp.explicit]: access is not checked in the template
// arguments of an explicit instantiation), so the offsets come from the SDK's own
// headers. --edf_kernel_tick_timer=false keeps the SDK's timer.
#include "kernel_tick_timer.h"

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/kernel/xboxkrnl/module.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/export_resolver.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/thread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#endif

REXCVAR_DEFINE_BOOL(edf_kernel_tick_timer, true, "EDF2027",
                    "Update the guest's 1 ms kernel tick (KeTimeStampBundle) from a high-resolution waitable timer "
                    "instead of the SDK's spinning timer thread (~14% of a core). Restart to apply");
REXCVAR_DECLARE(bool, edf_frametime_log);

namespace {
template <class Tag, typename Tag::type Member>
struct PrivateMember {
  friend typename Tag::type Get(Tag) { return Member; }
};
struct TimestampTimerTag {
  using type = std::unique_ptr<rex::thread::HighResolutionTimer> rex::kernel::xboxkrnl::XboxkrnlModule::*;
  friend type Get(TimestampTimerTag);
};
struct WaitItemTag {
  using type = std::weak_ptr<rex::thread::TimerQueueWaitItem> rex::thread::HighResolutionTimer::*;
  friend type Get(WaitItemTag);
};
template struct PrivateMember<TimestampTimerTag, &rex::kernel::xboxkrnl::XboxkrnlModule::timestamp_timer_>;
template struct PrivateMember<WaitItemTag, &rex::thread::HighResolutionTimer::wait_item_>;

constexpr uint16_t kKeTimeStampBundleOrdinal = 0x00AD;

#if defined(_WIN32)
struct TickThread {
  HANDLE timer = nullptr;
  HANDLE stop = nullptr;
  std::thread thread;
};
TickThread* g_tick = nullptr;

void TickThreadMain(HANDLE timer, HANDLE stop, uint8_t* tick_count, bool log) {
  SetThreadDescription(GetCurrentThread(), L"edf::KernelTickTimer");
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  const HANDLE handles[2] = {stop, timer};
  using clock = std::chrono::steady_clock;
  constexpr auto kPeriod = std::chrono::milliseconds(1);
  auto last = clock::now(), window = last, next = last;
  uint64_t updates = 0, late = 0;
  clock::duration max_gap{};
  for (;;) {
    rex::memory::store_and_swap<uint32_t>(tick_count, rex::chrono::Clock::QueryGuestUptimeMillis());
    if (log) {
      const auto now = clock::now();
      const auto gap = now - last;
      last = now;
      ++updates;
      if (gap > max_gap) max_gap = gap;
      if (gap > std::chrono::microseconds(2000)) ++late;
      if (now - window >= std::chrono::seconds(10)) {
        REXLOG_INFO("EDF2027: kernel tick timer: {} updates in {:.1f} s, max gap {:.3f} ms, {} gaps over 2 ms", updates,
                    std::chrono::duration<double>(now - window).count(),
                    std::chrono::duration<double, std::milli>(max_gap).count(), late);
        window = now;
        updates = late = 0;
        max_gap = {};
      }
    }
    // A 1 ms grid of one-shot waits: a periodic waitable timer rounds its period
    // up (measured ~1.5 ms, 661 updates a second). A wake later than the next
    // slot restarts the grid instead of catching up.
    next += kPeriod;
    const auto now = clock::now();
    if (next <= now) next = now + kPeriod;
    LARGE_INTEGER due{};
    due.QuadPart = -(std::max<LONGLONG>)(1, std::chrono::duration_cast<std::chrono::nanoseconds>(next - now).count() / 100);
    if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) Sleep(1);
    else if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) return;
    if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) return;
  }
}
#endif
}  // namespace

namespace edf {
void StartKernelTickTimer(rex::Runtime* runtime) {
#if defined(_WIN32)
  if (g_tick || !REXCVAR_GET(edf_kernel_tick_timer) || !runtime || !runtime->kernel_state() ||
      !runtime->export_resolver())
    return;
  auto* bundle_export = runtime->export_resolver()->GetExportByOrdinal("xboxkrnl.exe", kKeTimeStampBundleOrdinal);
  if (!bundle_export || !bundle_export->variable_ptr) {
    REXLOG_WARN("EDF2027: kernel tick timer: KeTimeStampBundle not found; keeping the SDK timer");
    return;
  }
  auto module = runtime->kernel_state()->GetKernelModule("xboxkrnl.exe");
  auto* krnl = dynamic_cast<rex::kernel::xboxkrnl::XboxkrnlModule*>(module.get());
  std::shared_ptr<rex::thread::TimerQueueWaitItem> wait_item;
  if (krnl) {
    if (auto& sdk_timer = krnl->*Get(TimestampTimerTag{})) wait_item = ((*sdk_timer).*Get(WaitItemTag{})).lock();
  }
  if (!wait_item) {
    REXLOG_WARN("EDF2027: kernel tick timer: the SDK's timestamp timer was not found; keeping it");
    return;
  }
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!timer) {
    REXLOG_WARN("EDF2027: kernel tick timer: no high-resolution waitable timer (error {}); keeping the SDK timer",
                GetLastError());
    return;
  }
  LARGE_INTEGER due{};
  due.QuadPart = -10000;  // a test arm; the thread re-arms it every millisecond
  HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!stop || !SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
    REXLOG_WARN("EDF2027: kernel tick timer: could not arm the timer (error {}); keeping the SDK timer", GetLastError());
    if (stop) CloseHandle(stop);
    CloseHandle(timer);
    return;
  }
  // No SDK callback runs after Disarm returns; the item stays owned by the module.
  wait_item->Disarm();
  auto* tick_count = runtime->memory()->TranslateVirtual<uint8_t*>(bundle_export->variable_ptr) + 16;
  g_tick = new TickThread{timer, stop, {}};
  bool log_timer = false;
#if !defined(EDF2027_HEADLESS_GUEST)
  log_timer = REXCVAR_GET(edf_frametime_log);
#endif
  g_tick->thread = std::thread(TickThreadMain, timer, stop, tick_count, log_timer);
  REXLOG_INFO("EDF2027: kernel tick (KeTimeStampBundle {:08X}) updated by a 1 ms high-resolution waitable timer",
              bundle_export->variable_ptr);
#else
  (void)runtime;
#endif
}

void StopKernelTickTimer() {
#if defined(_WIN32)
  if (!g_tick) return;
  SetEvent(g_tick->stop);
  if (g_tick->thread.joinable()) g_tick->thread.join();
  CancelWaitableTimer(g_tick->timer);
  CloseHandle(g_tick->timer);
  CloseHandle(g_tick->stop);
  delete g_tick;
  g_tick = nullptr;
#endif
}

bool KernelTickTimerRunning() noexcept {
#if defined(_WIN32)
  return g_tick != nullptr;
#else
  return false;
#endif
}
}  // namespace edf
