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
#include "core/settings.h"
#include "core/storage.h"
#include "core/account.h"
#include "core/engine.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHostAddress>
#include <QHostInfo>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
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

static QByteArray rawBytes(const Bytes &b) {
    return QByteArray(reinterpret_cast<const char *>(b.data()), int(b.size()));
}

static Bytes bytesOf(const QByteArray &b) {
    return Bytes(reinterpret_cast<const uint8_t *>(b.constData()),
                 reinterpret_cast<const uint8_t *>(b.constData()) + b.size());
}

// Historical on-disk plaintext layout (see AccountManager::packPrivate):
// ed25519Priv(32) || x25519Priv(32) || u32be(keyPemLen) || keyPem.
static QByteArray packLegacyPrivate(const Identity &id, const TlsMaterial &tls) {
    QByteArray out;
    out.append(rawBytes(id.ed25519Priv));
    out.append(rawBytes(id.x25519Priv));
    const quint32 n = quint32(tls.keyPem.size());
    out.append(char(quint8(n >> 24)));
    out.append(char(quint8(n >> 16)));
    out.append(char(quint8(n >> 8)));
    out.append(char(quint8(n)));
    out.append(tls.keyPem.data(), int(tls.keyPem.size()));
    return out;
}

static QByteArray readFileBytes(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

static bool writeFileBytes(const QString &path, const QByteArray &data) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const bool ok = f.write(data) == data.size();
    f.close();
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
    return ok;
}

// Replace an account row with a legacy machine-id-encrypted keystore blob, the
// same shape the pre-device-key build produced.
static bool replaceAccountWithLegacyBlob(Storage &storage, qint64 row, const Identity &id,
                                         const TlsMaterial &tls, const QByteArray &machineIdentity,
                                         const QByteArray &salt, QByteArray *blobBytes) {
    const QByteArray factor = machineIdentity + QByteArray::number(static_cast<qint64>(::getuid()));
    const QByteArray plain = packLegacyPrivate(id, tls);
    const Bytes blob = Crypto::aesGcmEncrypt(bytesOf(plain),
                                             Crypto::deriveKek(bytesOf(factor), bytesOf(salt)));
    if (blob.empty())
        return false;
    QSqlQuery up = storage.query("UPDATE account SET fingerprint=?,ed25519_pub=?,x25519_pub=?,"
                                 "keystore_blob=?,cert_pem=? WHERE id=?");
    up.bindValue(0, QString::fromStdString(id.fingerprint));
    up.bindValue(1, rawBytes(id.ed25519Pub));
    up.bindValue(2, rawBytes(id.x25519Pub));
    up.bindValue(3, rawBytes(blob));
    up.bindValue(4, QString::fromStdString(tls.certPem));
    up.bindValue(5, row);
    if (!up.exec())
        return false;
    if (blobBytes)
        *blobBytes = rawBytes(blob);
    return true;
}

static QByteArray readAccountBlob(Storage &storage, qint64 id) {
    QSqlQuery q = storage.query("SELECT keystore_blob FROM account WHERE id=?");
    q.bindValue(0, id);
    q.exec();
    return q.next() ? q.value(0).toByteArray() : QByteArray();
}

static QString legacyCopyPath(qint64 id) {
    return QFileInfo(Paths::keystoreDeviceKey()).absolutePath()
           + QStringLiteral("/legacy/%1.blob").arg(id);
}

static QByteArray deviceTagFor(const QByteArray &key) {
    const QByteArray context("meshlink.keystore.device-key.v1");
    return rawBytes(Crypto::hmacSha256(bytesOf(key), bytesOf(context)));
}

static int testKeystoreDeviceBinding() {
    using C = meshlink::Crypto;
    QTemporaryDir home;
    CHECK(home.isValid());
    qputenv("MESHLINK_DATA_HOME", home.path().toUtf8());
    qputenv("MESHLINK_CONFIG_HOME", home.path().toUtf8());
    // Test-only override simulating the legacy machine identity (machine-id +
    // uid). It is only read by the migration fallback, never by the new KEK.
    qputenv("MESHLINK_LEGACY_MACHINE_FACTOR", QByteArray("machine-id-A"));

    Storage storage;
    QString err;
    CHECK(storage.open(home.filePath(QStringLiteral("keystore.db")), &err));
    AccountManager accounts(&storage);

    // ---- (a) new account, device-secret KEK + permissions --------------------
    const qint64 first = accounts.createNewAccount(QStringLiteral("Keystore"), &err);
    CHECK(first > 0);
    const QString fingerprint = accounts.profile().fingerprint;
    const QByteArray blob0 = readAccountBlob(storage, first);
    CHECK(blob0.size() >= 16 + 12 + 64 + 4 + 16); // salt || iv || ct || tag
    CHECK(!blob0.contains("BEGIN PRIVATE KEY"));
    CHECK(!blob0.contains("PRIVATE KEY"));

    const QString deviceKeyPath = Paths::keystoreDeviceKey();
    CHECK(QFile::exists(deviceKeyPath));
    CHECK(QFileInfo(deviceKeyPath).size() == 32);
    const QFile::Permissions keyPerms = QFile::permissions(deviceKeyPath);
    CHECK(keyPerms.testFlag(QFileDevice::ReadOwner));
    CHECK(keyPerms.testFlag(QFileDevice::WriteOwner));
    CHECK(!keyPerms.testFlag(QFileDevice::ExeOwner));
    CHECK(!keyPerms.testFlag(QFileDevice::ReadGroup));
    CHECK(!keyPerms.testFlag(QFileDevice::WriteGroup));
    CHECK(!keyPerms.testFlag(QFileDevice::ReadOther));
    CHECK(!keyPerms.testFlag(QFileDevice::WriteOther));
    const QString deviceTagPath = deviceKeyPath + QStringLiteral(".tag");
    CHECK(QFile::exists(deviceTagPath));
    CHECK(QFileInfo(deviceTagPath).size() == 32);
    CHECK(QFile::permissions(deviceTagPath).testFlag(QFileDevice::ReadOwner));
    CHECK(!QFile::permissions(deviceTagPath).testFlag(QFileDevice::ReadOther));
    const QFile::Permissions dirPerms = QFile::permissions(home.path() + QStringLiteral("/keystore"));
    CHECK(dirPerms.testFlag(QFileDevice::ReadOwner));
    CHECK(dirPerms.testFlag(QFileDevice::WriteOwner));
    CHECK(dirPerms.testFlag(QFileDevice::ExeOwner));
    CHECK(!dirPerms.testFlag(QFileDevice::ReadGroup));
    CHECK(!dirPerms.testFlag(QFileDevice::ExeGroup));
    CHECK(!dirPerms.testFlag(QFileDevice::ReadOther));
    CHECK(!dirPerms.testFlag(QFileDevice::ExeOther));

    // Simulate a machine-id / hostname change: the new KEK must not care.
    const QByteArray originalDeviceKey = readFileBytes(deviceKeyPath);
    const QByteArray originalDeviceTag = readFileBytes(deviceTagPath);
    CHECK(originalDeviceKey.size() == 32);
    CHECK(originalDeviceTag.size() == 32);
    qputenv("MESHLINK_LEGACY_MACHINE_FACTOR", QByteArray("machine-id-B"));
    accounts.resetLoaded();
    CHECK(accounts.loadAccount(first, &err));
    CHECK(accounts.profile().fingerprint == fingerprint);
    CHECK(readAccountBlob(storage, first) == blob0); // no needless rewrite

    // ---- (b) legacy machine-id blob decrypts and migrates --------------------
    const auto legacyId = C::generateIdentity();
    CHECK(legacyId.has_value());
    const auto legacyTls = C::generateSelfSignedTls(legacyId->fingerprint);
    CHECK(legacyTls.has_value());
    const qint64 legacyRow = accounts.createNewAccount(QStringLiteral("Legacy"), &err);
    CHECK(legacyRow > 0 && legacyRow != first);

    // The legacy blob was written under machine identity A.
    qputenv("MESHLINK_LEGACY_MACHINE_FACTOR", QByteArray("machine-id-A"));
    const QByteArray saltRaw = readFileBytes(Paths::keystoreSalt());
    CHECK(saltRaw.size() == 16);
    const QByteArray factorRaw =
        QByteArray("machine-id-A") + QByteArray::number(static_cast<qint64>(::getuid()));
    const QByteArray legacyPlain = packLegacyPrivate(*legacyId, *legacyTls);
    const Bytes legacyBlob = C::aesGcmEncrypt(bytesOf(legacyPlain),
                                              C::deriveKek(bytesOf(factorRaw), bytesOf(saltRaw)));
    CHECK(!legacyBlob.empty());
    {
        QSqlQuery up = storage.query("UPDATE account SET fingerprint=?,ed25519_pub=?,x25519_pub=?,"
                                     "keystore_blob=?,cert_pem=? WHERE id=?");
        up.bindValue(0, QString::fromStdString(legacyId->fingerprint));
        up.bindValue(1, rawBytes(legacyId->ed25519Pub));
        up.bindValue(2, rawBytes(legacyId->x25519Pub));
        up.bindValue(3, rawBytes(legacyBlob));
        up.bindValue(4, QString::fromStdString(legacyTls->certPem));
        up.bindValue(5, legacyRow);
        CHECK(up.exec());
    }
    CHECK(readAccountBlob(storage, legacyRow) == rawBytes(legacyBlob));

    accounts.resetLoaded();
    CHECK(accounts.loadAccount(legacyRow, &err)); // migration happens here
    CHECK(accounts.profile().fingerprint == QString::fromStdString(legacyId->fingerprint));
    const QByteArray migrated = readAccountBlob(storage, legacyRow);
    CHECK(!migrated.isEmpty());
    CHECK(migrated != rawBytes(legacyBlob)); // old blob replaced
    CHECK(!migrated.contains("BEGIN PRIVATE KEY"));
    CHECK(!migrated.contains(rawBytes(legacyId->ed25519Priv)));
    CHECK(!migrated.contains(rawBytes(legacyId->x25519Priv)));

    // The migrated blob is now device-key bound: another machine-id change and
    // a fresh process-style reload still succeed without rewriting it.
    qputenv("MESHLINK_LEGACY_MACHINE_FACTOR", QByteArray("machine-id-C"));
    accounts.resetLoaded();
    CHECK(accounts.loadAccount(legacyRow, &err));
    CHECK(readAccountBlob(storage, legacyRow) == migrated);

    // Same migration through the production machine-identity resolver (no test
    // override): build a legacy blob under the real machine-id/hostname.
    qunsetenv("MESHLINK_LEGACY_MACHINE_FACTOR");
    QByteArray realIdentity;
    for (const char *path : {"/etc/machine-id", "/var/lib/dbus/machine-id"}) {
        QFile f(QString::fromLatin1(path));
        if (f.open(QIODevice::ReadOnly)) {
            realIdentity = f.readAll().trimmed();
            if (!realIdentity.isEmpty())
                break;
        }
    }
    if (realIdentity.isEmpty())
        realIdentity = QHostInfo::localHostName().toUtf8();
    CHECK(!realIdentity.isEmpty());
    const auto realId = C::generateIdentity();
    CHECK(realId.has_value());
    const auto realTls = C::generateSelfSignedTls(realId->fingerprint);
    CHECK(realTls.has_value());
    const qint64 realLegacyRow = accounts.createNewAccount(QStringLiteral("RealLegacy"), &err);
    CHECK(realLegacyRow > 0);
    {
        const QByteArray factor = realIdentity + QByteArray::number(static_cast<qint64>(::getuid()));
        const QByteArray plain = packLegacyPrivate(*realId, *realTls);
        const Bytes blob = C::aesGcmEncrypt(bytesOf(plain), C::deriveKek(bytesOf(factor), bytesOf(saltRaw)));
        CHECK(!blob.empty());
        QSqlQuery up = storage.query("UPDATE account SET fingerprint=?,ed25519_pub=?,x25519_pub=?,"
                                     "keystore_blob=?,cert_pem=? WHERE id=?");
        up.bindValue(0, QString::fromStdString(realId->fingerprint));
        up.bindValue(1, rawBytes(realId->ed25519Pub));
        up.bindValue(2, rawBytes(realId->x25519Pub));
        up.bindValue(3, rawBytes(blob));
        up.bindValue(4, QString::fromStdString(realTls->certPem));
        up.bindValue(5, realLegacyRow);
        CHECK(up.exec());
        accounts.resetLoaded();
        CHECK(accounts.loadAccount(realLegacyRow, &err));
        CHECK(accounts.profile().fingerprint == QString::fromStdString(realId->fingerprint));
        CHECK(readAccountBlob(storage, realLegacyRow) != rawBytes(blob));
    }

    // ---- (c) rotated device secret: clean failure, nothing destroyed ---------
    QByteArray wrongKey(32, char(0x5A));
    {
        QFile f(deviceKeyPath);
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        CHECK(f.write(wrongKey) == wrongKey.size());
        f.close();
        CHECK(QFile::setPermissions(deviceKeyPath, QFile::ReadOwner | QFile::WriteOwner));
    }
    accounts.resetLoaded();
    QString wrongErr;
    CHECK(!accounts.loadAccount(first, &wrongErr));
    CHECK(wrongErr == QStringLiteral("Private key decryption failed (device or credentials changed)"));
    CHECK(readAccountBlob(storage, first) == blob0); // original blob untouched
    CHECK(readFileBytes(deviceKeyPath) == wrongKey); // existing key never overwritten

    // A malformed (wrong-size) device secret is also preserved, never replaced.
    {
        QFile f(deviceKeyPath);
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        CHECK(f.write(wrongKey.left(31)) == 31);
        f.close();
        CHECK(QFile::setPermissions(deviceKeyPath, QFile::ReadOwner | QFile::WriteOwner));
    }
    accounts.resetLoaded();
    CHECK(!accounts.loadAccount(first, &err));
    CHECK(readFileBytes(deviceKeyPath).size() == 31);
    // New accounts also refuse to proceed instead of replacing a broken secret.
    CHECK(accounts.createNewAccount(QStringLiteral("ShouldFail"), &err) < 0);
    CHECK(readFileBytes(deviceKeyPath).size() == 31);

    // ---- (d) missing device secret with a surviving tag ----------------------
    // This is an ambiguous partial restore: never generate a replacement key or
    // overwrite the tag while the original key may still be restored.
    CHECK(QFile::remove(deviceKeyPath));
    accounts.resetLoaded();
    QString missingErr;
    CHECK(!accounts.loadAccount(first, &missingErr));
    CHECK(missingErr == QStringLiteral("Private key decryption failed (device or credentials changed)"));
    CHECK(readAccountBlob(storage, first) == blob0);
    CHECK(!QFile::exists(deviceKeyPath)); // no replacement key is created
    CHECK(readFileBytes(deviceTagPath) == originalDeviceTag); // tag untouched

    // Restoring the original key makes the account readable again.
    CHECK(writeFileBytes(deviceKeyPath, originalDeviceKey));
    accounts.resetLoaded();
    CHECK(accounts.loadAccount(first, &err));
    CHECK(accounts.profile().fingerprint == fingerprint);

    // New blobs use the restored device secret consistently.
    const qint64 fresh = accounts.createNewAccount(QStringLiteral("Fresh"), &err);
    CHECK(fresh > 0);
    accounts.resetLoaded();
    CHECK(accounts.loadAccount(fresh, &err));
    CHECK(accounts.profile().fingerprint.size() == 32);

    storage.close();
    qunsetenv("MESHLINK_LEGACY_MACHINE_FACTOR");
    return 0;
}

static int testDestroyKeepsOtherAccounts() {
    // The salt and the device secret are install-wide: destroying one account
    // must not delete them while another account still needs them.
    QTemporaryDir home;
    CHECK(home.isValid());
    qputenv("MESHLINK_DATA_HOME", home.path().toUtf8());
    qputenv("MESHLINK_CONFIG_HOME", home.path().toUtf8());
    qunsetenv("MESHLINK_LEGACY_MACHINE_FACTOR");

    Storage storage;
    QString err;
    CHECK(storage.open(home.filePath(QStringLiteral("shared.db")), &err));
    AccountManager accounts(&storage);

    const qint64 first = accounts.createNewAccount(QStringLiteral("KeepMe"), &err);
    CHECK(first > 0);
    const QString firstFp = accounts.profile().fingerprint;
    const qint64 second = accounts.createNewAccount(QStringLiteral("DestroyMe"), &err);
    CHECK(second > 0 && second != first);

    CHECK(QFile::exists(Paths::keystoreSalt()));
    CHECK(QFile::exists(Paths::keystoreDeviceKey()));
    CHECK(QFile::exists(Paths::keystoreDeviceKey() + QStringLiteral(".tag")));
    CHECK(accounts.destroyAccount(false, &err) == 1); // second is active
    CHECK(QFile::exists(Paths::keystoreSalt()));       // still needed by first
    CHECK(QFile::exists(Paths::keystoreDeviceKey()));
    CHECK(QFile::exists(Paths::keystoreDeviceKey() + QStringLiteral(".tag")));

    accounts.resetLoaded();
    CHECK(accounts.loadAccount(first, &err));
    CHECK(accounts.profile().fingerprint == firstFp);

    CHECK(accounts.destroyAccount(false, &err) == 0); // last account gone
    CHECK(!QFile::exists(Paths::keystoreSalt()));      // secrets wiped with it
    CHECK(!QFile::exists(Paths::keystoreDeviceKey()));
    CHECK(!QFile::exists(Paths::keystoreDeviceKey() + QStringLiteral(".tag")));
    storage.close();
    return 0;
}

static int testDeviceKeyProvenance() {
    using C = meshlink::Crypto;
    QTemporaryDir home;
    CHECK(home.isValid());
    qputenv("MESHLINK_DATA_HOME", home.path().toUtf8());
    qputenv("MESHLINK_CONFIG_HOME", home.path().toUtf8());
    qputenv("MESHLINK_LEGACY_MACHINE_FACTOR", QByteArray("machine-id-A"));

    Storage storage;
    QString err;
    CHECK(storage.open(home.filePath(QStringLiteral("provenance.db")), &err));
    AccountManager accounts(&storage);

    const qint64 first = accounts.createNewAccount(QStringLiteral("Provenance"), &err);
    CHECK(first > 0);
    const QString keyPath = Paths::keystoreDeviceKey();
    const QString tagPath = keyPath + QStringLiteral(".tag");
    const QByteArray goodKey = readFileBytes(keyPath);
    const QByteArray goodTag = readFileBytes(tagPath);
    CHECK(goodKey.size() == 32);
    CHECK(goodTag.size() == 32);

    const auto legacyId = C::generateIdentity();
    CHECK(legacyId.has_value());
    const auto legacyTls = C::generateSelfSignedTls(legacyId->fingerprint);
    CHECK(legacyTls.has_value());
    const qint64 legacyRow = accounts.createNewAccount(QStringLiteral("LegacyProvenance"), &err);
    CHECK(legacyRow > 0 && legacyRow != first);
    const QByteArray saltRaw = readFileBytes(Paths::keystoreSalt());
    CHECK(saltRaw.size() == 16);
    QByteArray legacyBlobBytes;
    CHECK(replaceAccountWithLegacyBlob(storage, legacyRow, *legacyId, *legacyTls,
                                       QByteArray("machine-id-A"), saltRaw, &legacyBlobBytes));
    CHECK(readAccountBlob(storage, legacyRow) == legacyBlobBytes);

    // A valid-but-wrong 32-byte device.key (its tag still belongs to the
    // original key): the legacy blob decrypts for this session, but it must
    // stay on disk unchanged so restoring the original key can still recover it.
    const QByteArray wrongKey(32, char(0x5A));
    CHECK(writeFileBytes(keyPath, wrongKey));
    accounts.resetLoaded();
    CHECK(accounts.loadAccount(legacyRow, &err));
    CHECK(accounts.profile().fingerprint == QString::fromStdString(legacyId->fingerprint));
    CHECK(readAccountBlob(storage, legacyRow) == legacyBlobBytes); // unchanged

    // Restore the original device key: provenance matches the tag again, so the
    // legacy blob is migrated exactly once.
    CHECK(writeFileBytes(keyPath, goodKey));
    CHECK(readFileBytes(tagPath) == goodTag);
    accounts.resetLoaded();
    CHECK(accounts.loadAccount(legacyRow, &err));
    const QByteArray migrated = readAccountBlob(storage, legacyRow);
    CHECK(migrated != legacyBlobBytes);
    qputenv("MESHLINK_LEGACY_MACHINE_FACTOR", QByteArray("machine-id-Z"));
    accounts.resetLoaded();
    CHECK(accounts.loadAccount(legacyRow, &err));
    CHECK(readAccountBlob(storage, legacyRow) == migrated); // device-key bound now

    // First-upgrade path: no device secret exists yet. Creating it during load
    // is the normal provenance for migrating a legacy blob.
    const auto upgradeId = C::generateIdentity();
    CHECK(upgradeId.has_value());
    const auto upgradeTls = C::generateSelfSignedTls(upgradeId->fingerprint);
    CHECK(upgradeTls.has_value());
    const qint64 upgradeRow = accounts.createNewAccount(QStringLiteral("FirstUpgrade"), &err);
    CHECK(upgradeRow > 0);
    qputenv("MESHLINK_LEGACY_MACHINE_FACTOR", QByteArray("machine-id-A"));
    QByteArray upgradeLegacyBytes;
    CHECK(replaceAccountWithLegacyBlob(storage, upgradeRow, *upgradeId, *upgradeTls,
                                       QByteArray("machine-id-A"), saltRaw, &upgradeLegacyBytes));
    CHECK(QFile::remove(keyPath));
    CHECK(QFile::remove(tagPath));
    accounts.resetLoaded();
    CHECK(accounts.loadAccount(upgradeRow, &err));
    CHECK(readFileBytes(keyPath).size() == 32); // created during this load
    CHECK(readFileBytes(tagPath).size() == 32); // tagged as verified
    const QByteArray upgradeMigrated = readAccountBlob(storage, upgradeRow);
    CHECK(upgradeMigrated != upgradeLegacyBytes);
    qunsetenv("MESHLINK_LEGACY_MACHINE_FACTOR");
    accounts.resetLoaded();
    CHECK(accounts.loadAccount(upgradeRow, &err));
    CHECK(readAccountBlob(storage, upgradeRow) == upgradeMigrated);

    storage.close();
    return 0;
}

static int testLegacyRetention() {
    // Reproduces t14-F1/F2 and proves the retained legacy ciphertext makes the
    // migration non-destructive even after the device key is later lost.
    using C = meshlink::Crypto;
    QTemporaryDir home;
    CHECK(home.isValid());
    qputenv("MESHLINK_DATA_HOME", home.path().toUtf8());
    qputenv("MESHLINK_CONFIG_HOME", home.path().toUtf8());
    qputenv("MESHLINK_LEGACY_MACHINE_FACTOR", QByteArray("machine-id-A"));

    Storage storage;
    QString err;
    CHECK(storage.open(home.filePath(QStringLiteral("retention.db")), &err));
    AccountManager accounts(&storage);

    // One device-key account is the independent provenance for this database.
    const qint64 proof = accounts.createNewAccount(QStringLiteral("Proof"), &err);
    CHECK(proof > 0);
    const QString keyPath = Paths::keystoreDeviceKey();
    const QString tagPath = keyPath + QStringLiteral(".tag");
    const QByteArray goodKey = readFileBytes(keyPath);
    const QByteArray goodTag = readFileBytes(tagPath);
    CHECK(goodKey.size() == 32);
    CHECK(goodTag.size() == 32);
    const QByteArray saltRaw = readFileBytes(Paths::keystoreSalt());
    CHECK(saltRaw.size() == 16);

    const auto idA = C::generateIdentity();
    CHECK(idA.has_value());
    const auto tlsA = C::generateSelfSignedTls(idA->fingerprint);
    CHECK(tlsA.has_value());
    const qint64 rowA = accounts.createNewAccount(QStringLiteral("LegacyA"), &err);
    CHECK(rowA > 0);
    QByteArray blobA;
    CHECK(replaceAccountWithLegacyBlob(storage, rowA, *idA, *tlsA, QByteArray("machine-id-A"),
                                       saltRaw, &blobA));

    const auto idB = C::generateIdentity();
    CHECK(idB.has_value());
    const auto tlsB = C::generateSelfSignedTls(idB->fingerprint);
    CHECK(tlsB.has_value());
    const qint64 rowB = accounts.createNewAccount(QStringLiteral("LegacyB"), &err);
    CHECK(rowB > 0);
    QByteArray blobB;
    CHECK(replaceAccountWithLegacyBlob(storage, rowB, *idB, *tlsB, QByteArray("machine-id-A"),
                                       saltRaw, &blobB));

    // ---- t14-F1: device.key missing, tag survives ----------------------------
    CHECK(QFile::remove(keyPath));
    accounts.resetLoaded();
    CHECK(accounts.loadAccount(rowA, &err)); // legacy fallback, read-only session
    CHECK(readAccountBlob(storage, rowA) == blobA); // never migrated without the key
    CHECK(!QFile::exists(keyPath)); // no replacement key generated
    CHECK(readFileBytes(tagPath) == goodTag); // tag untouched
    CHECK(!QFile::exists(legacyCopyPath(rowA))); // no copy while not migrated

    // Restoring the original key/tag proves provenance via the proof row and
    // allows the migration to proceed with a retained fallback copy.
    CHECK(writeFileBytes(keyPath, goodKey));
    CHECK(writeFileBytes(tagPath, goodTag));
    accounts.resetLoaded();
    CHECK(accounts.loadAccount(rowA, &err));
    const QByteArray migratedA = readAccountBlob(storage, rowA);
    CHECK(migratedA != blobA);
    const QByteArray copyA = readFileBytes(legacyCopyPath(rowA));
    CHECK(copyA == blobA); // byte-identical legacy ciphertext retained
    CHECK(QFileInfo(legacyCopyPath(rowA)).size() == blobA.size());
    const QFile::Permissions copyPerms = QFile::permissions(legacyCopyPath(rowA));
    CHECK(copyPerms.testFlag(QFileDevice::ReadOwner));
    CHECK(copyPerms.testFlag(QFileDevice::WriteOwner));
    CHECK(!copyPerms.testFlag(QFileDevice::ReadGroup));
    CHECK(!copyPerms.testFlag(QFileDevice::ReadOther));
    CHECK(!copyA.contains("PRIVATE KEY"));
    CHECK(!copyA.contains(rawBytes(idA->ed25519Priv)));
    const QString legacyDir = QFileInfo(Paths::keystoreDeviceKey()).absolutePath()
                              + QStringLiteral("/legacy");
    const QFile::Permissions legacyDirPerms = QFile::permissions(legacyDir);
    CHECK(legacyDirPerms.testFlag(QFileDevice::ReadOwner));
    CHECK(legacyDirPerms.testFlag(QFileDevice::WriteOwner));
    CHECK(legacyDirPerms.testFlag(QFileDevice::ExeOwner));
    CHECK(!legacyDirPerms.testFlag(QFileDevice::ReadGroup));
    CHECK(!legacyDirPerms.testFlag(QFileDevice::ReadOther));

    // ---- t14-F2: valid wrong key plus its own matching tag -------------------
    const QByteArray wrongKey(32, char(0x5A));
    CHECK(writeFileBytes(keyPath, wrongKey));
    CHECK(writeFileBytes(tagPath, deviceTagFor(wrongKey))); // matching but foreign
    accounts.resetLoaded();
    CHECK(accounts.loadAccount(rowB, &err)); // legacy fallback, read-only
    CHECK(readAccountBlob(storage, rowB) == blobB); // never migrated with wrong key
    CHECK(!QFile::exists(legacyCopyPath(rowB)));
    CHECK(readFileBytes(keyPath) == wrongKey); // wrong key never overwritten
    CHECK(readFileBytes(tagPath) == deviceTagFor(wrongKey));

    // Restoring the original key/tag restores provenance and migrates safely.
    CHECK(writeFileBytes(keyPath, goodKey));
    CHECK(writeFileBytes(tagPath, goodTag));
    accounts.resetLoaded();
    CHECK(accounts.loadAccount(rowB, &err));
    const QByteArray migratedB = readAccountBlob(storage, rowB);
    CHECK(migratedB != blobB);
    const QByteArray copyB = readFileBytes(legacyCopyPath(rowB));
    CHECK(copyB == blobB);
    CHECK(QFileInfo(legacyCopyPath(rowB)).size() == blobB.size());
    CHECK(QFile::permissions(legacyCopyPath(rowB)).testFlag(QFileDevice::ReadOwner));
    CHECK(!QFile::permissions(legacyCopyPath(rowB)).testFlag(QFileDevice::ReadOther));
    CHECK(!copyB.contains("PRIVATE KEY"));

    // A wrong key after migration also recovers through the retained copy,
    // without touching the already-migrated live row.
    CHECK(writeFileBytes(keyPath, wrongKey));
    CHECK(writeFileBytes(tagPath, deviceTagFor(wrongKey)));
    accounts.resetLoaded();
    CHECK(accounts.loadAccount(rowB, &err));
    CHECK(readAccountBlob(storage, rowB) == migratedB);
    CHECK(writeFileBytes(keyPath, goodKey));
    CHECK(writeFileBytes(tagPath, goodTag));

    // ---- retained fallback after the device key is lost again ----------------
    CHECK(QFile::remove(keyPath)); // tag still present
    accounts.resetLoaded();
    CHECK(accounts.loadAccount(rowA, &err)); // live row is device-key bound now
    CHECK(readAccountBlob(storage, rowA) == migratedA); // row untouched
    CHECK(accounts.loadAccount(rowB, &err));
    CHECK(readAccountBlob(storage, rowB) == migratedB);
    CHECK(!QFile::exists(keyPath)); // still no replacement key

    // Restore the key for normal use and cleanup.
    CHECK(writeFileBytes(keyPath, goodKey));
    CHECK(writeFileBytes(tagPath, goodTag));

    // ---- retained copies are wiped by the checked-COUNT destroy path ---------
    accounts.resetLoaded();
    CHECK(accounts.loadAccount(rowB, &err)); // active = rowB
    CHECK(accounts.destroyAccount(false, &err) == 2); // proof + rowA remain
    CHECK(!QFile::exists(legacyCopyPath(rowB))); // destroyed account copy gone
    CHECK(QFile::exists(legacyCopyPath(rowA))); // other account copy still needed
    CHECK(QFile::exists(Paths::keystoreSalt()));
    CHECK(QFile::exists(keyPath));

    accounts.resetLoaded();
    CHECK(accounts.loadAccount(rowA, &err));
    CHECK(accounts.destroyAccount(false, &err) == 1); // proof remains
    CHECK(!QFile::exists(legacyCopyPath(rowA)));

    accounts.resetLoaded();
    CHECK(accounts.loadAccount(proof, &err));
    CHECK(accounts.destroyAccount(false, &err) == 0); // last account
    CHECK(!QFile::exists(Paths::keystoreSalt()));
    CHECK(!QFile::exists(keyPath));
    CHECK(!QFile::exists(tagPath));
    CHECK(!QDir(QFileInfo(Paths::keystoreDeviceKey()).absolutePath() + QStringLiteral("/legacy"))
                .exists());

    storage.close();
    qunsetenv("MESHLINK_LEGACY_MACHINE_FACTOR");
    return 0;
}

static int testEngineAccountLifecycle() {
    // Failed switch rollback + destroy-by-id with a live engine. The engine is
    // started on loopback with ephemeral ports so the regression also proves
    // the network is restarted after a failed identity switch.
    QTemporaryDir home;
    CHECK(home.isValid());
    qputenv("MESHLINK_DATA_HOME", home.path().toUtf8());
    qputenv("MESHLINK_CONFIG_HOME", home.path().toUtf8());
    qunsetenv("MESHLINK_LEGACY_MACHINE_FACTOR");
    qunsetenv("MESHLINK_TEST_FAIL_ACCOUNT_COUNT");

    Settings settings;
    settings.setTcpPort(0);
    settings.setDiscoveryPort(0);
    LanSegment segment;
    segment.interfaceName = QStringLiteral("lo");
    segment.ip = QStringLiteral("127.0.0.1");
    segment.prefixLength = 8;
    settings.setLanSegment(segment);

    Engine engine(&settings);
    QString err;
    CHECK(engine.storage()->open(Paths::databaseFile(), &err));
    const qint64 first = engine.account()->createNewAccount(QStringLiteral("Stable"), &err);
    CHECK(first > 0);
    const QString firstFp = engine.account()->profile().fingerprint;
    const qint64 broken = engine.account()->createNewAccount(QStringLiteral("Broken"), &err);
    CHECK(broken > 0 && broken != first);
    const QString brokenFp = engine.account()->profile().fingerprint;

    settings.setActiveAccountId(first);
    engine.account()->resetLoaded();
    CHECK(engine.start(&err));
    CHECK(engine.networkRunning());
    CHECK(engine.account()->activeRowId() == first);
    CHECK(engine.account()->profile().fingerprint == firstFp);

    // Make the target's keystore undecryptable, then ask the engine to switch.
    {
        QSqlQuery corrupt = engine.storage()->query("UPDATE account SET keystore_blob=? WHERE id=?");
        corrupt.bindValue(0, QByteArray(64, 'X'));
        corrupt.bindValue(1, broken);
        CHECK(corrupt.exec());
    }

    // A retained legacy copy must be removed with its account, not with the
    // active one or the shared install secrets.
    QDir().mkpath(QFileInfo(legacyCopyPath(broken)).absolutePath());
    CHECK(writeFileBytes(legacyCopyPath(broken), QByteArray("legacy-ciphertext")));

    err.clear();
    CHECK(!engine.switchAccount(broken, &err));
    CHECK(!err.isEmpty());
    CHECK(engine.lastSwitchFailure() == Engine::SwitchFailure::Identity);
    CHECK(engine.settings()->activeAccountId() == first);
    CHECK(engine.account()->activeRowId() == first);
    CHECK(engine.account()->profile().fingerprint == firstFp);
    CHECK(engine.networkRunning());
    QByteArray rollbackSig;
    CHECK(engine.account()->sign(QByteArrayLiteral("rollback-probe"), &rollbackSig));
    CHECK(rollbackSig.size() == 64);

    // Unknown id: reject before touching anything.
    const QByteArray publicBefore = engine.account()->publicKey();
    err.clear();
    CHECK(engine.destroyAccountById(999999, false, &err) == -1);
    CHECK(!err.isEmpty());
    CHECK(engine.account()->activeRowId() == first);
    CHECK(engine.account()->profile().fingerprint == firstFp);
    CHECK(engine.account()->publicKey() == publicBefore);
    CHECK(engine.networkRunning());
    CHECK(engine.storage()->isOpen());
    CHECK(engine.account()->listAccounts().size() == 2);

    // Destroy the broken non-active account: the active identity, network,
    // open storage and shared secrets must survive.
    err.clear();
    CHECK(engine.destroyAccountById(broken, false, &err) == 1);
    CHECK(engine.account()->listAccounts().size() == 1);
    CHECK(!QFile::exists(legacyCopyPath(broken)));
    CHECK(engine.account()->activeRowId() == first);
    CHECK(engine.account()->profile().fingerprint == firstFp);
    CHECK(engine.settings()->activeAccountId() == first);
    CHECK(engine.networkRunning());
    CHECK(engine.storage()->isOpen());
    CHECK(QFile::exists(Paths::keystoreSalt()));
    CHECK(QFile::exists(Paths::keystoreDeviceKey()));
    CHECK(QFile::exists(Paths::keystoreDeviceKey() + QStringLiteral(".tag")));
    CHECK(QDir(QFileInfo(Paths::keystoreDeviceKey()).absolutePath() + QStringLiteral("/legacy")).exists());
    CHECK(engine.account()->loadAccount(first, &err)); // active still readable
    CHECK(engine.account()->profile().fingerprint == firstFp);

    // Destroying the active (and last) account keeps the historical return
    // value and performs the checked zero-count cleanup.
    err.clear();
    CHECK(engine.destroyAccountById(first, false, &err) == 0);
    CHECK(!engine.networkRunning());
    CHECK(!engine.storage()->isOpen());
    CHECK(engine.account()->activeRowId() == 0);
    CHECK(!QFile::exists(Paths::keystoreSalt()));
    CHECK(!QFile::exists(Paths::keystoreDeviceKey()));
    CHECK(!QFile::exists(Paths::keystoreDeviceKey() + QStringLiteral(".tag")));
    CHECK(!QDir(QFileInfo(Paths::keystoreDeviceKey()).absolutePath() + QStringLiteral("/legacy")).exists());

    // The broken target's fingerprint is intentionally not used after its row
    // is gone; keep the assertion explicit that the two identities differed.
    CHECK(brokenFp != firstFp);
    return 0;
}

static int testAccountCountFailure() {
    QTemporaryDir home;
    CHECK(home.isValid());
    qputenv("MESHLINK_DATA_HOME", home.path().toUtf8());
    qputenv("MESHLINK_CONFIG_HOME", home.path().toUtf8());
    qunsetenv("MESHLINK_LEGACY_MACHINE_FACTOR");
    qunsetenv("MESHLINK_TEST_FAIL_ACCOUNT_COUNT");

    Storage storage;
    QString err;
    CHECK(storage.open(home.filePath(QStringLiteral("countfail.db")), &err));
    AccountManager accounts(&storage);

    const qint64 first = accounts.createNewAccount(QStringLiteral("CountKeep"), &err);
    CHECK(first > 0);
    const QString firstFp = accounts.profile().fingerprint;
    const qint64 second = accounts.createNewAccount(QStringLiteral("CountDestroy"), &err);
    CHECK(second > 0 && second != first);

    const QString saltPath = Paths::keystoreSalt();
    const QString keyPath = Paths::keystoreDeviceKey();
    const QString tagPath = keyPath + QStringLiteral(".tag");
    CHECK(QFile::exists(saltPath) && QFile::exists(keyPath) && QFile::exists(tagPath));

    // Fault injection: the checked COUNT(*) fails after the wipe. The destroy
    // must fail loudly and preserve the secrets shared with the other account.
    qputenv("MESHLINK_TEST_FAIL_ACCOUNT_COUNT", QByteArray("1"));
    const int failed = accounts.destroyAccount(false, &err);
    CHECK(failed < 0);
    CHECK(!err.isEmpty());
    CHECK(QFile::exists(saltPath));
    CHECK(QFile::exists(keyPath));
    CHECK(QFile::exists(tagPath));

    qunsetenv("MESHLINK_TEST_FAIL_ACCOUNT_COUNT");
    accounts.resetLoaded();
    CHECK(accounts.loadAccount(first, &err)); // remaining account still readable
    CHECK(accounts.profile().fingerprint == firstFp);

    CHECK(accounts.destroyAccount(false, &err) == 0); // checked count succeeds
    CHECK(!QFile::exists(saltPath));                  // last account: wiped
    CHECK(!QFile::exists(keyPath));
    CHECK(!QFile::exists(tagPath));
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
        {"keystore", testKeystoreDeviceBinding},
        {"destroykeeps", testDestroyKeepsOtherAccounts},
        {"provenance", testDeviceKeyProvenance},
        {"retention", testLegacyRetention},
        {"countfail", testAccountCountFailure},
        {"engine", testEngineAccountLifecycle},
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
