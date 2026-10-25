#include "secure_channel.h"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace edf::net {
namespace {
constexpr size_t kWireSequenceBytes = sizeof(uint64_t);
constexpr size_t kAuthenticationTagBytes = crypto_aead_xchacha20poly1305_ietf_ABYTES;
constexpr size_t kAadBytes = 2 + 1 + sizeof(uint64_t) + sizeof(uint64_t);
constexpr size_t kAeadNonceBytes = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
constexpr std::string_view kHandshakeDomain = "EDF2027/server-handshake/v1";

void WriteU64(uint8_t* output, uint64_t value) {
  for (size_t i = 0; i < sizeof(value); ++i)
    output[i] = static_cast<uint8_t>(value >> (i * 8));
}

uint64_t ReadU64(const uint8_t* input) {
  uint64_t value = 0;
  for (size_t i = 0; i < sizeof(value); ++i)
    value |= uint64_t(input[i]) << (i * 8);
  return value;
}

std::array<uint8_t, kAadBytes> AssociatedData(uint8_t channel, uint64_t session_id,
                                               uint64_t sequence) {
  std::array<uint8_t, kAadBytes> aad{};
  aad[0] = static_cast<uint8_t>(kProtocolVersion & 0xFF);
  aad[1] = static_cast<uint8_t>(kProtocolVersion >> 8);
  aad[2] = channel;
  WriteU64(aad.data() + 3, session_id);
  WriteU64(aad.data() + 3 + sizeof(session_id), sequence);
  return aad;
}

std::array<uint8_t, kAeadNonceBytes> MakeAeadNonce(const CryptoNoncePrefix& prefix,
                                                   uint64_t sequence) {
  std::array<uint8_t, kAeadNonceBytes> nonce{};
  std::copy(prefix.begin(), prefix.end(), nonce.begin());
  WriteU64(nonce.data() + prefix.size(), sequence);
  return nonce;
}

std::vector<uint8_t> HandshakeTranscript(const CryptoPublicKey& client_public_key,
                                         const CryptoPublicKey& server_public_key,
                                         const CryptoNonce& client_nonce,
                                         const CryptoNonce& server_nonce,
                                         uint64_t session_id, uint64_t challenge_nonce) {
  std::vector<uint8_t> transcript;
  transcript.reserve(kHandshakeDomain.size() + 2 + kPortBuildFingerprint.size() +
                     client_public_key.size() + server_public_key.size() +
                     client_nonce.size() + server_nonce.size() + sizeof(session_id));
  transcript.insert(transcript.end(), kHandshakeDomain.begin(), kHandshakeDomain.end());
  transcript.push_back(static_cast<uint8_t>(kProtocolVersion & 0xff));
  transcript.push_back(static_cast<uint8_t>(kProtocolVersion >> 8));
  transcript.insert(transcript.end(), kPortBuildFingerprint.begin(), kPortBuildFingerprint.end());
  transcript.insert(transcript.end(), client_public_key.begin(), client_public_key.end());
  transcript.insert(transcript.end(), server_public_key.begin(), server_public_key.end());
  transcript.insert(transcript.end(), client_nonce.begin(), client_nonce.end());
  transcript.insert(transcript.end(), server_nonce.begin(), server_nonce.end());
  std::array<uint8_t, sizeof(session_id)> session{};
  WriteU64(session.data(), session_id);
  transcript.insert(transcript.end(), session.begin(), session.end());
  std::array<uint8_t, sizeof(challenge_nonce)> challenge{};
  WriteU64(challenge.data(), challenge_nonce);
  transcript.insert(transcript.end(), challenge.begin(), challenge.end());
  return transcript;
}

std::filesystem::path PinDirectory() {
#if defined(_WIN32)
  const char* root = std::getenv("LOCALAPPDATA");
  if (!root || !*root) return {};
  return std::filesystem::path(root) / "EDF2027" / "server-pins";
#else
  const char* config = std::getenv("XDG_CONFIG_HOME");
  if (config && *config) return std::filesystem::path(config) / "edf2027" / "server-pins";
  const char* home = std::getenv("HOME");
  if (!home || !*home) return {};
  return std::filesystem::path(home) / ".config" / "edf2027" / "server-pins";
#endif
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return {};
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}
}  // namespace

bool InitializeCrypto(std::string* error) {
  if (error) error->clear();
  if (sodium_init() >= 0) return true;
  if (error) *error = "libsodium initialization failed";
  return false;
}

bool GenerateCryptoKeyPair(CryptoKeyPair& key_pair) {
  if (!InitializeCrypto()) return false;
  if (crypto_kx_keypair(key_pair.public_key.data(), key_pair.secret_key.data()) == 0) return true;
  sodium_memzero(key_pair.secret_key.data(), key_pair.secret_key.size());
  key_pair.public_key.fill(0);
  return false;
}

bool GenerateCryptoNonce(CryptoNonce& nonce) {
  if (!InitializeCrypto()) return false;
  randombytes_buf(nonce.data(), nonce.size());
  return true;
}

bool DeriveSigningKeyPair(std::span<const uint8_t, 32> seed, CryptoSigningKeyPair& key_pair) {
  if (!InitializeCrypto()) return false;
  if (crypto_sign_seed_keypair(key_pair.public_key.data(), key_pair.secret_key.data(), seed.data()) == 0)
    return true;
  sodium_memzero(key_pair.secret_key.data(), key_pair.secret_key.size());
  key_pair.public_key.fill(0);
  return false;
}

bool SignHandshakeTranscript(const CryptoSigningKeyPair& key_pair,
                             const CryptoPublicKey& client_public_key,
                             const CryptoPublicKey& server_public_key,
                             const CryptoNonce& client_nonce,
                             const CryptoNonce& server_nonce,
                             uint64_t session_id, uint64_t challenge_nonce,
                             CryptoSigningSignature& signature) {
  if (!InitializeCrypto()) return false;
  const auto transcript = HandshakeTranscript(client_public_key, server_public_key,
                                               client_nonce, server_nonce, session_id, challenge_nonce);
  unsigned long long signature_bytes = 0;
  return crypto_sign_detached(signature.data(), &signature_bytes, transcript.data(),
                              transcript.size(), key_pair.secret_key.data()) == 0 &&
         signature_bytes == signature.size();
}

bool VerifyHandshakeTranscript(const CryptoPublicKey& identity_public_key,
                               const CryptoPublicKey& client_public_key,
                               const CryptoPublicKey& server_public_key,
                               const CryptoNonce& client_nonce,
                               const CryptoNonce& server_nonce,
                               uint64_t session_id, uint64_t challenge_nonce,
                               const CryptoSigningSignature& signature) {
  if (!InitializeCrypto()) return false;
  const auto transcript = HandshakeTranscript(client_public_key, server_public_key,
                                               client_nonce, server_nonce, session_id, challenge_nonce);
  return crypto_sign_verify_detached(signature.data(), transcript.data(), transcript.size(),
                                     identity_public_key.data()) == 0;
}

bool VerifyOrRememberServerIdentity(std::string_view endpoint,
                                    const CryptoPublicKey& identity_public_key,
                                    bool& first_use, std::string* error) {
  if (error) error->clear();
  first_use = false;
  if (!InitializeCrypto() || endpoint.empty() || endpoint.find_first_of("\r\n\t") != std::string_view::npos) {
    if (error) *error = "invalid server identity pin request";
    return false;
  }
  const auto directory = PinDirectory();
  if (directory.empty()) {
    if (error) *error = "could not locate a per-user server identity store";
    return false;
  }
  std::array<uint8_t, 16> endpoint_hash{};
  if (crypto_generichash(endpoint_hash.data(), endpoint_hash.size(),
                         reinterpret_cast<const uint8_t*>(endpoint.data()), endpoint.size(),
                         nullptr, 0) != 0) {
    if (error) *error = "could not derive server identity pin path";
    return false;
  }
  std::array<char, 33> endpoint_hex{};
  sodium_bin2hex(endpoint_hex.data(), endpoint_hex.size(), endpoint_hash.data(), endpoint_hash.size());
  const auto path = directory / (std::string(endpoint_hex.data()) + ".pin");
  const std::string fingerprint = CryptoFingerprint(identity_public_key);
  const std::string expected = "EDF2027-SERVER-KEY-PIN-v1\n" + std::string(endpoint) + "\n" +
                               fingerprint + "\n";
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    if (error) *error = "could not create per-user server identity store: " + ec.message();
    return false;
  }
  if (std::filesystem::exists(path, ec)) {
    const std::string saved = ReadFile(path);
    if (saved == expected) return true;
    if (error) {
      const size_t first = saved.find('\n');
      const size_t second = first == std::string::npos ? first : saved.find('\n', first + 1);
      const size_t third = second == std::string::npos ? second : saved.find('\n', second + 1);
      const std::string previous = second != std::string::npos && third != std::string::npos
          ? saved.substr(second + 1, third - second - 1) : "unreadable pin";
      *error = "server identity changed for " + std::string(endpoint) + "; saved fingerprint " +
               previous + ", received " + fingerprint +
               ". If you expect this server key rotation, remove its pin from " + path.string();
    }
    return false;
  }
  const auto temporary = path.string() + ".tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output.write(expected.data(), static_cast<std::streamsize>(expected.size()));
    output.flush();
    if (!output) {
      if (error) *error = "could not save the server identity pin";
      return false;
    }
  }
  std::filesystem::rename(temporary, path, ec);
  if (ec) {
    std::filesystem::remove(temporary);
    const std::string saved = ReadFile(path);
    if (saved == expected) return true;
    if (error) *error = "could not commit server identity pin: " + ec.message();
    return false;
  }
  first_use = true;
  return true;
}

void ClearCryptoKeyPair(CryptoKeyPair& key_pair) {
  sodium_memzero(key_pair.secret_key.data(), key_pair.secret_key.size());
  key_pair.public_key.fill(0);
}

void ClearCryptoSigningKeyPair(CryptoSigningKeyPair& key_pair) {
  sodium_memzero(key_pair.secret_key.data(), key_pair.secret_key.size());
  key_pair.public_key.fill(0);
}

void ClearCryptoSessionKeys(CryptoSessionKeys& keys) {
  sodium_memzero(keys.transmit.data(), keys.transmit.size());
  sodium_memzero(keys.receive.data(), keys.receive.size());
  keys.session_id = 0;
}

bool DeriveClientSessionKeys(const CryptoKeyPair& client,
                             const CryptoPublicKey& server_public_key,
                             CryptoSessionKeys& keys) {
  if (!InitializeCrypto()) return false;
  CryptoSessionKey receive{};
  CryptoSessionKey transmit{};
  if (crypto_kx_client_session_keys(receive.data(), transmit.data(), client.public_key.data(),
                                    client.secret_key.data(), server_public_key.data()) != 0) {
    sodium_memzero(receive.data(), receive.size());
    sodium_memzero(transmit.data(), transmit.size());
    return false;
  }
  keys.receive = receive;
  keys.transmit = transmit;
  return true;
}

bool DeriveServerSessionKeys(const CryptoKeyPair& server,
                             const CryptoPublicKey& client_public_key,
                             CryptoSessionKeys& keys) {
  if (!InitializeCrypto()) return false;
  CryptoSessionKey receive{};
  CryptoSessionKey transmit{};
  if (crypto_kx_server_session_keys(receive.data(), transmit.data(), server.public_key.data(),
                                    server.secret_key.data(), client_public_key.data()) != 0) {
    sodium_memzero(receive.data(), receive.size());
    sodium_memzero(transmit.data(), transmit.size());
    return false;
  }
  keys.receive = receive;
  keys.transmit = transmit;
  return true;
}

bool DeriveNoncePrefix(const CryptoNonce& client_nonce, const CryptoNonce& server_nonce,
                       uint64_t session_id, CryptoNoncePrefix& prefix) {
  if (!InitializeCrypto()) return false;
  constexpr std::string_view domain = "EDF2027/secure-channel/v1";
  std::vector<uint8_t> input;
  input.reserve(domain.size() + 2 + client_nonce.size() + server_nonce.size() + sizeof(session_id));
  input.insert(input.end(), domain.begin(), domain.end());
  input.push_back(static_cast<uint8_t>(kProtocolVersion & 0xFF));
  input.push_back(static_cast<uint8_t>(kProtocolVersion >> 8));
  input.insert(input.end(), client_nonce.begin(), client_nonce.end());
  input.insert(input.end(), server_nonce.begin(), server_nonce.end());
  std::array<uint8_t, sizeof(session_id)> session_bytes{};
  WriteU64(session_bytes.data(), session_id);
  input.insert(input.end(), session_bytes.begin(), session_bytes.end());
  return crypto_generichash(prefix.data(), prefix.size(), input.data(), input.size(), nullptr, 0) == 0;
}

std::string CryptoFingerprint(const CryptoPublicKey& public_key) {
  std::array<char, crypto_kx_PUBLICKEYBYTES * 2 + 1> hex{};
  if (!InitializeCrypto() || !sodium_bin2hex(hex.data(), hex.size(), public_key.data(), public_key.size()))
    return {};
  std::string fingerprint(hex.data());
  for (char& character : fingerprint)
    if (character >= 'a' && character <= 'f') character = static_cast<char>(character - 'a' + 'A');
  return fingerprint;
}

SecureChannel::SecureChannel(const CryptoSessionKeys& keys, const CryptoNoncePrefix& nonce_prefix)
    : transmit_key_(keys.transmit), receive_key_(keys.receive), nonce_prefix_(nonce_prefix),
      session_id_(keys.session_id), ready_(InitializeCrypto()) {}

SecureChannel::~SecureChannel() { Clear(); }

SecureChannel::SecureChannel(SecureChannel&& other) noexcept { *this = std::move(other); }

SecureChannel& SecureChannel::operator=(SecureChannel&& other) noexcept {
  if (this == &other) return *this;
  Clear();
  transmit_key_ = other.transmit_key_;
  receive_key_ = other.receive_key_;
  nonce_prefix_ = other.nonce_prefix_;
  session_id_ = other.session_id_;
  next_send_sequence_ = other.next_send_sequence_;
  highest_receive_sequence_ = other.highest_receive_sequence_;
  received_sequences_ = other.received_sequences_;
  receive_sequence_initialized_ = other.receive_sequence_initialized_;
  ready_ = other.ready_;
  other.Clear();
  return *this;
}

std::vector<uint8_t> SecureChannel::Seal(uint8_t channel, std::span<const uint8_t> plaintext) {
  if (!ready_ || channel > 1 || plaintext.empty() || plaintext.size() > kMaxSecurePayloadBytes ||
      next_send_sequence_ == std::numeric_limits<uint64_t>::max()) return {};
  const uint64_t sequence = next_send_sequence_++;
  const auto nonce = MakeAeadNonce(nonce_prefix_, sequence);
  const auto aad = AssociatedData(channel, session_id_, sequence);
  std::vector<uint8_t> packet(kWireSequenceBytes + plaintext.size() + kAuthenticationTagBytes);
  WriteU64(packet.data(), sequence);
  unsigned long long ciphertext_bytes = 0;
  const int result = crypto_aead_xchacha20poly1305_ietf_encrypt(
      packet.data() + kWireSequenceBytes, &ciphertext_bytes, plaintext.data(), plaintext.size(),
      aad.data(), aad.size(), nullptr, nonce.data(), transmit_key_.data());
  if (result != 0 || ciphertext_bytes != plaintext.size() + kAuthenticationTagBytes) {
    sodium_memzero(packet.data(), packet.size());
    return {};
  }
  return packet;
}

bool SecureChannel::Open(uint8_t channel, std::span<const uint8_t> packet,
                         std::vector<uint8_t>& plaintext) {
  plaintext.clear();
  if (!ready_ || channel > 1 || packet.size() <= kSecureEnvelopeBytes ||
      packet.size() > kMaxPacketBytes) return false;
  const uint64_t sequence = ReadU64(packet.data());
  if (!CanAcceptSequence(sequence)) return false;
  const auto nonce = MakeAeadNonce(nonce_prefix_, sequence);
  const auto aad = AssociatedData(channel, session_id_, sequence);
  const size_t ciphertext_bytes = packet.size() - kWireSequenceBytes;
  const size_t plaintext_capacity = ciphertext_bytes - kAuthenticationTagBytes;
  if (plaintext_capacity > kMaxSecurePayloadBytes) return false;
  std::vector<uint8_t> candidate(plaintext_capacity);
  unsigned long long plaintext_bytes = 0;
  const int result = crypto_aead_xchacha20poly1305_ietf_decrypt(
      candidate.data(), &plaintext_bytes, nullptr, packet.data() + kWireSequenceBytes,
      ciphertext_bytes, aad.data(), aad.size(), nonce.data(), receive_key_.data());
  if (result != 0 || plaintext_bytes != plaintext_capacity) {
    sodium_memzero(candidate.data(), candidate.size());
    return false;
  }
  CommitSequence(sequence);
  plaintext = std::move(candidate);
  return true;
}

bool SecureChannel::CanAcceptSequence(uint64_t sequence) const {
  if (!receive_sequence_initialized_ || sequence > highest_receive_sequence_) return true;
  const uint64_t behind = highest_receive_sequence_ - sequence;
  if (behind >= kSecureReplayWindowBits) return false;
  const size_t word = static_cast<size_t>(behind / 64);
  const unsigned bit = static_cast<unsigned>(behind % 64);
  return (received_sequences_[word] & (uint64_t{1} << bit)) == 0;
}

void SecureChannel::CommitSequence(uint64_t sequence) {
  if (!receive_sequence_initialized_) {
    received_sequences_.fill(0);
    highest_receive_sequence_ = sequence;
    receive_sequence_initialized_ = true;
  } else if (sequence > highest_receive_sequence_) {
    const uint64_t delta = sequence - highest_receive_sequence_;
    if (delta >= kSecureReplayWindowBits) {
      received_sequences_.fill(0);
    } else {
      const size_t word_shift = static_cast<size_t>(delta / 64);
      const unsigned bit_shift = static_cast<unsigned>(delta % 64);
      std::array<uint64_t, kSecureReplayWindowBits / 64> shifted{};
      for (size_t destination = word_shift; destination < received_sequences_.size(); ++destination) {
        const size_t source = destination - word_shift;
        shifted[destination] |= received_sequences_[source] << bit_shift;
        if (bit_shift && source > 0)
          shifted[destination] |= received_sequences_[source - 1] >> (64 - bit_shift);
      }
      received_sequences_ = shifted;
    }
    highest_receive_sequence_ = sequence;
  }
  const uint64_t behind = highest_receive_sequence_ - sequence;
  received_sequences_[static_cast<size_t>(behind / 64)] |= uint64_t{1} << (behind % 64);
}

void SecureChannel::Clear() {
  sodium_memzero(transmit_key_.data(), transmit_key_.size());
  sodium_memzero(receive_key_.data(), receive_key_.size());
  sodium_memzero(nonce_prefix_.data(), nonce_prefix_.size());
  session_id_ = next_send_sequence_ = highest_receive_sequence_ = 0;
  received_sequences_.fill(0);
  receive_sequence_initialized_ = false;
  ready_ = false;
}

}  // namespace edf::net
