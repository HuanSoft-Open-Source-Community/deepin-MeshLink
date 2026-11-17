// deepin-MeshLink — chat implementation (UC-08).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "chatmanager.h"

#include "account.h"
#include "crypto.h"
#include "roommanager.h"
#include "settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QSqlError>

namespace meshlink {

// ---------------- MessageStore ----------------
namespace MessageStore {

bool append(Storage *s, const Msg &m) {
    QSqlQuery q = s->query("INSERT OR REPLACE INTO messages (msg_id,room_id,seq,sender_fp,sender_nick,avatar_hash,kind,ts,status,text,content_hash,file_name,mime,file_size,img_w,img_h,reply_to,recalled,recalled_by,recalled_ts,recv_ts)"
                           " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    q.bindValue(0, m.msgId);
    q.bindValue(1, m.roomId);
    q.bindValue(2, qlonglong(m.seq));
    q.bindValue(3, m.senderFp);
    q.bindValue(4, m.senderNick);
    q.bindValue(5, m.avatarHash);
    q.bindValue(6, int(m.kind));
    q.bindValue(7, m.ts);
    q.bindValue(8, int(m.status));
    q.bindValue(9, m.text);
    q.bindValue(10, m.contentHash);
    q.bindValue(11, m.fileName);
    q.bindValue(12, m.mime);
    q.bindValue(13, m.fileSize);
    q.bindValue(14, m.imgW);
    q.bindValue(15, m.imgH);
    q.bindValue(16, m.replyTo);
    q.bindValue(17, m.recalled ? 1 : 0);
    q.bindValue(18, m.recalledBy);
    q.bindValue(19, m.recalledTs);
    q.bindValue(20, m.recvTs);
    if (!q.exec()) {
        qWarning("MessageStore::append failed: %s", qUtf8Printable(q.lastError().text()));
        return false;
    }
    return true;
}

QVector<Msg> latest(Storage *s, const QString &roomId, int limit) {
    QVector<Msg> out;
    QSqlQuery q = s->query("SELECT msg_id,seq,sender_fp,sender_nick,avatar_hash,kind,ts,status,text,content_hash,file_name,mime,file_size,img_w,img_h,reply_to,recalled,recalled_by,recalled_ts,recv_ts"
                           " FROM messages WHERE room_id=? ORDER BY seq DESC LIMIT ?");
    q.bindValue(0, roomId);
    q.bindValue(1, limit);
    q.exec();
    while (q.next()) {
        Msg m;
        m.roomId = roomId;
        m.msgId = q.value(0).toString();
        m.seq = q.value(1).toULongLong();
        m.senderFp = q.value(2).toString();
        m.senderNick = q.value(3).toString();
        m.avatarHash = q.value(4).toString();
        m.kind = quint8(q.value(5).toInt());
        m.ts = q.value(6).toLongLong();
        m.status = quint8(q.value(7).toInt());
        m.text = q.value(8).toString();
        m.contentHash = q.value(9).toString();
        m.fileName = q.value(10).toString();
        m.mime = q.value(11).toString();
        m.fileSize = q.value(12).toLongLong();
        m.imgW = q.value(13).toInt();
        m.imgH = q.value(14).toInt();
        m.replyTo = q.value(15).toString();
        m.recalled = q.value(16).toInt() != 0;
        m.recalledBy = q.value(17).toString();
        m.recalledTs = q.value(18).toLongLong();
        m.recvTs = q.value(19).toLongLong();
        out.prepend(m);
    }
    return out;
}

bool find(Storage *s, const QString &roomId, const QString &msgId, Msg &out) {
    QSqlQuery q = s->query("SELECT msg_id,seq,sender_fp,sender_nick,avatar_hash,kind,ts,status,text,content_hash,file_name,mime,file_size,img_w,img_h,reply_to,recalled,recalled_by,recalled_ts,recv_ts"
                           " FROM messages WHERE room_id=? AND msg_id=?");
    q.bindValue(0, roomId);
    q.bindValue(1, msgId);
    q.exec();
    if (!q.next())
        return false;
    out.roomId = roomId;
    out.msgId = q.value(0).toString();
    out.seq = q.value(1).toULongLong();
    out.senderFp = q.value(2).toString();
    out.senderNick = q.value(3).toString();
    out.avatarHash = q.value(4).toString();
    out.kind = quint8(q.value(5).toInt());
    out.ts = q.value(6).toLongLong();
    out.status = quint8(q.value(7).toInt());
    out.text = q.value(8).toString();
    out.contentHash = q.value(9).toString();
    out.fileName = q.value(10).toString();
    out.mime = q.value(11).toString();
    out.fileSize = q.value(12).toLongLong();
    out.imgW = q.value(13).toInt();
    out.imgH = q.value(14).toInt();
    out.replyTo = q.value(15).toString();
    out.recalled = q.value(16).toInt() != 0;
    out.recalledBy = q.value(17).toString();
    out.recalledTs = q.value(18).toLongLong();
    out.recvTs = q.value(19).toLongLong();
    return true;
}

bool markRecalled(Storage *s, const QString &roomId, const QString &msgId,
                  const QString &byFp, qint64 ts) {
    QSqlQuery q = s->query("UPDATE messages SET recalled=1,recalled_by=?,recalled_ts=?,"
                           "text='',content_hash='',file_name='',mime='',file_size=0,img_w=0,img_h=0"
                           " WHERE room_id=? AND msg_id=?");
    q.bindValue(0, byFp);
    q.bindValue(1, ts);
    q.bindValue(2, roomId);
    q.bindValue(3, msgId);
    if (!q.exec()) {
        qWarning("MessageStore::markRecalled failed: %s", qUtf8Printable(q.lastError().text()));
        return false;
    }
    return q.numRowsAffected() > 0;
}

quint64 maxSeq(Storage *s, const QString &roomId) {
    QSqlQuery q = s->query("SELECT MAX(seq) FROM messages WHERE room_id=?");
    q.bindValue(0, roomId);
    q.exec();
    if (q.next())
        return q.value(0).toULongLong();
    return 0;
}

bool setStatus(Storage *s, const QString &roomId, const QString &msgId, quint8 status) {
    QSqlQuery q = s->query("UPDATE messages SET status=? WHERE room_id=? AND msg_id=?");
    q.bindValue(0, int(status));
    q.bindValue(1, roomId);
    q.bindValue(2, msgId);
    return q.exec();
}

} // namespace MessageStore

// ---------------- ChatManager ----------------
ChatManager::ChatManager(AccountManager *acct, Storage *storage, RoomManager *rooms, SecureLink *link,
                         QObject *parent)
    : QObject(parent), m_acct(acct), m_storage(storage), m_rooms(rooms), m_link(link) {}

QString ChatManager::blobPath(const QString &hash) { return Paths::blobDir() + "/" + hash; }
bool ChatManager::hasBlob(const QString &hash) { return QFile::exists(blobPath(hash)); }

QString ChatManager::sendText(const QString &roomId, const QString &text, QString *error,
                              const QString &replyTo) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return {};
    if (trimmed.size() > 4000) {
        if (error)
            *error = tr("Message is too long (≤4000 characters)");
        return {};
    }
    qint64 until = 0;
    if (m_rooms->isMuted(roomId, &until)) { // UC-08 precondition: 未被禁言
        if (error) {
            *error = until == -1 ? tr("You are muted")
                                 : tr("You are muted for %1 more seconds").arg(qint64(until - nowMs()) / 1000);
        }
        return {};
    }
    const RoomLocal r = m_rooms->room(roomId);
    if (!replyTo.isEmpty()) {
        Msg quoted;
        if (!MessageStore::find(m_storage, roomId, replyTo, quoted)) {
            if (error)
                *error = tr("The quoted message is not available");
            return {};
        }
    }

    Msg m;
    m.roomId = roomId;
    m.msgId = QString::fromLatin1(Crypto::randomHex(8).c_str());
    m.senderFp = m_acct->profile().fingerprint;
    m.senderNick = m_acct->profile().nickname;
    m.avatarHash = m_acct->profile().avatarHash;
    m.kind = static_cast<quint8>(MsgKind::Text);
    m.ts = nowMs();
    m.status = static_cast<quint8>(MsgStatus::Pending);
    m.text = trimmed;
    m.replyTo = replyTo;
    m.recvTs = nowMs();
    MessageStore::append(m_storage, m); // optimistic local echo

    ChatMessageMsg cm;
    cm.roomId = roomId;
    cm.msgId = m.msgId;
    cm.senderFp = m.senderFp;
    cm.senderNick = m.senderNick;
    cm.avatarHash = m.avatarHash;
    cm.kind = m.kind;
    cm.ts = m.ts;
    cm.text = m.text;
    cm.replyTo = m.replyTo;

    if (r.hosted) {
        hostReceive(cm, nullptr); // self is the authoritative host
        emit messageStatusChanged(roomId, m.msgId, static_cast<quint8>(MsgStatus::Delivered)); // host echo
    } else if (!m_send || !m_send(r.hostFp, MsgType::ChatMessage, cm.toPayload())) {
        MessageStore::setStatus(m_storage, roomId, m.msgId, static_cast<quint8>(MsgStatus::Failed));
        emit messageStatusChanged(roomId, m.msgId, static_cast<quint8>(MsgStatus::Failed)); // UC-08 E1 未送达
        if (error)
            *error = tr("Message not delivered (host unreachable)");
    } else {
        m.status = static_cast<quint8>(MsgStatus::Sent);
        MessageStore::setStatus(m_storage, roomId, m.msgId, m.status);
    }
    emit messageReceived(roomId, m);
    return m.msgId;
}

QString ChatManager::sendImage(const QString &roomId, const QString &imagePath, QString *error,
                               const QString &replyTo) {
    QFile f(imagePath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = tr("The image could not be read");
        return {};
    }
    const QByteArray bytes = f.readAll();
    if (bytes.size() > 16 * 1024 * 1024) {
        if (error)
            *error = tr("The image is too large (≤16 MiB)");
        return {};
    }
    qint64 until = 0;
    if (m_rooms->isMuted(roomId, &until)) {
        if (error)
            *error = tr("You are muted");
        return {};
    }
    QImageReader ir(imagePath);
    const QSize sz = ir.size();

    const QString hash = QString::fromLatin1(
        Crypto::sha256Hex(reinterpret_cast<const uint8_t *>(bytes.constData()), size_t(bytes.size())).c_str());
    QDir().mkpath(Paths::blobDir());
    QFile bf(blobPath(hash));
    if (bf.open(QIODevice::WriteOnly))
        bf.write(bytes);

    Msg m;
    m.roomId = roomId;
    m.msgId = QString::fromLatin1(Crypto::randomHex(8).c_str());
    m.senderFp = m_acct->profile().fingerprint;
    m.senderNick = m_acct->profile().nickname;
    m.avatarHash = m_acct->profile().avatarHash;
    m.kind = static_cast<quint8>(MsgKind::Image);
    m.ts = nowMs();
    m.status = static_cast<quint8>(MsgStatus::Pending);
    m.contentHash = hash; // BR-15: no binary in the message body
    m.fileName = QFileInfo(imagePath).fileName();
    m.mime = QStringLiteral("image/*");
    m.fileSize = bytes.size();
    m.imgW = sz.width();
    m.imgH = sz.height();

    const RoomLocal r = m_rooms->room(roomId);
    if (!replyTo.isEmpty()) {
        Msg quoted;
        if (!MessageStore::find(m_storage, roomId, replyTo, quoted)) {
            if (error)
                *error = tr("The quoted message is not available");
            return {};
        }
    }
    m.replyTo = replyTo;
    m.recvTs = nowMs();
    MessageStore::append(m_storage, m);

    ChatMessageMsg cm;
    cm.roomId = roomId;
    cm.msgId = m.msgId;
    cm.senderFp = m.senderFp;
    cm.senderNick = m.senderNick;
    cm.avatarHash = m.avatarHash;
    cm.kind = m.kind;
    cm.ts = m.ts;
    cm.contentHash = hash;
    cm.fileName = m.fileName;
    cm.mime = m.mime;
    cm.fileSize = m.fileSize;
    cm.imgW = m.imgW;
    cm.imgH = m.imgH;
    cm.replyTo = m.replyTo;

    if (r.hosted) {
        hostReceive(cm, nullptr);
        emit messageStatusChanged(roomId, m.msgId, static_cast<quint8>(MsgStatus::Delivered)); // host echo
    } else if (!m_send || !m_send(r.hostFp, MsgType::ChatMessage, cm.toPayload())) {
        MessageStore::setStatus(m_storage, roomId, m.msgId, static_cast<quint8>(MsgStatus::Failed));
        if (error)
            *error = tr("Image message not delivered");
    } else {
        MessageStore::setStatus(m_storage, roomId, m.msgId, static_cast<quint8>(MsgStatus::Sent));
    }
    emit messageReceived(roomId, m);
    return m.msgId;
}

// ---- host (authoritative) path ----
void ChatManager::hostReceive(const ChatMessageMsg &cm, SecureChannel *ch) {
    // sender must be who the channel says (no spoofing) and a permitted speaker
    const QString sender = ch ? ch->peerFingerprint() : cm.senderFp;
    if (sender != cm.senderFp)
        return;
    // A late retransmission must never resurrect a recalled tombstone.
    {
        QSqlQuery existing = m_storage->query(
            "SELECT recalled FROM messages WHERE room_id=? AND msg_id=?");
        existing.bindValue(0, cm.roomId);
        existing.bindValue(1, cm.msgId);
        existing.exec();
        if (existing.next() && existing.value(0).toInt() != 0) {
            if (ch) {
                ChatAckMsg ack;
                ack.msgId = cm.msgId;
                ack.delivered = true;
                ch->send(MsgType::ChatAck, ack.toPayload());
            }
            return;
        }
    }
    if (m_rooms->isMuted(cm.roomId)) {
        if (ch) { // reject: ack undelivered (UC-07 step 6 greyed input handled by client)
            ChatAckMsg a;
            a.msgId = cm.msgId;
            a.delivered = false;
            ch->send(MsgType::ChatAck, a.toPayload());
            emit notice(cm.roomId, tr("A muted member tried to send a message"));
        }
        return;
    }

    Msg m;
    m.roomId = cm.roomId;
    m.msgId = cm.msgId;
    m.senderFp = cm.senderFp;
    m.senderNick = cm.senderNick;
    m.avatarHash = cm.avatarHash;
    m.kind = cm.kind;
    m.ts = cm.ts;
    m.seq = MessageStore::maxSeq(m_storage, cm.roomId) + 1; // BR-16: host assigns order
    m.status = static_cast<quint8>(MsgStatus::Delivered);
    m.text = cm.text;
    m.contentHash = cm.contentHash;
    m.fileName = cm.fileName;
    m.mime = cm.mime;
    m.fileSize = cm.fileSize;
    m.imgW = cm.imgW;
    m.imgH = cm.imgH;
    m.replyTo = cm.replyTo;
    m.recvTs = nowMs();
    MessageStore::append(m_storage, m);
    // host pulls image bytes on demand too (BR-15: render + future relay cache)
    if (m.kind == static_cast<quint8>(MsgKind::Image) && !m.contentHash.isEmpty())
        requestBlob(m.contentHash, m.senderFp);

    // relay with authoritative seq (UC-08 step 5)
    ChatMessageMsg out = cm;
    out.seq = m.seq;
    const auto ms = m_rooms->members(cm.roomId);
    for (const auto &mem : ms) {
        if (mem.fp == m_acct->profile().fingerprint || mem.fp == cm.senderFp)
            continue;
        m_send(mem.fp, MsgType::ChatMessage, out.toPayload());
    }
    // ack sender
    if (ch) {
        ChatAckMsg a;
        a.msgId = cm.msgId;
        a.seq = m.seq;
        a.delivered = true;
        ch->send(MsgType::ChatAck, a.toPayload());
    } else {
        MessageStore::setStatus(m_storage, cm.roomId, cm.msgId, static_cast<quint8>(MsgStatus::Delivered));
        emit messageStatusChanged(cm.roomId, cm.msgId, static_cast<quint8>(MsgStatus::Delivered));
    }
    emit messageReceived(cm.roomId, m);
}

QVector<Msg> ChatManager::history(const QString &roomId, int limit) const {
    return MessageStore::latest(m_storage, roomId, limit);
}

Msg ChatManager::message(const QString &roomId, const QString &msgId) const {
    Msg out;
    MessageStore::find(m_storage, roomId, msgId, out);
    return out;
}

bool ChatManager::recallMessage(const QString &roomId, const QString &msgId, QString *error) {
    Msg message;
    if (!MessageStore::find(m_storage, roomId, msgId, message)) {
        if (error)
            *error = tr("The message does not exist");
        return false;
    }
    if (message.recalled)
        return true;

    const RoomLocal room = m_rooms->room(roomId);
    if (!room.joined) {
        if (error)
            *error = tr("The room is unavailable");
        return false;
    }

    const QString self = m_acct->profile().fingerprint;
    const MemberRole role = m_rooms->roleOf(roomId, self);
    const bool moderator = role == MemberRole::Host || role == MemberRole::Admin;
    if (!moderator && self != message.senderFp) {
        if (error)
            *error = tr("Insufficient permissions");
        return false;
    }

    const qint64 received = message.recvTs > 0 ? message.recvTs : message.ts;
    if (!canRecallMessage(role, self, message.senderFp, received, nowMs(),
                          selfRecallWindowMs())) {
        if (error)
            *error = tr("The recall window has expired");
        return false;
    }

    // A failed local echo never reached the host; recall it locally.
    if (self == message.senderFp && message.status == static_cast<quint8>(MsgStatus::Failed)) {
        if (!MessageStore::markRecalled(m_storage, roomId, msgId, self, nowMs())) {
            if (error)
                *error = tr("Could not recall the message");
            return false;
        }
        emit messageRecalled(roomId, msgId, self);
        return true;
    }

    if (room.hosted)
        return hostRecall(roomId, msgId, self, error);

    ChatRecallMsg request;
    request.roomId = roomId;
    request.msgId = msgId;
    if (!m_send || !m_send(room.hostFp, MsgType::ChatRecall, request.toPayload())) {
        if (error)
            *error = tr("The host is unreachable");
        return false;
    }
    return true;
}

bool ChatManager::hostRecall(const QString &roomId, const QString &msgId,
                             const QString &actorFp, QString *error) {
    const RoomLocal room = m_rooms->room(roomId);
    if (!room.hosted) {
        if (error)
            *error = tr("The room is unavailable");
        return false;
    }
    if (!m_rooms->isMember(roomId, actorFp)) {
        if (error)
            *error = tr("Not a member of this room");
        return false;
    }

    Msg message;
    if (!MessageStore::find(m_storage, roomId, msgId, message)) {
        if (error)
            *error = tr("The message does not exist");
        return false;
    }
    if (message.recalled)
        return true;

    const bool moderator = m_rooms->isModerator(roomId, actorFp);
    if (!moderator && actorFp != message.senderFp) {
        if (error)
            *error = tr("Insufficient permissions");
        return false;
    }
    const qint64 received = message.recvTs > 0 ? message.recvTs : message.ts;
    const MemberRole actorRole = m_rooms->roleOf(roomId, actorFp);
    if (!canRecallMessage(actorRole, actorFp, message.senderFp, received, nowMs(),
                          selfRecallWindowMs())) {
        if (error)
            *error = tr("The recall window has expired");
        return false;
    }

    if (!MessageStore::markRecalled(m_storage, roomId, msgId, actorFp, nowMs())) {
        if (error)
            *error = tr("Could not recall the message");
        return false;
    }
    emit messageRecalled(roomId, msgId, actorFp);

    ChatRecallMsg event;
    event.roomId = roomId;
    event.msgId = msgId;
    const auto members = m_rooms->members(roomId);
    for (const auto &member : members) {
        if (member.fp == m_acct->profile().fingerprint)
            continue;
        m_send(member.fp, MsgType::ChatRecall, event.toPayload());
    }
    return true;
}

void ChatManager::handleRecall(SecureChannel *ch, const QByteArray &payload) {
    ChatRecallMsg recall;
    if (!ch || !ChatRecallMsg::fromPayload(payload, recall))
        return;
    const RoomLocal room = m_rooms->room(recall.roomId);
    if (room.roomId.isEmpty())
        return;

    if (!room.hosted) {
        // A relayed recall must come from our current room host.
        if (ch->peerFingerprint() != room.hostFp)
            return;
        Msg message;
        if (!MessageStore::find(m_storage, recall.roomId, recall.msgId, message))
            return;
        if (message.recalled)
            return;
        if (MessageStore::markRecalled(m_storage, recall.roomId, recall.msgId,
                                       room.hostFp, nowMs()))
            emit messageRecalled(recall.roomId, recall.msgId, room.hostFp);
        return;
    }

    const QString actor = ch->peerFingerprint();
    QString error;
    ChatRecallAckMsg ack;
    ack.roomId = recall.roomId;
    ack.msgId = recall.msgId;
    ack.ok = hostRecall(recall.roomId, recall.msgId, actor, &error);
    ack.error = error;
    ch->send(MsgType::ChatRecallAck, ack.toPayload());
}

void ChatManager::handleRecallAck(SecureChannel *ch, const QByteArray &payload) {
    ChatRecallAckMsg ack;
    if (!ch || !ChatRecallAckMsg::fromPayload(payload, ack))
        return;
    const RoomLocal room = m_rooms->room(ack.roomId);
    if (room.roomId.isEmpty() || ch->peerFingerprint() != room.hostFp)
        return;
    if (!ack.ok) {
        emit recallFailed(ack.roomId, ack.msgId, ack.error);
        return;
    }
    Msg message;
    if (!MessageStore::find(m_storage, ack.roomId, ack.msgId, message) || message.recalled)
        return;
    if (MessageStore::markRecalled(m_storage, ack.roomId, ack.msgId,
                                   room.hostFp, nowMs()))
        emit messageRecalled(ack.roomId, ack.msgId, room.hostFp);
}

void ChatManager::handle(MsgType type, SecureChannel *ch, const QByteArray &payload) {
    switch (type) {
    case MsgType::ChatMessage: {
        ChatMessageMsg cm;
        if (!ChatMessageMsg::fromPayload(payload, cm))
            break;
        const RoomLocal r = m_rooms->room(cm.roomId);
        if (r.hosted && ch) {
            hostReceive(cm, ch); // UC-08 step 3-4 (self host)
        } else if (!r.hosted) {
            // relayed to member (UC-08 step 6) — must come from the host
            if (ch && ch->peerFingerprint() != r.hostFp)
                break;
            Msg m;
            m.roomId = cm.roomId;
            m.msgId = cm.msgId;
            m.senderFp = cm.senderFp;
            m.senderNick = cm.senderNick;
            m.avatarHash = cm.avatarHash;
            m.kind = cm.kind;
            m.seq = cm.seq;
            m.ts = cm.ts;
            m.status = static_cast<quint8>(MsgStatus::Delivered);
            m.text = cm.text;
            m.contentHash = cm.contentHash;
            m.fileName = cm.fileName;
            m.mime = cm.mime;
            m.fileSize = cm.fileSize;
            m.imgW = cm.imgW;
            m.imgH = cm.imgH;
            m.replyTo = cm.replyTo;
            QSqlQuery ex = m_storage->query("SELECT 1 FROM messages WHERE room_id=? AND msg_id=?");
            ex.bindValue(0, m.roomId);
            ex.bindValue(1, m.msgId);
            ex.exec();
            if (ex.next())
                break; // dedupe (snapshot + live relay)
            MessageStore::append(m_storage, m);
            emit messageReceived(m.roomId, m);
            // UC-08 step 6: image bytes are fetched on demand (BR-15)
            if (m.kind == static_cast<quint8>(MsgKind::Image) && !m.contentHash.isEmpty())
                requestBlob(m.contentHash, m.senderFp);
        }
        break;
    }
    case MsgType::ChatAck: {
        ChatAckMsg a;
        if (!ChatAckMsg::fromPayload(payload, a))
            break;
        const quint8 st = a.delivered ? static_cast<quint8>(MsgStatus::Delivered) : static_cast<quint8>(MsgStatus::Failed);
        QSqlQuery q = m_storage->query("SELECT room_id FROM messages WHERE msg_id=?");
        q.bindValue(0, a.msgId);
        QString roomId;
        q.exec();
        if (q.next())
            roomId = q.value(0).toString();
        if (roomId.isEmpty())
            break;
        // NFR-13/14: delivery receipts come from the room host only.
        const RoomLocal ackRoom = m_rooms->room(roomId);
        if (!ch || ackRoom.roomId.isEmpty() || ch->peerFingerprint() != ackRoom.hostFp)
            break;
        MessageStore::setStatus(m_storage, roomId, a.msgId, st);
        emit messageStatusChanged(roomId, a.msgId, st);
        break;
    }
    case MsgType::ChatRecall:
        handleRecall(ch, payload);
        break;
    case MsgType::ChatRecallAck:
        handleRecallAck(ch, payload);
        break;
    case MsgType::BlobRequest:
        serveBlob(ch, payload);
        break;
    case MsgType::BlobData: {
        BlobDataMsg b;
        if (!BlobDataMsg::fromPayload(payload, b) || b.data.isEmpty() || b.hash.size() != 64)
            break;
        // hash must match content (supply chain sanity)
        const QString actual = QString::fromLatin1(
            Crypto::sha256Hex(reinterpret_cast<const uint8_t *>(b.data.constData()), size_t(b.data.size())).c_str());
        if (actual != b.hash)
            break;
        QDir().mkpath(Paths::blobDir());
        QFile f(blobPath(b.hash));
        if (f.open(QIODevice::WriteOnly)) {
            f.write(b.data);
            emit blobReady(b.hash);
        }
        break;
    }
    default:
        break;
    }
}

void ChatManager::requestBlob(const QString &hash, const QString &fromFp) {
    if (hasBlob(hash)) {
        emit blobReady(hash);
        return;
    }
    if (fromFp == m_acct->profile().fingerprint)
        return;
    BlobRequestMsg b;
    b.hash = hash;
    m_send(fromFp, MsgType::BlobRequest, b.toPayload());
}

void ChatManager::serveBlob(SecureChannel *ch, const QByteArray &payload) {
    BlobRequestMsg b;
    if (!BlobRequestMsg::fromPayload(payload, b) || !ch)
        return;
    QFile f(blobPath(b.hash));
    // Only serve files whose name in the blob store == their content hash;
    // avatars and chat images ride this same path (BR-06, UC-08 step 6).
    if (b.hash.size() != 64 || !f.open(QIODevice::ReadOnly) || f.size() > 16 * 1024 * 1024)
        return;
    BlobDataMsg d;
    d.hash = b.hash;
    d.data = f.readAll();
    ch->send(MsgType::BlobData, d.toPayload());
}

} // namespace meshlink
