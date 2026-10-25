# Multiplayer transport and security

## Transport choice

The game uses upstream ENet for gameplay UDP transport. ENet carries reliable
control and chat messages separately from unreliable movement snapshots. FiveM's
open source client and server also use ENet, with their own routing, connection
tokens, protocol, and game-specific behavior around it. Those Cfx layers are
not drop-in EDF libraries.

- [FiveM client ENet transport](https://github.com/citizenfx/fivem/blob/master/code/components/net/src/NetLibraryImplV2.cpp)
- [FiveM server ENet transport](https://github.com/citizenfx/fivem/blob/master/code/components/citizen-server-impl/src/GameServerNet.ENet.cpp)

Libsodium supplies cryptographic primitives. The Windows package carries the
upstream libsodium 1.0.22 runtime; Linux self-host builds use the distribution's
libsodium development package.

- [Libsodium key exchange](https://doc.libsodium.org/key_exchange)
- [Ed25519 signatures](https://doc.libsodium.org/public-key_cryptography/public-key_signatures)
- [XChaCha20-Poly1305 AEAD](https://doc.libsodium.org/secret-key_cryptography/aead/chacha20-poly1305)

## Gameplay handshake and packet protection

Protocol 20 keeps LAN discovery and directory registration compatible with the
existing browser while changing the direct gameplay handshake. The client sends
an ephemeral X25519 public key and a random nonce. The server replies with its
own ephemeral key, nonce, random session ID, and persistent Ed25519 public key.
The signature covers the ordered ephemeral keys, both random nonces, challenge
nonce, protocol version, and full port build fingerprint. The client rejects an
invalid signature or mismatched nonce before opening a session.

On first contact, the client saves the server signing-key fingerprint for the
canonical host and port in its per-user configuration directory. Later key
changes fail closed and show both fingerprints plus the pin file location. The
F5 connection status shows the full verified fingerprint. Trust-on-first-use
protects later connections from key substitution; it cannot prove the identity
of a server during the very first connection. Remove the pin only when the
operator confirms an expected key rotation.

After the signed challenge, both directions use separate `crypto_kx` session
keys and XChaCha20-Poly1305. Every subsequent gameplay packet, including the
encrypted Authenticate and Welcome messages, is sealed; there is no plaintext
fallback. The envelope adds a monotonic sequence number and authenticates the
protocol version, ENet channel, session ID, and sequence. A 1,024-packet replay
window accepts legitimate cross-channel reordering and commits a sequence only
after the authentication tag verifies. The envelope costs 24 bytes, leaving a
1,176-byte plaintext budget under the 1,200-byte ENet packet cap.

The server creates a 32-byte random seed in `identity.edfkey` on first start and
derives its Ed25519 signing key from that seed. Keep the file private and back
it up with the server data; losing it changes the fingerprint clients have
pinned. Docker Compose stores it in the persistent `server-state` volume while
mounting scripts read-only. `--identity-file` selects another path.

## Remaining multiplayer limits

The directory and LAN discovery messages remain unauthenticated. A directory
listing is only a hint; direct gameplay connections perform their own signed
handshake. The protocol does not have player accounts, bans, permissions, or
anti-cheat. The rendered F5 host publishes a copied pose for its own local
soldier, while remote clients report bounded transforms so EDF can retain
mission spawns and collision.
Mission combat, weapons, enemy state, and mission progress are not shared
authoritatively across machines. The optional guest-backed headless host route
remains experimental and has not been demonstrated to reach Mission 1.

Keep the directory under your control and only admit players you trust. The
secure transport protects gameplay packets and server identity after the first
connection; it does not make the simulation server-authoritative or provide
player identity.
