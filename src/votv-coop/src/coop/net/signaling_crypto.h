// coop/net/signaling_crypto.h -- end-to-end encryption for the signaling channel.
//
// DESIGN (the whole contract, so a reviewer never has to reconstruct it):
//
// What is encrypted: the <hex-payload> of signaling lines only -- the opaque ICE blob GNS
// produces in SendSignal and consumes in ReceivedP2PCustomSignal. Routing headers (the
// <dest-identity> we address, the <sender-identity> the relay stamps) stay plaintext: the
// relay must still route. The greeting, the nonce/auth registration proof and the
// self-addressed echo probe stay plaintext: they carry no ICE content, and the echo probe's
// empty payload is the liveness signal the dispatch path keys on. A relay or wire observer
// learns who signals whom, when, and how much -- never the ICE contents, which is what would
// let it MITM the P2P handshake.
//
// Key agreement: static ECDH, NON-interactive. Every peer's rendezvous identity IS its
// Ed25519 public key (`gen:<64 hex>`), proved to the relay at registration, so both sides
// already hold each other's public key and their own secret with no round trip. The Ed25519
// keys are converted to X25519 (libsodium's documented conversion: the private scalar is the
// clamped low half of SHA-512 over the 32-byte seed; the public u-coordinate is (1+y)/(1-y)),
// one X25519 scalar multiplication gives the shared secret S on both sides, and the session
// key is K = SHA-256("multivoid-signaling-enc-v1" || id_lo || id_hi || S) with the two
// 32-byte Ed25519 public keys in lexicographic order (binding the key to the pair, which
// blocks unknown-key-share confusion). Consequence: the FIRST ICE message is already
// encrypted -- there is no plaintext phase and no negotiation round trip. The task's premise
// ("the first messages cannot be encrypted before agreement") is resolved by observing the
// agreement needs no messages at all.
//
// Per message: ChaCha20-Poly1305 (RFC 8439), empty AAD, a fresh random 96-bit nonce per
// message (BCryptGenRandom, prepended -- stateless, no per-peer counters). Envelope bytes:
//   'M' 'V' || 0x01 || nonce[12] || ciphertext || tag[16]
// The Poly1305 tag, not the magic, is the real discriminator: inbound, anything that is not
// a well-formed envelope or fails authentication is DROPPED (fail closed), never handed to
// GNS.
//
// Backward compatibility: CLEAN REFUSAL, no downgrade. A new build drops plaintext inbound
// signaling with a log line naming the cause; an old build receiving an envelope hands
// garbage to its ICE layer and the connect fails there. Mixed-version pairs cannot complete
// P2P, by design: silently accepting plaintext would re-expose ICE to the relay whenever the
// relay chose to strip it. Both peers must run an encryption-capable build.
//
// What this does NOT give (stated, not hidden):
// - No forward secrecy: static ECDH means a future compromise of a peer's durable key
//   decrypts recorded traffic. The identity keys are already long-term; this matches the
//   existing posture rather than weakening it.
// - No replay protection: no per-message sequence numbers (that would need per-peer
//   persistent state). GNS/ICE tolerates duplicate signaling; a replayed message verifies
//   and is delivered again.
// - The relay can still drop, delay, or replay lines, and learns the signaling graph.
//
// Primitives: hand-rolled X25519 (RFC 7748 Montgomery ladder), Ed25519->X25519 conversion,
// ChaCha20-Poly1305 (RFC 8439); SHA-512 and the RNG come from Windows CNG (bcrypt), which
// GNS's BCrypt backend already links -- the same no-new-dependency rule peer_identity.cpp
// follows (Ed25519 itself stays the donna TU GNS compiles in). Every primitive is pinned to
// its RFC test vector in RunSelftest, in the numbered style of peer_identity's selftest.
// This file and its .cpp are the ONLY place these primitives live; SignalingClient calls
// Seal/Open and never sees a scalar.
//
// DEPENDENCY (resolved 2026-10-13): DeriveSessionKey needs our own Ed25519 seed, via
// peer_identity::LocalPrivateSeed() (added alongside this module).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace coop::net::signaling_crypto {

// A pairwise signaling-encryption key. Zeroed by the holder when done; never logged.
struct SessionKey {
    uint8_t bytes[32]{};
};

// Derive the session key for talking to `peerIdentity` ("gen:<64 hex>"). Deterministic:
// both peers derive the same bytes without exchanging anything. False when the identity is
// malformed, our own key is not loaded, or the DH result is degenerate (all-zero) -- the
// caller must then refuse to signal, never fall back to plaintext.
bool DeriveSessionKey(const std::string& peerIdentity, SessionKey& out);

// Seal `pt` into a fresh envelope (random nonce). False only when the OS refused randomness.
bool Seal(const SessionKey& key, const uint8_t* pt, size_t ptLen, std::string& envelopeOut);

// Open an envelope: verify the Poly1305 tag, then decrypt. False for anything that is not a
// well-formed, authentic envelope -- a malformed line, a wrong version, a failed tag. The
// caller drops the line and logs; it never reaches GNS.
bool Open(const SessionKey& key, const uint8_t* env, size_t envLen, std::string& ptOut);

// Numbered selftest in the peer_identity style: RFC 7748 (X25519), RFC 8439 (ChaCha20 block,
// Poly1305, AEAD), the Ed25519->X25519 conversion round-trip, and envelope open/seal
// properties including tamper rejection. True when every check passes. Pure: no sockets,
// no keys, no files.
bool RunSelftest();

}  // namespace coop::net::signaling_crypto
