#pragma once

#include <cstdint>
#include <vector>

namespace edf::multiplayer {

// Called by the game's single scenegraph-removal hook before it delegates to
// the original guest routine. This only records read-only trace data.
void TraceMissionAntRemoval(uint8_t* guest_base, uint32_t object, uint32_t caller);

struct MissionAntTraceKey {
  uint64_t local_generation = 0;
  uint32_t call_ordinal = 0;
  uint32_t ant_ordinal = 0;
  uint32_t object = 0;  // Local-only diagnostic identity; never serialized.
};

// Engine-thread snapshot. It revalidates each local object before returning a
// key. By default it returns live ants; include_dead also returns retained
// dead objects so an opted-in host state sample can restore their state.
std::vector<MissionAntTraceKey> SnapshotMissionAntTraceKeys(
    uint8_t* guest_base, bool include_dead = false);
uint64_t CurrentMissionAntTraceGeneration();

// Enables network sampling while connected. Diagnostic spawn logging is
// separately controlled by edf_mp_actor_trace. Identity tracking itself stays
// active from mission start so a later connection can use original spawn keys.
void SetMissionActorNetworkReplicationActive(bool active);
bool MissionActorNetworkReplicationActive();

}  // namespace edf::multiplayer
