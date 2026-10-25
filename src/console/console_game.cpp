// EDF2027 console - game commands: spawn, killall, god, ammo, teleport, pos, destroy,
// effects, timescale, stats. Engine thread only (console.h), through the game's own code.
//
// Everything here drives guest functions the game itself uses; none of it writes a game
// structure the game does not also write. Addresses are the retail image's (title
// 445007D3); docs/console.md has the evidence for each.
//
//   Spawning (mission-script native #300, sub_820CEBD0, which M001 uses for its ants):
//     path   = L"game:\\Object\\" + name + L".Sgo"            (a guest std::wstring)
//     clResourceManager::MakeObjectCache([0x82578648], path, 1) sub_820AFAC0: loads the
//            SGO and its model/texture dependencies once, synchronously; a cached name
//            returns at once
//     obj    = clCore::CreateObject([0x8257C030], &matrix, path, &params)  sub_821A6E08
//     go     = __RTDynamicCast(obj, 0, Sgs::clObject_Base, clGameObject_Base)  sub_821E9348
//     SetScale(go, 1.0)          sub_820D5430
//     SetFaction(go, 1, true)    sub_820D5ED0 (faction 1 = the invaders)
//     Awaken(go)                 sub_820D4A00 (the script's "start awake" argument)
//   Ground placement (natives #251/#301): sweep from +1000 Y by (0,-10000,0) through
//     sub_821AD100([0x82578678], 0, 0, &start, &move, 0, 0x821A3C78, &result); the hit
//     position is result+16.
//   Killing: clGameObject_Base::ApplyDamage(obj, &damage, force) sub_820D59D0 with damage
//     +24 = health damage; returns 2 on a kill (it sends BASEDTRY and leaves the faction list).
//     Removal: scenegraph mark-subtree sub_821C0ED8 (native #201).
//   Objects: faction manager [0x82578720]: 5 records of 40 bytes at [manager]; record+0
//     heads an intrusive list of object+608 nodes (next at +0). Object +604 faction,
//     +592/+596 health/max, +624 dead, +625 invulnerable (ApplyDamage returns at once),
//     +272 position. The player camera (clPlayerCamera, first on the clCore [0x8257C030]
//     list [core+0] -> [core+12], node+0 next, node+8 object) has its world matrix at
//     +416 (row 2 forward). Core bytes +2260..+2262 are nonzero while loading.
//   Teleport: clSoldierObject's accumulated position +1440 (copied to +272 each update).
//   Weapons (clSoldierObject): [player+1824] array of [player+1832] weapons, 1408 bytes
//     each; +800 magazine size, +804 rounds left (ints).
//   Buildings (clBuilding, clMapArtifact_Base): the debug console's "vanish" (sub_820BE088)
//     finds map objects around a point with the grid query sub_821C5AC0([0x82578678],
//     &center, radius, callback, 0); its callback sub_820B3B10 is hooked here to collect
//     them. Destroying one is clMapArtifact_Base::slot5's death branch (sub_820B38D8):
//     health +392 = 0, sub_820B3818(obj, &payload) (MB_Destp to the children), then
//     slot 0 (obj, &MB_Destr [0x82578660], &payload); clBuilding::slot0 (sub_820B81E0)
//     then collapses it (sub_820B2E78 "fallmodel") and plays a collapse sound.
//   Effects: the ordinary explosion sub_8210FF78([0x82578770], &pos, &dir, scale, r7 = shake).
#include "console.h"
#include "console_guest.h"
#if defined(_WIN32)
#include "multiplayer/mission_actor_trace.h"
#endif
#if defined(EDF2027_BUILD_MULTIPLAYER)
#include "multiplayer/protocol.h"
#endif

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <random>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include "frame_stats.h"
#include "native_graphics/native_console_counters.h"
#include "native_graphics/native_pacing.h"

REXCVAR_DEFINE_INT32(edf_console_spawn_per_tick, 20, "EDF2027",
                     "Console spawn: objects created per simulation tick (larger spawns spread over ticks)")
    .range(1, 1000);
#if defined(EDF2027_BUILD_MULTIPLAYER) && defined(_WIN32)
REXCVAR_DECLARE(bool, edf_mp_actor_trace);
REXCVAR_DECLARE(bool, edf_mp_owner_correction_trace);
REXCVAR_DECLARE(bool, edf_mp_mission_actor_reconcile);
#endif

REX_EXTERN(sub_821E9348);  // __RTDynamicCast
REX_EXTERN(sub_820A0D10);  // std::wstring::assign(const wchar_t*, size)
REX_EXTERN(sub_820A0858);  // std::wstring::~wstring
REX_EXTERN(sub_820AFAC0);  // clResourceManager::MakeObjectCache
REX_EXTERN(sub_821A6E08);  // clCore::CreateObject
REX_EXTERN(sub_820D5430);  // clGameObject_Base scale
REX_EXTERN(sub_820D5ED0);  // clGameObject_Base faction
REX_EXTERN(sub_820D4A00);  // clGameObject_Base awaken (BASENCUT)
REX_EXTERN(sub_820D59D0);  // clGameObject_Base damage
REX_EXTERN(sub_821C0ED8);  // scene graph: mark subtree for removal
#if defined(EDF2027_BUILD_MULTIPLAYER)
REX_EXTERN(__imp__sub_821C0ED8);  // scene graph import used by multiplayer cleanup
#endif
REX_EXTERN(sub_821AD100);  // collision sweep
REX_EXTERN(sub_821C5AC0);  // grid sphere query with callback
REX_EXTERN(sub_820B3818);  // map artifact: destroy children (MB_Destp)
REX_EXTERN(sub_8210FF78);  // ordinary explosion effect

namespace edf::console {
namespace {
// ---- Guest addresses -------------------------------------------------------------------
constexpr uint32_t kCorePointer = 0x8257C030;        // clCore*
constexpr uint32_t kResourceManagerPointer = 0x82578648;
constexpr uint32_t kCollisionPointer = 0x82578678;   // collision world / map grid
constexpr uint32_t kFactionManagerPointer = 0x82578720;
constexpr uint32_t kEffectManagerPointer = 0x82578770;
constexpr uint32_t kSweepFilter = 0x821A3C78;        // the scatter natives' sweep callback
constexpr uint32_t kVanishCallback = 0x820B3B10;     // hooked below as the collector
constexpr uint32_t kMessageDestroy = 0x82578660;     // "MB_Destr"
constexpr uint32_t kTypeSgsObject = 0x82550D14;      // RTTI Sgs::clObject_Base
constexpr uint32_t kTypeGameObject = 0x82551058;     // RTTI clGameObject_Base
constexpr uint32_t kTypeMapObject = 0x82550CCC;      // RTTI clMapObject_Base
constexpr uint32_t kFactionCount = 5, kFactionRecord = 40, kFactionNode = 608;
constexpr uint32_t kListLimit = 20000;               // walk bound: a corrupt list cannot hang the engine

// ---- Spawn types -----------------------------------------------------------------------
struct SpawnType {
  const char* name;
  const char* sgo;         // Object/<sgo>.Sgo on the disc
  const char* what;
  int faction;             // -1: leave the object's own
  const char* object_class;  // RTTI class, for killall filters
  float altitude = 0;        // above the player instead of on the ground (flyers)
};
// Durabilities from each SGO's go_Durability (the analysis notes' drop table).
constexpr SpawnType kTypes[] = {
    {"ant", "GiantAnt01", "black giant ant (durability 60)", 1, "clGiantAnt"},
    {"ant015", "GiantAnt015", "black giant ant, mission 1 variant (60)", 1, "clGiantAnt"},
    {"redant", "GiantAnt02", "red giant ant (180)", 1, "clGiantAnt"},
    {"queen", "GiantAntQueen", "giant ant queen (2000)", 1, "clGiantAnt"},
    {"spider", "GiantSpider01", "giant spider (45)", 1, "clGiantSpider"},
    {"spiderlord", "GiantSpiderLord", "giant spider lord (2000)", 1, "clGiantSpider"},
    {"ufo", "UfoSmall01", "small UFO gunship (50)", 1, "clUfoSmall01", 30.0f},
    {"ufo2", "UfoSmall02", "small UFO gunship, type 2 (200)", 1, "clUfoSmall01", 30.0f},
    {"ufo3", "UfoSmall03", "small UFO gunship, type 3 (200)", 1, "clUfoSmall01", 30.0f},
    {"carrier", "UfoCarrier01", "UFO carrier / dropship (600)", 1, "clUfoCarrier01", 80.0f},
    {"hector", "AlienTankCC", "Hector walking robot, twin cannon (300)", 1, "clAlienTank01"},
    {"hectorkk", "AlienTankKK", "Hector, KK arms (300)", 1, "clAlienTank01"},
    {"hectormc", "AlienTankMC", "Hector, MC arms (300)", 1, "clAlienTank01"},
    {"hectormm", "AlienTankMM", "Hector, MM arms (300)", 1, "clAlienTank01"},
    {"hectormo", "AlienTankMO", "Hector, MO arms (300)", 1, "clAlienTank01"},
    {"hectoroc", "AlienTankOC", "Hector, OC arms (300)", 1, "clAlienTank01"},
    {"hectoroo", "AlienTankOO", "Hector, OO arms (300)", 1, "clAlienTank01"},
    {"hectorrr", "AlienTankRR", "Hector, RR arms (300)", 1, "clAlienTank01"},
    {"bighector", "AlienTankBCC", "large Hector, twin cannon (1200)", 1, "clAlienTank01"},
    {"bighectormm", "AlienTankBMM", "large Hector, MM arms (1200)", 1, "clAlienTank01"},
    {"bighectorrr", "AlienTankBRR", "large Hector, RR arms (1200)", 1, "clAlienTank01"},
    {"anthill", "AntHill01", "ant hill (800)", 1, "clAntHill"},
    {"monster", "Monster01", "Solomon, the kaiju (8000)", 1, "clMonster01"},
    {"mech", "MonsterMech01", "mechanical kaiju (15000)", 1, "clMonster01Mech"},
    {"walker", "Alien4LegTank01", "four-legged walker boss (5000)", 1, "clAlien4LegTank01"},
    {"civilian", "PeopleM01", "civilian, man (30)", 3, "clFriendPeople"},
    {"civilianf", "PeopleF01", "civilian, woman (30)", 3, "clFriendPeople"},
};
#if defined(EDF2027_BUILD_MULTIPLAYER)
constexpr SpawnType kRemotePlayerType{"remote", "FriendSoldier01", "remote player replica", 0, "clFriendSoldier"};
constexpr SpawnType kRemoteAntType{"server_ant", "GiantAnt01", "server authoritative ant proxy", 1, "clGiantAnt"};
struct RemotePlayerPose {
  float x = 0, y = 0, z = 0;
  float yaw = 0;
};
struct RemotePlayerReplica {
  uint32_t object = 0;
  uint32_t snapshot_tick = 0;
  RemotePlayerPose from{}, to{};
  uint64_t last_sample_tick = 0;
  uint64_t blend_start_tick = 0;
  uint64_t blend_end_tick = 0;
  bool has_pose = false;
};
std::unordered_map<uint16_t, RemotePlayerReplica> remote_player_objects;

struct RemoteActorReplica {
  uint32_t object = 0;
  uint32_t snapshot_tick = 0;
  RemotePlayerPose from{}, to{};
  uint64_t last_sample_tick = 0;
  uint64_t blend_start_tick = 0;
  uint64_t blend_end_tick = 0;
  bool has_pose = false;
};
std::unordered_map<uint32_t, RemoteActorReplica> remote_actor_objects;
std::unordered_set<uint32_t> remote_dead_actor_ids;
struct OwnerCorrectionStats {
  uint16_t player_id = 0;
  uint32_t player_object = 0;
  uint64_t samples = 0;
  uint64_t large_corrections = 0;
  uint64_t next_log_tick = 0;
  double total_distance_cm = 0.0;
  double max_distance_cm = 0.0;
  double max_yaw_mrad = 0.0;

  void Reset() {
    *this = {};
  }
};
// The game clears clPlayerObject+492 to -1 on death. Keep the bridge-owned
// second-seat identity independently so its dead vitals survive until a native
// replacement or the bounded server-managed respawn is observed.
struct HostGuestSeatBinding {
  uint32_t player_object = 0;
  uint64_t mission_generation = 0;
  std::chrono::steady_clock::time_point death_started{};
  uint16_t death_max_health = 0;
  uint16_t remote_player_id = 0;
  uint64_t connection_generation = 0;
  bool death_latched = false;
  bool replacement_eligible = false;
  bool seat_authorized = false;
  bool reassignment_pending = false;
  bool reassignment_had_owned_seat = false;
};
HostGuestSeatBinding host_guest_seat;
// Native EDF actors created by server Luau scripts. These guest addresses are
// used only from the game thread and are never copied to the networking worker.
std::map<uint32_t, uint32_t> host_server_actor_objects;
uint64_t host_server_actor_generation = 0;

bool ApplyServerTransform(const GuestMemory& memory, uint32_t object, float x, float y, float z, float yaw) {
  if (!GuestMemory::Heap(object, 1452)) return false;
  const float c = std::cos(yaw), s = std::sin(yaw);
  const float rows[16] = {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, x, y, z, 1};
  for (uint32_t i = 0; i < 16; ++i) memory.WF32(object + 224 + i * 4, rows[i]);
  // The soldier's movement/collision state keeps a second position at +1440.
  memory.WF32(object + 1440, x);
  memory.WF32(object + 1444, y);
  memory.WF32(object + 1448, z);
  return true;
}

#if defined(EDF2027_BUILD_MULTIPLAYER)
bool ApplyServerPlayerVitals(const GuestMemory& memory, uint32_t object,
                             const edf::net::PlayerState& state) {
  if (!state.health_valid || !GuestMemory::Heap(object, 628) ||
      memory.ClassName(object) != "clPlayerObject" || memory.U32(object + 492) != 0)
    return false;
  const float local_maximum = memory.F32(object + 596);
  if (!std::isfinite(local_maximum) || local_maximum <= 0.0f) return false;
  const float health_fraction = float(state.health) / float(state.max_health);
  const float local_health = state.alive ? local_maximum * health_fraction : 0.0f;
  memory.WF32(object + 592, std::clamp(local_health, 0.0f, local_maximum));
  memory.W8(object + 624, state.alive ? 0 : 1);
  return true;
}

void LatchHostGuestSeatDeath(const GuestMemory& memory, uint32_t object) {
  if (host_guest_seat.death_latched) return;
  host_guest_seat.death_latched = true;
  host_guest_seat.death_started = std::chrono::steady_clock::now();
  const float maximum_health = memory.F32(object + 596);
  if (std::isfinite(maximum_health) && maximum_health > 0.0f) {
    host_guest_seat.death_max_health = static_cast<uint16_t>(std::clamp<long>(
        std::lround(std::min(maximum_health, 65535.0f)), 1, 65535));
  }
}

void ClearHostGuestSeatDeath() {
  host_guest_seat.death_latched = false;
  host_guest_seat.death_started = {};
  host_guest_seat.death_max_health = 0;
  host_guest_seat.replacement_eligible = false;
}

TrustedGuestPose LatchedHostGuestSeatDeathPose() {
  TrustedGuestPose pose;
  pose.health_valid = host_guest_seat.death_max_health != 0;
  pose.alive = false;
  pose.health = 0;
  pose.max_health = host_guest_seat.death_max_health;
  return pose;
}
#endif

bool ApplyRemoteActorTransform(const GuestMemory& memory, uint32_t object,
                               float x, float y, float z, float yaw) {
  if (!GuestMemory::Heap(object, 288)) return false;
  const float c = std::cos(yaw), s = std::sin(yaw);
  const float rows[16] = {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, x, y, z, 1};
  for (uint32_t i = 0; i < 16; ++i) memory.WF32(object + 224 + i * 4, rows[i]);
  return true;
}

bool ToNetworkCentimeters(float meters, int32_t& centimeters) {
  if (!std::isfinite(meters)) return false;
  const double scaled = std::round(double(meters) * 100.0);
  constexpr double kWorldLimitCm = 10'000'000.0;
  if (scaled < -kWorldLimitCm || scaled > kWorldLimitCm) return false;
  centimeters = static_cast<int32_t>(scaled);
  return true;
}

bool ServerResetTransformMatches(const GuestMemory& memory, uint32_t object,
                                 float x, float y, float z) {
  if (!GuestMemory::Heap(object, 1452)) return false;
  constexpr float kResetToleranceMeters = 0.25f;
  const auto near = [=](float a, float b) {
    return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= kResetToleranceMeters;
  };
  return near(memory.F32(object + 272), x) && near(memory.F32(object + 276), y) &&
         near(memory.F32(object + 280), z) &&
         near(memory.F32(object + 1440), x) && near(memory.F32(object + 1444), y) &&
         near(memory.F32(object + 1448), z);
}

void ClearRemotePlayers(EngineFrame& frame, const GuestMemory& memory, bool remove_objects) {
  // During a load, the game may tear down the guest heap without traversing each
  // replica through the removal native. Forget those addresses without dereferencing them.
  if (!remove_objects) {
    remote_player_objects.clear();
    return;
  }
  std::vector<uint32_t> objects;
  std::unordered_set<uint32_t> object_set;
  for (const auto& [id, replica] : remote_player_objects) {
    (void)id;
    if (replica.object && object_set.insert(replica.object).second &&
        memory.ClassName(replica.object) == "clFriendSoldier")
      objects.push_back(replica.object);
  }
  remote_player_objects.clear();
  if (!objects.empty()) {
    GuestCalls guest(*frame.ctx, frame.base);
    for (const uint32_t object : objects)
      guest.Call(sub_821C0ED8, GuestCalls::R({object}));
  }
}

void ClearRemoteActors(EngineFrame& frame, const GuestMemory& memory, bool remove_objects) {
  remote_dead_actor_ids.clear();
  if (!remove_objects) {
    remote_actor_objects.clear();
    return;
  }
  std::vector<uint32_t> objects;
  std::unordered_set<uint32_t> object_set;
  for (const auto& [id, replica] : remote_actor_objects) {
    (void)id;
    if (replica.object && object_set.insert(replica.object).second &&
        memory.ClassName(replica.object) == "clGiantAnt")
      objects.push_back(replica.object);
  }
  remote_actor_objects.clear();
  if (!objects.empty()) {
    GuestCalls guest(*frame.ctx, frame.base);
    for (const uint32_t object : objects)
      guest.Call(sub_821C0ED8, GuestCalls::R({object}));
  }
}

void ForgetRemoteReplicaObject(uint32_t object) {
  std::erase_if(remote_player_objects, [=](const auto& entry) {
    return entry.second.object == object;
  });
  std::erase_if(remote_actor_objects, [=](const auto& entry) {
    return entry.second.object == object;
  });
}
#endif
const SpawnType* FindType(std::string_view name) {
  for (const auto& type : kTypes)
    if (Lower(name) == Lower(type.name) || Lower(name) == Lower(type.sgo)) return &type;
  return nullptr;
}
std::vector<std::string> TypeNames(bool with_list) {
  std::vector<std::string> names;
  if (with_list) names.push_back("list");
  for (const auto& type : kTypes) names.push_back(type.name);
  return names;
}

// ---- Objects -----------------------------------------------------------------------------
bool EngineIdle(const GuestMemory& m) {
  const uint32_t core = m.U32(kCorePointer);
  if (!GuestMemory::Heap(core, 2264)) return false;
  return !m.U8(core + 2260) && !m.U8(core + 2261) && !m.U8(core + 2262);
}
// The objects of one faction's list.
std::vector<uint32_t> FactionObjects(const GuestMemory& m, uint32_t faction) {
  std::vector<uint32_t> objects;
  const uint32_t manager = m.U32(kFactionManagerPointer);
  if (!GuestMemory::Heap(manager) || faction >= kFactionCount) return objects;
  const uint32_t records = m.U32(manager);
  if (!GuestMemory::Heap(records, kFactionCount * kFactionRecord)) return objects;
  uint32_t node = m.U32(records + faction * kFactionRecord);
  for (uint32_t n = 0; node && n < kListLimit; ++n) {
    if (!GuestMemory::Heap(node, 8) || node < kFactionNode) break;
    objects.push_back(node - kFactionNode);
    node = m.U32(node);
  }
  return objects;
}
// (The core's lists, [core+0..+12) and [core+44..+56) that the step dispatcher
// sub_821A4BA0 walks, hold the top-level managers -- clPlayerCamera,
// clGameObject_Manager, clMapObjectManager, clEffectObjectManager, ... -- not the
// objects themselves, so object counts come from the faction lists and the map grid.)
struct Vec3 { float x = 0, y = 0, z = 0; };
Vec3 Position(const GuestMemory& m, uint32_t object) { return {m.F32(object + 272), m.F32(object + 276), m.F32(object + 280)}; }
// The view's heading: the player camera's world matrix (+416; row 2 forward), the object
// the retail debug console's "object" command also spawns in front of (sub_820BE738:
// the first object of [core+0], matrix +416, 5 units along row 2). 0 = +Z, pi/2 = +X.
uint32_t FindCamera(const GuestMemory& m) {
  const uint32_t core = m.U32(kCorePointer);
  if (!GuestMemory::Heap(core, 16)) return 0;
  const uint32_t end = m.U32(core + 12);
  uint32_t node = m.U32(core);
  for (uint32_t n = 0; node && node != end && n < 64; ++n) {
    if (!GuestMemory::Heap(node, 12)) break;
    if (const uint32_t object = m.U32(node + 8); m.ClassName(object) == "clPlayerCamera") return object;
    node = m.U32(node);
  }
  return 0;
}
float HeadingRadians(const GuestMemory& m, uint32_t /*player*/) {
  const uint32_t camera = FindCamera(m);
  if (!GuestMemory::Heap(camera, 480)) return 0;
  const float fx = m.F32(camera + 416 + 32), fz = m.F32(camera + 416 + 40);
  if (!std::isfinite(fx) || !std::isfinite(fz) || (fx == 0 && fz == 0)) return 0;
  return std::atan2(fx, fz);
}
uint32_t FindPlayerByIndex(const GuestMemory& m, uint32_t index) {
  for (const uint32_t object : FactionObjects(m, 0))
    if (m.ClassName(object) == "clPlayerObject" && m.U32(object + 492) == index) return object;
  return 0;
}
uint32_t FindPlayer(const GuestMemory& m) { return FindPlayerByIndex(m, 0); }
std::string Fmt(const char* format, auto... args) {
  char text[512];
  std::snprintf(text, sizeof(text), format, args...);
  return text;
}

// ---- Guest operations ----------------------------------------------------------------------
float GroundY(GuestCalls& g, float x, float y, float z, bool* hit_out = nullptr) {
  const auto& m = g.memory();
  const uint32_t mark = g.Mark();
  const uint32_t start = g.Alloc(16), move = g.Alloc(16), result = g.Alloc(96);
  m.WF32(start, x); m.WF32(start + 4, y + 1000.0f); m.WF32(start + 8, z); m.WF32(start + 12, 1.0f);
  m.WF32(move + 4, -10000.0f); m.WF32(move + 12, 1.0f);
  m.WF32(result + 28, 1.0f); m.WF32(result + 44, 1.0f);
  const uint32_t hit = g.Call(sub_821AD100, GuestCalls::R({m.U32(kCollisionPointer), 0, 0, start, move, 0, kSweepFilter,
                                                           result})) & 0xFF;
  const float ground = m.F32(result + 20);
  g.Release(mark);
  if (hit_out) *hit_out = hit != 0;
  return hit && std::isfinite(ground) ? ground : y;
}

// Creates one object of `type` at (x, y, z) facing `yaw`; returns the game object (or 0).
uint32_t CreateObject(GuestCalls& g, const SpawnType& type, float x, float y, float z, float yaw, bool awake) {
  const auto& m = g.memory();
  const uint32_t mark = g.Mark();
  const std::string path_text = std::string("game:\\Object\\") + type.sgo + ".Sgo";
  const uint32_t literal = g.WideString(path_text);
  const uint32_t path = g.Alloc(32);  // MSVC std::wstring: +4 buffer, +20 size, +24 capacity
  m.W32(path + 24, 7);
  g.Call(sub_820A0D10, GuestCalls::R({path, literal, uint32_t(path_text.size())}));
  g.Call(sub_820AFAC0, GuestCalls::R({m.U32(kResourceManagerPointer), path, 1}));
  const uint32_t matrix = g.Alloc(64), params = g.Alloc(80);
  const float c = std::cos(yaw), s = std::sin(yaw);
  const float rows[16] = {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, x, y, z, 1};
  for (int i = 0; i < 16; ++i) m.WF32(matrix + uint32_t(i) * 4, rows[i]);
  for (int i = 0; i < 4; ++i) m.WF32(params + uint32_t(i) * 20, 1.0f);  // identity; the rest zero
  const uint32_t object = g.Call(sub_821A6E08, GuestCalls::R({m.U32(kCorePointer), matrix, path, params}));
  uint32_t game_object = 0;
  if (object) {
    game_object = g.Call(sub_821E9348, GuestCalls::R({object, 0, kTypeSgsObject, kTypeGameObject, 0}));
    if (game_object) {
      g.Call(sub_820D5430, GuestCalls::R({game_object}, {1.0}));
      if (type.faction >= 0) g.Call(sub_820D5ED0, GuestCalls::R({game_object, uint32_t(type.faction), 1}));
      if (awake) g.Call(sub_820D4A00, GuestCalls::R({game_object}));
    }
  }
  g.Call(sub_820A0858, GuestCalls::R({path}));
  g.Release(mark);
  return game_object ? game_object : object;
}

// 2 = killed, 1 = damaged but alive, 0/3 = refused (invulnerable or already dead).
uint32_t Kill(GuestCalls& g, uint32_t object) {
  const auto& m = g.memory();
  const uint32_t mark = g.Mark();
  const uint32_t damage = g.Alloc(128);
  m.W32(damage + 20, 0xFFFFFFFFu);  // no faction
  m.WF32(damage + 24, 1.0e9f);
  const uint32_t result = g.Call(sub_820D59D0, GuestCalls::R({object, damage, 1}));
  g.Release(mark);
  return result;
}

// The objects the map grid holds within `radius` of `center` (the vanish query).
thread_local std::vector<uint32_t>* collecting = nullptr;
// The grid's box: origin at grid+80..+88 and [grid+64] holding the cells per axis (+8) and
// the cell size (+12), as its cell lookup sub_821C4BE8 reads them. The query refuses a box
// whose two corners both fall outside (sub_821C4D00), so a very large radius asks for
// the whole grid instead and filters by distance.
struct GridBox { Vec3 center; float half = 0; };
std::optional<GridBox> GridBounds(const GuestMemory& m) {
  const uint32_t grid = m.U32(kCollisionPointer);
  if (!GuestMemory::Heap(grid, 96)) return std::nullopt;
  const uint32_t cells = m.U32(grid + 64);
  if (!GuestMemory::Heap(cells, 16)) return std::nullopt;
  const float size = m.F32(cells + 12) * float(m.U32(cells + 8));
  if (!(size > 0) || !std::isfinite(size)) return std::nullopt;
  const float half = size * 0.5f;
  return GridBox{{m.F32(grid + 80) + half, m.F32(grid + 84) + half, m.F32(grid + 88) + half}, half};
}
std::vector<uint32_t> GridQuery(GuestCalls& g, Vec3 center, float radius);
std::vector<uint32_t> GridObjects(GuestCalls& g, Vec3 center, float radius) {
  const auto box = GridBounds(g.memory());
  if (box && radius < box->half) {
    auto objects = GridQuery(g, center, radius);
    if (!objects.empty()) return objects;
  }
  if (!box) return GridQuery(g, center, radius);
  // Whole grid, then keep those within the radius (horizontally, by their bounds centre).
  auto objects = GridQuery(g, box->center, box->half);
  const auto& m = g.memory();
  std::erase_if(objects, [&](uint32_t object) {
    const float dx = m.F32(object + 160) - center.x, dz = m.F32(object + 168) - center.z;
    return dx * dx + dz * dz > radius * radius;
  });
  return objects;
}
std::vector<uint32_t> GridQuery(GuestCalls& g, Vec3 center, float radius) {
  const auto& m = g.memory();
  std::vector<uint32_t> objects;
  const uint32_t mark = g.Mark();
  const uint32_t point = g.Alloc(16);
  m.WF32(point, center.x); m.WF32(point + 4, center.y); m.WF32(point + 8, center.z); m.WF32(point + 12, 1.0f);
  collecting = &objects;
  try {
    g.Call(sub_821C5AC0, GuestCalls::R({m.U32(kCollisionPointer), point, 0, kVanishCallback, 0}, {double(radius)}));
  } catch (...) { collecting = nullptr; throw; }
  collecting = nullptr;
  g.Release(mark);
  std::sort(objects.begin(), objects.end());
  objects.erase(std::unique(objects.begin(), objects.end()), objects.end());
  return objects;
}
struct MapObject { uint32_t object; Vec3 center; float distance; std::string cls; };
std::vector<MapObject> StandingMapObjects(GuestCalls& g, Vec3 center, float radius) {
  const auto& m = g.memory();
  std::vector<MapObject> out;
  for (const uint32_t object : GridObjects(g, center, radius)) {
    if (!GuestMemory::Heap(object, 400)) continue;
    const uint32_t map_object = g.Call(sub_821E9348, GuestCalls::R({object, 0, kTypeSgsObject, kTypeMapObject, 0}));
    if (!map_object || !(m.F32(map_object + 392) > 0)) continue;  // not a map object, or already down
    MapObject entry{map_object, {m.F32(map_object + 160), m.F32(map_object + 164), m.F32(map_object + 168)}, 0,
                    m.ClassName(map_object)};
    const float dx = entry.center.x - center.x, dz = entry.center.z - center.z;
    entry.distance = std::sqrt(dx * dx + dz * dz);
    out.push_back(std::move(entry));
  }
  std::sort(out.begin(), out.end(), [](const MapObject& a, const MapObject& b) { return a.distance < b.distance; });
  return out;
}
void DestroyMapObject(GuestCalls& g, const MapObject& target) {
  const auto& m = g.memory();
  const uint32_t mark = g.Mark();
  // The death branch's payload: impact point, direction, and a damage record whose +28 is
  // the collapse power sub_820B2E78 reads.
  const uint32_t payload = g.Alloc(48), record = g.Alloc(32);
  m.WF32(payload, target.center.x); m.WF32(payload + 4, target.center.y); m.WF32(payload + 8, target.center.z);
  m.WF32(payload + 12, 1.0f);
  m.WF32(payload + 20, -1.0f); m.WF32(payload + 28, 1.0f);
  m.W32(payload + 32, record);
  m.WF32(record + 24, 1.0e6f);
  m.WF32(record + 28, 1.0f);
  m.WF32(target.object + 392, 0.0f);
  g.Call(sub_820B3818, GuestCalls::R({target.object, payload}));
  const uint32_t vtable = m.U32(target.object);
  if (GuestMemory::Image(vtable)) g.CallAddress(m.U32(vtable), GuestCalls::R({target.object, kMessageDestroy, payload}));
  g.Release(mark);
}
void Explosion(GuestCalls& g, Vec3 at, float scale, bool shake) {
  const auto& m = g.memory();
  const uint32_t mark = g.Mark();
  const uint32_t position = g.Alloc(16), direction = g.Alloc(16);
  m.WF32(position, at.x); m.WF32(position + 4, at.y); m.WF32(position + 8, at.z); m.WF32(position + 12, 1.0f);
  m.WF32(direction + 4, 1.0f);
  g.Call(sub_8210FF78, GuestCalls::R({m.U32(kEffectManagerPointer), position, direction, 0, shake ? 1u : 0u}, {double(scale)}));
  g.Release(mark);
}

// ---- Per-tick state ------------------------------------------------------------------------
std::atomic<bool> god{false}, god_release{false}, infinite_ammo{false};
struct Job {
  std::string label;
  std::function<bool(GuestCalls&, EngineFrame&)> step;  // true: done
};
std::mutex jobs_mutex;
std::vector<Job> jobs;
void AddJob(Job job) {
  std::lock_guard lock(jobs_mutex);
  jobs.push_back(std::move(job));
}

// Objects the console created, for "killall spawned". Engine thread only. An address the
// game frees and reuses for one of its own objects would be taken for ours; the set is
// pruned against the live faction list on every killall.
std::unordered_set<uint32_t>& Spawned() {
  static std::unordered_set<uint32_t> spawned;
  return spawned;
}

// Ring placements around a centre: `count` points from `first_radius` outwards, about
// `spacing` apart, starting at `yaw0` (in front of the player).
struct Placement { float x, z; };
std::vector<Placement> Rings(float cx, float cz, float first_radius, int count, float yaw0, float spacing = 5.0f) {
  std::vector<Placement> out;
  float radius = first_radius;
  if (radius <= 0.01f) { out.push_back({cx, cz}); radius = spacing; }
  while (int(out.size()) < count) {
    const int capacity = std::max(6, int(2.0f * std::numbers::pi_v<float> * radius / spacing));
    const int here = std::min(capacity, count - int(out.size()));
    for (int i = 0; i < here; ++i) {
      const float angle = yaw0 + 2.0f * std::numbers::pi_v<float> * float(i) / float(here);
      out.push_back({cx + std::sin(angle) * radius, cz + std::cos(angle) * radius});
    }
    radius += spacing * 1.2f;
  }
  return out;
}

void Spawn(Invocation& in) {
  auto& ctx = *static_cast<ExecContext*>(in.host());
  const GuestMemory m(ctx.frame->base);
  if (Lower(in.str(0)) == "list") {
    in.Print("spawn types (name or SGO name):");
    for (const auto& type : kTypes) in.Print(Fmt("  %-12s %-16s %s", type.name, type.sgo, type.what));
    in.Print("There is no golden ant in EDF 2017's data; the disc's ant SGOs are GiantAnt01/015/02 and the queen.");
    return;
  }
  const SpawnType* type = FindType(in.str(0));
  const uint32_t player = FindPlayer(m);
  if (!player) { in.Error("spawn: no player object (not in a mission)"); return; }
  const int count = in.has(1) ? int(in.integer(1)) : 1;
  const Vec3 origin = Position(m, player);
  const float heading = HeadingRadians(m, player);
  std::vector<Placement> spots;
  float base_y = origin.y;
  if (in.count() >= 5) {
    base_y = float(in.number(3));
    spots = Rings(float(in.number(2)), float(in.number(4)), 0.0f, count, heading);
  } else {
    const float distance = in.has(2) ? float(in.number(2)) : 30.0f;
    if (count == 1) spots.push_back({origin.x + std::sin(heading) * distance, origin.z + std::cos(heading) * distance});
    else spots = Rings(origin.x, origin.z, distance, count, heading);
  }
  in.Print(Fmt("spawn: %d x %s (%s) queued, %d per tick", count, type->name, type->sgo, REXCVAR_GET(edf_console_spawn_per_tick)));
  struct State { size_t next = 0; int created = 0, failed = 0, grounded = 0; };
  auto state = std::make_shared<State>();
  const SpawnType chosen = *type;
  AddJob({std::string("spawn ") + type->name, [=](GuestCalls& g, EngineFrame& frame) {
            const GuestMemory mem(frame.base);
            const uint32_t now_player = FindPlayer(mem);
            if (!now_player || !EngineIdle(mem)) {
              Service::Get().Print(Severity::kWarn, Fmt("spawn %s: mission ended after %d of %zu", chosen.name, state->created, spots.size()));
              return true;
            }
            const Vec3 target = Position(mem, now_player);
            int budget = REXCVAR_GET(edf_console_spawn_per_tick);
            while (budget-- > 0 && state->next < spots.size()) {
              const auto spot = spots[state->next++];
              bool hit = false;
              float y = base_y + chosen.altitude;  // flyers hover above the player's height
              if (chosen.altitude <= 0) {
                y = GroundY(g, spot.x, base_y, spot.z, &hit);
                state->grounded += hit;
              }
              const float yaw = std::atan2(target.x - spot.x, target.z - spot.z);
              if (const uint32_t object = CreateObject(g, chosen, spot.x, y, spot.z, yaw, true)) {
                ++state->created;
                Spawned().insert(object);
              } else {
                ++state->failed;
              }
            }
            if (state->next < spots.size()) return false;
            Service::Get().Print(state->failed ? Severity::kWarn : Severity::kOk,
                                 Fmt("spawn %s: created %d (%d failed, %d placed on the ground by the sweep) at tick %llu",
                                     chosen.name, state->created, state->failed, state->grounded,
                                     static_cast<unsigned long long>(frame.tick)));
            return true;
          }});
}

// "killall [type|class|all|spawned] [remove]"
void KillAll(Invocation& in) {
  auto& ctx = *static_cast<ExecContext*>(in.host());
  GuestCalls g(*ctx.frame->ctx, ctx.frame->base);
  const auto& m = g.memory();
  std::string filter;
  bool remove = false, spawned_only = false;
  for (size_t i = 0; i < in.count(); ++i) {
    const auto word = Lower(in.str(i));
    if (word == "remove") remove = true;
    else if (word == "spawned") spawned_only = true;
    else if (word != "all") {
      if (const auto* type = FindType(word)) filter = type->object_class;
      else filter = in.str(i);
    }
  }
  std::map<std::string, int> killed, survived;
  const auto enemies = FactionObjects(m, 1);
  for (const uint32_t object : enemies) {
    const auto cls = m.ClassName(object);
    if (!filter.empty() && Lower(cls) != Lower(filter)) continue;
    if (spawned_only && !Spawned().contains(object)) continue;
    if (remove) { g.Call(sub_821C0ED8, GuestCalls::R({object})); ++killed[cls]; continue; }
    const uint32_t result = Kill(g, object);
    if (result == 2) ++killed[cls];
    else ++survived[cls];
  }
  {
    const std::unordered_set<uint32_t> live(enemies.begin(), enemies.end());
    std::erase_if(Spawned(), [&](uint32_t object) { return !live.contains(object); });
  }
  int total = 0;
  std::string detail;
  for (const auto& [cls, n] : killed) { total += n; detail += Fmt(" %s=%d", cls.c_str(), n); }
  in.Ok(Fmt("killall: %s %d of %zu enemies%s", remove ? "removed" : "killed", total, enemies.size(), detail.c_str()));
  for (const auto& [cls, n] : survived) in.Warn(Fmt("killall: %d %s survived (invulnerable or scripted); try 'killall %s remove'", n, cls.c_str(), cls.c_str()));
}

void Destroy(Invocation& in) {
  auto& ctx = *static_cast<ExecContext*>(in.host());
  GuestCalls g(*ctx.frame->ctx, ctx.frame->base);
  const auto& m = g.memory();
  const uint32_t player = FindPlayer(m);
  const auto mode = Lower(in.str(0));
  float radius = 100.0f;
  size_t first_limit_arg = 1;
  if (mode == "all") radius = 100000.0f;
  else if (mode == "radius") { radius = in.has(1) ? float(in.number(1)) : 100.0f; first_limit_arg = 2; }
  else if (mode == "nearest") first_limit_arg = 2;
  else if (ParseFloat(mode)) radius = float(*ParseFloat(mode));
  else { in.Error("destroy: usage: destroy <radius R | all | nearest [n] | R> [max]"); return; }
  size_t limit = SIZE_MAX;
  if (mode == "nearest") {
    limit = in.has(1) ? size_t(std::max<int64_t>(1, in.integer(1))) : 1;
    radius = 100000.0f;
  } else if (in.has(first_limit_arg)) {
    limit = size_t(std::max<int64_t>(1, in.integer(first_limit_arg)));
  }
  const Vec3 center = Position(m, player);
  auto targets = StandingMapObjects(g, center, radius);
  const size_t found = targets.size();
  if (targets.size() > limit) targets.resize(limit);
  std::map<std::string, int> classes;
  for (const auto& target : targets) {
    DestroyMapObject(g, target);
    ++classes[target.cls];
  }
  std::string detail;
  for (const auto& [cls, n] : classes) detail += Fmt(" %s=%d", cls.c_str(), n);
  in.Ok(Fmt("destroy: %zu of %zu standing map objects within %.0f%s", targets.size(), found, radius, detail.c_str()));
}

void Effects(Invocation& in) {
  auto& ctx = *static_cast<ExecContext*>(in.host());
  const GuestMemory m(ctx.frame->base);
  const int count = int(in.integer(0));
  const float radius = in.has(1) ? float(in.number(1)) : 15.0f;
  const float scale = in.has(2) ? float(in.number(2)) : 3.0f;
  const int per_tick = in.has(3) ? int(in.integer(3)) : count;
  const uint32_t player = FindPlayer(m);
  const Vec3 center = Position(m, player);
  const float heading = HeadingRadians(m, player);
  auto next = std::make_shared<int>(0);
  auto random = std::make_shared<std::mt19937>(uint32_t(ctx.frame->tick));
  AddJob({"effects", [=](GuestCalls& g, EngineFrame& frame) {
            std::uniform_real_distribution<float> unit(0.0f, 1.0f);
            for (int i = 0; i < per_tick && *next < count; ++i, ++(*next)) {
              const float angle = heading + 2.0f * std::numbers::pi_v<float> * unit(*random);
              const float r = radius * std::sqrt(unit(*random));
              const float x = center.x + std::sin(angle) * r, z = center.z + std::cos(angle) * r;
              const float y = GroundY(g, x, center.y, z) + 1.0f;
              Explosion(g, {x, y, z}, scale, false);
            }
            if (*next < count) return false;
            Service::Get().Print(Severity::kOk, Fmt("effects: %d explosions done at tick %llu", count,
                                                    static_cast<unsigned long long>(frame.tick)));
            return true;
          }});
  in.Print(Fmt("effects: %d explosions within %.0f of the player, scale %.1f, %d per tick", count, radius, scale, per_tick));
}

void Stats(Invocation& in) {
  auto& ctx = *static_cast<ExecContext*>(in.host());
  const GuestMemory m(ctx.frame->base);
  auto& service = Service::Get();
  const auto& frames = CurrentFrameStats();
  auto& timing = service.timing();
  in.Print(Fmt("frames: %.1f FPS, avg %.2f ms, min %.2f, max %.2f, 1%% low %.2f ms (5 s window, %u frames total)",
               frames.fps.load(), frames.average_ms.load(), frames.minimum_ms.load(), frames.maximum_ms.load(),
               frames.low_1_percent_ms.load(), frames.total_frames.load()));
  in.Print(Fmt("simulation: tick %llu, %.1f steps/s, step dispatch avg %.2f ms, max %.2f ms (last second), timescale %.2f",
               static_cast<unsigned long long>(ctx.frame->tick), timing.steps_per_second.load(),
               timing.dispatch_ms_avg.load(), timing.dispatch_ms_max.load(),
               edf::native::NativeTimeScaleRequest().load() ));
  const auto& renderer = edf::native::ConsoleCounters();
  in.Print(Fmt("effects (last native frame): %u effect objects walked, %u filed for drawing, %u immediate draws "
               "(per-pass GPU times and draw counts: edf_native_gpu_timings / edf_native_frame_times)",
               renderer.effects_visited.load(), renderer.effects_filed.load(), renderer.effects_drawn.load()));
  static const char* kFactionNames[kFactionCount] = {"EDF/player", "invaders", "faction2", "civilians", "faction4"};
  std::string factions;
  for (uint32_t f = 0; f < kFactionCount; ++f) factions += Fmt(" %s=%zu", kFactionNames[f], FactionObjects(m, f).size());
  in.Print("factions (live):" + factions);
  // Objects per class: every faction's live objects, and everything the map grid holds
  // (buildings, props and the moving objects it tracks for collision).
  std::unordered_set<uint32_t> objects;
  for (uint32_t f = 0; f < kFactionCount; ++f)
    for (const uint32_t object : FactionObjects(m, f)) objects.insert(object);
  size_t grid_count = 0;
  // The grid is the mission's; outside one it may be stale, so it is only asked in one.
  if (const auto box = GameInMission(*ctx.frame) ? GridBounds(m) : std::nullopt) {
    GuestCalls g(*ctx.frame->ctx, ctx.frame->base);
    const auto grid = GridQuery(g, box->center, box->half);
    grid_count = grid.size();
    objects.insert(grid.begin(), grid.end());
  }
  std::map<std::string, int> classes;
  for (const uint32_t object : objects) {
    auto cls = m.ClassName(object);
    ++classes[cls.empty() ? "?" : cls];
  }
  std::vector<std::pair<int, std::string>> sorted;
  for (auto& [cls, n] : classes) sorted.push_back({n, cls});
  std::sort(sorted.rbegin(), sorted.rend());
  const size_t shown = in.has(0) && Lower(in.str(0)) == "all" ? sorted.size() : std::min<size_t>(sorted.size(), 16);
  in.Print(Fmt("objects: %zu in %zu classes (faction lists and the %zu in the map grid)%s", objects.size(), sorted.size(),
               grid_count, shown < sorted.size() ? "; 'stats all' lists every class" : ""));
  std::string line;
  for (size_t i = 0; i < shown; ++i) {
    line += Fmt("  %s=%d", sorted[i].second.c_str(), sorted[i].first);
    if ((i + 1) % 4 == 0 || i + 1 == shown) { in.Print(line); line.clear(); }
  }
  in.Print(Fmt("cheats: %s", service.cheats_used() ? "USED" : "none"));
}

void Pos(Invocation& in) {
  auto& ctx = *static_cast<ExecContext*>(in.host());
  const GuestMemory m(ctx.frame->base);
  const uint32_t player = FindPlayer(m);
  const Vec3 p = Position(m, player);
  const float heading = HeadingRadians(m, player) * 180.0f / std::numbers::pi_v<float>;
  in.Print(Fmt("pos: %.2f %.2f %.2f  heading %.1f deg  health %.0f/%.0f  (object 0x%08X)", p.x, p.y, p.z, heading,
               m.F32(player + 592), m.F32(player + 596), player));
}

void Teleport(Invocation& in) {
  auto& ctx = *static_cast<ExecContext*>(in.host());
  const GuestMemory m(ctx.frame->base);
  const uint32_t player = FindPlayer(m);
  const float x = float(in.number(0)), y = float(in.number(1)), z = float(in.number(2));
  // clSoldierObject integrates its accumulated position +1440 and copies it to the
  // public +272 every update (after clamping to the map bounds +1024/+1040).
  for (uint32_t field : {1440u, 272u}) {
    m.WF32(player + field, x);
    m.WF32(player + field + 4, y);
    m.WF32(player + field + 8, z);
  }
  in.Ok(Fmt("teleport: %.2f %.2f %.2f (the next step moves the player from there)", x, y, z));
}

void RefillAmmo(const GuestMemory& m, uint32_t player) {
  const uint32_t weapons = m.U32(player + 1824), count = m.U32(player + 1832);
  if (count > 16 || !GuestMemory::Heap(weapons, count * 1408)) return;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t weapon = weapons + i * 1408;
    const int32_t capacity = int32_t(m.U32(weapon + 800)), left = int32_t(m.U32(weapon + 804));
    if (capacity > 0 && capacity < 100000 && left < capacity) m.W32(weapon + 804, uint32_t(capacity));
  }
}

void GameTick(EngineFrame& frame) {
  // Spread work (spawns, effects).
  std::vector<Job> pending;
  {
    std::lock_guard lock(jobs_mutex);
    pending.swap(jobs);
  }
  if (!pending.empty()) {
    GuestCalls g(*frame.ctx, frame.base);
    std::vector<Job> keep;
    for (auto& job : pending) {
      bool done = true;
      try { done = job.step(g, frame); }
      catch (const std::exception& error) {
        Service::Get().Print(Severity::kError, job.label + ": " + error.what());
      }
      if (!done) keep.push_back(std::move(job));
    }
    std::lock_guard lock(jobs_mutex);
    jobs.insert(jobs.begin(), std::make_move_iterator(keep.begin()), std::make_move_iterator(keep.end()));
  }
  const bool want_god = god.load(std::memory_order_relaxed), release = god_release.exchange(false);
  const bool want_ammo = infinite_ammo.load(std::memory_order_relaxed);
  if (!want_god && !release && !want_ammo) return;
  const GuestMemory m(frame.base);
  const uint32_t player = FindPlayer(m);
  if (!player) return;
  if (want_god) {
    m.W8(player + 625, 1);
    if (m.F32(player + 592) < m.F32(player + 596)) m.WF32(player + 592, m.F32(player + 596));
  } else if (release) {
    m.W8(player + 625, 0);
  }
  if (want_ammo) RefillAmmo(m, player);
}
}  // namespace

bool MissionWorldActive(EngineFrame& frame) {
  if (!frame.base) return false;
  const GuestMemory m(frame.base);
  return EngineIdle(m) && (FindPlayerByIndex(m, 0) != 0 || FindPlayerByIndex(m, 1) != 0);
}

bool GameInMission(EngineFrame& frame) {
  if (!MissionWorldActive(frame)) return false;
  const GuestMemory m(frame.base);
  for (uint32_t index = 0; index <= 1; ++index) {
    const uint32_t player = FindPlayerByIndex(m, index);
    if (player && !m.U8(player + 624)) return true;
  }
  return false;
}

#if defined(EDF2027_BUILD_MULTIPLAYER)
bool SyncLocalHostGuestSeat(EngineFrame& frame, bool remote_active,
                            uint16_t remote_player_id, uint64_t connection_generation) {
  static uint64_t last_spawn_attempt_tick = 0;
  if (!remote_active && !host_guest_seat.player_object && !host_guest_seat.death_latched &&
      !host_guest_seat.seat_authorized && !host_guest_seat.reassignment_pending) return false;
  if (!remote_active && (host_guest_seat.player_object || host_guest_seat.death_latched ||
                         host_guest_seat.seat_authorized)) {
    // Invalidate the old peer before checking guest-memory idleness. A fast
    // reconnect can otherwise replace it before this thread sees a menu frame.
    host_guest_seat.reassignment_pending = true;
    host_guest_seat.reassignment_had_owned_seat =
        host_guest_seat.player_object || host_guest_seat.seat_authorized;
    // A pointer cannot safely survive an unknown number of non-idle guest
    // frames; cleanup below will locate the unique native index-1 seat instead.
    host_guest_seat.player_object = 0;
    host_guest_seat.seat_authorized = false;
    host_guest_seat.remote_player_id = 0;
    host_guest_seat.connection_generation = 0;
    ClearHostGuestSeatDeath();
  } else if (remote_active) {
    if (host_guest_seat.connection_generation && connection_generation &&
        host_guest_seat.connection_generation != connection_generation) {
      host_guest_seat.reassignment_pending = true;
      host_guest_seat.reassignment_had_owned_seat =
          host_guest_seat.reassignment_had_owned_seat ||
          host_guest_seat.player_object || host_guest_seat.seat_authorized;
      host_guest_seat.player_object = 0;
      host_guest_seat.seat_authorized = false;
      ClearHostGuestSeatDeath();
    }
    host_guest_seat.remote_player_id = remote_player_id;
    host_guest_seat.connection_generation = connection_generation;
  }
  if (!frame.ctx || !frame.base) return false;

  try {
    const GuestMemory memory(frame.base);
    if (!EngineIdle(memory)) return false;
    uint64_t mission_generation = 0;
#if defined(_WIN32)
    mission_generation = edf::multiplayer::CurrentMissionAntTraceGeneration();
#endif
    if (mission_generation && host_guest_seat.mission_generation &&
        mission_generation != host_guest_seat.mission_generation) {
      // A new clMissionSequence::Begin invalidates guest addresses and any
      // death latch from the previous mission. Keep only this bridge's seat
      // authorization so it can bind the new native player-1 object.
      host_guest_seat.player_object = 0;
      ClearHostGuestSeatDeath();
      if (host_guest_seat.reassignment_pending) {
        // The old guest object belonged to the previous scene; a new native
        // seat created in this mission can be assigned to the current peer.
        host_guest_seat.reassignment_had_owned_seat = false;
        host_guest_seat.seat_authorized = true;
      }
      last_spawn_attempt_tick = 0;
      host_guest_seat.mission_generation = mission_generation;
      REXLOG_INFO("F5 remote player seat: reset stale guest binding at mission transition");
    } else if (!host_guest_seat.mission_generation) {
      host_guest_seat.mission_generation = mission_generation;
    }
    const auto faction_players = FactionObjects(memory, 0);
    const auto find_player_index = [&](uint32_t index) {
      return std::find_if(faction_players.begin(), faction_players.end(), [&](uint32_t object) {
        return GuestMemory::Heap(object, 1452) && memory.ClassName(object) == "clPlayerObject" &&
            memory.U32(object + 492) == index;
      });
    };
    const auto owned_player = std::find_if(faction_players.begin(), faction_players.end(), [&](uint32_t object) {
      return host_guest_seat.player_object && object == host_guest_seat.player_object &&
          GuestMemory::Heap(object, 1452) && memory.ClassName(object) == "clPlayerObject";
    });
    bool owned_player_present = false;
    bool owned_player_dead = false;
    if (owned_player != faction_players.end()) {
      const uint32_t native_index = memory.U32(*owned_player + 492);
      const float health = memory.F32(*owned_player + 592);
      const bool native_dead_marker = memory.U8(*owned_player + 624) != 0;
      const bool zero_health = std::isfinite(health) && health <= 0.0f;
      // +624 also means input-disabled in scripted states. It is only a death
      // signal after EDF has deassigned this player from its native seat.
      const bool native_dead = native_index == std::numeric_limits<uint32_t>::max() &&
          (native_dead_marker || zero_health);
      const bool is_native_remote_seat = native_index == 1 || native_dead;
      if (is_native_remote_seat) {
        owned_player_present = true;
        owned_player_dead = native_dead;
        if (owned_player_dead && !host_guest_seat.reassignment_pending) {
          LatchHostGuestSeatDeath(memory, *owned_player);
        } else if (!owned_player_dead && !host_guest_seat.death_latched) {
          ClearHostGuestSeatDeath();
        }
      } else {
        // A guest allocator can recycle an address for a different player.
        // Never remove or sample it as the bridge-owned seat.
        host_guest_seat.player_object = 0;
      }
    } else if (host_guest_seat.player_object) {
      // Keep only scalar dead vitals after removal. Never carry a dereferenceable
      // guest address across faction-list removal, where the allocator can reuse it.
      host_guest_seat.player_object = 0;
    }
    const auto remote_player = find_player_index(1);
    if (host_guest_seat.death_latched && remote_player == faction_players.end())
      host_guest_seat.replacement_eligible = true;
    const bool in_mission = MissionWorldActive(frame) || owned_player_dead;
    const auto death_time_elapsed = [&] {
      return host_guest_seat.death_latched &&
          host_guest_seat.death_started != std::chrono::steady_clock::time_point{} &&
          std::chrono::steady_clock::now() - host_guest_seat.death_started >=
              std::chrono::seconds(5);
    };

    if (!remote_active) {
      if (owned_player_present) {
        GuestCalls guest(*frame.ctx, frame.base);
        guest.Call(sub_821C0ED8, GuestCalls::R({*owned_player}));
      } else if (host_guest_seat.reassignment_had_owned_seat &&
                 remote_player != faction_players.end()) {
        GuestCalls guest(*frame.ctx, frame.base);
        guest.Call(sub_821C0ED8, GuestCalls::R({*remote_player}));
      }
      host_guest_seat = {};
      last_spawn_attempt_tick = 0;
      return false;
    }
    if (host_guest_seat.reassignment_pending) {
      const bool keep_mission_rebind_authorization =
          host_guest_seat.seat_authorized && !host_guest_seat.reassignment_had_owned_seat;
      if (host_guest_seat.reassignment_had_owned_seat && owned_player_present) {
        GuestCalls guest(*frame.ctx, frame.base);
        guest.Call(sub_821C0ED8, GuestCalls::R({*owned_player}));
        return false;
      }
      if (host_guest_seat.reassignment_had_owned_seat &&
          remote_player != faction_players.end()) {
        GuestCalls guest(*frame.ctx, frame.base);
        guest.Call(sub_821C0ED8, GuestCalls::R({*remote_player}));
        return false;
      }
      host_guest_seat.player_object = 0;
      ClearHostGuestSeatDeath();
      host_guest_seat.reassignment_pending = false;
      host_guest_seat.reassignment_had_owned_seat = false;
      host_guest_seat.seat_authorized = keep_mission_rebind_authorization;
      last_spawn_attempt_tick = 0;
    }
    // If the death transition removed both indexed players, retain its scalar
    // vitals until EDF presents a replacement or the owner disconnects.
    if (!in_mission && !host_guest_seat.death_latched) {
      if (owned_player_present) {
        GuestCalls guest(*frame.ctx, frame.base);
        guest.Call(sub_821C0ED8, GuestCalls::R({*owned_player}));
      }
      host_guest_seat.player_object = 0;
      ClearHostGuestSeatDeath();
      return false;
    }

    // Preserve an existing couch-coop seat. F5 can only claim the native seat
    // if this bridge created it itself and can therefore validate ownership.
    if (remote_player != faction_players.end()) {
      if (host_guest_seat.player_object == *remote_player) {
        if (death_time_elapsed()) ClearHostGuestSeatDeath();
        return true;
      }
      if (host_guest_seat.death_latched &&
          (host_guest_seat.replacement_eligible || death_time_elapsed())) {
        // EDF may replace the dead soldier during its own recovery flow. Adopt
        // a new index-1 object after an observed empty-seat gap. If EDF creates
        // it in the same frame as deassignment, wait out the full death window
        // first. The native slot is unique; do not claim a pre-existing seat
        // before this bridge has observed its own bound seat die.
        host_guest_seat.player_object = *remote_player;
        if (death_time_elapsed()) ClearHostGuestSeatDeath();
        REXLOG_INFO("F5 remote player seat: adopted EDF's replacement player 1 after the prior seat died");
        return true;
      }
      if (host_guest_seat.seat_authorized && !host_guest_seat.death_latched) {
        // The bridge owned player 1 before a mission load; the new scene may
        // have created its native player-1 object before this tick.
        host_guest_seat.player_object = *remote_player;
        host_guest_seat.mission_generation = mission_generation;
        REXLOG_INFO("F5 remote player seat: rebound the authorized native seat in the new mission");
        return true;
      }
      return false;
    }

    if (host_guest_seat.death_latched) {
      // Keep the native dead state visible through many 20 Hz snapshots before
      // constructing a replacement. Do not discard the latch while EDF has no
      // live primary seat; that may be a simultaneous death/respawn gap.
      if (!death_time_elapsed()) return true;
      const auto primary = find_player_index(0);
      if (primary == faction_players.end() || memory.U8(*primary + 624)) return true;
      if (owned_player_present && owned_player_dead) {
        GuestCalls guest(*frame.ctx, frame.base);
        guest.Call(sub_821C0ED8, GuestCalls::R({*owned_player}));
      }
      host_guest_seat.player_object = 0;
    }

    const auto primary = find_player_index(0);
    if (primary == faction_players.end() || memory.U8(*primary + 624)) return false;

    constexpr uint64_t kSpawnRetryTicks = 120;
    if (last_spawn_attempt_tick && frame.tick >= last_spawn_attempt_tick &&
        frame.tick - last_spawn_attempt_tick < kSpawnRetryTicks) return false;
    last_spawn_attempt_tick = frame.tick;

    const float x = memory.F32(*primary + 272) + 2.0f;
    const float y = memory.F32(*primary + 276);
    const float z = memory.F32(*primary + 280);
    const float yaw = HeadingRadians(memory, *primary);
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(yaw))
      return false;

    GuestCalls guest(*frame.ctx, frame.base);
    const uint32_t mark = guest.Mark();
    uint32_t object = 0;
    uint32_t path = 0;
    bool path_initialized = false;
    bool path_destroyed = false;
    bool accepted = false;
    try {
      const std::string path_text = "game:\\Object\\Player01.Sgo";
      const uint32_t literal = guest.WideString(path_text);
      path = guest.Alloc(32);
      memory.W32(path + 24, 7);
      guest.Call(sub_820A0D10, GuestCalls::R(
          {path, literal, static_cast<uint32_t>(path_text.size())}));
      path_initialized = true;
      guest.Call(sub_820AFAC0, GuestCalls::R({memory.U32(kResourceManagerPointer), path, 1}));

      const uint32_t matrix = guest.Alloc(64);
      const uint32_t params = guest.Alloc(80);
      const float c = std::cos(yaw), s = std::sin(yaw);
      const float rows[16] = {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, x, y, z, 1};
      for (uint32_t i = 0; i < 16; ++i) memory.WF32(matrix + i * 4, rows[i]);
      for (uint32_t i = 0; i < 4; ++i) memory.WF32(params + i * 20, 1.0f);
      object = guest.Call(sub_821A6E08, GuestCalls::R(
          {memory.U32(kCorePointer), matrix, path, params}));
      const uint32_t game_object = object ? guest.Call(sub_821E9348, GuestCalls::R(
          {object, 0, kTypeSgsObject, kTypeGameObject, 0})) : 0;
      if (GuestMemory::Heap(game_object, 1452) && memory.ClassName(game_object) == "clPlayerObject") {
        guest.Call(sub_820D5430, GuestCalls::R({game_object}, {1.0}));
        guest.Call(sub_820D5ED0, GuestCalls::R({game_object, 0, 1}));
        memory.W32(game_object + 492, 1);
        guest.Call(sub_820D4A00, GuestCalls::R({game_object}));
        accepted = memory.ClassName(game_object) == "clPlayerObject" &&
            memory.U32(game_object + 492) == 1;
        if (accepted) object = game_object;
      }
      guest.Call(sub_820A0858, GuestCalls::R({path}));
      path_destroyed = true;
    } catch (...) {
      if (path_initialized && !path_destroyed) {
        try { guest.Call(sub_820A0858, GuestCalls::R({path})); } catch (...) {}
      }
      if (object && GuestMemory::Heap(object, 4)) {
        try { guest.Call(sub_821C0ED8, GuestCalls::R({object})); } catch (...) {}
      }
      guest.Release(mark);
      REXLOG_WARN("F5 remote player seat: EDF rejected the native second-player spawn");
      return false;
    }
    if (!accepted && object && GuestMemory::Heap(object, 4)) {
      try { guest.Call(sub_821C0ED8, GuestCalls::R({object})); } catch (...) {}
    }
    guest.Release(mark);
    if (!accepted) {
      REXLOG_WARN("F5 remote player seat: Player01.Sgo did not become a validated player 1 object");
      return false;
    }
    host_guest_seat.player_object = object;
    host_guest_seat.mission_generation = mission_generation;
    host_guest_seat.seat_authorized = true;
    ClearHostGuestSeatDeath();
    REXLOG_INFO("F5 remote player seat: created EDF player 1 for the first connected remote pilot");
    return true;
  } catch (const std::exception& error) {
    REXLOG_WARN("F5 remote player seat: {}", error.what());
    return false;
  } catch (...) {
    REXLOG_WARN("F5 remote player seat: guest validation failed");
    return false;
  }
}

TrustedGuestPose SampleGuestPlayerPose(EngineFrame& frame, uint32_t player_index) {
  TrustedGuestPose pose;
  if (!frame.base || player_index > 3) return pose;
  try {
    const GuestMemory memory(frame.base);
    // SyncLocalHostGuestSeat runs before sampling on the host game thread. The
    // scalar latch remains authoritative while EDF swaps or removes the object.
    if (player_index == 1 && host_guest_seat.death_latched)
      return LatchedHostGuestSeatDeathPose();
    // Seat 1 can remain active while seat 0 is dead; gate on the loaded world,
    // then validate the requested seat independently below.
    if (!MissionWorldActive(frame)) {
      return pose;
    }
    const auto players = FactionObjects(memory, 0);
    auto player = std::find_if(players.begin(), players.end(), [&](uint32_t object) {
      return GuestMemory::Heap(object, 1452) && memory.ClassName(object) == "clPlayerObject" &&
          memory.U32(object + 492) == player_index;
    });
    if (player_index == 1 && host_guest_seat.player_object) {
      const auto owned_player = std::find(players.begin(), players.end(), host_guest_seat.player_object);
      if (owned_player != players.end() && GuestMemory::Heap(*owned_player, 1452) &&
          memory.ClassName(*owned_player) == "clPlayerObject") {
        const uint32_t native_index = memory.U32(*owned_player + 492);
        const float owned_health = memory.F32(*owned_player + 592);
        const bool owned_zero_health = std::isfinite(owned_health) && owned_health <= 0.0f;
        const bool owned_native_dead =
            native_index == std::numeric_limits<uint32_t>::max() &&
            (memory.U8(*owned_player + 624) != 0 || owned_zero_health);
        if (native_index == 1 || owned_native_dead) player = owned_player;
        if (owned_native_dead) {
          LatchHostGuestSeatDeath(memory, *owned_player);
          return LatchedHostGuestSeatDeathPose();
        }
      } else {
        // Do not retain a dead object's guest address after faction removal.
        host_guest_seat.player_object = 0;
      }
    }
    if (player == players.end()) {
      return pose;
    }

    const float health = memory.F32(*player + 592);
    const float maximum_health = memory.F32(*player + 596);
    const uint32_t native_index = memory.U32(*player + 492);
    const bool zero_health = std::isfinite(health) && health <= 0.0f;
    const bool native_dead = native_index == std::numeric_limits<uint32_t>::max() &&
        (memory.U8(*player + 624) != 0 || zero_health);
    const bool dead = zero_health || native_dead;
    if (player_index == 1 && host_guest_seat.player_object == *player && native_dead) {
      LatchHostGuestSeatDeath(memory, *player);
      return LatchedHostGuestSeatDeathPose();
    }
    if (std::isfinite(health) && std::isfinite(maximum_health) && maximum_health > 0.0f) {
      const long rounded_max = std::clamp<long>(std::lround(std::min(maximum_health, 65535.0f)), 1, 65535);
      const long rounded_health = dead ? 0 : std::clamp<long>(
          std::lround(std::clamp(health, 0.0f, float(rounded_max))), 0, rounded_max);
      pose.health_valid = true;
      pose.alive = rounded_health > 0;
      pose.health = static_cast<uint16_t>(rounded_health);
      pose.max_health = static_cast<uint16_t>(rounded_max);
    }
    if (dead || !pose.alive) return pose;

    const float x = memory.F32(*player + 272);
    const float y = memory.F32(*player + 276);
    const float z = memory.F32(*player + 280);
    float yaw = std::atan2(memory.F32(*player + 224 + 32), memory.F32(*player + 224 + 40));
    if (!std::isfinite(yaw)) yaw = HeadingRadians(memory, *player);
    int32_t x_cm = 0, y_cm = 0, z_cm = 0;
    if (!std::isfinite(yaw) || !ToNetworkCentimeters(x, x_cm) ||
        !ToNetworkCentimeters(y, y_cm) || !ToNetworkCentimeters(z, z_cm)) return pose;
    pose.available = true;
    pose.x_cm = x_cm;
    pose.y_cm = y_cm;
    pose.z_cm = z_cm;
    pose.yaw_mrad = static_cast<int16_t>(std::clamp<long>(std::lround(double(yaw) * 1000.0), -3142, 3142));
  } catch (...) {
    return {};
  }
  return pose;
}

void MultiplayerEngineTick(EngineFrame& frame, bool connected, uint16_t local_player_id,
                           bool server_authoritative_movement,
                           edf::net::InputCommand& input, const edf::net::WorldSnapshot* snapshot,
                           const edf::net::ActorSnapshot* actor_snapshot,
                           TrustedGuestPose* trusted_guest_pose,
                           uint16_t host_guest_player_id,
                           const TrustedGuestPositionReset* host_guest_reset,
                           TrustedGuestPositionResetAck* host_guest_reset_ack) {
  static uint32_t last_applied_reset_id = 0;
  static uint32_t reset_player_object = 0;
  static uint32_t pending_reset_ack_id = 0;
  static uint32_t pending_reset_ack_object = 0;
  static uint64_t reset_applied_engine_tick = 0;
  static uint8_t reset_reapply_count = 0;
  static uint16_t snapshot_player_id = 0;
  static uint16_t host_guest_reset_player_id = 0;
  static uint32_t host_guest_reset_id = 0;
  static uint32_t host_guest_reset_object = 0;
  static uint64_t host_guest_reset_applied_tick = 0;
  static uint8_t host_guest_reset_reapply_count = 0;
  static uint32_t last_server_movement_snapshot_tick = 0;
  static bool needs_server_spawn = false;
#if defined(EDF2027_BUILD_MULTIPLAYER)
  static OwnerCorrectionStats owner_correction_stats;
#endif
#if defined(EDF2027_BUILD_MULTIPLAYER)
  static uint32_t server_vitals_protected_player = 0;
  static uint8_t previous_server_vitals_invulnerability = 0;
#endif
  if (trusted_guest_pose) *trusted_guest_pose = {};
  if (host_guest_reset_ack) *host_guest_reset_ack = {};
#if defined(EDF2027_BUILD_MULTIPLAYER)
  const bool owner_correction_trace = connected && REXCVAR_GET(edf_mp_owner_correction_trace);
  if (!owner_correction_trace) owner_correction_stats.Reset();
#endif
  const GuestMemory m(frame.base);
#if defined(EDF2027_BUILD_MULTIPLAYER)
  const auto restore_server_vitals_protection = [&] {
    if (server_vitals_protected_player &&
        GuestMemory::Heap(server_vitals_protected_player, 628) &&
        m.ClassName(server_vitals_protected_player) == "clPlayerObject")
      m.W8(server_vitals_protected_player + 625, previous_server_vitals_invulnerability);
    server_vitals_protected_player = 0;
    previous_server_vitals_invulnerability = 0;
  };
#endif
  if (!connected) {
#if defined(EDF2027_BUILD_MULTIPLAYER)
    restore_server_vitals_protection();
#endif
    ClearRemotePlayers(frame, m, EngineIdle(m));
    ClearRemoteActors(frame, m, EngineIdle(m));
    last_applied_reset_id = 0;
    reset_player_object = 0;
    pending_reset_ack_id = 0;
    pending_reset_ack_object = 0;
    reset_applied_engine_tick = 0;
    reset_reapply_count = 0;
    snapshot_player_id = 0;
    host_guest_reset_player_id = 0;
    host_guest_reset_id = 0;
    host_guest_reset_object = 0;
    host_guest_reset_applied_tick = 0;
    host_guest_reset_reapply_count = 0;
    last_server_movement_snapshot_tick = 0;
    needs_server_spawn = false;
    input.gameplay_active = false;
    input.request_spawn = false;
    input.position_reset_ack = 0;
    input.transform_valid = false;
    input.x_cm = input.y_cm = input.z_cm = 0;
    return;
  }

  if (!EngineIdle(m)) {
#if defined(EDF2027_BUILD_MULTIPLAYER)
    restore_server_vitals_protection();
#endif
    // Keep an authenticated player stationary while a mission loads or ends. On
    // return to live play, request the host-configured spawn for this player.
    input.buttons = 0;
    input.left_trigger = input.right_trigger = 0;
    input.move_x = input.move_y = input.aim_x = input.aim_y = 0;
    input.gameplay_active = false;
    input.request_spawn = false;
    input.position_reset_ack = 0;
    input.transform_valid = false;
    input.x_cm = input.y_cm = input.z_cm = 0;
    needs_server_spawn = true;
    reset_player_object = 0;
    pending_reset_ack_id = 0;
    pending_reset_ack_object = 0;
    reset_reapply_count = 0;
    last_server_movement_snapshot_tick = 0;
    ClearRemotePlayers(frame, m, false);
    ClearRemoteActors(frame, m, false);
    return;
  }

  // A host guest reset belongs to native seat 1 and must proceed even while
  // seat 0 is dead or waiting for its own respawn.
  if (host_guest_reset && host_guest_reset->player_id == host_guest_player_id &&
      host_guest_reset->player_id && host_guest_reset->reset_id && FindPlayer(m)) {
    const auto players = FactionObjects(m, 0);
    const auto seat = std::find_if(players.begin(), players.end(), [&](uint32_t object) {
      return GuestMemory::Heap(object, 1452) && m.ClassName(object) == "clPlayerObject" &&
          m.U32(object + 492) == 1;
    });
    const bool new_reset = host_guest_reset_player_id != host_guest_reset->player_id ||
        host_guest_reset_id != host_guest_reset->reset_id ||
        (seat != players.end() && host_guest_reset_object != *seat);
    // Do not acknowledge a reset against a dead native object. Its pose is not
    // published as authoritative, and the seat lifecycle will clear/recreate
    // the object as soon as the remote pilot becomes inactive/active again.
    if (seat != players.end() && !m.U8(*seat + 624)) {
      const float x = float(host_guest_reset->x_cm) * 0.01f;
      const float y = float(host_guest_reset->y_cm) * 0.01f;
      const float z = float(host_guest_reset->z_cm) * 0.01f;
      if (new_reset) {
        if (ApplyServerTransform(m, *seat, x, y, z,
                float(host_guest_reset->yaw_mrad) * 0.001f)) {
          host_guest_reset_player_id = host_guest_reset->player_id;
          host_guest_reset_id = host_guest_reset->reset_id;
          host_guest_reset_object = *seat;
          host_guest_reset_applied_tick = frame.tick;
          host_guest_reset_reapply_count = 0;
        }
      } else {
        const bool pose_matches = ServerResetTransformMatches(m, *seat, x, y, z);
        if (!pose_matches && host_guest_reset_reapply_count < 3 &&
            ApplyServerTransform(m, *seat, x, y, z,
                float(host_guest_reset->yaw_mrad) * 0.001f)) {
          host_guest_reset_applied_tick = frame.tick;
          ++host_guest_reset_reapply_count;
        } else if ((pose_matches || host_guest_reset_reapply_count >= 3) &&
                   frame.tick > host_guest_reset_applied_tick && host_guest_reset_ack) {
          // Match the local owner handshake: after bounded reapplications, the
          // reset was written to the same validated object over several ticks.
          // ACK it so physics drift cannot leave the server seat inactive forever.
          host_guest_reset_ack->player_id = host_guest_reset->player_id;
          host_guest_reset_ack->reset_id = host_guest_reset->reset_id;
        }
      }
    }
  } else if (!host_guest_reset || host_guest_reset->player_id != host_guest_player_id ||
             host_guest_reset->player_id != host_guest_reset_player_id ||
             host_guest_reset->reset_id != host_guest_reset_id) {
    host_guest_reset_player_id = 0;
    host_guest_reset_id = 0;
    host_guest_reset_object = 0;
    host_guest_reset_applied_tick = 0;
    host_guest_reset_reapply_count = 0;
  }

  const uint32_t local_player = FindPlayer(m);
#if defined(EDF2027_BUILD_MULTIPLAYER)
  const edf::net::PlayerState* local_state = nullptr;
  if (snapshot && snapshot->tick) {
    const auto found = std::find_if(snapshot->players.begin(), snapshot->players.end(),
        [local_player_id](const edf::net::PlayerState& state) { return state.id == local_player_id; });
    if (found != snapshot->players.end()) local_state = &*found;
  }
  if (!local_state || !local_state->health_valid || !local_player ||
      !snapshot || !snapshot->owner_pose_authoritative || trusted_guest_pose) {
    restore_server_vitals_protection();
  } else if (server_vitals_protected_player != local_player) {
    restore_server_vitals_protection();
    server_vitals_protected_player = local_player;
    previous_server_vitals_invulnerability = m.U8(local_player + 625);
  }
  if (local_player && local_state && local_state->health_valid) {
    if (snapshot->owner_pose_authoritative && !trusted_guest_pose)
      m.W8(local_player + 625, 1);
    (void)ApplyServerPlayerVitals(m, local_player, *local_state);
  }
#endif
  if (!local_player || m.U8(local_player + 624)) {
    // Keep current buttons/axes for EDF's native revive and respawn flow. The
    // server still sees an inactive player and no authoritative transform.
    input.gameplay_active = false;
    input.request_spawn = false;
    input.position_reset_ack = 0;
    needs_server_spawn = true;
    input.transform_valid = false;
    input.x_cm = input.y_cm = input.z_cm = 0;
    reset_player_object = 0;
    pending_reset_ack_id = 0;
    pending_reset_ack_object = 0;
    reset_reapply_count = 0;
    ClearRemotePlayers(frame, m, true);
    ClearRemoteActors(frame, m, true);
    return;
  }

  input.gameplay_active = true;
  input.position_reset_ack = 0;
  // EDF's own mission flow owns respawns whenever the server accepts native
  // client transforms. Server-simulated fallback clients still request the
  // host-configured spawn after their local soldier returns.
  input.request_spawn = server_authoritative_movement && needs_server_spawn;
  input.aim_yaw_mrad = static_cast<int16_t>(std::clamp<long>(
      std::lround(double(HeadingRadians(m, local_player)) * 1000.0), -3142, 3142));
  const Vec3 local_position = Position(m, local_player);
  input.transform_valid = ToNetworkCentimeters(local_position.x, input.x_cm) &&
      ToNetworkCentimeters(local_position.y, input.y_cm) &&
      ToNetworkCentimeters(local_position.z, input.z_cm);
  if (!input.transform_valid) input.x_cm = input.y_cm = input.z_cm = 0;
  if (trusted_guest_pose && input.transform_valid) {
    trusted_guest_pose->available = true;
    trusted_guest_pose->x_cm = input.x_cm;
    trusted_guest_pose->y_cm = input.y_cm;
    trusted_guest_pose->z_cm = input.z_cm;
    trusted_guest_pose->yaw_mrad = input.aim_yaw_mrad;
    const float health = m.F32(local_player + 592);
    const float maximum_health = m.F32(local_player + 596);
    if (std::isfinite(health) && std::isfinite(maximum_health) && maximum_health > 0.0f) {
      const long rounded_max = std::clamp<long>(std::lround(std::min(maximum_health, 65535.0f)), 1, 65535);
      const long rounded_health = std::clamp<long>(
          std::lround(std::clamp(health, 0.0f, float(rounded_max))), 0, rounded_max);
      trusted_guest_pose->health_valid = true;
      trusted_guest_pose->alive = rounded_health > 0;
      trusted_guest_pose->health = static_cast<uint16_t>(rounded_health);
      trusted_guest_pose->max_health = static_cast<uint16_t>(rounded_max);
    }
  }
  // Copy the host's trusted guest pose before sanitizing the network command.
  // Remote clients must send input only when the server owns movement.
  if (server_authoritative_movement) {
    input.transform_valid = false;
    input.x_cm = input.y_cm = input.z_cm = 0;
  }

  if (!snapshot || snapshot->tick == 0) return;
  if (snapshot_player_id != local_player_id) {
#if defined(EDF2027_BUILD_MULTIPLAYER)
    owner_correction_stats.Reset();
#endif
    snapshot_player_id = local_player_id;
    last_server_movement_snapshot_tick = 0;
    last_applied_reset_id = 0;
    reset_player_object = 0;
    pending_reset_ack_id = 0;
    pending_reset_ack_object = 0;
    reset_applied_engine_tick = 0;
    reset_reapply_count = 0;
  }
  if (local_state && local_state->id == local_player_id) {
    const bool needs_reset_apply = local_state->position_reset &&
        (local_state->position_reset_id != last_applied_reset_id || reset_player_object != local_player);
    // The live guest owns ordinary movement and collision. Only explicit
    // server-approved spawn/position-reset records may move the local soldier.
    const bool apply_transform = needs_reset_apply;
    bool applied_reset_this_sample = false;
    if (apply_transform) {
      const float x = float(local_state->x_cm) * 0.01f;
      const float y = float(local_state->y_cm) * 0.01f;
      const float z = float(local_state->z_cm) * 0.01f;
      if (ApplyServerTransform(m, local_player, x, y, z,
                               float(local_state->yaw_mrad) * 0.001f)) {
        if (local_state->position_reset) {
          last_applied_reset_id = local_state->position_reset_id;
          reset_player_object = local_player;
          pending_reset_ack_id = local_state->position_reset_id;
          pending_reset_ack_object = local_player;
          reset_applied_engine_tick = frame.tick;
          reset_reapply_count = 0;
          applied_reset_this_sample = true;
        }
      }
    }
    const bool reset_still_applied_to_player = local_state->position_reset &&
        pending_reset_ack_id == local_state->position_reset_id &&
        pending_reset_ack_object == local_player && reset_player_object == local_player &&
        last_applied_reset_id == local_state->position_reset_id &&
        GuestMemory::Heap(local_player, 1452);
    const float reset_x = float(local_state->x_cm) * 0.01f;
    const float reset_y = float(local_state->y_cm) * 0.01f;
    const float reset_z = float(local_state->z_cm) * 0.01f;
    const bool reset_transform_matches = reset_still_applied_to_player &&
        ServerResetTransformMatches(m, local_player, reset_x, reset_y, reset_z);
    constexpr uint8_t kMaxResetReapplications = 3;
    if (reset_still_applied_to_player && !reset_transform_matches &&
        reset_reapply_count < kMaxResetReapplications &&
        ApplyServerTransform(m, local_player, reset_x, reset_y, reset_z,
                             float(local_state->yaw_mrad) * 0.001f)) {
      reset_applied_engine_tick = frame.tick;
      ++reset_reapply_count;
      applied_reset_this_sample = true;
    }
    // Physics may keep moving a valid midair target. Retry the authoritative
    // transform a few times, then finish the handshake so server snapshots can
    // continue correcting it instead of leaving the player stuck inactive.
    const bool position_confirmed = reset_transform_matches ||
        reset_reapply_count >= kMaxResetReapplications;
    input.position_reset_ack = position_confirmed && !applied_reset_this_sample &&
        frame.tick > reset_applied_engine_tick ? pending_reset_ack_id : 0;
    if (!local_state->position_reset) {
      pending_reset_ack_id = 0;
      pending_reset_ack_object = 0;
      reset_reapply_count = 0;
    } else if (input.position_reset_ack) {
      needs_server_spawn = false;
    }

    // In server-authoritative movement mode, the server integrates accepted
    // input at 60 Hz and sends its owner pose in each 20 Hz snapshot. Reconcile
    // the local soldier once per new snapshot so its rendered/gameplay pose is
    // driven by the same position that remote peers observe. Client-authority
    // mode continues to leave ordinary movement and collision entirely to EDF.
    if ((server_authoritative_movement || snapshot->owner_pose_authoritative) &&
        !trusted_guest_pose && local_state->active &&
        !local_state->position_reset && snapshot->tick != last_server_movement_snapshot_tick) {
      const float target_x = float(local_state->x_cm) * 0.01f;
      const float target_y = float(local_state->y_cm) * 0.01f;
      const float target_z = float(local_state->z_cm) * 0.01f;
      const float target_yaw = float(local_state->yaw_mrad) * 0.001f;
#if defined(EDF2027_BUILD_MULTIPLAYER)
      Vec3 before{};
      double distance_error_cm = 0.0;
      double yaw_error_mrad = 0.0;
      if (owner_correction_trace) {
        before = Position(m, local_player);
        distance_error_cm = std::hypot(
            std::hypot(double(before.x - target_x), double(before.y - target_y)),
            double(before.z - target_z)) * 100.0;
        yaw_error_mrad = std::abs(std::remainder(
            double(target_yaw - HeadingRadians(m, local_player)), 6.2831853071795864769)) * 1000.0;
      }
      if (ApplyServerTransform(m, local_player, target_x, target_y, target_z, target_yaw)) {
        last_server_movement_snapshot_tick = snapshot->tick;
        if (owner_correction_trace && std::isfinite(distance_error_cm) && std::isfinite(yaw_error_mrad)) {
          if (owner_correction_stats.player_id != local_player_id ||
              owner_correction_stats.player_object != local_player) {
            owner_correction_stats.Reset();
            owner_correction_stats.player_id = local_player_id;
            owner_correction_stats.player_object = local_player;
            owner_correction_stats.next_log_tick = frame.tick + 600;
          }
          ++owner_correction_stats.samples;
          owner_correction_stats.total_distance_cm += distance_error_cm;
          owner_correction_stats.max_distance_cm = std::max(owner_correction_stats.max_distance_cm, distance_error_cm);
          owner_correction_stats.max_yaw_mrad = std::max(owner_correction_stats.max_yaw_mrad, yaw_error_mrad);
          if (distance_error_cm >= 100.0 || yaw_error_mrad >= 250.0)
            ++owner_correction_stats.large_corrections;
          if (frame.tick >= owner_correction_stats.next_log_tick) {
            REXLOG_INFO("MP owner correction: player={} samples={} mean_distance_cm={:.1f} max_distance_cm={:.1f} large={} max_yaw_mrad={:.1f}",
                owner_correction_stats.player_id, owner_correction_stats.samples,
                owner_correction_stats.total_distance_cm / double(owner_correction_stats.samples),
                owner_correction_stats.max_distance_cm, owner_correction_stats.large_corrections,
                owner_correction_stats.max_yaw_mrad);
            owner_correction_stats.samples = 0;
            owner_correction_stats.large_corrections = 0;
            owner_correction_stats.total_distance_cm = 0.0;
            owner_correction_stats.max_distance_cm = 0.0;
            owner_correction_stats.max_yaw_mrad = 0.0;
            owner_correction_stats.next_log_tick = frame.tick + 600;
          }
        }
      }
#else
      if (ApplyServerTransform(m, local_player, target_x, target_y, target_z, target_yaw))
        last_server_movement_snapshot_tick = snapshot->tick;
#endif
    }
  } else {
    input.position_reset_ack = 0;
  }
  std::unordered_set<uint16_t> visible_players;
  visible_players.reserve(snapshot->players.size());
  for (const auto& state : snapshot->players) {
    if (!state.id || state.id == local_player_id) continue;
    if (state.id == host_guest_player_id) {
      const auto existing = remote_player_objects.find(state.id);
      if (existing != remote_player_objects.end()) {
        const uint32_t object = existing->second.object;
        remote_player_objects.erase(existing);
        if (GuestMemory::Heap(object, 4) && m.ClassName(object) == "clFriendSoldier") {
          GuestCalls guest(*frame.ctx, frame.base);
          guest.Call(sub_821C0ED8, GuestCalls::R({object}));
        }
      }
      continue;
    }
    if (!state.active) continue;
    visible_players.insert(state.id);
    auto& replica = remote_player_objects[state.id];
      if (replica.object && m.ClassName(replica.object) != "clFriendSoldier") replica = {};
    const float x = float(state.x_cm) * 0.01f;
    const float y = float(state.y_cm) * 0.01f;
    const float z = float(state.z_cm) * 0.01f;
    const float yaw = float(state.yaw_mrad) * 0.001f;
    if (!replica.object) {
      GuestCalls guest(*frame.ctx, frame.base);
      replica.object = CreateObject(guest, kRemotePlayerType, x, y, z, yaw, false);
      if (replica.object && m.ClassName(replica.object) != "clFriendSoldier") {
        const uint32_t rejected_object = replica.object;
        replica = {};
        guest.Call(sub_821C0ED8, GuestCalls::R({rejected_object}));
      }
      if (!replica.object) continue;
    }

    // Snapshots arrive at 20 Hz. Interpolate over the engine-tick interval between the
    // previous and newest server samples, so smoothing is stable at different frame rates.
    if (!replica.has_pose || snapshot->tick != replica.snapshot_tick) {
      uint64_t blend_ticks = 3;  // 20 Hz server snapshots on the game's 60 Hz simulation clock
      if (replica.has_pose && frame.tick > replica.last_sample_tick)
        blend_ticks = std::clamp<uint64_t>(frame.tick - replica.last_sample_tick, uint64_t{1}, uint64_t{12});
      replica.from = replica.has_pose ? replica.to : RemotePlayerPose{x, y, z, yaw};
      replica.to = {x, y, z, yaw};
      replica.snapshot_tick = snapshot->tick;
      replica.last_sample_tick = frame.tick;
      replica.blend_start_tick = frame.tick;
      replica.blend_end_tick = frame.tick + blend_ticks;
      replica.has_pose = true;
    }
    float alpha = 1.0f;
    if (replica.blend_end_tick > replica.blend_start_tick) {
      const uint64_t elapsed = frame.tick > replica.blend_start_tick ? frame.tick - replica.blend_start_tick : 0;
      const uint64_t duration = replica.blend_end_tick - replica.blend_start_tick;
      alpha = std::clamp(float(elapsed) / float(duration), 0.0f, 1.0f);
    }
    const float next_x = replica.from.x + (replica.to.x - replica.from.x) * alpha;
    const float next_y = replica.from.y + (replica.to.y - replica.from.y) * alpha;
    const float next_z = replica.from.z + (replica.to.z - replica.from.z) * alpha;
    const float yaw_delta = std::remainder(replica.to.yaw - replica.from.yaw, 2.0f * std::numbers::pi_v<float>);
    const float next_yaw = replica.from.yaw + yaw_delta * alpha;
    ApplyServerTransform(m, replica.object, next_x, next_y, next_z, next_yaw);
  }

  std::vector<uint32_t> stale_player_objects;
  std::unordered_set<uint32_t> stale_player_set;
  for (auto it = remote_player_objects.begin(); it != remote_player_objects.end();) {
    if (visible_players.contains(it->first)) { ++it; continue; }
    const uint32_t object = it->second.object;
    if (object && stale_player_set.insert(object).second) stale_player_objects.push_back(object);
    it = remote_player_objects.erase(it);
  }
  for (const uint32_t object : stale_player_objects) {
    if (object && m.ClassName(object) == "clFriendSoldier") {
      GuestCalls guest(*frame.ctx, frame.base);
      guest.Call(sub_821C0ED8, GuestCalls::R({object}));
    }
  }

  if (actor_snapshot && actor_snapshot->tick) {
    std::unordered_set<uint32_t> visible_actors;
    visible_actors.reserve(actor_snapshot->actors.size());
    for (const auto& state : actor_snapshot->actors) {
      if (!state.id || state.archetype != edf::net::ActorArchetype::kAnt) continue;
      visible_actors.insert(state.id);

      // A zero-health actor stays out of this client's scene even if the host
      // retains its corpse in the snapshot briefly after the lethal hit.
      if (!state.health) {
        if (remote_dead_actor_ids.insert(state.id).second) {
          const auto found = remote_actor_objects.find(state.id);
          if (found != remote_actor_objects.end()) {
            const uint32_t object = found->second.object;
            remote_actor_objects.erase(found);
            if (object && m.ClassName(object) == "clGiantAnt") {
              GuestCalls guest(*frame.ctx, frame.base);
              guest.Call(sub_821C0ED8, GuestCalls::R({object}));
            }
          }
        }
        continue;
      }
      remote_dead_actor_ids.erase(state.id);

      auto existing = remote_actor_objects.find(state.id);
      if (existing != remote_actor_objects.end() && existing->second.object &&
          m.ClassName(existing->second.object) == "clGiantAnt" &&
          GuestMemory::Heap(existing->second.object, 628) && m.U8(existing->second.object + 624)) {
        const uint32_t object = existing->second.object;
        remote_actor_objects.erase(existing);
        GuestCalls guest(*frame.ctx, frame.base);
        guest.Call(sub_821C0ED8, GuestCalls::R({object}));
      } else if (existing != remote_actor_objects.end() && existing->second.object &&
                 m.ClassName(existing->second.object) != "clGiantAnt") {
        existing->second = {};
      }

      auto& replica = remote_actor_objects[state.id];
      if (replica.object && m.ClassName(replica.object) != "clGiantAnt") replica = {};
      const float x = float(state.x_cm) * 0.01f;
      const float y = float(state.y_cm) * 0.01f;
      const float z = float(state.z_cm) * 0.01f;
      const float yaw = float(state.yaw_mrad) * 0.001f;
      if (!replica.object) {
        GuestCalls guest(*frame.ctx, frame.base);
        replica.object = CreateObject(guest, kRemoteAntType, x, y, z, yaw, false);
        if (replica.object && m.ClassName(replica.object) != "clGiantAnt") {
          const uint32_t rejected_object = replica.object;
          replica = {};
          guest.Call(sub_821C0ED8, GuestCalls::R({rejected_object}));
        }
        if (!replica.object) continue;
      }
      if (!replica.has_pose || actor_snapshot->tick != replica.snapshot_tick) {
        uint64_t blend_ticks = 3;
        if (replica.has_pose && frame.tick > replica.last_sample_tick)
          blend_ticks = std::clamp<uint64_t>(frame.tick - replica.last_sample_tick, uint64_t{1}, uint64_t{12});
        replica.from = replica.has_pose ? replica.to : RemotePlayerPose{x, y, z, yaw};
        replica.to = {x, y, z, yaw};
        replica.snapshot_tick = actor_snapshot->tick;
        replica.last_sample_tick = frame.tick;
        replica.blend_start_tick = frame.tick;
        replica.blend_end_tick = frame.tick + blend_ticks;
        replica.has_pose = true;

      }
      // Server-owned enemies join the invader faction so local EDF weapons can
      // collide with them, but they never run client AI or own their health.
      // Reassert both fields every guest tick; local ApplyDamage must not be
      // able to kill or permanently desync a server-controlled replica.
      if (GuestMemory::Heap(replica.object, 628)) {
        const float maximum_health = m.F32(replica.object + 596);
        const float authoritative_health = float(state.health);
        if (std::isfinite(maximum_health) && maximum_health > 0.0f)
          m.WF32(replica.object + 592,
                 std::clamp(authoritative_health, 0.0f, maximum_health));
        m.W8(replica.object + 624, 0);
        m.W8(replica.object + 625, 1);
      }
      const uint64_t duration = replica.blend_end_tick > replica.blend_start_tick
          ? replica.blend_end_tick - replica.blend_start_tick : 1;
      const uint64_t elapsed = frame.tick > replica.blend_start_tick
          ? frame.tick - replica.blend_start_tick : 0;
      const float alpha = std::clamp(float(elapsed) / float(duration), 0.0f, 1.0f);
      const float next_x = replica.from.x + (replica.to.x - replica.from.x) * alpha;
      const float next_y = replica.from.y + (replica.to.y - replica.from.y) * alpha;
      const float next_z = replica.from.z + (replica.to.z - replica.from.z) * alpha;
      const float yaw_delta = std::remainder(replica.to.yaw - replica.from.yaw,
                                              2.0f * std::numbers::pi_v<float>);
      const float next_yaw = replica.from.yaw + yaw_delta * alpha;
      ApplyRemoteActorTransform(m, replica.object, next_x, next_y, next_z, next_yaw);
    }
    std::vector<uint32_t> stale_actor_objects;
    std::unordered_set<uint32_t> stale_actor_set;
    for (auto it = remote_actor_objects.begin(); it != remote_actor_objects.end();) {
      if (visible_actors.contains(it->first)) { ++it; continue; }
      const uint32_t object = it->second.object;
      if (object && stale_actor_set.insert(object).second) stale_actor_objects.push_back(object);
      it = remote_actor_objects.erase(it);
    }
    for (const uint32_t object : stale_actor_objects) {
      if (object && m.ClassName(object) == "clGiantAnt") {
        GuestCalls guest(*frame.ctx, frame.base);
        guest.Call(sub_821C0ED8, GuestCalls::R({object}));
      }
    }
    for (auto it = remote_dead_actor_ids.begin(); it != remote_dead_actor_ids.end();) {
      if (visible_actors.contains(*it)) ++it;
      else it = remote_dead_actor_ids.erase(it);
    }
  }
  if (trusted_guest_pose) {
    TrustedGuestPose final_pose;
    if (input.gameplay_active && local_player && GuestMemory::Heap(local_player, 1452)) {
      const Vec3 final_position = Position(m, local_player);
      final_pose.available = ToNetworkCentimeters(final_position.x, final_pose.x_cm) &&
          ToNetworkCentimeters(final_position.y, final_pose.y_cm) &&
          ToNetworkCentimeters(final_position.z, final_pose.z_cm);
      if (final_pose.available) {
        final_pose.yaw_mrad = static_cast<int16_t>(std::clamp<long>(
            std::lround(double(HeadingRadians(m, local_player)) * 1000.0), -3142, 3142));
      }
    }
    *trusted_guest_pose = final_pose;
  }
}

void TraceMissionActorShadowMatches(EngineFrame& frame, const edf::net::ActorSnapshot* snapshot) {
  struct LocalAnt {
    uint32_t object = 0;
    uint32_t vtable = 0;
    float x = 0, y = 0, z = 0, health = 0;
  };
  struct PreviousMatch {
    uint32_t object = 0;
    uint32_t vtable = 0;
    uint32_t stable_samples = 0;
    uint32_t last_tick = 0;
  };
  struct CandidateMatch {
    uint32_t host_id = 0;
    uint32_t object = 0;
    uint32_t vtable = 0;
    uint32_t stable_samples = 0;
    int32_t health_delta = 0;
    int32_t distance_cm = 0;
    int32_t nearest_margin_cm = -1;
    bool ambiguous = false;
    bool association_changed = false;
  };

  static std::unordered_map<uint32_t, PreviousMatch> previous_matches;
  static uint32_t last_processed_tick = 0;
  static uint32_t last_summary_tick = 0;
  static size_t last_matched_count = std::numeric_limits<size_t>::max();
  static size_t last_ambiguous_count = std::numeric_limits<size_t>::max();
  static size_t last_unmatched_count = std::numeric_limits<size_t>::max();
  static size_t last_local_count = std::numeric_limits<size_t>::max();

  const auto clear_state = [&] {
    previous_matches.clear();
    last_processed_tick = 0;
    last_summary_tick = 0;
    last_matched_count = last_ambiguous_count = last_unmatched_count =
        std::numeric_limits<size_t>::max();
    last_local_count = std::numeric_limits<size_t>::max();
  };
  if (!snapshot || !snapshot->tick) {
    clear_state();
    return;
  }
  if (snapshot->tick == last_processed_tick) return;
  const GuestMemory memory(frame.base);
  if (!EngineIdle(memory)) {
    clear_state();
    return;
  }

  std::unordered_set<uint32_t> proxy_objects;
  proxy_objects.reserve(remote_actor_objects.size());
  for (const auto& [id, replica] : remote_actor_objects) {
    (void)id;
    if (replica.object) proxy_objects.insert(replica.object);
  }

  std::vector<LocalAnt> local_ants;
  for (const uint32_t object : FactionObjects(memory, 1)) {
    if (proxy_objects.contains(object) || !GuestMemory::Heap(object, 628) ||
        memory.ClassName(object) != "clGiantAnt" || memory.U32(object + 604) != 1 ||
        memory.U8(object + 624) != 0)
      continue;
    const float health = memory.F32(object + 592);
    const float x = memory.F32(object + 272), y = memory.F32(object + 276), z = memory.F32(object + 280);
    if (!std::isfinite(health) || health <= 0.0f || !std::isfinite(x) ||
        !std::isfinite(y) || !std::isfinite(z))
      continue;
    local_ants.push_back({object, memory.U32(object), x, y, z,
                          std::clamp(health, 0.0f, 65535.0f)});
  }

  constexpr int32_t kMaximumCandidateDistanceCm = 500;
  constexpr int32_t kAmbiguousNearestMarginCm = 50;
  constexpr double kMaximumCandidateDistanceSquaredCm =
      double(kMaximumCandidateDistanceCm) * kMaximumCandidateDistanceCm;
  std::vector<CandidateMatch> matches;
  matches.reserve(snapshot->actors.size());
  for (const auto& host : snapshot->actors) {
    if (!host.id || host.archetype != edf::net::ActorArchetype::kAnt || !host.health) continue;
    const float host_x = float(host.x_cm) * 0.01f;
    const float host_y = float(host.y_cm) * 0.01f;
    const float host_z = float(host.z_cm) * 0.01f;
    double nearest_distance_squared_cm = std::numeric_limits<double>::infinity();
    double second_distance_squared_cm = std::numeric_limits<double>::infinity();
    const LocalAnt* nearest = nullptr;
    for (const LocalAnt& local : local_ants) {
      const double dx_cm = (double(local.x) - host_x) * 100.0;
      const double dy_cm = (double(local.y) - host_y) * 100.0;
      const double dz_cm = (double(local.z) - host_z) * 100.0;
      const double distance_squared_cm = dx_cm * dx_cm + dy_cm * dy_cm + dz_cm * dz_cm;
      if (distance_squared_cm > kMaximumCandidateDistanceSquaredCm) continue;
      if (distance_squared_cm < nearest_distance_squared_cm) {
        second_distance_squared_cm = nearest_distance_squared_cm;
        nearest_distance_squared_cm = distance_squared_cm;
        nearest = &local;
      } else if (distance_squared_cm < second_distance_squared_cm) {
        second_distance_squared_cm = distance_squared_cm;
      }
    }
    if (!nearest) continue;
    const int32_t nearest_cm = static_cast<int32_t>(std::lround(std::sqrt(nearest_distance_squared_cm)));
    const int32_t second_cm = second_distance_squared_cm <= kMaximumCandidateDistanceSquaredCm
        ? static_cast<int32_t>(std::lround(std::sqrt(second_distance_squared_cm)))
        : std::numeric_limits<int32_t>::max();
    const int32_t health_delta = static_cast<int32_t>(std::lround(nearest->health)) -
                                 static_cast<int32_t>(host.health);
    const bool ambiguous = second_cm <= kMaximumCandidateDistanceCm &&
                           second_cm - nearest_cm <= kAmbiguousNearestMarginCm;
    matches.push_back({host.id, nearest->object, nearest->vtable, 0, health_delta,
                       nearest_cm, second_cm == std::numeric_limits<int32_t>::max()
                           ? -1 : std::max(0, second_cm - nearest_cm),
                       ambiguous, false});
  }

  // Never claim a one-to-one shadow mapping if two host samples choose the same
  // local object. Keep both as ambiguous telemetry instead.
  std::unordered_map<uint32_t, size_t> selected_object_uses;
  for (const CandidateMatch& match : matches)
    ++selected_object_uses[match.object];
  for (CandidateMatch& match : matches)
    if (selected_object_uses[match.object] > 1) match.ambiguous = true;

  std::unordered_map<uint32_t, PreviousMatch> next_matches;
  next_matches.reserve(matches.size());
  size_t matched_count = 0, ambiguous_count = 0;
  int64_t total_distance_cm = 0;
  for (CandidateMatch& match : matches) {
    if (match.ambiguous) {
      ++ambiguous_count;
      continue;
    }
    ++matched_count;
    total_distance_cm += match.distance_cm;
    const auto previous = previous_matches.find(match.host_id);
    const int32_t sample_gap = previous == previous_matches.end() ? 0 :
        static_cast<int32_t>(snapshot->tick - previous->second.last_tick);
    if (previous != previous_matches.end() && previous->second.object == match.object &&
        previous->second.vtable == match.vtable &&
        sample_gap > 0 && sample_gap <= 24) {
      match.stable_samples = previous->second.stable_samples + 1;
    } else {
      match.stable_samples = 1;
      match.association_changed = true;
    }
    next_matches.emplace(match.host_id,
                         PreviousMatch{match.object, match.vtable, match.stable_samples,
                                       snapshot->tick});
  }
  const size_t unmatched_count = snapshot->actors.size() >= matched_count + ambiguous_count
      ? snapshot->actors.size() - matched_count - ambiguous_count : 0;
  for (const CandidateMatch& match : matches) {
    if (!match.association_changed) continue;
    REXLOG_INFO("Mission actor shadow candidate: host_id={} local_object={:#x} local_vtable={:#x} distance_cm={} health_delta={} nearest_margin_cm={} stable_samples={} evidence=proximity_candidate",
                match.host_id, match.object, match.vtable, match.distance_cm, match.health_delta,
                match.nearest_margin_cm, match.stable_samples);
  }
  const bool counts_changed = matched_count != last_matched_count ||
      ambiguous_count != last_ambiguous_count || unmatched_count != last_unmatched_count ||
      local_ants.size() != last_local_count;
  if (!last_summary_tick || counts_changed ||
      static_cast<int32_t>(snapshot->tick - last_summary_tick) >= 60) {
    const int32_t mean_distance_cm = matched_count
        ? static_cast<int32_t>(total_distance_cm / static_cast<int64_t>(matched_count)) : 0;
    size_t stable_count = 0;
    for (const auto& [id, previous] : next_matches) {
      (void)id;
      if (previous.stable_samples >= 10) ++stable_count;
    }
    REXLOG_INFO("Mission actor shadow summary: tick={} host_samples={} local_ants={} unique_proximity_candidates={} ambiguous={} unmatched={} stable_10_samples={} mean_distance_cm={} matching_radius_cm={}",
                snapshot->tick, snapshot->actors.size(), local_ants.size(), matched_count,
                ambiguous_count, unmatched_count, stable_count, mean_distance_cm,
                kMaximumCandidateDistanceCm);
    last_summary_tick = snapshot->tick;
    last_matched_count = matched_count;
    last_ambiguous_count = ambiguous_count;
    last_unmatched_count = unmatched_count;
    last_local_count = local_ants.size();
  }
  previous_matches = std::move(next_matches);
  last_processed_tick = snapshot->tick;
}
#endif

#if defined(EDF2027_BUILD_MULTIPLAYER)
void TraceMissionActorIdentityMatches(
    EngineFrame& frame, const edf::net::MissionActorIdentitySnapshot* snapshot) {
#if defined(_WIN32)
  struct PreviousCandidate {
    uint32_t object = 0;
    uint64_t local_generation = 0;
    uint64_t host_generation = 0;
    uint32_t call_ordinal = 0;
    uint32_t ant_ordinal = 0;
  };
  struct Candidate {
    uint32_t host_id = 0;
    uint32_t object = 0;
    uint64_t local_generation = 0;
    uint32_t call_ordinal = 0;
    uint32_t ant_ordinal = 0;
    bool ambiguous = false;
  };
  using SpawnKey = std::pair<uint32_t, uint32_t>;
  using LocalKeySignature = std::tuple<uint64_t, uint32_t, uint32_t, uint32_t>;
  static std::unordered_map<uint32_t, PreviousCandidate> previous_candidates;
  static uint32_t last_processed_tick = 0;
  static uint64_t last_host_generation = 0;
  static std::vector<LocalKeySignature> last_local_signature;

  const auto clear_state = [&] {
    previous_candidates.clear();
    last_processed_tick = 0;
    last_host_generation = 0;
    last_local_signature.clear();
  };
  if (!snapshot || !snapshot->tick || !snapshot->host_trace_generation ||
      !edf::console::MissionWorldActive(frame) || !REXCVAR_GET(edf_mp_actor_trace)) {
    clear_state();
    return;
  }
  const auto local_keys = edf::multiplayer::SnapshotMissionAntTraceKeys(frame.base);
  std::vector<LocalKeySignature> local_signature;
  local_signature.reserve(local_keys.size());
  for (const auto& local : local_keys)
    local_signature.emplace_back(local.local_generation, local.call_ordinal,
                                 local.ant_ordinal, local.object);
  std::sort(local_signature.begin(), local_signature.end());
  if (snapshot->tick == last_processed_tick &&
      snapshot->host_trace_generation == last_host_generation &&
      local_signature == last_local_signature) return;
  if (last_processed_tick && snapshot->tick != last_processed_tick &&
      static_cast<int32_t>(snapshot->tick - last_processed_tick) < 0) return;

  std::map<SpawnKey, std::vector<edf::multiplayer::MissionAntTraceKey>> by_key;
  for (const auto& local : local_keys)
    by_key[{local.call_ordinal, local.ant_ordinal}].push_back(local);

  std::vector<Candidate> candidates;
  candidates.reserve(snapshot->actors.size());
  std::unordered_set<uint32_t> host_ids;
  std::set<SpawnKey> host_keys;
  for (const auto& host : snapshot->actors) {
    if ((host.id & edf::net::kMissionActorIdNamespace) == 0 ||
        host.call_ordinal >= 4096 || host.ant_ordinal >= 64 ||
        !host_ids.insert(host.id).second ||
        !host_keys.insert({host.call_ordinal, host.ant_ordinal}).second) continue;
    const auto found = by_key.find({host.call_ordinal, host.ant_ordinal});
    if (found == by_key.end() || found->second.size() != 1) continue;
    const auto& local = found->second.front();
    candidates.push_back({host.id, local.object, local.local_generation,
                          host.call_ordinal, host.ant_ordinal, false});
  }

  std::unordered_map<uint32_t, size_t> object_uses;
  for (const Candidate& candidate : candidates) ++object_uses[candidate.object];
  for (Candidate& candidate : candidates)
    if (object_uses[candidate.object] != 1) candidate.ambiguous = true;

  std::unordered_map<uint32_t, PreviousCandidate> next_candidates;
  size_t ambiguous = 0;
  for (const Candidate& candidate : candidates) {
    if (candidate.ambiguous) {
      ++ambiguous;
      continue;
    }
    const PreviousCandidate current{candidate.object, candidate.local_generation,
        snapshot->host_trace_generation, candidate.call_ordinal, candidate.ant_ordinal};
    const auto previous = previous_candidates.find(candidate.host_id);
    if (previous == previous_candidates.end() ||
        previous->second.object != current.object ||
        previous->second.local_generation != current.local_generation ||
        previous->second.host_generation != current.host_generation ||
        previous->second.call_ordinal != current.call_ordinal ||
        previous->second.ant_ordinal != current.ant_ordinal) {
      REXLOG_INFO("Mission actor identity candidate: host_id={} local_object={:#x} call_ordinal={} ant_ordinal={} host_trace_generation={} local_trace_generation={} evidence=spawn_ordinal_candidate",
          candidate.host_id, candidate.object, candidate.call_ordinal, candidate.ant_ordinal,
          snapshot->host_trace_generation, candidate.local_generation);
    }
    next_candidates.emplace(candidate.host_id, current);
  }
  if (candidates.size() != previous_candidates.size() || ambiguous ||
      snapshot->tick - last_processed_tick >= 60) {
    REXLOG_INFO("Mission actor identity summary: tick={} host_generation={} host_keys={} local_keys={} unique_candidates={} ambiguous={} evidence=diagnostic_only",
        snapshot->tick, snapshot->host_trace_generation, snapshot->actors.size(),
        local_keys.size(), next_candidates.size(), ambiguous);
  }
  previous_candidates = std::move(next_candidates);
  last_processed_tick = snapshot->tick;
  last_host_generation = snapshot->host_trace_generation;
  last_local_signature = std::move(local_signature);
#else
  (void)frame;
  (void)snapshot;
#endif
}

void ReconcileMissionActorStates(
    EngineFrame& frame, const edf::net::MissionActorStateSnapshot* snapshot,
    bool host_owns_mission_simulation) {
#if defined(_WIN32)
  using SpawnKey = std::pair<uint32_t, uint32_t>;
  static uint32_t last_processed_tick = 0;
  static uint64_t last_host_generation = 0;
  static size_t last_matched_count = std::numeric_limits<size_t>::max();
  static size_t last_unmatched_count = std::numeric_limits<size_t>::max();
  struct ProtectedFlag {
    uint8_t original = 0;
    uint64_t local_generation = 0;
    uint32_t call_ordinal = 0;
    uint32_t ant_ordinal = 0;
    uint32_t vtable = 0;
  };
  static std::unordered_map<uint32_t, ProtectedFlag> protected_flags;

  const auto clear_state = [&] {
    last_processed_tick = 0;
    last_host_generation = 0;
    last_matched_count = last_unmatched_count = std::numeric_limits<size_t>::max();
  };
  // Host/client trace generations are process-local and matching spawn
  // ordinals do not prove both games are on the same mission instance. Keep
  // guest-memory writes behind the explicit experimental operator opt-in.
  const bool reconciliation_enabled = REXCVAR_GET(edf_mp_mission_actor_reconcile) &&
      edf::multiplayer::MissionActorNetworkReplicationActive();
  const bool in_mission = edf::console::MissionWorldActive(frame);
  const GuestMemory memory(frame.base);
  if (!reconciliation_enabled || !in_mission || host_owns_mission_simulation) {
    if (protected_flags.empty()) {
      clear_state();
      return;
    }
    const auto keys = edf::multiplayer::SnapshotMissionAntTraceKeys(frame.base, true);
    std::unordered_map<uint32_t, edf::multiplayer::MissionAntTraceKey> current_keys;
    for (const auto& key : keys) current_keys.emplace(key.object, key);
    for (const auto& [object, saved] : protected_flags) {
      const auto key = current_keys.find(object);
      if (key == current_keys.end() || key->second.local_generation != saved.local_generation ||
          key->second.call_ordinal != saved.call_ordinal ||
          key->second.ant_ordinal != saved.ant_ordinal ||
          !GuestMemory::Heap(object, 738) || memory.U32(object) != saved.vtable ||
          memory.ClassName(object) != "clGiantAnt") continue;
      memory.W8(object + 625, saved.original);
    }
    protected_flags.clear();
    clear_state();
    return;
  }
  if (!EngineIdle(memory)) {
    clear_state();
    return;
  }

  if (!snapshot || !snapshot->tick || !snapshot->host_trace_generation) {
    clear_state();
    return;
  }
  const int32_t tick_delta = static_cast<int32_t>(snapshot->tick - last_processed_tick);
  if (last_processed_tick && tick_delta < 0) return;
  if (last_host_generation == snapshot->host_trace_generation && tick_delta == 0) return;
  const auto local_keys = edf::multiplayer::SnapshotMissionAntTraceKeys(frame.base, true);
  std::map<SpawnKey, std::vector<edf::multiplayer::MissionAntTraceKey>> local_by_spawn;
  for (const auto& key : local_keys)
    local_by_spawn[{key.call_ordinal, key.ant_ordinal}].push_back(key);

  std::unordered_set<uint32_t> claimed_objects;
  claimed_objects.reserve(snapshot->actors.size());
  size_t matched = 0;
  size_t unmatched = 0;
  for (const auto& state : snapshot->actors) {
    if (!state.trace_identity_valid ||
        state.trace_generation != snapshot->host_trace_generation ||
        state.archetype != edf::net::ActorArchetype::kAnt || !state.health) {
      ++unmatched;
      continue;
    }
    const auto found = local_by_spawn.find({state.trace_call_ordinal, state.trace_ant_ordinal});
    if (found == local_by_spawn.end() || found->second.size() != 1) {
      ++unmatched;
      continue;
    }
    const auto& key = found->second.front();
    const uint32_t object = key.object;
    if (!key.local_generation ||
        key.local_generation != edf::multiplayer::CurrentMissionAntTraceGeneration() ||
        !object || claimed_objects.contains(object) ||
        !GuestMemory::Heap(object, 738) || memory.ClassName(object) != "clGiantAnt" ||
        memory.U32(object + 604) != 1) {
      ++unmatched;
      continue;
    }
    const float maximum_health = memory.F32(object + 596);
    if (!std::isfinite(maximum_health) || maximum_health <= 0.0f) {
      ++unmatched;
      continue;
    }
    const float x = float(state.x_cm) * 0.01f;
    const float y = float(state.y_cm) * 0.01f;
    const float z = float(state.z_cm) * 0.01f;
    const float yaw = float(state.yaw_mrad) * 0.001f;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
        !std::isfinite(yaw)) {
      ++unmatched;
      continue;
    }

    // Keep the fields sampled by the host ant reader and the guest world matrix
    // aligned. The next 20 Hz host sample corrects local AI drift again.
    memory.WF32(object + 272, x);
    memory.WF32(object + 276, y);
    memory.WF32(object + 280, z);
    if (!ApplyRemoteActorTransform(memory, object, x, y, z, yaw)) {
      ++unmatched;
      continue;
    }
    memory.WF32(object + 592,
        std::clamp(float(state.health), 0.0f, maximum_health));
    // Exact-key host state owns these matched enemies. Revive a local-only
    // death if the guest still retains the object, and prevent client shots
    // from changing its health between server snapshots.
    memory.W8(object + 624, 0);
    // Protect only an actor that matched a host state sample. Save its original
    // damage flag so disconnecting or disabling reconciliation restores it.
    if (!host_owns_mission_simulation) {
      const uint32_t vtable = memory.U32(object);
      auto [saved, inserted] = protected_flags.try_emplace(object, ProtectedFlag{
          memory.U8(object + 625), key.local_generation, key.call_ordinal,
          key.ant_ordinal, vtable});
      if (!inserted && (saved->second.local_generation != key.local_generation ||
                        saved->second.call_ordinal != key.call_ordinal ||
                        saved->second.ant_ordinal != key.ant_ordinal ||
                        saved->second.vtable != vtable)) {
        saved->second = ProtectedFlag{memory.U8(object + 625), key.local_generation,
            key.call_ordinal, key.ant_ordinal, vtable};
      }
      memory.W8(object + 625, 1);
    }
    claimed_objects.insert(object);
    ++matched;
  }

  const bool counts_changed = matched != last_matched_count || unmatched != last_unmatched_count;
  if (counts_changed || !last_processed_tick ||
      last_host_generation != snapshot->host_trace_generation || tick_delta >= 60) {
    REXLOG_INFO("Mission actor reconciliation: tick={} host_generation={} host_actors={} matched={} unmatched={} local_keys={} mode=pose_health_only",
        snapshot->tick, snapshot->host_trace_generation, snapshot->actors.size(),
        matched, unmatched, local_keys.size());
  }
  last_processed_tick = snapshot->tick;
  last_host_generation = snapshot->host_trace_generation;
  last_matched_count = matched;
  last_unmatched_count = unmatched;
#else
  (void)frame;
  (void)snapshot;
#endif
}

std::vector<edf::net::ActorState> SampleHostMissionActors(EngineFrame& frame) {
  std::vector<edf::net::ActorState> actors;
#if defined(_WIN32)
  static uint64_t sample_generation = 0;
  static size_t next_sample_actor = 0;
  if (!edf::multiplayer::MissionActorNetworkReplicationActive() ||
      !edf::console::MissionWorldActive(frame)) return actors;
  const uint64_t generation = edf::multiplayer::CurrentMissionAntTraceGeneration();
  if (!generation) return actors;
  if (sample_generation != generation) {
    sample_generation = generation;
    next_sample_actor = 0;
  }

  const GuestMemory memory(frame.base);
  const auto keys = edf::multiplayer::SnapshotMissionAntTraceKeys(frame.base);
  actors.reserve(std::min(keys.size(), edf::net::kMaxSnapshotActors));
  for (const auto& key : keys) {
    const uint32_t object = key.object;
    if (key.local_generation != generation || !object ||
        !GuestMemory::Heap(object, 738) || memory.ClassName(object) != "clGiantAnt" ||
        memory.U32(object + 604) != 1 || memory.U8(object + 624) != 0) continue;
    const float health = memory.F32(object + 592);
    const float x = memory.F32(object + 272);
    const float y = memory.F32(object + 276);
    const float z = memory.F32(object + 280);
    const float yaw_x = memory.F32(object + 256);
    const float yaw_z = memory.F32(object + 264);
    int32_t x_cm = 0, y_cm = 0, z_cm = 0;
    if (!std::isfinite(health) || health <= 0.0f ||
        !ToNetworkCentimeters(x, x_cm) || !ToNetworkCentimeters(y, y_cm) ||
        !ToNetworkCentimeters(z, z_cm)) continue;
    const float yaw = std::isfinite(yaw_x) && std::isfinite(yaw_z)
        ? std::atan2(yaw_x, yaw_z) : 0.0f;
    const auto yaw_mrad = static_cast<int16_t>(std::clamp(
        std::lround(yaw * 1000.0f), -3142l, 3142l));
    const auto bounded_health = static_cast<uint16_t>(std::clamp(
        std::lround(std::min(health, 65535.0f)), 1l, 65535l));
    edf::net::ActorState actor{0, edf::net::ActorArchetype::kAnt,
        x_cm, y_cm, z_cm, yaw_mrad, bounded_health};
    actor.trace_identity_valid = true;
    actor.trace_generation = generation;
    actor.trace_call_ordinal = key.call_ordinal;
    actor.trace_ant_ordinal = key.ant_ordinal;
    actors.push_back(actor);
  }
  std::sort(actors.begin(), actors.end(), [](const auto& left, const auto& right) {
    return std::tie(left.trace_call_ordinal, left.trace_ant_ordinal) <
        std::tie(right.trace_call_ordinal, right.trace_ant_ordinal);
  });
  if (actors.size() > edf::net::kMaxSnapshotActors) {
    const size_t actor_count = actors.size();
    const size_t start = next_sample_actor % actor_count;
    std::vector<edf::net::ActorState> sample;
    sample.reserve(edf::net::kMaxSnapshotActors);
    for (size_t offset = 0; offset < edf::net::kMaxSnapshotActors; ++offset)
      sample.push_back(std::move(actors[(start + offset) % actor_count]));
    next_sample_actor = (start + sample.size()) % actor_count;
    actors = std::move(sample);
  } else {
    next_sample_actor = 0;
  }
#else
  (void)frame;
#endif
  return actors;
}

size_t ApplyHostMissionActorDamage(
    EngineFrame& frame, const std::vector<edf::net::MissionActorDamageRequest>& requests) {
#if defined(_WIN32)
  if (requests.empty() || !edf::multiplayer::MissionActorNetworkReplicationActive() ||
      !edf::console::MissionWorldActive(frame)) return 0;
  const uint64_t generation = edf::multiplayer::CurrentMissionAntTraceGeneration();
  if (!generation) return 0;
  const GuestMemory memory(frame.base);
  const auto keys = edf::multiplayer::SnapshotMissionAntTraceKeys(frame.base);
  using SpawnKey = std::pair<uint32_t, uint32_t>;
  std::map<SpawnKey, std::vector<uint32_t>> objects_by_spawn;
  for (const auto& key : keys) {
    if (key.local_generation == generation)
      objects_by_spawn[{key.call_ordinal, key.ant_ordinal}].push_back(key.object);
  }

  GuestCalls guest(*frame.ctx, frame.base);
  size_t applied = 0;
  constexpr size_t kMaxDamagePerEngineTick = 16;
  for (const auto& request : requests) {
    if (applied >= kMaxDamagePerEngineTick) break;
    if (request.trace_generation != generation ||
        request.call_ordinal >= 4096 || request.ant_ordinal >= 64 ||
        !std::isfinite(request.damage_points) || request.damage_points <= 0.0f ||
        request.damage_points > 65535.0f) continue;
    const auto found = objects_by_spawn.find({request.call_ordinal, request.ant_ordinal});
    if (found == objects_by_spawn.end() || found->second.size() != 1) continue;
    const uint32_t object = found->second.front();
    if (!GuestMemory::Heap(object, 738) || memory.ClassName(object) != "clGiantAnt" ||
        memory.U32(object + 604) != 1 || memory.U8(object + 624) != 0) continue;
    const float health = memory.F32(object + 592);
    if (!std::isfinite(health) || health <= 0.0f) continue;
    const uint32_t mark = guest.Mark();
    try {
      const uint32_t damage = guest.Alloc(128);
      memory.W32(damage + 20, 0xFFFFFFFFu);
      memory.WF32(damage + 24, request.damage_points);
      (void)guest.Call(sub_820D59D0, GuestCalls::R({object, damage, 1}));
      ++applied;
    } catch (...) {
      guest.Release(mark);
      continue;
    }
    guest.Release(mark);
  }
  return applied;
#else
  (void)frame;
  (void)requests;
  return 0;
#endif
}

std::vector<uint32_t> UpdateHostServerActors(
    EngineFrame& frame, const std::vector<edf::net::ActorSpawnRequest>& spawns,
    std::vector<edf::net::ActorState>& actors_out) {
  std::vector<uint32_t> spawned;
  actors_out.clear();
#if defined(_WIN32)
  if (!edf::multiplayer::MissionActorNetworkReplicationActive() ||
      !edf::console::MissionWorldActive(frame)) {
    host_server_actor_objects.clear();
    host_server_actor_generation = 0;
    return spawned;
  }
  const uint64_t generation = edf::multiplayer::CurrentMissionAntTraceGeneration();
  if (!generation) {
    host_server_actor_objects.clear();
    host_server_actor_generation = 0;
    return spawned;
  }
  if (generation != host_server_actor_generation) {
    // A mission load can recycle guest heap addresses. Forget old pointers
    // without dereferencing them after the trace generation changes.
    host_server_actor_objects.clear();
    host_server_actor_generation = generation;
  }

  const GuestMemory memory(frame.base);
  GuestCalls guest(*frame.ctx, frame.base);
  constexpr size_t kMaxNativeSpawnsPerTick = 4;
  size_t created_this_tick = 0;
  for (const auto& request : spawns) {
    if (created_this_tick >= kMaxNativeSpawnsPerTick || !request.id ||
        (request.id & edf::net::kMissionActorIdNamespace) ||
        request.archetype != edf::net::ActorArchetype::kAnt ||
        request.x_cm < -1'000'000 || request.x_cm > 1'000'000 ||
        request.y_cm < -1'000'000 || request.y_cm > 1'000'000 ||
        request.z_cm < -1'000'000 || request.z_cm > 1'000'000 ||
        request.yaw_mrad < -3142 || request.yaw_mrad > 3142 ||
        host_server_actor_objects.contains(request.id)) continue;
    const uint32_t object = CreateObject(guest, kTypes[0],
        static_cast<float>(request.x_cm) / 100.0f,
        static_cast<float>(request.y_cm) / 100.0f,
        static_cast<float>(request.z_cm) / 100.0f,
        static_cast<float>(request.yaw_mrad) / 1000.0f, true);
    if (!GuestMemory::Heap(object, 738) || memory.ClassName(object) != "clGiantAnt" ||
        memory.U32(object + 604) != 1 || memory.U8(object + 624) != 0) {
      if (GuestMemory::Heap(object, 8)) guest.Call(sub_821C0ED8, GuestCalls::R({object}));
      continue;
    }
    host_server_actor_objects.emplace(request.id, object);
    spawned.push_back(request.id);
    ++created_this_tick;
  }

  actors_out.reserve(std::min(host_server_actor_objects.size(), edf::net::kMaxSnapshotActors));
  for (auto it = host_server_actor_objects.begin(); it != host_server_actor_objects.end();) {
    const uint32_t id = it->first;
    const uint32_t object = it->second;
    if (!GuestMemory::Heap(object, 738) || memory.ClassName(object) != "clGiantAnt" ||
        memory.U32(object + 604) != 1 || memory.U8(object + 624) != 0) {
      it = host_server_actor_objects.erase(it);
      continue;
    }
    const float health = memory.F32(object + 592);
    int32_t x_cm = 0, y_cm = 0, z_cm = 0;
    if (!std::isfinite(health) || health <= 0.0f ||
        !ToNetworkCentimeters(memory.F32(object + 272), x_cm) ||
        !ToNetworkCentimeters(memory.F32(object + 276), y_cm) ||
        !ToNetworkCentimeters(memory.F32(object + 280), z_cm)) {
      it = host_server_actor_objects.erase(it);
      continue;
    }
    const float yaw_x = memory.F32(object + 256);
    const float yaw_z = memory.F32(object + 264);
    const float yaw = std::isfinite(yaw_x) && std::isfinite(yaw_z)
        ? std::atan2(yaw_x, yaw_z) : 0.0f;
    edf::net::ActorState actor;
    actor.id = id;
    actor.archetype = edf::net::ActorArchetype::kAnt;
    actor.x_cm = x_cm;
    actor.y_cm = y_cm;
    actor.z_cm = z_cm;
    actor.yaw_mrad = static_cast<int16_t>(std::clamp(
        std::lround(yaw * 1000.0f), -3142l, 3142l));
    actor.health = static_cast<uint16_t>(std::clamp(
        std::lround(std::min(health, 65535.0f)), 1l, 65535l));
    actor.server_owned = true;
    if (actors_out.size() < edf::net::kMaxSnapshotActors)
      actors_out.push_back(actor);
    ++it;
  }
#else
  (void)frame;
  (void)spawns;
#endif
  return spawned;
}

size_t ApplyHostServerActorDamage(
    EngineFrame& frame, const std::vector<edf::net::ActorDamageRequest>& requests) {
#if defined(_WIN32)
  if (requests.empty() || !edf::multiplayer::MissionActorNetworkReplicationActive() ||
      !edf::console::MissionWorldActive(frame)) return 0;
  const uint64_t generation = edf::multiplayer::CurrentMissionAntTraceGeneration();
  if (!generation || generation != host_server_actor_generation) return 0;
  const GuestMemory memory(frame.base);
  GuestCalls guest(*frame.ctx, frame.base);
  size_t applied = 0;
  constexpr size_t kMaxDamagePerEngineTick = 16;
  for (const auto& request : requests) {
    if (applied >= kMaxDamagePerEngineTick) break;
    const auto found = host_server_actor_objects.find(request.actor_id);
    if (!request.actor_id || found == host_server_actor_objects.end() ||
        !std::isfinite(request.damage_points) || request.damage_points <= 0.0f ||
        request.damage_points > 65535.0f) continue;
    const uint32_t object = found->second;
    if (!GuestMemory::Heap(object, 738) || memory.ClassName(object) != "clGiantAnt" ||
        memory.U32(object + 604) != 1 || memory.U8(object + 624) != 0 ||
        !std::isfinite(memory.F32(object + 592)) || memory.F32(object + 592) <= 0.0f) {
      host_server_actor_objects.erase(found);
      continue;
    }
    const uint32_t mark = guest.Mark();
    try {
      const uint32_t damage = guest.Alloc(128);
      memory.W32(damage + 20, 0xFFFFFFFFu);
      memory.WF32(damage + 24, request.damage_points);
      (void)guest.Call(sub_820D59D0, GuestCalls::R({object, damage, 1}));
      ++applied;
    } catch (...) {
      guest.Release(mark);
      continue;
    }
    guest.Release(mark);
  }
  return applied;
#else
  (void)frame;
  (void)requests;
  return 0;
#endif
}

void ClearHostServerActors(EngineFrame& frame) {
#if defined(_WIN32)
  if (!host_server_actor_objects.empty()) {
    const GuestMemory memory(frame.base);
    GuestCalls guest(*frame.ctx, frame.base);
    for (const auto& [id, object] : host_server_actor_objects) {
      (void)id;
      if (GuestMemory::Heap(object, 8) && memory.ClassName(object) == "clGiantAnt")
        guest.Call(sub_821C0ED8, GuestCalls::R({object}));
    }
  }
  host_server_actor_objects.clear();
  host_server_actor_generation = 0;
#else
  (void)frame;
#endif
}

void ReconcileMissionActorRemovals(
    EngineFrame& frame, const edf::net::MissionActorRemovalSnapshot* snapshot) {
#if defined(_WIN32)
  if (!snapshot || !snapshot->tick || !snapshot->host_trace_generation ||
      !REXCVAR_GET(edf_mp_mission_actor_reconcile) ||
      !edf::multiplayer::MissionActorNetworkReplicationActive() ||
      !edf::console::MissionWorldActive(frame)) return;
  const GuestMemory memory(frame.base);
  if (!EngineIdle(memory)) return;

  const uint64_t local_generation =
      edf::multiplayer::CurrentMissionAntTraceGeneration();
  if (!local_generation) return;
  const auto local_keys = edf::multiplayer::SnapshotMissionAntTraceKeys(frame.base, true);
  using SpawnKey = std::pair<uint32_t, uint32_t>;
  std::map<SpawnKey, std::vector<edf::multiplayer::MissionAntTraceKey>> local_by_spawn;
  for (const auto& key : local_keys)
    local_by_spawn[{key.call_ordinal, key.ant_ordinal}].push_back(key);

  GuestCalls guest(*frame.ctx, frame.base);
  std::unordered_set<uint32_t> removed_objects;
  for (const auto& removal : snapshot->removals) {
    if ((removal.id & edf::net::kMissionActorIdNamespace) == 0 ||
        removal.call_ordinal >= 4096 || removal.ant_ordinal >= 64) continue;
    const auto found = local_by_spawn.find({removal.call_ordinal, removal.ant_ordinal});
    if (found == local_by_spawn.end() || found->second.size() != 1) continue;
    const auto& key = found->second.front();
    const uint32_t object = key.object;
    if (!key.local_generation || key.local_generation != local_generation ||
        !object || removed_objects.contains(object) ||
        !GuestMemory::Heap(object, 738) || memory.ClassName(object) != "clGiantAnt") continue;

    // The server reports a generic entity removal, not a kill. Reuse the same
    // scenegraph removal routine the local game and replica cleanup already use.
    guest.Call(sub_821C0ED8, GuestCalls::R({object}));
    removed_objects.insert(object);
    REXLOG_INFO("Mission actor reconciliation: host_removed id={} host_generation={} call_ordinal={} ant_ordinal={}",
        removal.id, snapshot->host_trace_generation,
        removal.call_ordinal, removal.ant_ordinal);
  }
#else
  (void)frame;
  (void)snapshot;
#endif
}
#endif

void RegisterGameCommands(Registry& r) {
  static bool ticking = false;
  if (!ticking) { AddEngineTick(GameTick); ticking = true; }
  const auto on_off = [] { return std::vector<std::string>{"on", "off"}; };
  r.Add({.name = "spawn", .usage = "spawn <type|list> [count] [distance | x y z]",
         .help = "Create enemies around the player (a ring at distance, default 30) or at x y z",
         .args = {{.name = "type", .type = ArgType::Choice, .choices = [] { return TypeNames(true); }},
                  {.name = "count", .type = ArgType::Int, .optional = true, .min = 1, .max = 2000},
                  {.name = "distance|x", .type = ArgType::Float, .optional = true, .min = -100000, .max = 100000},
                  {.name = "y", .type = ArgType::Float, .optional = true, .min = -100000, .max = 100000},
                  {.name = "z", .type = ArgType::Float, .optional = true, .min = -100000, .max = 100000}},
         .flags = kEngine | kCheat,
         .validate = [](const std::vector<std::string>& args) -> std::string {
           if (!args.empty() && Lower(args[0]) == "list") return args.size() == 1 ? "" : "spawn list takes no arguments";
           if (args.size() == 4) return "give a distance, or all three of x y z";
           if (args.size() == 3 && *ParseFloat(args[2]) < 0) return "distance must not be negative";
           return {};
         },
         .handler = [](Invocation& in) {
           auto& ctx = *static_cast<ExecContext*>(in.host());
           if (Lower(in.str(0)) != "list" && !GameInMission(*ctx.frame)) {
             in.Error("spawn: not in a mission (start or continue a mission first)");
             return;
           }
           Spawn(in);
         }});
  r.Add({.name = "killall", .usage = "killall [type|class|all|spawned] [remove]",
         .help = "Kill every live invader (faction 1), one type, or only console-spawned ones; 'remove' deletes instead",
         .args = {{.name = "type", .optional = true,
                   .choices = [] { auto names = TypeNames(false); names.push_back("all"); names.push_back("spawned"); names.push_back("remove"); return names; }},
                  {.name = "remove", .optional = true, .choices = [] { return std::vector<std::string>{"remove"}; }}},
         .flags = kEngine | kCheat | kMission, .handler = KillAll});
  r.Add({.name = "god", .usage = "god [on|off]", .help = "Player invulnerable (and kept at full health); no argument toggles",
         .args = {{.name = "state", .type = ArgType::Bool, .optional = true, .choices = on_off}},
         .flags = kEngine | kCheat,
         .handler = [](Invocation& in) {
           const bool on = in.has(0) ? *ParseBool(in.str(0)) : !god.load();
           if (!on && god.load()) god_release = true;
           god = on;
           in.Ok(on ? "god: on" : "god: off");
         }});
  r.Add({.name = "ammo", .usage = "ammo [on|off|refill]", .help = "Infinite ammunition (magazines kept full); 'refill' once",
         .args = {{.name = "state", .optional = true, .choices = [] { return std::vector<std::string>{"on", "off", "refill"}; }}},
         .flags = kEngine | kCheat,
         .validate = [](const std::vector<std::string>& args) -> std::string {
           if (args.empty() || Lower(args[0]) == "refill" || ParseBool(args[0])) return {};
           return "ammo takes on, off or refill";
         },
         .handler = [](Invocation& in) {
           if (in.has(0) && Lower(in.str(0)) == "refill") {
             auto& ctx = *static_cast<ExecContext*>(in.host());
             const GuestMemory m(ctx.frame->base);
             const uint32_t player = FindPlayer(m);
             if (!player) { in.Error("ammo: no player object (not in a mission)"); return; }
             RefillAmmo(m, player);
             in.Ok("ammo: refilled");
             return;
           }
           const bool on = in.has(0) ? *ParseBool(in.str(0)) : !infinite_ammo.load();
           infinite_ammo = on;
           in.Ok(on ? "ammo: infinite" : "ammo: normal");
         }});
  r.Add({.name = "pos", .usage = "pos", .help = "Print the player's position, heading and health",
         .flags = kEngine | kMission, .handler = Pos});
  r.Add({.name = "teleport", .usage = "teleport <x> <y> <z>", .help = "Move the player (see 'pos')",
         .args = {{.name = "x", .type = ArgType::Float, .min = -100000, .max = 100000},
                  {.name = "y", .type = ArgType::Float, .min = -100000, .max = 100000},
                  {.name = "z", .type = ArgType::Float, .min = -100000, .max = 100000}},
         .flags = kEngine | kCheat | kMission, .handler = Teleport});
  r.Add({.name = "destroy", .usage = "destroy <radius R | all | nearest [n] | R> [max]",
         .help = "Collapse buildings around the player (map objects, through their own death branch)",
         .args = {{.name = "mode", .choices = [] { return std::vector<std::string>{"radius", "all", "nearest"}; }},
                  {.name = "value", .type = ArgType::Float, .optional = true, .min = 0, .max = 1e6},
                  {.name = "max", .type = ArgType::Int, .optional = true, .min = 1, .max = 100000}},
         .flags = kEngine | kCheat | kMission,
         .validate = [](const std::vector<std::string>& args) -> std::string {
           const auto mode = Lower(args[0]);
           if (mode == "radius" || mode == "all" || mode == "nearest") return {};
           if (const auto radius = ParseFloat(mode); radius && *radius > 0) return args.size() <= 2 ? "" : "too many arguments";
           return "mode must be radius, all, nearest or a radius";
         },
         .handler = Destroy});
  r.Add({.name = "effects", .usage = "effects <count> [radius] [scale] [per_tick]",
         .help = "Explosion effects around the player (visual only: no damage)",
         .args = {{.name = "count", .type = ArgType::Int, .min = 1, .max = 5000},
                  {.name = "radius", .type = ArgType::Float, .optional = true, .min = 0, .max = 1000},
                  {.name = "scale", .type = ArgType::Float, .optional = true, .min = 0.1, .max = 20},
                  {.name = "per_tick", .type = ArgType::Int, .optional = true, .min = 1, .max = 5000}},
         .flags = kEngine | kCheat | kMission, .handler = Effects});
  r.Add({.name = "timescale", .usage = "timescale [scale]",
         .help = "Simulation speed: engine ticks per second = 60 x scale (0.1 to 4; 1 = normal)",
         .args = {{.name = "scale", .type = ArgType::Float, .optional = true, .min = 0.1, .max = 4.0}},
         .handler = [](Invocation& in) {
           auto& request = edf::native::NativeTimeScaleRequest();
           if (in.has(0)) {
             request.store(in.number(0));
             if (in.number(0) != 1.0) Service::Get().MarkCheat("timescale");
             REXLOG_INFO("Console: timescale {}", in.number(0));
           }
           in.Ok(Fmt("timescale: %.2f (%.0f simulation ticks per second; the game is frame-locked, so this is its speed)",
                     request.load(), 60.0 * request.load()));
         }});
  r.Add({.name = "stats", .usage = "stats [all]",
         .help = "Frame rate and times, simulation step times, object counts per class and faction",
         .args = {{.name = "all", .optional = true, .choices = [] { return std::vector<std::string>{"all"}; }}},
         .flags = kEngine, .handler = Stats});
}

}  // namespace edf::console

#if defined(EDF2027_BUILD_MULTIPLAYER)
REX_EXTERN(__imp__sub_821C0ED8);
REX_HOOK_RAW(sub_821C0ED8) {
#if defined(_WIN32)
  edf::multiplayer::TraceMissionAntRemoval(base, ctx.r3.u32, uint32_t(ctx.lr));
#endif
  edf::console::ForgetRemoteReplicaObject(ctx.r3.u32);
  __imp__sub_821C0ED8(ctx, base);
}
#endif

// The vanish query's callback, borrowed as the console's collector while "destroy" asks the
// grid for the objects around a point; otherwise the original.
REX_EXTERN(__imp__sub_820B3B10);
REX_HOOK_RAW(sub_820B3B10) {
  if (auto* sink = edf::console::collecting) {
    sink->push_back(ctx.r3.u32);
    return;
  }
  __imp__sub_820B3B10(ctx, base);
}
