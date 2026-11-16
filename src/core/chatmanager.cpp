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
    QSqlQuery q = s->query("INSERT OR REPLACE INTO messages (msg_id,room_id,seq,sender_fp,sender_nick,avatar_hash,kind,ts,status,text,content_hash,file_name,mime,file_size,img_w,img_h)"
                           " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
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
    return q.exec();
}

QVector<Msg> latest(Storage *s, const QString &roomId, int limit) {
    QVector<Msg> out;
    QSqlQuery q = s->query("SELECT msg_id,seq,sender_fp,sender_nick,avatar_hash,kind,ts,status,text,content_hash,file_name,mime,file_size,img_w,img_h"
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
        out.prepend(m);
    }
    return out;
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

QString ChatManager::sendText(const QString &roomId, const QString &text, QString *error) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return {};
    if (trimmed.size() > 4000) {
        if (error)
            *error = QStringLiteral("消息过长（≤4000 字符）");
        return {};
    }
    qint64 until = 0;
    if (m_rooms->isMuted(roomId, &until)) { // UC-08 precondition: 未被禁言
        if (error) {
            *error = until == -1 ? QStringLiteral("你已被禁言")
                                 : QStringLiteral("你已被禁言，剩余 %1 秒").arg(qint64(until - nowMs()) / 1000);
        }
        return {};
    }
    const RoomLocal r = m_rooms->room(roomId);

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

    if (r.hosted) {
        hostReceive(cm, nullptr); // self is the authoritative host
        emit messageStatusChanged(roomId, m.msgId, static_cast<quint8>(MsgStatus::Delivered)); // host echo
    } else if (!m_send || !m_send(r.hostFp, MsgType::ChatMessage, cm.toPayload())) {
        MessageStore::setStatus(m_storage, roomId, m.msgId, static_cast<quint8>(MsgStatus::Failed));
        emit messageStatusChanged(roomId, m.msgId, static_cast<quint8>(MsgStatus::Failed)); // UC-08 E1 未送达
        if (error)
            *error = QStringLiteral("消息未送达（托管节点不可达）");
    } else {
        m.status = static_cast<quint8>(MsgStatus::Sent);
        MessageStore::setStatus(m_storage, roomId, m.msgId, m.status);
    }
    emit messageReceived(roomId, m);
    return m.msgId;
}

QString ChatManager::sendImage(const QString &roomId, const QString &imagePath, QString *error) {
    QFile f(imagePath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("图片无法读取");
        return {};
    }
    const QByteArray bytes = f.readAll();
    if (bytes.size() > 16 * 1024 * 1024) {
        if (error)
            *error = QStringLiteral("图片过大（≤16 MiB）");
        return {};
    }
    qint64 until = 0;
    if (m_rooms->isMuted(roomId, &until)) {
        if (error)
            *error = QStringLiteral("你已被禁言");
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

    const RoomLocal r = m_rooms->room(roomId);
    if (r.hosted) {
        hostReceive(cm, nullptr);
        emit messageStatusChanged(roomId, m.msgId, static_cast<quint8>(MsgStatus::Delivered)); // host echo
    } else if (!m_send || !m_send(r.hostFp, MsgType::ChatMessage, cm.toPayload())) {
        MessageStore::setStatus(m_storage, roomId, m.msgId, static_cast<quint8>(MsgStatus::Failed));
        if (error)
            *error = QStringLiteral("图片消息未送达");
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
    if (m_rooms->isMuted(cm.roomId)) {
        if (ch) { // reject: ack undelivered (UC-07 step 6 greyed input handled by client)
            ChatAckMsg a;
            a.msgId = cm.msgId;
            a.delivered = false;
            ch->send(MsgType::ChatAck, a.toPayload());
            emit notice(cm.roomId, QStringLiteral("有成员在禁言期间尝试发送消息"));
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
