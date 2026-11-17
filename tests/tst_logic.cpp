// deepin-MeshLink — pure-logic unit test: framing, serialization, chunk math.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
//
// No sockets, no GUI: runs anywhere meshlink_core links. Failures abort with
// a descriptive message; success prints "tst_logic: ALL OK".
#include "core/chunkplan.h"
#include "core/crypto.h"
#include "core/lansegment.h"
#include "core/notifypolicy.h"
#include "core/protocol.h"
#include "core/storage.h"
#include "core/account.h"

#include <QByteArray>
#include <QDir>
#include <QGuiApplication>
#include <QHostAddress>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace meshlink;

static int g_checks = 0;
#define CHECK(cond)                                                                                          \
    do {                                                                                                     \
        ++g_checks;                                                                                          \
        if (!(cond)) {                                                                                       \
            std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);                           \
            return 1;                                                                                        \
        }                                                                                                    \
    } while (0)

static int testFraming() {
    MsgType t = MsgType::Error;
    QByteArray payload;
    int consumed = 0;

    const QByteArray frame = frameMessage(MsgType::ChatMessage, QByteArray("hello"));
    CHECK(frame.size() > kFrameHeaderSize);
    CHECK(tryParseFrame(frame, t, payload, consumed));
    CHECK(consumed == frame.size());
    CHECK(t == MsgType::ChatMessage);
    CHECK(payload == QByteArray("hello"));

    // truncated frame must not parse
    QByteArray truncated = frame.left(frame.size() - 1);
    CHECK(!tryParseFrame(truncated, t, payload, consumed));

    // corrupt magic must fail
    QByteArray bad = frame;
    bad[0] = quint8(bad[0] ^ 0xFF);
    CHECK(!tryParseFrame(bad, t, payload, consumed));

    // two frames back to back
    QByteArray duo = frame + frameMessage(MsgType::Heartbeat, {});
    CHECK(tryParseFrame(duo, t, payload, consumed));
    CHECK(consumed == frame.size());
    duo.remove(0, consumed);
    CHECK(tryParseFrame(duo, t, payload, consumed));
    CHECK(t == MsgType::Heartbeat);
    return 0;
}

static int testSerializers() {
    {
        AuthMsg a, b;
        a.fp = QStringLiteral("abcd");
        a.nickname = QStringLiteral("测试用户🙂");
        a.avatarHash = QString(64, 'f');
        a.ed25519Pub = QByteArray(32, 'p');
        a.nonce = QByteArray(16, '\0');
        a.sig = QByteArray(64, 's');
        CHECK(AuthMsg::fromPayload(a.toPayload(), b));
        CHECK(b.fp == a.fp && b.nickname == a.nickname && b.ed25519Pub == a.ed25519Pub && b.sig == a.sig);
    }
    {
        ChatMessageMsg a, b;
        a.roomId = "r1";
        a.msgId = "m1";
        a.senderFp = "fp";
        a.senderNick = QStringLiteral("深度织讯");
        a.kind = static_cast<quint8>(MsgKind::File);
        a.seq = 42;
        a.ts = 123456789012;
        a.text = QStringLiteral("多行\n内容");
        a.contentHash = QString(64, 'a');
        a.fileName = QStringLiteral("报告.pdf");
        a.fileSize = -7;
        CHECK(ChatMessageMsg::fromPayload(a.toPayload(), b));
        CHECK(b.seq == a.seq && b.text == a.text && b.fileName == a.fileName && b.fileSize == a.fileSize);
    }
    {
        ChunkDataMsg a, b;
        a.fileHash = QString(64, '7');
        a.index = 99999999999ull;
        a.data = QByteArray(257, '\x55');
        a.sha256 = QString(64, '1');
        CHECK(ChunkDataMsg::fromPayload(a.toPayload(), b));
        CHECK(b.index == a.index && b.data == a.data && b.sha256 == a.sha256);
    }
    {
        FileAnnounceMsg a, b;
        a.roomId = "r";
        a.msgId = "f-1";
        a.fileHash = QString(64, '2');
        a.fileName = QStringLiteral("名 with spaces.bin");
        a.mime = QStringLiteral("application/octet-stream");
        a.announcerFp = "fp2";
        a.announcerNick = QStringLiteral("节点2");
        a.fileSize = 1234567890123ll;
        a.ts = 5;
        a.mode = 1;
        CHECK(FileAnnounceMsg::fromPayload(a.toPayload(), b));
        CHECK(b.mode == 1 && b.fileName == a.fileName && b.fileSize == a.fileSize && b.msgId == a.msgId);
    }
    {
        MemberEventMsg a, b;
        a.roomId = "r";
        a.targetFp = "t";
        a.targetNick = QStringLiteral("被移出者");
        a.op = 4;
        a.role = 2;
        a.mutedUntil = -1;
        a.reason = QStringLiteral("违反群规");
        CHECK(MemberEventMsg::fromPayload(a.toPayload(), b));
        CHECK(b.op == 4 && b.mutedUntil == -1 && b.targetNick == a.targetNick && b.reason == a.reason);
    }
    {
        SuppliersResponseMsg a, b;
        a.fileHash = "h";
        SupplierInfo s;
        s.fp = "fp";
        s.nickname = QStringLiteral("供给者");
        s.ip = "10.0.0.9";
        s.port = 24261;
        s.online = true;
        a.suppliers = {s, s};
        CHECK(SuppliersResponseMsg::fromPayload(a.toPayload(), b));
        CHECK(b.suppliers.size() == 2 && b.suppliers[1].port == 24261 && b.suppliers[0].ip == "10.0.0.9");
    }
    {
        RoomSnapshotMsg a, b;
        a.roomId = QString(32, QLatin1Char('a'));
        a.name = QStringLiteral("研发讨论");
        a.inviteCode = QStringLiteral("123456");
        a.policy = static_cast<quint8>(JoinPolicy::Approval);
        RoomMemberInfo mi;
        mi.fp = "f1";
        mi.nickname = QStringLiteral("甲");
        mi.role = 0;
        a.members = {mi};
        Msg m;
        m.msgId = "m1";
        m.seq = 1;
        m.text = QStringLiteral("hi");
        a.recent = {m, m};
        CHECK(RoomSnapshotMsg::fromPayload(a.toPayload(), b));
        CHECK(b.members.size() == 1 && b.recent.size() == 2 && b.recent[0].seq == 1 && b.policy == a.policy);
        CHECK(b.inviteCode == a.inviteCode && b.roomId == a.roomId);
    }
    {
        // garbage payload must be rejected, not crash
        QByteArray junk = QByteArray::fromRawData("\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff", 10);
        ChatMessageMsg c;
        CHECK(!ChatMessageMsg::fromPayload(junk, c));
        ChunkRequestMsg cr;
        CHECK(!ChunkRequestMsg::fromPayload(junk, cr));
    }
    return 0;
}

static int testChunkPlan() {
    CHECK(clampChunkSize(100) == kChunkSizeMin);
    CHECK(clampChunkSize(50ull << 20) == kChunkSizeMax);
    CHECK(clampChunkSize(256ull << 10) == (256ull << 10));
    CHECK(totalChunks(0, 256 << 10) == 0);
    CHECK(totalChunks(1, 256 << 10) == 1);
    CHECK(totalChunks(qint64(256 << 10), 256ull << 10) == 1);
    CHECK(totalChunks(qint64((256 << 10) + 1), 256ull << 10) == 2);
    CHECK(chunkOffset(3, 256 << 10) == 3 * (256 << 10));
    CHECK(chunkLength(0, 100, 256 << 10) == 100);
    CHECK(chunkLength(1, 100, 256 << 10) == 0);
    const qint64 big = 1ll << 34; // 16 GiB
    CHECK(totalChunks(big, 1ull << 20) == (big >> 20));
    return 0;
}

static int testCrypto() {
    using C = meshlink::Crypto;
    auto id = C::generateIdentity();
    CHECK(id.has_value());
    CHECK(id->fingerprint.size() == 32);
    CHECK(C::fingerprintFromEd25519Pub(id->ed25519Pub) == id->fingerprint);

    QByteArray msg("ML-A1abcdef0123456789");
    Bytes sig;
    CHECK(C::ed25519Sign(id->ed25519Priv, Bytes(msg.begin(), msg.end()), sig));
    CHECK(C::ed25519Verify(id->ed25519Pub, Bytes(msg.begin(), msg.end()), sig));
    msg[3] = char(msg[3] + 1);
    CHECK(!C::ed25519Verify(id->ed25519Pub, Bytes(msg.begin(), msg.end()), sig)); // tampered

    auto tls = C::generateSelfSignedTls(id->fingerprint);
    CHECK(tls.has_value());
    CHECK(QString::fromStdString(tls->certPem).contains(QStringLiteral("BEGIN CERTIFICATE")));
    CHECK(C::certPemFingerprintHex(tls->certPem).size() == 64);

    Bytes key = C::randomBytes(32);
    Bytes k2 = C::deriveKek(C::randomBytes(16), C::randomBytes(16));
    CHECK(k2.size() == 32);
    const Bytes secret{1, 2, 3, 4, 5, 6, 7, 8};
    Bytes blob = C::aesGcmEncrypt(secret, key);
    CHECK(blob.size() > secret.size());
    auto back = C::aesGcmDecrypt(blob, key);
    CHECK(back.has_value() && *back == secret);
    blob[20] = uint8_t(blob[20] ^ 0x5A); // flip ciphertext
    auto tamper = C::aesGcmDecrypt(blob, key);
    CHECK(!tamper.has_value()); // GCM tag must reject
    return 0;
}

static int testLanSegment() {
    LanSegment a{QStringLiteral("eth0"), QStringLiteral("192.168.1.23"), 24};
    CHECK(a.isValid());
    CHECK(a.broadcastAddress().toString() == QStringLiteral("192.168.1.255"));
    CHECK(a.contains(QHostAddress(QStringLiteral("192.168.1.44"))));
    CHECK(!a.contains(QHostAddress(QStringLiteral("192.168.2.1"))));
    CHECK(!a.label().isEmpty());

    LanSegment b{QStringLiteral("eth0"), QStringLiteral("10.1.2.3"), 16};
    CHECK(b.broadcastAddress().toString() == QStringLiteral("10.1.255.255"));
    CHECK(b.contains(QHostAddress(QStringLiteral("10.1.200.7"))));
    CHECK(!b.contains(QHostAddress(QStringLiteral("10.2.0.1"))));

    LanSegment slash32{QStringLiteral("eth0"), QStringLiteral("172.16.5.4"), 32};
    CHECK(slash32.broadcastAddress().toString() == QStringLiteral("255.255.255.255"));
    CHECK(!slash32.contains(QHostAddress(QStringLiteral("172.16.5.5"))));

    LanSegment loop{QStringLiteral("lo"), QStringLiteral("127.0.0.1"), 8};
    CHECK(loop.contains(QHostAddress(QStringLiteral("127.0.0.1"))));
    CHECK(loop.broadcastAddress().toString() == QStringLiteral("255.255.255.255"));

    const auto all = LanSegments::enumerate();
    CHECK(!all.isEmpty());
    for (const auto &s : all)
        CHECK(s.isValid());
    CHECK(LanSegments::preferred().isValid());
    return 0;
}

static int testRoomMigration() {
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString path = tmp.filePath(QStringLiteral("migration.db"));
    const QString oldRoom = QStringLiteral("0123456789abcdef"); // 16-hex id from the 1.0.0 build
    {
        Storage storage;
        QString err;
        CHECK(storage.open(path, &err));
        QSqlQuery ins = storage.query(
            "INSERT INTO rooms (room_id,name,intro,policy,global_mute,capacity,host_fp,hosted,joined,created_ts)"
            " VALUES (?,?,?,0,0,0,?,'1',1,?)");
        ins.bindValue(0, oldRoom);
        ins.bindValue(1, QStringLiteral("旧房间"));
        ins.bindValue(2, QString());
        ins.bindValue(3, QStringLiteral("hostfp"));
        ins.bindValue(4, 123456);
        CHECK(ins.exec());
        QSqlQuery member = storage.query(
            "INSERT INTO members (room_id,fingerprint,nickname,role,joined_ts) VALUES (?,?,?,0,?)");
        member.bindValue(0, oldRoom);
        member.bindValue(1, QStringLiteral("memberfp"));
        member.bindValue(2, QStringLiteral("成员"));
        member.bindValue(3, 1);
        CHECK(member.exec());
        QSqlQuery msg = storage.query(
            "INSERT INTO messages (msg_id,room_id,seq,sender_fp,ts) VALUES (?,?,1,?,?)");
        msg.bindValue(0, QStringLiteral("m1"));
        msg.bindValue(1, oldRoom);
        msg.bindValue(2, QStringLiteral("memberfp"));
        msg.bindValue(3, 1);
        CHECK(msg.exec());
        storage.close();
    }
    {
        Storage storage;
        QString err;
        CHECK(storage.open(path, &err)); // migration runs here
        QSqlQuery rooms = storage.query("SELECT room_id,invite_code FROM rooms");
        CHECK(rooms.exec());
        CHECK(rooms.next());
        const QString newId = rooms.value(0).toString();
        const QString code = rooms.value(1).toString();
        CHECK(newId.size() == kRoomIdHexLen);
        CHECK(newId != oldRoom);
        CHECK(code.size() == kInviteCodeDigits);
        for (const QChar c : code)
            CHECK(c.isDigit());

        QSqlQuery members = storage.query("SELECT COUNT(*) FROM members WHERE room_id=?");
        members.bindValue(0, newId);
        CHECK(members.exec());
        CHECK(members.next() && members.value(0).toInt() == 1);
        QSqlQuery stale = storage.query("SELECT COUNT(*) FROM messages WHERE room_id=?");
        stale.bindValue(0, oldRoom);
        CHECK(stale.exec());
        CHECK(stale.next() && stale.value(0).toInt() == 0);

        // v3 migration adds reply/recall columns to pre-existing databases.
        QSqlQuery info = storage.query("PRAGMA table_info(messages)");
        CHECK(info.exec());
        QStringList columns;
        while (info.next())
            columns << info.value(1).toString();
        for (const QString &column : {QStringLiteral("reply_to"), QStringLiteral("recalled"),
                                      QStringLiteral("recalled_by"), QStringLiteral("recalled_ts"),
                                      QStringLiteral("recv_ts")})
            CHECK(columns.contains(column));
        storage.close();
    }
    return 0;
}

static int testRecallPolicy() {
    const qint64 now = 1700000000000;
    CHECK(canRecallMessage(MemberRole::Host, QStringLiteral("host"), QStringLiteral("other"),
                           now - 9999999, now));
    CHECK(canRecallMessage(MemberRole::Admin, QStringLiteral("admin"), QStringLiteral("other"),
                           now - 9999999, now));
    CHECK(canRecallMessage(MemberRole::Member, QStringLiteral("bob"), QStringLiteral("bob"),
                           now - kSelfRecallWindowMs, now));
    CHECK(!canRecallMessage(MemberRole::Member, QStringLiteral("bob"), QStringLiteral("bob"),
                            now - kSelfRecallWindowMs - 1, now));
    CHECK(!canRecallMessage(MemberRole::Member, QStringLiteral("bob"), QStringLiteral("alice"),
                            now, now));
    return 0;
}

static int testNotifyPolicy() {
    namespace NP = meshlink::NotifyPolicy;
    // ---- @-mention boundary matching ----
    CHECK(NP::textMentions(QStringLiteral("@alice hi"), QStringLiteral("alice")));
    CHECK(!NP::textMentions(QStringLiteral("@alicia hi"), QStringLiteral("alice")));
    CHECK(!NP::textMentions(QStringLiteral("mail me at bob@example.com"), QStringLiteral("bob")));
    CHECK(NP::textMentions(QStringLiteral("hey @Alice!"), QStringLiteral("alice"))); // case + boundary
    CHECK(NP::textMentions(QStringLiteral("(@张三) 在吗"), QStringLiteral("张三")));
    CHECK(!NP::textMentions(QStringLiteral("@张三丰 来了"), QStringLiteral("张三")));
    CHECK(!NP::textMentions(QStringLiteral("plain text"), QStringLiteral("alice")));
    CHECK(!NP::textMentions(QStringLiteral("@alice"), QString())); // empty nickname never matches

    // ---- mention-all tokens ----
    CHECK(NP::textMentionsAll(QStringLiteral("@all 开会")));
    CHECK(NP::textMentionsAll(QStringLiteral("请 @所有人 关注")));
    CHECK(NP::textMentionsAll(QStringLiteral("@全体成员 注意")));
    CHECK(!NP::textMentionsAll(QStringLiteral("ball is round")));
    CHECK(!NP::textMentionsAll(QStringLiteral("call it done"))); // "all" inside word

    // ---- roster-based self mention ----
    QVector<RoomMemberInfo> members;
    RoomMemberInfo me;
    me.fp = QStringLiteral("fp-me");
    me.nickname = QStringLiteral("小明");
    RoomMemberInfo other;
    other.fp = QStringLiteral("fp-other");
    other.nickname = QStringLiteral("小红");
    members = {other, me};
    CHECK(NP::textMentionsMe(QStringLiteral("@小明 在吗"), members, QStringLiteral("fp-me")));
    CHECK(!NP::textMentionsMe(QStringLiteral("@小红 在吗"), members, QStringLiteral("fp-me")));
    CHECK(!NP::textMentionsMe(QStringLiteral("@小明 在吗"), members, QStringLiteral("fp-other")));
    CHECK(NP::textMentionsMe(QStringLiteral("@all 开会"), members, QStringLiteral("fp-other")));
    CHECK(!NP::textMentionsMe(QStringLiteral("普通消息"), members, QStringLiteral("fp-me")));

    // ---- notification decision matrix ----
    NP::Context c;
    c.globalEnabled = true;
    c.windowActive = false;
    c.viewingRoom = false;
    c.roomDnd = false;
    c.mentionsMe = false;
    c.fromSelf = false;
    CHECK(NP::decide(c) == NP::Action::Notify);
    c.fromSelf = true;
    CHECK(NP::decide(c) == NP::Action::Skip); // own echo never notifies
    c.fromSelf = false;
    c.windowActive = true;
    c.viewingRoom = true;
    CHECK(NP::decide(c) == NP::Action::Skip); // event already on screen
    c.viewingRoom = false;
    CHECK(NP::decide(c) == NP::Action::Notify); // other room while focused
    c.roomDnd = true;
    CHECK(NP::decide(c) == NP::Action::Skip); // muted room
    c.mentionsMe = true;
    CHECK(NP::decide(c) == NP::Action::Notify); // @-mention overrides DND
    c.globalEnabled = false;
    CHECK(NP::decide(c) == NP::Action::Skip); // master switch wins over mentions
    return 0;
}

static int testAccountMulti() {
    // Headless multi-account lifecycle against a redirected data home.
    QTemporaryDir home;
    CHECK(home.isValid());
    qputenv("MESHLINK_DATA_HOME", home.path().toUtf8());
    qputenv("MESHLINK_CONFIG_HOME", home.path().toUtf8());

    Storage storage;
    QString err;
    CHECK(storage.open(home.filePath(QStringLiteral("multi.db")), &err));
    AccountManager accounts(&storage);

    CHECK(!accounts.hasAccount());
    const qint64 first = accounts.createNewAccount(QStringLiteral("小明"), &err);
    CHECK(first > 0);
    CHECK(accounts.hasAccount());
    CHECK(accounts.profile().valid);
    CHECK(accounts.profile().fingerprint.size() == 32);
    CHECK(accounts.profile().nickname == QStringLiteral("小明"));

    const qint64 second = accounts.createNewAccount(QStringLiteral("小红"), &err);
    CHECK(second > 0 && second != first);
    CHECK(accounts.activeRowId() == second); // newly created account is active
    CHECK(accounts.listAccounts().size() == 2);

    // Switch back and forth; keystore decryption must round-trip.
    CHECK(accounts.loadAccount(first, &err));
    CHECK(accounts.profile().nickname == QStringLiteral("小明"));
    CHECK(accounts.loadAccount(second, &err));
    CHECK(accounts.profile().nickname == QStringLiteral("小红"));

    // Deleting a non-active account keeps the active one intact.
    if (!accounts.deleteAccount(first, true, &err)) {
        std::fprintf(stderr, "deleteAccount failed: %s\n", qUtf8Printable(err));
        return 1;
    }
    CHECK(accounts.listAccounts().size() == 1);
    CHECK(accounts.profile().nickname == QStringLiteral("小红"));

    // Destroying the last account leaves an empty, reusable database.
    const int remaining = accounts.destroyAccount(true, &err);
    CHECK(remaining == 0);
    CHECK(!accounts.hasAccount());
    CHECK(accounts.createNewAccount(QStringLiteral("小亮"), &err) > 0);
    storage.close();
    return 0;
}

static int testRoomPrefsMigration() {
    // Legacy single-account database (account table CHECK(id=1), no owner_fp)
    // must rebuild into the multi-account schema and backfill room ownership.
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString path = tmp.filePath(QStringLiteral("legacy-account.db"));
    {
        Storage storage;
        QString err;
        CHECK(storage.open(path, &err));
        QSqlQuery drop = storage.query("DROP TABLE account");
        CHECK(drop.exec());
        // Recreate the 1.x account/profile tables exactly as the historical
        // build created them: hard-wired id, NOT NULL identity material.
        QSqlQuery legacy = storage.query(
            "CREATE TABLE account (id INTEGER PRIMARY KEY CHECK(id=1), fingerprint TEXT NOT NULL, "
            "nickname TEXT NOT NULL, ed25519_pub BLOB NOT NULL, x25519_pub BLOB NOT NULL, "
            "keystore_blob BLOB NOT NULL, cert_pem TEXT NOT NULL, created_ts INTEGER NOT NULL)");
        CHECK(legacy.exec());
        QSqlQuery ins = storage.query(
            "INSERT INTO account (id, fingerprint, nickname, ed25519_pub, x25519_pub, "
            "keystore_blob, cert_pem, created_ts) VALUES (1, 'legacyfp', '旧用户', x'0031', "
            "x'0032', x'0033', '-----BEGIN CERTIFICATE-----', 123)");
        CHECK(ins.exec());
        QSqlQuery dropProfile = storage.query("DROP TABLE profile");
        CHECK(dropProfile.exec());
        QSqlQuery legacyProfile = storage.query(
            "CREATE TABLE profile (id INTEGER PRIMARY KEY CHECK(id = 1), avatar_path TEXT, "
            "avatar_hash TEXT, lan_ip TEXT)");
        CHECK(legacyProfile.exec());
        QSqlQuery insProfile = storage.query(
            "INSERT INTO profile (id, avatar_path, avatar_hash, lan_ip) VALUES (1, '', '', '')");
        CHECK(insProfile.exec());
        QSqlQuery legacyRoom = storage.query(
            "INSERT INTO rooms (room_id,name,intro,policy,global_mute,capacity,host_fp,hosted,"
            "joined,created_ts) VALUES ('0123456789abcdef0123456789abcdef', '旧房间', NULL, 0, 0, 0,"
            " NULL, 1, 1, 1)");
        CHECK(legacyRoom.exec());
        storage.close();
    }
    {
        Storage storage;
        QString err;
        if (!storage.open(path, &err)) { // migration runs here
            std::fprintf(stderr, "reopen failed: %s\n", qUtf8Printable(err));
            return 1;
        }
        QSqlQuery account = storage.query("SELECT id, fingerprint FROM account");
        CHECK(account.exec());
        CHECK(account.next());
        CHECK(account.value(0).toLongLong() == 1);
        CHECK(account.value(1).toString() == QStringLiteral("legacyfp"));

        // Rooms created by the legacy account inherit its fingerprint as owner.
        QSqlQuery room = storage.query("SELECT owner_fp FROM rooms");
        CHECK(room.exec());
        CHECK(room.next());
        CHECK(room.value(0).toString() == QStringLiteral("legacyfp"));

        // Per-room preference table exists and upserts cleanly.
        QSqlQuery prefs = storage.query("SELECT COUNT(*) FROM room_prefs");
        CHECK(prefs.exec());
        CHECK(prefs.next() && prefs.value(0).toInt() == 0);
        QSqlQuery upsert = storage.query(
            "INSERT INTO room_prefs (room_id, dnd) VALUES ('0123456789abcdef0123456789abcdef', 1)"
            " ON CONFLICT(room_id) DO UPDATE SET dnd = excluded.dnd");
        CHECK(upsert.exec());
        QSqlQuery check = storage.query(
            "SELECT dnd FROM room_prefs WHERE room_id='0123456789abcdef0123456789abcdef'");
        CHECK(check.exec());
        CHECK(check.next() && check.value(0).toInt() == 1);
        storage.close();
    }
    return 0;
}

int main(int argc, char **argv) {
    // QGuiApplication (offscreen in ctest) because account creation renders the
    // default identicon avatar through QPainter/QPixmap.
    std::fprintf(stderr, "tst_logic: pre-app\n");
    QGuiApplication app(argc, argv);
    std::fprintf(stderr, "tst_logic: post-app\n");
    app.setApplicationName(QStringLiteral("tst_logic"));
    struct Entry {
        const char *name;
        int (*fn)();
    };
    const Entry entries[] = {
        {"framing", testFraming},       {"serializers", testSerializers},
        {"chunkplan", testChunkPlan},   {"crypto", testCrypto},
        {"lansegment", testLanSegment}, {"recall", testRecallPolicy},
        {"notifypolicy", testNotifyPolicy}, {"account", testAccountMulti},
        {"migration", testRoomMigration},   {"roomprefs", testRoomPrefsMigration},
    };
    if (argc > 1) { // debug helper: run a single suite only
        for (const auto &e : entries) {
            if (std::strcmp(argv[1], e.name) == 0)
                return e.fn();
        }
        return 2;
    }
    for (const auto &e : entries)
        if (e.fn())
            return 1;
    std::printf("tst_logic: ALL OK (%d checks)\n", g_checks);
    return 0;
}
