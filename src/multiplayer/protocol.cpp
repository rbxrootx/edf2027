#include "protocol.h"

#include <algorithm>
#include <cstring>

namespace edf::net {
namespace {
constexpr uint32_t kMagic = 0x37464445;  // "EDF7" in little endian.
constexpr uint8_t kWireVersion = 11;
constexpr int32_t kMaxClientCoordinateCm = 10'000'000;

bool IsValidUtf8(std::string_view text) {
  size_t i = 0;
  while (i < text.size()) {
    const auto lead = static_cast<uint8_t>(text[i]);
    uint32_t codepoint = 0;
    size_t length = 0;
    if (lead < 0x80) { codepoint = lead; length = 1; }
    else if (lead >= 0xc2 && lead <= 0xdf) { codepoint = lead & 0x1f; length = 2; }
    else if (lead >= 0xe0 && lead <= 0xef) { codepoint = lead & 0x0f; length = 3; }
    else if (lead >= 0xf0 && lead <= 0xf4) { codepoint = lead & 0x07; length = 4; }
    else return false;
    if (length > text.size() - i) return false;
    for (size_t j = 1; j < length; ++j) {
      const auto continuation = static_cast<uint8_t>(text[i + j]);
      if ((continuation & 0xc0) != 0x80) return false;
      codepoint = (codepoint << 6) | (continuation & 0x3f);
    }
    if ((length == 2 && codepoint < 0x80) || (length == 3 && codepoint < 0x800) ||
        (length == 4 && codepoint < 0x10000) || codepoint > 0x10ffff ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff)) return false;
    i += length;
  }
  return true;
}

bool IsLowerHex(std::string_view value, size_t expected_size) {
  return value.size() == expected_size &&
      std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      });
}

class Writer {
 public:
  explicit Writer(Message type) {
    U32(kMagic);
    U8(kWireVersion);
    U8(static_cast<uint8_t>(type));
  }
  void U8(uint8_t v) { bytes_.push_back(v); }
  void U16(uint16_t v) { U8(uint8_t(v)); U8(uint8_t(v >> 8)); }
  void I16(int16_t v) { U16(static_cast<uint16_t>(v)); }
  void U32(uint32_t v) { U16(uint16_t(v)); U16(uint16_t(v >> 16)); }
  void I32(int32_t v) { U32(static_cast<uint32_t>(v)); }
  void U64(uint64_t v) { U32(uint32_t(v)); U32(uint32_t(v >> 32)); }
  void Bytes(std::span<const uint8_t> bytes) { bytes_.insert(bytes_.end(), bytes.begin(), bytes.end()); }
  void String(std::string_view s, size_t max) {
    if (s.size() > max || s.size() > 255 || !IsValidUtf8(s)) {
      valid_ = false;
      return;
    }
    const size_t n = s.size();
    U8(static_cast<uint8_t>(n));
    for (size_t i = 0; i < n; ++i) {
      const auto c = static_cast<unsigned char>(s[i]);
      bytes_.push_back(c < 0x20 || c == 0x7f ? uint8_t(' ') : static_cast<uint8_t>(c));
    }
  }
  std::vector<uint8_t> Finish() && {
    if (!valid_ || bytes_.size() > kMaxPacketBytes) return {};
    return std::move(bytes_);
  }
 private:
  std::vector<uint8_t> bytes_;
  bool valid_ = true;
};

class Reader {
 public:
  explicit Reader(std::span<const uint8_t> bytes) : bytes_(bytes) {}
  bool U8(uint8_t& v) { if (!Need(1)) return false; v = bytes_[at_++]; return true; }
  bool U16(uint16_t& v) { uint8_t a, b; if (!U8(a) || !U8(b)) return false; v = uint16_t(a) | (uint16_t(b) << 8); return true; }
  bool I16(int16_t& v) { uint16_t x; if (!U16(x)) return false; v = static_cast<int16_t>(x); return true; }
  bool U32(uint32_t& v) { uint16_t a, b; if (!U16(a) || !U16(b)) return false; v = uint32_t(a) | (uint32_t(b) << 16); return true; }
  bool I32(int32_t& v) { uint32_t x; if (!U32(x)) return false; v = static_cast<int32_t>(x); return true; }
  bool U64(uint64_t& v) { uint32_t a, b; if (!U32(a) || !U32(b)) return false; v = uint64_t(a) | (uint64_t(b) << 32); return true; }
  bool Bytes(std::span<uint8_t> output) {
    if (!Need(output.size())) return false;
    std::copy_n(bytes_.data() + at_, output.size(), output.data());
    at_ += output.size();
    return true;
  }
  bool String(std::string& s, size_t max) {
    uint8_t n;
    if (!U8(n) || n > max || !Need(n)) return false;
    s.assign(reinterpret_cast<const char*>(bytes_.data() + at_), n);
    at_ += n;
    return IsValidUtf8(s) &&
        std::none_of(s.begin(), s.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
  }
  bool Done() const { return at_ == bytes_.size(); }
 private:
  bool Need(size_t n) const { return n <= bytes_.size() - at_; }
  std::span<const uint8_t> bytes_;
  size_t at_ = 0;
};

std::vector<uint8_t> Empty(Message type) { return Writer(type).Finish(); }
void WriteServerInfo(Writer& w, const ServerInfo& i) {
  w.String(i.name, kMaxServerNameBytes); w.String(i.mission, kMaxMissionNameBytes);
  w.U16(i.game_port); w.U16(i.players); w.U16(i.max_players); w.U16(i.protocol);
  w.String(i.build_tag, kServerBuildTagBytes);
  w.U8(i.server_authoritative_movement ? 1 : 0);
}
bool ReadServerInfo(Reader& r, ServerInfo& i) {
  uint8_t server_authoritative_movement = 0;
  if (!r.String(i.name, kMaxServerNameBytes) || !r.String(i.mission, kMaxMissionNameBytes) ||
      !r.U16(i.game_port) || !r.U16(i.players) || !r.U16(i.max_players) || !r.U16(i.protocol) ||
      !r.String(i.build_tag, kServerBuildTagBytes) || !r.U8(server_authoritative_movement) ||
      server_authoritative_movement > 1 || !IsLowerHex(i.build_tag, kServerBuildTagBytes) ||
      i.name.empty() || i.game_port == 0 || i.max_players == 0 ||
      i.max_players > kMaxServerPlayers || i.players > i.max_players) return false;
  i.server_authoritative_movement = server_authoritative_movement != 0;
  return true;
}
bool Fail(std::string* error, const char* text) {
  if (error) *error = text;
  return false;
}
}  // namespace

std::vector<uint8_t> EncodeDiscoveryRequest() { return Empty(Message::kDiscoveryRequest); }
std::vector<uint8_t> EncodeServerInfo(const ServerInfo& i) {
  Writer w(Message::kServerInfo); WriteServerInfo(w, i); return std::move(w).Finish();
}
std::vector<uint8_t> EncodeHello(std::string_view name) {
  return EncodeHello(name, {}, {});
}
std::vector<uint8_t> EncodeHello(std::string_view name,
                                const std::array<uint8_t, 32>& public_key,
                                const std::array<uint8_t, 32>& client_nonce) {
  Writer w(Message::kHello);
  w.U16(kProtocolVersion);
  w.String(kPortBuildFingerprint, kBuildFingerprintBytes);
  w.String(name, kMaxPlayerNameBytes);
  w.Bytes(public_key);
  w.Bytes(client_nonce);
  return std::move(w).Finish();
}
std::vector<uint8_t> EncodeChallenge(uint64_t nonce, uint64_t session_id,
                                    const std::array<uint8_t, 32>& client_nonce,
                                    const std::array<uint8_t, 32>& server_public_key,
                                    const std::array<uint8_t, 32>& server_nonce,
                                    const std::array<uint8_t, 32>& server_identity_key,
                                    const std::array<uint8_t, 64>& signature) {
  Writer w(Message::kChallenge);
  w.U64(nonce); w.U64(session_id);
  w.Bytes(client_nonce); w.Bytes(server_public_key); w.Bytes(server_nonce);
  w.Bytes(server_identity_key); w.Bytes(signature);
  return std::move(w).Finish();
}
std::vector<uint8_t> EncodeAuthenticate(uint64_t nonce) { Writer w(Message::kAuthenticate); w.U64(nonce); return std::move(w).Finish(); }
std::vector<uint8_t> EncodeWelcome(uint16_t id, uint32_t tick, bool server_authoritative_movement) {
  Writer w(Message::kWelcome);
  w.U16(id);
  w.U32(tick);
  w.U8(server_authoritative_movement ? 1 : 0);
  return std::move(w).Finish();
}
std::vector<uint8_t> EncodeInput(const InputCommand& i) {
  if (i.transform_valid && (i.x_cm < -kMaxClientCoordinateCm || i.x_cm > kMaxClientCoordinateCm ||
      i.y_cm < -kMaxClientCoordinateCm || i.y_cm > kMaxClientCoordinateCm ||
      i.z_cm < -kMaxClientCoordinateCm || i.z_cm > kMaxClientCoordinateCm)) return {};
  Writer w(Message::kInput);
  w.U32(i.sequence); w.U16(i.buttons); w.U8(i.left_trigger); w.U8(i.right_trigger);
  w.I16(i.move_x); w.I16(i.move_y); w.I16(i.aim_x); w.I16(i.aim_y); w.I16(i.aim_yaw_mrad);
  w.U8(i.gameplay_active ? 1 : 0);
  w.U8(i.request_spawn ? 1 : 0);
  w.U32(i.position_reset_ack);
  w.U8(i.transform_valid ? 1 : 0);
  w.I32(i.x_cm); w.I32(i.y_cm); w.I32(i.z_cm);
  return std::move(w).Finish();
}
std::vector<uint8_t> EncodeSnapshot(const WorldSnapshot& s) {
  if (s.players.size() > kMaxSnapshotPlayers ||
      (s.owner_pose_authoritative && s.players.empty())) return {};
  uint32_t active_mask = 0;
  bool has_position_reset = false;
  for (size_t i = 0; i < s.players.size(); ++i) {
    const PlayerState& player = s.players[i];
    if ((player.health_valid && (!player.max_health || player.health > player.max_health ||
                                 player.alive != (player.health > 0))) ||
        (!player.health_valid && (player.alive || player.health || player.max_health)))
      return {};
    if (s.players[i].position_reset) {
      // Server snapshots place the observer first; only that player can receive a
      // local position reset. Keep reset metadata out of ordinary player records.
      if (i != 0 || s.players[i].active || !s.players[i].position_reset_id) return {};
      has_position_reset = true;
    }
    if (s.players[i].active) active_mask |= uint32_t{1} << i;
  }
  // The generation adds four bytes to a snapshot. Reserve enough room for the
  // protocol's maximum player-name length under the 1200-byte packet cap.
  if (has_position_reset && s.players.size() > kMaxSnapshotPlayers - 1) return {};
  Writer w(Message::kSnapshot);
  const uint8_t count = static_cast<uint8_t>(s.players.size());
  const uint8_t flags = static_cast<uint8_t>((has_position_reset ? 0x80 : 0) |
                                              (s.owner_pose_authoritative ? 0x40 : 0));
  w.U32(s.tick); w.U8(static_cast<uint8_t>(count | flags)); w.U32(active_mask);
  if (has_position_reset) w.U32(s.players.front().position_reset_id);
  for (const auto& p : s.players) {
    w.U16(p.id); w.String(p.name, kMaxPlayerNameBytes);
    w.I32(p.x_cm); w.I32(p.y_cm); w.I32(p.z_cm);
    w.I16(p.yaw_mrad);
    const uint8_t vitals_flags = static_cast<uint8_t>((p.health_valid ? 0x01 : 0) |
                                                       (p.alive ? 0x02 : 0));
    w.U8(vitals_flags); w.U16(p.health); w.U16(p.max_health);
    w.U16(p.buttons); w.U8(p.left_trigger); w.U8(p.right_trigger);
  }
  return std::move(w).Finish();
}
namespace {
std::vector<uint8_t> EncodeActorSnapshotMessage(const ActorSnapshot& s, Message type) {
  if (s.actors.size() > kMaxSnapshotActors) return {};
  Writer w(type);
  w.U32(s.tick);
  w.U8(static_cast<uint8_t>(s.actors.size()));
  for (size_t i = 0; i < s.actors.size(); ++i) {
    const ActorState& actor = s.actors[i];
    if (!actor.id || actor.archetype != ActorArchetype::kAnt ||
        actor.x_cm < -10'000'000 || actor.x_cm > 10'000'000 ||
        actor.y_cm < -10'000'000 || actor.y_cm > 10'000'000 ||
        actor.z_cm < -10'000'000 || actor.z_cm > 10'000'000 ||
        actor.yaw_mrad < -3142 || actor.yaw_mrad > 3142)
      return {};
    for (size_t j = 0; j < i; ++j)
      if (s.actors[j].id == actor.id) return {};
    w.U32(actor.id);
    w.U8(static_cast<uint8_t>(actor.archetype));
    w.I32(actor.x_cm); w.I32(actor.y_cm); w.I32(actor.z_cm);
    w.I16(actor.yaw_mrad); w.U16(actor.health);
  }
  return std::move(w).Finish();
}
}  // namespace
std::vector<uint8_t> EncodeActorSnapshot(const ActorSnapshot& s) {
  return EncodeActorSnapshotMessage(s, Message::kActorSnapshot);
}
std::vector<uint8_t> EncodeMissionActorDiagnosticSnapshot(const ActorSnapshot& s) {
  return EncodeActorSnapshotMessage(s, Message::kMissionActorDiagnosticSnapshot);
}
std::vector<uint8_t> EncodeMissionActorIdentitySnapshot(const MissionActorIdentitySnapshot& s) {
  if ((!s.host_trace_generation && !s.actors.empty()) ||
      s.actors.size() > kMaxSnapshotActors) return {};
  Writer w(Message::kMissionActorIdentitySnapshot);
  w.U32(s.tick);
  w.U64(s.host_trace_generation);
  w.U8(static_cast<uint8_t>(s.actors.size()));
  for (size_t i = 0; i < s.actors.size(); ++i) {
    const MissionActorIdentity& actor = s.actors[i];
    if ((actor.id & kMissionActorIdNamespace) == 0 ||
        actor.call_ordinal >= 4096 || actor.ant_ordinal >= 64) return {};
    for (size_t j = 0; j < i; ++j)
      if (s.actors[j].id == actor.id ||
          (s.actors[j].call_ordinal == actor.call_ordinal &&
           s.actors[j].ant_ordinal == actor.ant_ordinal)) return {};
    w.U32(actor.id);
    w.U32(actor.call_ordinal);
    w.U32(actor.ant_ordinal);
  }
  return std::move(w).Finish();
}
std::vector<uint8_t> EncodeMissionActorStateSnapshot(const MissionActorStateSnapshot& s) {
  if ((!s.host_trace_generation && !s.actors.empty()) ||
      s.actors.size() > kMaxSnapshotActors) return {};
  Writer w(Message::kMissionActorStateSnapshot);
  w.U32(s.tick);
  w.U64(s.host_trace_generation);
  w.U8(static_cast<uint8_t>(s.actors.size()));
  for (size_t i = 0; i < s.actors.size(); ++i) {
    const ActorState& actor = s.actors[i];
    if ((actor.id & kMissionActorIdNamespace) == 0 ||
        actor.archetype != ActorArchetype::kAnt || !actor.trace_identity_valid ||
        actor.trace_generation != s.host_trace_generation ||
        actor.trace_call_ordinal >= 4096 || actor.trace_ant_ordinal >= 64 ||
        actor.x_cm < -10'000'000 || actor.x_cm > 10'000'000 ||
        actor.y_cm < -10'000'000 || actor.y_cm > 10'000'000 ||
        actor.z_cm < -10'000'000 || actor.z_cm > 10'000'000 ||
        actor.yaw_mrad < -3142 || actor.yaw_mrad > 3142)
      return {};
    const uint32_t spawn_key = (actor.trace_call_ordinal << 6) | actor.trace_ant_ordinal;
    for (size_t j = 0; j < i; ++j) {
      const ActorState& previous = s.actors[j];
      if (previous.id == actor.id ||
          (previous.trace_call_ordinal == actor.trace_call_ordinal &&
           previous.trace_ant_ordinal == actor.trace_ant_ordinal)) return {};
    }
    w.U32(actor.id);
    w.U8(static_cast<uint8_t>(spawn_key));
    w.U8(static_cast<uint8_t>(spawn_key >> 8));
    w.U8(static_cast<uint8_t>(spawn_key >> 16));
    w.U8(static_cast<uint8_t>(actor.archetype));
    w.I32(actor.x_cm); w.I32(actor.y_cm); w.I32(actor.z_cm);
    w.I16(actor.yaw_mrad); w.U16(actor.health);
  }
  return std::move(w).Finish();
}
bool IsMissionActorStateSnapshotNewer(const MissionActorStateSnapshot& incoming,
                                     const MissionActorStateSnapshot& current) {
  if (!incoming.tick) return false;
  return !current.tick || static_cast<int32_t>(incoming.tick - current.tick) > 0;
}
std::vector<uint8_t> EncodeMissionActorRemovalSnapshot(const MissionActorRemovalSnapshot& s) {
  if (s.revision > kMaxMissionActorRemovalHistory ||
      s.page_offset > s.revision || s.removals.size() > kMaxMissionActorRemovals ||
      uint64_t(s.page_offset) + s.removals.size() > s.revision ||
      (!s.host_trace_generation && (s.revision || s.page_offset || !s.removals.empty()))) return {};
  Writer w(Message::kMissionActorRemovalSnapshot);
  w.U32(s.tick);
  w.U64(s.host_trace_generation);
  w.U32(s.revision);
  w.U32(s.page_offset);
  w.U8(static_cast<uint8_t>(s.removals.size()));
  for (size_t i = 0; i < s.removals.size(); ++i) {
    const MissionActorRemoval& removal = s.removals[i];
    if ((removal.id & kMissionActorIdNamespace) == 0 ||
        removal.call_ordinal >= 4096 || removal.ant_ordinal >= 64 ||
        !s.host_trace_generation) return {};
    for (size_t j = 0; j < i; ++j)
      if (s.removals[j].id == removal.id ||
          (s.removals[j].call_ordinal == removal.call_ordinal &&
           s.removals[j].ant_ordinal == removal.ant_ordinal)) return {};
    w.U32(removal.id);
    w.U32(removal.call_ordinal);
    w.U32(removal.ant_ordinal);
  }
  return std::move(w).Finish();
}
bool IsMissionActorRemovalSnapshotNewer(const MissionActorRemovalSnapshot& incoming,
                                       const MissionActorRemovalSnapshot& current) {
  if (!incoming.tick) return false;
  if (!current.tick) return true;
  if (incoming.host_trace_generation != current.host_trace_generation)
    return incoming.page_offset == 0;
  return incoming.revision > current.revision ||
      uint64_t(incoming.page_offset) + incoming.removals.size() > current.removals.size();
}
bool MergeMissionActorRemovalPage(const MissionActorRemovalSnapshot& page,
                                  MissionActorRemovalSnapshot& accumulated) {
  if (!page.tick || page.revision > kMaxMissionActorRemovalHistory ||
      page.page_offset > page.revision || page.removals.size() > kMaxMissionActorRemovals ||
      uint64_t(page.page_offset) + page.removals.size() > page.revision ||
      (!page.host_trace_generation && (page.revision || page.page_offset || !page.removals.empty())))
    return false;
  if (!accumulated.tick || page.host_trace_generation != accumulated.host_trace_generation) {
    if (page.page_offset != 0) return false;
    accumulated = page;
    accumulated.page_offset = 0;
    return true;
  }
  if (page.revision < accumulated.revision || page.page_offset > accumulated.removals.size()) return false;
  const size_t offset = page.page_offset;
  const size_t overlap = std::min(page.removals.size(), accumulated.removals.size() - offset);
  for (size_t i = 0; i < overlap; ++i) {
    const auto& incoming = page.removals[i];
    const auto& existing = accumulated.removals[offset + i];
    if (incoming.id != existing.id || incoming.call_ordinal != existing.call_ordinal ||
        incoming.ant_ordinal != existing.ant_ordinal) return false;
  }
  if (overlap < page.removals.size()) {
    if (accumulated.removals.size() + page.removals.size() - overlap >
        kMaxMissionActorRemovalHistory) return false;
    accumulated.removals.insert(accumulated.removals.end(), page.removals.begin() + overlap,
                                page.removals.end());
  }
  const bool changed = overlap < page.removals.size() || page.revision > accumulated.revision;
  accumulated.tick = page.tick;
  accumulated.revision = page.revision;
  accumulated.page_offset = 0;
  return changed;
}
std::vector<uint8_t> EncodePing(uint32_t tick) { Writer w(Message::kPing); w.U32(tick); return std::move(w).Finish(); }
std::vector<uint8_t> EncodePong(uint32_t tick) { Writer w(Message::kPong); w.U32(tick); return std::move(w).Finish(); }
std::vector<uint8_t> EncodeError(std::string_view text) { Writer w(Message::kError); w.String(text, 96); return std::move(w).Finish(); }
std::vector<uint8_t> EncodeGoodbye() { return Empty(Message::kGoodbye); }
std::vector<uint8_t> EncodePlayerLeft(uint16_t player_id, uint32_t tick) {
  if (!player_id) return {};
  Writer w(Message::kPlayerLeft);
  w.U16(player_id);
  w.U32(tick);
  return std::move(w).Finish();
}
std::vector<uint8_t> EncodeChat(uint16_t sender, std::string_view text) { Writer w(Message::kChat); w.U16(sender); w.String(text, kMaxChatBytes); return std::move(w).Finish(); }
std::vector<uint8_t> EncodeDirectoryRegister(const ServerInfo& i, uint32_t advertised_ipv4) {
  Writer w(Message::kDirectoryRegister);
  w.U32(advertised_ipv4);
  WriteServerInfo(w, i);
  return std::move(w).Finish();
}
std::vector<uint8_t> EncodeDirectoryQuery(uint16_t page) { Writer w(Message::kDirectoryQuery); w.U16(page); return std::move(w).Finish(); }
std::vector<uint8_t> EncodeDirectoryList(uint16_t page, bool more, std::span<const DirectoryServer> servers) {
  if (servers.size() > 8) return {};
  Writer w(Message::kDirectoryList); w.U16(page); w.U8(more ? 1 : 0); w.U8(static_cast<uint8_t>(servers.size()));
  for (const auto& server : servers) { w.U32(server.ipv4); WriteServerInfo(w, server.info); }
  return std::move(w).Finish();
}

bool Decode(std::span<const uint8_t> bytes, DecodedMessage& out, std::string* error) {
  if (bytes.size() < 6 || bytes.size() > kMaxPacketBytes) return Fail(error, "invalid packet size");
  Reader r(bytes);
  uint32_t magic; uint8_t version, raw_type;
  if (!r.U32(magic) || !r.U8(version) || !r.U8(raw_type) || magic != kMagic || version != kWireVersion)
    return Fail(error, "invalid protocol header");
  out = {};
  out.type = static_cast<Message>(raw_type);
  switch (out.type) {
    case Message::kDiscoveryRequest: break;
    case Message::kServerInfo:
      if (!ReadServerInfo(r, out.server_info))
        return Fail(error, "invalid server info");
      break;
    case Message::kHello: {
      uint16_t protocol;
      if (!r.U16(protocol) || protocol != kProtocolVersion ||
          !r.String(out.build_fingerprint, kBuildFingerprintBytes) ||
          !IsLowerHex(out.build_fingerprint, kBuildFingerprintBytes) ||
          !r.String(out.text, kMaxPlayerNameBytes) || out.text.empty() ||
          !r.Bytes(out.client_public_key) || !r.Bytes(out.client_nonce))
        return Fail(error, "invalid client hello");
      break;
    }
    case Message::kChallenge:
      if (!r.U64(out.nonce) || !out.nonce || !r.U64(out.session_id) || !out.session_id ||
          !r.Bytes(out.client_nonce) || !r.Bytes(out.server_public_key) ||
          !r.Bytes(out.server_nonce) || !r.Bytes(out.server_identity_key) ||
          !r.Bytes(out.server_signature))
        return Fail(error, "invalid authenticated challenge");
      break;
    case Message::kAuthenticate:
      if (!r.U64(out.nonce)) return Fail(error, "invalid challenge");
      break;
    case Message::kWelcome: {
      uint8_t server_authoritative_movement = 0;
      if (!r.U16(out.player_id) || !r.U32(out.tick) ||
          !r.U8(server_authoritative_movement) || server_authoritative_movement > 1 || !out.player_id)
        return Fail(error, "invalid welcome");
      out.server_authoritative_movement = server_authoritative_movement != 0;
      break;
    }
    case Message::kInput: {
      uint8_t gameplay_active = 0;
      uint8_t request_spawn = 0;
      uint8_t transform_valid = 0;
      if (!r.U32(out.input.sequence) || !r.U16(out.input.buttons) || !r.U8(out.input.left_trigger) ||
          !r.U8(out.input.right_trigger) || !r.I16(out.input.move_x) || !r.I16(out.input.move_y) ||
          !r.I16(out.input.aim_x) || !r.I16(out.input.aim_y) || !r.I16(out.input.aim_yaw_mrad) ||
          out.input.aim_yaw_mrad < -3142 || out.input.aim_yaw_mrad > 3142 ||
          !r.U8(gameplay_active) || gameplay_active > 1 ||
          !r.U8(request_spawn) || request_spawn > 1 ||
          !r.U32(out.input.position_reset_ack) || !r.U8(transform_valid) || transform_valid > 1 ||
          !r.I32(out.input.x_cm) || !r.I32(out.input.y_cm) || !r.I32(out.input.z_cm))
        return Fail(error, "invalid input command");
      out.input.gameplay_active = gameplay_active != 0;
      out.input.request_spawn = request_spawn != 0;
      out.input.transform_valid = transform_valid != 0;
      if (out.input.transform_valid &&
          (out.input.x_cm < -kMaxClientCoordinateCm || out.input.x_cm > kMaxClientCoordinateCm ||
           out.input.y_cm < -kMaxClientCoordinateCm || out.input.y_cm > kMaxClientCoordinateCm ||
           out.input.z_cm < -kMaxClientCoordinateCm || out.input.z_cm > kMaxClientCoordinateCm))
        return Fail(error, "client transform is outside world bounds");
      break;
    }
    case Message::kSnapshot: {
      uint8_t raw_count;
      uint32_t active_mask, reset_id = 0;
      if (!r.U32(out.snapshot.tick) || !r.U8(raw_count))
        return Fail(error, "invalid snapshot header");
      const bool has_position_reset = (raw_count & 0x80) != 0;
      out.snapshot.owner_pose_authoritative = (raw_count & 0x40) != 0;
      const uint8_t count = raw_count & 0x3f;
      if (count > kMaxSnapshotPlayers || !r.U32(active_mask) ||
          (count < 32 && (active_mask >> count) != 0) ||
          (out.snapshot.owner_pose_authoritative && count == 0) ||
          (has_position_reset && (!r.U32(reset_id) || !reset_id || count == 0 || (active_mask & 1) != 0)))
        return Fail(error, "invalid snapshot header");
      out.snapshot.players.reserve(count);
      for (uint8_t i = 0; i < count; ++i) {
        PlayerState p;
        uint8_t vitals_flags = 0;
        if (!r.U16(p.id) || !p.id || !r.String(p.name, kMaxPlayerNameBytes) ||
            !r.I32(p.x_cm) || !r.I32(p.y_cm) || !r.I32(p.z_cm) ||
            !r.I16(p.yaw_mrad) || !r.U8(vitals_flags) || (vitals_flags & ~uint8_t{0x03}) ||
            !r.U16(p.health) || !r.U16(p.max_health) ||
            !r.U16(p.buttons) || !r.U8(p.left_trigger) || !r.U8(p.right_trigger) ||
            p.x_cm < -10'000'000 || p.x_cm > 10'000'000 ||
            p.y_cm < -10'000'000 || p.y_cm > 10'000'000 || p.z_cm < -10'000'000 || p.z_cm > 10'000'000 ||
            ((vitals_flags & 0x01) && (!p.max_health || p.health > p.max_health ||
                                      bool(vitals_flags & 0x02) != (p.health > 0))) ||
            (!(vitals_flags & 0x01) && (p.health || p.max_health || (vitals_flags & 0x02))))
          return Fail(error, "invalid player state");
        p.active = (active_mask & (uint32_t{1} << i)) != 0;
        p.health_valid = (vitals_flags & 0x01) != 0;
        p.alive = (vitals_flags & 0x02) != 0;
        p.position_reset = i == 0 && has_position_reset;
        if (p.position_reset) p.position_reset_id = reset_id;
        out.snapshot.players.push_back(std::move(p));
      }
      break;
    }
    case Message::kActorSnapshot:
    case Message::kMissionActorDiagnosticSnapshot: {
      ActorSnapshot& snapshot = out.type == Message::kActorSnapshot
          ? out.actor_snapshot : out.mission_actor_diagnostic_snapshot;
      uint8_t count = 0;
      if (!r.U32(snapshot.tick) || !r.U8(count) || count > kMaxSnapshotActors)
        return Fail(error, "invalid actor snapshot header");
      snapshot.actors.reserve(count);
      for (uint8_t i = 0; i < count; ++i) {
        ActorState actor;
        uint8_t archetype = 0;
        if (!r.U32(actor.id) || !actor.id || !r.U8(archetype) ||
            archetype != static_cast<uint8_t>(ActorArchetype::kAnt) ||
            !r.I32(actor.x_cm) || !r.I32(actor.y_cm) || !r.I32(actor.z_cm) ||
            !r.I16(actor.yaw_mrad) || !r.U16(actor.health) ||
            actor.x_cm < -10'000'000 || actor.x_cm > 10'000'000 ||
            actor.y_cm < -10'000'000 || actor.y_cm > 10'000'000 ||
            actor.z_cm < -10'000'000 || actor.z_cm > 10'000'000 ||
            actor.yaw_mrad < -3142 || actor.yaw_mrad > 3142)
          return Fail(error, "invalid actor state");
        actor.archetype = static_cast<ActorArchetype>(archetype);
        for (const ActorState& previous : snapshot.actors)
          if (previous.id == actor.id) return Fail(error, "duplicate actor id");
        snapshot.actors.push_back(actor);
      }
      break;
    }
    case Message::kMissionActorIdentitySnapshot: {
      auto& snapshot = out.mission_actor_identity_snapshot;
      uint8_t count = 0;
      if (!r.U32(snapshot.tick) || !r.U64(snapshot.host_trace_generation) ||
          !r.U8(count) || count > kMaxSnapshotActors ||
          (!snapshot.host_trace_generation && count != 0))
        return Fail(error, "invalid mission actor identity header");
      snapshot.actors.reserve(count);
      for (uint8_t i = 0; i < count; ++i) {
        MissionActorIdentity actor;
        if (!r.U32(actor.id) || (actor.id & kMissionActorIdNamespace) == 0 ||
            !r.U32(actor.call_ordinal) || actor.call_ordinal >= 4096 ||
            !r.U32(actor.ant_ordinal) || actor.ant_ordinal >= 64)
          return Fail(error, "invalid mission actor identity");
        for (const MissionActorIdentity& previous : snapshot.actors)
          if (previous.id == actor.id ||
              (previous.call_ordinal == actor.call_ordinal &&
               previous.ant_ordinal == actor.ant_ordinal))
            return Fail(error, "duplicate mission actor identity");
        snapshot.actors.push_back(actor);
      }
      break;
    }
    case Message::kMissionActorStateSnapshot: {
      auto& snapshot = out.mission_actor_state_snapshot;
      uint8_t count = 0;
      if (!r.U32(snapshot.tick) || !r.U64(snapshot.host_trace_generation) ||
          !r.U8(count) || count > kMaxSnapshotActors ||
          (!snapshot.host_trace_generation && count != 0))
        return Fail(error, "invalid mission actor state header");
      snapshot.actors.reserve(count);
      for (uint8_t i = 0; i < count; ++i) {
        ActorState actor;
        uint8_t key_low = 0, key_middle = 0, key_high = 0, archetype = 0;
        if (!r.U32(actor.id) || (actor.id & kMissionActorIdNamespace) == 0 ||
            !r.U8(key_low) || !r.U8(key_middle) || !r.U8(key_high) ||
            !r.U8(archetype) || archetype != static_cast<uint8_t>(ActorArchetype::kAnt) ||
            !r.I32(actor.x_cm) || !r.I32(actor.y_cm) || !r.I32(actor.z_cm) ||
            !r.I16(actor.yaw_mrad) || !r.U16(actor.health) ||
            actor.x_cm < -10'000'000 || actor.x_cm > 10'000'000 ||
            actor.y_cm < -10'000'000 || actor.y_cm > 10'000'000 ||
            actor.z_cm < -10'000'000 || actor.z_cm > 10'000'000 ||
            actor.yaw_mrad < -3142 || actor.yaw_mrad > 3142)
          return Fail(error, "invalid mission actor state");
        const uint32_t spawn_key = uint32_t(key_low) |
            (uint32_t(key_middle) << 8) | (uint32_t(key_high) << 16);
        actor.trace_call_ordinal = spawn_key >> 6;
        actor.trace_ant_ordinal = spawn_key & 63u;
        actor.trace_identity_valid = true;
        actor.trace_generation = snapshot.host_trace_generation;
        actor.archetype = static_cast<ActorArchetype>(archetype);
        for (const ActorState& previous : snapshot.actors)
          if (previous.id == actor.id ||
              (previous.trace_call_ordinal == actor.trace_call_ordinal &&
               previous.trace_ant_ordinal == actor.trace_ant_ordinal))
            return Fail(error, "duplicate mission actor state identity");
        snapshot.actors.push_back(actor);
      }
      break;
    }
    case Message::kMissionActorRemovalSnapshot: {
      auto& snapshot = out.mission_actor_removal_snapshot;
      uint8_t count = 0;
      if (!r.U32(snapshot.tick) || !r.U64(snapshot.host_trace_generation) ||
          !r.U32(snapshot.revision) || !r.U32(snapshot.page_offset) || !r.U8(count) ||
          snapshot.revision > kMaxMissionActorRemovalHistory ||
          count > kMaxMissionActorRemovals || snapshot.page_offset > snapshot.revision ||
          uint64_t(snapshot.page_offset) + count > snapshot.revision ||
          (!snapshot.host_trace_generation && (snapshot.revision || snapshot.page_offset || count)))
        return Fail(error, "invalid mission actor removal header");
      snapshot.removals.reserve(count);
      for (uint8_t i = 0; i < count; ++i) {
        MissionActorRemoval removal;
        if (!r.U32(removal.id) || (removal.id & kMissionActorIdNamespace) == 0 ||
            !r.U32(removal.call_ordinal) || removal.call_ordinal >= 4096 ||
            !r.U32(removal.ant_ordinal) || removal.ant_ordinal >= 64 ||
            !snapshot.host_trace_generation)
          return Fail(error, "invalid mission actor removal");
        for (const MissionActorRemoval& previous : snapshot.removals)
          if (previous.id == removal.id ||
              (previous.call_ordinal == removal.call_ordinal &&
               previous.ant_ordinal == removal.ant_ordinal))
            return Fail(error, "duplicate mission actor removal identity");
        snapshot.removals.push_back(removal);
      }
      break;
    }
    case Message::kPing:
    case Message::kPong:
      if (!r.U32(out.tick)) return Fail(error, "invalid heartbeat");
      break;
    case Message::kError:
      if (!r.String(out.text, 96)) return Fail(error, "invalid error");
      break;
    case Message::kChat:
      if (!r.U16(out.player_id) || !r.String(out.text, kMaxChatBytes)) return Fail(error, "invalid chat message");
      break;
    case Message::kDirectoryRegister:
      if (!r.U32(out.directory_ipv4) || !ReadServerInfo(r, out.server_info) ||
          out.server_info.protocol != kProtocolVersion)
        return Fail(error, "invalid directory registration");
      break;
    case Message::kDirectoryQuery:
      if (!r.U16(out.directory_page) || out.directory_page > 4096) return Fail(error, "invalid directory page");
      break;
    case Message::kDirectoryList: {
      uint8_t more, count;
      if (!r.U16(out.directory_page) || !r.U8(more) || more > 1 || !r.U8(count) || count > 8)
        return Fail(error, "invalid directory list header");
      out.directory_more = more != 0;
      out.directory_servers.reserve(count);
      for (uint8_t i = 0; i < count; ++i) {
        DirectoryServer server;
        if (!r.U32(server.ipv4) || !ReadServerInfo(r, server.info) || server.info.protocol != kProtocolVersion)
          return Fail(error, "invalid directory server entry");
        out.directory_servers.push_back(std::move(server));
      }
      break;
    }
    case Message::kGoodbye: break;
    case Message::kPlayerLeft:
      if (!r.U16(out.player_id) || !out.player_id || !r.U32(out.tick))
        return Fail(error, "invalid player-left event");
      break;
    default: return Fail(error, "unknown message type");
  }
  if (!r.Done()) return Fail(error, "unexpected trailing packet bytes");
  return true;
}

}  // namespace edf::net
