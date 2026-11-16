// deepin-MeshLink — TLS 1.3 transport with TOFU + Ed25519 channel binding.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
//
// NFR-10: every TCP connection is TLS 1.3 (QSslSocket); no plaintext TCP.
// NFR-13: TOFU pinning of each peer's TLS certificate fingerprint, keyed by
//         account fingerprint; mismatch => suspected MITM, connection aborted.
// NFR-16: the self-signed cert carries CN == account fingerprint, and after
//         the handshake both directions prove possession of the *account*
//         Ed25519 key with a signature bound to this TLS session
//         ("ML-A1" || fp || nonce || peerCertFingerprint).
#pragma once

#include "account.h"
#include "common.h"
#include "protocol.h"
#include "storage.h"

#include <QObject>
#include <QList>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QTcpServer>
#include <QTimer>

#include <functional>

namespace meshlink {

// One encrypted peer connection (pre-auth or authenticated).
class SecureChannel : public QObject {
    Q_OBJECT
public:
    // `expectedFp` (from heartbeat claim) may be empty for inbound channels;
    // it is then learned from the AUTH frame. tofuCheck must return true when
    // the pin is new/consistent (NFR-13); on false the channel aborts.
    using TofuCheck = std::function<bool(const QString &fp, const QString &certFp, QString *why)>;
    SecureChannel(QSslSocket *sock, AccountManager *acct, const QString &expectedFp, TofuCheck tofuCheck);

    QString peerFingerprint() const { return m_peerFp; }
    QString peerNickname() const { return m_peerNick; }
    QString peerAvatarHash() const { return m_peerAvatar; }
    QByteArray peerEd25519Pub() const { return m_peerPub; }
    bool authenticated() const { return m_authenticated; }
    QString peerAddress() const;

    void send(MsgType type, const QByteArray &payload);
    void close();

signals:
    void authenticatedReady(SecureChannel *ch);
    void messageReceived(SecureChannel *ch, MsgType type, const QByteArray &payload);
    void destroyed(SecureChannel *ch, const QString &reason);

private:
    void onEncrypted();
    void onReadyRead();
    void pump();
    void handleAuth(const QByteArray &payload);
    void tryFinish();
    void fail(const QString &reason); // emits destroyed + closes

    QSslSocket *m_sock = nullptr;
    AccountManager *m_acct = nullptr;
    QString m_expectedFp;
    TofuCheck m_tofu;
    QByteArray m_in;
    QByteArray m_localNonce;
    QString m_peerCertFp, m_localCertFp;
    bool m_authenticated = false;
    bool m_sentAuth = false;
    bool m_peerAuthSeen = false;
    bool m_finished = false;
    QString m_peerFp, m_peerNick, m_peerAvatar;
    QByteArray m_peerPub;
    QTimer m_authTimer;
};

// Owns the listening socket + all live channels; performs TOFU checks.
class SecureLink : public QObject {
    Q_OBJECT
public:
    SecureLink(AccountManager *acct, Storage *storage, QObject *parent = nullptr);
    ~SecureLink() override;

    bool listen(quint16 port, QString *error); // TLS node server (UC-04 TCP side)
    void stopListening();
    quint16 port() const { return m_port; }

    // Outbound authenticated channel to peer ip:port with expected fp.
    // Exactly one of the callbacks fires; `owner` keeps both alive while used.
    void connectTo(const QString &fp, const QString &ip, quint16 port,
                   QObject *owner,
                   std::function<void(SecureChannel *)> onReady,
                   std::function<void(const QString &)> onFail);

    // Reuse any authenticated channel to fp (control-plane messages).
    SecureChannel *channelTo(const QString &fp) const;
    QList<SecureChannel *> channels() const { return m_all; }

signals:
    void incoming(SecureChannel *ch); // new authenticated server-side channel
    void channelLost(SecureChannel *ch, const QString &fp); // for higher layers
    void mitmSuspected(const QString &fp, const QString &detail); // UC-06 E3 / NFR-13

private slots:
    void serveConnection(qintptr fd);

private:
    QSslConfiguration makeConfig();
    void adopt(SecureChannel *ch);
    bool tofuCheck(const QString &fp, const QString &certFp, QString *why);

    AccountManager *m_acct;
    Storage *m_storage;
    QTcpServer *m_server = nullptr;
    quint16 m_port = 0;
    QList<SecureChannel *> m_all;
};

} // namespace meshlink
