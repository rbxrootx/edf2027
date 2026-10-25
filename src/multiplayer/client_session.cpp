#include "client_session.h"
#include "enet_lifecycle.h"
#include "resolve_ipv4.h"
#include "secure_channel.h"

#include <enet/enet.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <charconv>
#include <string_view>
#include <unordered_set>

namespace edf::net {
namespace {
struct Endpoint { std::string host; uint16_t port = kDefaultGamePort; };
PlayerState AsRosterPresence(PlayerState player) {
  player.active = false;
  player.position_reset = false;
  player.position_reset_id = 0;
  player.x_cm = player.y_cm = player.z_cm = 0;
  player.yaw_mrad = 0;
  player.health_valid = false;
  player.alive = false;
  player.health = player.max_health = 0;
  player.buttons = 0;
  player.left_trigger = player.right_trigger = 0;
  return player;
}
bool ParseEndpoint(std::string_view text, Endpoint& out) {
  if (text.empty() || text.size() > 255) return false;
  const auto colon = text.rfind(':');
  std::string_view host = text;
  if (colon != std::string_view::npos) {
    host = text.substr(0, colon);
    const auto port_text = text.substr(colon + 1);
    unsigned port = 0;
    const auto [end, error] = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    if (error != std::errc{} || end != port_text.data() + port_text.size() || port < 1 || port > 65535) return false;
    out.port = static_cast<uint16_t>(port);
  }
  if (host.empty() || host.find_first_of(" \t/\\\r\n") != std::string_view::npos ||
      std::any_of(host.begin(), host.end(), [](unsigned char c) { return c < 0x21 || c == 0x7f; })) return false;
  out.host.assign(host);
  std::transform(out.host.begin(), out.host.end(), out.host.begin(), [](unsigned char c) {
    return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
  });
  return true;
}
void Send(ENetPeer* peer, uint8_t channel, uint32_t flags, const std::vector<uint8_t>& bytes) {
  if (!peer || bytes.empty() || bytes.size() > kMaxPacketBytes) return;
  ENetPacket* packet = enet_packet_create(bytes.data(), bytes.size(), flags);
  if (!packet || enet_peer_send(peer, channel, packet) < 0) {
    if (packet) enet_packet_destroy(packet);
    enet_peer_disconnect(peer, 2);
  }
}
}

ClientSession& ClientSession::Global() {
  static ClientSession session;
  return session;
}

ClientSession::~ClientSession() {
  Disconnect();
  if (worker_.joinable()) worker_.join();
  thread_running_ = false;
}

bool ClientSession::Connect(std::string endpoint, std::string player_name, std::string* error) {
  Endpoint parsed;
  if (!ParseEndpoint(endpoint, parsed)) { if (error) *error = "Use a host name or IPv4 address, optionally followed by :port"; return false; }
  if (player_name.empty() || player_name.size() > kMaxPlayerNameBytes) { if (error) *error = "Player name must be 1-16 bytes"; return false; }
  std::lock_guard lifecycle_lock(lifecycle_mutex_);
  DisconnectLocked();
  if (worker_.joinable()) {
    if (!worker_finished_.load(std::memory_order_acquire)) {
      if (error) *error = "The previous server connection is still shutting down; try again shortly";
      std::lock_guard lock(mutex_);
      view_.status = "Disconnecting previous connection";
      return false;
    }
    worker_.join();
  }
  thread_running_ = false;
  {
    std::lock_guard lock(mutex_);
    view_ = {};
    roster_cache_.clear();
    departed_players_.clear();
    view_.state = ClientState::kConnecting;
    view_.endpoint = std::move(endpoint);
    view_.status = "Resolving server address";
    if (++connection_epoch_ == 0) ++connection_epoch_;
    latest_input_ = {};
    latest_input_at_ = {};
    outgoing_chat_.clear();
    next_sequence_ = 0;
  }
  stop_requested_.store(false, std::memory_order_relaxed);
  worker_finished_.store(false, std::memory_order_release);
  try {
    std::string pin_endpoint = parsed.host + ":" + std::to_string(parsed.port);
    worker_ = std::thread([this, host = std::move(parsed.host), port = parsed.port,
                           player = std::move(player_name), pin = std::move(pin_endpoint)]() mutable {
      Run(std::move(host), port, std::move(player), std::move(pin));
      worker_finished_.store(true, std::memory_order_release);
    });
    thread_running_ = true;
  } catch (const std::exception& failure) {
    worker_finished_.store(true, std::memory_order_release);
    if (error) *error = std::string("Could not start network worker: ") + failure.what();
    SetStatus(ClientState::kFailed, error ? *error : "Could not start network worker");
    return false;
  }
  return true;
}

void ClientSession::Disconnect() {
  std::lock_guard lifecycle_lock(lifecycle_mutex_);
  DisconnectLocked();
}

bool ClientSession::CancelPendingConnect() {
  std::lock_guard lifecycle_lock(lifecycle_mutex_);
  {
    // The WELCOME handler uses this same mutex before publishing kConnected.
    // Whichever side wins decides atomically whether this is still cancelable.
    std::lock_guard lock(mutex_);
    if (view_.state != ClientState::kConnecting) return false;
    stop_requested_.store(true, std::memory_order_release);
  }
  DisconnectLocked();
  return true;
}

void ClientSession::DisconnectLocked() {
  if (!thread_running_ && !worker_.joinable()) return;
  stop_requested_.store(true, std::memory_order_release);
  // DNS cancellation and transport shutdown happen on the worker. Never wait
  // for that work from the browser or game thread; Connect() reaps a finished
  // worker before starting another one, and destruction still joins safely.
  if (worker_.joinable() && worker_finished_.load(std::memory_order_acquire)) {
    worker_.join();
    thread_running_ = false;
  }
  std::lock_guard lock(mutex_);
  view_.state = ClientState::kDisconnected;
  view_.local_player_id = 0;
  view_.server_authoritative_movement = false;
  view_.snapshot = {};
  roster_cache_.clear();
  departed_players_.clear();
  view_.actor_snapshot = {};
  view_.mission_actor_diagnostic_snapshot = {};
  view_.mission_actor_identity_snapshot = {};
  view_.mission_actor_state_snapshot = {};
  view_.mission_actor_removal_snapshot = {};
  mission_actor_removal_generation_ = 0;
  view_.chat.clear();
  outgoing_chat_.clear();
  view_.status = "Disconnected";
}

void ClientSession::SetInput(InputCommand input) {
  std::lock_guard lock(mutex_);
  latest_input_ = input;
  latest_input_at_ = std::chrono::steady_clock::now();
}

bool ClientSession::SendChat(std::string message) {
  if (message.empty() || EncodeChat(0, message).empty()) return false;
  std::lock_guard lock(mutex_);
  if (view_.state != ClientState::kConnected || outgoing_chat_.size() >= 16) return false;
  outgoing_chat_.push_back(std::move(message));
  return true;
}

ClientView ClientSession::View() const { std::lock_guard lock(mutex_); return view_; }
bool ClientSession::CopySnapshotIfNew(uint64_t& generation, WorldSnapshot& snapshot, uint16_t& local_player_id) const {
  std::lock_guard lock(mutex_);
  if (view_.state != ClientState::kConnected || view_.snapshot.tick == 0 || snapshot_generation_ == generation)
    return false;
  snapshot = view_.snapshot;
  local_player_id = view_.local_player_id;
  generation = snapshot_generation_;
  return true;
}
bool ClientSession::CopyActorSnapshotIfNew(uint64_t& generation, ActorSnapshot& snapshot) const {
  std::lock_guard lock(mutex_);
  if (view_.state != ClientState::kConnected || view_.actor_snapshot.tick == 0 ||
      actor_snapshot_generation_ == generation) return false;
  snapshot = view_.actor_snapshot;
  generation = actor_snapshot_generation_;
  return true;
}
bool ClientSession::CopyMissionActorDiagnosticSnapshotIfNew(uint64_t& generation,
                                                             ActorSnapshot& snapshot) const {
  std::lock_guard lock(mutex_);
  if (view_.state != ClientState::kConnected ||
      view_.mission_actor_diagnostic_snapshot.tick == 0 ||
      mission_actor_diagnostic_generation_ == generation) return false;
  snapshot = view_.mission_actor_diagnostic_snapshot;
  generation = mission_actor_diagnostic_generation_;
  return true;
}
bool ClientSession::CopyMissionActorIdentitySnapshotIfNew(
    uint64_t& generation, MissionActorIdentitySnapshot& snapshot) const {
  std::lock_guard lock(mutex_);
  if (view_.state != ClientState::kConnected ||
      !view_.mission_actor_identity_snapshot.tick ||
      mission_actor_identity_generation_ == generation) return false;
  snapshot = view_.mission_actor_identity_snapshot;
  generation = mission_actor_identity_generation_;
  return true;
}
bool ClientSession::CopyMissionActorStateSnapshotIfNew(
    uint64_t& generation, MissionActorStateSnapshot& snapshot) const {
  std::lock_guard lock(mutex_);
  if (view_.state != ClientState::kConnected ||
      !view_.mission_actor_state_snapshot.tick ||
      mission_actor_state_generation_ == generation) return false;
  snapshot = view_.mission_actor_state_snapshot;
  generation = mission_actor_state_generation_;
  return true;
}
bool ClientSession::CopyMissionActorRemovalSnapshotIfNew(
    uint64_t& generation, MissionActorRemovalSnapshot& snapshot) const {
  std::lock_guard lock(mutex_);
  if (view_.state != ClientState::kConnected ||
      !view_.mission_actor_removal_snapshot.tick ||
      mission_actor_removal_generation_ == generation) return false;
  snapshot = view_.mission_actor_removal_snapshot;
  generation = mission_actor_removal_generation_;
  return true;
}
bool ClientSession::connected() const { std::lock_guard lock(mutex_); return view_.state == ClientState::kConnected; }
bool ClientSession::connected(uint64_t& connection_epoch) const {
  std::lock_guard lock(mutex_);
  connection_epoch = connection_epoch_;
  return view_.state == ClientState::kConnected;
}
uint16_t ClientSession::local_player_id() const {
  std::lock_guard lock(mutex_);
  return view_.state == ClientState::kConnected ? view_.local_player_id : 0;
}
bool ClientSession::server_authoritative_movement() const {
  std::lock_guard lock(mutex_);
  return view_.state == ClientState::kConnected && view_.server_authoritative_movement;
}

void ClientSession::SetStatus(ClientState state, std::string text) {
  std::lock_guard lock(mutex_);
  if (stop_requested_.load(std::memory_order_relaxed)) return;
  view_.state = state;
  view_.status = std::move(text);
  if (state != ClientState::kConnected) {
    view_.local_player_id = 0;
    view_.server_authoritative_movement = false;
    view_.snapshot = {};
    view_.actor_snapshot = {};
    view_.mission_actor_diagnostic_snapshot = {};
    view_.mission_actor_identity_snapshot = {};
    view_.mission_actor_state_snapshot = {};
    view_.mission_actor_removal_snapshot = {};
    mission_actor_removal_generation_ = 0;
    roster_cache_.clear();
    departed_players_.clear();
    outgoing_chat_.clear();
  }
}

void ClientSession::Run(std::string host_name, uint16_t port, std::string player_name,
                        std::string pin_endpoint) {
  if (!AcquireENet()) { SetStatus(ClientState::kFailed, "ENet initialization failed"); return; }
  ENetHost* host = enet_host_create(nullptr, 1, 2, 0, 0);
  if (!host) { ReleaseENet(); SetStatus(ClientState::kFailed, "Could not create ENet client"); return; }
  host->maximumPacketSize = kMaxPacketBytes;
  host->maximumWaitingData = kMaxPacketBytes * 8;
  ENetAddress address{};
  std::string resolution_error;
  const auto cancelled = [this] { return stop_requested_.load(std::memory_order_relaxed); };
  if (!ResolveIPv4(host_name, port, std::chrono::seconds(5), cancelled,
                   address, &resolution_error)) {
    enet_host_destroy(host);
    ReleaseENet();
    if (!cancelled()) SetStatus(ClientState::kFailed,
        resolution_error.empty() ? "Could not resolve the server address" : std::move(resolution_error));
    return;
  }
  if (cancelled()) { enet_host_destroy(host); ReleaseENet(); return; }
  ENetPeer* peer = enet_host_connect(host, &address, 2, kProtocolVersion);
  if (!peer) { enet_host_destroy(host); ReleaseENet(); SetStatus(ClientState::kFailed, "No ENet connection slots"); return; }
  enet_peer_timeout(peer, 8, 5'000, 15'000);
  CryptoKeyPair client_ephemeral;
  CryptoNonce client_nonce{};
  std::optional<SecureChannel> secure_channel;
  std::string server_fingerprint;
  if (!GenerateCryptoKeyPair(client_ephemeral) || !GenerateCryptoNonce(client_nonce)) {
    ClearCryptoKeyPair(client_ephemeral);
    enet_peer_disconnect_now(peer, 3);
    enet_host_destroy(host);
    ReleaseENet();
    SetStatus(ClientState::kFailed, "Could not create an authenticated network session");
    return;
  }
  auto send_message = [&](uint8_t channel, uint32_t flags, const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) return;
    const std::vector<uint8_t> wire = secure_channel
        ? secure_channel->Seal(channel, bytes) : bytes;
    Send(peer, channel, flags, wire);
  };
  auto last_input = std::chrono::steady_clock::now();
  auto last_ping = last_input;
  auto last_snapshot = last_input;
  auto last_chat_send = std::chrono::steady_clock::time_point{};
  auto next_input_sequence = 0u;
  bool connecting = true;
  bool authenticated = false;
  while (!stop_requested_.load(std::memory_order_relaxed)) {
    ENetEvent event{};
    const int serviced = enet_host_service(host, &event, 10);
    if (serviced < 0) {
      SetStatus(ClientState::kFailed, "Connection transport failed");
      break;
    }
    if (serviced > 0) {
      if (event.type == ENET_EVENT_TYPE_CONNECT) {
        Send(peer, 0, ENET_PACKET_FLAG_RELIABLE,
             EncodeHello(player_name, client_ephemeral.public_key, client_nonce));
      } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
        DecodedMessage message;
        std::vector<uint8_t> plaintext;
        bool decoded = false;
        if (event.packet->dataLength <= kMaxPacketBytes) {
          if (secure_channel) {
            decoded = secure_channel->Open(event.channelID,
                std::span(event.packet->data, event.packet->dataLength), plaintext) &&
                Decode(plaintext, message);
            if (!decoded) {
              SetStatus(ClientState::kFailed, "Server sent an invalid or unauthenticated session packet");
              enet_packet_destroy(event.packet);
              break;
            }
          } else {
            decoded = event.channelID == 0 &&
                Decode(std::span(event.packet->data, event.packet->dataLength), message);
            if (decoded && message.type == Message::kError) {
              SetStatus(ClientState::kFailed, std::move(message.text));
              enet_packet_destroy(event.packet);
              break;
            }
            decoded = decoded && message.type == Message::kChallenge &&
                      message.client_nonce == client_nonce;
            if (!decoded || !VerifyHandshakeTranscript(message.server_identity_key,
                    client_ephemeral.public_key, message.server_public_key, client_nonce,
                    message.server_nonce, message.session_id, message.nonce,
                    message.server_signature)) {
              SetStatus(ClientState::kFailed, "Server authentication failed; no secure session was opened");
              enet_packet_destroy(event.packet);
              break;
            }
            bool first_use = false;
            std::string pin_error;
            server_fingerprint = CryptoFingerprint(message.server_identity_key);
            if (!VerifyOrRememberServerIdentity(pin_endpoint, message.server_identity_key,
                                                first_use, &pin_error)) {
              SetStatus(ClientState::kFailed, std::move(pin_error));
              enet_packet_destroy(event.packet);
              break;
            }
            CryptoSessionKeys keys;
            keys.session_id = message.session_id;
            CryptoNoncePrefix nonce_prefix{};
            if (!DeriveClientSessionKeys(client_ephemeral, message.server_public_key, keys) ||
                !DeriveNoncePrefix(client_nonce, message.server_nonce, message.session_id, nonce_prefix)) {
              SetStatus(ClientState::kFailed, "Could not derive encrypted session keys");
              enet_packet_destroy(event.packet);
              break;
            }
            secure_channel.emplace(keys, nonce_prefix);
            ClearCryptoSessionKeys(keys);
            std::fill(nonce_prefix.begin(), nonce_prefix.end(), 0);
            ClearCryptoKeyPair(client_ephemeral);
            const auto authenticate = EncodeAuthenticate(message.nonce);
            send_message(0, ENET_PACKET_FLAG_RELIABLE, authenticate);
            SetStatus(ClientState::kConnecting,
                std::string(first_use ? "First-use server fingerprint saved: " : "Verified server fingerprint: ") + server_fingerprint);
            decoded = false;
          }
        }
        if (decoded && message.type == Message::kWelcome && event.channelID == 0) {
            std::lock_guard lock(mutex_);
            if (!stop_requested_.load(std::memory_order_relaxed)) {
              authenticated = true;
              connecting = false;
              view_.state = ClientState::kConnected;
              view_.local_player_id = message.player_id;
              view_.server_authoritative_movement = message.server_authoritative_movement;
              view_.status = (message.server_authoritative_movement
                  ? "Connected (server-authoritative movement; key "
                  : "Connected (server key ") + server_fingerprint + ")";
            }
        } else if (decoded && message.type == Message::kSnapshot && authenticated && event.channelID == 1) {
            std::lock_guard lock(mutex_);
            if (!stop_requested_.load(std::memory_order_relaxed) &&
                (view_.snapshot.tick == 0 || static_cast<int32_t>(message.snapshot.tick - view_.snapshot.tick) >= 0)) {
              WorldSnapshot incoming = std::move(message.snapshot);
              const auto now = std::chrono::steady_clock::now();
              std::unordered_set<uint16_t> included;
              included.reserve(incoming.players.size());
              for (const auto& player : incoming.players) {
                if (const auto departed = departed_players_.find(player.id);
                    departed != departed_players_.end()) {
                  if (static_cast<int32_t>(incoming.tick - departed->second) <= 0) continue;
                  departed_players_.erase(departed);
                }
                included.insert(player.id);
                roster_cache_[player.id] = CachedPresence{player, now};
              }
              constexpr auto kRosterEntryLifetime = std::chrono::seconds(8);
              for (auto entry = roster_cache_.begin(); entry != roster_cache_.end();) {
                if (now - entry->second.last_seen > kRosterEntryLifetime)
                  entry = roster_cache_.erase(entry);
                else
                  ++entry;
              }
              std::vector<PlayerState> cached_presence;
              cached_presence.reserve(roster_cache_.size() - std::min(roster_cache_.size(), included.size()));
              for (const auto& [id, cached] : roster_cache_)
                if (!included.contains(id)) cached_presence.push_back(AsRosterPresence(cached.state));
              std::sort(cached_presence.begin(), cached_presence.end(),
                        [](const PlayerState& left, const PlayerState& right) { return left.id < right.id; });
              incoming.players.insert(incoming.players.end(), cached_presence.begin(), cached_presence.end());
              view_.snapshot = std::move(incoming);
              ++snapshot_generation_;
              last_snapshot = std::chrono::steady_clock::now();
            }
        } else if (decoded && message.type == Message::kPlayerLeft && authenticated && event.channelID == 0) {
            std::lock_guard lock(mutex_);
            if (!stop_requested_.load(std::memory_order_relaxed)) {
              const auto previous = departed_players_.find(message.player_id);
              if (previous == departed_players_.end() ||
                  static_cast<int32_t>(message.tick - previous->second) > 0)
                departed_players_[message.player_id] = message.tick;
              roster_cache_.erase(message.player_id);
              const size_t previous_size = view_.snapshot.players.size();
              std::erase_if(view_.snapshot.players, [&](const PlayerState& player) {
                return player.id == message.player_id;
              });
              if (previous_size != view_.snapshot.players.size()) ++snapshot_generation_;
            }
        } else if (decoded && message.type == Message::kActorSnapshot && authenticated && event.channelID == 1) {
            std::lock_guard lock(mutex_);
            if (!stop_requested_.load(std::memory_order_relaxed) &&
                (view_.actor_snapshot.tick == 0 ||
                 static_cast<int32_t>(message.actor_snapshot.tick - view_.actor_snapshot.tick) >= 0)) {
              view_.actor_snapshot = std::move(message.actor_snapshot);
              ++actor_snapshot_generation_;
            }
        } else if (decoded && message.type == Message::kMissionActorDiagnosticSnapshot &&
                      authenticated && event.channelID == 1) {
            std::lock_guard lock(mutex_);
            const ActorSnapshot& incoming = message.mission_actor_diagnostic_snapshot;
            if (!stop_requested_.load(std::memory_order_relaxed) &&
                (view_.mission_actor_diagnostic_snapshot.tick == 0 ||
                 static_cast<int32_t>(incoming.tick - view_.mission_actor_diagnostic_snapshot.tick) >= 0)) {
              view_.mission_actor_diagnostic_snapshot = std::move(message.mission_actor_diagnostic_snapshot);
              ++mission_actor_diagnostic_generation_;
            }
        } else if (decoded && message.type == Message::kMissionActorIdentitySnapshot &&
                      authenticated && event.channelID == 1) {
            std::lock_guard lock(mutex_);
            const MissionActorIdentitySnapshot& incoming = message.mission_actor_identity_snapshot;
            if (!stop_requested_.load(std::memory_order_relaxed) &&
                (!view_.mission_actor_identity_snapshot.host_trace_generation ||
                 static_cast<int32_t>(incoming.tick - view_.mission_actor_identity_snapshot.tick) >= 0)) {
              view_.mission_actor_identity_snapshot = std::move(message.mission_actor_identity_snapshot);
              ++mission_actor_identity_generation_;
            }
        } else if (decoded && message.type == Message::kMissionActorStateSnapshot &&
                      authenticated && event.channelID == 1) {
            std::lock_guard lock(mutex_);
            const MissionActorStateSnapshot& incoming = message.mission_actor_state_snapshot;
            if (!stop_requested_.load(std::memory_order_relaxed) &&
                IsMissionActorStateSnapshotNewer(incoming, view_.mission_actor_state_snapshot)) {
              view_.mission_actor_state_snapshot = std::move(message.mission_actor_state_snapshot);
              ++mission_actor_state_generation_;
            }
        } else if (decoded && message.type == Message::kMissionActorRemovalSnapshot &&
                      authenticated && event.channelID == 1) {
            std::lock_guard lock(mutex_);
            const MissionActorRemovalSnapshot& incoming = message.mission_actor_removal_snapshot;
            if (!stop_requested_.load(std::memory_order_relaxed) &&
                MergeMissionActorRemovalPage(incoming, view_.mission_actor_removal_snapshot)) {
              ++mission_actor_removal_generation_;
            }
        } else if (decoded && message.type == Message::kChat && authenticated && event.channelID == 0) {
            std::lock_guard lock(mutex_);
            if (!stop_requested_.load(std::memory_order_relaxed)) {
              const std::string prefix = message.player_id ? "Player " + std::to_string(message.player_id) + ": " : "Server: ";
              view_.chat.push_back(prefix + message.text);
              if (view_.chat.size() > 64) view_.chat.erase(view_.chat.begin(), view_.chat.begin() + 16);
            }
        } else if (decoded && message.type == Message::kError && event.channelID == 0) {
            SetStatus(ClientState::kFailed, message.text);
            enet_packet_destroy(event.packet);
            break;
        } else if (decoded && message.type == Message::kPong && event.channelID == 0) {
            // Kept for the connection heartbeat; the UI can add latency display later.
        }
        enet_packet_destroy(event.packet);
      } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
        SetStatus(connecting ? ClientState::kFailed : ClientState::kDisconnected,
                  connecting ? "Connection was rejected or timed out" : "Server disconnected");
        break;
      }
    }
    const auto now = std::chrono::steady_clock::now();
    if (stop_requested_.load(std::memory_order_relaxed)) break;
    if (authenticated && now - last_input >= std::chrono::milliseconds(16)) {
      InputCommand input;
      bool server_authoritative_movement = false;
      {
        std::lock_guard lock(mutex_);
        input = latest_input_;
        server_authoritative_movement = view_.server_authoritative_movement;
        if (latest_input_at_ == std::chrono::steady_clock::time_point{} ||
            now - latest_input_at_ > std::chrono::milliseconds(250)) {
          input.buttons = 0;
          input.left_trigger = input.right_trigger = 0;
          input.move_x = input.move_y = input.aim_x = input.aim_y = input.aim_yaw_mrad = 0;
          input.gameplay_active = false;
          input.request_spawn = false;
        }
      }
      if (server_authoritative_movement) {
        input.transform_valid = false;
        input.x_cm = input.y_cm = input.z_cm = 0;
      }
      input.sequence = ++next_input_sequence;
      send_message(1, ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT, EncodeInput(input));
      last_input = now;
    }
    if (authenticated && (last_chat_send == std::chrono::steady_clock::time_point{} ||
                          now - last_chat_send >= std::chrono::milliseconds(600))) {
      std::string chat;
      {
        std::lock_guard lock(mutex_);
        if (!outgoing_chat_.empty()) {
          chat = std::move(outgoing_chat_.front());
          outgoing_chat_.pop_front();
        }
      }
      if (!chat.empty()) {
        send_message(0, ENET_PACKET_FLAG_RELIABLE, EncodeChat(0, chat));
        last_chat_send = now;
      }
    }
    if (authenticated && now - last_ping >= std::chrono::seconds(1)) {
      send_message(0, ENET_PACKET_FLAG_RELIABLE, EncodePing(uint32_t(now.time_since_epoch().count())));
      last_ping = now;
    }
    if (!connecting && now - last_snapshot > std::chrono::seconds(5)) {
      SetStatus(ClientState::kFailed, "No server snapshots received");
      break;
    }
    enet_host_flush(host);
  }
  if (peer && peer->state != ENET_PEER_STATE_DISCONNECTED) {
    if (authenticated) send_message(0, ENET_PACKET_FLAG_RELIABLE, EncodeGoodbye());
    enet_peer_disconnect_now(peer, 0);
  }
  enet_host_destroy(host);
  ReleaseENet();
  ClearCryptoKeyPair(client_ephemeral);
}

}  // namespace edf::net
