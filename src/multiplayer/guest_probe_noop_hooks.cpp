#include <cstdint>

namespace edf::net::probe {

// The opt-in game executable compiles generated probe call sites but does not link the
// recorder used by the headless diagnostic executables. Keep those calls link-safe.
void RecordRawVmDispatch(uint8_t*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                         uint32_t) {}

void RecordMissionStartDispatch(uint8_t*, uint32_t, uint32_t, uint32_t, uint32_t,
                                uint32_t, uint32_t, uint32_t, bool) {}

}  // namespace edf::net::probe
