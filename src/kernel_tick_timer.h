#pragma once
// KeTimeStampBundle tick updater (kernel_tick_timer.cpp).

namespace rex { class Runtime; }

namespace edf {
// Takes over the SDK's 1 ms KeTimeStampBundle.TickCount update. Call once the
// runtime is set up and before the guest runs; Stop before the runtime goes away.
void StartKernelTickTimer(rex::Runtime* runtime);
void StopKernelTickTimer();
bool KernelTickTimerRunning() noexcept;
}  // namespace edf
