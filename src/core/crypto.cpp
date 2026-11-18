// deepin-MeshLink — core cryptographic primitives implementation.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "crypto.h"

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/ossl_typ.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cstring>

namespace meshlink {
namespace {

struct ClearError {
    ~ClearError() { ERR_clear_error(); }
};

std::string toHex(const uint8_t *d, size_t n) {
    static const char *H = "0123456789abcdef";
    std::string s;
    s.resize(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s[2 * i] = H[d[i] >> 4];
        s[2 * i + 1] = H[d[i] & 0xF];
    }
    return s;
}

// Wrap a raw EVP_PKEY into DER SubjectPublicKeyInfo bytes.
Bytes pubKeyDer(EVP_PKEY *pkey) {
    Bytes out;
    unsigned char *der = nullptr;
    int len = i2d_PUBKEY(pkey, &der);
    if (len > 0 && der) {
        out.assign(der, der + len);
        OPENSSL_free(der);
    }
    return out;
}

EVP_PKEY *ed25519FromRaw(const Bytes &raw) {
    if (raw.size() != 32)
        return nullptr;
    return EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, raw.data(), raw.size());
}

} // namespace

Bytes Crypto::sha256(const uint8_t *data, size_t len) {
    Bytes out(SHA256_DIGEST_LENGTH);
    SHA256(data, len, out.data());
    return out;
}
Bytes Crypto::sha256(const Bytes &d) { return sha256(d.data(), d.size()); }
std::string Crypto::sha256Hex(const uint8_t *d, size_t n) { return toHex(sha256(d, n).data(), SHA256_DIGEST_LENGTH); }
std::string Crypto::sha256Hex(const Bytes &d) { return sha256Hex(d.data(), d.size()); }

Bytes Crypto::hmacSha256(const Bytes &key, const Bytes &msg) {
    Bytes out(EVP_MAX_MD_SIZE);
    unsigned int len = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), msg.data(), msg.size(), out.data(), &len);
    out.resize(len);
    return out;
}

std::string Crypto::fingerprintFromEd25519Pub(const Bytes &rawPub32) {
    EVP_PKEY *p = ed25519FromRaw(rawPub32);
    if (!p)
        return {};
    Bytes der = pubKeyDer(p);
    EVP_PKEY_free(p);
    std::string full = sha256Hex(der);
    if (full.size() > 32)
        full.resize(32); // first 32 hex chars (NFR-12)
    return full;
}

std::optional<Identity> Crypto::generateIdentity() {
    ClearError ce;
    Identity id;

    // Ed25519 (signing / identity)
    EVP_PKEY_CTX *ectx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    if (!ectx)
        return std::nullopt;
    EVP_PKEY *ed = nullptr;
    bool ok = EVP_PKEY_keygen_init(ectx) == 1 && EVP_PKEY_keygen(ectx, &ed) == 1;
    EVP_PKEY_CTX_free(ectx);
    if (!ok || !ed)
        return std::nullopt;

    size_t n = 32;
    id.ed25519Pub.resize(32);
    id.ed25519Priv.resize(32);
    if (EVP_PKEY_get_raw_public_key(ed, id.ed25519Pub.data(), &n) != 1 || n != 32) {
        EVP_PKEY_free(ed);
        return std::nullopt;
    }
    n = 32;
    if (EVP_PKEY_get_raw_private_key(ed, id.ed25519Priv.data(), &n) != 1 || n != 32) {
        EVP_PKEY_free(ed);
        return std::nullopt;
    }
    EVP_PKEY_free(ed);

    // X25519 (key agreement)
    EVP_PKEY_CTX *xctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
    if (!xctx)
        return std::nullopt;
    EVP_PKEY *xk = nullptr;
    ok = EVP_PKEY_keygen_init(xctx) == 1 && EVP_PKEY_keygen(xctx, &xk) == 1;
    EVP_PKEY_CTX_free(xctx);
    if (!ok || !xk)
        return std::nullopt;
    id.x25519Pub.resize(32);
    id.x25519Priv.resize(32);
    n = 32;
    if (EVP_PKEY_get_raw_public_key(xk, id.x25519Pub.data(), &n) != 1 || n != 32) {
        EVP_PKEY_free(xk);
        return std::nullopt;
    }
    n = 32;
    if (EVP_PKEY_get_raw_private_key(xk, id.x25519Priv.data(), &n) != 1 || n != 32) {
        EVP_PKEY_free(xk);
        return std::nullopt;
    }
    EVP_PKEY_free(xk);

    id.fingerprint = fingerprintFromEd25519Pub(id.ed25519Pub);
    if (id.fingerprint.empty())
        return std::nullopt;
    return id;
}

bool Crypto::ed25519Sign(const Bytes &priv32, const Bytes &msg, Bytes &outSig) {
    ClearError ce;
    if (priv32.size() != 32)
        return false;
    EVP_PKEY *p = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, priv32.data(), priv32.size());
    if (!p)
        return false;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    bool ok = false;
    size_t siglen = 0;
    if (ctx && EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, p) == 1) {
        if (EVP_DigestSign(ctx, nullptr, &siglen, msg.data(), msg.size()) == 1) {
            outSig.resize(siglen);
            if (EVP_DigestSign(ctx, outSig.data(), &siglen, msg.data(), msg.size()) == 1) {
                outSig.resize(siglen);
                ok = true;
            }
        }
    }
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(p);
    return ok;
}

bool Crypto::ed25519Verify(const Bytes &pub32, const Bytes &msg, const Bytes &sig) {
    ClearError ce;
    EVP_PKEY *p = ed25519FromRaw(pub32);
    if (!p)
        return false;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    bool ok = false;
    if (ctx && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, p) == 1)
        ok = EVP_DigestVerify(ctx, sig.data(), sig.size(), msg.data(), msg.size()) == 1;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(p);
    return ok;
}

std::optional<Bytes> Crypto::x25519Shared(const Bytes &priv, const Bytes &peerPub) {
    ClearError ce;
    if (priv.size() != 32 || peerPub.size() != 32)
        return std::nullopt;
    EVP_PKEY *p = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, priv.data(), priv.size());
    EVP_PKEY *q = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, peerPub.data(), peerPub.size());
    if (!p || !q) {
        if (p) EVP_PKEY_free(p);
        if (q) EVP_PKEY_free(q);
        return std::nullopt;
    }
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(p, nullptr);
    Bytes secret(32);
    size_t len = 32;
    bool ok = ctx && EVP_PKEY_derive_init(ctx) == 1 && EVP_PKEY_derive_set_peer(ctx, q) == 1
              && EVP_PKEY_derive(ctx, secret.data(), &len) == 1;
    if (ctx) EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(p);
    EVP_PKEY_free(q);
    if (!ok)
        return std::nullopt;
    secret.resize(len);
    return secret;
}

std::optional<TlsMaterial> Crypto::generateSelfSignedTls(const std::string &commonName) {
    ClearError ce;
    // ECDSA P-256 key
    EVP_PKEY_CTX *kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    if (!kctx)
        return std::nullopt;
    EVP_PKEY *ec = nullptr;
    bool ok = EVP_PKEY_keygen_init(kctx) == 1 && EVP_PKEY_CTX_set_ec_paramgen_curve_nid(kctx, NID_X9_62_prime256v1) == 1
              && EVP_PKEY_CTX_set_ec_param_enc(kctx, OPENSSL_EC_NAMED_CURVE) == 1 && EVP_PKEY_keygen(kctx, &ec) == 1;
    EVP_PKEY_CTX_free(kctx);
    if (!ok || !ec) {
        if (ec) EVP_PKEY_free(ec);
        return std::nullopt;
    }

    X509 *x = X509_new();
    if (!x) {
        EVP_PKEY_free(ec);
        return std::nullopt;
    }
    X509_SET:;
    // self-signed: issuer == subject
    X509_NAME *name = X509_NAME_new();
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char *>(commonName.c_str()), -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char *>("deepin-MeshLink"), -1, -1, 0);
    X509_set_issuer_name(x, name);
    X509_set_subject_name(x, name);
    X509_NAME_free(name);

    X509_gmtime_adj(X509_getm_notBefore(x), 0);
    X509_gmtime_adj(X509_getm_notAfter(x), 60L * 60 * 24 * 3650); // 10y ~ account lifetime
    ASN1_INTEGER *serial = X509_get_serialNumber(x);
    ASN1_INTEGER_set_uint64(serial, static_cast<uint64_t>(0x4d4c5a) ^ static_cast<uint64_t>(::time(nullptr)));
    X509_set_pubkey(x, ec);

    // Mark as CA:false, basic constraints.
    X509V3_CTX extctx;
    X509V3_set_ctx_nodb(&extctx);
    X509V3_set_ctx(&extctx, x, x, nullptr, nullptr, 0);
    X509_EXTENSION *bc = X509V3_EXT_conf_nid(nullptr, &extctx, NID_basic_constraints, "critical,CA:FALSE");
    if (bc) {
        X509_add_ext(x, bc, -1);
        X509_EXTENSION_free(bc);
    }
    std::string san = "URI:meshlink://" + commonName;
    X509_EXTENSION *sanExt = X509V3_EXT_conf_nid(nullptr, &extctx, NID_subject_alt_name, san.c_str());
    if (sanExt) {
        X509_add_ext(x, sanExt, -1);
        X509_EXTENSION_free(sanExt);
    }

    if (X509_sign(x, ec, EVP_sha256()) == 0) {
        X509_free(x);
        EVP_PKEY_free(ec);
        return std::nullopt;
    }

    TlsMaterial tm;
    // cert PEM
    {
        BIO *b = BIO_new(BIO_s_mem());
        PEM_write_bio_X509(b, x);
        char *data = nullptr;
        long len = BIO_get_mem_data(b, &data);
        tm.certPem.assign(data, data + len);
        BIO_free(b);
    }
    // key PEM (PKCS#8)
    {
        BIO *b = BIO_new(BIO_s_mem());
        PEM_write_bio_PrivateKey(b, ec, nullptr, nullptr, 0, nullptr, nullptr);
        char *data = nullptr;
        long len = BIO_get_mem_data(b, &data);
        tm.keyPem.assign(data, data + len);
        BIO_free(b);
    }
    X509_free(x);
    EVP_PKEY_free(ec);
    return tm;
}

std::string Crypto::certPemFingerprintHex(const std::string &certPem) {
    ClearError ce;
    BIO *b = BIO_new_mem_buf(certPem.data(), static_cast<int>(certPem.size()));
    X509 *x = PEM_read_bio_X509(b, nullptr, nullptr, nullptr);
    BIO_free(b);
    if (!x)
        return {};
    unsigned char md[SHA256_DIGEST_LENGTH];
    unsigned int n = 0;
    X509_digest(x, EVP_sha256(), md, &n);
    X509_free(x);
    return toHex(md, n);
}

Bytes Crypto::randomBytes(size_t n) {
    Bytes b(n);
    if (n)
        RAND_bytes(b.data(), static_cast<int>(n));
    return b;
}

std::string Crypto::randomHex(size_t bytes) {
    Bytes b = randomBytes(bytes);
    static const char *H = "0123456789abcdef";
    std::string s;
    s.resize(bytes * 2);
    for (size_t i = 0; i < bytes; ++i) {
        s[2 * i] = H[b[i] >> 4];
        s[2 * i + 1] = H[b[i] & 0xF];
    }
    return s;
}

Bytes Crypto::deriveKek(const Bytes &factor, const Bytes &salt) {
    // HKDF-like: KEK = SHA256(SHA256(factor || salt) || salt)
    Bytes joined = factor;
    joined.insert(joined.end(), salt.begin(), salt.end());
    Bytes h1 = sha256(joined);
    Bytes h1s = h1;
    h1s.insert(h1s.end(), salt.begin(), salt.end());
    return sha256(h1s); // 32 bytes
}

Bytes Crypto::aesGcmEncrypt(const Bytes &plaintext, const Bytes &kek) {
    ClearError ce;
    if (kek.size() != 32)
        return {};
    Bytes salt = randomBytes(16);
    Bytes iv = randomBytes(12);
    Bytes ct(plaintext.size());
    int outl = 0, tmpl = 0;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return {};
    bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
              && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) == 1
              && EVP_EncryptInit_ex(ctx, nullptr, nullptr, kek.data(), iv.data()) == 1
              && (plaintext.empty() || EVP_EncryptUpdate(ctx, ct.data(), &outl, plaintext.data(),
                                                         static_cast<int>(plaintext.size())) == 1)
              && EVP_EncryptFinal_ex(ctx, ct.data() + outl, &tmpl) == 1;
    if (!plaintext.empty()) {
        // outl already includes update; final appends tag-ready remainder
    }
    ct.resize(outl + tmpl);
    Bytes tag(16);
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag.data()) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok)
        return {};
    Bytes blob;
    blob.reserve(salt.size() + iv.size() + ct.size() + tag.size());
    blob.insert(blob.end(), salt.begin(), salt.end());
    blob.insert(blob.end(), iv.begin(), iv.end());
    blob.insert(blob.end(), ct.begin(), ct.end());
    blob.insert(blob.end(), tag.begin(), tag.end());
    return blob;
}

std::optional<Bytes> Crypto::aesGcmDecrypt(const Bytes &blob, const Bytes &kek) {
    ClearError ce;
    if (kek.size() != 32 || blob.size() < 16 + 12 + 16)
        return std::nullopt;
    const uint8_t *p = blob.data();
    Bytes iv(p + 16, p + 16 + 12);
    size_t ctLen = blob.size() - 16 - 12 - 16;
    const uint8_t *ct = p + 16 + 12;
    const uint8_t *tag = ct + ctLen;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return std::nullopt;
    Bytes out(ctLen);
    int outl = 0, tmpl = 0;
    bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
              && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) == 1
              && EVP_DecryptInit_ex(ctx, nullptr, nullptr, kek.data(), iv.data()) == 1
              && (ctLen == 0 || EVP_DecryptUpdate(ctx, out.data(), &outl, ct, static_cast<int>(ctLen)) == 1)
              && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, const_cast<uint8_t *>(tag)) == 1
              && EVP_DecryptFinal_ex(ctx, out.data() + outl, &tmpl) == 1;
    out.resize(outl + tmpl);
    EVP_CIPHER_CTX_free(ctx);
    if (!ok)
        return std::nullopt;
    return out;
}

bool Crypto::available() {
    return OpenSSL_version_num() >= 0x30000000L;
}
std::string Crypto::version() { return OpenSSL_version(OPENSSL_VERSION); }

} // namespace meshlink
