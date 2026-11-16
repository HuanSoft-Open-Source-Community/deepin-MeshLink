// deepin-MeshLink — file transfer: provider + chunked downloader (UC-10/11/12).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
//
// 不请求不传输 (BR-19): nothing here starts a transfer before an explicit
// local download request. Resume is driven by the *persisted per-chunk
// completion table*, never by byte offsets (BR-23). Progress rows bind to the
// content hash so chunks can be stitched across suppliers (BR-22).
// Verified files are delivered under the final name only after SHA-256 of the
// merged file matches the announcement (BR-24).
#pragma once

#include "common.h"
#include "protocol.h"
#include "securelink.h"
#include "storage.h"

#include <QAtomicInt>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QThreadPool>

#include <memory>

namespace meshlink {

class AccountManager;
class Settings;

struct DownloadTaskView {
    QString hash, name, mime, roomId, destPath, error;
    qint64 size = 0;
    quint64 chunkSize = 0, totalChunks = 0, doneChunks = 0;
    TaskStatus status = TaskStatus::Queued;
    QString currentSource;
};

class Downloader : public QObject {
    Q_OBJECT
public:
    Downloader(AccountManager *acct, Storage *storage, Settings *settings, SecureLink *link,
               QObject *parent = nullptr);

    // UC-10 steps 4-6: metadata handshake with the chosen supplier, then
    // create/persist the task and start parallel chunk workers (UC-11).
    void startTask(const QString &roomId, const QString &hash, const QString &name, qint64 size,
                   const QString &mime, const QVector<SupplierInfo> &suppliers);
    void pause(const QString &hash);
    void resume(const QString &hash, const QVector<SupplierInfo> &suppliers); // may be empty -> stored list
    void cancel(const QString &hash, bool deletePartialChunks);

    QList<DownloadTaskView> tasks() const;
    DownloadTaskView task(const QString &hash) const;

    // ---- provider side (serving other nodes) ----
    void serveFileRequest(SecureChannel *ch, const QByteArray &payload);
    void serveChunkRequest(SecureChannel *ch, const QByteArray &payload);

    // call at startup: loads persisted tasks, clears half-written temps (UC-11 E2)
    void recoverTasks();
    // a supplier that was marked dead is back online -> revive waiting tasks (UC-11 E1 recovery)
    void supplierSeen(const QString &fp);

    static QString chunkDir(const QString &hash);
    static QString chunkFile(const QString &hash, quint64 idx);

signals:
    void taskChanged(const QString &hash);
    void taskCompleted(const QString &hash, const QString &destPath); // UC-12 step 3
    void taskFailed(const QString &hash, const QString &why);
    // completion makes this node a candidate supplier (BR-18); engine wires it
    void becameSupplier(const QString &roomId, const QString &hash);

private:
    struct JobCtx {
        QString hash;
        QAtomicInt paused{0};
        QAtomicInt cancelled{0};
        quint64 chunkSize = 0;
        qint64 fileSize = 0;
        quint64 totalChunks = 0;
        bool haveChunkHashes = false;
        QVector<QString> chunkHashes; // per-chunk sha256 hex (may be empty)
        QVector<SupplierInfo> suppliers;
        QSet<QString> deadSuppliers;
        QString currentSource;        // supplier label shown in the transfer panel
        int round = 0; // verification rounds for UC-12 E1
        bool needReMeta = false;      // UC-12 E2: re-handshake with a different supplier
        QString metaSupplier;         // supplier whose chunk hashes we currently trust
    };
    struct Claim {
        QSet<quint64> claimed;
    };

    void setStatusDb(const QString &hash, quint8 st, const QString &error = {});
    void requestMeta(const QString &hash);
    void dispatchNext(const QString &hash);
    void beginVerify(const QString &hash);
    void finalizeOk(const QString &hash, const QString &mergedTmp);
    void handleCorrupt(const QString &hash);
    void handleCorruptWithBadList(const QString &hash, const QJsonArray &badList);
    void applyMeta(const QString &hash, const QJsonObject &meta);
    void onMetaFailure(const QString &hash);
    void loadSuppliersJson(const QString &hash, QVector<SupplierInfo> &out) const;
    std::shared_ptr<Claim> claims(const QString &hash);
    void updateProgress(const QString &hash);

    AccountManager *m_acct;
    Storage *m_storage;
    Settings *m_settings;
    SecureLink *m_link;
    QThreadPool m_pool;
    QHash<QString, std::shared_ptr<JobCtx>> m_ctx;
    QHash<QString, std::shared_ptr<Claim>> m_claims;
    QMutex m_claimMutex;
};

} // namespace meshlink
