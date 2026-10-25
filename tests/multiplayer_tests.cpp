#include "client_session.h"
#include "discovery.h"
#include "guest_seat_mailbox.h"
#include "local_guest_input.h"
#include "protocol.h"
#include "script_runtime.h"
#include "secure_channel.h"
#include "server_runtime.h"
#include "server_world.h"

#include <chrono>
#include <algorithm>
#include <iostream>
#include <limits>
#include <mutex>
#include <atomic>
#include <filesystem>
#include <thread>
#include <stdexcept>
#include <string>

namespace {
void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

template <typename Fn>
void RunStage(const char* name, Fn&& fn) {
  std::cout << "[ RUN      ] " << name << '\n' << std::flush;
  fn();
  std::cout << "[       OK ] " << name << '\n' << std::flush;
}

void SecureChannelRoundTrip() {
  edf::net::CryptoKeyPair client_key_pair;
  edf::net::CryptoKeyPair server_key_pair;
  Require(edf::net::GenerateCryptoKeyPair(client_key_pair) &&
              edf::net::GenerateCryptoKeyPair(server_key_pair),
          "libsodium could not generate ephemeral X25519 keys");
  edf::net::CryptoSessionKeys client_keys;
  edf::net::CryptoSessionKeys server_keys;
  client_keys.session_id = server_keys.session_id = 0x97f4a31c5062ull;
  Require(edf::net::DeriveClientSessionKeys(client_key_pair, server_key_pair.public_key, client_keys) &&
              edf::net::DeriveServerSessionKeys(server_key_pair, client_key_pair.public_key, server_keys),
          "X25519 client/server session key derivation failed");
  Require(client_keys.transmit == server_keys.receive && client_keys.receive == server_keys.transmit &&
              client_keys.transmit != client_keys.receive,
          "client/server transport keys were not directionally separated");
  edf::net::CryptoNonce client_nonce;
  edf::net::CryptoNonce server_nonce;
  edf::net::CryptoNoncePrefix nonce_prefix;
  Require(edf::net::GenerateCryptoNonce(client_nonce) && edf::net::GenerateCryptoNonce(server_nonce) &&
              edf::net::DeriveNoncePrefix(client_nonce, server_nonce, client_keys.session_id, nonce_prefix),
          "session nonce prefix derivation failed");
  const auto fingerprint = edf::net::CryptoFingerprint(server_key_pair.public_key);
  Require(fingerprint.size() == 64, "server public-key fingerprint was not 256-bit hex");

  edf::net::SecureChannel client(client_keys, nonce_prefix);
  edf::net::SecureChannel server(server_keys, nonce_prefix);
  const std::vector<uint8_t> first{0x45, 0x44, 0x46, 0x37, 1, 2, 3};
  const auto packet0 = client.Seal(0, first);
  Require(packet0.size() == first.size() + edf::net::kSecureEnvelopeBytes,
          "encrypted packet envelope size changed");
  std::vector<uint8_t> plaintext;
  Require(server.Open(0, packet0, plaintext) && plaintext == first,
          "server could not authenticate and decrypt the client packet");
  Require(!server.Open(0, packet0, plaintext), "replayed encrypted packet was accepted");

  const auto packet1 = client.Seal(1, std::span<const uint8_t>(first));
  Require(!server.Open(0, packet1, plaintext), "packet encrypted for ENet channel 1 passed on channel 0");
  Require(server.Open(1, packet1, plaintext) && plaintext == first,
          "valid packet failed after a wrong-channel authentication attempt");

  auto tampered = client.Seal(0, std::span<const uint8_t>(first));
  tampered.back() ^= 0x80;
  Require(!server.Open(0, tampered, plaintext), "tampered ciphertext was accepted");
  const auto packet3 = client.Seal(0, std::span<const uint8_t>(first));
  const auto packet4 = client.Seal(0, std::span<const uint8_t>(first));
  Require(server.Open(0, packet4, plaintext) && server.Open(0, packet3, plaintext),
          "out-of-order ENet channel delivery was rejected inside the replay window");
  Require(!server.Seal(0, first).empty(), "server could not encrypt a reply with its directional key");

  std::vector<uint8_t> oversized(edf::net::kMaxSecurePayloadBytes + 1, 0x5a);
  Require(client.Seal(0, oversized).empty(), "secure channel exceeded the path-safe packet limit");
}

void ProtocolRoundTrip() {
  edf::net::InputCommand input{.sequence = 42, .buttons = 0x1a5, .move_x = -1234, .move_y = 5678,
                               .aim_x = 8192, .aim_y = -16384, .gameplay_active = true,
                               .transform_valid = true, .x_cm = -12345, .y_cm = 678, .z_cm = 9001};
  const auto bytes = edf::net::EncodeInput(input);
  edf::net::DecodedMessage decoded;
  Require(edf::net::Decode(bytes, decoded), "input packet did not decode");
  Require(decoded.type == edf::net::Message::kInput, "decoded wrong packet type");
  Require(decoded.input.sequence == input.sequence && decoded.input.buttons == input.buttons &&
           decoded.input.move_x == input.move_x && decoded.input.move_y == input.move_y &&
           decoded.input.aim_x == input.aim_x && decoded.input.aim_y == input.aim_y &&
           decoded.input.gameplay_active == input.gameplay_active && decoded.input.transform_valid &&
           decoded.input.x_cm == input.x_cm && decoded.input.y_cm == input.y_cm &&
           decoded.input.z_cm == input.z_cm,
           "input packet fields changed on the wire");

  edf::net::ServerInfo server_info;
  server_info.name = "server-owned movement";
  server_info.server_authoritative_movement = true;
  Require(edf::net::Decode(edf::net::EncodeServerInfo(server_info), decoded) &&
              decoded.type == edf::net::Message::kServerInfo &&
              decoded.server_info.server_authoritative_movement,
          "server-authoritative movement mode did not round trip in server discovery");
  Require(edf::net::Decode(edf::net::EncodeWelcome(9, 123, true), decoded) &&
              decoded.type == edf::net::Message::kWelcome && decoded.player_id == 9 &&
              decoded.tick == 123 && decoded.server_authoritative_movement,
          "server-authoritative movement mode did not round trip in the join handshake");
  Require(edf::net::Decode(edf::net::EncodePlayerLeft(9, 124), decoded) &&
              decoded.type == edf::net::Message::kPlayerLeft && decoded.player_id == 9 &&
              decoded.tick == 124,
          "player removal event did not round trip");

  edf::net::WorldSnapshot snapshot;
  snapshot.tick = 99;
  snapshot.owner_pose_authoritative = true;
  snapshot.players.push_back({.id = 7, .name = "edf", .x_cm = -100, .y_cm = 0, .z_cm = 250,
                              .yaw_mrad = 314, .buttons = 3});
  const auto snapshot_bytes = edf::net::EncodeSnapshot(snapshot);
  Require(edf::net::Decode(snapshot_bytes, decoded), "snapshot did not decode");
  Require(decoded.snapshot.tick == 99 && decoded.snapshot.owner_pose_authoritative &&
          decoded.snapshot.players.size() == 1 &&
          decoded.snapshot.players.front().x_cm == -100 && decoded.snapshot.players.front().name == "edf",
          "snapshot fields changed on the wire");

  edf::net::MissionActorIdentitySnapshot identity_snapshot;
  identity_snapshot.tick = 101;
  identity_snapshot.host_trace_generation = 7;
  identity_snapshot.actors = {{0x80000001u, 12, 0}, {0x80000002u, 13, 1}};
  const auto identity_bytes = edf::net::EncodeMissionActorIdentitySnapshot(identity_snapshot);
  Require(edf::net::Decode(identity_bytes, decoded) &&
              decoded.type == edf::net::Message::kMissionActorIdentitySnapshot &&
              decoded.mission_actor_identity_snapshot.tick == identity_snapshot.tick &&
              decoded.mission_actor_identity_snapshot.host_trace_generation == 7 &&
              decoded.mission_actor_identity_snapshot.actors.size() == 2 &&
              decoded.mission_actor_identity_snapshot.actors.back().ant_ordinal == 1,
          "mission actor identity diagnostics did not round trip");

  edf::net::MissionActorStateSnapshot mission_state;
  mission_state.tick = 105;
  mission_state.host_trace_generation = 7;
  edf::net::ActorState mission_ant{.id = 0x80000001u,
      .archetype = edf::net::ActorArchetype::kAnt,
      .x_cm = -1234, .y_cm = 567, .z_cm = 8901,
      .yaw_mrad = -314, .health = 48,
      .trace_identity_valid = true, .trace_generation = 7,
      .trace_call_ordinal = 4095, .trace_ant_ordinal = 63};
  mission_state.actors.push_back(mission_ant);
  const auto mission_state_bytes = edf::net::EncodeMissionActorStateSnapshot(mission_state);
  Require(edf::net::Decode(mission_state_bytes, decoded) &&
              decoded.type == edf::net::Message::kMissionActorStateSnapshot &&
              decoded.mission_actor_state_snapshot.tick == mission_state.tick &&
              decoded.mission_actor_state_snapshot.host_trace_generation == 7 &&
              decoded.mission_actor_state_snapshot.actors.size() == 1 &&
              decoded.mission_actor_state_snapshot.actors.front().x_cm == -1234 &&
              decoded.mission_actor_state_snapshot.actors.front().health == 48 &&
              decoded.mission_actor_state_snapshot.actors.front().trace_call_ordinal == 4095 &&
              decoded.mission_actor_state_snapshot.actors.front().trace_ant_ordinal == 63,
          "mission actor host state and packed spawn identity did not round trip");

  edf::net::MissionActorRemovalSnapshot removals;
  removals.tick = 106;
  removals.host_trace_generation = 7;
  removals.revision = 2;
  removals.removals = {{0x80000001u, 12, 0}, {0x80000002u, 13, 1}};
  const auto removal_bytes = edf::net::EncodeMissionActorRemovalSnapshot(removals);
  Require(edf::net::Decode(removal_bytes, decoded) &&
              decoded.type == edf::net::Message::kMissionActorRemovalSnapshot &&
              decoded.mission_actor_removal_snapshot.tick == removals.tick &&
              decoded.mission_actor_removal_snapshot.host_trace_generation == 7 &&
              decoded.mission_actor_removal_snapshot.revision == 2 &&
              decoded.mission_actor_removal_snapshot.removals.size() == 2 &&
              decoded.mission_actor_removal_snapshot.removals.back().ant_ordinal == 1,
          "reliable mission actor removals did not round trip");
  Require(edf::net::IsMissionActorRemovalSnapshotNewer(removals, {}),
          "initial mission actor removal snapshot was not considered newer");

  edf::net::MissionActorStateSnapshot current_mission_state;
  current_mission_state.tick = 105;
  auto newer_mission_state = mission_state;
  newer_mission_state.tick = 106;
  Require(edf::net::IsMissionActorStateSnapshotNewer(newer_mission_state, current_mission_state) &&
              !edf::net::IsMissionActorStateSnapshotNewer(current_mission_state, newer_mission_state),
          "stale mission actor state tick was accepted");
  current_mission_state.tick = std::numeric_limits<uint32_t>::max() - 2;
  newer_mission_state.tick = 2;
  Require(edf::net::IsMissionActorStateSnapshotNewer(newer_mission_state, current_mission_state),
          "mission actor state tick wrap was not handled");

  auto trailing = bytes;
  trailing.push_back(0);
  Require(!edf::net::Decode(trailing, decoded), "trailing packet bytes were accepted");
  Require(!edf::net::Decode(std::span<const uint8_t>(bytes.data(), bytes.size() - 1), decoded), "truncated packet was accepted");
  auto out_of_bounds = edf::net::EncodeInput(input);
  // Input position x begins after the 6-byte header and 25 bytes of controls/reset metadata.
  const int32_t invalid_x = 10'000'001;
  for (size_t i = 0; i < 4; ++i)
    out_of_bounds[31 + i] = static_cast<uint8_t>(static_cast<uint32_t>(invalid_x) >> (i * 8));
  Require(!edf::net::Decode(out_of_bounds, decoded), "out-of-bounds client transform was accepted");
  input.x_cm = invalid_x;
  Require(edf::net::EncodeInput(input).empty(), "encoder accepted an out-of-bounds client transform");
  std::vector<uint8_t> oversized(edf::net::kMaxPacketBytes + 1);
  Require(!edf::net::Decode(oversized, decoded), "oversized packet was accepted");

  auto control = edf::net::EncodeHello("bad\nname");
  Require(edf::net::Decode(control, decoded) && decoded.text.find('\n') == std::string::npos,
          "wire strings did not sanitize terminal control characters");
}

void ProtocolBoundaryCases() {
  edf::net::DecodedMessage decoded;

  edf::net::WorldSnapshot full;
  full.tick = 123456;
  for (uint16_t i = 1; i <= edf::net::kMaxPlayers; ++i) {
    full.players.push_back({.id = i, .name = std::string(edf::net::kMaxPlayerNameBytes, 'p')});
  }
  const auto full_bytes = edf::net::EncodeSnapshot(full);
  Require(!full_bytes.empty() && full_bytes.size() <= edf::net::kMaxPacketBytes,
          "maximum player snapshot exceeded the wire packet limit");
  Require(edf::net::Decode(full_bytes, decoded) && decoded.snapshot.players.size() == edf::net::kMaxPlayers &&
              decoded.snapshot.players.back().name.size() == edf::net::kMaxPlayerNameBytes,
          "maximum player snapshot did not round trip");

  const std::string max_chat(edf::net::kMaxChatBytes, 'c');
  const auto chat_bytes = edf::net::EncodeChat(17, max_chat);
  Require(edf::net::Decode(chat_bytes, decoded) && decoded.player_id == 17 && decoded.text == max_chat,
          "maximum length chat message did not round trip");
  Require(edf::net::EncodeChat(17, max_chat + "x").empty(),
          "overlong chat message was silently truncated");
  Require(edf::net::EncodeHello(std::string(edf::net::kMaxPlayerNameBytes + 1, 'n')).empty(),
          "overlong player name was silently truncated");
  Require(edf::net::EncodeHello(std::string("\xc0\xaf", 2)).empty(),
          "invalid UTF-8 player name was encoded");

  edf::net::MissionActorIdentitySnapshot identities;
  identities.tick = 123456;
  identities.host_trace_generation = 99;
  for (uint32_t i = 0; i < edf::net::kMaxSnapshotActors; ++i)
    identities.actors.push_back({0x80000001u + i, i, i % 64});
  const auto identity_bytes = edf::net::EncodeMissionActorIdentitySnapshot(identities);
  Require(!identity_bytes.empty() && identity_bytes.size() <= edf::net::kMaxPacketBytes &&
              edf::net::Decode(identity_bytes, decoded) &&
              decoded.mission_actor_identity_snapshot.actors.size() == edf::net::kMaxSnapshotActors,
          "maximum identity diagnostic packet exceeded bounds or failed to decode");

  edf::net::MissionActorStateSnapshot mission_states;
  mission_states.tick = 123456;
  mission_states.host_trace_generation = 99;
  for (uint32_t i = 0; i < edf::net::kMaxSnapshotActors; ++i) {
    edf::net::ActorState actor{.id = 0x80000001u + i,
        .archetype = edf::net::ActorArchetype::kAnt,
        .x_cm = static_cast<int32_t>(i * 10), .health = 80,
        .trace_identity_valid = true, .trace_generation = 99,
        .trace_call_ordinal = i, .trace_ant_ordinal = i % 64};
    mission_states.actors.push_back(actor);
  }
  const auto mission_state_bytes = edf::net::EncodeMissionActorStateSnapshot(mission_states);
  Require(!mission_state_bytes.empty() && mission_state_bytes.size() <= edf::net::kMaxPacketBytes &&
              edf::net::Decode(mission_state_bytes, decoded) &&
              decoded.mission_actor_state_snapshot.actors.size() == edf::net::kMaxSnapshotActors,
          "maximum mission actor state snapshot exceeded bounds or failed to decode");
  mission_states.actors[1].trace_call_ordinal = mission_states.actors[0].trace_call_ordinal;
  mission_states.actors[1].trace_ant_ordinal = mission_states.actors[0].trace_ant_ordinal;
  Require(edf::net::EncodeMissionActorStateSnapshot(mission_states).empty(),
          "duplicate mission actor spawn key was encoded in the state snapshot");
  mission_states.actors[1].trace_call_ordinal = 1;
  mission_states.actors[1].trace_ant_ordinal = 1;
  mission_states.actors[1].id = mission_states.actors[0].id;
  Require(edf::net::EncodeMissionActorStateSnapshot(mission_states).empty(),
          "duplicate mission actor ID was encoded in the state snapshot");
  mission_states.actors[1].id = 0x80000002u;
  mission_states.actors[1].trace_generation = 1;
  Require(edf::net::EncodeMissionActorStateSnapshot(mission_states).empty(),
          "mixed host trace generations were encoded in one state snapshot");
  edf::net::MissionActorRemovalSnapshot maximum_removals;
  maximum_removals.tick = 123456;
  maximum_removals.host_trace_generation = 99;
  maximum_removals.revision = static_cast<uint32_t>(edf::net::kMaxMissionActorRemovals);
  for (uint32_t i = 0; i < edf::net::kMaxMissionActorRemovals; ++i)
    maximum_removals.removals.push_back({0x80000001u + i, i, i % 64});
  const auto maximum_removal_bytes = edf::net::EncodeMissionActorRemovalSnapshot(maximum_removals);
  Require(!maximum_removal_bytes.empty() && maximum_removal_bytes.size() <= edf::net::kMaxPacketBytes &&
              edf::net::Decode(maximum_removal_bytes, decoded) &&
              decoded.mission_actor_removal_snapshot.removals.size() == edf::net::kMaxMissionActorRemovals,
          "maximum mission actor removal snapshot exceeded bounds or failed to decode");
  maximum_removals.removals[1].call_ordinal = maximum_removals.removals[0].call_ordinal;
  maximum_removals.removals[1].ant_ordinal = maximum_removals.removals[0].ant_ordinal;
  Require(edf::net::EncodeMissionActorRemovalSnapshot(maximum_removals).empty(),
          "duplicate mission actor removal key was encoded");
  identities.actors[1].call_ordinal = identities.actors[0].call_ordinal;
  identities.actors[1].ant_ordinal = identities.actors[0].ant_ordinal;
  Require(edf::net::EncodeMissionActorIdentitySnapshot(identities).empty(),
          "duplicate mission actor spawn key was encoded");
  identities.actors[1].call_ordinal = 1;
  identities.actors[1].ant_ordinal = 1;
  identities.actors[1].id = identities.actors[0].id;
  Require(edf::net::EncodeMissionActorIdentitySnapshot(identities).empty(),
          "duplicate mission actor ID was encoded");
  identities.actors[1].id = 0x80000002u;
  identities.host_trace_generation = 0;
  Require(edf::net::EncodeMissionActorIdentitySnapshot(identities).empty(),
          "identity records without a host trace generation were encoded");
  identities.actors.clear();
  const auto empty_identities = edf::net::EncodeMissionActorIdentitySnapshot(identities);
  Require(!empty_identities.empty() && edf::net::Decode(empty_identities, decoded) &&
              decoded.mission_actor_identity_snapshot.host_trace_generation == 0 &&
              decoded.mission_actor_identity_snapshot.actors.empty(),
          "empty identity snapshot did not clear stale host candidates");

  edf::net::WorldSnapshot invalid;
  invalid.players.push_back({.id = 1, .name = "bounds", .x_cm = std::numeric_limits<int32_t>::min()});
  Require(!edf::net::Decode(edf::net::EncodeSnapshot(invalid), decoded),
          "minimum signed coordinate was accepted");
  invalid.players.front().x_cm = 0;
  invalid.players.front().z_cm = std::numeric_limits<int32_t>::max();
  Require(!edf::net::Decode(edf::net::EncodeSnapshot(invalid), decoded),
          "maximum signed coordinate was accepted");
}

void MissionActorRemovalPagesMerge() {
  using namespace edf::net;
  MissionActorRemovalSnapshot accumulated;
  MissionActorRemovalSnapshot first_page;
  first_page.tick = 10;
  first_page.host_trace_generation = 17;
  first_page.revision = 600;
  for (uint32_t i = 0; i < kMaxMissionActorRemovals; ++i)
    first_page.removals.push_back({0x80000001u + i, i / 64, i % 64});
  Require(MergeMissionActorRemovalPage(first_page, accumulated) &&
              accumulated.removals.size() == kMaxMissionActorRemovals &&
              accumulated.revision == 600,
          "first mission removal page did not initialize the cumulative view");
  Require(!MergeMissionActorRemovalPage(first_page, accumulated) &&
              accumulated.removals.size() == kMaxMissionActorRemovals,
          "duplicate mission removal page was not idempotent");

  MissionActorRemovalSnapshot third_page;
  third_page.tick = 12;
  third_page.host_trace_generation = 17;
  third_page.revision = 600;
  third_page.page_offset = 510;
  for (uint32_t i = 510; i < 600; ++i)
    third_page.removals.push_back({0x80000001u + i, i / 64, i % 64});
  Require(!MergeMissionActorRemovalPage(third_page, accumulated),
          "a gapped out-of-order removal page was merged");

  MissionActorRemovalSnapshot second_page;
  second_page.tick = 11;
  second_page.host_trace_generation = 17;
  second_page.revision = 600;
  second_page.page_offset = 255;
  for (uint32_t i = 255; i < 510; ++i)
    second_page.removals.push_back({0x80000001u + i, i / 64, i % 64});
  Require(MergeMissionActorRemovalPage(second_page, accumulated) &&
              accumulated.removals.size() == 510,
          "second mission removal page did not append contiguously");
  Require(MergeMissionActorRemovalPage(third_page, accumulated) &&
              accumulated.removals.size() == 600 &&
              accumulated.removals.back().id == 0x80000001u + 599,
          "third mission removal page did not complete the cumulative view");

  MissionActorRemovalSnapshot next_generation;
  next_generation.tick = 13;
  next_generation.host_trace_generation = 18;
  next_generation.revision = 1;
  next_generation.removals = {{0x80010000u, 123, 4}};
  Require(MergeMissionActorRemovalPage(next_generation, accumulated) &&
              accumulated.host_trace_generation == 18 && accumulated.revision == 1 &&
              accumulated.removals.size() == 1 && accumulated.removals.front().id == 0x80010000u,
          "new mission generation did not clear the previous removal history");
}

void AuthoritativeWorldValidation() {
  edf::net::ServerWorld world(1);
  const auto first = world.AddPlayer("player-one");
  Require(first.id != 0, "valid player was rejected");
  Require(world.AddPlayer("second").id == 0, "server exceeded its configured player limit");
  Require(world.SubmitInput(first.id, {.sequence = 1, .move_x = 32767, .gameplay_active = true}),
          "fresh input was rejected");
  world.Tick();
  auto snapshot = world.Snapshot();
  Require(snapshot.players.size() == 1 && snapshot.players.front().x_cm > 0, "server did not simulate the accepted input");
  Require(!world.SubmitInput(first.id, {.sequence = 1, .move_x = -32767},
                            std::chrono::steady_clock::now() + std::chrono::milliseconds(20)),
          "stale client input sequence was accepted");
  Require(world.RemovePlayer(first.id) && world.player_count() == 0, "player disconnect was not applied");
}

void ClientAuthoritativeTransform() {
  edf::net::ServerWorld world(2);
  const auto player = world.AddPlayer("client-pose");
  Require(player.id != 0, "client-transform player was rejected");
  auto received = std::chrono::steady_clock::now();
  edf::net::InputCommand input{.sequence = 1, .aim_yaw_mrad = 765, .gameplay_active = true,
                               .transform_valid = true, .x_cm = 1234, .y_cm = 567, .z_cm = -890};
  Require(world.SubmitInput(player.id, input, received), "valid client transform input was rejected");
  world.Tick();
  auto state = world.FindPlayerState(player.id);
  Require(state && state->active && state->x_cm == input.x_cm && state->y_cm == input.y_cm &&
              state->z_cm == input.z_cm && state->yaw_mrad == input.aim_yaw_mrad,
          "server did not publish the accepted client-authoritative pose");

  world.Tick();
  state = world.FindPlayerState(player.id);
  Require(state && state->x_cm == input.x_cm && state->y_cm == input.y_cm && state->z_cm == input.z_cm,
          "server planar fallback moved a client-authoritative player between pose updates");

  input.sequence = 2;
  input.x_cm = 10'000'001;
  Require(!world.SubmitInput(player.id, input, received + std::chrono::milliseconds(20)),
          "server accepted a client transform outside the protocol bounds");
  state = world.FindPlayerState(player.id);
  Require(state && state->x_cm == 1234 && state->y_cm == 567 && state->z_cm == -890,
          "rejected client transform changed server player state");
}

void ClientTransformSpeedCeiling() {
  // 1,000 cm/s plus the fixed 25 cm jitter allowance. This tiny configured
  // ceiling makes the server receive-time bound deterministic and easy to hit.
  edf::net::ServerWorld world(2, 0, 0, 0, false, true, 1'000);
  const auto player = world.AddPlayer("speed-bound");
  Require(player.id != 0, "speed-bound player was rejected");
  const auto received = std::chrono::steady_clock::now();
  edf::net::InputCommand input{.sequence = 1, .gameplay_active = true,
                               .transform_valid = true};
  Require(world.SubmitInput(player.id, input, received), "initial transform anchor was rejected");

  input.sequence = 2;
  input.x_cm = 150;  // 1.5 m in 100 ms exceeds 1 m + 25 cm allowance.
  Require(!world.SubmitInput(player.id, input, received + std::chrono::milliseconds(100)),
          "client transform above the configured speed ceiling was accepted");
  auto state = world.FindPlayerState(player.id);
  Require(state && state->x_cm == 0, "rejected speed outlier changed the replicated pose");

  input.sequence = 3;
  input.x_cm = 120;
  Require(world.SubmitInput(player.id, input, received + std::chrono::milliseconds(100)),
          "client transform within the configured speed ceiling was rejected");
  state = world.FindPlayerState(player.id);
  Require(state && state->active && state->x_cm == 120,
          "accepted transform within the speed ceiling was not replicated");

  input.sequence = 4;
  input.gameplay_active = false;
  input.buttons = 0x1000;
  input.left_trigger = 200;
  input.right_trigger = 255;
  input.move_x = 2000;
  input.move_y = -1750;
  input.aim_x = 1250;
  input.aim_y = -1500;
  input.aim_yaw_mrad = 2300;
  input.transform_valid = true;
  input.x_cm = 999999;
  input.y_cm = -888888;
  input.z_cm = 777777;
  Require(world.SubmitInput(player.id, input, received + std::chrono::milliseconds(108)),
          "inactive transition input was rejected");
  const auto sanitized_input = world.FindPlayerInput(player.id);
  const auto inactive_hint_state = world.FindPlayerState(player.id);
  Require(sanitized_input && !sanitized_input->gameplay_active && !sanitized_input->buttons &&
              !sanitized_input->left_trigger && !sanitized_input->right_trigger &&
              !sanitized_input->move_x && !sanitized_input->move_y &&
              !sanitized_input->aim_x && !sanitized_input->aim_y &&
              !sanitized_input->aim_yaw_mrad && !sanitized_input->transform_valid &&
              !sanitized_input->x_cm && !sanitized_input->y_cm && !sanitized_input->z_cm,
          "client activity hint did not neutralize controls and pose before forwarding");
  Require(inactive_hint_state && inactive_hint_state->active && inactive_hint_state->x_cm == 120,
          "client activity hint hid the player or moved it from the last accepted pose");
  const auto inactive_hint_snapshot = world.SnapshotFor(player.id, 50'000);
  Require(inactive_hint_snapshot.players.size() == 1 &&
              !inactive_hint_snapshot.players.front().buttons &&
              !inactive_hint_snapshot.players.front().left_trigger &&
              !inactive_hint_snapshot.players.front().right_trigger,
          "client activity hint left replicated attack controls active");
  input.sequence = 5;
  input.gameplay_active = true;
  input.transform_valid = true;
  input.x_cm = 1'000;
  input.y_cm = 0;
  input.z_cm = 0;
  Require(!world.SubmitInput(player.id, input, received + std::chrono::milliseconds(116)),
          "inactive-to-active transition bypassed the client speed ceiling");
  state = world.FindPlayerState(player.id);
  Require(state && state->active && state->x_cm == 120,
          "rejected inactive-to-active teleport changed the player pose");

  input.x_cm = 160;
  Require(world.SubmitInput(player.id, input, received + std::chrono::milliseconds(116)),
          "valid reactivation within the speed ceiling was rejected");
  state = world.FindPlayerState(player.id);
  Require(state && state->active && state->x_cm == 160,
          "valid reactivation did not restore the accepted player pose");
}

void ClientAimTurnRateCeiling() {
  constexpr uint32_t kAimRateMradPerSecond = 10'000;
  edf::net::ServerWorld world(1, 0, 0, 0, false, true, 6'000, false, 100,
                              kAimRateMradPerSecond);
  const auto player = world.AddPlayer("aim-rate-bound");
  Require(player.id != 0, "aim-rate-bound player was rejected");
  const auto received = std::chrono::steady_clock::now();
  edf::net::InputCommand input{.sequence = 1, .aim_yaw_mrad = 3'100,
                               .gameplay_active = true, .transform_valid = true};
  Require(world.SubmitInput(player.id, input, received), "initial arbitrary aim was rejected");
  auto state = world.FindPlayerState(player.id);
  Require(state && state->yaw_mrad == 3'100,
          "the first active input could not establish the player's starting aim");

  // Crossing +180/-180 is a short 42 mrad turn, not a nearly full revolution.
  input.sequence = 2;
  input.aim_yaw_mrad = -3'142;
  Require(world.SubmitInput(player.id, input, received + std::chrono::milliseconds(8)),
          "wrapped aim update was rejected");
  state = world.FindPlayerState(player.id);
  Require(state && state->yaw_mrad == -3'142,
          "server did not use the shortest wrapped angle across +/-180 degrees");

  // An 8 ms packet permits 80 mrad at this configured rate, so a snap to zero is clamped.
  input.sequence = 3;
  input.aim_yaw_mrad = 0;
  Require(world.SubmitInput(player.id, input, received + std::chrono::milliseconds(16)),
          "fast aim update was rejected instead of being bounded");
  state = world.FindPlayerState(player.id);
  auto accepted = world.FindPlayerInput(player.id);
  Require(state && state->yaw_mrad == -3'062 && accepted && accepted->aim_yaw_mrad == -3'062,
          "server accepted a client aim snap above the receive-time turn-rate ceiling");

  // 100 ms permits a full 1,000 mrad turn.
  input.sequence = 4;
  input.aim_yaw_mrad = -2'062;
  Require(world.SubmitInput(player.id, input, received + std::chrono::milliseconds(116)),
          "aim update within elapsed-time allowance was rejected");
  state = world.FindPlayerState(player.id);
  Require(state && state->yaw_mrad == -2'062,
          "server did not allow aim movement proportional to elapsed server time");

  input.sequence = 5;
  input.gameplay_active = false;
  input.aim_yaw_mrad = 3'142;
  Require(world.SubmitInput(player.id, input, received + std::chrono::milliseconds(124)),
          "inactive aim hint packet was rejected");
  state = world.FindPlayerState(player.id);
  Require(state && state->yaw_mrad == -2'062,
          "inactive aim hint changed the accepted player heading");
  world.Tick();
  state = world.FindPlayerState(player.id);
  const auto script_players_after_inactive = world.ScriptPlayers();
  Require(state && state->yaw_mrad == -2'062 && script_players_after_inactive.size() == 1 &&
              script_players_after_inactive.front().input.aim_yaw_mrad == -2'062 &&
              !script_players_after_inactive.front().input.gameplay_active,
          "inactive hint reset the replicated or script-visible aim after a server tick");

  input.sequence = 6;
  input.gameplay_active = true;
  input.aim_yaw_mrad = 0;
  Require(world.SubmitInput(player.id, input, received + std::chrono::milliseconds(132)),
          "active aim after an inactive hint was rejected");
  state = world.FindPlayerState(player.id);
  accepted = world.FindPlayerInput(player.id);
  Require(state && state->yaw_mrad == -1'982 && accepted && accepted->aim_yaw_mrad == -1'982,
          "inactive aim hint altered the remembered yaw or accumulated turn allowance");

  edf::net::ServerWorld timed_out_world(1, 0, 0, 0, false, true, 6'000, false, 100,
                                        kAimRateMradPerSecond);
  const auto timed_out_player = timed_out_world.AddPlayer("aim-timeout");
  Require(timed_out_player.id != 0, "aim-timeout player was rejected");
  const auto old_received = std::chrono::steady_clock::now() - std::chrono::seconds(2);
  edf::net::InputCommand old_input{.sequence = 1, .aim_yaw_mrad = 0,
                                   .gameplay_active = true, .transform_valid = true};
  Require(timed_out_world.SubmitInput(timed_out_player.id, old_input, old_received),
          "initial input for timeout test was rejected");
  timed_out_world.Tick();
  old_input.sequence = 2;
  old_input.aim_yaw_mrad = 2'000;
  Require(timed_out_world.SubmitInput(timed_out_player.id, old_input),
          "player could not reactivate after the normal input timeout");
  const auto timed_out_state = timed_out_world.FindPlayerState(timed_out_player.id);
  Require(timed_out_state && timed_out_state->active && timed_out_state->yaw_mrad == 1'000,
          "a long packet gap banked more than the configured 100 ms of aim allowance");
}

void ServerAuthoritativeMovement() {
  edf::net::ServerWorld world(2, 1250, 300, -800, false, false);
  const auto player = world.AddPlayer("server-pose");
  Require(player.id != 0, "server-authoritative player was rejected");
  auto state = world.FindPlayerState(player.id);
  Require(state && state->position_reset && state->position_reset_id == 1 &&
              state->x_cm == 1250 && state->y_cm == 300 && state->z_cm == -800,
          "server-authoritative join did not require the configured server spawn");

  const auto received = std::chrono::steady_clock::now();
  edf::net::InputCommand input{.sequence = 1, .move_y = 32767, .aim_yaw_mrad = 0,
                               .gameplay_active = true, .position_reset_ack = 1,
                               .transform_valid = true, .x_cm = 9000000, .y_cm = 9000000,
                               .z_cm = 9000000};
  Require(world.SubmitInput(player.id, input, received),
          "server-authoritative input command was rejected");
  world.Tick();
  state = world.FindPlayerState(player.id);
  Require(state && state->active && !state->position_reset && state->x_cm == 1250 &&
              state->y_cm == 300 && state->z_cm > -800 && state->z_cm < -700,
          "client-reported coordinates overrode server-simulated movement");
}

void ClientNativeRespawnDoesNotUseServerSpawn() {
  edf::net::ServerWorld world(1, 0, 0, 0, false, true);
  const auto player = world.AddPlayer("native-respawn");
  Require(player.id != 0, "native-respawn player was rejected");
  const auto received = std::chrono::steady_clock::now();
  edf::net::InputCommand input{.sequence = 1, .gameplay_active = true,
      .transform_valid = true, .x_cm = 1234, .y_cm = 250, .z_cm = -5678};
  Require(world.SubmitInput(player.id, input, received), "initial native pose was rejected");
  input.sequence = 2;
  input.gameplay_active = false;
  input.transform_valid = false;
  Require(world.SubmitInput(player.id, input, received + std::chrono::milliseconds(8)),
          "native death transition was rejected");
  input.sequence = 3;
  input.gameplay_active = true;
  input.request_spawn = true;
  input.transform_valid = true;
  Require(world.SubmitInput(player.id, input, received + std::chrono::seconds(3)),
          "native EDF respawn command was rejected");
  const auto state = world.FindPlayerState(player.id);
  Require(state && state->active && !state->position_reset && state->x_cm == 1234 &&
              state->y_cm == 250 && state->z_cm == -5678,
          "client-transform mode replaced EDF's native spawn with the server origin");
}

void HostSimulatedOwnerPoseIsObserverScoped() {
  edf::net::ServerWorld world(2, 0, 0, 0, false, true);
  const auto host = world.AddPlayer("host");
  const auto guest = world.AddPlayer("guest");
  Require(host.id && guest.id, "host/guest players were rejected");
  Require(world.ApplyAuthoritativeTransform(guest.id, 400, 250, -800, 900, true),
          "host-sampled guest transform was rejected");
  const auto owner_snapshot = world.SnapshotFor(guest.id, 50'000);
  const auto other_snapshot = world.SnapshotFor(host.id, 50'000);
  Require(owner_snapshot.owner_pose_authoritative && !other_snapshot.owner_pose_authoritative,
          "host-simulated pose authority was not scoped to the owning guest");
  const auto owner_bytes = edf::net::EncodeSnapshot(owner_snapshot);
  edf::net::DecodedMessage decoded;
  Require(edf::net::Decode(owner_bytes, decoded) && decoded.snapshot.owner_pose_authoritative,
          "host-simulated owner pose authority was lost in the server snapshot");
  Require(world.ReleaseAuthoritativeSimulation(guest.id), "guest simulation could not be released");
  Require(!world.SnapshotFor(guest.id, 50'000).owner_pose_authoritative,
          "released host simulation kept owner-pose reconciliation enabled");
}

void HostDeadVitalsRemainVisibleToInactiveOwner() {
  edf::net::ServerWorld world(1, 0, 0, 0, false, true);
  const auto guest = world.AddPlayer("dead-owner");
  Require(guest.id != 0, "dead-owner player was rejected");
  Require(world.ApplyAuthoritativeTransform(guest.id, 400, 250, -800, 900, true),
          "host-owned player pose was rejected");
  Require(world.ApplyAuthoritativeVitals(guest.id, true, true, 100, 100),
          "live host-owned player vitals were rejected");
  Require(world.ApplyAuthoritativeVitals(guest.id, true, false, 0, 100),
          "dead host-owned player vitals were rejected");
  Require(world.MarkAuthoritativeUnavailable(guest.id),
          "dead host-owned player could not become spatially inactive");

  const auto snapshot = world.SnapshotFor(guest.id, 50'000);
  Require(snapshot.owner_pose_authoritative && snapshot.players.size() == 1 &&
              !snapshot.players.front().active && snapshot.players.front().health_valid &&
              !snapshot.players.front().alive && snapshot.players.front().health == 0 &&
              snapshot.players.front().max_health == 100,
          "inactive owner snapshot discarded its trusted dead vitals");
  edf::net::DecodedMessage decoded;
  Require(edf::net::Decode(edf::net::EncodeSnapshot(snapshot), decoded) &&
              decoded.snapshot.owner_pose_authoritative && decoded.snapshot.players.size() == 1 &&
              decoded.snapshot.players.front().health_valid && !decoded.snapshot.players.front().alive &&
              decoded.snapshot.players.front().health == 0 &&
              decoded.snapshot.players.front().max_health == 100,
          "dead owner vitals did not survive snapshot encoding");
}

void GuestNetworkSeatIsolation() {
  edf::net::probe::NetworkSeatMailbox mailbox;
  mailbox.Configure();
  const std::array<bool, edf::net::probe::kNetworkGuestSeatCount> both_seats_valid{true, true};
  const std::array<bool, edf::net::probe::kNetworkGuestSeatCount> primary_only_valid{true, false};
  edf::net::InputCommand first_input{.sequence = 1, .buttons = 0x0010, .gameplay_active = true};
  edf::net::InputCommand second_input{.sequence = 1, .buttons = 0x1000, .gameplay_active = true};

  Require(mailbox.Publish(41, first_input, std::nullopt, both_seats_valid),
          "first network peer could not claim validated seat 0");
  Require(!mailbox.Publish(82, second_input, std::nullopt, primary_only_valid),
          "peer was admitted to seat 1 before its guest object was validated");
  auto view = mailbox.Read();
  Require(view.seats[0].player_id == 41 && view.seats[0].player_index == 0 &&
              !view.seats[1].player_id,
          "rejected seat 1 admission changed the existing seat mapping");
  Require(mailbox.Publish(82, second_input, std::nullopt, both_seats_valid),
          "second network peer could not claim the validated seat 1");
  view = mailbox.Read();
  Require(view.seats[0].player_id == 41 && view.seats[1].player_id == 82 &&
              view.seats[1].player_index == 1,
          "network peers were not mapped stably to distinct guest indices");

  first_input.sequence = 2;
  first_input.buttons = 0x0001;
  Require(mailbox.Publish(41, first_input, std::nullopt, primary_only_valid),
          "existing seat 0 peer lost its validated seat during an update");
  view = mailbox.Read();
  Require(view.seats[0].command && view.seats[0].command->buttons == 0x0001 &&
              view.seats[1].command && view.seats[1].command->buttons == 0x1000,
          "one peer's input overwrote the other guest seat command");

  const edf::net::probe::NetworkPositionReset first_reset{
      .player_id = 41, .reset_id = 7, .x_cm = 100, .y_cm = 200, .z_cm = 300, .yaw_mrad = 400};
  const edf::net::probe::NetworkPositionReset second_reset{
      .player_id = 82, .reset_id = 9, .x_cm = -100, .y_cm = 500, .z_cm = -300, .yaw_mrad = -400};
  Require(mailbox.PublishPositionReset(first_reset) && mailbox.PublishPositionReset(second_reset),
          "independent guest seat resets were not retained");
  Require(mailbox.WithPendingPositionReset(82, 9, [](auto& reset) { reset.guest_applied = true; }),
          "seat 1 reset could not be applied independently");
  mailbox.AcknowledgePositionReset(82, 9);
  view = mailbox.Read();
  Require(view.seats[0].position_reset && view.seats[0].position_reset->reset_id == 7 &&
              !view.seats[0].position_reset->guest_acknowledged &&
              view.seats[1].position_reset && view.seats[1].position_reset->reset_id == 9 &&
              view.seats[1].position_reset->guest_acknowledged &&
              view.seats[1].acknowledged_reset_player_id == 82,
          "seat 1 reset acknowledgement modified seat 0 reset state");
  Require(mailbox.WithPendingPositionReset(41, 7, [](auto& reset) { reset.guest_applied = true; }),
          "seat 0 reset was lost while seat 1 acknowledged its own reset");

  Require(mailbox.Publish(41, std::nullopt, std::nullopt, {}), "seat 0 disconnect was rejected");
  view = mailbox.Read();
  Require(!view.seats[0].player_id && view.seats[1].player_id == 82 &&
              view.seats[1].position_reset && view.seats[1].position_reset->reset_id == 9,
          "disconnecting peer released another peer's seat or reset");
  edf::net::InputCommand replacement_input{.sequence = 1, .buttons = 0x0002, .gameplay_active = true};
  Require(mailbox.Publish(99, replacement_input, std::nullopt, both_seats_valid),
          "a released guest seat could not be reused");
  view = mailbox.Read();
  Require(view.seats[0].player_id == 99 && view.seats[1].player_id == 82 &&
              view.seats[1].position_reset && view.seats[1].position_reset->reset_id == 9,
          "seat reuse displaced the still-connected peer or its reset state");
}

void LocalHostGuestInputRouting() {
  using Clock = edf::net::LocalHostGuestInputMailbox::Clock;
  edf::net::LocalHostGuestInputMailbox mailbox;
  const auto now = Clock::time_point{} + std::chrono::seconds(10);

  edf::net::InputCommand host_input{.buttons = 0x0010, .gameplay_active = true};
  mailbox.SetLocalPlayerId(100);
  mailbox.Publish(100, host_input, now);
  Require(!mailbox.HasRemoteGuestSeat() && !mailbox.Read(now),
          "F5 host input was incorrectly assigned to the remote guest seat");

  edf::net::InputCommand inactive_remote{.gameplay_active = false};
  mailbox.Publish(103, inactive_remote, now);
  Require(!mailbox.HasRemoteGuestSeat(),
          "a remote peer outside the mission claimed EDF's native second seat");

  edf::net::InputCommand remote_input{.buttons = 0x1000, .move_x = 2345,
                                      .gameplay_active = true};
  mailbox.Publish(101, remote_input, now);
  Require(mailbox.HasRemoteGuestSeat() && !mailbox.Read(now),
          "remote input became readable before EDF validated its second player seat");
  const auto remote_identity = mailbox.PendingRemoteGuestIdentity();
  Require(remote_identity.player_id == 101 && mailbox.SetRemoteGuestSeatReady(remote_identity, true),
          "EDF seat validation was not applied to the pending remote identity");
  const auto routed = mailbox.Read(now);
  Require(routed && routed->buttons == remote_input.buttons && routed->move_x == remote_input.move_x,
          "validated remote input did not reach the native second-seat mailbox");

  edf::net::InputCommand second_remote{.buttons = 0x0020, .gameplay_active = true};
  mailbox.Publish(102, second_remote, now);
  const auto still_first_remote = mailbox.Read(now);
  Require(still_first_remote && still_first_remote->buttons == remote_input.buttons,
          "a second remote peer stole the single native EDF guest seat");

  const auto stale = mailbox.Read(now + std::chrono::milliseconds(301));
  Require(stale && stale->buttons == 0 && !stale->gameplay_active,
          "stale remote input was not neutralized");
  mailbox.Publish(101, std::nullopt, now + std::chrono::milliseconds(302));
  Require(!mailbox.HasRemoteGuestSeat() && !mailbox.Read(now + std::chrono::milliseconds(302)),
          "remote disconnect left the native second-seat input active");

  mailbox.Publish(104, remote_input, now + std::chrono::milliseconds(303));
  const auto replacement_identity = mailbox.PendingRemoteGuestIdentity();
  Require(replacement_identity.player_id == 104 &&
              replacement_identity.connection_generation != remote_identity.connection_generation &&
              !mailbox.SetRemoteGuestSeatReady(remote_identity, true) &&
              mailbox.SetRemoteGuestSeatReady(replacement_identity, true),
          "a stale guest-seat identity was accepted after the remote peer reconnected");
  mailbox.Publish(104, std::nullopt, now + std::chrono::milliseconds(304));

  mailbox.Publish(100, host_input, now + std::chrono::milliseconds(305));
  Require(!mailbox.HasRemoteGuestSeat(),
          "local host input claimed the remote seat after remote disconnect");

  mailbox.Reset();
  mailbox.Publish(100, host_input, now);
  mailbox.SetLocalPlayerId(100);
  Require(!mailbox.HasRemoteGuestSeat(),
          "late host identity did not reclaim player 0 from the remote-seat mailbox");
}

void ConnectedPlayersVisibleBeforeGameplay() {
  edf::net::ServerWorld world(8);
  const auto first = world.AddPlayer("player-one");
  const auto second = world.AddPlayer("player-two");
  Require(first.id && second.id, "could not add players for join-visibility check");

  auto snapshot = world.SnapshotFor(first.id, 50'000);
  auto peer = std::find_if(snapshot.players.begin(), snapshot.players.end(), [&](const auto& player) {
    return player.id == second.id;
  });
  Require(peer != snapshot.players.end() && peer->name == "player-two" && !peer->active,
          "inactive connected peer was not visible as lobby presence");

  const auto received = std::chrono::steady_clock::now();
  Require(world.SubmitInput(first.id, {.sequence = 1, .gameplay_active = true}, received),
          "first active-player input was rejected");
  Require(world.SubmitInput(second.id, {.sequence = 1, .gameplay_active = true},
                            received + std::chrono::milliseconds(8)),
          "second active-player input was rejected");
  world.Tick();
  snapshot = world.SnapshotFor(first.id, 100);
  peer = std::find_if(snapshot.players.begin(), snapshot.players.end(), [&](const auto& player) {
    return player.id == second.id;
  });
  Require(peer != snapshot.players.end() && !peer->active && peer->x_cm == 0,
          "out-of-range peer presence leaked its transform or entered the gameplay set");

  snapshot = world.SnapshotFor(first.id, 50'000);
  peer = std::find_if(snapshot.players.begin(), snapshot.players.end(), [&](const auto& player) {
    return player.id == second.id;
  });
  Require(peer != snapshot.players.end() && peer->active,
          "active nearby peer was not visible for gameplay replication");

  Require(world.RemovePlayer(second.id), "second player disconnect was not removed");
  snapshot = world.SnapshotFor(first.id, 50'000);
  Require(std::none_of(snapshot.players.begin(), snapshot.players.end(), [&](const auto& player) {
            return player.id == second.id;
          }), "disconnected peer remained in the observer snapshot");
}

void HighPopulationSnapshotsRemainBoundedAndUnique() {
  constexpr uint16_t kPlayers = 256;
  edf::net::ServerWorld world(kPlayers);
  std::vector<uint16_t> player_ids;
  player_ids.reserve(kPlayers);
  for (uint16_t index = 0; index < kPlayers; ++index) {
    const auto joined = world.AddPlayer("load-player-" + std::to_string(index));
    Require(joined.id != 0, "high-population server rejected a configured player slot");
    player_ids.push_back(joined.id);
  }

  const auto received = std::chrono::steady_clock::now();
  for (uint16_t index = 0; index < kPlayers; ++index) {
    Require(world.SubmitInput(player_ids[index],
                {.sequence = 1, .gameplay_active = true}, received),
            "high-population server rejected a valid player input");
  }
  world.Tick();

  const auto snapshot = world.SnapshotFor(player_ids.front(), 50'000, 41);
  Require(snapshot.players.size() == edf::net::kMaxSnapshotPlayers,
          "high-population observer snapshot did not fill the bounded wire roster");
  for (size_t left = 0; left < snapshot.players.size(); ++left) {
    Require(std::none_of(snapshot.players.begin() + left + 1, snapshot.players.end(),
                         [&](const auto& candidate) { return candidate.id == snapshot.players[left].id; }),
            "high-population observer snapshot duplicated a player ID");
  }
  const size_t active_players = static_cast<size_t>(std::count_if(
      snapshot.players.begin(), snapshot.players.end(), [](const auto& player) { return player.active; }));
  Require(active_players == 28,
          "high-population observer snapshot did not reserve four slots for name-only presence");
}

void InputSequenceWraparound() {
  edf::net::ServerWorld world;
  const auto player = world.AddPlayer("sequence-test");
  Require(player.id != 0, "sequence test player was rejected");
  const auto received = std::chrono::steady_clock::now();
  Require(world.SubmitInput(player.id, {.sequence = std::numeric_limits<uint32_t>::max()}, received),
          "initial input sequence was rejected");
  Require(world.SubmitInput(player.id, {.sequence = 0}, received + std::chrono::milliseconds(8)),
          "input sequence did not advance across uint32 wraparound");
  Require(!world.SubmitInput(player.id, {.sequence = std::numeric_limits<uint32_t>::max()},
                             received + std::chrono::milliseconds(16)),
          "replayed pre-wrap input replaced newer server state");
  Require(!world.SubmitInput(player.id, {.sequence = uint32_t{1} << 31},
                             received + std::chrono::milliseconds(24)),
          "ambiguous half-range input sequence was accepted");
  Require(world.SubmitInput(player.id, {.sequence = 1}, received + std::chrono::milliseconds(32)),
          "valid input after an ambiguous sequence was rejected");
}

void LuauSandboxAndHooks() {
  std::string output;
  edf::net::ScriptRuntime runtime([&](std::string message) { output = std::move(message); },
                                  [&](std::string message) { output = std::move(message); });
  Require(runtime.available(), "Luau VM failed to initialize");
  Require(runtime.RunString("assert(os == nil and io == nil and debug == nil)\n"
                            "edf.on('tick', function(tick) edf.broadcast('tick ' .. tick) end)", "@test"),
          "safe Luau hook did not load");
  runtime.Tick(17);
  Require(output == "tick 17", "Luau tick hook did not run");
  Require(!runtime.RunString("while true do end", "@budget"), "infinite script exceeded its instruction budget");
}

void LuauServerActorTransform() {
  uint32_t rebase_count = 0;
  uint32_t spawn_count = 0;
  uint32_t damage_count = 0;
  uint32_t callback_count = 0;
  uint32_t accepted_count = 0;
  uint32_t observed_id = 0;
  int32_t observed_x = 0, observed_y = 0, observed_z = 0;
  int16_t observed_yaw = 0;
  edf::net::ScriptRuntime runtime({}, {},
      [&](uint16_t player_id, int32_t x_cm, int32_t y_cm, int32_t z_cm, int16_t yaw_mrad) {
        ++rebase_count;
        return player_id == 7 && x_cm == 1000 && y_cm == 2000 && z_cm == 3000 && yaw_mrad == 250;
      },
      {},
      [&](std::string_view archetype, int32_t x_cm, int32_t y_cm, int32_t z_cm, int16_t yaw_mrad)
          -> std::optional<uint32_t> {
        ++spawn_count;
        if (archetype == "ant" && x_cm == 1200 && y_cm == 0 && z_cm == -300 && yaw_mrad == 0)
          return 55;
        return std::nullopt;
      },
      [&](uint32_t id, float damage) {
        ++damage_count;
        return id == 41 && damage == 25.0f;
      }, {},
      [&](uint32_t id, int32_t x_cm, int32_t y_cm, int32_t z_cm, int16_t yaw_mrad) {
        ++callback_count;
        if (id != 41) return false;
        ++accepted_count;
        observed_id = id;
        observed_x = x_cm;
        observed_y = y_cm;
        observed_z = z_cm;
        observed_yaw = yaw_mrad;
        return true;
      });
  Require(runtime.available(), "Luau VM for server actor transforms failed to initialize");
  Require(runtime.RunString(
              "assert(edf.rebase_player(7, 1000, 2000, 3000, 250))\n"
              "assert(not pcall(function() edf.rebase_player(7, 0.5, 0, 0, 0) end))\n"
              "assert(edf.spawn_actor('ant', 1200, 0, -300, 0) == 55)\n"
              "assert(not pcall(function() edf.spawn_actor('ant', 0.5, 0, 0, 0) end))\n"
              "assert(edf.damage_actor(41, 25))\n"
              "assert(not pcall(function() edf.damage_actor(41.5, 25) end))\n"
              "assert(edf.set_actor_transform(41, 100, -200, 300, 900))\n"
              "assert(not edf.set_actor_transform(42, 0, 0, 0, 0))\n"
              "assert(not pcall(function() edf.set_actor_transform(41, 1000001, 0, 0, 0) end))\n"
              "assert(not pcall(function() edf.set_actor_transform(41, 0.5, 0, 0, 0) end))\n"
              "assert(not pcall(function() edf.set_actor_transform(0, 0, 0, 0, 0) end))\n"
              "assert(not pcall(function() edf.set_actor_transform(41, 0, 0, 0, 3143) end))",
              "@actor-transform"),
          "server Luau hooks rejected whole-number literals or accepted invalid bounds");
  Require(rebase_count == 1 && spawn_count == 1 && damage_count == 1 && callback_count == 2 &&
              accepted_count == 1 && observed_id == 41 && observed_x == 100 &&
              observed_y == -200 && observed_z == 300 && observed_yaw == 900,
          "invalid server Luau hook arguments reached their callbacks");
}

void ServerActorMovementRoundTrip(const std::string& endpoint) {
  edf::net::ClientSession first, second;
  std::string error;
  Require(first.Connect(endpoint, "actor-one", &error), error.c_str());
  Require(second.Connect(endpoint, "actor-two", &error), error.c_str());
  const auto connect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
  while ((!first.connected() || !second.connected()) && std::chrono::steady_clock::now() < connect_deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  Require(first.connected() && second.connected(), "actor test clients did not complete the server handshake");

  bool first_received_moving_ant = false;
  bool second_received_moving_ant = false;
  uint32_t observed_actor_id = 0;
  edf::net::ActorSnapshot latest_first, latest_second;
  const auto snapshot_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
  while (std::chrono::steady_clock::now() < snapshot_deadline) {
    first.SetInput({.gameplay_active = true, .position_reset_ack = 1,
                    .transform_valid = true, .x_cm = 0, .y_cm = 0, .z_cm = 0});
    second.SetInput({.gameplay_active = true, .position_reset_ack = 1,
                     .transform_valid = true, .x_cm = 500, .y_cm = 0, .z_cm = 0});
    const auto find_moving_ant = [](const edf::net::ClientView& view) -> const edf::net::ActorState* {
      const auto found = std::find_if(view.actor_snapshot.actors.begin(), view.actor_snapshot.actors.end(),
          [](const auto& actor) { return actor.archetype == edf::net::ActorArchetype::kAnt && actor.x_cm < 1200; });
      return found == view.actor_snapshot.actors.end() ? nullptr : &*found;
    };
    const auto first_view = first.View();
    const auto second_view = second.View();
    latest_first = first_view.actor_snapshot;
    latest_second = second_view.actor_snapshot;
    const auto first_ant = find_moving_ant(first_view);
    const auto second_ant = find_moving_ant(second_view);
    if (first_ant) {
      observed_actor_id = first_ant->id;
      first_received_moving_ant = true;
    }
    if (second_ant && (!observed_actor_id || second_ant->id == observed_actor_id))
      second_received_moving_ant = true;
    if (first_received_moving_ant && second_received_moving_ant) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!first_received_moving_ant || !second_received_moving_ant) {
    std::cerr << "actor snapshots: first tick=" << latest_first.tick << " count=" << latest_first.actors.size()
              << " second tick=" << latest_second.tick << " count=" << latest_second.actors.size();
    if (!latest_first.actors.empty())
      std::cerr << " first id=" << latest_first.actors.front().id << " x=" << latest_first.actors.front().x_cm;
    if (!latest_second.actors.empty())
      std::cerr << " second id=" << latest_second.actors.front().id << " x=" << latest_second.actors.front().x_cm;
    std::cerr << '\n';
  }
  Require(first_received_moving_ant && second_received_moving_ant,
          "script-moved server-owned ant was not replicated to both connected clients");

  bool first_saw_server_damage = false;
  bool second_saw_server_damage = false;
  const auto combat_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
  while (std::chrono::steady_clock::now() < combat_deadline) {
    first.SetInput({.right_trigger = 255, .aim_yaw_mrad = 1571, .gameplay_active = true,
                    .position_reset_ack = 1,
                    .transform_valid = true, .x_cm = 0, .y_cm = 0, .z_cm = 0});
    second.SetInput({.right_trigger = 255, .aim_yaw_mrad = 1571, .gameplay_active = true,
                     .position_reset_ack = 1,
                     .transform_valid = true, .x_cm = 500, .y_cm = 0, .z_cm = 0});
    const auto has_damaged_ant = [observed_actor_id](const edf::net::ClientView& view) {
      const auto found = std::find_if(view.actor_snapshot.actors.begin(), view.actor_snapshot.actors.end(),
          [observed_actor_id](const auto& actor) { return actor.id == observed_actor_id; });
      return found != view.actor_snapshot.actors.end() && found->health < 100;
    };
    first_saw_server_damage = has_damaged_ant(first.View());
    second_saw_server_damage = has_damaged_ant(second.View());
    if (first_saw_server_damage && second_saw_server_damage) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  first.Disconnect();
  second.Disconnect();
  Require(first_saw_server_damage && second_saw_server_damage,
          "server-selected Luau combat damage was not replicated to both connected clients");
}

void MissionActorRemovalReliableRoundTrip() {
  using namespace edf::net;
  ServerConfig config;
  config.name = "mission-removal-test";
  config.game_port = static_cast<uint16_t>(40'000 +
      (std::chrono::steady_clock::now().time_since_epoch().count() % 20'000));
  config.discovery_port = 0;
  config.max_players = 4;
  config.mission_actor_reconciliation = true;

  std::mutex removal_mutex;
  MissionActorRemovalSnapshot removal_state;
  removal_state.host_trace_generation = 17;
  removal_state.revision = 600;
  for (uint32_t i = 0; i < removal_state.revision; ++i)
    removal_state.removals.push_back({0x80000001u + i, i / 64, i % 64});
  ServerRuntime runtime(config, {}, {}, {}, {}, {}, {}, {}, {}, [&] {
    std::lock_guard lock(removal_mutex);
    return removal_state;
  });
  std::string error;
  Require(runtime.Start(&error), error.c_str());
  std::atomic<bool> stop{false};
  std::thread worker([&] { runtime.Run(stop); });
  const auto stop_server = [&] {
    stop.store(true, std::memory_order_relaxed);
    if (worker.joinable()) worker.join();
  };

  ClientSession first;
  ClientSession late_joiner;
  try {
    const std::string endpoint = "127.0.0.1:" + std::to_string(config.game_port);
    Require(first.Connect(endpoint, "removal-first", &error), error.c_str());
    const auto first_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < first_deadline) {
      const auto snapshot = first.View().mission_actor_removal_snapshot;
      if (first.connected() && snapshot.host_trace_generation == 17 && snapshot.revision == 600 &&
          snapshot.removals.size() == 600) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    auto first_snapshot = first.View().mission_actor_removal_snapshot;
    Require(first.connected() && first_snapshot.host_trace_generation == 17 &&
                first_snapshot.revision == 600 && first_snapshot.removals.size() == 600,
            "connected client did not receive all reliable host removal pages");

    {
      std::lock_guard lock(removal_mutex);
      removal_state.revision = 601;
      removal_state.removals.push_back({0x80000001u + 600, 600 / 64, 600 % 64});
    }
    const auto update_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < update_deadline) {
      first_snapshot = first.View().mission_actor_removal_snapshot;
      if (first_snapshot.revision == 601 && first_snapshot.removals.size() == 601) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    Require(first_snapshot.revision == 601 && first_snapshot.removals.size() == 601,
            "connected client did not receive a removal appended after full-page delivery");

    Require(late_joiner.Connect(endpoint, "removal-late", &error), error.c_str());
    const auto late_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < late_deadline) {
      const auto snapshot = late_joiner.View().mission_actor_removal_snapshot;
      if (late_joiner.connected() && snapshot.host_trace_generation == 17 &&
          snapshot.revision == 601 && snapshot.removals.size() == 601) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto late_snapshot = late_joiner.View().mission_actor_removal_snapshot;
    Require(late_joiner.connected() && late_snapshot.host_trace_generation == 17 &&
                late_snapshot.revision == 601 && late_snapshot.removals.size() == 601,
            "late joiner did not receive all cumulative host removal pages");

    {
      std::lock_guard lock(removal_mutex);
      removal_state.host_trace_generation = 18;
      removal_state.revision = 1;
      removal_state.removals = {{0x80010000u, 123, 4}};
    }
    const auto generation_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < generation_deadline) {
      const auto first_view = first.View().mission_actor_removal_snapshot;
      const auto late_view = late_joiner.View().mission_actor_removal_snapshot;
      if (first_view.host_trace_generation == 18 && first_view.removals.size() == 1 &&
          late_view.host_trace_generation == 18 && late_view.removals.size() == 1) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto first_generation = first.View().mission_actor_removal_snapshot;
    const auto late_generation = late_joiner.View().mission_actor_removal_snapshot;
    Require(first_generation.host_trace_generation == 18 && first_generation.revision == 1 &&
                first_generation.removals.size() == 1 &&
                late_generation.host_trace_generation == 18 && late_generation.revision == 1 &&
                late_generation.removals.size() == 1,
            "new generation did not replace old removals for existing and late-joining clients");
  } catch (...) {
    first.Disconnect();
    late_joiner.Disconnect();
    stop_server();
    throw;
  }
  first.Disconnect();
  late_joiner.Disconnect();
  stop_server();
}

void ClientPendingConnectCancellation() {
  using namespace edf::net;
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto test_root = std::filesystem::temp_directory_path() /
                         ("edf2027-client-cancel-" + std::to_string(nonce));
  ServerConfig config;
  config.name = "client-cancel-test";
  config.game_port = static_cast<uint16_t>(40'000 + (nonce % 20'000));
  config.discovery_port = 0;
  config.max_players = 4;
  config.identity_file = test_root / "identity.edfkey";
  config.scripts_directory = test_root / "scripts";

  ServerRuntime runtime(config);
  std::string error;
  Require(runtime.Start(&error), error.c_str());
  std::atomic<bool> run_server{false};
  std::atomic<bool> stop_server{false};
  std::thread server([&] {
    while (!run_server.load(std::memory_order_acquire) &&
           !stop_server.load(std::memory_order_acquire))
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!stop_server.load(std::memory_order_acquire)) runtime.Run(stop_server);
  });
  const auto stop = [&] {
    stop_server.store(true, std::memory_order_release);
    run_server.store(true, std::memory_order_release);
    if (server.joinable()) server.join();
    std::error_code ignored;
    std::filesystem::remove_all(test_root, ignored);
  };

  const std::string endpoint = "127.0.0.1:" + std::to_string(config.game_port);
  ClientSession pending;
  try {
    // The ENet listener is bound but not servicing events yet, so this attempt
    // is guaranteed to remain in the kConnecting state until canceled.
    Require(pending.Connect(endpoint, "cancel-pending", &error), error.c_str());
    Require(pending.View().state == ClientState::kConnecting,
            "client left pending state before the server began servicing ENet");
    Require(pending.CancelPendingConnect(),
            "pending client connection was not cancelable");
    Require(pending.View().state == ClientState::kDisconnected,
            "canceling a pending client connection did not clear its view");

    run_server.store(true, std::memory_order_release);
    const auto reconnect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    bool reconnect_started = false;
    while (std::chrono::steady_clock::now() < reconnect_deadline) {
      if (pending.Connect(endpoint, "cancel-reconnect", &error)) {
        reconnect_started = true;
        break;
      }
      Require(error.find("still shutting down") != std::string::npos,
              "client retry failed for a reason other than the prior worker stopping");
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    Require(reconnect_started, "client did not allow retry after the canceled worker stopped");
    const auto connected_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!pending.connected() && std::chrono::steady_clock::now() < connected_deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    Require(pending.connected(), "client did not connect after retrying a canceled attempt");
    Require(!pending.CancelPendingConnect(),
            "CancelPendingConnect accepted an already established session");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    Require(pending.connected(),
            "CancelPendingConnect disconnected an established session");
    pending.Disconnect();

    // Race the cancellation call against the network worker's WELCOME handler.
    // The atomic outcome must be either a canceled/disconnected attempt or a
    // connected session which remains alive; it may never report cancellation
    // and then publish kConnected afterward.
    for (int attempt = 0; attempt < 12; ++attempt) {
      ClientSession racing;
      Require(racing.Connect(endpoint, "cancel-race", &error), error.c_str());
      std::atomic<bool> cancelled{false};
      std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(attempt * 2));
        cancelled.store(racing.CancelPendingConnect(), std::memory_order_release);
      });
      canceller.join();
      const bool did_cancel = cancelled.load(std::memory_order_acquire);
      const ClientState state = racing.View().state;
      Require(did_cancel ? state == ClientState::kDisconnected
                         : state == ClientState::kConnected,
              "connection cancellation raced WELCOME into an inconsistent state");
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      Require(did_cancel ? racing.View().state == ClientState::kDisconnected
                         : racing.connected(),
              "a canceled connection later became connected or a completed session was lost");
      racing.Disconnect();
    }
  } catch (...) {
    pending.Disconnect();
    stop();
    throw;
  }
  stop();
}

void NetworkRoundTrip(const std::string& endpoint, bool expect_server_authoritative = false) {
  edf::net::ClientSession first, second;
  std::string error;
  Require(first.Connect(endpoint, "network-one", &error), error.c_str());
  const auto connect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
  while (first.View().state == edf::net::ClientState::kConnecting && std::chrono::steady_clock::now() < connect_deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  const std::string first_status = first.View().status;
  Require(first.connected(), ("first ENet client did not complete the server handshake: " + first_status).c_str());
  Require(first.server_authoritative_movement() == expect_server_authoritative,
          "first client did not receive the server movement-authority mode");
  Require(second.Connect(endpoint, "network-two", &error), error.c_str());
  const auto second_connect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
  while (second.View().state == edf::net::ClientState::kConnecting &&
         std::chrono::steady_clock::now() < second_connect_deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  const std::string second_status = second.View().status;
  Require(second.connected(), ("second ENet client did not complete the server handshake: " + second_status).c_str());
  Require(second.server_authoritative_movement() == expect_server_authoritative,
          "second client did not receive the server movement-authority mode");
  const uint16_t first_id = first.View().local_player_id;
  const uint16_t second_id = second.View().local_player_id;
  const auto contains_player = [](const edf::net::ClientView& view, uint16_t id) {
    return std::any_of(view.snapshot.players.begin(), view.snapshot.players.end(),
                       [id](const auto& player) { return player.id == id; });
  };
  bool joined_visible = false;
  const auto join_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < join_deadline) {
    first.SetInput({.gameplay_active = expect_server_authoritative,
                    .position_reset_ack = expect_server_authoritative ? 1u : 0u});
    second.SetInput({.gameplay_active = expect_server_authoritative,
                     .position_reset_ack = expect_server_authoritative ? 1u : 0u});
    joined_visible = contains_player(first.View(), second_id) && contains_player(second.View(), first_id);
    if (joined_visible) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  bool first_moved = false, second_moved = false;
  bool first_sees_peer_active = false, second_sees_peer_active = false;
  const auto snapshot_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < snapshot_deadline) {
    first.SetInput({.move_x = 32767, .gameplay_active = true,
                    .position_reset_ack = expect_server_authoritative ? 1u : 0u,
                    .transform_valid = true, .x_cm = expect_server_authoritative ? 9000000 : 1200,
                    .y_cm = expect_server_authoritative ? 9000000 : 100,
                    .z_cm = expect_server_authoritative ? 9000000 : 300});
    second.SetInput({.move_y = 32767, .gameplay_active = true,
                     .position_reset_ack = expect_server_authoritative ? 1u : 0u,
                     .transform_valid = true, .x_cm = expect_server_authoritative ? -9000000 : -800,
                     .y_cm = expect_server_authoritative ? -9000000 : 200,
                     .z_cm = expect_server_authoritative ? -9000000 : 2400});
    const auto first_view = first.View();
    const auto second_view = second.View();
    for (const auto& player : first_view.snapshot.players) {
      if (player.id == first_id && (expect_server_authoritative
              ? player.active && player.x_cm > 0 && player.x_cm < 100000
              : player.x_cm == 1200 && player.y_cm == 100 && player.z_cm == 300))
        first_moved = true;
      if (player.id == second_id && player.active && (expect_server_authoritative
              ? player.z_cm > 0 && player.z_cm < 100000
              : player.x_cm == -800 && player.z_cm == 2400))
        first_sees_peer_active = true;
    }
    for (const auto& player : second_view.snapshot.players) {
      if (player.id == second_id && (expect_server_authoritative
              ? player.active && player.z_cm > 0 && player.z_cm < 100000
              : player.x_cm == -800 && player.y_cm == 200 && player.z_cm == 2400))
        second_moved = true;
      if (player.id == first_id && player.active && (expect_server_authoritative
              ? player.x_cm > 0 && player.x_cm < 100000
              : player.x_cm == 1200 && player.z_cm == 300))
        second_sees_peer_active = true;
    }
    if (first_moved && second_moved && first_sees_peer_active && second_sees_peer_active) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  Require(first.SendChat("hello from client"), "connected client did not queue chat");
  const std::string expected_chat = "Player " + std::to_string(first_id) + ": hello from client";
  const auto chat_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  bool chat_received = false;
  while (std::chrono::steady_clock::now() < chat_deadline) {
    const auto view = second.View();
    chat_received = std::find(view.chat.begin(), view.chat.end(), expected_chat) != view.chat.end();
    if (chat_received) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  second.Disconnect();
  bool disconnect_visible = false;
  const auto disconnect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < disconnect_deadline) {
    first.SetInput({.move_x = 32767, .gameplay_active = true,
                    .position_reset_ack = expect_server_authoritative ? 1u : 0u});
    disconnect_visible = !contains_player(first.View(), second_id);
    if (disconnect_visible) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  first.Disconnect();
  Require(joined_visible, "two connected clients did not see each other before gameplay became active");
  Require(first_moved && second_moved && first_sees_peer_active && second_sees_peer_active,
          expect_server_authoritative
              ? "server did not replicate two-client server-simulated movement"
              : "server did not return active two-client client-authoritative transform snapshots");
  Require(chat_received, "server did not broadcast client chat to the second player");
  Require(disconnect_visible, "remaining client did not observe peer removal");
}

void DirectoryAndNetworkRoundTrip(const std::string& game_endpoint, const std::string& directory_endpoint) {
  std::string error;
  const auto servers = edf::net::QueryMasterDirectory(directory_endpoint, std::chrono::seconds(3), &error);
  Require(error.empty(), error.c_str());
  Require(std::any_of(servers.begin(), servers.end(), [](const auto& server) {
            return server.info.name == "e2e-test" && server.address == "127.0.0.1";
          }),
          "self-hosted directory did not list the registered server");
  NetworkRoundTrip(game_endpoint);
}

void LanDiscoveryRoundTrip(const std::string& port_text) {
  const unsigned long parsed_port = std::stoul(port_text);
  Require(parsed_port > 0 && parsed_port <= 65535, "LAN discovery port is out of range");
  std::string error;
  const auto servers = edf::net::DiscoverServers(static_cast<uint16_t>(parsed_port),
                                                  std::chrono::milliseconds(1800), &error);
  Require(error.empty(), error.c_str());
  Require(std::any_of(servers.begin(), servers.end(), [](const auto& server) {
            return server.info.name == "lan-smoke";
          }),
          "native LAN discovery did not find the running dedicated server");
}
}

int main(int argc, char** argv) {
  try {
    if (argc == 3 && std::string_view(argv[1]) == "--actors") {
      ServerActorMovementRoundTrip(argv[2]);
      std::cout << "script-driven server ant movement and damage replicated to two ENet clients\n";
      return 0;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--network") {
      NetworkRoundTrip(argv[2]);
      std::cout << "two-client ENet join, movement, and removal round trip passed\n";
      return 0;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--server-authoritative-network") {
      NetworkRoundTrip(argv[2], true);
      std::cout << "two-client ENet server-authoritative movement round trip passed\n";
      return 0;
    }
    if (argc == 4 && std::string_view(argv[1]) == "--e2e") {
      DirectoryAndNetworkRoundTrip(argv[2], argv[3]);
      std::cout << "directory registration and two-client ENet round trip passed\n";
      return 0;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--lan-discovery") {
      LanDiscoveryRoundTrip(argv[2]);
      std::cout << "native LAN server discovery found the dedicated server\n";
      return 0;
    }
    RunStage("ProtocolRoundTrip", ProtocolRoundTrip);
    RunStage("SecureChannelRoundTrip", SecureChannelRoundTrip);
    RunStage("ProtocolBoundaryCases", ProtocolBoundaryCases);
    RunStage("MissionActorRemovalPagesMerge", MissionActorRemovalPagesMerge);
    RunStage("AuthoritativeWorldValidation", AuthoritativeWorldValidation);
    RunStage("ClientAuthoritativeTransform", ClientAuthoritativeTransform);
    RunStage("ClientTransformSpeedCeiling", ClientTransformSpeedCeiling);
    RunStage("ClientAimTurnRateCeiling", ClientAimTurnRateCeiling);
    RunStage("ServerAuthoritativeMovement", ServerAuthoritativeMovement);
    RunStage("ClientNativeRespawnDoesNotUseServerSpawn", ClientNativeRespawnDoesNotUseServerSpawn);
    RunStage("HostSimulatedOwnerPoseIsObserverScoped", HostSimulatedOwnerPoseIsObserverScoped);
    RunStage("HostDeadVitalsRemainVisibleToInactiveOwner", HostDeadVitalsRemainVisibleToInactiveOwner);
    RunStage("GuestNetworkSeatIsolation", GuestNetworkSeatIsolation);
    RunStage("LocalHostGuestInputRouting", LocalHostGuestInputRouting);
    RunStage("ConnectedPlayersVisibleBeforeGameplay", ConnectedPlayersVisibleBeforeGameplay);
    RunStage("HighPopulationSnapshotsRemainBoundedAndUnique", HighPopulationSnapshotsRemainBoundedAndUnique);
    RunStage("InputSequenceWraparound", InputSequenceWraparound);
    RunStage("LuauSandboxAndHooks", LuauSandboxAndHooks);
    RunStage("LuauServerActorTransform", LuauServerActorTransform);
    RunStage("ClientPendingConnectCancellation", ClientPendingConnectCancellation);
    RunStage("MissionActorRemovalReliableRoundTrip", MissionActorRemovalReliableRoundTrip);
    std::cout << "multiplayer protocol, simulation, and Luau checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "multiplayer tests failed: " << error.what() << '\n';
    return 1;
  }
}
