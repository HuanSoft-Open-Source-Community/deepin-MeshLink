// deepin-MeshLink — room manager implementation (UC-05/06/07/13).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "roommanager.h"
#include <cstdio>

#include "account.h"
#include "crypto.h"
#include "settings.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>

namespace meshlink {

// MemberEvent op codes beyond AdminOp:
static constexpr quint8 kEvJoined = 100;
static constexpr quint8 kEvLeft = 101;

RoomManager::RoomManager(AccountManager *acct, Storage *storage, Settings *settings, SecureLink *link,
                         QObject *parent)
    : QObject(parent), m_acct(acct), m_storage(storage), m_settings(settings), m_link(link) {}

// ---------------- helpers ----------------
namespace {
QString generateInviteCode(Storage *storage) {
    for (int attempt = 0; attempt < 128; ++attempt) {
        const Bytes bytes = Crypto::randomBytes(4);
        const quint32 value = (quint32(bytes[0]) << 24) | (quint32(bytes[1]) << 16) | (quint32(bytes[2]) << 8) |
                              quint32(bytes[3]);
        const QString code = QStringLiteral("%1").arg(value % 1000000u, kInviteCodeDigits, 10, QChar('0'));
        QSqlQuery q = storage->query("SELECT 1 FROM rooms WHERE invite_code=?");
        q.bindValue(0, code);
        q.exec();
        if (!q.next())
            return code;
    }
    return {};
}
} // namespace

bool RoomManager::isValidInviteCode(const QString &code) {
    if (code.size() != kInviteCodeDigits)
        return false;
    for (const QChar c : code)
        if (!c.isDigit())
            return false;
    return true;
}

QString RoomManager::createRoom(const QString &name, const QString &intro, JoinPolicy policy, int capacity,
                                QString *error) {
    const QString n = name.trimmed();
    if (n.length() < 2 || n.length() > 64) { // UC-05 step 2
        if (error)
            *error = QStringLiteral("聊天室名称需为 2–64 个字符");
        return {};
    }
    if (capacity < 0)
        capacity = 0;
    const qint64 ts = nowMs();
    const QString self = m_acct->profile().fingerprint;
    const QString inviteCode = generateInviteCode(m_storage);
    if (inviteCode.isEmpty()) {
        if (error)
            *error = QStringLiteral("无法生成唯一邀请码，请稍后重试");
        return {};
    }
    const QByteArray seed = (self + "|" + n + "|" + inviteCode + "|" + QString::number(ts)).toUtf8();
    const QString roomId = QString::fromLatin1(
        Crypto::sha256Hex(reinterpret_cast<const uint8_t *>(seed.constData()), size_t(seed.size()))
            .substr(0, kRoomIdHexLen)
            .c_str());

    QSqlQuery q = m_storage->query("INSERT INTO rooms (room_id,name,intro,invite_code,policy,global_mute,capacity,host_fp,hosted,joined,created_ts)"
                                   " VALUES (?,?,?,?,?,0,?,?,1,1,?)");
    q.bindValue(0, roomId);
    q.bindValue(1, n);
    q.bindValue(2, intro.trimmed());
    q.bindValue(3, inviteCode);
    q.bindValue(4, static_cast<int>(policy));
    q.bindValue(5, capacity);
    q.bindValue(6, self);
    q.bindValue(7, ts);
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return {};
    }
    upsertMember(roomId, self, m_acct->profile().nickname, static_cast<quint8>(MemberRole::Host));
    emit roomsChanged();
    return roomId;
}

void RoomManager::upsertMember(const QString &roomId, const QString &fp, const QString &nick, quint8 role) {
    QSqlQuery q = m_storage->query("INSERT INTO members (room_id,fingerprint,nickname,role,muted_until,joined_ts)"
                                   " VALUES (?,?,?,?,0,?) ON CONFLICT(room_id,fingerprint) DO UPDATE SET nickname=excluded.nickname");
    q.bindValue(0, roomId);
    q.bindValue(1, fp);
    q.bindValue(2, nick);
    q.bindValue(3, int(role));
    q.bindValue(4, nowMs());
    q.exec();
}

QVector<RoomLocal> RoomManager::myRooms() const {
    QVector<RoomLocal> out;
    QSqlQuery q = m_storage->query(
        "SELECT r.room_id,r.name,r.intro,r.invite_code,r.policy,r.global_mute,r.capacity,r.host_fp,r.hosted,r.joined,r.created_ts,"
        " (SELECT COUNT(*) FROM members m WHERE m.room_id=r.room_id),"
        " (SELECT m.nickname FROM members m WHERE m.room_id=r.room_id AND m.fingerprint=r.host_fp)"
        " FROM rooms r WHERE r.joined=1 OR r.hosted=1 ORDER BY r.created_ts");
    q.exec();
    while (q.next()) {
        RoomLocal r;
        r.roomId = q.value(0).toString();
        r.name = q.value(1).toString();
        r.intro = q.value(2).toString();
        r.inviteCode = q.value(3).toString();
        r.policy = static_cast<JoinPolicy>(q.value(4).toInt());
        r.globalMute = q.value(5).toInt() != 0;
        r.capacity = q.value(6).toInt();
        r.hostFp = q.value(7).toString();
        r.hosted = q.value(8).toInt() != 0;
        r.joined = q.value(9).toInt() != 0;
        r.createdTs = q.value(10).toLongLong();
        r.memberCount = q.value(11).toInt();
        r.hostNickname = q.value(12).toString();
        r.hostIsOnline = m_resolve ? m_resolve(r.hostFp).has_value() : r.hosted;
        out.append(r);
    }
    return out;
}

RoomLocal RoomManager::room(const QString &roomId) const {
    for (const auto &r : myRooms())
        if (r.roomId == roomId)
            return r;
    return {};
}

QVector<RoomMemberInfo> RoomManager::members(const QString &roomId) const {
    QVector<RoomMemberInfo> out;
    QSqlQuery q = m_storage->query("SELECT fingerprint,nickname,role,muted_until FROM members WHERE room_id=? ORDER BY role,joined_ts");
    q.bindValue(0, roomId);
    q.exec();
    while (q.next()) {
        RoomMemberInfo mi;
        mi.fp = q.value(0).toString();
        mi.nickname = q.value(1).toString();
        mi.role = static_cast<quint8>(q.value(2).toInt());
        mi.mutedUntil = q.value(3).toLongLong();
        mi.online = m_resolve ? m_resolve(mi.fp).has_value() : true;
        mi.avatarHash = m_resolve ? QString::fromStdString("") : QString(); // avatars resolved by engine
        if (mi.avatarHash.isEmpty() && m_resolve) {
            if (auto p = m_resolve(mi.fp))
                mi.avatarHash = p->avatarHash;
        }
        out.append(mi);
    }
    return out;
}

MemberRole RoomManager::myRole(const QString &roomId) const {
    QSqlQuery q = m_storage->query("SELECT role FROM members WHERE room_id=? AND fingerprint=?");
    q.bindValue(0, roomId);
    q.bindValue(1, m_acct->profile().fingerprint);
    q.exec();
    if (q.next())
        return static_cast<MemberRole>(q.value(0).toInt());
    return MemberRole::Member;
}

bool RoomManager::isMuted(const QString &roomId, qint64 *untilOut) const {
    const qint64 now = nowMs();
    QSqlQuery q = m_storage->query("SELECT r.global_mute, m.role, m.muted_until FROM rooms r"
                                   " JOIN members m ON m.room_id=r.room_id AND m.fingerprint=? WHERE r.room_id=?");
    q.bindValue(0, m_acct->profile().fingerprint);
    q.bindValue(1, roomId);
    q.exec();
    if (!q.next())
        return true; // not a member -> cannot speak
    const bool gMute = q.value(0).toInt() != 0;
    const auto role = static_cast<MemberRole>(q.value(1).toInt());
    const qint64 until = q.value(2).toLongLong();
    if (role == MemberRole::Host || role == MemberRole::Admin)
        return false; // host/admins bypass global & targeted mute (UC-07 table)
    if (until == -1) {
        if (untilOut)
            *untilOut = -1;
        return true;
    }
    if (until > now) {
        if (untilOut)
            *untilOut = until;
        return true;
    }
    if (gMute) { // BR: 全员禁言 affects members only
        if (untilOut)
            *untilOut = 0;
        return true;
    }
    return false;
}

QVector<PendingJoin> RoomManager::pendingApprovals(const QString &roomId) const {
    QVector<PendingJoin> out;
    QSqlQuery q = m_storage->query("SELECT id,fingerprint,nickname,ip,port FROM approvals WHERE room_id=? ORDER BY req_ts");
    q.bindValue(0, roomId);
    q.exec();
    while (q.next()) {
        PendingJoin p;
        p.rowId = q.value(0).toLongLong();
        p.roomId = roomId;
        p.requester.fingerprint = q.value(1).toString();
        p.requester.nickname = q.value(2).toString();
        p.requester.ip = q.value(3).toString();
        p.requester.tcpPort = quint16(q.value(4).toInt());
        out.append(p);
    }
    return out;
}

QVector<QString> RoomManager::blacklist(const QString &roomId) const {
    QVector<QString> out;
    QSqlQuery q = m_storage->query("SELECT fingerprint FROM blacklist WHERE room_id=?");
    q.bindValue(0, roomId);
    q.exec();
    while (q.next())
        out.append(q.value(0).toString());
    return out;
}

QVector<RoomSummary> RoomManager::hostedSummaries() const {
    QVector<RoomSummary> out;
    QSqlQuery q = m_storage->query("SELECT r.room_id,r.name,r.invite_code,r.policy,(SELECT COUNT(*) FROM members m WHERE m.room_id=r.room_id)"
                                   " FROM rooms r WHERE r.hosted=1");
    q.exec();
    while (q.next()) {
        RoomSummary s;
        s.roomId = q.value(0).toString();
        s.name = q.value(1).toString();
        s.inviteCode = q.value(2).toString();
        s.policy = static_cast<JoinPolicy>(q.value(3).toInt());
        s.members = q.value(4).toInt();
        s.hostFp = m_acct->profile().fingerprint;
        out.append(s);
    }
    return out;
}

// ---------------- UC-06 join ----------------
void RoomManager::joinRoom(const RoomSummary &summary, JoinCb cb) {
    const QString hostFp = summary.hostFp;
    auto hostPeer = m_resolve ? m_resolve(hostFp) : std::nullopt;
    if (!hostPeer) { // UC-06 E1: host unreachable -> no half-joined state
        cb(JoinStatus::RoomGone, QStringLiteral("聊天室暂时不可用（托管节点不在线）"));
        return;
    }
    m_pendingJoins[summary.roomId] = cb;
    m_pendingJoinHost[summary.roomId] = hostFp;
    JoinRequestMsg j;
    j.roomId = summary.roomId;
    j.fingerprint = m_acct->profile().fingerprint;
    j.nickname = m_acct->profile().nickname;
    if (!m_send(hostFp, MsgType::JoinRequest, j.toPayload())) {
        m_pendingJoins.remove(summary.roomId);
        m_pendingJoinHost.remove(summary.roomId);
        cb(JoinStatus::RoomGone, QStringLiteral("聊天室暂时不可用")); // E1
    }
}

void RoomManager::handleJoinRequest(SecureChannel *ch, const QByteArray &payload) {
    JoinRequestMsg req;
    if (!JoinRequestMsg::fromPayload(payload, req))
        return;
    if (qEnvironmentVariableIsSet("MESHLINK_DEBUG"))
        fprintf(stderr, "[room] JOINREQ in room=%s req=%s ch=%d\n", qUtf8Printable(req.roomId),
                qUtf8Printable(req.fingerprint.left(8)), int(ch != nullptr));
    const RoomLocal r = room(req.roomId);
    // where to dial the requester back if the inbound channel is gone
    QString dialIp = ch ? ch->peerAddress() : QString();
    quint16 dialPort = 0;
    if (m_resolve) {
        if (auto p = m_resolve(req.fingerprint)) {
            dialIp = p->ip;
            dialPort = p->tcpPort;
        }
    }
    auto respond = [&](JoinStatus st, const QString &msg) {
        JoinResponseMsg resp;
        resp.roomId = req.roomId;
        resp.fingerprint = req.fingerprint;
        resp.status = static_cast<quint8>(st);
        resp.message = msg;
        const QByteArray bytes = resp.toPayload();
        if (ch && ch->authenticated() && ch->peerFingerprint() == req.fingerprint)
            ch->send(MsgType::JoinResponse, bytes);
        else if (!dialIp.isEmpty() && dialPort)
            m_link->connectTo(req.fingerprint, dialIp, dialPort, this,
                              [bytes](SecureChannel *c) { c->send(MsgType::JoinResponse, bytes); },
                              [](const QString &) {});
    };

    if (!r.hosted || r.roomId.isEmpty()) {
        respond(JoinStatus::RoomGone, QStringLiteral("聊天室不存在"));
        return;
    }
    // BR-14: blacklist blocks rejoin even with open policy.
    {
        QSqlQuery bq = m_storage->query("SELECT 1 FROM blacklist WHERE room_id=? AND fingerprint=?");
        bq.bindValue(0, req.roomId);
        bq.bindValue(1, req.fingerprint);
        bq.exec();
        const bool blacklisted = bq.next();
        if (qEnvironmentVariableIsSet("MESHLINK_DEBUG"))
            fprintf(stderr, "[room] blackgate room=%s fp=%s hit=%d\n", qUtf8Printable(req.roomId),
                    qUtf8Printable(req.fingerprint.left(8)), int(blacklisted));
        if (blacklisted) { // BR-14
            respond(JoinStatus::Blacklisted, QStringLiteral("你已被该聊天室列入黑名单"));
            return;
        }
    }
    const bool alreadyMember = [&] {
        QSqlQuery mq = m_storage->query("SELECT 1 FROM members WHERE room_id=? AND fingerprint=?");
        mq.bindValue(0, req.roomId);
        mq.bindValue(1, req.fingerprint);
        mq.exec();
        return mq.next();
    }();

    if (!alreadyMember && r.capacity > 0) {
        QSqlQuery cq = m_storage->query("SELECT COUNT(*) FROM members WHERE room_id=?");
        cq.bindValue(0, req.roomId);
        cq.exec();
        cq.next();
        if (cq.value(0).toInt() >= r.capacity) { // UC-06 E2
            respond(JoinStatus::RoomFull, QStringLiteral("聊天室已满"));
            return;
        }
    }

    if (r.policy == JoinPolicy::Approval && !alreadyMember) {
        // UC-06 A1 / UC-07 approval queue
        QSqlQuery aq = m_storage->query("INSERT INTO approvals (room_id,fingerprint,nickname,ip,port,req_ts) VALUES (?,?,?,?,?,?)");
        aq.bindValue(0, req.roomId);
        aq.bindValue(1, req.fingerprint);
        aq.bindValue(2, req.nickname);
        aq.bindValue(3, dialIp);
        aq.bindValue(4, int(dialPort));
        aq.bindValue(5, nowMs());
        aq.exec();
        PendingJoin p;
        p.rowId = aq.lastInsertId().toLongLong();
        p.roomId = req.roomId;
        p.requester.fingerprint = req.fingerprint;
        p.requester.nickname = req.nickname;
        p.requester.ip = dialIp;
        p.requester.tcpPort = dialPort;
        respond(JoinStatus::PendingApproval, QStringLiteral("申请已提交，等待托管者批准"));
        emit joinRequested(req.roomId, p);
        return;
    }

    upsertMember(req.roomId, req.fingerprint, req.nickname, static_cast<quint8>(MemberRole::Member));
    respond(JoinStatus::Accepted, QStringLiteral("已加入"));
    sendSnapshotTo(req.fingerprint, req.roomId);
    MemberEventMsg ev;
    ev.roomId = req.roomId;
    ev.targetFp = req.fingerprint;
    ev.targetNick = req.nickname;
    ev.op = kEvJoined;
    ev.role = static_cast<quint8>(MemberRole::Member);
    broadcastRoom(req.roomId, MsgType::MemberEvent, ev.toPayload(), req.fingerprint);
    emit membersChanged(req.roomId);
}

void RoomManager::respondApproval(qint64 rowId, bool accept) {
    QSqlQuery q = m_storage->query("SELECT room_id,fingerprint,nickname,ip,port FROM approvals WHERE id=?");
    q.bindValue(0, rowId);
    q.exec();
    if (!q.next())
        return;
    const QString roomId = q.value(0).toString();
    const QString fp = q.value(1).toString();
    const QString nick = q.value(2).toString();
    const QString ip = q.value(3).toString();
    const quint16 port = quint16(q.value(4).toInt());

    JoinResponseMsg resp;
    resp.roomId = roomId;
    resp.fingerprint = fp;
    QByteArray bytes;
    if (accept) {
        upsertMember(roomId, fp, nick, static_cast<quint8>(MemberRole::Member));
        resp.status = static_cast<quint8>(JoinStatus::Accepted);
        bytes = resp.toPayload();
        MemberEventMsg ev;
        ev.roomId = roomId;
        ev.targetFp = fp;
        ev.targetNick = nick;
        ev.op = kEvJoined;
        ev.role = static_cast<quint8>(MemberRole::Member);
        broadcastRoom(roomId, MsgType::MemberEvent, ev.toPayload(), fp);
        emit membersChanged(roomId);
    } else {
        resp.status = static_cast<quint8>(JoinStatus::Denied);
        resp.message = QStringLiteral("管理员拒绝了你的申请");
        bytes = resp.toPayload();
    }
    QSqlQuery dq = m_storage->query("DELETE FROM approvals WHERE id=?");
    dq.bindValue(0, rowId);
    dq.exec();

    if (auto *ch = m_link->channelTo(fp)) {
        ch->send(MsgType::JoinResponse, bytes);
        if (accept)
            sendSnapshotTo(fp, roomId);
    } else if (!ip.isEmpty() && port) {
        m_link->connectTo(fp, ip, port, this,
                          [this, bytes, roomId, fp, accept](SecureChannel *c) {
                              c->send(MsgType::JoinResponse, bytes);
                              if (accept)
                                  sendSnapshotTo(c->peerFingerprint(), roomId);
                              (void)fp;
                          },
                          [](const QString &) {});
    }
}

void RoomManager::sendSnapshotTo(const QString &fp, const QString &roomId) {
    const RoomLocal r = room(roomId);
    RoomSnapshotMsg snap;
    snap.roomId = r.roomId;
    snap.name = r.name;
    snap.intro = r.intro;
    snap.inviteCode = r.inviteCode;
    snap.policy = static_cast<quint8>(r.policy);
    snap.globalMute = r.globalMute;
    snap.capacity = r.capacity;
    snap.members = members(roomId);
    snap.memberCount = snap.members.size();
    {
        QSqlQuery q = m_storage->query("SELECT msg_id,sender_fp,sender_nick,avatar_hash,kind,seq,ts,text,content_hash,file_name,mime,file_size,img_w,img_h"
                                       " FROM messages WHERE room_id=? ORDER BY seq DESC LIMIT ?");
        q.bindValue(0, roomId);
        q.bindValue(1, m_settings->maxRoomHistory());
        q.exec();
        while (q.next()) {
            Msg m;
            m.roomId = roomId;
            m.msgId = q.value(0).toString();
            m.senderFp = q.value(1).toString();
            m.senderNick = q.value(2).toString();
            m.avatarHash = q.value(3).toString();
            m.kind = quint8(q.value(4).toInt());
            m.seq = q.value(5).toULongLong();
            m.ts = q.value(6).toLongLong();
            m.text = q.value(7).toString();
            m.contentHash = q.value(8).toString();
            m.fileName = q.value(9).toString();
            m.mime = q.value(10).toString();
            m.fileSize = q.value(11).toLongLong();
            m.imgW = q.value(12).toInt();
            m.imgH = q.value(13).toInt();
            m.status = static_cast<quint8>(MsgStatus::Delivered);
            snap.recent.prepend(m);
        }
    }
    if (auto *ch = m_link->channelTo(fp))
        ch->send(MsgType::RoomSnapshot, snap.toPayload());
    else
        m_send(fp, MsgType::RoomSnapshot, snap.toPayload());
}

// ---------------- member-side ----------------
void RoomManager::handleJoinResponse(SecureChannel *ch, const QByteArray &payload) {
    JoinResponseMsg resp;
    if (!JoinResponseMsg::fromPayload(payload, resp))
        return;
    // NFR-13/14: only the host we asked may answer our join request.
    const RoomLocal existing = room(resp.roomId);
    const QString expectHost = m_pendingJoinHost.value(resp.roomId, existing.hostFp);
    if (!ch || expectHost.isEmpty() || ch->peerFingerprint() != expectHost) {
        if (qEnvironmentVariableIsSet("MESHLINK_DEBUG"))
            fprintf(stderr, "[room] JOINRESP rejected from %s (expected host %s)\n",
                    ch ? qUtf8Printable(ch->peerFingerprint().left(8)) : "-", qUtf8Printable(expectHost.left(8)));
        return;
    }
    if (qEnvironmentVariableIsSet("MESHLINK_DEBUG"))
        fprintf(stderr, "[room] JOINRESP in status=%d pending=%d\n", int(resp.status), int(m_pendingJoins.contains(resp.roomId)));
    auto it = m_pendingJoins.find(resp.roomId);
    if (it != m_pendingJoins.end()) {
        auto cb = it.value();
        m_pendingJoins.erase(it);
        cb(static_cast<JoinStatus>(resp.status), resp.message);
    }
    // Keep the expected host while the request is pending approval (later
    // "approved" response) or accepted (the RoomSnapshot that follows the
    // acceptance still has to pass the same host check).
    const auto st = static_cast<JoinStatus>(resp.status);
    if (st != JoinStatus::PendingApproval && st != JoinStatus::Accepted)
        m_pendingJoinHost.remove(resp.roomId);
    emit joinResult(resp.roomId, st, resp.message);
}

void RoomManager::handleSnapshot(SecureChannel *ch, const QByteArray &payload) {
    RoomSnapshotMsg snap;
    if (!RoomSnapshotMsg::fromPayload(payload, snap))
        return;
    // Authoritative snapshot: accept only from the room host (or the host we
    // are currently joining).
    const RoomLocal existing = room(snap.roomId);
    const QString expectHost = existing.roomId.isEmpty() ? m_pendingJoinHost.value(snap.roomId) : existing.hostFp;
    if (!ch || expectHost.isEmpty() || ch->peerFingerprint() != expectHost) {
        if (qEnvironmentVariableIsSet("MESHLINK_DEBUG"))
            fprintf(stderr, "[room] SNAPSHOT rejected room=%s from %s (host %s)\n", qUtf8Printable(snap.roomId),
                    ch ? qUtf8Printable(ch->peerFingerprint().left(8)) : "-", qUtf8Printable(expectHost.left(8)));
        return;
    }
    m_pendingJoinHost.remove(snap.roomId); // join handshake completed
    QSqlQuery rq = m_storage->query("INSERT INTO rooms (room_id,name,intro,invite_code,policy,global_mute,capacity,host_fp,hosted,joined,created_ts)"
                                    " VALUES (?,?,?,?,?,?,?,?,0,1,?) ON CONFLICT(room_id) DO UPDATE SET"
                                    " name=excluded.name,intro=excluded.intro,invite_code=excluded.invite_code,"
                                    " policy=excluded.policy,global_mute=excluded.global_mute,"
                                    " capacity=excluded.capacity,joined=1");
    rq.bindValue(0, snap.roomId);
    rq.bindValue(1, snap.name);
    rq.bindValue(2, snap.intro);
    rq.bindValue(3, snap.inviteCode);
    rq.bindValue(4, int(snap.policy));
    rq.bindValue(5, snap.globalMute ? 1 : 0);
    rq.bindValue(6, snap.capacity);
    rq.bindValue(7, ch ? ch->peerFingerprint() : QString());
    rq.bindValue(8, nowMs());
    rq.exec();

    // Replace member roster (authoritative snapshot, BR-12)
    QSqlQuery del = m_storage->query("DELETE FROM members WHERE room_id=?");
    del.bindValue(0, snap.roomId);
    del.exec();
    for (const auto &m : snap.members) {
        QSqlQuery iq = m_storage->query("INSERT OR REPLACE INTO members (room_id,fingerprint,nickname,role,muted_until,joined_ts) VALUES (?,?,?,?,?,?)");
        iq.bindValue(0, snap.roomId);
        iq.bindValue(1, m.fp);
        iq.bindValue(2, m.nickname);
        iq.bindValue(3, int(m.role));
        iq.bindValue(4, m.mutedUntil);
        iq.bindValue(5, nowMs());
        iq.exec();
    }
    for (const auto &m : snap.recent) {
        QSqlQuery iq = m_storage->query("INSERT OR REPLACE INTO messages (msg_id,room_id,seq,sender_fp,sender_nick,avatar_hash,kind,ts,status,text,content_hash,file_name,mime,file_size,img_w,img_h)"
                                        " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        iq.bindValue(0, m.msgId);
        iq.bindValue(1, snap.roomId);
        iq.bindValue(2, qlonglong(m.seq));
        iq.bindValue(3, m.senderFp);
        iq.bindValue(4, m.senderNick);
        iq.bindValue(5, m.avatarHash);
        iq.bindValue(6, int(m.kind));
        iq.bindValue(7, m.ts);
        iq.bindValue(8, int(MsgStatus::Delivered));
        iq.bindValue(9, m.text);
        iq.bindValue(10, m.contentHash);
        iq.bindValue(11, m.fileName);
        iq.bindValue(12, m.mime);
        iq.bindValue(13, m.fileSize);
        iq.bindValue(14, m.imgW);
        iq.bindValue(15, m.imgH);
        iq.exec();
    }
    emit membersChanged(snap.roomId);
    emit roomsChanged();
    emit joinResult(snap.roomId, JoinStatus::Accepted, QStringLiteral("已加入"));
}

// ---------------- UC-13 leave ----------------
void RoomManager::leaveRoom(const QString &roomId, std::function<void(bool, QString)> cb) {
    const RoomLocal r = room(roomId);
    if (r.hosted) {
        // UC-13 step 5: host leaving closes the room (V1 has no host migration).
        broadcastRoom(roomId, MsgType::RoomClosed, payloadOf([&](QDataStream &ds) { ds << roomId; }));
        QSqlQuery q1 = m_storage->query("DELETE FROM rooms WHERE room_id=?");
        q1.bindValue(0, roomId);
        q1.exec();
        for (const char *t : {"members", "blacklist", "approvals"}) {
            QSqlQuery q = m_storage->query(QString("DELETE FROM %1 WHERE room_id=?").arg(t));
            q.bindValue(0, roomId);
            q.exec();
        }
        emit roomsChanged();
        cb(true, QStringLiteral("聊天室已关闭"));
        return;
    }
    // send leave via TLS (UC-13 step 2)
    QByteArray bytes = payloadOf([&](QDataStream &ds) { ds << roomId << m_acct->profile().fingerprint; });
    m_send(r.hostFp, MsgType::LeaveRoom, bytes);
    QSqlQuery q = m_storage->query("UPDATE rooms SET joined=0 WHERE room_id=?");
    q.bindValue(0, roomId);
    q.exec();
    QSqlQuery dq = m_storage->query("DELETE FROM members WHERE room_id=? AND fingerprint=?");
    dq.bindValue(0, roomId);
    dq.bindValue(1, m_acct->profile().fingerprint);
    dq.exec();
    emit roomsChanged();
    cb(true, QString());
}

void RoomManager::onPeerGone(const QString &fp) {
    // BR-09: hosted room becomes unavailable when the host is offline.
    QSqlQuery q = m_storage->query("SELECT 1 FROM rooms WHERE joined=1 AND host_fp=?");
    q.bindValue(0, fp);
    q.exec();
    if (q.next())
        emit roomsChanged();
}

// ---------------- UC-07 admin ----------------
bool RoomManager::canManage(const QString &roomId, const QString &actorFp, const QString &targetFp, AdminOp op,
                            QString *why) {
    QSqlQuery aq = m_storage->query("SELECT role FROM members WHERE room_id=? AND fingerprint=?");
    aq.bindValue(0, roomId);
    aq.bindValue(1, actorFp);
    aq.exec();
    if (!aq.next()) {
        if (why)
            *why = QStringLiteral("操作者不是成员");
        return false;
    }
    const auto actorRole = static_cast<MemberRole>(aq.value(0).toInt());

    QSqlQuery tq = m_storage->query("SELECT role FROM members WHERE room_id=? AND fingerprint=?");
    tq.bindValue(0, roomId);
    tq.bindValue(1, targetFp);
    tq.exec();
    const bool targetIsMember = tq.next();
    const auto targetRole = targetIsMember ? static_cast<MemberRole>(tq.value(0).toInt()) : MemberRole::Member;

    switch (op) {
    case AdminOp::SetAdmin:
    case AdminOp::UnsetAdmin:
    case AdminOp::GlobalMuteOn:
    case AdminOp::GlobalMuteOff:
        if (actorRole != MemberRole::Host) {
            if (why)
                *why = QStringLiteral("仅托管者可执行该操作"); // UC-07 table
            return false;
        }
        break;
    default:
        if (actorRole == MemberRole::Member) {
            if (why)
                *why = QStringLiteral("权限不足");
            return false;
        }
        break;
    }
    // A targeted op must address a real member or an already-blacklisted node
    // (prevents poisoning the blacklist with arbitrary strings, UC-07).
    if (op != AdminOp::GlobalMuteOn && op != AdminOp::GlobalMuteOff && op != AdminOp::Unblacklist &&
        op != AdminOp::Unmute && op != AdminOp::Blacklist && !targetIsMember) {
        QSqlQuery bl = m_storage->query("SELECT 1 FROM blacklist WHERE room_id=? AND fingerprint=?");
        bl.bindValue(0, roomId);
        bl.bindValue(1, targetFp);
        bl.exec();
        if (!bl.next()) {
            if (why)
                *why = QStringLiteral("目标节点不在该聊天室中");
            return false;
        }
    }
    // BR-13: admins cannot operate on host or other admins; nobody operates on self for these.
    if (actorRole == MemberRole::Admin && (targetRole == MemberRole::Host || targetRole == MemberRole::Admin)) {
        if (why)
            *why = QStringLiteral("管理员不可操作托管者或其他管理员"); // E1
        return false;
    }
    if (actorFp == targetFp && op != AdminOp::GlobalMuteOn && op != AdminOp::GlobalMuteOff) {
        if (why)
            *why = QStringLiteral("不能对自己执行该操作");
        return false;
    }
    if (op == AdminOp::SetAdmin && targetRole == MemberRole::Host) {
        if (why)
            *why = QStringLiteral("托管者无需设为管理员");
        return false;
    }
    return true;
}

AdminOpResult RoomManager::applyAdminOp(const QString &roomId, const QString &actorFp, const QString &targetFp,
                                        AdminOp op, qint64 durationMs) {
    AdminOpResult res;
    QString why;
    if (!canManage(roomId, actorFp, targetFp, op, &why)) {
        res.ok = false;
        res.error = why;
        return res;
    }
    // snapshot target info BEFORE mutation (kick/blacklist delete the row)
    QString tNick;
    int tRole = int(MemberRole::Member);
    {
        QSqlQuery nq = m_storage->query("SELECT nickname,role FROM members WHERE room_id=? AND fingerprint=?");
        nq.bindValue(0, roomId);
        nq.bindValue(1, targetFp);
        nq.exec();
        if (nq.next()) {
            tNick = nq.value(0).toString();
            tRole = nq.value(1).toInt();
        }
    }
    auto ev = [&](MemberEventMsg &m) {
        m.roomId = roomId;
        m.targetFp = targetFp;
        m.targetNick = tNick;
        m.role = quint8(tRole);
        m.op = static_cast<quint8>(op);
    };
    auto setRole = [&](int role) {
        QSqlQuery q = m_storage->query("UPDATE members SET role=? WHERE room_id=? AND fingerprint=?");
        q.bindValue(0, role);
        q.bindValue(1, roomId);
        q.bindValue(2, targetFp);
        q.exec();
    };
    auto setMuted = [&](qint64 until) {
        QSqlQuery q = m_storage->query("UPDATE members SET muted_until=? WHERE room_id=? AND fingerprint=?");
        q.bindValue(0, until);
        q.bindValue(1, roomId);
        q.bindValue(2, targetFp);
        q.exec();
    };
    switch (op) {
    case AdminOp::SetAdmin:
        setRole(int(MemberRole::Admin));
        break;
    case AdminOp::UnsetAdmin:
        setRole(int(MemberRole::Member));
        break;
    case AdminOp::Mute: // UC-07: timed (10min/1h) or until manual
        setMuted(durationMs > 0 ? nowMs() + durationMs : -1);
        break;
    case AdminOp::Unmute:
        setMuted(0);
        break;
    case AdminOp::Kick: {
        QSqlQuery q = m_storage->query("DELETE FROM members WHERE room_id=? AND fingerprint=?");
        q.bindValue(0, roomId);
        q.bindValue(1, targetFp);
        q.exec();
        break;
    }
    case AdminOp::Blacklist: { // BR-14: keyed by fingerprint
        QSqlQuery q = m_storage->query("INSERT OR REPLACE INTO blacklist (room_id,fingerprint) VALUES (?,?)");
        q.bindValue(0, roomId);
        q.bindValue(1, targetFp);
        const bool bins = q.exec();
        if (qEnvironmentVariableIsSet("MESHLINK_DEBUG"))
            fprintf(stderr, "[room] blacklist insert ok=%d err=%s\n", int(bins), qUtf8Printable(q.lastError().text()));
        QSqlQuery dq = m_storage->query("DELETE FROM members WHERE room_id=? AND fingerprint=?");
        dq.bindValue(0, roomId);
        dq.bindValue(1, targetFp);
        dq.exec();
        break;
    }
    case AdminOp::Unblacklist: {
        QSqlQuery q = m_storage->query("DELETE FROM blacklist WHERE room_id=? AND fingerprint=?");
        q.bindValue(0, roomId);
        q.bindValue(1, targetFp);
        q.exec();
        break;
    }
    case AdminOp::GlobalMuteOn:
    case AdminOp::GlobalMuteOff: {
        QSqlQuery q = m_storage->query("UPDATE rooms SET global_mute=? WHERE room_id=?");
        q.bindValue(0, op == AdminOp::GlobalMuteOn ? 1 : 0);
        q.bindValue(1, roomId);
        q.exec();
        break;
    }
    }
    MemberEventMsg m;
    ev(m);
    if (op == AdminOp::Mute) {
        m.mutedUntil = durationMs > 0 ? nowMs() + durationMs : -1;
    } else if (op == AdminOp::Unmute) {
        m.mutedUntil = 0;
    } else if (op == AdminOp::SetAdmin) {
        m.role = static_cast<quint8>(MemberRole::Admin);
    } else if (op == AdminOp::UnsetAdmin) {
        m.role = static_cast<quint8>(MemberRole::Member);
    }
    // notify target + all members (UC-07 step 5). Kick/blacklist removed the
    // target's row above, so the victim must be addressed explicitly — the
    // membership-derived roster alone would never reach them.
    {
        const QString self = m_acct->profile().fingerprint;
        const QByteArray bytes = m.toPayload();
        QStringList sent;
        for (const auto &mem : members(roomId)) {
            if (mem.fp == self || mem.fp == targetFp || sent.contains(mem.fp))
                continue;
            sent << mem.fp;
            m_send(mem.fp, MsgType::MemberEvent, bytes);
        }
        if (targetFp != self)
            m_send(targetFp, MsgType::MemberEvent, bytes);
    }
    emit membersChanged(roomId);
    res.ok = true;
    return res;
}

void RoomManager::requestAdminOp(const QString &roomId, const QString &targetFp, AdminOp op, qint64 durationMs,
                                 std::function<void(bool, QString)> cb) {
    const RoomLocal r = room(roomId);
    if (r.hosted) {
        auto res = applyAdminOp(roomId, m_acct->profile().fingerprint, targetFp, op, durationMs);
        cb(res.ok, res.error);
        return;
    }
    const QString key = roomId + "|" + targetFp + "|" + QString::number(int(op));
    m_pendingAdminOps[key] = cb;
    AdminOpMsg a;
    a.roomId = roomId;
    a.actorFp = m_acct->profile().fingerprint;
    a.targetFp = targetFp;
    a.op = static_cast<quint8>(op);
    a.durationMs = durationMs;
    m_send(r.hostFp, MsgType::AdminOp, a.toPayload());
}

void RoomManager::handleAdminOpRemote(SecureChannel *ch, const QByteArray &payload) {
    AdminOpMsg a;
    if (!AdminOpMsg::fromPayload(payload, a))
        return;
    // actor must be the authenticated channel peer (no impersonation)
    if (!ch || ch->peerFingerprint() != a.actorFp)
        return;
    if (qEnvironmentVariableIsSet("MESHLINK_DEBUG"))
        fprintf(stderr, "[room] remote op=%d actor=%s target=%s room=%s\n", int(a.op), qUtf8Printable(a.actorFp.left(8)),
                qUtf8Printable(a.targetFp.left(8)), qUtf8Printable(a.roomId));
    auto res = applyAdminOp(a.roomId, a.actorFp, a.targetFp, static_cast<AdminOp>(a.op), a.durationMs);
    if (qEnvironmentVariableIsSet("MESHLINK_DEBUG"))
        fprintf(stderr, "[room] remote op result ok=%d err=%s\n", int(res.ok), qUtf8Printable(res.error));
    if (!res.ok) { // UC-07 E1: permission denied -> Error frame
        ErrorMessageMsg e;
        e.reason = QStringLiteral("ADM|%1|%2|%3|%4").arg(a.roomId, a.targetFp, QString::number(int(a.op)), res.error);
        ch->send(MsgType::Error, e.toPayload());
    }
}

void RoomManager::handleAdminError(const QString &reason) {
    // ADM|room|target|op|message from the host (permission denied, UC-07 E1)
    const QStringList p = reason.split('|');
    if (p.size() < 5 || p[0] != QLatin1String("ADM"))
        return;
    const QString key = p[1] + "|" + p[2] + "|" + p[3];
    if (auto it = m_pendingAdminOps.find(key); it != m_pendingAdminOps.end()) {
        auto cb = it.value();
        m_pendingAdminOps.erase(it);
        cb(false, QStringList(p.mid(4)).join('|'));
    }
}

// ---------------- router ----------------
void RoomManager::handle(MsgType type, SecureChannel *ch, const QByteArray &payload) {
    switch (type) {
    case MsgType::JoinRequest:
        handleJoinRequest(ch, payload);
        break;
    case MsgType::JoinResponse:
        handleJoinResponse(ch, payload);
        break;
    case MsgType::RoomSnapshot:
        handleSnapshot(ch, payload);
        break;
    case MsgType::MemberEvent:
        handleMemberEvent(ch, payload);
        break;
    case MsgType::LeaveRoom: {
        QDataStream ds(payload);
        ds.setVersion(QDataStream::Qt_6_0);
        QString roomId, fp;
        ds >> roomId >> fp;
        if (!room(roomId).hosted)
            break;
        // NFR-13/14: a leave notification must come from the member itself.
        if (!ch || ch->peerFingerprint() != fp)
            break;
        QSqlQuery q = m_storage->query("DELETE FROM members WHERE room_id=? AND fingerprint=?");
        q.bindValue(0, roomId);
        q.bindValue(1, fp);
        q.exec();
        MemberEventMsg m;
        m.roomId = roomId;
        m.targetFp = fp;
        m.op = kEvLeft;
        broadcastRoom(roomId, MsgType::MemberEvent, m.toPayload());
        emit membersChanged(roomId);
        break;
    }
    case MsgType::RoomClosed: {
        QDataStream ds(payload);
        ds.setVersion(QDataStream::Qt_6_0);
        QString roomId;
        ds >> roomId;
        // NFR-13/14: only the hosting node may dissolve the room.
        const RoomLocal closed = room(roomId);
        if (!ch || closed.roomId.isEmpty() || ch->peerFingerprint() != closed.hostFp)
            break;
        QSqlQuery q = m_storage->query("UPDATE rooms SET joined=0 WHERE room_id=?");
        q.bindValue(0, roomId);
        q.exec();
        emit roomClosedByHost(roomId);
        emit roomsChanged();
        break;
    }
    case MsgType::RoomStateSync: {
        QDataStream ds(payload);
        ds.setVersion(QDataStream::Qt_6_0);
        QString roomId, fp;
        ds >> roomId >> fp;
        const RoomLocal r = room(roomId);
        if (!r.hosted || !ch)
            break;
        if (ch->peerFingerprint() != fp) // cannot pull another node's snapshot
            break;
        bool isMember = false;
        QSqlQuery q = m_storage->query("SELECT 1 FROM members WHERE room_id=? AND fingerprint=?");
        q.bindValue(0, roomId);
        q.bindValue(1, fp);
        q.exec();
        isMember = q.next();
        if (isMember) {
            sendSnapshotTo(fp, roomId);
        } else {
            MemberEventMsg m;
            m.roomId = roomId;
            m.targetFp = fp;
            m.op = static_cast<quint8>(AdminOp::Kick);
            m.reason = QStringLiteral("你已不再是该聊天室成员");
            ch->send(MsgType::MemberEvent, m.toPayload());
        }
        break;
    }
    case MsgType::AdminOp:
        handleAdminOpRemote(ch, payload);
        break;
    default:
        break; // not a room message
    }
}

void RoomManager::handleMemberEvent(SecureChannel *ch, const QByteArray &payload) {
    MemberEventMsg m;
    if (!MemberEventMsg::fromPayload(payload, m))
        return;
    // NFR-13/14: role/mute/kick/join events are authoritative host statements.
    const RoomLocal evRoom = room(m.roomId);
    if (!ch || evRoom.roomId.isEmpty() || evRoom.hostFp.isEmpty() || ch->peerFingerprint() != evRoom.hostFp) {
        if (qEnvironmentVariableIsSet("MESHLINK_DEBUG"))
            fprintf(stderr, "[room] MEMBEREVENT rejected room=%s from %s\n", qUtf8Printable(m.roomId),
                    ch ? qUtf8Printable(ch->peerFingerprint().left(8)) : "-");
        return;
    }
    const QString self = m_acct->profile().fingerprint;
    // remote requester learns its op was applied (success ack, UC-07 step 5)
    const QString okKey = m.roomId + "|" + m.targetFp + "|" + QString::number(int(m.op));
    if (auto it = m_pendingAdminOps.find(okKey); it != m_pendingAdminOps.end()) {
        auto cb = it.value();
        m_pendingAdminOps.erase(it);
        cb(true, QString());
    }
    if (m.targetFp == self) {
        const auto op = static_cast<AdminOp>(m.op);
        if (op == AdminOp::Kick || op == AdminOp::Blacklist) { // kicked: close + reason (UC-07 step 6)
            QSqlQuery q = m_storage->query("DELETE FROM members WHERE room_id=? AND fingerprint=?");
            q.bindValue(0, m.roomId);
            q.bindValue(1, self);
            q.exec();
            QSqlQuery rq = m_storage->query("UPDATE rooms SET joined=0 WHERE room_id=?");
            rq.bindValue(0, m.roomId);
            rq.exec();
            emit kickedFromRoom(m.roomId, m.reason.isEmpty() ? QStringLiteral("你已被移出聊天室") : m.reason);
            emit roomsChanged();
            return;
        }
    }
    // apply roster change locally (sync via encrypted channel)
    if (m.op == kEvLeft) {
        QSqlQuery q = m_storage->query("DELETE FROM members WHERE room_id=? AND fingerprint=?");
        q.bindValue(0, m.roomId);
        q.bindValue(1, m.targetFp);
        q.exec();
    } else if (m.op == kEvJoined) {
        upsertMember(m.roomId, m.targetFp, m.targetNick.isEmpty() ? m.targetFp.left(8) : m.targetNick, m.role);
    } else {
        QSqlQuery q = m_storage->query("UPDATE members SET role=?, muted_until=? WHERE room_id=? AND fingerprint=?");
        q.bindValue(0, int(m.role));
        q.bindValue(1, m.mutedUntil);
        q.bindValue(2, m.roomId);
        q.bindValue(3, m.targetFp);
        q.exec();
    }
    emit membersChanged(m.roomId);
    const QString key = m.roomId + "|" + m.targetFp + "|" + QString::number(int(m.op));
    if (auto it = m_pendingAdminOps.find(key); it != m_pendingAdminOps.end()) {
        auto cb = it.value();
        m_pendingAdminOps.erase(it);
        cb(true, QString());
    }
}

void RoomManager::broadcastRoom(const QString &roomId, MsgType type, const QByteArray &payload,
                                const QString &exceptFp) {
    for (const auto &m : members(roomId)) {
        if (m.fp == m_acct->profile().fingerprint || m.fp == exceptFp)
            continue;
        m_send(m.fp, type, payload); // fire-and-forget; heartbeat/state-sync repairs gaps
    }
}

void RoomManager::resyncAll() {
    const QString self = m_acct->profile().fingerprint;
    for (const auto &r : myRooms()) {
        if (r.hosted || !r.joined)
            continue;
        QByteArray bytes = payloadOf([&](QDataStream &ds) { ds << r.roomId << self; });
        m_send(r.hostFp, MsgType::RoomStateSync, bytes);
    }
}

} // namespace meshlink
