// deepin-MeshLink — local account: identity, keystore, profile (UC-01/02/03).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#pragma once

#include "common.h"
#include "crypto.h"
#include "storage.h"

#include <QPixmap>
#include <QString>

namespace meshlink {

// In-memory view of one local account (multi-account: the active one).
struct Profile {
    QString fingerprint;
    QString nickname;
    QString avatarPath; // local file of the shared avatar PNG
    QString avatarHash; // sha256 hex of avatar file (UC-03/BR-06, only hash is broadcast)
    QString lanIp;
    qint64 createdTs = 0;
    QByteArray ed25519Pub; // 32 raw bytes
    QByteArray x25519Pub;
    QString tlsCertPem;
    bool valid = false;
};

// Persisted account row summary (multi-account list views).
struct AccountEntry {
    qint64 id = 0;
    QString fingerprint;
    QString nickname;
    QString avatarHash;
    QString avatarPath;
    qint64 createdTs = 0;
};

class AccountManager : public QObject {
    Q_OBJECT
public:
    explicit AccountManager(Storage *storage, QObject *parent = nullptr);

    bool hasAccount();
    const Profile &profile() const { return m_profile; }

    // ---- multi-account surface ----
    // All local accounts, ordered by creation.
    QVector<AccountEntry> listAccounts() const;
    // Row id of the profile currently loaded into memory; 0 when none loaded.
    qint64 activeRowId() const { return m_activeId; }
    // UC-01: create a new identity row and load it into memory. Never touches
    // other accounts; the caller picks the active id through Settings.
    // Returns the new row id, or -1 on failure (E1 -> *error).
    qint64 createNewAccount(const QString &desiredNickname /*may be empty -> default*/,
                            QString *error);
    // UC-01 A1: load account `id`; fails if the keystore cannot be decrypted.
    bool loadAccount(qint64 id, QString *error);
    // Drop the loaded profile so the next loadAccount() actually reloads.
    void resetLoaded() { m_profile = Profile{}; m_activeId = 0; }

    // UC-02 steps 5-6 (local part): secure-erase the loaded identity, remove
    // its rows and (optionally) its rooms/history. Returns accounts left.
    int destroyAccount(bool wipeHistoryAndFiles, QString *error);
    // Same semantics for an arbitrary account id. The active row is reset from
    // memory; a non-active row leaves the loaded profile untouched. Returns the
    // checked number of accounts left, or -1 on failure (unknown id included).
    int destroyAccountById(qint64 id, bool wipeHistoryAndFiles, QString *error);
    // Same semantics for an arbitrary account (deleting a non-active one).
    bool deleteAccount(qint64 id, bool wipeHistoryAndFiles, QString *error);

    // UC-03: validate (2..32 chars) + persist nickname/avatar; avatar file is
    // copied into the data dir, rendered circular, hashed for broadcast.
    bool updateProfile(const QString &nickname, const QString &avatarSourcePath, QString *error);

    // UC-01 step 7: associate the current LAN address with the account (refreshed
    // whenever the interface changes; not broadcast as extra private info).
    void setLanIp(const QString &ip);

    // Ed25519 helpers bound to the loaded identity.
    bool sign(const QByteArray &msg, QByteArray *sigOut) const;
    QByteArray publicKey() const { return m_profile.ed25519Pub; }
    // Decrypted TLS private key PEM for this session (NFR-16 listener config).
    QString tlsKeyPem() const { return m_tlsKeyPem; }
    // Verify against an arbitrary peer public key.
    static bool verify(const QByteArray &ed25519Pub, const QByteArray &msg, const QByteArray &sig);

    // Deterministic avatar fallback for peers whose avatar is not (yet) fetched.
    static QPixmap makeIdenticon(const QString &seed, int size = 256);

    static bool validateNickname(const QString &nick, QString *why = nullptr);
    static QString defaultNickname();

signals:
    void profileChanged(const Profile &p);

private:
    // New keystore KEK: derived from the per-install random device secret
    // (<data>/keystore/device.key, created once) and the per-install salt.
    // Machine-id, hostname, MAC and IP are never part of this derivation.
    // Returns an empty vector when the local keystore is unavailable; callers
    // must fail cleanly instead of writing a machine-bound blob.
    Bytes kek() const;
    // Replace the encrypted keystore blob of account `id` in place (used by the
    // legacy machine-id keystore migration after a successful decrypt).
    bool rewriteKeystoreBlob(qint64 id, const Bytes &blob, QString *error);
    // Shared destroy path for the active and by-id variants. `active` controls
    // only the in-memory profile reset; the shared install secrets are removed
    // only after the checked remaining count reaches zero.
    int destroyAccountInternal(qint64 id, const QString &fingerprint, const QString &avatarPath,
                               bool active, bool wipeHistoryAndFiles, QString *error);
    // Legacy single-account builds stored avatars at avatars/current.png;
    // multi-account locations are avatars/<id>.png. Moves the file if needed.
    void ensureAvatarLocation(qint64 id);

    Storage *m_storage = nullptr;
    Profile m_profile;
    qint64 m_activeId = 0;
    Bytes m_edPriv;   // decrypted session-private key material (memory only)
    QString m_tlsKeyPem;
};

} // namespace meshlink
