// deepin-MeshLink — downloader/provider implementation (UC-10/11/12).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "downloader.h"
#include <cstdio>

#include "account.h"
#include "chunkplan.h"
#include "crypto.h"
#include "settings.h"
#include "tlsutil.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QReadWriteLock>
#include <QSqlError>
#include <QThread>
#include <QtConcurrent>

namespace meshlink {

// ===================================================================
// per-worker-thread SQLite connection (QSqlDatabase is not shareable)
// ===================================================================
static QSqlDatabase workerDb() {
    const QString name = QStringLiteral("dl-%1").arg(reinterpret_cast<quintptr>(QThread::currentThread()));
    if (!QSqlDatabase::contains(name)) {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", name);
        db.setDatabaseName(Paths::databaseFile());
        db.open();
        QSqlQuery pragma(db);
        pragma.exec(QStringLiteral("PRAGMA busy_timeout=8000"));
    }
    return QSqlDatabase::database(name);
}

// ===================================================================
// BlockingPeer: synchronous TLS client for worker threads (no event loop)
// ===================================================================
namespace {

struct PeerMaterial {
    QString certPem, keyPem, fp, nickname, avatarHash;
    QByteArray edPub;
    std::function<bool(const QByteArray &, QByteArray *)> sign;
};

class BlockingPeer {
public:
    ~BlockingPeer() { close(); }

    bool connectTo(const PeerMaterial &mat, const QString &host, quint16 port, const QString &expectFp,
                   const QString &localCertFp, QString *err) {
        m_sock = new QSslSocket;
        m_sock->setSslConfiguration(makeMeshlinkTlsConfig(mat.certPem, mat.keyPem));
        m_sock->connectToHostEncrypted(host, port);
        if (!m_sock->waitForEncrypted(8000)) {
            *err = QStringLiteral("TLS handshake: %1").arg(m_sock->errorString()); if (qEnvironmentVariableIsSet("MESHLINK_DEBUG")) fprintf(stderr, "[dl] blocking-peer: %s\n", qUtf8Printable(*err));
            close();
            return false;
        }
        if (m_sock->sessionProtocol() != QSsl::TlsV1_3) { // NFR-10
            *err = QStringLiteral("peer did not negotiate TLS 1.3"); if (qEnvironmentVariableIsSet("MESHLINK_DEBUG")) fprintf(stderr, "[dl] blocking-peer: %s\n", qUtf8Printable(*err));
            close();
            return false;
        }
        const QSslCertificate peer = m_sock->peerCertificate();
        if (peer.isNull() || certCn(peer) != expectFp) { // cert binds to claimed fp
            *err = QStringLiteral("peer certificate does not bind fingerprint %1").arg(expectFp); if (qEnvironmentVariableIsSet("MESHLINK_DEBUG")) fprintf(stderr, "[dl] blocking-peer: %s\n", qUtf8Printable(*err));
            close();
            return false;
        }
        m_peerCertFp = certFpHex(peer);
        m_localCertFp = localCertFp;
        m_expectFp = expectFp;
        m_mat = &mat;

        // send AUTH
        AuthMsg auth;
        auth.fp = mat.fp;
        auth.nickname = mat.nickname;
        auth.avatarHash = mat.avatarHash;
        auth.ed25519Pub = mat.edPub;
        Bytes nb = Crypto::randomBytes(16);
        auth.nonce = QByteArray(reinterpret_cast<const char *>(nb.data()), int(nb.size()));
        if (!mat.sign(makeAuthContent(auth.fp, auth.nonce, m_peerCertFp), &auth.sig)) {
            *err = QStringLiteral("sign failed"); if (qEnvironmentVariableIsSet("MESHLINK_DEBUG")) fprintf(stderr, "[dl] blocking-peer: %s\n", qUtf8Printable(*err));
            close();
            return false;
        }
        m_sock->write(frameMessage(MsgType::Hello, auth.toPayload()));
        m_sock->flush();

        // wait peer AUTH
        MsgType t;
        QByteArray p;
        for (int guard = 0; guard < 16; ++guard) {
            if (!pumpOne(&t, &p, 8000, err)) {
                close();
                return false;
            }
            if (t == MsgType::Hello)
                break;
        }
        AuthMsg pa;
        if (!AuthMsg::fromPayload(p, pa)) {
            *err = QStringLiteral("malformed peer auth"); if (qEnvironmentVariableIsSet("MESHLINK_DEBUG")) fprintf(stderr, "[dl] blocking-peer: %s\n", qUtf8Printable(*err));
            close();
            return false;
        }
        const QString derived =
            QString::fromStdString(Crypto::fingerprintFromEd25519Pub(Bytes(pa.ed25519Pub.begin(), pa.ed25519Pub.end())));
        if (derived != pa.fp || pa.fp != m_expectFp) {
            *err = QStringLiteral("peer identity mismatch (MITM?)"); if (qEnvironmentVariableIsSet("MESHLINK_DEBUG")) fprintf(stderr, "[dl] blocking-peer: %s\n", qUtf8Printable(*err));
            close();
            return false;
        }
        if (certCn(peer) != pa.fp) {
            *err = QStringLiteral("cert/account binding failed"); if (qEnvironmentVariableIsSet("MESHLINK_DEBUG")) fprintf(stderr, "[dl] blocking-peer: %s\n", qUtf8Printable(*err));
            close();
            return false;
        }
        QByteArray content = makeAuthContent(pa.fp, pa.nonce, m_localCertFp);
        if (!Crypto::ed25519Verify(Bytes(pa.ed25519Pub.begin(), pa.ed25519Pub.end()), Bytes(content.begin(), content.end()),
                                   Bytes(pa.sig.begin(), pa.sig.end()))) {
            *err = QStringLiteral("bad auth signature"); if (qEnvironmentVariableIsSet("MESHLINK_DEBUG")) fprintf(stderr, "[dl] blocking-peer: %s\n", qUtf8Printable(*err));
            close();
            return false;
        }
        // TOFU (best effort on this thread's connection)
        {
            QSqlDatabase db = workerDb();
            QSqlQuery q(db);
            q.prepare("SELECT cert_fp FROM tofu WHERE fingerprint=?");
            q.bindValue(0, pa.fp);
            if (q.exec() && q.next()) {
                if (q.value(0).toString() != m_peerCertFp) {
                    *err = QStringLiteral("TOFU mismatch: suspected MITM"); if (qEnvironmentVariableIsSet("MESHLINK_DEBUG")) fprintf(stderr, "[dl] blocking-peer: %s\n", qUtf8Printable(*err));
                    close();
                    return false;
                }
            } else {
                QSqlQuery ins(db);
                ins.prepare("INSERT OR REPLACE INTO tofu (fingerprint,cert_fp,first_seen) VALUES (?,?,?)");
                ins.bindValue(0, pa.fp);
                ins.bindValue(1, m_peerCertFp);
                ins.bindValue(2, nowMs());
                ins.exec();
            }
        }
        m_ok = true;
        return true;
    }

    // Send one request, await the expected response type (interleaved frames ignored).
    bool roundtrip(MsgType req, const QByteArray &reqPayload, MsgType expect, QByteArray *resp, QString *err,
                   int timeoutMs = 20000) {
        if (!m_ok) {
            *err = QStringLiteral("not connected"); if (qEnvironmentVariableIsSet("MESHLINK_DEBUG")) fprintf(stderr, "[dl] blocking-peer: %s\n", qUtf8Printable(*err));
            return false;
        }
        m_sock->write(frameMessage(req, reqPayload));
        m_sock->flush();
        MsgType t;
        QByteArray p;
        qint64 deadline = nowMs() + timeoutMs;
        while (nowMs() < deadline) {
            if (!pumpOne(&t, &p, int(deadline - nowMs()), err))
                return false;
            if (t == expect) {
                *resp = p;
                return true;
            }
            if (t == MsgType::Error) {
                ErrorMessageMsg e;
                ErrorMessageMsg::fromPayload(p, e);
                *err = e.reason;
                return false;
            }
        }
        *err = QStringLiteral("request timeout"); if (qEnvironmentVariableIsSet("MESHLINK_DEBUG")) fprintf(stderr, "[dl] blocking-peer: %s\n", qUtf8Printable(*err));
        return false;
    }

    void close() {
        if (m_sock) {
            m_sock->abort();
            m_sock->deleteLater();
            m_sock = nullptr;
        }
        m_buf.clear();
        m_ok = false;
    }

private:
    bool pumpOne(MsgType *t, QByteArray *p, int timeoutMs, QString *err) {
        while (true) {
            int consumed = 0;
            if (tryParseFrame(m_buf, *t, *p, consumed)) {
                m_buf.remove(0, consumed);
                return true;
            }
            if (!m_sock->waitForReadyRead(timeoutMs)) {
                *err = QStringLiteral("read timeout / connection lost"); if (qEnvironmentVariableIsSet("MESHLINK_DEBUG")) fprintf(stderr, "[dl] blocking-peer: %s\n", qUtf8Printable(*err));
                return false;
            }
            m_buf.append(m_sock->readAll());
        }
    }

    QSslSocket *m_sock = nullptr;
    QByteArray m_buf, m_localBuf;
    QString m_peerCertFp, m_localCertFp, m_expectFp;
    const PeerMaterial *m_mat = nullptr;
    bool m_ok = false;
};

// sha256 of a byte range of a file, streaming.
bool fileRangeSha(QFile &f, qint64 off, qint64 len, QString *hexOut) {
    if (!f.seek(off))
        return false;
    QCryptographicHash h(QCryptographicHash::Sha256);
    const qint64 step = 1 << 20;
    qint64 left = len;
    while (left > 0) {
        const QByteArray chunk = f.read(qMin(left, step));
        if (chunk.isEmpty())
            return false;
        h.addData(chunk);
        left -= chunk.size();
    }
    *hexOut = QString::fromLatin1(h.result().toHex());
    return true;
}

bool hashFile(const QString &path, QString *hexOut) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    return fileRangeSha(f, 0, f.size(), hexOut);
}

} // namespace

// ===================================================================
// Downloader
// ===================================================================
Downloader::Downloader(AccountManager *acct, Storage *storage, Settings *settings, SecureLink *link, QObject *parent)
    : QObject(parent), m_acct(acct), m_storage(storage), m_settings(settings), m_link(link) {
    m_pool.setMaxThreadCount(qMax(1, settings->downloadThreads()));
}

QString Downloader::chunkDir(const QString &hash) { return Paths::downloadDir() + "/" + hash; }
QString Downloader::chunkFile(const QString &hash, quint64 idx) {
    return chunkDir(hash) + QStringLiteral("/c_%1.bin").arg(idx, 8, 10, QChar('0'));
}

void Downloader::setStatusDb(const QString &hash, quint8 st, const QString &error) {
    QSqlQuery q = m_storage->query("UPDATE downloads SET status=?, updated_ts=?, error=? WHERE file_hash=?");
    q.bindValue(0, int(st));
    q.bindValue(1, nowMs());
    q.bindValue(2, error);
    q.bindValue(3, hash);
    q.exec();
    emit taskChanged(hash);
}

void Downloader::loadSuppliersJson(const QString &hash, QVector<SupplierInfo> &out) const {
    QSqlQuery q = m_storage->query("SELECT suppliers_json FROM downloads WHERE file_hash=?");
    q.bindValue(0, hash);
    q.exec(); // Storage::query() only prepares
    if (!q.next())
        return;
    const QJsonArray arr = QJsonDocument::fromJson(q.value(0).toByteArray()).array();
    for (const auto &v : arr) {
        const QJsonObject o = v.toObject();
        SupplierInfo s;
        s.fp = o["fp"].toString();
        s.nickname = o["nick"].toString();
        s.ip = o["ip"].toString();
        s.port = quint16(o["port"].toInt());
        s.online = true;
        out.append(s);
    }
}

static QString suppliersJson(const QVector<SupplierInfo> &sup) {
    QJsonArray arr;
    for (const auto &s : sup) {
        QJsonObject o;
        o["fp"] = s.fp;
        o["nick"] = s.nickname;
        o["ip"] = s.ip;
        o["port"] = int(s.port);
        arr.append(o);
    }
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

// ---- UC-10 step 4/5 + UC-11 dispatch --------------------------------
void Downloader::startTask(const QString &roomId, const QString &hash, const QString &name, qint64 size,
                           const QString &mime, const QVector<SupplierInfo> &suppliers) {
    if (suppliers.isEmpty()) { // UC-10 E1: no supply yet
        setStatusDb(hash, static_cast<quint8>(TaskStatus::WaitingSupply), QStringLiteral("等待供给节点"));
        return;
    }
    auto ctx = m_ctx.value(hash);
    const bool fresh = !ctx;
    if (fresh) {
        ctx = std::make_shared<JobCtx>();
        ctx->hash = hash;
        ctx->chunkSize = clampChunkSize(m_settings->chunkSize());
        ctx->fileSize = size; // announced size; applyMeta() overrides with the supplier's value
        m_ctx[hash] = ctx;
        m_claims[hash] = std::make_shared<Claim>();
        QSqlQuery q = m_storage->query(
            "INSERT OR IGNORE INTO downloads (file_hash,room_id,name,size,mime,chunk_size,total_chunks,status,created_ts,updated_ts,suppliers_json)"
            " VALUES (?,?,?,?,?,?,0,?,?,?,?)");
        q.bindValue(0, hash);
        q.bindValue(1, roomId);
        q.bindValue(2, name);
        q.bindValue(3, size);
        q.bindValue(4, mime);
        q.bindValue(5, ctx->chunkSize);
        q.bindValue(6, int(TaskStatus::Queued));
        q.bindValue(7, nowMs());
        q.bindValue(8, nowMs());
        q.bindValue(9, suppliersJson(suppliers));
        q.exec();
    }
    ctx->suppliers = suppliers;
    ctx->deadSuppliers.clear();
    ctx->haveChunkHashes = false;
    ctx->chunkHashes.clear();
    setStatusDb(hash, static_cast<quint8>(TaskStatus::Queued));
    if (fresh)
        QDir().mkpath(chunkDir(hash));
    requestMeta(hash);
}

// Metadata handshake in the pool thread (no GUI-thread network I/O, NFR-03):
// ask a live supplier for FileResponse {size, chunkSize, total, hashes}.
void Downloader::requestMeta(const QString &hash) {
    auto ctx = m_ctx.value(hash);
    if (!ctx)
        return;
    QVector<SupplierInfo> live;
    for (const auto &s : ctx->suppliers) {
        if (!ctx->deadSuppliers.contains(s.fp))
            live.append(s);
    }
    PeerMaterial mat{m_acct->profile().tlsCertPem, m_acct->tlsKeyPem(), m_acct->profile().fingerprint,
                     m_acct->profile().nickname, m_acct->profile().avatarHash, m_acct->publicKey(),
                     [this](const QByteArray &in, QByteArray *out) { return m_acct->sign(in, out); }};
    const QString localCertFp = certFpHex(QSslCertificate(m_acct->profile().tlsCertPem.toUtf8(), QSsl::Pem));
    const quint64 wantChunk = ctx->chunkSize;
    QPointer<Downloader> guard(this);
    (void)QtConcurrent::run([guard, mat, hash, wantChunk, localCertFp, live]() {
        if (!guard)
            return;
        // meta can come from any supplier; prefer non-dead ones in order
        for (const auto &s : live) {
            BlockingPeer peer;
            QString err;
            if (!peer.connectTo(mat, s.ip, s.port, s.fp, localCertFp, &err))
                continue;
            FileRequestMsg req;
            req.fileHash = hash;
            req.chunkSize = wantChunk;
            QByteArray resp;
            if (!peer.roundtrip(MsgType::FileRequest, req.toPayload(), MsgType::FileResponse, &resp, &err))
                continue;
            FileResponseMsg fr;
            if (!FileResponseMsg::fromPayload(resp, fr) || fr.err != static_cast<quint8>(FileErr::Ok))
                continue;
            // marshal the result back to the owning thread
            QJsonObject out;
            out["hash"] = hash;
            out["size"] = fr.fileSize;
            out["chunkSize"] = double(fr.chunkSize);
            out["total"] = double(fr.totalChunks);
            out["supportsChunkHash"] = fr.supportsChunkHash;
            QJsonArray ha;
            for (const auto &h : fr.chunkHashes)
                ha.append(h);
            out["chunkHashes"] = ha;
            out["supplier"] = s.fp;
            QMetaObject::invokeMethod(guard.data(), [guard, hash, out]() {
                if (guard)
                    guard->applyMeta(hash, out);
            }, Qt::QueuedConnection);
            return;
        }
        QMetaObject::invokeMethod(guard.data(), [guard, hash]() {
            if (guard)
                guard->onMetaFailure(hash);
        }, Qt::QueuedConnection);
    });
}

void Downloader::onMetaFailure(const QString &hash) {
    // UC-10 E1: nobody could serve it right now -> keep progress, wait for supply.
    // Mark every known supplier dead so that supplierSeen() revives the task as
    // soon as one of them announces itself again (UC-11 E1/E3).
    if (auto ctx = m_ctx.value(hash)) {
        for (const auto &s : ctx->suppliers)
            ctx->deadSuppliers.insert(s.fp);
    }
    setStatusDb(hash, static_cast<quint8>(TaskStatus::WaitingSupply),
                QStringLiteral("供给节点暂时不可用，进度已保留"));
}

void Downloader::applyMeta(const QString &hash, const QJsonObject &meta) {
    auto ctx = m_ctx.value(hash);
    if (!ctx)
        return;
    const quint64 chunkSize = quint64(meta["chunkSize"].toDouble());
    const quint64 total = quint64(meta["total"].toDouble());
    const qint64 size = qint64(meta["size"].toDouble());

    if (chunkSize != ctx->chunkSize) {
        // layout changed vs persisted progress: chunks are only valid per (hash,chunkSize)
        QSqlQuery del = m_storage->query("DELETE FROM chunk_progress WHERE file_hash=?");
        del.bindValue(0, hash);
        del.exec();
        QDir(chunkDir(hash)).removeRecursively();
        QDir().mkpath(chunkDir(hash));
        ctx->chunkSize = chunkSize;
    }
    ctx->fileSize = size;
    ctx->totalChunks = total;
    ctx->metaSupplier = meta["supplier"].toString();
    ctx->haveChunkHashes = meta["supportsChunkHash"].toBool() && !meta["chunkHashes"].toArray().isEmpty();
    ctx->chunkHashes.clear();
    for (const auto &v : meta["chunkHashes"].toArray())
        ctx->chunkHashes.append(v.toString());

    QSqlQuery upd = m_storage->query("UPDATE downloads SET size=?, chunk_size=?, total_chunks=?, status=? WHERE file_hash=?");
    upd.bindValue(0, size);
    upd.bindValue(1, qint64(chunkSize));
    upd.bindValue(2, qlonglong(total));
    upd.bindValue(3, int(TaskStatus::Downloading));
    upd.bindValue(4, hash);
    upd.exec();

    // ensure a progress row per chunk (resume keeps existing Done rows, BR-23)
    for (quint64 i = 0; i < total; ++i) {
        QSqlQuery q = m_storage->query(
            "INSERT OR IGNORE INTO chunk_progress (file_hash,chunk_index,chunk_size,sha256,status) VALUES (?,?,?,'',0)");
        q.bindValue(0, hash);
        q.bindValue(1, qlonglong(i));
        q.bindValue(2, qlonglong(chunkLength(i, size, chunkSize)));
        q.exec();
    }
    emit taskChanged(hash);
    dispatchNext(hash);
}

std::shared_ptr<Downloader::Claim> Downloader::claims(const QString &hash) {
    auto c = m_claims.value(hash);
    if (!c) {
        c = std::make_shared<Claim>();
        m_claims[hash] = c;
    }
    return c;
}

// pick the next live supplier deterministically (internal helper type is private)
// (implemented as a lambda inside dispatchNext)

void Downloader::dispatchNext(const QString &hash) {
    auto ctx = m_ctx.value(hash);
    if (!ctx || ctx->cancelled || ctx->paused)
        return;
    if (ctx->needReMeta) { // UC-12 E2: full re-handshake (prefer other suppliers)
        ctx->needReMeta = false;
        requestMeta(hash);
        return;
    }

    QSet<quint64> done;
    {
        QSqlQuery q = m_storage->query("SELECT chunk_index FROM chunk_progress WHERE file_hash=? AND status=1");
        q.bindValue(0, hash);
        q.exec();
        while (q.next())
            done.insert(q.value(0).toULongLong());
    }
    auto claim = claims(hash);

    // everything already on disk? -> verify (UC-11 step 8)
    {
        QMutexLocker lk(&m_claimMutex);
        if (done.size() >= int(ctx->totalChunks) && ctx->totalChunks > 0 && claim->claimed.isEmpty()) {
            beginVerify(hash);
            return;
        }
    }
    if (ctx->totalChunks == 0) {
        // The layout is only known after the FileResponse handshake. A revived
        // task (supplier came back) must retry the handshake instead of failing
        // with "empty file" (UC-11 E1).
        if (ctx->fileSize == 0) {
            setStatusDb(hash, static_cast<quint8>(TaskStatus::Failed), QStringLiteral("文件为空或不可知"));
            return;
        }
        requestMeta(hash);
        return;
    }

    const int threads = qMax(1, m_settings->downloadThreads());
    m_pool.setMaxThreadCount(threads);
    int launched = 0;
    QVector<quint64> pending;
    {
        QMutexLocker lk(&m_claimMutex);
        for (quint64 i = 0; i < ctx->totalChunks; ++i) {
            if (!done.contains(i) && !claim->claimed.contains(i))
                pending.append(i);
        }
        if (pending.isEmpty())
            return; // workers still running
    }

    auto pickSupplier = [this, &ctx](quint64 w) -> SupplierInfo {
        QVector<SupplierInfo> live;
        for (const auto &s : ctx->suppliers)
            if (!ctx->deadSuppliers.contains(s.fp))
                live.append(s);
        return live[int(w % quint64(live.size()))];
    };

    // UC-11 step 2: N parallel workers pick from the unfinished chunk queue
    for (int w = 0; w < threads && launched < threads && !pending.isEmpty(); ++w) {
        // batches of up to 8 consecutive chunks per worker connection
        QVector<quint64> batch;
        for (int k = 0; k < 8 && !pending.isEmpty(); ++k) {
            const quint64 idx = pending.takeFirst();
            batch.append(idx);
            QMutexLocker lk(&m_claimMutex);
            claim->claimed.insert(idx);
        }
        {
            QVector<SupplierInfo> live;
            for (const auto &s : ctx->suppliers)
                if (!ctx->deadSuppliers.contains(s.fp))
                    live.append(s);
            if (live.isEmpty()) { // every supplier dead -> wait for supply (UC-11 E1)
                QMutexLocker lk(&m_claimMutex);
                for (auto i : batch)
                    claim->claimed.remove(i);
                setStatusDb(hash, static_cast<quint8>(TaskStatus::WaitingSupply),
                            QStringLiteral("所有候选供给节点不可用，已下载分片保留"));
                return;
            }
        }
        const SupplierInfo supplier = pickSupplier(quint64(w));
        ctx->currentSource = supplier.nickname.isEmpty() ? supplier.fp.left(8) : supplier.nickname;
        PeerMaterial mat{m_acct->profile().tlsCertPem, m_acct->tlsKeyPem(), m_acct->profile().fingerprint,
                         m_acct->profile().nickname, m_acct->profile().avatarHash, m_acct->publicKey(),
                         [this](const QByteArray &in, QByteArray *out) { return m_acct->sign(in, out); }};
        const QString localCertFp = certFpHex(QSslCertificate(m_acct->profile().tlsCertPem.toUtf8(), QSsl::Pem));
        auto job = [this, ctx, claim, hash, batch, supplier, mat, localCertFp, w]() {
            // runs in a pool thread
            PeerMaterial lmat = mat;
            BlockingPeer peer;
            QString err;
            QVector<quint64> remain = batch;
            if (!peer.connectTo(lmat, supplier.ip, supplier.port, supplier.fp, localCertFp, &err)) {
                // UC-11 step 5: requeue batch for other workers/threads
                QMutexLocker lk(&m_claimMutex);
                for (auto i : remain)
                    claim->claimed.remove(i);
                lk.unlock();
                QPointer<Downloader> guard(this);
                QMetaObject::invokeMethod(this, [guard, hash, supplierFp = supplier.fp]() {
                    if (!guard)
                        return;
                    auto c = guard->m_ctx.value(hash);
                    if (c) {
                        c->deadSuppliers.insert(supplierFp);
                        guard->dispatchNext(hash);
                    }
                }, Qt::QueuedConnection);
                return;
            }
            for (int pos = 0; pos < remain.size(); ++pos) {
                const quint64 idx = remain[pos];
                if (ctx->cancelled || ctx->paused) {
                    // UC-11 step 6: release the whole batch, not just the current
                    // index, otherwise resume() would never re-claim these chunks.
                    QMutexLocker lk(&m_claimMutex);
                    for (int k = pos; k < remain.size(); ++k)
                        claim->claimed.remove(remain[k]);
                    return; // in-flight chunk discarded, progress rows untouched
                }
                ChunkRequestMsg req;
                req.fileHash = hash;
                req.index = idx;
                req.chunkSize = ctx->chunkSize;
                QByteArray resp;
                if (!peer.roundtrip(MsgType::ChunkRequest, req.toPayload(), MsgType::ChunkData, &resp, &err, 30000)) {
                    // UC-11 step 5 / E3: requeue the rest; connection will be rebuilt
                    QMutexLocker lk(&m_claimMutex);
                    for (int k = pos; k < remain.size(); ++k)
                        claim->claimed.remove(remain[k]);
                    lk.unlock();
                    QPointer<Downloader> guard(this);
                    QMetaObject::invokeMethod(this, [guard, hash, supplierFp = supplier.fp]() {
                        if (!guard)
                            return;
                        if (auto c = guard->m_ctx.value(hash))
                            c->deadSuppliers.insert(supplierFp); // UC-11 E1/E3: try other nodes
                        guard->dispatchNext(hash);
                    }, Qt::QueuedConnection);
                    return;
                }
                ChunkDataMsg cd;
                if (!ChunkDataMsg::fromPayload(resp, cd) || cd.index != idx) {
                    QMutexLocker lk(&m_claimMutex);
                    claim->claimed.remove(idx);
                    lk.unlock();
                    QPointer<Downloader> guard(this);
                    QMetaObject::invokeMethod(this, [guard, hash]() {
                        if (guard)
                            guard->dispatchNext(hash);
                    }, Qt::QueuedConnection);
                    return;
                }
                // content integrity vs what the data claims itself
                const QString actual = QString::fromLatin1(
                    Crypto::sha256Hex(reinterpret_cast<const uint8_t *>(cd.data.constData()), size_t(cd.data.size())).c_str());
                if (actual != cd.sha256) {
                    // transport-level corruption (should be impossible under TLS) -> requeue
                    QMutexLocker lk(&m_claimMutex);
                    claim->claimed.remove(idx);
                    lk.unlock();
                    continue;
                }
                // per-chunk hash from the meta handshake (UC-12 step 1)
                if (ctx->haveChunkHashes && idx < quint64(ctx->chunkHashes.size()) &&
                    !ctx->chunkHashes[quint64(idx)].isEmpty() && ctx->chunkHashes[quint64(idx)] != actual) {
                    // supplier served bytes that do not match its own announced hash
                    // => this node's data source is faulty; re-claim against another supplier
                    {
                        QMutexLocker lk(&m_claimMutex);
                        claim->claimed.remove(idx);
                    }
                    QPointer<Downloader> guard(this);
                    QMetaObject::invokeMethod(this, [guard, hash, supplierFp = supplier.fp, idx]() {
                        if (!guard)
                            return;
                        if (auto c = guard->m_ctx.value(hash)) {
                            c->deadSuppliers.insert(supplierFp); // UC-12: switch nodes for suspect chunks
                        }
                        guard->dispatchNext(hash);
                        (void)idx;
                    }, Qt::QueuedConnection);
                    continue;
                }
                if (ctx->paused || ctx->cancelled) {
                    // UC-11 step 6: a chunk that arrived while pausing is discarded;
                    // its progress row stays pending.
                    QMutexLocker lk(&m_claimMutex);
                    for (int k = pos; k < remain.size(); ++k)
                        claim->claimed.remove(remain[k]);
                    return;
                }
                // write chunk atomically then mark done (UC-11 step 4)
                const QString path = chunkFile(hash, idx);
                const QString tmp = path + ".part";
                {
                    QFile f(tmp);
                    if (!f.open(QIODevice::WriteOnly)) {
                        QMutexLocker lk(&m_claimMutex);
                        claim->claimed.remove(idx);
                        lk.unlock();
                        continue;
                    }
                    f.write(cd.data);
                    f.flush();
                    f.close();
                }
                QFile::remove(path);
                QFile::rename(tmp, path);
                {
                    QSqlDatabase db = workerDb();
                    QSqlQuery q(db);
                    q.prepare("UPDATE chunk_progress SET status=1, sha256=?, source_peer_id=? WHERE file_hash=? AND chunk_index=?");
                    q.bindValue(0, actual);
                    q.bindValue(1, supplier.fp);
                    q.bindValue(2, hash);
                    q.bindValue(3, qlonglong(idx));
                    q.exec();
                }
                {
                    QMutexLocker lk(&m_claimMutex);
                    claim->claimed.remove(idx);
                }
                // Test hook: deterministic pacing so the integration harness can
                // observe mid-transfer state (pause / supplier crash) on loopback.
                // Never set in production.
                if (const QByteArray delayMs = qgetenv("MESHLINK_CHUNK_DELAY_MS"); !delayMs.isEmpty())
                    QThread::msleep(ulong(qMax(0, delayMs.toInt())));
            }
            QPointer<Downloader> guard(this);
            QMetaObject::invokeMethod(this, [guard, hash]() {
                if (guard) {
                    guard->updateProgress(hash);
                    guard->dispatchNext(hash);
                }
            }, Qt::QueuedConnection);
        };
        (void)QtConcurrent::run(&m_pool, job); // honour the configured parallelism (NFR-05)
        ++launched;
    }
}

void Downloader::updateProgress(const QString &hash) { emit taskChanged(hash); }

// ---- UC-12: merge + verify + targeted retransmit ----------------------
void Downloader::beginVerify(const QString &hash) {
    auto ctx = m_ctx.value(hash);
    if (!ctx)
        return;
    setStatusDb(hash, static_cast<quint8>(TaskStatus::Verifying));
    PeerMaterial mat{m_acct->profile().tlsCertPem, m_acct->tlsKeyPem(), m_acct->profile().fingerprint,
                     m_acct->profile().nickname, m_acct->profile().avatarHash, m_acct->publicKey(),
                     [this](const QByteArray &in, QByteArray *out) { return m_acct->sign(in, out); }};
    const QString localCertFp = certFpHex(QSslCertificate(m_acct->profile().tlsCertPem.toUtf8(), QSsl::Pem));
    auto ctxShared = ctx;
    QPointer<Downloader> guard(this);
    (void)QtConcurrent::run([guard, ctxShared, mat, localCertFp, hash]() {
        if (!guard)
            return;
        // merge chunks in index order into the temp file while hashing (streaming, UC-12 step 1)
        const QString tmp = Paths::downloadDir() + "/" + hash + "/merged.tmp";
        QCryptographicHash whole(QCryptographicHash::Sha256);
        {
            QFile out(tmp);
            if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                QMetaObject::invokeMethod(guard.data(), [guard, hash]() {
                    if (guard)
                        guard->setStatusDb(hash, static_cast<quint8>(TaskStatus::Failed), QStringLiteral("无法写入临时文件"));
                }, Qt::QueuedConnection);
                return;
            }
            for (quint64 i = 0; i < ctxShared->totalChunks; ++i) {
                QFile cf(chunkFile(hash, i));
                if (!cf.open(QIODevice::ReadOnly)) {
                    out.close();
                    QFile::remove(tmp);
                    QMetaObject::invokeMethod(guard.data(), [guard, hash]() {
                        if (guard)
                            guard->handleCorrupt(hash); // missing chunk -> treat as corrupt set
                    }, Qt::QueuedConnection);
                    return;
                }
                const qint64 want = qint64(chunkLength(i, ctxShared->fileSize, ctxShared->chunkSize));
                qint64 left = want;
                while (left > 0) {
                    QByteArray b = cf.read(qMin(left, qint64(1 << 20)));
                    if (b.isEmpty())
                        break;
                    out.write(b);
                    whole.addData(b);
                    left -= b.size();
                }
                out.flush();
            }
        }
        const QString got = QString::fromLatin1(whole.result().toHex());
        const bool okHash = (got == hash);

        // On mismatch with per-chunk hashes known: locate corrupt chunks precisely (UC-12 子流程 step 1)
        QJsonArray bad;
        if (!okHash && ctxShared->haveChunkHashes) {
            for (quint64 i = 0; i < ctxShared->totalChunks && i < quint64(ctxShared->chunkHashes.size()); ++i) {
                const QString want = ctxShared->chunkHashes[quint64(i)];
                if (want.isEmpty())
                    continue;
                QFile cf(chunkFile(hash, i));
                if (!cf.open(QIODevice::ReadOnly)) {
                    bad.append(double(i));
                    continue;
                }
                QString hex;
                if (!fileRangeSha(cf, 0, cf.size(), &hex) || hex != want)
                    bad.append(double(i));
            }
        }
        QMetaObject::invokeMethod(guard.data(), [guard, hash, okHash, bad, tmp]() {
            if (!guard)
                return;
            if (okHash) {
                guard->finalizeOk(hash, tmp); // verified temp is renamed, never merged twice
            } else {
                QFile::remove(tmp); // unverified temp is never delivered (BR-24)
                guard->handleCorruptWithBadList(hash, bad);
            }
        }, Qt::QueuedConnection);
    });
}

void Downloader::handleCorrupt(const QString &hash) {
    handleCorruptWithBadList(hash, QJsonArray());
}

void Downloader::handleCorruptWithBadList(const QString &hash, const QJsonArray &badList) {
    auto ctx = m_ctx.value(hash);
    if (!ctx)
        return;
    ctx->round++;
    if (ctx->round > 5) { // UC-12 E1
        setStatusDb(hash, static_cast<quint8>(TaskStatus::Failed),
                    QStringLiteral("文件校验持续失败，建议更换供给节点或放弃任务"));
        emit taskFailed(hash, QStringLiteral("校验持续失败"));
        return;
    }
    QVector<quint64> redo;
    if (!badList.isEmpty()) {
        for (const auto &v : badList)
            redo.append(quint64(v.toDouble()));
    } else {
        // E2 degradation: no trustworthy per-chunk basis -> full re-send, earliest
        // chunks first, re-handshaking metadata with (preferably) a different node.
        redo.clear();
        for (quint64 i = 0; i < ctx->totalChunks; ++i)
            redo.append(i);
        if (!ctx->metaSupplier.isEmpty())
            ctx->deadSuppliers.insert(ctx->metaSupplier); // rule out single-node source (UC-12 step 3)
        ctx->haveChunkHashes = false;
        ctx->chunkHashes.clear();
        ctx->needReMeta = true;
    }
    for (auto idx : redo) {
        QFile::remove(chunkFile(hash, idx));
        QSqlQuery q = m_storage->query("UPDATE chunk_progress SET status=0, sha256='', source_peer_id=NULL WHERE file_hash=? AND chunk_index=?");
        q.bindValue(0, hash);
        q.bindValue(1, qlonglong(idx));
        q.exec();
    }
    QSqlQuery up = m_storage->query("UPDATE downloads SET status=?, retry_count=retry_count+1 WHERE file_hash=?");
    up.bindValue(0, int(TaskStatus::Downloading));
    up.bindValue(1, hash);
    up.exec();
    dispatchNext(hash);
}

void Downloader::finalizeOk(const QString &hash, const QString &mergedTmp) {
    auto ctx = m_ctx.value(hash);
    if (!ctx)
        return;
    QSqlQuery q = m_storage->query("SELECT room_id,name,size FROM downloads WHERE file_hash=?");
    q.bindValue(0, hash);
    q.exec();
    if (!q.next())
        return;
    const QString roomId = q.value(0).toString();
    const QString name = q.value(1).toString();

    // BR-24: only a verified file is promoted to its real name.
    QDir().mkpath(Paths::fileDir());
    QString dest = Paths::fileDir() + "/" + name;
    if (QFile::exists(dest))
        dest = Paths::fileDir() + "/" + hash.left(12) + "-" + name;

    bool placed = false;
    if (!mergedTmp.isEmpty() && QFile::exists(mergedTmp)) {
        // final safety re-hash of the verified merge (streaming), then rename
        QString hex;
        if (hashFile(mergedTmp, &hex) && hex == hash) {
            QFile::remove(dest);
            placed = QFile::rename(mergedTmp, dest);
        } else {
            QFile::remove(mergedTmp);
        }
    }
    if (!placed) {
        // fallback: re-merge chunk files (still durable until verification passes)
        QFile out(dest);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            setStatusDb(hash, static_cast<quint8>(TaskStatus::Failed), QStringLiteral("无法写入目标文件"));
            return;
        }
        for (quint64 i = 0; i < ctx->totalChunks; ++i) {
            QFile cf(chunkFile(hash, i));
            if (!cf.open(QIODevice::ReadOnly)) {
                out.close();
                QFile::remove(dest);
                setStatusDb(hash, static_cast<quint8>(TaskStatus::Failed), QStringLiteral("分片丢失"));
                return;
            }
            out.write(cf.readAll());
        }
        out.close();
        QString hex;
        if (!hashFile(dest, &hex) || hex != hash) {
            QFile::remove(dest);
            handleCorrupt(hash);
            return;
        }
    }
    QSqlQuery ins = m_storage->query("INSERT OR REPLACE INTO local_files (file_hash,dest_path,name,size,mime,added_ts) VALUES (?,?,?,?,?,?)");
    ins.bindValue(0, hash);
    ins.bindValue(1, dest);
    ins.bindValue(2, name);
    ins.bindValue(3, ctx->fileSize);
    ins.bindValue(4, QString());
    ins.bindValue(5, nowMs());
    ins.exec();

    QDir(chunkDir(hash)).removeRecursively(); // verified: chunk cache no longer needed
    setStatusDb(hash, static_cast<quint8>(TaskStatus::Done));
    m_ctx.remove(hash);
    m_claims.remove(hash);
    emit taskCompleted(hash, dest);
    emit becameSupplier(roomId, hash); // BR-18
}

// ---- pause / resume / cancel (UC-11 steps 6-7) ------------------------
void Downloader::pause(const QString &hash) {
    if (auto ctx = m_ctx.value(hash))
        ctx->paused = 1;
    setStatusDb(hash, static_cast<quint8>(TaskStatus::Paused));
}

void Downloader::resume(const QString &hash, const QVector<SupplierInfo> &suppliers) {
    auto ctx = m_ctx.value(hash);
    if (!ctx) {
        QVector<SupplierInfo> sup = suppliers;
        if (sup.isEmpty())
            loadSuppliersJson(hash, sup);
        QSqlQuery q = m_storage->query("SELECT room_id,name,mime FROM downloads WHERE file_hash=?");
        q.bindValue(0, hash);
        q.exec();
        if (!q.next())
            return;
        startTask(q.value(0).toString(), hash, q.value(1).toString(), -1, q.value(2).toString(), sup);
        return;
    }
    ctx->paused = 0;
    if (!suppliers.isEmpty())
        ctx->suppliers = suppliers;
    setStatusDb(hash, static_cast<quint8>(TaskStatus::Queued));
    dispatchNext(hash);
}

void Downloader::cancel(const QString &hash, bool deletePartialChunks) {
    if (auto ctx = m_ctx.value(hash)) {
        ctx->cancelled = 1;
        ctx->paused = 1;
    }
    m_ctx.remove(hash);
    m_claims.remove(hash);
    if (deletePartialChunks) {
        QDir(chunkDir(hash)).removeRecursively();
        QSqlQuery q = m_storage->query("DELETE FROM chunk_progress WHERE file_hash=?");
        q.bindValue(0, hash);
        q.exec();
        QSqlQuery dq = m_storage->query("DELETE FROM downloads WHERE file_hash=?");
        dq.bindValue(0, hash);
        dq.exec();
    } else {
        setStatusDb(hash, static_cast<quint8>(TaskStatus::Paused), QStringLiteral("已暂停"));
    }
    emit taskChanged(hash);
}

// ---- startup recovery (UC-11 E2 / NFR-07 / BR-20) ---------------------
void Downloader::recoverTasks() {
    QSqlQuery q = m_storage->query("SELECT file_hash,status FROM downloads");
    q.exec();
    while (q.next()) {
        const QString hash = q.value(0).toString();
        const int st = q.value(1).toInt();
        // clean half-written temp artifacts; completed chunks stay (atomic rename)
        QDir d(chunkDir(hash));
        for (const auto &f : d.entryList({"*.part", "merged.tmp"}, QDir::Files))
            d.remove(f);
        if (st == int(TaskStatus::Queued) || st == int(TaskStatus::Downloading) ||
            st == int(TaskStatus::Verifying) || st == int(TaskStatus::WaitingSupply)) {
            QVector<SupplierInfo> sup;
            loadSuppliersJson(hash, sup);
            QSqlQuery info = m_storage->query("SELECT room_id,name,size,mime,chunk_size FROM downloads WHERE file_hash=?");
            info.bindValue(0, hash);
            info.exec();
            if (info.next()) {
                auto ctx = std::make_shared<JobCtx>();
                ctx->hash = hash;
                ctx->chunkSize = quint64(info.value(4).toLongLong());
                ctx->fileSize = info.value(2).toLongLong();
                m_ctx[hash] = ctx;
                m_claims[hash] = std::make_shared<Claim>();
                if (sup.isEmpty()) {
                    setStatusDb(hash, static_cast<quint8>(TaskStatus::WaitingSupply), QStringLiteral("等待供给节点"));
                } else if (st == int(TaskStatus::WaitingSupply)) {
                    // Waiting for an offline supplier: keep the task and mark every
                    // candidate dead so the next heartbeat revives it (UC-11 E1).
                    for (const auto &s : sup)
                        ctx->deadSuppliers.insert(s.fp);
                    setStatusDb(hash, static_cast<quint8>(TaskStatus::WaitingSupply), QStringLiteral("等待供给节点"));
                } else {
                    startTask(info.value(0).toString(), hash, info.value(1).toString(),
                              info.value(2).toLongLong(), info.value(3).toString(), sup);
                }
            }
        }
    }
}

void Downloader::supplierSeen(const QString &fp) {
    for (auto it = m_ctx.begin(); it != m_ctx.end(); ++it) {
        const auto &ctx = it.value();
        bool contains = false;
        for (const auto &s : ctx->suppliers)
            if (s.fp == fp)
                contains = true;
        if (!contains || !ctx->deadSuppliers.contains(fp))
            continue;
        ctx->deadSuppliers.remove(fp);
        if (ctx->cancelled || ctx->paused)
            continue;
        dispatchNext(it.key()); // resume progress from persisted chunks (BR-20)
    }
}

QList<DownloadTaskView> Downloader::tasks() const {
    QList<DownloadTaskView> out;
    QSqlQuery q = m_storage->query("SELECT d.file_hash,d.name,d.size,d.mime,d.chunk_size,d.total_chunks,d.status,d.dest_path,d.error,d.room_id,"
                                   "(SELECT COUNT(*) FROM chunk_progress c WHERE c.file_hash=d.file_hash AND c.status=1) FROM downloads d");
    q.exec();
    while (q.next()) {
        DownloadTaskView t;
        t.hash = q.value(0).toString();
        t.name = q.value(1).toString();
        t.size = q.value(2).toLongLong();
        t.mime = q.value(3).toString();
        t.chunkSize = q.value(4).toULongLong();
        t.totalChunks = q.value(5).toULongLong();
        t.status = static_cast<TaskStatus>(q.value(6).toInt());
        t.destPath = q.value(7).toString();
        t.error = q.value(8).toString();
        t.roomId = q.value(9).toString();
        t.doneChunks = q.value(10).toULongLong();
        if (auto c = m_ctx.value(t.hash))
            t.currentSource = c->currentSource;
        if (t.status == TaskStatus::Done)
            t.doneChunks = t.totalChunks;
        out.append(t);
    }
    return out;
}

DownloadTaskView Downloader::task(const QString &hash) const {
    for (const auto &t : tasks())
        if (t.hash == hash)
            return t;
    return {};
}

// ===================================================================
// provider side (UC-09 BR-18: announcing makes you a supplier; serving is
// strictly request-driven, BR-19)
// ===================================================================
void Downloader::serveFileRequest(SecureChannel *ch, const QByteArray &payload) {
    FileRequestMsg req;
    if (!FileRequestMsg::fromPayload(payload, req) || !ch)
        return;
    QSqlQuery q = m_storage->query("SELECT dest_path,size FROM local_files WHERE file_hash=?");
    q.bindValue(0, req.fileHash);
    q.exec();
    if (!q.next()) { // UC-10 E1: file no longer here
        FileResponseMsg r;
        r.fileHash = req.fileHash;
        r.err = static_cast<quint8>(FileErr::NotFound);
        ch->send(MsgType::FileResponse, r.toPayload());
        return;
    }
    const QString path = q.value(0).toString();
    const qint64 declared = q.value(1).toLongLong();
    QFileInfo fi(path);
    if (!fi.exists() || (declared > 0 && fi.size() != declared)) { // file changed/disappeared
        FileResponseMsg r;
        r.fileHash = req.fileHash;
        r.err = static_cast<quint8>(FileErr::NotFound);
        ch->send(MsgType::FileResponse, r.toPayload());
        return;
    }
    const quint64 chunkSize = clampChunkSize(req.chunkSize ? req.chunkSize : m_settings->chunkSize());
    const qint64 fileSize = fi.size();
    const quint64 total = totalChunks(fileSize, chunkSize);
    const qint64 mtime = fi.lastModified().toMSecsSinceEpoch();

    // cached per-chunk hashes? (file_index keyed by hash+layout+mtime)
    QSqlQuery iq = m_storage->query("SELECT chunk_hashes,mtime FROM file_index WHERE file_hash=? AND chunk_size=?");
    iq.bindValue(0, req.fileHash);
    iq.bindValue(1, qint64(chunkSize));
    iq.exec();
    if (iq.next() && iq.value(1).toLongLong() == mtime) {
        FileResponseMsg r;
        r.fileHash = req.fileHash;
        r.fileSize = fileSize;
        r.chunkSize = chunkSize;
        r.totalChunks = total;
        r.supportsChunkHash = true;
        const QJsonArray arr = QJsonDocument::fromJson(iq.value(0).toByteArray()).array();
        for (const auto &v : arr)
            r.chunkHashes.append(v.toString());
        ch->send(MsgType::FileResponse, r.toPayload());
        return;
    }

    // compute the chunk hash table in the pool, answer asynchronously
    const QString hash = req.fileHash;
    QPointer<Downloader> guard(this);
    (void)QtConcurrent::run([guard, ch, hash, path, fileSize, chunkSize, total, mtime]() {
        if (!guard)
            return;
        QJsonArray arr;
        bool ok = true;
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            for (quint64 i = 0; i < total; ++i) {
                QString hex;
                if (!fileRangeSha(f, chunkOffset(i, chunkSize), qint64(chunkLength(i, fileSize, chunkSize)), &hex)) {
                    ok = false;
                    break;
                }
                arr.append(hex);
            }
        } else {
            ok = false;
        }
        QMetaObject::invokeMethod(guard.data(), [guard, ch, hash, fileSize, chunkSize, total, mtime, path, arr, ok]() {
            if (!guard)
                return;
            FileResponseMsg r;
            r.fileHash = hash;
            if (!ok) {
                r.err = static_cast<quint8>(FileErr::NotFound);
            } else {
                r.fileSize = fileSize;
                r.chunkSize = chunkSize;
                r.totalChunks = total;
                r.supportsChunkHash = true;
                r.chunkHashes = [&] {
                    QVector<QString> v;
                    for (const auto &x : arr)
                        v.append(x.toString());
                    return v;
                }();
                QSqlQuery ins = guard->m_storage->query(
                    "INSERT OR REPLACE INTO file_index (file_hash,chunk_size,path,mtime,chunk_hashes) VALUES (?,?,?,?,?)");
                ins.bindValue(0, hash);
                ins.bindValue(1, qint64(chunkSize));
                ins.bindValue(2, path);
                ins.bindValue(3, mtime);
                ins.bindValue(4, QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
                ins.exec();
            }
            if (ch)
                ch->send(MsgType::FileResponse, r.toPayload());
        }, Qt::QueuedConnection);
    });
}

void Downloader::serveChunkRequest(SecureChannel *ch, const QByteArray &payload) {
    ChunkRequestMsg req;
    if (!ChunkRequestMsg::fromPayload(payload, req) || !ch)
        return;
    QSqlQuery q = m_storage->query("SELECT dest_path,size FROM local_files WHERE file_hash=?");
    q.bindValue(0, req.fileHash);
    q.exec();
    if (!q.next()) {
        ErrorMessageMsg e;
        e.reason = QStringLiteral("FILE_MISSING");
        ch->send(MsgType::Error, e.toPayload());
        return;
    }
    const QString path = q.value(0).toString();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        ErrorMessageMsg e;
        e.reason = QStringLiteral("FILE_MISSING");
        ch->send(MsgType::Error, e.toPayload());
        return;
    }
    const qint64 fileSize = f.size();
    const quint64 chunkSize = clampChunkSize(req.chunkSize);
    if (req.index >= totalChunks(fileSize, chunkSize)) {
        ErrorMessageMsg e;
        e.reason = QStringLiteral("CHUNK_OUT_OF_RANGE");
        ch->send(MsgType::Error, e.toPayload());
        return;
    }
    if (!f.seek(chunkOffset(req.index, chunkSize))) {
        ErrorMessageMsg e;
        e.reason = QStringLiteral("SEEK_FAIL");
        ch->send(MsgType::Error, e.toPayload());
        return;
    }
    const QByteArray data = f.read(qint64(chunkLength(req.index, fileSize, chunkSize)));
    const QString hex = QString::fromLatin1(
        Crypto::sha256Hex(reinterpret_cast<const uint8_t *>(data.constData()), size_t(data.size())).c_str());
    // test hook: deliberately damaged supplier for the self-test harness
    if (qEnvironmentVariableIsSet("MESHLINK_EVIL_CHUNKS")) {
        QByteArray evil = data;
        if (!evil.isEmpty())
            evil[0] = quint8(evil[0] ^ 0x5A);
        // advertise the *real* hash of the corrupted bytes: the receiver's
        // transport check passes, and the mismatch is only detectable against
        // the per-chunk hashes from the meta handshake (UC-12).
        const QString evilHex = QString::fromLatin1(
            Crypto::sha256Hex(reinterpret_cast<const uint8_t *>(evil.constData()), size_t(evil.size())).c_str());
        ch->send(MsgType::ChunkData, [&] {
            ChunkDataMsg d;
            d.fileHash = req.fileHash;
            d.index = req.index;
            d.size = quint64(evil.size());
            d.sha256 = evilHex; // self-consistent but wrong vs meta
            d.data = evil;
            return d.toPayload();
        }());
        return;
    }
    ChunkDataMsg d;
    d.fileHash = req.fileHash;
    d.index = req.index;
    d.size = quint64(data.size());
    d.sha256 = hex;
    d.data = data;
    ch->send(MsgType::ChunkData, d.toPayload());
}

} // namespace meshlink
