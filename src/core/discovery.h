// deepin-MeshLink — LAN node discovery & presence (UC-04).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
//
// Signed UDP heartbeats only — no mDNS/DHCP/external services (BR-07).
// Every datagram is payload||sig(64)||ed25519pub(32); receivers verify the
// signature AND that SHA-256(pubkey DER)[0:32] equals the claimed fingerprint
// (NFR-12/14) before accepting the node. Forged/invalid packets are dropped
// silently and logged (UC-04 E3). Packets stay well under MTU (BR-08).
#pragma once

#include "common.h"
#include "lansegment.h"

#include <QObject>
#include <QHostAddress>
#include <QTimer>
#include <QUdpSocket>

#include <functional>
#include <optional>

namespace meshlink {

struct RoomSummary {
    QString roomId, name, inviteCode;
    int members = 0;
    JoinPolicy policy = JoinPolicy::Open;
    QString hostFp;
};

struct PeerInfo {
    QString fingerprint, nickname, avatarHash, ip;
    quint16 tcpPort = 0;
    QByteArray ed25519Pub;
    qint64 upSince = 0;
    qint64 lastSeen = 0;
    QVector<RoomSummary> hostedRooms;
    bool online = false;
};

class Settings;

class Discovery : public QObject {
    Q_OBJECT
public:
    explicit Discovery(Settings *settings, QObject *parent = nullptr);
    ~Discovery() override;

    // Binds receive socket + broadcast send. Returns false on port conflict
    // (UC-04 E1) — caller surfaces the setting to change the port.
    bool start(QString *error);
    void stop(); // sends signed farewell (UC-04 A1)

    void setSelf(const QString &fp, const QString &nickname, const QString &avatarHash, const QByteArray &ed25519Pub);
    void setSelfEndpoint(const QString &ip, quint16 tcpPort);
    void setHostedRooms(const QVector<RoomSummary> &rooms);

    // LAN segment: heartbeats are broadcast into (and only accepted from) this
    // IPv4 subnet. switchSegment() clears the peer table and re-announces, so
    // the user can move between networks without restarting the app.
    void setSegment(const LanSegment &segment);
    void switchSegment(const LanSegment &segment);
    LanSegment segment() const { return m_segment; }
    int droppedOutOfSegmentCount() const { return m_droppedOutOfSegment; }

    // Sign callback bound to the account's Ed25519 private key (NFR-14).
    void setSigner(std::function<bool(const QByteArray &, QByteArray *)> signer);

    // Optional bootstrap targets for environments where broadcast is blocked
    // (UC-04 E2 helper): heartbeats are also unicast to these endpoints.
    void addManualTarget(const QString &ip, quint16 port);

    QVector<PeerInfo> peers() const;
    bool isPeerOnline(const QString &fp) const;
    std::optional<PeerInfo> peer(const QString &fp) const;

    // Joinable room catalogue gathered from the heartbeats of online peers.
    QVector<RoomSummary> roomCatalogue() const;
    std::optional<RoomSummary> roomById(const QString &roomId) const;
    std::optional<RoomSummary> roomByInviteCode(const QString &inviteCode) const;

    quint16 boundPort() const { return m_port; }
    void restartTimer(); // settings live-apply (defined in .cpp; needs Settings)
    qint64 lastRxHeartbeatMs() const { return m_lastRx; } // network diagnostics
    int droppedInvalidCount() const { return m_droppedInvalid; }

signals:
    void peerSeen(const PeerInfo &p);       // new or updated
    void peerGone(const QString &fingerprint); // timeout or farewell
    void peersChanged();
    void spoofingSuspected(const QString &sourceIp, const QString &claimedFp); // UC-04 E3

private slots:
    void onTick();        // broadcast + prune
    void onReadPending();

private:
    QByteArray buildDatagram(bool farewell);
    void handleDatagram(const QByteArray &dgram, const QHostAddress &senderIp);
    bool senderAllowed(const QHostAddress &senderIp) const;
    QHostAddress broadcastTarget() const;

    QUdpSocket *m_socket = nullptr;
    QTimer m_timer;
    Settings *m_settings = nullptr;
    quint16 m_port = 0;
    QString m_fp, m_nickname, m_avatarHash, m_ip;
    quint16 m_portTcp = 0;
    QByteArray m_edPub;
    LanSegment m_segment;
    std::function<bool(const QByteArray &, QByteArray *)> m_signer;
    QVector<RoomSummary> m_rooms;
    QVector<QPair<QString, quint16>> m_manualTargets;
    QHash<QString, PeerInfo> m_peerTable;
    qint64 m_upSince = 0;
    qint64 m_lastRx = 0;
    int m_droppedInvalid = 0;
    int m_droppedOutOfSegment = 0;
    bool m_running = false;
};

} // namespace meshlink
