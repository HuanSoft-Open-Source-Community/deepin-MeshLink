// deepin-MeshLink — engine wiring implementation.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "engine.h"
#include <cstdio>

#include <QSet>
#include <QSqlQuery>
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
    m_startStage = StartStage::None;
    m_lastSwitchFailure = SwitchFailure::None;
    Paths::ensureDirs();
    m_startStage = StartStage::Storage;
    if (!m_storage.open(Paths::databaseFile(), error))
        return false;

    m_startStage = StartStage::Identity;
    if (!m_account.hasAccount()) {
        const qint64 id = m_account.createNewAccount({}, error); // UC-01 first-run default path
        if (id < 0)
            return false;
        m_settings->setActiveAccountId(id);
    } else if (!m_account.loadAccount(qMax<qint64>(1, m_settings->activeAccountId()), error)) {
        // UC-01 A1 failure -> re-init dialog
        return false;
    }
    return startNetwork(error);
}

bool Engine::startNetwork(QString *error) {
    m_startStage = StartStage::Tcp;
    m_segment = m_settings->lanSegment();
    m_lanIp = m_segment.ip;
    m_account.setLanIp(m_lanIp);

    const auto prof = m_account.profile();
    m_discovery.setSigner([this](const QByteArray &in, QByteArray *out) { return m_account.sign(in, out); });
    m_discovery.setSelf(prof.fingerprint, prof.nickname, prof.avatarHash, m_account.publicKey());
    m_discovery.setSelfEndpoint(m_lanIp, m_settings->tcpPort());
    m_discovery.setSegment(m_segment);

    QString lerr;
    if (!m_link.listen(m_settings->tcpPort(), &lerr)) { // UC-04 E1 (TCP side)
        *error = lerr;
        return false;
    }
    m_startStage = StartStage::Udp;
    if (!m_discovery.start(error)) // UC-04 E1 (UDP side)
        return false;
    m_startStage = StartStage::None;
    // UC-04 E2: unicast bootstrap peers where broadcast is blocked
    for (const auto &entry : m_settings->bootstrapTargets()) {
        const int sep = entry.lastIndexOf(':');
        if (sep > 0)
            m_discovery.addManualTarget(entry.left(sep), quint16(entry.mid(sep + 1).toUShort()));
    }
    m_discovery.setHostedRooms(m_rooms.hostedSummaries());

    // periodic room-state resync keeps snapshots fresh after flaps (NFR-01)
    if (!m_resyncTimer) {
        m_resyncTimer = new QTimer(this);
        connect(m_resyncTimer, &QTimer::timeout, this, [this]() { m_rooms.resyncAll(); });
    }
    m_resyncTimer->start(60'000);

    m_downloader.recoverTasks(); // UC-11 resume-after-restart
    m_networkRunning = true;
    emit started(true);
    return true;
}

void Engine::stopNetwork() {
    // Idempotent: also cleans a partially started listener after a failed
    // startNetwork(), where m_networkRunning is still false.
    if (m_resyncTimer)
        m_resyncTimer->stop();
    m_discovery.stop(); // signed farewell first (UC-04 A1)
    m_link.stopListening();
    // Channels authenticated with the old identity must not survive a switch.
    for (SecureChannel *ch : m_link.channels())
        ch->close();
    m_networkRunning = false;
}

void Engine::shutdown() {
    stopNetwork();
    m_storage.close();
}

bool Engine::setLanSegment(const LanSegment &segment, QString *error) {
    if (!segment.isValid()) {
        if (error)
            *error = tr("Invalid LAN segment: %1/%2").arg(segment.ip).arg(segment.prefixLength);
        return false;
    }
    if (segment == m_segment) {
        m_settings->setLanSegment(segment);
        return true;
    }
    m_segment = segment;
    m_lanIp = segment.ip;
    m_settings->setLanSegment(segment);
    m_account.setLanIp(segment.ip);
    m_discovery.setSelfEndpoint(segment.ip, m_settings->tcpPort());
    m_discovery.switchSegment(segment);
    emit lanSegmentChanged(segment.label());
    return true;
}

bool Engine::createFreshAccount(const QString &nickname, QString *error) {
    Paths::ensureDirs();
    if (!m_storage.open(Paths::databaseFile(), error))
        return false;
    if (m_account.hasAccount()) { // UC-01 A2: replace the (broken) active identity
        // The broken identity may not be loaded (identity failure), so resolve
        // it from Settings instead of wiping an empty fingerprint.
        qint64 oldId = m_account.activeRowId();
        if (oldId <= 0)
            oldId = m_settings->activeAccountId();
        if (oldId <= 0) {
            const auto all = m_account.listAccounts();
            if (all.size() == 1)
                oldId = all.first().id;
        }
        if (oldId <= 0 || m_account.destroyAccountById(oldId, true, error) < 0)
            return false;
    }
    const qint64 id = m_account.createNewAccount(nickname, error);
    if (id < 0)
        return false;
    m_settings->setActiveAccountId(id);
    return true;
}

qint64 Engine::createAdditionalAccount(const QString &nickname, QString *error) {
    Paths::ensureDirs();
    if (!m_storage.isOpen() && !m_storage.open(Paths::databaseFile(), error))
        return -1;
    const qint64 id = m_account.createNewAccount(nickname, error);
    if (id < 0)
        return -1;
    m_settings->setActiveAccountId(id);
    if (m_networkRunning && !switchAccount(id, error))
        return -1;
    return id;
}

bool Engine::switchAccount(qint64 accountId, QString *error) {
    m_lastSwitchFailure = SwitchFailure::None;
    if (!m_storage.isOpen() && !m_storage.open(Paths::databaseFile(), error)) {
        m_lastSwitchFailure = SwitchFailure::Identity;
        return false;
    }
    if (accountId <= 0) {
        m_lastSwitchFailure = SwitchFailure::Identity;
        if (error)
            *error = tr("No local account with id %1").arg(accountId);
        return false;
    }
    const qint64 previousId = m_account.activeRowId() > 0 ? m_account.activeRowId()
                                                          : m_settings->activeAccountId();
    const bool wasRunning = m_networkRunning;
    stopNetwork();
    m_settings->setActiveAccountId(accountId);
    m_account.resetLoaded();
    m_startStage = StartStage::Identity;

    QString switchError;
    if (!m_account.loadAccount(accountId, &switchError)) {
        m_lastSwitchFailure = SwitchFailure::Identity;
        if (error)
            *error = switchError; // report the original cause, not the rollback
        QString rollbackError;
        if (!restorePreviousAccount(previousId, wasRunning, &rollbackError)) {
            qWarning("meshlink: account switch rollback failed: %s",
                     qUtf8Printable(rollbackError));
        }
        return false;
    }

    m_startStage = StartStage::None;
    if (wasRunning && !startNetwork(&switchError)) {
        m_lastSwitchFailure = SwitchFailure::Network;
        if (error)
            *error = switchError;
        QString rollbackError;
        if (!restorePreviousAccount(previousId, wasRunning, &rollbackError)) {
            qWarning("meshlink: account switch rollback failed: %s",
                     qUtf8Printable(rollbackError));
        }
        return false;
    }

    m_rooms.resyncAll();
    emit accountSwitched(accountId);
    return true;
}

bool Engine::restorePreviousAccount(qint64 previousId, bool wasRunning, QString *error) {
    stopNetwork(); // also cleans a partially started network from the failed target
    m_settings->setActiveAccountId(previousId);
    m_account.resetLoaded();
    if (previousId > 0) {
        QString loadError;
        if (!m_account.loadAccount(previousId, &loadError)) {
            if (error)
                *error = loadError;
            return false;
        }
    }
    m_startStage = StartStage::None;
    if (wasRunning && previousId > 0 && !startNetwork(error))
        return false;
    m_rooms.resyncAll();
    if (previousId > 0)
        emit accountSwitched(previousId);
    return true;
}

int Engine::destroyAccount(bool alsoHistory, QString *error) {
    const qint64 id = m_account.activeRowId() > 0 ? m_account.activeRowId()
                                                   : m_settings->activeAccountId();
    return destroyAccountById(id, alsoHistory, error);
}

int Engine::destroyAccountById(qint64 id, bool alsoHistory, QString *error) {
    if (id <= 0) {
        if (error)
            *error = tr("No local account with id %1").arg(id);
        return -1;
    }
    if (!m_storage.isOpen() && !m_storage.open(Paths::databaseFile(), error))
        return -1;

    // Validate before any side effect (leaves, download cancellation, wipe).
    const auto accounts = m_account.listAccounts();
    const AccountEntry *target = nullptr;
    for (const auto &entry : accounts) {
        if (entry.id == id) {
            target = &entry;
            break;
        }
    }
    if (!target) {
        if (error)
            *error = tr("No local account with id %1").arg(id);
        return -1;
    }

    const qint64 loadedId = m_account.activeRowId();
    const bool active = loadedId > 0 ? (id == loadedId)
                                     : (id == m_settings->activeAccountId());
    const bool dbg = qEnvironmentVariableIsSet("MESHLINK_DEBUG");

    if (!active) {
        // A non-active identity has no authenticated channels in this process.
        // Sending LeaveRoom/RoomClosed under the active identity would spoof
        // the wrong account, so the best effort here is local: cancel tasks
        // that belong to its rooms and let the storage wipe remove its
        // membership (and, when requested, rooms/history).
        QSet<QString> roomIds;
        QSqlQuery rq = m_storage.query("SELECT room_id FROM rooms WHERE owner_fp=?");
        rq.bindValue(0, target->fingerprint);
        if (rq.exec()) {
            while (rq.next())
                roomIds.insert(rq.value(0).toString());
        }
        for (const auto &t : m_downloader.tasks()) {
            if (roomIds.contains(t.roomId))
                m_downloader.cancel(t.hash, false);
        }
        if (dbg)
            fprintf(stderr, "[destroy] non-active id=%lld fingerprint=%s rooms=%lld\n",
                    (long long)id, qUtf8Printable(target->fingerprint.left(8)),
                    (long long)roomIds.size());
        const int remaining = m_account.destroyAccountById(id, alsoHistory, error);
        if (dbg)
            fprintf(stderr, "[destroy] non-active wipe done remaining=%d err=%s\n", remaining,
                    error ? qUtf8Printable(*error) : "-");
        return remaining;
    }

    // UC-02 step 4: best-effort "leave"/"dissolve" notifications over the
    // already-established TLS channels, before the identity and its keys go.
    const auto rooms = m_rooms.myRooms();
    if (dbg)
        fprintf(stderr, "[destroy] myRooms=%lld\n", (long long)rooms.size());
    for (const auto &r : rooms)
        m_rooms.leaveRoom(r.roomId, [](bool, const QString &) {});
    if (dbg)
        fprintf(stderr, "[destroy] leaves sent\n");
    // Stop everything that touches storage: resync timer, heartbeats, the
    // TLS listener, live channels and in-flight download workers (their
    // chunk_progress writes would race the wipe below).
    stopNetwork();
    for (const auto &t : m_downloader.tasks())
        m_downloader.cancel(t.hash, false);
    m_networkRunning = false;
    if (dbg)
        fprintf(stderr, "[destroy] network stopped, wiping\n");
    // Wipe while the database connection is still usable, close it afterwards.
    const int remaining = m_account.destroyAccountById(id, alsoHistory, error);
    if (dbg)
        fprintf(stderr, "[destroy] wipe done remaining=%d err=%s\n", remaining,
                error ? qUtf8Printable(*error) : "-");
    m_storage.close();
    if (dbg)
        fprintf(stderr, "[destroy] storage closed\n");
    return remaining;
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
    else if (type == MsgType::ChatMessage || type == MsgType::ChatAck ||
             type == MsgType::ChatRecall || type == MsgType::ChatRecallAck ||
             type == MsgType::BlobRequest || type == MsgType::BlobData)
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
