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

// In-memory view of the (single) local account.
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

class AccountManager : public QObject {
    Q_OBJECT
public:
    explicit AccountManager(Storage *storage, QObject *parent = nullptr);

    bool hasAccount();
    const Profile &profile() const { return m_profile; }

    // UC-01: create account (keygen, fingerprint, encrypted keystore, default profile).
    // Emits created/failed through return value; E1 -> returns false with *error.
    bool createAccount(const QString &desiredNickname /*may be empty -> default*/, QString *error);
    // UC-01 A1: load existing account; fails if keystore cannot be decrypted.
    bool loadAccount(QString *error);
    // UC-03: validate (2..32 chars) + persist nickname/avatar; avatar file is
    // copied into the data dir, rendered circular, hashed for broadcast.
    bool updateProfile(const QString &nickname, const QString &avatarSourcePath, QString *error);

    // UC-02 steps 5-6 (local part): secure-erase private key, remove identity rows.
    bool destroyAccount(bool wipeHistoryAndFiles, QString *error);

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
    bool persistKeystore(const Bytes &keystoreBlob, QString *error);
    Bytes kek() const;

    Storage *m_storage = nullptr;
    Profile m_profile;
    Bytes m_edPriv;   // decrypted session-private key material (memory only)
    QString m_tlsKeyPem;
};

} // namespace meshlink
