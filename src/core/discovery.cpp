// deepin-MeshLink — discovery implementation (signed UDP heartbeats).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "discovery.h"
#include <cstdio>

#include "crypto.h"
#include "protocol.h"
#include "settings.h"

#include <QDataStream>
#include <QNetworkDatagram>
#include <QNetworkInterface>

namespace meshlink {

// Heartbeat payload serialization (also signed region).
static QByteArray encodeHeartbeat(const QString &fp, const QString &nick, const QString &avatar,
                                 const QString &ip, quint16 port, qint64 upSince, bool farewell,
                                 const QVector<RoomSummary> &rooms) {
    return payloadOf([&](QDataStream &ds) {
        ds << fp << nick << avatar << ip << port << upSince << farewell << static_cast<quint32>(rooms.size());
        for (const auto &r : rooms) {
            ds << r.roomId << r.name << r.members << static_cast<quint8>(r.policy) << r.hostFp;
        }
    });
}

bool Discovery::start(QString *error) {
    m_socket = new QUdpSocket(this);
    m_port = m_settings->discoveryPort();
    // ReuseAddress lets multiple instances coexist during development/testing.
    if (!m_socket->bind(QHostAddress::Any, m_port, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        if (error)
            *error = QStringLiteral("UDP 端口 %1 绑定失败：%2（请在设置中修改发现端口）")
                         .arg(m_port)
                         .arg(m_socket->errorString());
        delete m_socket;
        m_socket = nullptr;
        return false;
    }
    m_upSince = nowMs();
    connect(m_socket, &QUdpSocket::readyRead, this, &Discovery::onReadPending);
    connect(&m_timer, &QTimer::timeout, this, &Discovery::onTick);
    m_timer.start(m_settings->heartbeatIntervalMs());
    m_running = true;
    onTick(); // send immediately so UI sees prompt broadcast
    return true;
}

void Discovery::restartTimer() { m_timer.start(m_settings->heartbeatIntervalMs()); }

void Discovery::stop() {
    if (!m_running)
        return;
    m_running = false;
    // UC-04 A1 signed farewell so peers remove us immediately.
    QByteArray dgram = buildDatagram(/*farewell=*/true);
    for (const auto &addr : QNetworkInterface::allAddresses()) {
        if (addr.protocol() == QAbstractSocket::IPv4Protocol && !addr.isLoopback())
            m_socket->writeDatagram(dgram, QHostAddress(QHostAddress::Broadcast), m_port);
    }
    m_timer.stop();
}

Discovery::Discovery(Settings *settings, QObject *parent)
    : QObject(parent), m_settings(settings) {}

Discovery::~Discovery() {
    stop();
    if (m_socket)
        m_socket->close();
}

void Discovery::setSelf(const QString &fp, const QString &nick, const QString &avatar, const QByteArray &pub) {
    m_fp = fp;
    m_nickname = nick;
    m_avatarHash = avatar;
    m_edPub = pub;
}
void Discovery::setSelfEndpoint(const QString &ip, quint16 tcpPort) {
    m_ip = ip;
    m_portTcp = tcpPort;
}
void Discovery::setHostedRooms(const QVector<RoomSummary> &rooms) {
    m_rooms = rooms;
    if (qEnvironmentVariableIsSet("MESHLINK_DEBUG"))
        fprintf(stderr, "[disc] setHostedRooms n=%d\n", (int)rooms.size());
}
void Discovery::setSigner(std::function<bool(const QByteArray &, QByteArray *)> signer) {
    m_signer = std::move(signer);
}
void Discovery::addManualTarget(const QString &ip, quint16 port) { m_manualTargets.append({ip, port}); }

QByteArray Discovery::buildDatagram(bool farewell) {
    // payload (up to kMaxHeartbeatRooms rooms to keep it compact, BR-08)
    QVector<RoomSummary> rooms = m_rooms;
    if (rooms.size() > kMaxHeartbeatRooms)
        rooms = rooms.mid(0, kMaxHeartbeatRooms);
    QByteArray payload = encodeHeartbeat(m_fp, m_nickname, m_avatarHash, m_ip, m_portTcp, m_upSince, farewell, rooms);

    QByteArray sig; // 64 bytes; requires loaded account key
    if (m_signer)
        m_signer(payload, &sig);
    if (sig.size() != 64)
        sig = QByteArray(64, '\0'); // still send something verifiable-false; receiver drops on bad sig

    QByteArray dgram;
    dgram.append(payload);
    dgram.append(sig);
    dgram.append(m_edPub);
    return dgram;
}

void Discovery::onTick() {
    if (!m_socket)
        return;
    QByteArray dgram = buildDatagram(false);
    qint64 sent = m_socket->writeDatagram(dgram, QHostAddress(QHostAddress::Broadcast), m_port);
    static int dbgN = 0;
    if (qEnvironmentVariableIsSet("MESHLINK_DEBUG") && ++dbgN % 10 == 1)
        fprintf(stderr, "[disc %s dbg] state=%d bound=%u sent=%lld err=%s peers=%d targets=%d hosted=%d\n", qgetenv("MESHLINK_NODE").constData(),
                int(m_socket->state()), m_port, (long long)sent, qUtf8Printable(m_socket->errorString()),
                (int)m_peerTable.size(), (int)m_manualTargets.size(), (int)m_rooms.size());
    for (const auto &t : m_manualTargets)
        m_socket->writeDatagram(dgram, QHostAddress(t.first), t.second);

    // prune stale peers (> N heartbeat periods silent) — UC-04 step 5
    const qint64 timeout = qint64(m_settings->heartbeatIntervalMs()) * m_settings->offlinePeriods();
    const qint64 now = nowMs();
    bool changed = false;
    for (auto it = m_peerTable.begin(); it != m_peerTable.end();) {
        if (it->online && now - it->lastSeen > timeout) {
            it->online = false;
            emit peerGone(it.key());
            changed = true;
        }
        ++it;
    }
    if (changed)
        emit peersChanged();
}

void Discovery::onReadPending() {
    while (m_socket && m_socket->hasPendingDatagrams()) {
        QNetworkDatagram dg = m_socket->receiveDatagram();
        handleDatagram(dg.data(), dg.senderAddress());
    }
}

void Discovery::handleDatagram(const QByteArray &dgram, const QHostAddress &senderIp) {
    if (dgram.size() < 96) {
        ++m_droppedInvalid;
        return;
    }
    const QByteArray payload = dgram.left(dgram.size() - 96);
    const QByteArray sig = dgram.mid(dgram.size() - 96, 64);
    const QByteArray pub = dgram.mid(dgram.size() - 32);
    if (pub.size() != 32) {
        ++m_droppedInvalid;
        return;
    }
    // Derive the authoritative fingerprint from the embedded public key.
    const QString claimedFp = QString::fromStdString(Crypto::fingerprintFromEd25519Pub(Bytes(pub.begin(), pub.end())));
    if (claimedFp.isEmpty()) {
        ++m_droppedInvalid;
        return;
    }
    // Verify the Ed25519 signature over the payload (NFR-14 / UC-04 step 4).
    if (sig.size() != 64 ||
        !Crypto::ed25519Verify(Bytes(pub.begin(), pub.end()), Bytes(payload.begin(), payload.end()),
                               Bytes(sig.begin(), sig.end()))) {
        ++m_droppedInvalid;
        emit spoofingSuspected(senderIp.toString(), claimedFp); // UC-04 E3
        return;
    }

    QDataStream ds(payload);
    ds.setVersion(QDataStream::Qt_6_0);
    QString fp, nick, avatar, ip;
    quint16 port = 0;
    qint64 upSince = 0;
    bool bye = false;
    quint32 nRooms = 0;
    ds >> fp >> nick >> avatar >> ip >> port >> upSince >> bye >> nRooms;

    // Cross-check: claimed fingerprint must equal the derived one, else forged.
    if (fp != claimedFp) {
        ++m_droppedInvalid;
        emit spoofingSuspected(senderIp.toString(), fp);
        return;
    }

    QVector<RoomSummary> rooms;
    for (quint32 i = 0; i < nRooms && i < 64; ++i) {
        RoomSummary r;
        quint8 pol = 0;
        ds >> r.roomId >> r.name >> r.members >> pol >> r.hostFp;
        r.policy = static_cast<JoinPolicy>(pol);
        rooms.append(r);
    }

    m_lastRx = nowMs();

    if (bye) {
        if (m_peerTable.remove(fp))
            emit peersChanged();
        emit peerGone(fp);
        return;
    }

    PeerInfo info;
    info.fingerprint = fp;
    info.nickname = nick;
    info.avatarHash = avatar;
    info.ip = ip.isEmpty() ? senderIp.toString() : ip;
    info.tcpPort = port;
    info.ed25519Pub = pub;
    info.upSince = upSince;
    info.lastSeen = nowMs();
    info.hostedRooms = rooms;
    info.online = true;

    const bool isNew = !m_peerTable.contains(fp);
    m_peerTable[fp] = info;
    emit peerSeen(info);
    if (isNew)
        emit peersChanged();
}

QVector<PeerInfo> Discovery::peers() const {
    QVector<PeerInfo> out;
    for (const auto &p : m_peerTable)
        if (p.online)
            out.append(p);
    return out;
}
bool Discovery::isPeerOnline(const QString &fp) const { return m_peerTable.value(fp).online; }
std::optional<PeerInfo> Discovery::peer(const QString &fp) const {
    auto it = m_peerTable.find(fp);
    if (it == m_peerTable.end())
        return std::nullopt;
    return *it;
}

} // namespace meshlink
