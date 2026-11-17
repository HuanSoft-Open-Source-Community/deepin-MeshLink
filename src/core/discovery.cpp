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
        ds << kHeartbeatFormat << fp << nick << avatar << ip << port << upSince << farewell
           << static_cast<quint32>(rooms.size());
        for (const auto &r : rooms) {
            ds << r.roomId << r.name << r.inviteCode << r.members << static_cast<quint8>(r.policy) << r.hostFp;
        }
    });
}

bool Discovery::start(QString *error) {
    m_socket = new QUdpSocket(this);
    m_port = m_settings->discoveryPort();
    // ReuseAddress lets multiple instances coexist during development/testing.
    if (!m_socket->bind(QHostAddress::Any, m_port, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        if (error)
            *error = tr("Could not bind UDP port %1: %2 (change the discovery port in Settings)")
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
    // UC-04 A1 signed farewell to the selected segment so peers remove us now.
    QByteArray dgram = buildDatagram(/*farewell=*/true);
    if (m_socket)
        m_socket->writeDatagram(dgram, broadcastTarget(), m_port);
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

void Discovery::setSegment(const LanSegment &segment) {
    if (segment.isValid())
        m_segment = segment;
}

void Discovery::switchSegment(const LanSegment &segment) {
    if (!segment.isValid())
        return;
    if (m_running && m_socket) {
        // Best-effort farewell on the old segment before leaving it.
        const QByteArray farewell = buildDatagram(/*farewell=*/true);
        m_socket->writeDatagram(farewell, broadcastTarget(), m_port);
    }
    setSegment(segment);
    const auto previous = m_peerTable;
    m_peerTable.clear();
    for (auto it = previous.begin(); it != previous.end(); ++it)
        emit peerGone(it.key());
    emit peersChanged();
    if (qEnvironmentVariableIsSet("MESHLINK_DEBUG"))
        fprintf(stderr, "[disc] segment switched: %s\n", qUtf8Printable(m_segment.label()));
    if (m_running)
        onTick(); // announce immediately on the new segment
}

QHostAddress Discovery::broadcastTarget() const {
    if (!m_segment.isValid())
        return QHostAddress(QHostAddress::Broadcast);
    return m_segment.broadcastAddress();
}

bool Discovery::senderAllowed(const QHostAddress &senderIp) const {
    if (!m_segment.isValid())
        return true;
    // Strict segment filter. Manual bootstrap targets only change where we
    // *send* unicast heartbeats (broadcast-blocked LANs); inbound traffic from
    // outside the selected segment is ignored.
    return m_segment.contains(senderIp);
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
    const QHostAddress target = broadcastTarget();
    qint64 sent = m_socket->writeDatagram(dgram, target, m_port);
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
    // A machine may sit on several networks; only heartbeats from the selected
    // segment (plus explicitly configured bootstrap peers) are accepted.
    if (!senderAllowed(senderIp)) {
        ++m_droppedOutOfSegment;
        if (qEnvironmentVariableIsSet("MESHLINK_DEBUG") && m_droppedOutOfSegment % 20 == 1)
            fprintf(stderr, "[disc] dropped out-of-segment packet from %s (segment %s)\n",
                    qUtf8Printable(senderIp.toString()), qUtf8Printable(m_segment.label()));
        return;
    }
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
    quint8 format = 0;
    ds >> format;
    if (format != kHeartbeatFormat) { // alpha rollout: reject incompatible formats
        ++m_droppedInvalid;
        return;
    }
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
        ds >> r.roomId >> r.name >> r.inviteCode >> r.members >> pol >> r.hostFp;
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

QVector<RoomSummary> Discovery::roomCatalogue() const {
    QVector<RoomSummary> out;
    for (const auto &p : m_peerTable) {
        if (!p.online)
            continue;
        out += p.hostedRooms;
    }
    return out;
}

std::optional<RoomSummary> Discovery::roomById(const QString &roomId) const {
    for (const auto &p : m_peerTable) {
        if (!p.online)
            continue;
        for (const auto &r : p.hostedRooms)
            if (r.roomId == roomId)
                return r;
    }
    return std::nullopt;
}

std::optional<RoomSummary> Discovery::roomByInviteCode(const QString &inviteCode) const {
    if (inviteCode.size() != kInviteCodeDigits)
        return std::nullopt;
    for (const auto &p : m_peerTable) {
        if (!p.online)
            continue;
        for (const auto &r : p.hostedRooms)
            if (r.inviteCode == inviteCode)
                return r;
    }
    return std::nullopt;
}

} // namespace meshlink
