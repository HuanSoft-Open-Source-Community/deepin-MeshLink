// deepin-MeshLink — file announcement & supplier registry (UC-09/10).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "filemanager.h"

#include "account.h"
#include "chatmanager.h"
#include "crypto.h"
#include "downloader.h"
#include "roommanager.h"
#include "settings.h"

#include <QFileInfo>
#include <QMimeDatabase>
#include <QSqlError>
#include <QtConcurrent>

namespace meshlink {

FileManager::FileManager(AccountManager *acct, Storage *storage, RoomManager *rooms, Downloader *downloader,
                         QObject *parent)
    : QObject(parent), m_acct(acct), m_storage(storage), m_rooms(rooms), m_downloader(downloader) {}

QVector<AnnouncedFile> FileManager::announcements(const QString &roomId) const {
    QVector<AnnouncedFile> out;
    QSqlQuery q = m_storage->query("SELECT file_hash,name,size,mime,announcer_fp,ts FROM file_announce WHERE room_id=? ORDER BY ts DESC");
    q.bindValue(0, roomId);
    q.exec();
    while (q.next()) {
        AnnouncedFile f;
        f.fileHash = q.value(0).toString();
        f.fileName = q.value(1).toString();
        f.fileSize = q.value(2).toLongLong();
        f.mime = q.value(3).toString();
        f.announcerFp = q.value(4).toString();
        f.ts = q.value(5).toLongLong();
        QSqlQuery aq = m_storage->query("SELECT nickname FROM messages WHERE content_hash=? LIMIT 1");
        aq.bindValue(0, f.fileHash);
        aq.exec();
        if (aq.next())
            f.announcerNick = aq.value(0).toString();
        out.append(f);
    }
    return out;
}

QVector<SupplierInfo> FileManager::suppliersLocal(const QString &hash) const {
    QVector<SupplierInfo> out;
    QSqlQuery q = m_storage->query("SELECT fingerprint,nickname,ip,port FROM suppliers WHERE file_hash=?");
    q.bindValue(0, hash);
    q.exec();
    while (q.next()) {
        SupplierInfo s;
        s.fp = q.value(0).toString();
        s.nickname = q.value(1).toString();
        s.ip = q.value(2).toString();
        s.port = quint16(q.value(3).toInt());
        s.online = false;
        if (m_resolve)
            m_resolve(s.fp, s.ip, s.port, s.online); // refresh live endpoint
        out.append(s);
    }
    return out;
}

void FileManager::announceFile(const QString &roomId, const QString &path,
                               std::function<void(const QString &, const QString &)> cb) {
    const QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile()) {
        cb(QString(), QStringLiteral("文件不存在"));
        return;
    }
    const QString name = fi.fileName();
    const qint64 size = fi.size();
    const QString mime = QMimeDatabase().mimeTypeForFile(path).name();

    // SHA-256 streaming in the pool (NFR-03: hashing must not block the GUI)
    QPointer<FileManager> guard(this);
    (void)QtConcurrent::run([guard, path, roomId, name, size, mime, cb]() {
        if (!guard)
            return;
        QString hash;
        {
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly)) {
                QMetaObject::invokeMethod(guard.data(), [cb]() { cb(QString(), QStringLiteral("文件不可读")); },
                                          Qt::QueuedConnection);
                return;
            }
            QCryptographicHash h(QCryptographicHash::Sha256);
            while (!f.atEnd())
                h.addData(f.read(1 << 20));
            hash = QString::fromLatin1(h.result().toHex());
        }
        QMetaObject::invokeMethod(
            guard.data(),
            [guard, roomId, path, name, size, mime, hash, cb]() {
                if (guard)
                    guard->announceHashed(roomId, path, hash, name, size, mime, cb);
            },
            Qt::QueuedConnection);
    });
}

void FileManager::announceHashed(const QString &roomId, const QString &path, const QString &hash, const QString &name,
                                qint64 size, const QString &mime,
                                std::function<void(const QString &, const QString &)> cb) {
    const RoomLocal r = m_rooms->room(roomId);
    if (!r.joined) {
        cb(QString(), QStringLiteral("请先加入聊天室"));
        return;
    }
    qint64 until = 0;
    if (m_rooms->isMuted(roomId, &until)) {
        cb(QString(), QStringLiteral("你已被禁言")); // 禁言覆盖文件通告（属消息流）
        return;
    }
    // BR-18: announcing registers *this* node as a holder/supplier.
    QSqlQuery ins = m_storage->query("INSERT OR REPLACE INTO local_files (file_hash,dest_path,name,size,mime,added_ts) VALUES (?,?,?,?,?,?)");
    ins.bindValue(0, hash);
    ins.bindValue(1, path);
    ins.bindValue(2, name);
    ins.bindValue(3, size);
    ins.bindValue(4, mime);
    ins.bindValue(5, nowMs());
    ins.exec();

    const auto prof = m_acct->profile();
    FileAnnounceMsg fa;
    fa.roomId = roomId;
    fa.msgId = QStringLiteral("f-") + QString::fromLatin1(Crypto::randomHex(6).c_str());
    fa.fileHash = hash;
    fa.fileName = name;
    fa.mime = mime;
    fa.announcerFp = prof.fingerprint;
    fa.announcerNick = prof.nickname;
    fa.avatarHash = prof.avatarHash;
    fa.fileSize = size;
    fa.ts = nowMs();
    fa.mode = 0;

    if (r.hosted) {
        hostReceiveAnnounce(fa, nullptr); // self host: authoritative store + fan-out
    } else if (!m_send || !m_send(r.hostFp, MsgType::FileAnnounce, fa.toPayload())) {
        cb(QString(), QStringLiteral("通告未送达（托管节点不可达）")); // UC-09 异常: host offline
        return;
    }
    cb(fa.msgId, QString());
}

// host (authoritative) path — also used for pure supplier registrations (mode 1)
void FileManager::hostReceiveAnnounce(const FileAnnounceMsg &fa, SecureChannel *ch) {
    const RoomLocal r = m_rooms->room(fa.roomId);
    if (!r.hosted)
        return;
    if (ch && ch->peerFingerprint() != fa.announcerFp) // no spoofed announcers
        return;
    // announcer must currently be a member
    bool member = false;
    for (const auto &m : m_rooms->members(fa.roomId))
        if (m.fp == fa.announcerFp)
            member = true;
    if (!member)
        return;

    QString ip;
    quint16 port = 0;
    bool online = true;
    if (m_resolve)
        m_resolve(fa.announcerFp, ip, port, online);

    QSqlQuery reg = m_storage->query("INSERT OR REPLACE INTO file_announce (file_hash,room_id,name,size,mime,announcer_fp,ts) VALUES (?,?,?,?,?,?,?)");
    reg.bindValue(0, fa.fileHash);
    reg.bindValue(1, fa.roomId);
    reg.bindValue(2, fa.fileName);
    reg.bindValue(3, fa.fileSize);
    reg.bindValue(4, fa.mime);
    reg.bindValue(5, fa.announcerFp);
    reg.bindValue(6, fa.ts);
    reg.exec();
    QSqlQuery sup = m_storage->query("INSERT OR REPLACE INTO suppliers (file_hash,fingerprint,nickname,ip,port) VALUES (?,?,?,?,?)");
    sup.bindValue(0, fa.fileHash);
    sup.bindValue(1, fa.announcerFp);
    sup.bindValue(2, fa.announcerNick);
    sup.bindValue(3, ip);
    sup.bindValue(4, int(port));
    sup.exec();

    if (fa.mode == 1) // registration only: no feed message
        return;

    Msg m;
    m.roomId = fa.roomId;
    m.msgId = fa.msgId;
    m.senderFp = fa.announcerFp;
    m.senderNick = fa.announcerNick;
    m.avatarHash = fa.avatarHash;
    m.kind = static_cast<quint8>(MsgKind::File); // UC-09 step 6: 以特殊聊天室消息形式随 UC-08 通路分发
    m.seq = MessageStore::maxSeq(m_storage, fa.roomId) + 1;
    m.ts = fa.ts;
    m.status = static_cast<quint8>(MsgStatus::Delivered);
    m.contentHash = fa.fileHash;
    m.fileName = fa.fileName;
    m.mime = fa.mime;
    m.fileSize = fa.fileSize;
    MessageStore::append(m_storage, m);

    const auto ms = m_rooms->members(fa.roomId);
    for (const auto &mem : ms) {
        if (mem.fp == fa.announcerFp || mem.fp == m_acct->profile().fingerprint)
            continue;
        m_send(mem.fp, MsgType::FileAnnounce, fa.toPayload());
    }
    if (ch) {
        ChatAckMsg a;
        a.msgId = fa.msgId;
        a.seq = m.seq;
        a.delivered = true;
        ch->send(MsgType::ChatAck, a.toPayload());
    }
    emit announced(fa.roomId, {fa.fileHash, fa.fileName, fa.mime, fa.announcerFp, fa.announcerNick, fa.fileSize, fa.ts});
}

void FileManager::registerAsSupplier(const QString &roomId, const QString &hash) {
    const RoomLocal r = m_rooms->room(roomId);
    QSqlQuery info = m_storage->query("SELECT name,size,mime FROM file_announce WHERE file_hash=? AND room_id=?");
    info.bindValue(0, hash);
    info.bindValue(1, roomId);
    info.exec();
    if (!info.next())
        return; // not announced here anymore: nothing to register
    const auto prof = m_acct->profile();
    FileAnnounceMsg fa;
    fa.roomId = roomId;
    fa.msgId = QStringLiteral("r-") + QString::fromLatin1(Crypto::randomHex(6).c_str());
    fa.fileHash = hash;
    fa.fileName = info.value(0).toString();
    fa.fileSize = info.value(1).toLongLong();
    fa.mime = info.value(2).toString();
    fa.announcerFp = prof.fingerprint;
    fa.announcerNick = prof.nickname;
    fa.avatarHash = prof.avatarHash;
    fa.ts = nowMs();
    fa.mode = 1;
    if (r.hosted) {
        hostReceiveAnnounce(fa, nullptr);
    } else {
        m_send(r.hostFp, MsgType::FileAnnounce, fa.toPayload());
    }
}

// ---- supplier discovery (UC-10 steps 1-2) ----
void FileManager::querySuppliers(const QString &roomId, const QString &hash,
                                 std::function<void(const QVector<SupplierInfo> &)> cb) {
    const RoomLocal r = m_rooms->room(roomId);
    if (r.hosted || r.roomId.isEmpty()) {
        // act as (or ask whoever might be) the registry: use local table
        cb(suppliersLocal(hash));
        return;
    }
    m_pendingSupplierCb[hash] = cb;
    m_pendingSupplierHost[hash] = r.hostFp;
    SuppliersQueryMsg q;
    q.fileHash = hash;
    if (!m_send || !m_send(r.hostFp, MsgType::SuppliersQuery, q.toPayload())) {
        m_pendingSupplierCb.remove(hash);
        m_pendingSupplierHost.remove(hash);
        cb({}); // host unreachable -> treated as "waiting for supply" upstream
    }
}

void FileManager::respondSuppliers(SecureChannel *ch, const QString &hash) {
    if (!ch)
        return;
    SuppliersResponseMsg resp;
    resp.fileHash = hash;
    resp.suppliers = suppliersLocal(hash);
    ch->send(MsgType::SuppliersResponse, resp.toPayload());
}

bool FileManager::hasVerifiedFile(const QString &hash) const {
    QSqlQuery q = m_storage->query("SELECT dest_path FROM local_files WHERE file_hash=?");
    q.bindValue(0, hash);
    q.exec();
    return q.next() && QFile::exists(q.value(0).toString());
}

void FileManager::handle(MsgType type, SecureChannel *ch, const QByteArray &payload) {
    switch (type) {
    case MsgType::FileAnnounce: {
        FileAnnounceMsg fa;
        if (!FileAnnounceMsg::fromPayload(payload, fa))
            break;
        const RoomLocal r = m_rooms->room(fa.roomId);
        if (r.hosted && ch) {
            hostReceiveAnnounce(fa, ch);
        } else if (!r.hosted && ch && ch->peerFingerprint() == r.hostFp) {
            // relayed announcement from our host: store + feed (UC-09 step 6-7)
            QString ip;
            quint16 port = 0;
            bool online = false;
            if (m_resolve)
                m_resolve(fa.announcerFp, ip, port, online);
            QSqlQuery reg = m_storage->query("INSERT OR REPLACE INTO file_announce (file_hash,room_id,name,size,mime,announcer_fp,ts) VALUES (?,?,?,?,?,?,?)");
            reg.bindValue(0, fa.fileHash);
            reg.bindValue(1, fa.roomId);
            reg.bindValue(2, fa.fileName);
            reg.bindValue(3, fa.fileSize);
            reg.bindValue(4, fa.mime);
            reg.bindValue(5, fa.announcerFp);
            reg.bindValue(6, fa.ts);
            reg.exec();
            QSqlQuery sup = m_storage->query("INSERT OR REPLACE INTO suppliers (file_hash,fingerprint,nickname,ip,port) VALUES (?,?,?,?,?)");
            sup.bindValue(0, fa.fileHash);
            sup.bindValue(1, fa.announcerFp);
            sup.bindValue(2, fa.announcerNick);
            sup.bindValue(3, ip);
            sup.bindValue(4, int(port));
            sup.exec();
            if (fa.mode == 0) {
                Msg m;
                m.roomId = fa.roomId;
                m.msgId = fa.msgId;
                m.senderFp = fa.announcerFp;
                m.senderNick = fa.announcerNick;
                m.avatarHash = fa.avatarHash;
                m.kind = static_cast<quint8>(MsgKind::File);
                m.ts = fa.ts;
                m.status = static_cast<quint8>(MsgStatus::Delivered);
                m.contentHash = fa.fileHash;
                m.fileName = fa.fileName;
                m.mime = fa.mime;
                m.fileSize = fa.fileSize;
                QSqlQuery ex = m_storage->query("SELECT 1 FROM messages WHERE room_id=? AND msg_id=?");
                ex.bindValue(0, m.roomId);
                ex.bindValue(1, m.msgId);
                ex.exec();
                if (!ex.next()) {
                    MessageStore::append(m_storage, m);
                    emit announced(fa.roomId, m.contentHash.isEmpty() ? AnnouncedFile{} : AnnouncedFile{
                                                                                     fa.fileHash, fa.fileName, fa.mime, fa.announcerFp, fa.announcerNick, fa.fileSize, fa.ts});
                }
            } else {
                emit announced(fa.roomId, AnnouncedFile{});
            }
        }
        break;
    }
    case MsgType::SuppliersQuery: {
        SuppliersQueryMsg q;
        if (!ch || !SuppliersQueryMsg::fromPayload(payload, q))
            break;
        // Only the room host owns the supplier registry (UC-10 step 2).
        QSqlQuery rq = m_storage->query("SELECT room_id FROM file_announce WHERE file_hash=? LIMIT 1");
        rq.bindValue(0, q.fileHash);
        rq.exec();
        if (!rq.next() || !m_rooms->room(rq.value(0).toString()).hosted)
            break;
        respondSuppliers(ch, q.fileHash);
        break;
    }
    case MsgType::SuppliersResponse: {
        SuppliersResponseMsg resp;
        if (!SuppliersResponseMsg::fromPayload(payload, resp))
            break;
        auto it = m_pendingSupplierCb.find(resp.fileHash);
        if (it == m_pendingSupplierCb.end())
            break;
        // NFR-13/14: only the host we queried may answer the supplier query.
        if (!ch || m_pendingSupplierHost.value(resp.fileHash) != ch->peerFingerprint())
            break;
        m_pendingSupplierHost.remove(resp.fileHash);
        const auto cb = it.value();
        m_pendingSupplierCb.erase(it);
        QVector<SupplierInfo> list = resp.suppliers;
        for (auto &s : list) {
            s.online = false;
            if (m_resolve)
                m_resolve(s.fp, s.ip, s.port, s.online); // fresh endpoints from discovery table
        }
        // cache for resume/redo after restart
        for (const auto &s : list) {
            QSqlQuery sup = m_storage->query("INSERT OR REPLACE INTO suppliers (file_hash,fingerprint,nickname,ip,port) VALUES (?,?,?,?,?)");
            sup.bindValue(0, resp.fileHash);
            sup.bindValue(1, s.fp);
            sup.bindValue(2, s.nickname);
            sup.bindValue(3, s.ip);
            sup.bindValue(4, int(s.port));
            sup.exec();
        }
        cb(list);
        break;
    }
    case MsgType::FileRequest:
        m_downloader->serveFileRequest(ch, payload);
        break;
    case MsgType::ChunkRequest:
        m_downloader->serveChunkRequest(ch, payload);
        break;
    default:
        break;
    }
}

} // namespace meshlink
