#pragma once

#include "protocol.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct lua_State;

namespace edf::net {

class ScriptRuntime {
 public:
  using Log = std::function<void(std::string)>;
  using Broadcast = std::function<void(std::string)>;
  using RebasePlayer = std::function<bool(uint16_t, int32_t, int32_t, int32_t, int16_t)>;
  using ListPlayers = std::function<std::vector<ScriptPlayerView>()>;
  using ListActors = std::function<std::vector<ActorState>()>;
  using SpawnActor = std::function<std::optional<uint32_t>(std::string_view, int32_t, int32_t, int32_t, int16_t)>;
  using DamageActor = std::function<bool(uint32_t, float)>;
  using SetActorTransform = std::function<bool(uint32_t, int32_t, int32_t, int32_t, int16_t)>;
  using DamagePlayer = std::function<bool(uint16_t, uint16_t)>;

  explicit ScriptRuntime(Log log = {}, Broadcast broadcast = {}, RebasePlayer rebase_player = {},
                         ListPlayers list_players = {}, SpawnActor spawn_actor = {},
                         DamageActor damage_actor = {}, ListActors list_actors = {},
                         SetActorTransform set_actor_transform = {},
                         DamagePlayer damage_player = {});
  ~ScriptRuntime();
  ScriptRuntime(const ScriptRuntime&) = delete;
  ScriptRuntime& operator=(const ScriptRuntime&) = delete;

  bool LoadFile(const std::filesystem::path& path);
  bool RunString(std::string_view source, std::string_view chunk_name = "@server");
  void Reset();
  void Tick(uint32_t tick);
  void PlayerJoined(uint16_t id, std::string_view name);
  void PlayerLeft(uint16_t id, std::string_view name);
  bool available() const { return state_ != nullptr; }

 private:
  static void Interrupt(lua_State* state, int gc);
  static int RegisterHook(lua_State* state);
  static int BroadcastMessage(lua_State* state);
  static int RebasePlayerPosition(lua_State* state);
  static int ReadPlayers(lua_State* state);
  static int ReadActors(lua_State* state);
  static int SpawnServerActor(lua_State* state);
  static int DamageServerActor(lua_State* state);
  static int DamageServerPlayer(lua_State* state);
  static int SetServerActorTransform(lua_State* state);
  static void* Allocate(void* ud, void* ptr, size_t old_size, size_t new_size);
  void Initialize();
  bool Run(std::string_view source, std::string_view chunk_name);
  void Fire(std::string_view event, int integer_arg, std::string_view string_arg = {});
  void LogError(std::string_view prefix);

  lua_State* state_ = nullptr;
  Log log_;
  Broadcast broadcast_;
  RebasePlayer rebase_player_;
  ListPlayers list_players_;
  ListActors list_actors_;
  SpawnActor spawn_actor_;
  DamageActor damage_actor_;
  SetActorTransform set_actor_transform_;
  DamagePlayer damage_player_;
  size_t allocated_bytes_ = 0;
  uint32_t instruction_budget_ = 0;
  std::unordered_map<std::string, std::vector<int>> hooks_;
};

}  // namespace edf::net
