// deepin-MeshLink — file announcement & supplier registry (UC-09/10).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
//
// Announce-only semantics (BR-18/19): a hash/name/size record moves through
// the host relay like any UC-08 message; bytes are served solely on explicit
// request, later, by the downloader protocol.
#pragma once

#include "common.h"
#include "protocol.h"
#include "securelink.h"
#include "storage.h"

#include <QObject>

#include <functional>

namespace meshlink {

class AccountManager;
class RoomManager;
class Downloader;

struct AnnouncedFile {
    QString fileHash, fileName, mime, announcerFp, announcerNick;
    qint64 fileSize = 0, ts = 0;
};

class FileManager : public QObject {
    Q_OBJECT
public:
    using SendFn = std::function<bool(const QString &, MsgType, const QByteArray &)>;
    using ResolveFn = std::function<bool(const QString &fp, QString &ip, quint16 &port, bool &online)>;

    FileManager(AccountManager *acct, Storage *storage, RoomManager *rooms, Downloader *downloader,
               QObject *parent = nullptr);
    void setSend(SendFn fn) { m_send = std::move(fn); }
    void setResolve(ResolveFn fn) { m_resolve = std::move(fn); }

    // UC-09: hash the file (streaming, pool thread), then announce via host.
    // cb(msgId_or_empty, error) — announced asynchronously; hash runs off-GUI.
    void announceFile(const QString &roomId, const QString &path,
                      std::function<void(const QString &, const QString &)> cb);
    // BR-18: completing a download makes us a supplier -> register with host.
    void registerAsSupplier(const QString &roomId, const QString &hash);

    // UC-10 step 1-2: ask host (or act as host) for candidate suppliers.
    void querySuppliers(const QString &roomId, const QString &hash,
                        std::function<void(const QVector<SupplierInfo> &)> cb);

    QVector<AnnouncedFile> announcements(const QString &roomId) const;
    QVector<SupplierInfo> suppliersLocal(const QString &hash) const;
    bool hasVerifiedFile(const QString &hash) const; // BR-17 dedupe hint

    void handle(MsgType type, SecureChannel *ch, const QByteArray &payload);

signals:
    void announced(const QString &roomId, const AnnouncedFile &f); // file feed refresh

private:
    void announceHashed(const QString &roomId, const QString &path, const QString &hash, const QString &name,
                        qint64 size, const QString &mime, std::function<void(const QString &, const QString &)> cb);
    void hostReceiveAnnounce(const FileAnnounceMsg &fa, SecureChannel *ch);
    void respondSuppliers(SecureChannel *ch, const QString &hash);
    AccountManager *m_acct;
    Storage *m_storage;
    RoomManager *m_rooms;
    Downloader *m_downloader;
    SendFn m_send;
    ResolveFn m_resolve;
    QHash<QString, std::function<void(const QVector<SupplierInfo> &)>> m_pendingSupplierCb; // hash -> cb
    QHash<QString, QString> m_pendingSupplierHost; // hash -> host fp that must answer
};

} // namespace meshlink
