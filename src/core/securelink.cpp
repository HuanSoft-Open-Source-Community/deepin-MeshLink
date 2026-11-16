// deepin-MeshLink — secure transport implementation.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "securelink.h"

#include "crypto.h"

#include <QSslCertificate>
#include <cstdio>
#include <QSslKey>

namespace meshlink {

// ---- helpers ----
static QString certFingerprint(const QSslCertificate &c) {
    const QByteArray der = c.toDer();
    return QString::fromLatin1(Crypto::sha256Hex(reinterpret_cast<const uint8_t *>(der.constData()),
                                                 size_t(der.size())).c_str());
}
static QString certCN(const QSslCertificate &c) {
    const auto names = c.subjectInfo(QSslCertificate::CommonName);
    return names.isEmpty() ? QString() : names.first();
}
// Session-bound auth content both peers can reconstruct.
static QByteArray authContent(const QString &fp, const QByteArray &nonce, const QString &peerCertFp) {
    QByteArray c = "ML-A1";
    c += fp.toLatin1();
    c += nonce;
    c += peerCertFp.toLatin1();
    return c;
}

// ------------------------------------------------------------------
SecureChannel::SecureChannel(QSslSocket *sock, AccountManager *acct, const QString &expectedFp,
                             TofuCheck tofuCheck)
    : m_sock(sock), m_acct(acct), m_expectedFp(expectedFp), m_tofu(std::move(tofuCheck)) {
    m_sock->setParent(this);
    m_sock->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    Bytes nb = Crypto::randomBytes(16);
    m_localNonce = QByteArray(reinterpret_cast<const char *>(nb.data()), int(nb.size()));

    connect(m_sock, &QSslSocket::encrypted, this, &SecureChannel::onEncrypted);
    connect(m_sock, &QSslSocket::readyRead, this, &SecureChannel::onReadyRead);
    connect(m_sock, &QAbstractSocket::disconnected, this, [this] { fail(QStringLiteral("closed")); });
    connect(m_sock, &QAbstractSocket::errorOccurred, this, [this](QAbstractSocket::SocketError e) {
        if (m_finished)
            return;
        if (e == QAbstractSocket::SslHandshakeFailedError || e == QAbstractSocket::ConnectionRefusedError
            || e == QAbstractSocket::RemoteHostClosedError || e == QAbstractSocket::SocketTimeoutError)
            fail(m_sock->errorString());
    });
    // TLS errors for self-signed certs are expected; we authenticate in AUTH.
    connect(m_sock, &QSslSocket::sslErrors, this, [this](const QList<QSslError> &errs) {
        if (qEnvironmentVariableIsSet("MESHLINK_DEBUG"))
            for (const auto &e : errs)
                fprintf(stderr, "[tls %p] sslError: %s\n", this, qUtf8Printable(e.errorString()));
    });

    m_authTimer.setSingleShot(true);
    m_authTimer.setInterval(10000);
    connect(&m_authTimer, &QTimer::timeout, this, [this] {
        if (!m_authenticated)
            fail(QStringLiteral("auth timeout"));
    });
    m_authTimer.start();
}

QString SecureChannel::peerAddress() const { return m_sock->peerAddress().toString(); }

void SecureChannel::onEncrypted() {
    if (m_finished)
        return;
    if (m_sock->sessionProtocol() != QSsl::TlsV1_3) { // NFR-10
        fail(QStringLiteral("negotiated protocol is not TLS 1.3"));
        return;
    }
    const QSslCertificate peer = m_sock->peerCertificate();
    if (peer.isNull()) {
        fail(QStringLiteral("no peer certificate"));
        return;
    }
    m_peerCertFp = certFingerprint(peer);
    if (!m_expectedFp.isEmpty() && certCN(peer) != m_expectedFp) { // UC-06 E3
        fail(QStringLiteral("certificate CN does not match expected fingerprint"));
        return;
    }
    QSslCertificate localCert(m_acct->profile().tlsCertPem.toUtf8(), QSsl::Pem);
    m_localCertFp = certFingerprint(localCert);

    AuthMsg auth;
    auth.fp = m_acct->profile().fingerprint;
    auth.nickname = m_acct->profile().nickname;
    auth.avatarHash = m_acct->profile().avatarHash;
    auth.ed25519Pub = m_acct->publicKey();
    auth.nonce = m_localNonce;
    if (!m_acct->sign(authContent(auth.fp, auth.nonce, m_peerCertFp), &auth.sig)) {
        fail(QStringLiteral("cannot sign auth"));
        return;
    }
    send(MsgType::Hello, auth.toPayload());
    m_sentAuth = true;
    tryFinish();
}

void SecureChannel::handleAuth(const QByteArray &payload) {
    if (m_peerAuthSeen)
        return;
    AuthMsg auth;
    if (!AuthMsg::fromPayload(payload, auth) || auth.fp.isEmpty() || auth.ed25519Pub.size() != 32) {
        fail(QStringLiteral("malformed auth"));
        return;
    }
    // derived fingerprint must equal claimed (NFR-12)
    const QString derived =
        QString::fromStdString(Crypto::fingerprintFromEd25519Pub(Bytes(auth.ed25519Pub.begin(), auth.ed25519Pub.end())));
    if (derived != auth.fp) {
        fail(QStringLiteral("fingerprint mismatch: suspected MITM"));
        return;
    }
    if (!m_expectedFp.isEmpty() && m_expectedFp != auth.fp) {
        fail(QStringLiteral("peer identified as a different node"));
        return;
    }
    // cert CN binds certificate to the account fingerprint (NFR-16)
    if (certCN(m_sock->peerCertificate()) != auth.fp) {
        fail(QStringLiteral("cert/account binding failed"));
        return;
    }
    QByteArray content = authContent(auth.fp, auth.nonce, m_localCertFp);
    if (!Crypto::ed25519Verify(Bytes(auth.ed25519Pub.begin(), auth.ed25519Pub.end()),
                               Bytes(content.begin(), content.end()), Bytes(auth.sig.begin(), auth.sig.end()))) {
        fail(QStringLiteral("bad auth signature"));
        return;
    }
    QString why;
    if (m_tofu && !m_tofu(auth.fp, m_peerCertFp, &why)) {
        fail(why.isEmpty() ? QStringLiteral("TOFU mismatch") : why);
        return;
    }
    m_peerFp = auth.fp;
    m_peerNick = auth.nickname;
    m_peerAvatar = auth.avatarHash;
    m_peerPub = auth.ed25519Pub;
    m_peerAuthSeen = true;
    m_authenticated = true;
    tryFinish();
}

void SecureChannel::tryFinish() {
    if (m_finished || !m_sentAuth || !m_peerAuthSeen)
        return;
    m_finished = true;
    m_authTimer.stop();
    emit authenticatedReady(this);
}

void SecureChannel::onReadyRead() {
    m_in.append(m_sock->readAll());
    pump();
}

void SecureChannel::pump() {
    while (true) {
        MsgType type;
        QByteArray payload;
        int consumed = 0;
        if (!tryParseFrame(m_in, type, payload, consumed))
            break;
        m_in.remove(0, consumed);
        if (type == MsgType::Hello) {
            if (!m_peerAuthSeen)
                handleAuth(payload);
            continue;
        }
        if (!m_authenticated)
            continue; // drop pre-auth application frames
        emit messageReceived(this, type, payload);
    }
}

void SecureChannel::send(MsgType type, const QByteArray &payload) {
    if (m_sock->state() == QAbstractSocket::ConnectedState && m_sock->isEncrypted()) {
        m_sock->write(frameMessage(type, payload));
        // Push the frame out now: shutdown/destroy paths send best-effort
        // notifications (e.g. UC-02 leave, UC-13 dissolve) and immediately
        // tear the channel down afterwards.
        m_sock->flush();
    }
}

void SecureChannel::close() {
    if (m_sock->state() == QAbstractSocket::ConnectedState)
        m_sock->disconnectFromHost();
}

void SecureChannel::fail(const QString &reason) {
    if (qEnvironmentVariableIsSet("MESHLINK_DEBUG"))
        fprintf(stderr, "[tls %p] FAIL %s (enc=%d auth=%d peer=%s)\n", this, qUtf8Printable(reason),
                int(m_sock->isEncrypted()), int(m_authenticated), qUtf8Printable(peerAddress()));
    if (m_finished && !m_authenticated)
        return;
    m_finished = true;
    m_authTimer.stop();
    if (m_sock->state() == QAbstractSocket::ConnectedState)
        m_sock->abort();
    emit destroyed(this, reason);
}

// ------------------------------------------------------------------
SecureLink::SecureLink(AccountManager *acct, Storage *storage, QObject *parent)
    : QObject(parent), m_acct(acct), m_storage(storage) {}

SecureLink::~SecureLink() {
    stopListening();
}

static QSslConfiguration buildConfig(const QString &certPem, const QString &keyPem) {
    QSslConfiguration cfg;
    cfg.setProtocol(QSsl::TlsV1_3); // NFR-10: exactly TLS 1.3
    cfg.setLocalCertificate(QSslCertificate(certPem.toUtf8(), QSsl::Pem));
    cfg.setPrivateKey(QSslKey(keyPem.toUtf8(), QSsl::Ec, QSsl::Pem, QSsl::PrivateKey));
    // Self-signed, no CA: skip chain verification; the application AUTH
    // signature + TOFU pin provide real authentication. QueryPeer still
    // makes the server REQUEST the client certificate (TLS1.3 sends no
    // CertificateRequest under VerifyNone, which would kill the channel —
    // the AUTH cert-binding needs peerCertificate() on both ends).
    cfg.setPeerVerifyMode(QSslSocket::QueryPeer);
    return cfg;
}

QSslConfiguration SecureLink::makeConfig() { return buildConfig(m_acct->profile().tlsCertPem, m_acct->tlsKeyPem()); }

// Hand the raw accepted descriptor straight to a QSslSocket: stealing it via
// setSocketDescriptor(-1) from nextPendingConnection() races deleteLater()
// which FINs the connection before the TLS handshake even starts.
namespace {
class MeshTcpServer : public QTcpServer {
public:
    using QTcpServer::QTcpServer;
    std::function<void(qintptr)> onFd;

protected:
    void incomingConnection(qintptr socketDescriptor) override {
        if (onFd)
            onFd(socketDescriptor);
        else
            QTcpServer::incomingConnection(socketDescriptor);
    }
};
} // namespace

bool SecureLink::listen(quint16 port, QString *error) {
    m_server = new MeshTcpServer(this);
    if (!m_server->listen(QHostAddress::Any, port)) {
        if (error)
            *error = m_server->errorString();
        delete m_server;
        m_server = nullptr;
        return false;
    }
    m_port = port;
    static_cast<MeshTcpServer *>(m_server)->onFd = [this](qintptr fd) { serveConnection(fd); };
    return true;
}

void SecureLink::stopListening() {
    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }
    const auto snapshot = m_all;
    for (auto *ch : snapshot)
        ch->deleteLater();
    m_all.clear();
}

void SecureLink::adopt(SecureChannel *ch) {
    m_all.append(ch);
    connect(ch, &SecureChannel::destroyed, this, [this](SecureChannel *c, const QString &reason) {
        const QString fp = c->peerFingerprint();
        m_all.removeAll(c);
        c->deleteLater();
        if (!fp.isEmpty())
            emit channelLost(c, fp);
    });
    connect(ch, &SecureChannel::authenticatedReady, this, [this](SecureChannel *c) { emit incoming(c); });
}

void SecureLink::serveConnection(qintptr fd) {
    {
        auto *ssl = new QSslSocket;
        auto *ch = new SecureChannel(ssl, m_acct, QString() /* inbound: learn fp via AUTH */,
                                     [this](const QString &fp, const QString &certFp, QString *why) {
                                         return tofuCheck(fp, certFp, why);
                                     });
        adopt(ch);
        if (!ssl->setSocketDescriptor(fd)) {
            ch->deleteLater();
            return;
        }
        ssl->setSslConfiguration(makeConfig());
        ssl->startServerEncryption(); // Qt6: descriptor already in ConnectedState
    }
}

void SecureLink::connectTo(const QString &fp, const QString &ip, quint16 port, QObject *owner,
                           std::function<void(SecureChannel *)> onReady,
                           std::function<void(const QString &)> onFail) {
    if (auto *existing = channelTo(fp)) {
        onReady(existing);
        return;
    }
    auto *ssl = new QSslSocket;
    auto *ch = new SecureChannel(ssl, m_acct, fp, [this](const QString &p, const QString &certFp, QString *why) {
        return tofuCheck(p, certFp, why);
    });
    adopt(ch);
    // single-shot dispatch
    connect(ch, &SecureChannel::authenticatedReady, owner, [ch, onReady](SecureChannel *) { onReady(ch); },
            Qt::SingleShotConnection);
    connect(ch, &SecureChannel::destroyed, owner, [onFail](SecureChannel *, const QString &why) { onFail(why); },
            Qt::SingleShotConnection);
    ssl->setSslConfiguration(makeConfig());
    ssl->connectToHostEncrypted(ip, port);
}

SecureChannel *SecureLink::channelTo(const QString &fp) const {
    for (auto *ch : m_all)
        if (ch->authenticated() && ch->peerFingerprint() == fp)
            return ch;
    return nullptr;
}

bool SecureLink::tofuCheck(const QString &fp, const QString &certFp, QString *why) {
    QSqlQuery q = m_storage->query("SELECT cert_fp FROM tofu WHERE fingerprint=?");
    q.bindValue(0, fp);
    q.exec();
    if (q.next()) {
        const QString known = q.value(0).toString();
        if (known != certFp) {
            if (why)
                *why = QStringLiteral("目标节点证书指纹与首次信任记录不符，疑似中间人攻击");
            emit mitmSuspected(fp, QStringLiteral("cert %1 != pinned %2").arg(certFp.left(12), known.left(12)));
            return false;
        }
        return true;
    }
    QSqlQuery ins = m_storage->query("INSERT OR REPLACE INTO tofu (fingerprint,cert_fp,first_seen) VALUES (?,?,?)");
    ins.bindValue(0, fp);
    ins.bindValue(1, certFp);
    ins.bindValue(2, nowMs());
    ins.exec();
    return true;
}

} // namespace meshlink
