// deepin-MeshLink — engine wiring implementation.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "engine.h"
#include <cstdio>

#include <QTimer>

namespace meshlink {

Engine::Engine(Settings *settings, QObject *parent)
    : QObject(parent), m_settings(settings), m_storage(), m_account(&m_storage, this),
      m_discovery(settings, this), m_link(&m_account, &m_storage, this),
      m_rooms(&m_account, &m_storage, settings, &m_link, this),
      m_chat(&m_account, &m_storage, &m_rooms, &m_link, this),
      m_downloader(&m_account, &m_storage, settings, &m_link, this),
      m_files(&m_account, &m_storage, &m_rooms, &m_downloader, this) {
    // control-plane send: every module talks to a node by fingerprint only.
    auto send = [this](const QString &fp, MsgType t, const QByteArray &p) { return sendToPeer(fp, t, p); };
    m_rooms.setSend(send);
    m_chat.setSend(send);
    m_files.setSend(send);

    auto resolve = [this](const QString &fp) -> std::optional<PeerInfo> { return m_discovery.peer(fp); };
    m_rooms.setResolver(resolve);
    m_files.setResolve([this](const QString &fp, QString &ip, quint16 &port, bool &online) {
        return resolvePeer(fp, ip, port, online);
    });

    // frame router for every authenticated channel (server- or client-side)
    connect(&m_link, &SecureLink::incoming, this, [this](SecureChannel *ch) {
        connect(ch, &SecureChannel::messageReceived, this,
                [this](SecureChannel *c, MsgType t, const QByteArray &p) { route(t, c, p); });
        connect(ch, &SecureChannel::destroyed, this, [this](SecureChannel *, const QString &) {
            // a member may have dropped; UI refreshes from peer table timeouts
        });
    });
    // channels we dialed ourselves also carry server responses: route them
    connect(&m_link, &SecureLink::channelLost, this, [this](SecureChannel *ch, const QString &) {
        ch->deleteLater();
    });

    // discovery events (UC-03/04 + UC-05 room lists refresh)
    connect(&m_discovery, &Discovery::peerSeen, this, [this](const PeerInfo &p) {
        QSqlQuery q = m_storage.query("INSERT OR REPLACE INTO peers (fingerprint,nickname,avatar_hash,ip,tcp_port,ed25519_pub,last_seen) VALUES (?,?,?,?,?,?,?)");
        q.bindValue(0, p.fingerprint);
        q.bindValue(1, p.nickname);
        q.bindValue(2, p.avatarHash);
        q.bindValue(3, p.ip);
        q.bindValue(4, int(p.tcpPort));
        q.bindValue(5, p.ed25519Pub);
        q.bindValue(6, nowMs());
        q.exec();
        // pull the peer avatar bytes if we don't have them yet (BR-06)
        if (!p.avatarHash.isEmpty() && !ChatManager::hasBlob(p.avatarHash))
            m_chat.requestBlob(p.avatarHash, p.fingerprint);
        // waiting downloads pick their suppliers back up (UC-11 E1 recovery)
        m_downloader.supplierSeen(p.fingerprint);
    });
    connect(&m_discovery, &Discovery::peerGone, this, [this](const QString &fp) { m_rooms.onPeerGone(fp); });
    connect(&m_discovery, &Discovery::spoofingSuspected, this, [this](const QString &, const QString &fp) {
        emit spoofingSuspected(fp); // UC-03 A1/UC-04 E3 surfaced (yellow banner)
    });
    connect(&m_chat, &ChatManager::blobReady, this, [this](const QString &) { emit m_discovery.peersChanged(); });

    // my hosted-room list rides every heartbeat (UC-05 step 5)
    connect(&m_rooms, &RoomManager::roomsChanged, this, [this]() {
        m_discovery.setHostedRooms(m_rooms.hostedSummaries());
    });

    // verified downloads turn us into additional suppliers (BR-18)
    connect(&m_downloader, &Downloader::becameSupplier, this, [this](const QString &roomId, const QString &hash) {
        m_files.registerAsSupplier(roomId, hash);
    });
}

Engine::~Engine() { shutdown(); }

bool Engine::hasLocalAccount() {
    if (!m_storage.isOpen()) {
        QString err;
        if (!m_storage.open(Paths::databaseFile(), &err))
            return false;
    }
    return m_account.hasAccount();
}

bool Engine::start(QString *error) {
    Paths::ensureDirs();
    if (!m_storage.open(Paths::databaseFile(), error))
        return false;

    if (!m_account.hasAccount()) {
        if (!m_account.createAccount({}, error)) // UC-01 first-run default path
            return false;
    } else if (!m_account.loadAccount(error)) { // UC-01 A1 failure -> re-init dialog
        return false;
    }

    m_lanIp = m_settings->lanIpAddress();
    m_account.setLanIp(m_lanIp);

    const auto prof = m_account.profile();
    m_discovery.setSigner([this](const QByteArray &in, QByteArray *out) { return m_account.sign(in, out); });
    m_discovery.setSelf(prof.fingerprint, prof.nickname, prof.avatarHash, m_account.publicKey());
    m_discovery.setSelfEndpoint(m_lanIp, m_settings->tcpPort());

    QString lerr;
    if (!m_link.listen(m_settings->tcpPort(), &lerr)) { // UC-04 E1 (TCP side)
        *error = lerr;
        return false;
    }
    if (!m_discovery.start(error)) // UC-04 E1 (UDP side)
        return false;
    // UC-04 E2: unicast bootstrap peers where broadcast is blocked
    for (const auto &entry : m_settings->bootstrapTargets()) {
        const int sep = entry.lastIndexOf(':');
        if (sep > 0)
            m_discovery.addManualTarget(entry.left(sep), quint16(entry.mid(sep + 1).toUShort()));
    }
    m_discovery.setHostedRooms(m_rooms.hostedSummaries());

    // periodic room-state resync keeps snapshots fresh after flaps (NFR-01)
    auto *t = new QTimer(this);
    connect(t, &QTimer::timeout, this, [this]() { m_rooms.resyncAll(); });
    t->start(60'000);

    m_downloader.recoverTasks(); // UC-11 resume-after-restart
    emit started(true);
    return true;
}

void Engine::shutdown() {
    m_discovery.stop(); // signed farewell first (UC-04 A1)
    m_link.stopListening();
    m_storage.close();
}

bool Engine::createFreshAccount(const QString &nickname, QString *error) {
    Paths::ensureDirs();
    if (!m_storage.open(Paths::databaseFile(), error))
        return false;
    if (m_account.hasAccount()) { // UC-01 A2: replace old identity deliberately
        if (!m_account.destroyAccount(true, error))
            return false;
    }
    return m_account.createAccount(nickname, error);
}

bool Engine::destroyAccount(bool alsoHistory, QString *error) {
    // UC-02 step 4: best-effort "leave"/"dissolve" notifications over the
    // already-established TLS channels, before the identity and its keys go.
    const auto rooms = m_rooms.myRooms();
    for (const auto &r : rooms)
        m_rooms.leaveRoom(r.roomId, [](bool, const QString &) {});
    m_discovery.stop(); // signed farewell first (UC-04 A1)
    m_link.stopListening();
    // Wipe while the database connection is still usable, close it afterwards.
    const bool ok = m_account.destroyAccount(alsoHistory, error);
    m_storage.close();
    return ok;
}

bool Engine::sendToPeer(const QString &fp, MsgType type, const QByteArray &payload) {
    if (fp == m_account.profile().fingerprint)
        return true; // self-relay paths are short-circuited by callers
    if (auto *ch = m_link.channelTo(fp)) {
        ch->send(type, payload);
        return true;
    }
    auto p = m_discovery.peer(fp);
    if (!p || p->ip.isEmpty() || p->tcpPort == 0) {
        if (qEnvironmentVariableIsSet("MESHLINK_DEBUG"))
            fprintf(stderr, "[eng] send %s type=%d OFFLINE (peer=%s)\n", qUtf8Printable(fp.left(8)), int(type), p ? "info-empty" : "none");
        return false; // offline (caller renders E1 states)
    }
    if (qEnvironmentVariableIsSet("MESHLINK_DEBUG"))
        fprintf(stderr, "[eng] send %s type=%d via NEW dial\n", qUtf8Printable(fp.left(8)), int(type));
    m_link.connectTo(fp, p->ip, p->tcpPort, this,
                     [type, payload](SecureChannel *c) { c->send(type, payload); },
                     [fp](const QString &why) { qWarning("meshlink: dial %s failed: %s", qUtf8Printable(fp.left(8)), qUtf8Printable(why)); });
    return true;
}

bool Engine::resolvePeer(const QString &fp, QString &ip, quint16 &port, bool &online) {
    auto p = m_discovery.peer(fp);
    online = bool(p);
    if (p) {
        ip = p->ip;
        port = p->tcpPort;
        return true;
    }
    // fall back to the persisted peer table (offline endpoints still useful for UI)
    QSqlQuery q = m_storage.query("SELECT ip,tcp_port FROM peers WHERE fingerprint=?");
    q.bindValue(0, fp);
    q.exec();
    if (q.next()) {
        ip = q.value(0).toString();
        port = quint16(q.value(1).toInt());
        return true;
    }
    return false;
}

void Engine::route(MsgType type, SecureChannel *ch, const QByteArray &payload) {
    const auto t = std::underlying_type_t<MsgType>(type);
    if (t >= std::underlying_type_t<MsgType>(MsgType::JoinRequest) && t <= std::underlying_type_t<MsgType>(MsgType::AdminOp))
        m_rooms.handle(type, ch, payload); // room/join/admin/member frames
    else if (type == MsgType::ChatMessage || type == MsgType::ChatAck || type == MsgType::BlobRequest ||
             type == MsgType::BlobData)
        m_chat.handle(type, ch, payload);
    else if (type == MsgType::FileAnnounce || type == MsgType::SuppliersQuery || type == MsgType::SuppliersResponse ||
             type == MsgType::FileRequest || type == MsgType::ChunkRequest)
        m_files.handle(type, ch, payload);
    else if (type == MsgType::Error) {
        ErrorMessageMsg e;
        if (ErrorMessageMsg::fromPayload(payload, e)) {
            if (e.reason.startsWith(QStringLiteral("ADM|")))
                m_rooms.handleAdminError(e.reason); // admin-op rejection back to actor
            else
                qWarning("meshlink: peer error: %s", qUtf8Printable(e.reason));
        }
    }
}

} // namespace meshlink
