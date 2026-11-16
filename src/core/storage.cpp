// deepin-MeshLink — SQLite persistence implementation.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "storage.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace meshlink {

bool Storage::open(const QString &filePath, QString *error) {
    QDir().mkpath(QFileInfo(filePath).absolutePath());
    QSqlDatabase db = QSqlDatabase::contains(kConnectionName)
                          ? QSqlDatabase::database(kConnectionName)
                          : QSqlDatabase::addDatabase("QSQLITE", kConnectionName);
    db.setDatabaseName(filePath);
    if (!db.open()) {
        if (error)
            *error = db.lastError().text();
        return false;
    }
    // Chunk worker threads write chunk_progress concurrently with GUI-side
    // status updates; wait instead of failing on a transient lock (UC-11).
    db.exec("PRAGMA busy_timeout=8000");
    if (!createSchema(error))
        return false;
    return true;
}

void Storage::close() {
    if (!QSqlDatabase::contains(kConnectionName))
        return; // already closed (e.g. destroyAccount() + ~Engine())
    {
        QSqlDatabase db = QSqlDatabase::database(kConnectionName, /*open=*/false);
        if (db.isOpen())
            db.close();
    }
    QSqlDatabase::removeDatabase(kConnectionName);
}

bool Storage::isOpen() const {
    if (!QSqlDatabase::contains(kConnectionName))
        return false;
    return QSqlDatabase::database(kConnectionName, /*open=*/false).isOpen();
}

QSqlDatabase Storage::db() const { return QSqlDatabase::database(kConnectionName); }

QSqlQuery Storage::query(const QString &sql) {
    QSqlQuery q(db());
    q.prepare(sql);
    return q;
}

bool Storage::exec(const QString &sql, QString *error) {
    QSqlQuery q = query(sql);
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    return true;
}

bool Storage::createSchema(QString *error) {
    db().exec("PRAGMA journal_mode=WAL");
    db().exec("PRAGMA foreign_keys=ON");
    const char *ddl[] = {
        // NFR-07: account identity, public material, encrypted private keystore blob.
        "CREATE TABLE IF NOT EXISTS account ("
        " id INTEGER PRIMARY KEY CHECK(id=1),"
        " fingerprint TEXT NOT NULL,"
        " nickname TEXT NOT NULL,"
        " ed25519_pub BLOB NOT NULL,"
        " x25519_pub  BLOB NOT NULL,"
        " keystore_blob BLOB NOT NULL,"   // AES-GCM(salt||iv||ct||tag) of private key material (BR-02a)"
        " cert_pem TEXT NOT NULL,"
        " created_ts INTEGER NOT NULL)",

        // UC-03/UC-01: profile + LAN ip snapshot.
        "CREATE TABLE IF NOT EXISTS profile ("
        " id INTEGER PRIMARY KEY CHECK(id=1),"
        " avatar_path TEXT,"
        " avatar_hash TEXT,"
        " lan_ip TEXT)",

        // UC-04: dynamic online-node list (persisted for convenience, refreshed by heartbeats).
        "CREATE TABLE IF NOT EXISTS peers ("
        " fingerprint TEXT PRIMARY KEY,"
        " nickname TEXT, avatar_hash TEXT, ip TEXT, port INTEGER,"
        " ed25519_pub BLOB, last_seen INTEGER, hosted_rooms TEXT)",

        // NFR-13: TOFU pinned certificate fingerprints per peer.
        "CREATE TABLE IF NOT EXISTS tofu ("
        " fingerprint TEXT PRIMARY KEY,"
        " cert_fp TEXT NOT NULL, first_seen INTEGER NOT NULL)",

        // UC-05/06/07: rooms + membership.
        "CREATE TABLE IF NOT EXISTS rooms ("
        " room_id TEXT PRIMARY KEY, name TEXT, intro TEXT,"
        " policy INTEGER, global_mute INTEGER DEFAULT 0,"
        " capacity INTEGER DEFAULT 0, host_fp TEXT, hosted INTEGER DEFAULT 0,"
        " joined INTEGER DEFAULT 0, created_ts INTEGER)",

        "CREATE TABLE IF NOT EXISTS members ("
        " room_id TEXT, fingerprint TEXT, nickname TEXT, role INTEGER,"
        " muted_until INTEGER DEFAULT 0, joined_ts INTEGER,"
        " PRIMARY KEY(room_id, fingerprint))",

        // BR-14: blacklist keyed by fingerprint.
        "CREATE TABLE IF NOT EXISTS blacklist ("
        " room_id TEXT, fingerprint TEXT, PRIMARY KEY(room_id, fingerprint))",

        // UC-06 A1: join requests awaiting host approval.
        "CREATE TABLE IF NOT EXISTS approvals ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " room_id TEXT, fingerprint TEXT, nickname TEXT,"
        " ip TEXT, port INTEGER, req_ts INTEGER)",

        // UC-08: authoritative-ish message log (seq assigned by host, BR-16).
        "CREATE TABLE IF NOT EXISTS messages ("
        " msg_id TEXT, room_id TEXT, seq INTEGER, sender_fp TEXT, sender_nick TEXT,"
        " avatar_hash TEXT, kind INTEGER, ts INTEGER, status INTEGER,"
        " text TEXT, content_hash TEXT, file_name TEXT, mime TEXT, file_size INTEGER,"
        " img_w INTEGER, img_h INTEGER,"
        " PRIMARY KEY(room_id, msg_id))",

        // UC-09/10: file announcements + supplier registry.
        "CREATE TABLE IF NOT EXISTS file_announce ("
        " file_hash TEXT, room_id TEXT, name TEXT, size INTEGER, mime TEXT,"
        " announcer_fp TEXT, ts INTEGER, PRIMARY KEY(file_hash, room_id))",

        "CREATE TABLE IF NOT EXISTS suppliers ("
        " file_hash TEXT, fingerprint TEXT, nickname TEXT, ip TEXT, port INTEGER,"
        " PRIMARY KEY(file_hash, fingerprint))",

        // UC-11: download tasks + per-chunk progress (BR-23).
        "CREATE TABLE IF NOT EXISTS downloads ("
        " file_hash TEXT PRIMARY KEY, room_id TEXT, name TEXT, size INTEGER, mime TEXT,"
        " chunk_size INTEGER, total_chunks INTEGER, status INTEGER,"
        " dest_path TEXT, created_ts INTEGER, updated_ts INTEGER, retry_count INTEGER DEFAULT 0,"
        " suppliers_json TEXT, error TEXT)",

        "CREATE TABLE IF NOT EXISTS chunk_progress ("
        " file_hash TEXT, chunk_index INTEGER, chunk_size INTEGER,"
        " sha256 TEXT, status INTEGER, source_peer_id TEXT,"
        " PRIMARY KEY(file_hash, chunk_index))",

        // index of verified finished files (BR-17 reuse).
        "CREATE TABLE IF NOT EXISTS local_files ("
        " file_hash TEXT PRIMARY KEY, dest_path TEXT, name TEXT, size INTEGER,"
        " mime TEXT, added_ts INTEGER)",

        // provider-side per-chunk-hash cache (UC-12 step 1 分片级哈希通告).
        // Keyed by (hash, chunk_size) because chunk size is user-configurable.
        "CREATE TABLE IF NOT EXISTS file_index ("
        " file_hash TEXT, chunk_size INTEGER, path TEXT, mtime INTEGER,"
        " chunk_hashes TEXT,"
        " PRIMARY KEY(file_hash, chunk_size))",
    };
    for (const char *sql : ddl) {
        if (!exec(sql, error))
            return false;
    }
    return true;
}

bool Storage::wipeAccount(const QString &fp, bool alsoChatHistoryAndFiles, QString *error) {
    if (!exec("BEGIN TRANSACTION", error))
        return false;
    // 1) optional: full local session data (UC-02 step 6, user's choice).
    //    Done FIRST so subqueries on membership rows still see them.
    if (alsoChatHistoryAndFiles) {
        for (const char *t : {"messages", "rooms", "members", "blacklist", "file_announce",
                              "suppliers", "chunk_progress", "downloads", "local_files"}) {
            if (!exec(QString("DELETE FROM %1").arg(t), error)) {
                exec("ROLLBACK");
                return false;
            }
        }
    } else {
        // keep history/room structure, but drop *this identity's* membership rows
        QSqlQuery q = query("DELETE FROM members WHERE fingerprint=?");
        q.bindValue(0, fp);
        if (!q.exec()) {
            if (error)
                *error = q.lastError().text();
            exec("ROLLBACK");
            return false;
        }
    }
    // 2) identity material (UC-02 step 5/6).
    QSqlQuery q = query("DELETE FROM account WHERE fingerprint=?");
    q.bindValue(0, fp);
    bool ok = q.exec();
    if (!ok && error)
        *error = q.lastError().text();
    ok = ok && exec("DELETE FROM profile", error);
    ok = ok && exec("DELETE FROM peers", error);      // dynamic list, rebuilds itself
    ok = ok && exec("DELETE FROM tofu", error);       // old identity's pins are useless now
    ok = ok && exec("COMMIT", error);
    if (!ok)
        exec("ROLLBACK");
    return ok;
}

} // namespace meshlink
