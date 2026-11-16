// deepin-MeshLink — chat rooms: hosting authority + member client (UC-05/06/07/13).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
//
// The hosting node is the authoritative state source for its rooms (BR-09):
// members, roles, mutes, blacklist, approvals, join policy and message order
// (BR-16) live here. Members keep a local snapshot (BR-12).
#pragma once

#include "common.h"
#include "discovery.h"
#include "protocol.h"
#include "securelink.h"
#include "storage.h"

#include <QObject>
#include <QHash>
#include <functional>

namespace meshlink {

class AccountManager;
class Settings;

struct RoomLocal {
    QString roomId, name, intro, hostFp;
    JoinPolicy policy = JoinPolicy::Open;
    bool globalMute = false;
    int capacity = 0;      // 0 = unlimited
    bool hosted = false;   // self is the host (托管者)
    bool joined = false;   // self is a member (or host)
    qint64 createdTs = 0;
    int memberCount = 0;
    QString hostNickname;
    bool hostIsOnline = false;
};

struct PendingJoin {
    qint64 rowId = 0;
    QString roomId;
    PeerInfo requester; // may be partially filled from the request frame
};

class RoomManager : public QObject {
    Q_OBJECT
public:
    using SendFn = std::function<bool(const QString &fp, MsgType, const QByteArray &)>;
    using ResolveFn = std::function<std::optional<PeerInfo>(const QString &fp)>;
    using JoinCb = std::function<void(JoinStatus, const QString &detail)>;

    RoomManager(AccountManager *acct, Storage *storage, Settings *settings,
                SecureLink *link, QObject *parent = nullptr);

    void setSend(SendFn fn) { m_send = std::move(fn); }
    void setResolver(ResolveFn fn) { m_resolve = std::move(fn); }

    // ---- UC-05 ----
    QString createRoom(const QString &name, const QString &intro, JoinPolicy policy, int capacity, QString *error);

    // ---- UC-06 ----
    void joinRoom(const RoomSummary &summary, JoinCb cb);
    // ---- UC-13 ----
    void leaveRoom(const QString &roomId, std::function<void(bool, QString)> cb);

    // router entry: dispatch application frames arriving on channels
    void handle(MsgType type, SecureChannel *ch, const QByteArray &payload);

    // notifications from discovery
    void onPeerGone(const QString &fp);

    // ---- views ----
    QVector<RoomLocal> myRooms() const;
    QVector<RoomMemberInfo> members(const QString &roomId) const;
    QVector<PendingJoin> pendingApprovals(const QString &roomId) const;
    QVector<QString> blacklist(const QString &roomId) const;
    RoomLocal room(const QString &roomId) const;
    MemberRole myRole(const QString &roomId) const;
    bool isMuted(const QString &roomId, qint64 *untilOut = nullptr) const; // incl. globalMute
    QVector<RoomSummary> hostedSummaries() const; // for heartbeat broadcast (UC-05 step 5)

    // ---- UC-07 admin surface (host executes; remote admin sends ADMIN_OP) ----
    bool canManage(const QString &roomId, const QString &actorFp, const QString &targetFp, AdminOp op, QString *why);
    AdminOpResult applyAdminOp(const QString &roomId, const QString &actorFp, const QString &targetFp, AdminOp op,
                               qint64 durationMs); // authoritative mutation (host-side)
    void requestAdminOp(const QString &roomId, const QString &targetFp, AdminOp op, qint64 durationMs,
                        std::function<void(bool, QString)> cb); // member-side request path
    void respondApproval(qint64 rowId, bool accept);

    // startup: pull latest state for joined rooms (after crash/restart)
    void resyncAll();

signals:
    void roomsChanged();
    void membersChanged(const QString &roomId);
    void joinRequested(const QString &roomId, const PendingJoin &request); // host UI notification
    void joinResult(const QString &roomId, JoinStatus status, const QString &detail); // also async approvals
    void kickedFromRoom(const QString &roomId, const QString &reason);
    void roomClosedByHost(const QString &roomId);
    void adminCompleted(const QString &roomId, const QString &targetFp, quint8 op, bool ok, const QString &err);

private:
    // host-side
    void handleJoinRequest(SecureChannel *ch, const QByteArray &payload);
    void sendSnapshotTo(const QString &fp, const QString &roomId);
    void broadcastRoom(const QString &roomId, MsgType type, const QByteArray &payload,
                       const QString &exceptFp = QString());
    void upsertMember(const QString &roomId, const QString &fp, const QString &nick, quint8 role);
    // member-side
    void handleJoinResponse(SecureChannel *ch, const QByteArray &payload);
    void handleSnapshot(SecureChannel *ch, const QByteArray &payload);
    void handleMemberEvent(SecureChannel *ch, const QByteArray &payload);

public:
    void handleAdminError(const QString &reason); // ADM|... Error frames (host -> actor)

private:
    void handleAdminOpRemote(SecureChannel *ch, const QByteArray &payload);

    AccountManager *m_acct;
    Storage *m_storage;
    Settings *m_settings;
    SecureLink *m_link;
    SendFn m_send;
    ResolveFn m_resolve;
    QHash<QString, JoinCb> m_pendingJoins; // roomId -> cb (until first response)
    QHash<QString, QString> m_pendingJoinHost; // roomId -> host fp we asked to join
    QHash<QString, std::function<void(bool, QString)>> m_pendingLeaves;
    QHash<QString, std::function<void(bool, QString)>> m_pendingAdminOps; // "room|target|op" -> cb
};

} // namespace meshlink
