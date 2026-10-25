// Loading and the load trace: the map, mission and menu load scopes, the resource coordinator and helper, the loading screen presenter.
// Moved from guest_shader_bridge.cpp unchanged (stage 3 of its split); what this file shares with the bridge
// and the other hook files is in bridge/bridge_shared.h.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../../bridge/bridge_shared.h"
#include "../../guest_shader_bridge.h"
#include "../../guest_sdk_readable_range.h"
#include "../../native_load_trace.h"
#include "../../bridge/native_cvars.h"
#include "../../bridge/bridge_state.h"
#include "../../bridge/bridge_helpers.h"
#include "console/console_guest.h"
#include "multiplayer/mission_actor_trace.h"
#include <rex/hook.h>
#include <rex/cvar.h>
#include <rex/ppc/func.h>
#include <rex/logging.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

REXCVAR_DEFINE_BOOL(edf_mp_actor_trace,false,"EDF2027",
    "Trace mission ant spawns/removals and received host samples (development; read-only)");
REXCVAR_DEFINE_BOOL(edf_mp_mission_actor_reconcile,false,"EDF2027/Multiplayer",
    "Experimental: apply host mission-ant state to matching spawn keys (may match the wrong mission/actor)");

namespace {
std::atomic<bool> mission_actor_network_replication_active{false};

bool MissionActorTraceCaptureEnabled() {
  return REXCVAR_GET(edf_mp_actor_trace);
}

// Observational resource-worker boundaries. The persistent coordinator/helper
// are not BeginLoading/EndLoading transactions; preserve their original calls.
class ScriptLoadTiming {
 public:
  ScriptLoadTiming(const char* api,uint32_t mode,uint32_t index,bool active=true)
      : api_(api),mode_(mode),index_(index),enabled_(active && REXCVAR_GET(edf_native_load_timings)) {
    if(enabled_) start_=std::chrono::steady_clock::now();
  }
  ~ScriptLoadTiming() {
    if(!enabled_) return;
    const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start_).count();
    if(ms>=1.0) REXLOG_INFO("Native script timing: api={} mode={} index={} total_ms={} (inclusive, calls below 1ms omitted)",
                            api_,mode_,index_,ms);
  }
 private:
  const char* api_;
  uint32_t mode_,index_;
  bool enabled_;
  std::chrono::steady_clock::time_point start_{};
};
thread_local uint32_t map_load_depth=0;
constexpr size_t kMissionNativeSelectors=128;
std::array<std::atomic<uint32_t>,kMissionNativeSelectors> mission_native_samples{};
std::atomic<uint32_t> mission_factory_other_samples{0},mission_factory_candidate_samples{0};
std::atomic<uint32_t> mission_factory_faction_samples{0};
std::atomic<uint32_t> mission_player_index_init_samples{0},mission_player_count_samples{0};
std::atomic<uint32_t> mission_ant_spawn_samples{0};
std::atomic<uint32_t> mission_actor_trace_removals{0};
std::atomic<uint64_t> mission_actor_trace_generation{0};
std::atomic<bool> mission_actor_trace_pending{false};
constexpr uint32_t kMissionActorTraceCallLimit=4096;
// The current factory returns one object per invocation, so each accepted ant
// gets its own call ordinal and the per-call ant ordinal remains zero. Keep
// the lifetime bound aligned with the host's 4,096-entry actor registry.
constexpr uint32_t kMissionActorTraceAntLifetimeLimit=4096;
constexpr uint32_t kMissionActorTraceAntOrdinalLimit=64;

struct MissionAntTraceObject {
  uint64_t generation=0;
  uint32_t call_ordinal=0,ant_ordinal=0,vtable=0;
};
std::unordered_map<uint32_t,MissionAntTraceObject> mission_actor_trace_objects;

struct MissionAntSpawnTraceRecord {
  uint64_t generation=0;
  uint32_t call_ordinal=0,ant_ordinal=0,caller=0;
  uint32_t r3=0,r4=0,r5=0,r6=0,r7=0;
  uint64_t f1_bits=0;
  uint32_t object=0,vtable=0,faction=0;
  uint8_t awaken_latch_after=0;
  float health=0,x=0,y=0,z=0,yaw=0;
  bool yaw_valid=false;
};
std::mutex mission_actor_trace_mutex;
std::vector<MissionAntSpawnTraceRecord> mission_actor_trace_records;

void ResetMissionActorTrace() {
  mission_actor_trace_generation.fetch_add(1,std::memory_order_relaxed);
  mission_ant_spawn_samples.store(0,std::memory_order_relaxed);
  mission_actor_trace_removals.store(0,std::memory_order_relaxed);
  mission_actor_trace_objects.clear();
  std::lock_guard lock(mission_actor_trace_mutex);
  mission_actor_trace_records.clear();
  mission_actor_trace_pending.store(false,std::memory_order_relaxed);
}

void QueueMissionAntSpawnTrace(MissionAntSpawnTraceRecord record) {
  std::lock_guard lock(mission_actor_trace_mutex);
  mission_actor_trace_records.push_back(record);
  mission_actor_trace_pending.store(true,std::memory_order_release);
}

void FlushMissionActorTrace() {
  if(!mission_actor_trace_pending.exchange(false,std::memory_order_acq_rel)) return;
  std::vector<MissionAntSpawnTraceRecord> records;
  {
    std::lock_guard lock(mission_actor_trace_mutex);
    records.swap(mission_actor_trace_records);
  }
  for(const auto& record:records) REXLOG_INFO(
      "Mission ant identity trace: side=client generation={} call_ordinal={} ant_ordinal={} caller={:#x} args=[{:#x},{:#x},{:#x},{:#x},{:#x}] f1_bits={:#x} r7_awaken_gate={} object={:#x} vtable={:#x} class=clGiantAnt faction={} awaken_latch_after={} health={} x={} y={} z={} yaw_valid={} yaw_rad={}",
      record.generation,record.call_ordinal,record.ant_ordinal,record.caller,
      record.r3,record.r4,record.r5,record.r6,record.r7,record.f1_bits,
      record.r7!=0,record.object,record.vtable,record.faction,record.awaken_latch_after,
      record.health,record.x,record.y,
      record.z,record.yaw_valid,record.yaw);
}

bool IsPlayerConstructionCandidate(const std::string& class_name) {
  return class_name.find("Player")!=std::string::npos ||
      class_name.find("Soldier")!=std::string::npos ||
      class_name.find("Camera")!=std::string::npos;
}

std::string ReadGuestWidePath(uint8_t* base,uint32_t object) {
  try {
    // The same 32-byte MSVC std::wstring layout is used by console_game.cpp:
    // buffer at +4, size at +20, capacity at +24, with seven inline wchar_t slots.
    const edf::native::GuestReader reader(base);
    const uint32_t size=reader.Word(reader.Add(object,20));
    const uint32_t capacity=reader.Word(reader.Add(object,24));
    if(size>512 || capacity<size || capacity>4096) return "<invalid-length>";
    if(!size) return {};
    const uint32_t inline_buffer=reader.Add(object,4);
    const uint32_t data=capacity>7 ? reader.Word(inline_buffer) : inline_buffer;
    const auto* bytes=reader.Bytes(data,size_t(size)*2);
    std::string path;
    path.reserve(size);
    for(uint32_t i=0;i<size;++i) {
      const uint16_t character=uint16_t(bytes[i*2])<<8 | uint16_t(bytes[i*2+1]);
      if(character==0) break;
      path.push_back(character<=0x7F ? char(character) : '?');
    }
    return path;
  } catch(...) {
    return "<unreadable>";
  }
}

void TracePlayerSetupCall(const char* function,std::atomic<uint32_t>& samples,
    uint32_t caller,uint32_t r3,uint32_t r4,uint32_t r5,uint32_t r6,uint32_t result) {
  if(!LoadTraceOn()) return;
  const uint32_t sample=samples.fetch_add(1,std::memory_order_relaxed);
  if(sample>=16) return;
  REXLOG_INFO("Player seat setup sample: function={} sample={} caller={:#x} r3={:#x} r4={:#x} r5={:#x} r6={:#x} result_r3={:#x}",
    function,sample,caller,r3,r4,r5,r6,result);
}

void ResetMissionConstructionTraceSamples() {
  for(auto& sample:mission_native_samples) sample.store(0,std::memory_order_relaxed);
  mission_factory_other_samples.store(0,std::memory_order_relaxed);
  mission_factory_candidate_samples.store(0,std::memory_order_relaxed);
  mission_factory_faction_samples.store(0,std::memory_order_relaxed);
  mission_player_index_init_samples.store(0,std::memory_order_relaxed);
  mission_player_count_samples.store(0,std::memory_order_relaxed);
}
REX_EXTERN(__imp__sub_820CBD28);
REX_HOOK_RAW(sub_820CBD28) {
  struct Scope {
    Scope() { ++map_load_depth; }
    ~Scope() { --map_load_depth; }
  } scope;
  const uint32_t caller=uint32_t(ctx.lr);
  LoadTraceEvent("map_load_begin",caller);
  {
    edf::native::LoadTraceScope trace(LoadTraceOn(),edf::native::LoadTraceKind::MapLoad);
    __imp__sub_820CBD28(ctx,base);
  }
  LoadTraceEvent("map_load_end",caller);
}
// clMissionSequence::Begin (loads the mission's .bvm, .xPath and .Cam, then
// runs "main"): marks where the previous scene's teardown ends and the
// mission's own load starts. Observation only.
REX_EXTERN(__imp__sub_820CD448);
REX_HOOK_RAW(sub_820CD448) {
  const uint32_t caller=uint32_t(ctx.lr);
  if(LoadTraceOn()) ResetMissionConstructionTraceSamples();
  // Keep mission spawn identities even before networking starts. A client can
  // join a running mission and still match actors that were created earlier.
  ResetMissionActorTrace();
  LoadTraceEvent("mission_begin",caller);
  __imp__sub_820CD448(ctx,base);
  FlushMissionActorTrace();
  LoadTraceEvent("mission_begin_end",caller);
}
// Inclusive nested timings, restricted to this thread's LoadMap call chain.
// Address labels avoid assigning unverified semantic names to guest helpers.
#define EDF_MAP_TIMED_HOOK(address) \
  REX_EXTERN(__imp__sub_##address); \
  REX_HOOK_RAW(sub_##address) { \
    ScriptLoadTiming timing("map." #address,0,0,map_load_depth!=0); \
    __imp__sub_##address(ctx,base); \
  }
EDF_MAP_TIMED_HOOK(821B8090)
EDF_MAP_TIMED_HOOK(820BE628)
EDF_MAP_TIMED_HOOK(820B4D58)
EDF_MAP_TIMED_HOOK(820B3B00)
EDF_MAP_TIMED_HOOK(821A6158)
EDF_MAP_TIMED_HOOK(821B7278)
EDF_MAP_TIMED_HOOK(821B2850)
EDF_MAP_TIMED_HOOK(821ACBD0)
EDF_MAP_TIMED_HOOK(820ADD28)
EDF_MAP_TIMED_HOOK(821B24C0)
EDF_MAP_TIMED_HOOK(820B77A0)
EDF_MAP_TIMED_HOOK(820B6458)
EDF_MAP_TIMED_HOOK(820B6FA8)
EDF_MAP_TIMED_HOOK(821A6278)
EDF_MAP_TIMED_HOOK(820B4858)
// Not the rotation builders 821C7B20/7D80/7E30: the skeletal evaluation calls
// them for every bone every step, and one call never reaches the 1 ms logged.
EDF_MAP_TIMED_HOOK(821ACAD8)
EDF_MAP_TIMED_HOOK(821AB708)
EDF_MAP_TIMED_HOOK(821D6448)
EDF_MAP_TIMED_HOOK(821D6AE0)
EDF_MAP_TIMED_HOOK(821D6C20)
EDF_MAP_TIMED_HOOK(821AA130)
EDF_MAP_TIMED_HOOK(821AAB70)
EDF_MAP_TIMED_HOOK(821B5568)
EDF_MAP_TIMED_HOOK(821AB520)
EDF_MAP_TIMED_HOOK(821CB6A0)
EDF_MAP_TIMED_HOOK(821CB550)
EDF_MAP_TIMED_HOOK(821D88C0)
EDF_MAP_TIMED_HOOK(821DB008)
EDF_MAP_TIMED_HOOK(821D85B8)
EDF_MAP_TIMED_HOOK(821D7C18)
EDF_MAP_TIMED_HOOK(821D7850)
EDF_MAP_TIMED_HOOK(821D79D8)
EDF_MAP_TIMED_HOOK(821D8770)
EDF_MAP_TIMED_HOOK(821B3C98)
#undef EDF_MAP_TIMED_HOOK
REX_EXTERN(__imp__sub_820C7220);
REX_HOOK_RAW(sub_820C7220) {
  ScriptLoadTiming timing("menu",ctx.r6.u32,ctx.r7.u32);
  __imp__sub_820C7220(ctx,base);
}
REX_EXTERN(__imp__sub_820D1518);
REX_HOOK_RAW(sub_820D1518) {
  ScriptLoadTiming timing("mission",ctx.r6.u32,ctx.r7.u32);
  if(!LoadTraceOn()) { __imp__sub_820D1518(ctx,base); FlushMissionActorTrace(); return; }
  // The dispatcher selects its native with r7 & 0x7f. Keep the incoming
  // registers verbatim as evidence until their script-value layout is known.
  const uint32_t caller=uint32_t(ctx.lr);
  const uint32_t r3=ctx.r3.u32,r4=ctx.r4.u32,r5=ctx.r5.u32,r6=ctx.r6.u32,r7=ctx.r7.u32;
  const auto native=(std::min)(size_t(r7 & 0x7f),NativeLoadTraceState::kNatives-1);
  const auto start=std::chrono::steady_clock::now();
  __imp__sub_820D1518(ctx,base);
  FlushMissionActorTrace();
  auto& state=LoadTraceState();
  state.native_calls[native].fetch_add(1,std::memory_order_relaxed);
  state.native_nanos[native].fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now()-start).count()),std::memory_order_relaxed);
  // At most two raw samples for each of 128 selectors; no guest pointers are
  // dereferenced here because the BVM argument encoding is not decoded yet.
  const uint32_t sample=mission_native_samples[native].fetch_add(1,std::memory_order_relaxed);
  if(sample<2) REXLOG_INFO("Mission native dispatch sample: selector={} sample={} r7={:#x} caller={:#x} r3={:#x} r4={:#x} r5={:#x} r6={:#x} result_r3={:#x}",
    native,sample,r7,caller,r3,r4,r5,r6,ctx.r3.u32);
}
// Native #300 creates mission objects. Preserve raw entry values as opaque
// evidence; only inspect the returned object after validating its heap range.
REX_EXTERN(__imp__sub_820CEBD0);
REX_HOOK_RAW(sub_820CEBD0) {
  if(mission_ant_spawn_samples.load(std::memory_order_relaxed)>=kMissionActorTraceAntLifetimeLimit) {
    __imp__sub_820CEBD0(ctx,base);
    return;
  }
  const uint32_t caller=uint32_t(ctx.lr);
  const uint32_t r3=ctx.r3.u32,r4=ctx.r4.u32,r5=ctx.r5.u32,r6=ctx.r6.u32,r7=ctx.r7.u32;
  const uint64_t f1_bits=ctx.f1.u64;
  __imp__sub_820CEBD0(ctx,base);

  const uint32_t object=ctx.r3.u32;
  if(!edf::console::GuestMemory::Heap(object,738)) return;
  const edf::console::GuestMemory memory(base);
  if(memory.ClassName(object)!="clGiantAnt") return;
  const uint32_t faction=memory.U32(object+604);
  if(faction!=1) return;
  const uint32_t call_ordinal=mission_ant_spawn_samples.fetch_add(1,std::memory_order_relaxed);
  if(call_ordinal>=kMissionActorTraceAntLifetimeLimit) return;
  constexpr uint32_t ant_ordinal=0;

  const uint64_t generation=mission_actor_trace_generation.load(std::memory_order_relaxed);
  const uint32_t vtable=memory.U32(object);
  mission_actor_trace_objects.insert_or_assign(object,
      MissionAntTraceObject{generation,call_ordinal,ant_ordinal,vtable});
  if(MissionActorTraceCaptureEnabled()) {
    const float health=memory.F32(object+592);
    const float x=memory.F32(object+272),y=memory.F32(object+276),z=memory.F32(object+280);
    const float forward_x=memory.F32(object+256),forward_z=memory.F32(object+264);
    if(std::isfinite(health)&&std::isfinite(x)&&std::isfinite(y)&&std::isfinite(z)&&
       std::isfinite(forward_x)&&std::isfinite(forward_z)) {
      const bool yaw_valid=std::abs(forward_x)>=1e-6f||std::abs(forward_z)>=1e-6f;
      const float yaw=yaw_valid?std::atan2(forward_x,forward_z):0.0f;
      const uint8_t awaken_latch_after=memory.U8(object+737);
      QueueMissionAntSpawnTrace({generation,
          call_ordinal,ant_ordinal,caller,r3,r4,r5,r6,r7,f1_bits,object,vtable,faction,
          awaken_latch_after,health,x,y,z,yaw,yaw_valid});
    }
  }
}
void TraceMissionAntRemovalImpl(uint8_t* base,uint32_t object,uint32_t caller) {
  const auto tracked=mission_actor_trace_objects.find(object);
  if(tracked!=mission_actor_trace_objects.end()) {
    const MissionAntTraceObject trace=tracked->second;
    mission_actor_trace_objects.erase(tracked);
    if(MissionActorTraceCaptureEnabled() &&
        trace.generation==mission_actor_trace_generation.load(std::memory_order_relaxed) &&
        edf::console::GuestMemory::Heap(object,738)) {
      const edf::console::GuestMemory memory(base);
      if(memory.U32(object)==trace.vtable && memory.ClassName(object)=="clGiantAnt") {
        const uint32_t removal_ordinal=mission_actor_trace_removals.fetch_add(1,std::memory_order_relaxed);
        REXLOG_INFO("Mission ant lifecycle trace: side=client generation={} removal_ordinal={} call_ordinal={} ant_ordinal={} caller={:#x} object={:#x} vtable={:#x} class=clGiantAnt faction={} health={} x={} y={} z={}",
            trace.generation,removal_ordinal,trace.call_ordinal,trace.ant_ordinal,caller,
            object,trace.vtable,memory.U32(object+604),memory.F32(object+592),
            memory.F32(object+272),memory.F32(object+276),memory.F32(object+280));
      }
    }
  }
}
// The generic object factory reveals the constructed object's RTTI class. For
// player candidates, decode its SGO path using the std::wstring layout already
// used by the console's game-thread object factory.
REX_EXTERN(__imp__sub_821A6E08);
REX_HOOK_RAW(sub_821A6E08) {
  const bool trace=LoadTraceOn();
  const uint32_t caller=uint32_t(ctx.lr);
  const uint32_t core=ctx.r3.u32,matrix=ctx.r4.u32,path=ctx.r5.u32,params=ctx.r6.u32;
  __imp__sub_821A6E08(ctx,base);
  if(!trace || !ctx.r3.u32) return;

  const edf::console::GuestMemory memory(base);
  const uint32_t object=ctx.r3.u32;
  const bool heap_object=edf::console::GuestMemory::Heap(object);
  const uint32_t vtable=heap_object?memory.U32(object):0;
  const std::string class_name=heap_object?memory.ClassName(object):std::string{};
  const bool player_candidate=IsPlayerConstructionCandidate(class_name);
  auto& logs=player_candidate?mission_factory_candidate_samples:mission_factory_other_samples;
  const uint32_t sample=logs.fetch_add(1,std::memory_order_relaxed);
  if(sample>=128) return;
  const std::string path_text=player_candidate?ReadGuestWidePath(base,path):std::string{};
  const int32_t player_index=class_name=="clPlayerObject" &&
      edf::console::GuestMemory::Heap(object,496)
      ? std::bit_cast<int32_t>(memory.U32(object+492)) : -1;
  REXLOG_INFO("Game object factory sample: group={} sample={} caller={:#x} core={:#x} matrix={:#x} path_object={:#x} path={} params={:#x} object={:#x} vtable={:#x} class={} player_index={} heap_object={}",
      player_candidate?"player_candidate":"other",sample,caller,core,matrix,path,
      player_candidate?(path_text.empty()?"<empty>":path_text):"<not-read>",params,object,vtable,
      class_name.empty()?"<unknown>":class_name,player_index,heap_object);
}

// clGameObject_Base faction registration follows object construction. Correlate
// its player index with the factory/path record by object address.
REX_EXTERN(__imp__sub_820D5ED0);
REX_HOOK_RAW(sub_820D5ED0) {
  const uint32_t object=ctx.r3.u32,faction=ctx.r4.u32,notify=ctx.r5.u32;
  const uint32_t caller=uint32_t(ctx.lr);
  __imp__sub_820D5ED0(ctx,base);
  if(!LoadTraceOn()) return;
  const edf::console::GuestMemory memory(base);
  const std::string class_name=memory.ClassName(object);
  if(!IsPlayerConstructionCandidate(class_name)) return;
  const uint32_t sample=mission_factory_faction_samples.fetch_add(1,std::memory_order_relaxed);
  if(sample>=128) return;
  const int32_t player_index=class_name=="clPlayerObject" &&
      edf::console::GuestMemory::Heap(object,496)
      ? std::bit_cast<int32_t>(memory.U32(object+492)) : -1;
  const float health=edf::console::GuestMemory::Heap(object,596)?memory.F32(object+592):0.0f;
  REXLOG_INFO("Candidate SetFaction sample: sample={} caller={:#x} object={:#x} class={} faction={} notify={} player_index={} health={}",
    sample,caller,object,class_name,faction,notify,player_index,health);
}

// Generated input setup routines: log bounded raw registers so a second-seat
// path can be correlated without assuming their C++ or script argument layouts.
REX_EXTERN(__imp__sub_820A0220);
REX_HOOK_RAW(sub_820A0220) {
  const uint32_t caller=uint32_t(ctx.lr),r3=ctx.r3.u32,r4=ctx.r4.u32,r5=ctx.r5.u32,r6=ctx.r6.u32;
  __imp__sub_820A0220(ctx,base);
  TracePlayerSetupCall("820A0220",mission_player_index_init_samples,caller,r3,r4,r5,r6,ctx.r3.u32);
}
REX_EXTERN(__imp__sub_820BDC68);
REX_HOOK_RAW(sub_820BDC68) {
  const uint32_t caller=uint32_t(ctx.lr),r3=ctx.r3.u32,r4=ctx.r4.u32,r5=ctx.r5.u32,r6=ctx.r6.u32;
  __imp__sub_820BDC68(ctx,base);
  TracePlayerSetupCall("820BDC68",mission_player_count_samples,caller,r3,r4,r5,r6,ctx.r3.u32);
}
REX_EXTERN(__imp__sub_821A4170);
REX_HOOK_RAW(sub_821A4170) {
  const bool trace=REXCVAR_GET(edf_native_load_timings);
  const uint32_t manager=ctx.r3.u32,mode=ctx.r4.u32;
  if(trace) REXLOG_INFO("Native load request: begin manager={:#x} mode={} caller={:#x}",manager,mode,ctx.lr);
  LoadTraceEvent(mode?"begin_loading_mission":"begin_loading_menu",uint32_t(ctx.lr),true);
  __imp__sub_821A4170(ctx,base);
  if(trace) REXLOG_INFO("Native load request: armed manager={:#x}",manager);
}
REX_EXTERN(__imp__sub_821A41E8);
REX_HOOK_RAW(sub_821A41E8) {
  const bool trace=REXCVAR_GET(edf_native_load_timings);
  const uint32_t manager=ctx.r3.u32;
  if(trace) REXLOG_INFO("Native load request: finish manager={:#x} caller={:#x}",manager,ctx.lr);
  LoadTraceEvent("end_loading",uint32_t(ctx.lr));
  __imp__sub_821A41E8(ctx,base);
  if(trace) REXLOG_INFO("Native load request: disarmed manager={:#x}",manager);
}
REX_EXTERN(__imp__sub_821A4FC0);
REX_HOOK_RAW(sub_821A4FC0) {
  const bool trace=REXCVAR_GET(edf_native_load_timings);
  const uint32_t manager=ctx.r3.u32;
  if(trace) REXLOG_INFO("Native resource worker: begin manager={:#x}",manager);
  if(trace) {
    // Snapshot identities only; do not invoke callbacks or replace the original
    // traversal. r4 owns the list, while r3 supplies the slot-6 argument.
    // The bounded snapshot is diagnostic, not an ownership certificate.
    try {
      const edf::native::GuestReader reader(base);
      const uint32_t list_owner=ctx.r4.u32;
      const uint32_t owner=reader.Word(reader.Add(list_owner,132));
      const uint32_t owner_vtable=reader.Word(owner);
      REXLOG_INFO("Native resource callbacks: manager={:#x} owner={:#x} enter={:#x} leave={:#x}",
        manager,owner,reader.Word(reader.Add(owner_vtable,32)),reader.Word(reader.Add(owner_vtable,36)));
      const uint32_t sentinel=reader.Word(reader.Add(list_owner,2232));
      uint32_t node=reader.Word(sentinel);
      uint32_t count=0;
      for(;node!=sentinel && count<256;++count) {
        const uint32_t object=reader.Word(reader.Add(node,12));
        const uint32_t vtable=reader.Word(object);
        REXLOG_INFO("Native resource callback: manager={:#x} index={} object={:#x} vtable={:#x} slot6={:#x}",
          manager,count,object,vtable,reader.Word(reader.Add(vtable,24)));
        node=reader.Word(node);
      }
      if(node!=sentinel) REXLOG_WARN("Native resource callback snapshot truncated at {} records",count);
    } catch(const std::exception& error) {
      REXLOG_WARN("Native resource callback snapshot unavailable: {}",error.what());
    }
  }
  edf::native::HookTiming timing(edf::native::HookPhase::ResourceOneShot);
  __imp__sub_821A4FC0(ctx,base);
  timing.Finish();
  if(trace) REXLOG_INFO("Native resource worker: end manager={:#x}",manager);
}
// The loading screen's presenter thread body (clSatoCallback::slot6): it
// loops drawing the loading screen while the resource manager's loading
// flags (+2261 actual, +2262 requested) are set and returns as soon as both
// are clear - the guest has no minimum display time here. Observation only.
REX_EXTERN(__imp__sub_8216EBC0);
REX_HOOK_RAW(sub_8216EBC0) {
  native_load_trace_presenter_thread=true;
  LoadTraceEvent("presenter_start",ctx.r4.u32,true);
  __imp__sub_8216EBC0(ctx,base);
  LoadTraceEvent("presenter_exit",0);
  native_load_trace_presenter_thread=false;
}

}  // namespace

namespace edf::multiplayer {

void TraceMissionAntRemoval(uint8_t* guest_base,uint32_t object,uint32_t caller) {
  ::TraceMissionAntRemovalImpl(guest_base,object,caller);
}

std::vector<MissionAntTraceKey> SnapshotMissionAntTraceKeys(
    uint8_t* guest_base,bool include_dead) {
  std::vector<MissionAntTraceKey> keys;
  if(!guest_base) return keys;
  const uint64_t generation=mission_actor_trace_generation.load(std::memory_order_relaxed);
  if(!generation) return keys;
  const edf::console::GuestMemory memory(guest_base);
  keys.reserve(mission_actor_trace_objects.size());
  for(const auto& [object,trace]:mission_actor_trace_objects) {
    if(trace.generation!=generation || trace.call_ordinal>=kMissionActorTraceCallLimit ||
       trace.ant_ordinal>=kMissionActorTraceAntOrdinalLimit ||
       !edf::console::GuestMemory::Heap(object,738) || memory.U32(object)!=trace.vtable ||
       memory.ClassName(object)!="clGiantAnt" || memory.U32(object+604)!=1) continue;
    if(!include_dead) {
      const float health=memory.F32(object+592);
      const float x=memory.F32(object+272),y=memory.F32(object+276),z=memory.F32(object+280);
      if(memory.U8(object+624)!=0 || !std::isfinite(health) || health<=0.0f ||
         !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) continue;
    }
    keys.push_back({generation,trace.call_ordinal,trace.ant_ordinal,object});
  }
  return keys;
}

uint64_t CurrentMissionAntTraceGeneration() {
  return mission_actor_trace_generation.load(std::memory_order_relaxed);
}

void SetMissionActorNetworkReplicationActive(bool active) {
  mission_actor_network_replication_active.store(active, std::memory_order_relaxed);
}

bool MissionActorNetworkReplicationActive() {
  return mission_actor_network_replication_active.load(std::memory_order_relaxed);
}

}  // namespace edf::multiplayer
