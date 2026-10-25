#pragma once

#include "protocol.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace edf::net {

inline constexpr size_t kSecureEnvelopeBytes = 8 + 16;
inline constexpr size_t kMaxSecurePayloadBytes = kMaxPacketBytes - kSecureEnvelopeBytes;
inline constexpr size_t kSecureReplayWindowBits = 1024;

using CryptoPublicKey = std::array<uint8_t, 32>;
using CryptoSecretKey = std::array<uint8_t, 32>;
using CryptoSessionKey = std::array<uint8_t, 32>;
using CryptoNonce = std::array<uint8_t, 32>;
using CryptoNoncePrefix = std::array<uint8_t, 16>;
using CryptoSigningSecretKey = std::array<uint8_t, 64>;
using CryptoSigningSignature = std::array<uint8_t, 64>;

struct CryptoSigningKeyPair {
  CryptoPublicKey public_key{};
  CryptoSigningSecretKey secret_key{};
};

struct CryptoKeyPair {
  CryptoPublicKey public_key{};
  CryptoSecretKey secret_key{};
};

struct CryptoSessionKeys {
  CryptoSessionKey transmit{};
  CryptoSessionKey receive{};
  uint64_t session_id = 0;
};

bool InitializeCrypto(std::string* error = nullptr);
bool GenerateCryptoKeyPair(CryptoKeyPair& key_pair);
bool GenerateCryptoNonce(CryptoNonce& nonce);
bool DeriveSigningKeyPair(std::span<const uint8_t, 32> seed, CryptoSigningKeyPair& key_pair);
bool SignHandshakeTranscript(const CryptoSigningKeyPair& key_pair,
                             const CryptoPublicKey& client_public_key,
                             const CryptoPublicKey& server_public_key,
                             const CryptoNonce& client_nonce,
                             const CryptoNonce& server_nonce,
                             uint64_t session_id, uint64_t challenge_nonce,
                             CryptoSigningSignature& signature);
bool VerifyHandshakeTranscript(const CryptoPublicKey& identity_public_key,
                               const CryptoPublicKey& client_public_key,
                               const CryptoPublicKey& server_public_key,
                               const CryptoNonce& client_nonce,
                               const CryptoNonce& server_nonce,
                               uint64_t session_id, uint64_t challenge_nonce,
                               const CryptoSigningSignature& signature);
bool VerifyOrRememberServerIdentity(std::string_view endpoint,
                                    const CryptoPublicKey& identity_public_key,
                                    bool& first_use, std::string* error = nullptr);
void ClearCryptoKeyPair(CryptoKeyPair& key_pair);
void ClearCryptoSigningKeyPair(CryptoSigningKeyPair& key_pair);
void ClearCryptoSessionKeys(CryptoSessionKeys& keys);
bool DeriveClientSessionKeys(const CryptoKeyPair& client,
                            const CryptoPublicKey& server_public_key,
                            CryptoSessionKeys& keys);
bool DeriveServerSessionKeys(const CryptoKeyPair& server,
                            const CryptoPublicKey& client_public_key,
                            CryptoSessionKeys& keys);
bool DeriveNoncePrefix(const CryptoNonce& client_nonce, const CryptoNonce& server_nonce,
                       uint64_t session_id, CryptoNoncePrefix& prefix);
std::string CryptoFingerprint(const CryptoPublicKey& public_key);

// Encrypts one complete EDF protocol message per ENet packet. The eight-byte
// wire header is a monotonic sequence; protocol version, ENet channel, session
// ID and sequence are authenticated as associated data. Directional keys come
// from crypto_kx, so a send counter is never shared between directions.
class SecureChannel {
 public:
  SecureChannel() = default;
  SecureChannel(const CryptoSessionKeys& keys, const CryptoNoncePrefix& nonce_prefix);
  ~SecureChannel();
  SecureChannel(const SecureChannel&) = delete;
  SecureChannel& operator=(const SecureChannel&) = delete;
  SecureChannel(SecureChannel&& other) noexcept;
  SecureChannel& operator=(SecureChannel&& other) noexcept;

  bool ready() const { return ready_; }
  std::vector<uint8_t> Seal(uint8_t channel, std::span<const uint8_t> plaintext);
  bool Open(uint8_t channel, std::span<const uint8_t> packet, std::vector<uint8_t>& plaintext);
  void Clear();

 private:
  bool CanAcceptSequence(uint64_t sequence) const;
  void CommitSequence(uint64_t sequence);

  CryptoSessionKey transmit_key_{};
  CryptoSessionKey receive_key_{};
  CryptoNoncePrefix nonce_prefix_{};
  uint64_t session_id_ = 0;
  uint64_t next_send_sequence_ = 0;
  uint64_t highest_receive_sequence_ = 0;
  std::array<uint64_t, kSecureReplayWindowBits / 64> received_sequences_{};
  bool receive_sequence_initialized_ = false;
  bool ready_ = false;
};

}  // namespace edf::net
