// deepin-MeshLink — SQLite persistence implementation.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "storage.h"

#include "common.h"
#include "crypto.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QPair>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <QVector>

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
    if (!migrate(error))
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
        // NFR-07: account identity, public material, encrypted private keystore
        // blob. Multi-account: rows are keyed by an autoincrement id; the
        // active account is chosen through Settings.
        "CREATE TABLE IF NOT EXISTS account ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " fingerprint TEXT NOT NULL UNIQUE,"
        " nickname TEXT NOT NULL,"
        " ed25519_pub BLOB NOT NULL,"
        " x25519_pub  BLOB NOT NULL,"
        " keystore_blob BLOB NOT NULL,"   // AES-GCM(salt||iv||ct||tag) of private key material (BR-02a)"
        " cert_pem TEXT NOT NULL,"
        " created_ts INTEGER NOT NULL)",

        // UC-03/UC-01: profile + LAN ip snapshot, one row per account.
        "CREATE TABLE IF NOT EXISTS profile ("
        " account_id INTEGER PRIMARY KEY,"
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
        " invite_code TEXT,"
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
        " img_w INTEGER, img_h INTEGER, reply_to TEXT, recalled INTEGER DEFAULT 0,"
        " recalled_by TEXT, recalled_ts INTEGER DEFAULT 0, recv_ts INTEGER DEFAULT 0,"
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

        // Per-room local preferences (do-not-disturb). Local-only state that
        // never rides the wire protocol.
        "CREATE TABLE IF NOT EXISTS room_prefs ("
        " room_id TEXT PRIMARY KEY,"
        " dnd INTEGER DEFAULT 0)",
    };
    for (const char *sql : ddl) {
        if (!exec(sql, error))
            return false;
    }
    return true;
}

namespace {

bool tableHasColumn(const QSqlDatabase &database, const QString &table, const QString &column) {
    QSqlQuery info(database);
    if (!info.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table)))
        return false;
    while (info.next())
        if (info.value(1).toString() == column)
            return true;
    return false;
}

QString freshInviteCode(const QSqlDatabase &database) {
    for (int attempt = 0; attempt < 128; ++attempt) {
        const Bytes bytes = Crypto::randomBytes(4);
        const quint32 value = (quint32(bytes[0]) << 24) | (quint32(bytes[1]) << 16) | (quint32(bytes[2]) << 8) |
                              quint32(bytes[3]);
        const QString code =
            QStringLiteral("%1").arg(value % 1000000u, kInviteCodeDigits, 10, QChar('0'));
        QSqlQuery exists(database);
        exists.prepare("SELECT 1 FROM rooms WHERE invite_code=?");
        exists.addBindValue(code);
        if (exists.exec() && !exists.next())
            return code;
    }
    return {};
}

} // namespace

bool Storage::migrate(QString *error) {
    QSqlDatabase database = db();
    // v2 schema additions on databases created by the 1.0.0 alpha build.
    if (!tableHasColumn(database, "rooms", "invite_code")) {
        QSqlQuery alter(database);
        if (!alter.exec("ALTER TABLE rooms ADD COLUMN invite_code TEXT")) {
            if (error)
                *error = alter.lastError().text();
            return false;
        }
    }

    // v3 chat schema: quoted-message reference, recall tombstone and the
    // host-authoritative receive timestamp used by the 2-minute window.
    struct AddedColumn {
        const char *name;
        const char *definition;
    };
    for (const AddedColumn &column : {AddedColumn{"reply_to", "TEXT"},
                                      AddedColumn{"recalled", "INTEGER DEFAULT 0"},
                                      AddedColumn{"recalled_by", "TEXT"},
                                      AddedColumn{"recalled_ts", "INTEGER DEFAULT 0"},
                                      AddedColumn{"recv_ts", "INTEGER DEFAULT 0"}}) {
        if (tableHasColumn(database, "messages", QString::fromLatin1(column.name)))
            continue;
        QSqlQuery alter(database);
        if (!alter.exec(QStringLiteral("ALTER TABLE messages ADD COLUMN %1 %2")
                            .arg(QString::fromLatin1(column.name),
                                 QString::fromLatin1(column.definition)))) {
            if (error)
                *error = alter.lastError().text();
            return false;
        }
    }

    // Multi-account schema: the 1.0.0 build pinned account/profile to id=1 and
    // had no notion of which local identity owns a room. Rebuild both tables
    // (idempotent) and tag rooms with their owning identity.
    bool legacyAccount = false;
    {
        QSqlQuery ddl(database);
        // Whitespace-normalised match so minor DDL formatting variants of the
        // historical CHECK(id=1) tables are still recognised.
        const QString accountSql = [&] {
            QSqlQuery q(database);
            QString sql;
            if (q.exec("SELECT sql FROM sqlite_master WHERE type='table' AND name='account'") &&
                q.next())
                sql = q.value(0).toString();
            sql.remove(QChar(' '));
            return sql;
        }();
        legacyAccount = accountSql.contains(QLatin1String("CHECK(id=1)"));
        if (legacyAccount) {
            if (!database.transaction()) {
                if (error)
                    *error = database.lastError().text();
                return false;
            }
            auto rebuildFail = [&](const QString &why) {
                database.rollback();
                if (error)
                    *error = why;
                return false;
            };
            QSqlQuery build(database);
            if (!build.exec("CREATE TABLE account_new ("
                            " id INTEGER PRIMARY KEY AUTOINCREMENT,"
                            " fingerprint TEXT NOT NULL UNIQUE,"
                            " nickname TEXT NOT NULL,"
                            " ed25519_pub BLOB NOT NULL,"
                            " x25519_pub BLOB NOT NULL,"
                            " keystore_blob BLOB NOT NULL,"
                            " cert_pem TEXT NOT NULL,"
                            " created_ts INTEGER NOT NULL)"))
                return rebuildFail(build.lastError().text());
            if (!build.exec("INSERT INTO account_new (id,fingerprint,nickname,ed25519_pub,"
                            "x25519_pub,keystore_blob,cert_pem,created_ts)"
                            " SELECT id,fingerprint,nickname,ed25519_pub,x25519_pub,"
                            "keystore_blob,cert_pem,created_ts FROM account"))
                return rebuildFail(build.lastError().text());
            if (!build.exec("DROP TABLE account"))
                return rebuildFail(build.lastError().text());
            if (!build.exec("ALTER TABLE account_new RENAME TO account"))
                return rebuildFail(build.lastError().text());
            if (!build.exec("CREATE TABLE profile_new ("
                            " account_id INTEGER PRIMARY KEY,"
                            " avatar_path TEXT, avatar_hash TEXT, lan_ip TEXT)"))
                return rebuildFail(build.lastError().text());
            if (!build.exec("INSERT INTO profile_new (account_id,avatar_path,avatar_hash,lan_ip)"
                            " SELECT id,avatar_path,avatar_hash,lan_ip FROM profile"))
                return rebuildFail(build.lastError().text());
            if (!build.exec("DROP TABLE profile"))
                return rebuildFail(build.lastError().text());
            if (!build.exec("ALTER TABLE profile_new RENAME TO profile"))
                return rebuildFail(build.lastError().text());
            if (!database.commit())
                return rebuildFail(database.lastError().text());
        }
    }

    // Rooms are per-identity: tag the owning local account so multiple local
    // accounts never see each other's rooms.
    bool columnAdded = false;
    if (!tableHasColumn(database, "rooms", "owner_fp")) {
        QSqlQuery alter(database);
        if (!alter.exec("ALTER TABLE rooms ADD COLUMN owner_fp TEXT")) {
            if (error)
                *error = alter.lastError().text();
            return false;
        }
        columnAdded = true;
    }
    // Pre-multi-account databases held exactly one identity; every legacy room
    // belongs to it. Backfill after the schema conversion and whenever an
    // account-table rebuild ran, so rooms with a NULL/empty owner are claimed.
    if (columnAdded || legacyAccount) {
        QSqlQuery backfill(database);
        if (!backfill.exec("UPDATE rooms SET owner_fp=(SELECT fingerprint FROM account LIMIT 1)"
                           " WHERE owner_fp IS NULL OR owner_fp=''")) {
            if (error)
                *error = backfill.lastError().text();
            return false;
        }
    }

    // Collect legacy rooms first: SQLite does not like updating a table while a
    // cursor over the same table is still open.
    QVector<QPair<QString, QString>> rekeys; // old room id -> new 32-hex id
    {
        QSqlQuery q(database);
        if (!q.exec("SELECT room_id,COALESCE(host_fp,''),COALESCE(created_ts,0) FROM rooms")) {
            if (error)
                *error = q.lastError().text();
            return false;
        }
        while (q.next()) {
            const QString oldId = q.value(0).toString();
            if (oldId.size() == kRoomIdHexLen)
                continue;
            const QString host = q.value(1).toString();
            const qint64 createdTs = q.value(2).toLongLong();
            QString newId;
            for (int salt = 0; salt < 32 && newId.isEmpty(); ++salt) {
                const QByteArray seed = QStringLiteral("legacy-room|%1|%2|%3|%4")
                                            .arg(oldId, host)
                                            .arg(createdTs)
                                            .arg(salt)
                                            .toUtf8();
                const QString candidate = QString::fromLatin1(
                    Crypto::sha256Hex(reinterpret_cast<const uint8_t *>(seed.constData()), size_t(seed.size()))
                        .substr(0, kRoomIdHexLen)
                        .c_str());
                QSqlQuery exists(database);
                exists.prepare("SELECT 1 FROM rooms WHERE room_id=?");
                exists.addBindValue(candidate);
                if (exists.exec() && !exists.next())
                    newId = candidate;
            }
            if (newId.isEmpty()) {
                if (error)
                    *error = QCoreApplication::translate("Storage", "Could not generate a new 32-hex fingerprint id for the old room %1").arg(oldId);
                return false;
            }
            rekeys.append({oldId, newId});
        }
    }

    if (!database.transaction()) {
        if (error)
            *error = database.lastError().text();
        return false;
    }
    auto fail = [&](const QString &why) {
        database.rollback();
        if (error)
            *error = why;
        return false;
    };
    auto update = [&](const QString &sql, const QVariant &a, const QVariant &b) {
        QSqlQuery q(database);
        q.prepare(sql);
        q.addBindValue(a);
        q.addBindValue(b);
        if (!q.exec()) {
            fail(q.lastError().text());
            return false;
        }
        return true;
    };

    for (const auto &pair : rekeys) {
        for (const char *table : {"members", "messages", "file_announce", "blacklist", "approvals", "downloads"}) {
            if (!update(QStringLiteral("UPDATE %1 SET room_id=? WHERE room_id=?").arg(table), pair.second,
                        pair.first))
                return false;
        }
        if (!update(QStringLiteral("UPDATE rooms SET room_id=? WHERE room_id=?"), pair.second, pair.first))
            return false;
    }

    QVector<QString> roomsWithoutCode;
    {
        QSqlQuery q(database);
        if (!q.exec("SELECT room_id FROM rooms WHERE invite_code IS NULL OR invite_code=''"))
            return fail(q.lastError().text());
        while (q.next())
            roomsWithoutCode.append(q.value(0).toString());
    }
    for (const auto &roomId : roomsWithoutCode) {
        const QString code = freshInviteCode(database);
        if (code.isEmpty())
            return fail(QCoreApplication::translate("Storage", "Could not generate a unique invite code"));
        QSqlQuery q(database);
        q.prepare("UPDATE rooms SET invite_code=? WHERE room_id=?");
        q.addBindValue(code);
        q.addBindValue(roomId);
        if (!q.exec())
            return fail(q.lastError().text());
    }

    if (!database.commit()) {
        database.rollback();
        if (error)
            *error = database.lastError().text();
        return false;
    }
    return true;
}

bool Storage::wipeAccount(const QString &fp, bool alsoChatHistoryAndFiles, QString *error) {
    if (!exec("BEGIN TRANSACTION", error))
        return false;
    // Parameterized single-bind DELETE helper (exec() cannot bind values).
    auto del = [&](const QString &sql, const QString &bind, QString *why) {
        QSqlQuery q = query(sql);
        q.bindValue(0, bind);
        if (!q.exec()) {
            if (why)
                *why = QStringLiteral("%1 -> %2").arg(sql, q.lastError().text());
            return false;
        }
        return true;
    };
    auto fail = [&](const QString &why) {
        exec("ROLLBACK");
        if (error)
            *error = why;
        return false;
    };
    // 1) optional: this identity's room data (UC-02 step 6, user's choice).
    //    Multi-account: scope every delete by rooms.owner_fp so other local
    //    identities keep their rooms, history and downloads.
    if (alsoChatHistoryAndFiles) {
        QString why;
        const QString scoped = QStringLiteral(
            "DELETE FROM %1 WHERE room_id IN (SELECT room_id FROM rooms WHERE owner_fp=?)");
        // room_prefs first; the rest of the subqueries reference rooms itself.
        if (!del(scoped.arg(QStringLiteral("room_prefs")), fp, &why))
            return fail(why);
        // suppliers/chunk_progress carry no room_id: wipe by provider identity
        // and prune progress rows whose download task is gone.
        if (!del(QStringLiteral("DELETE FROM suppliers WHERE fingerprint=?"), fp, &why))
            return fail(why);
        for (const char *t : {"members", "blacklist", "approvals", "file_announce",
                              "downloads", "messages"}) {
            if (!del(scoped.arg(QLatin1String(t)), fp, &why))
                return fail(why);
        }
        if (!exec(QStringLiteral("DELETE FROM chunk_progress WHERE file_hash NOT IN"
                                 " (SELECT file_hash FROM downloads)"),
                  &why))
            return fail(why);
        if (!del(QStringLiteral("DELETE FROM rooms WHERE owner_fp=?"), fp, &why))
            return fail(why);
        // the finished-file index is content-addressed, not identity-scoped
        if (!exec("DELETE FROM local_files", &why))
            return fail(why);
    } else {
        // keep history/room structure, but drop *this identity's* membership rows
        if (!del(QStringLiteral("DELETE FROM members WHERE fingerprint=?"), fp, error))
            return fail(*error);
    }
    // 2) identity material (UC-02 step 5/6). Profile first (it references the
    //    account row); peers/tofu are peer-side caches that rebuild themselves
    //    and stay valid for other local accounts.
    QString why2;
    if (!del(QStringLiteral("DELETE FROM profile WHERE account_id IN"
                            " (SELECT id FROM account WHERE fingerprint=?)"),
             fp, &why2))
        return fail(why2);
    if (!del(QStringLiteral("DELETE FROM account WHERE fingerprint=?"), fp, &why2))
        return fail(why2);
    if (!exec("COMMIT", error))
        return fail(*error);
    return true;
}

} // namespace meshlink
