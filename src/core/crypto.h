// deepin-MeshLink — core cryptographic primitives (OpenSSL 3.x).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
//
// Implements the primitives required by NFR-11..NFR-16:
//   * Ed25519 identity key pair  -> account fingerprint + heartbeat/TLS proof signatures
//   * X25519 key agreement pair  -> generated for completeness / future E2E ratcheting
//   * ECDSA P-256 self-signed cert -> TLS 1.3 transport identity (NFR-16)
//   * SHA-256 / hex / HMAC
//   * AES-256-GCM at-rest encryption of private keys (BR-02a)
// The module is deliberately Qt-free so it can be unit-tested and reused.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace meshlink {

using Bytes = std::vector<uint8_t>;

// A 32-byte lowercase hex account fingerprint, derived from the Ed25519 public key.
struct Identity {
    Bytes ed25519Pub;   // 32 bytes raw public key
    Bytes ed25519Priv;  // 32 bytes raw private key (never leaves keystore in plaintext)
    Bytes x25519Pub;    // 32 bytes
    Bytes x25519Priv;   // 32 bytes
    std::string fingerprint; // SHA256(ed25519Pub DER)[0:32] hex
};

// PEM material for the node's TLS listener.
struct TlsMaterial {
    std::string certPem; // self-signed ECDSA P-256 certificate, CN == fingerprint
    std::string keyPem;  // PKCS#8 PEM private key
};

class Crypto {
public:
    // ---- hashing ----
    static Bytes sha256(const uint8_t *data, size_t len);
    static Bytes sha256(const Bytes &data);
    static std::string sha256Hex(const uint8_t *data, size_t len);
    static std::string sha256Hex(const Bytes &data);
    static Bytes hmacSha256(const Bytes &key, const Bytes &msg);

    // Account fingerprint = first 32 hex chars of SHA-256 over the Ed25519
    // public key in DER (SubjectPublicKeyInfo) form. BR-01 / NFR-12.
    static std::string fingerprintFromEd25519Pub(const Bytes &rawPub32);

    // ---- asymmetric keys ----
    static std::optional<Identity> generateIdentity();
    static bool ed25519Sign(const Bytes &priv32, const Bytes &msg, Bytes &outSig);
    static bool ed25519Verify(const Bytes &pub32, const Bytes &msg, const Bytes &sig);

    // X25519 shared secret (used for optional application-layer key derivation).
    static std::optional<Bytes> x25519Shared(const Bytes &priv, const Bytes &peerPub);

    // Generate a self-signed TLS certificate (ECDSA P-256 / SHA-256). The
    // account fingerprint is bound into the subject CN so transport identity
    // and account identity are cryptographically linked (NFR-12/16).
    static std::optional<TlsMaterial> generateSelfSignedTls(const std::string &commonName);

    // PEM certificate -> SHA-256 fingerprint of the DER encoding (for TOFU pinning).
    static std::string certPemFingerprintHex(const std::string &certPem);

    // ---- AES-256-GCM keystore ----
    // Output layout: salt(16) || iv(12) || ciphertext || tag(16).
    static Bytes aesGcmEncrypt(const Bytes &plaintext, const Bytes &kek);
    static std::optional<Bytes> aesGcmDecrypt(const Bytes &blob, const Bytes &kek);

    // Derive a 32-byte key-encryption-key from a secret factor and a random
    // per-install salt (HKDF-like: SHA256(SHA256(factor || salt) || salt)).
    // Current keystores pass the per-install random device secret so the KEK no
    // longer depends on machine-id, hostname, MAC or IP. Legacy keystores used
    // the machine identity as the factor; the migration path still derives that
    // old KEK to re-encrypt them (see AccountManager::loadAccount).
    static Bytes deriveKek(const Bytes &factor, const Bytes &salt);

    // Random bytes helper (CSPRNG).
    static Bytes randomBytes(size_t n);
    static std::string randomHex(size_t bytes);

    static bool available();      // OpenSSL present & usable
    static std::string version(); // runtime OpenSSL version string
};

} // namespace meshlink
