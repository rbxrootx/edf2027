#include "script_runtime.h"

#include <luacode.h>
#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <unordered_map>

namespace edf::net {
namespace {
constexpr size_t kScriptMemoryLimit = 16 * 1024 * 1024;
constexpr uint32_t kInstructionBudget = 200'000;
constexpr size_t kMaxHooksPerEvent = 256;

ScriptRuntime* Runtime(lua_State* state) {
  return static_cast<ScriptRuntime*>(lua_callbacks(state)->userdata);
}

void RemoveGlobal(lua_State* state, const char* name) {
  lua_pushnil(state);
  lua_setglobal(state, name);
}

double CheckWholeNumber(lua_State* state, int argument) {
  const double value = luaL_checknumber(state, argument);
  if (!std::isfinite(value) || std::floor(value) != value) {
    luaL_error(state, "expected a finite whole number");
    return 0.0;
  }
  return value;
}
}  // namespace

ScriptRuntime::ScriptRuntime(Log log, Broadcast broadcast, RebasePlayer rebase_player, ListPlayers list_players,
                             SpawnActor spawn_actor, DamageActor damage_actor, ListActors list_actors,
                             SetActorTransform set_actor_transform, DamagePlayer damage_player)
    : log_(std::move(log)), broadcast_(std::move(broadcast)), rebase_player_(std::move(rebase_player)),
      list_players_(std::move(list_players)), list_actors_(std::move(list_actors)),
      spawn_actor_(std::move(spawn_actor)),
      damage_actor_(std::move(damage_actor)), set_actor_transform_(std::move(set_actor_transform)),
      damage_player_(std::move(damage_player)) {
  Initialize();
}

void ScriptRuntime::Initialize() {
  state_ = lua_newstate(&ScriptRuntime::Allocate, this);
  if (!state_) return;
  lua_callbacks(state_)->userdata = this;
  lua_callbacks(state_)->interrupt = &ScriptRuntime::Interrupt;
  luaL_openlibs(state_);
  // Server scripts get language libraries only. Files, process/environment access,
  // dynamic module loading, debug facilities, and bytecode loading stay unavailable.
  for (const char* global : {"os", "debug", "io", "package", "require", "dofile", "loadfile", "loadstring", "collectgarbage"})
    RemoveGlobal(state_, global);
  lua_newtable(state_);
  lua_pushcfunction(state_, &ScriptRuntime::RegisterHook, "edf.on");
  lua_setfield(state_, -2, "on");
  lua_pushcfunction(state_, &ScriptRuntime::BroadcastMessage, "edf.broadcast");
  lua_setfield(state_, -2, "broadcast");
  lua_pushcfunction(state_, &ScriptRuntime::RebasePlayerPosition, "edf.rebase_player");
  lua_setfield(state_, -2, "rebase_player");
  lua_pushcfunction(state_, &ScriptRuntime::ReadPlayers, "edf.players");
  lua_setfield(state_, -2, "players");
  lua_pushcfunction(state_, &ScriptRuntime::ReadActors, "edf.actors");
  lua_setfield(state_, -2, "actors");
  lua_pushcfunction(state_, &ScriptRuntime::SpawnServerActor, "edf.spawn_actor");
  lua_setfield(state_, -2, "spawn_actor");
  lua_pushcfunction(state_, &ScriptRuntime::DamageServerActor, "edf.damage_actor");
  lua_setfield(state_, -2, "damage_actor");
  lua_pushcfunction(state_, &ScriptRuntime::DamageServerPlayer, "edf.damage_player");
  lua_setfield(state_, -2, "damage_player");
  lua_pushcfunction(state_, &ScriptRuntime::SetServerActorTransform, "edf.set_actor_transform");
  lua_setfield(state_, -2, "set_actor_transform");
  lua_setglobal(state_, "edf");
  luaL_sandbox(state_);
}

ScriptRuntime::~ScriptRuntime() {
  if (state_) lua_close(state_);
}

void ScriptRuntime::Reset() {
  if (state_) lua_close(state_);
  state_ = nullptr;
  allocated_bytes_ = 0;
  instruction_budget_ = 0;
  hooks_.clear();
  Initialize();
}

void* ScriptRuntime::Allocate(void* ud, void* ptr, size_t old_size, size_t new_size) {
  auto* self = static_cast<ScriptRuntime*>(ud);
  const size_t old = ptr ? old_size : 0;
  if (!new_size) {
    std::free(ptr);
    self->allocated_bytes_ = old <= self->allocated_bytes_ ? self->allocated_bytes_ - old : 0;
    return nullptr;
  }
  if (new_size > old && new_size - old > kScriptMemoryLimit - std::min(kScriptMemoryLimit, self->allocated_bytes_)) return nullptr;
  void* next = std::realloc(ptr, new_size);
  if (next) self->allocated_bytes_ = self->allocated_bytes_ - std::min(old, self->allocated_bytes_) + new_size;
  return next;
}

void ScriptRuntime::Interrupt(lua_State* state, int) {
  auto* self = Runtime(state);
  if (!self) return;
  if (self->instruction_budget_ <= 1000) luaL_error(state, "server script instruction budget exceeded");
  self->instruction_budget_ -= 1000;
}

int ScriptRuntime::RegisterHook(lua_State* state) {
  const char* event = luaL_checkstring(state, 1);
  const size_t n = std::char_traits<char>::length(event);
  if (n > 32 || (std::string_view(event) != "tick" && std::string_view(event) != "player_join" && std::string_view(event) != "player_leave"))
    { luaL_error(state, "unsupported EDF server hook"); return 0; }
  luaL_checktype(state, 2, LUA_TFUNCTION);
  auto* self = Runtime(state);
  if (!self) { luaL_error(state, "server runtime is unavailable"); return 0; }
  const auto existing = self->hooks_.find(event);
  if (existing != self->hooks_.end() && existing->second.size() >= kMaxHooksPerEvent) {
    luaL_error(state, "server hook limit exceeded");
    return 0;
  }
  lua_pushvalue(state, 2);
  self->hooks_[event].push_back(lua_ref(state, -1));
  lua_pop(state, 1);
  return 0;
}

int ScriptRuntime::BroadcastMessage(lua_State* state) {
  size_t size = 0;
  const char* text = luaL_checklstring(state, 1, &size);
  if (size > kMaxChatBytes) { luaL_error(state, "broadcast text is limited to 255 bytes"); return 0; }
  if (auto* self = Runtime(state); self && self->broadcast_) self->broadcast_(std::string(text, size));
  return 0;
}

int ScriptRuntime::RebasePlayerPosition(lua_State* state) {
  const double player_id = CheckWholeNumber(state, 1);
  const double x_cm = CheckWholeNumber(state, 2);
  const double y_cm = CheckWholeNumber(state, 3);
  const double z_cm = CheckWholeNumber(state, 4);
  const double yaw_mrad = CheckWholeNumber(state, 5);
  if (player_id <= 0 || player_id > 65535 ||
      x_cm < -1'000'000 || x_cm > 1'000'000 ||
      y_cm < -1'000'000 || y_cm > 1'000'000 ||
      z_cm < -1'000'000 || z_cm > 1'000'000 ||
      yaw_mrad < -3142 || yaw_mrad > 3142) {
    luaL_error(state, "invalid player rebase coordinates");
    return 0;
  }
  auto* self = Runtime(state);
  const bool ok = self && self->rebase_player_ && self->rebase_player_(
      static_cast<uint16_t>(player_id), static_cast<int32_t>(x_cm), static_cast<int32_t>(y_cm),
      static_cast<int32_t>(z_cm), static_cast<int16_t>(yaw_mrad));
  lua_pushboolean(state, ok);
  return 1;
}

int ScriptRuntime::ReadPlayers(lua_State* state) {
  auto* self = Runtime(state);
  const std::vector<ScriptPlayerView> players = self && self->list_players_
      ? self->list_players_() : std::vector<ScriptPlayerView>{};
  lua_createtable(state, static_cast<int>(players.size()), 0);
  int index = 1;
  for (const ScriptPlayerView& view : players) {
    const PlayerState& player = view.state;
    const InputCommand& input = view.input;
    lua_createtable(state, 0, 22);
    lua_pushinteger(state, player.id);
    lua_setfield(state, -2, "id");
    lua_pushlstring(state, player.name.data(), player.name.size());
    lua_setfield(state, -2, "name");
    lua_pushboolean(state, player.active);
    lua_setfield(state, -2, "active");
    lua_pushboolean(state, player.health_valid);
    lua_setfield(state, -2, "health_valid");
    lua_pushboolean(state, player.alive);
    lua_setfield(state, -2, "alive");
    lua_pushinteger(state, player.health);
    lua_setfield(state, -2, "health");
    lua_pushinteger(state, player.max_health);
    lua_setfield(state, -2, "max_health");
    lua_pushinteger(state, player.x_cm);
    lua_setfield(state, -2, "x_cm");
    lua_pushinteger(state, player.y_cm);
    lua_setfield(state, -2, "y_cm");
    lua_pushinteger(state, player.z_cm);
    lua_setfield(state, -2, "z_cm");
    lua_pushinteger(state, player.yaw_mrad);
    lua_setfield(state, -2, "yaw_mrad");
    lua_pushinteger(state, player.buttons);
    lua_setfield(state, -2, "buttons");
    lua_pushinteger(state, player.left_trigger);
    lua_setfield(state, -2, "left_trigger");
    lua_pushinteger(state, player.right_trigger);
    lua_setfield(state, -2, "right_trigger");
    lua_pushinteger(state, input.sequence);
    lua_setfield(state, -2, "input_sequence");
    lua_pushinteger(state, player.active ? input.move_x : 0);
    lua_setfield(state, -2, "move_x");
    lua_pushinteger(state, player.active ? input.move_y : 0);
    lua_setfield(state, -2, "move_y");
    lua_pushinteger(state, player.active ? input.aim_x : 0);
    lua_setfield(state, -2, "aim_x");
    lua_pushinteger(state, player.active ? input.aim_y : 0);
    lua_setfield(state, -2, "aim_y");
    lua_pushinteger(state, player.active ? input.aim_yaw_mrad : player.yaw_mrad);
    lua_setfield(state, -2, "aim_yaw_mrad");
    lua_pushboolean(state, input.gameplay_active);
    lua_setfield(state, -2, "gameplay_active");
    lua_pushboolean(state, input.request_spawn);
    lua_setfield(state, -2, "request_spawn");
    lua_rawseti(state, -2, index++);
  }
  return 1;
}

int ScriptRuntime::ReadActors(lua_State* state) {
  auto* self = Runtime(state);
  const std::vector<ActorState> actors = self && self->list_actors_ ? self->list_actors_() : std::vector<ActorState>{};
  // The detached script view may combine two independently bounded host lists:
  // explicit server actor-source records and guest-sampled mission ants.
  const size_t actor_count = std::min(actors.size(), 2 * kMaxSnapshotActors);
  lua_createtable(state, static_cast<int>(actor_count), 0);
  for (size_t i = 0; i < actor_count; ++i) {
    const ActorState& actor = actors[i];
    lua_createtable(state, 0, 8);
    lua_pushinteger(state, actor.id);
    lua_setfield(state, -2, "id");
    const char* archetype = actor.archetype == ActorArchetype::kAnt ? "ant" : "unknown";
    lua_pushstring(state, archetype);
    lua_setfield(state, -2, "archetype");
    lua_pushinteger(state, actor.x_cm);
    lua_setfield(state, -2, "x_cm");
    lua_pushinteger(state, actor.y_cm);
    lua_setfield(state, -2, "y_cm");
    lua_pushinteger(state, actor.z_cm);
    lua_setfield(state, -2, "z_cm");
    lua_pushinteger(state, actor.yaw_mrad);
    lua_setfield(state, -2, "yaw_mrad");
    lua_pushinteger(state, actor.health);
    lua_setfield(state, -2, "health");
    lua_pushboolean(state, actor.server_owned);
    lua_setfield(state, -2, "server_owned");
    lua_rawseti(state, -2, static_cast<int>(i + 1));
  }
  return 1;
}

int ScriptRuntime::SpawnServerActor(lua_State* state) {
  size_t type_size = 0;
  const char* type = luaL_checklstring(state, 1, &type_size);
  const double x_cm = CheckWholeNumber(state, 2);
  const double y_cm = CheckWholeNumber(state, 3);
  const double z_cm = CheckWholeNumber(state, 4);
  const double yaw_mrad = CheckWholeNumber(state, 5);
  if (type_size != 3 || std::string_view(type, type_size) != "ant" ||
      x_cm < -1'000'000 || x_cm > 1'000'000 ||
      y_cm < -1'000'000 || y_cm > 1'000'000 ||
      z_cm < -1'000'000 || z_cm > 1'000'000 ||
      yaw_mrad < -3142 || yaw_mrad > 3142) {
    luaL_error(state, "edf.spawn_actor expects 'ant', x/y/z centimeters, and yaw milliradians in range");
    return 0;
  }
  auto* self = Runtime(state);
  std::optional<uint32_t> id;
  try {
    if (self && self->spawn_actor_)
      id = self->spawn_actor_(std::string_view(type, type_size), static_cast<int32_t>(x_cm),
          static_cast<int32_t>(y_cm), static_cast<int32_t>(z_cm), static_cast<int16_t>(yaw_mrad));
  } catch (...) {
    id.reset();
  }
  if (id) lua_pushinteger(state, *id);
  else lua_pushnil(state);
  return 1;
}

int ScriptRuntime::DamageServerActor(lua_State* state) {
  const double actor_id = CheckWholeNumber(state, 1);
  const double damage_points = luaL_checknumber(state, 2);
  constexpr double kMaximumDamagePoints = 65535.0;
  if (actor_id <= 0 || actor_id > UINT32_MAX || !std::isfinite(damage_points) ||
      damage_points <= 0.0 || damage_points > kMaximumDamagePoints) {
    luaL_error(state, "edf.damage_actor expects a positive actor ID and damage_points in (0, 65535]");
    return 0;
  }
  auto* self = Runtime(state);
  bool queued = false;
  try {
    if (self && self->damage_actor_)
      queued = self->damage_actor_(static_cast<uint32_t>(actor_id), static_cast<float>(damage_points));
  } catch (...) {
    queued = false;
  }
  lua_pushboolean(state, queued);
  return 1;
}

int ScriptRuntime::DamageServerPlayer(lua_State* state) {
  const double player_id = CheckWholeNumber(state, 1);
  const double damage_points = CheckWholeNumber(state, 2);
  if (player_id < 1 || player_id > kMaxServerPlayers || damage_points < 1 || damage_points > 65535) {
    luaL_error(state, "edf.damage_player expects a player ID and whole damage_points in [1, 65535]");
    return 0;
  }
  auto* self = Runtime(state);
  bool applied = false;
  try {
    if (self && self->damage_player_)
      applied = self->damage_player_(static_cast<uint16_t>(player_id),
                                     static_cast<uint16_t>(damage_points));
  } catch (...) {
    applied = false;
  }
  lua_pushboolean(state, applied);
  return 1;
}

int ScriptRuntime::SetServerActorTransform(lua_State* state) {
  const double actor_id = CheckWholeNumber(state, 1);
  const double x_cm = CheckWholeNumber(state, 2);
  const double y_cm = CheckWholeNumber(state, 3);
  const double z_cm = CheckWholeNumber(state, 4);
  const double yaw_mrad = CheckWholeNumber(state, 5);
  if (actor_id <= 0 || actor_id > UINT32_MAX ||
      x_cm < -1'000'000 || x_cm > 1'000'000 ||
      y_cm < -1'000'000 || y_cm > 1'000'000 ||
      z_cm < -1'000'000 || z_cm > 1'000'000 ||
      yaw_mrad < -3142 || yaw_mrad > 3142) {
    luaL_error(state, "edf.set_actor_transform expects an actor ID, x/y/z centimeters, and yaw milliradians in range");
    return 0;
  }
  auto* self = Runtime(state);
  bool updated = false;
  try {
    if (self && self->set_actor_transform_)
      updated = self->set_actor_transform_(static_cast<uint32_t>(actor_id),
          static_cast<int32_t>(x_cm), static_cast<int32_t>(y_cm), static_cast<int32_t>(z_cm),
          static_cast<int16_t>(yaw_mrad));
  } catch (...) {
    updated = false;
  }
  lua_pushboolean(state, updated);
  return 1;
}

bool ScriptRuntime::LoadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) { if (log_) log_("Luau: cannot open " + path.string()); return false; }
  std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (source.size() > 1024 * 1024) { if (log_) log_("Luau: script exceeds 1 MiB: " + path.string()); return false; }
  return Run(source, "@" + path.filename().string());
}

bool ScriptRuntime::RunString(std::string_view source, std::string_view chunk_name) { return Run(source, chunk_name); }

bool ScriptRuntime::Run(std::string_view source, std::string_view chunk_name) {
  if (!state_) return false;
  lua_CompileOptions options{};
  options.optimizationLevel = 1;
  options.debugLevel = 1;
  size_t bytecode_size = 0;
  char* bytecode = luau_compile(source.data(), source.size(), &options, &bytecode_size);
  if (!bytecode) { if (log_) log_("Luau: compiler allocation failed"); return false; }
  const std::string chunk(chunk_name);
  instruction_budget_ = kInstructionBudget;
  const int load_result = luau_load(state_, chunk.c_str(), bytecode, bytecode_size, 0);
  std::free(bytecode);
  if (load_result != 0) { LogError("Luau compile/load"); lua_pop(state_, 1); return false; }
  const bool ok = lua_pcall(state_, 0, 0, 0) == 0;
  if (!ok) { LogError("Luau script"); lua_pop(state_, 1); }
  return ok;
}

void ScriptRuntime::Fire(std::string_view event, int integer_arg, std::string_view string_arg) {
  if (!state_) return;
  const std::string key(event);
  const auto found = hooks_.find(key);
  if (found == hooks_.end()) return;
  // Callbacks may register more callbacks. Dispatch a stable copy so adding a hook
  // cannot invalidate the vector currently being traversed; new hooks begin next fire.
  const auto references = found->second;
  instruction_budget_ = kInstructionBudget;
  for (const int reference : references) {
    if (instruction_budget_ <= 1000) break;
    if (reference == LUA_NOREF) continue;
    lua_getref(state_, reference);
    lua_pushinteger(state_, integer_arg);
    int nargs = 1;
    if (!string_arg.empty()) { lua_pushlstring(state_, string_arg.data(), string_arg.size()); ++nargs; }
    if (lua_pcall(state_, nargs, 0, 0) != 0) {
      LogError(std::string("Luau hook ") + key);
      lua_pop(state_, 1);
      lua_unref(state_, reference);
      if (auto active = hooks_.find(key); active != hooks_.end())
        std::erase(active->second, reference);
    }
  }
}

void ScriptRuntime::Tick(uint32_t tick) { Fire("tick", static_cast<int>(tick)); }
void ScriptRuntime::PlayerJoined(uint16_t id, std::string_view name) { Fire("player_join", id, name); }
void ScriptRuntime::PlayerLeft(uint16_t id, std::string_view name) { Fire("player_leave", id, name); }

void ScriptRuntime::LogError(std::string_view prefix) {
  if (!log_) return;
  const char* error = lua_tostring(state_, -1);
  log_(std::string(prefix) + ": " + (error ? error : "unknown Luau error"));
}

}  // namespace edf::net
