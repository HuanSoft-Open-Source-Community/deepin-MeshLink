// deepin-MeshLink — multi-process integration scenario (UC-01..UC-13 end to end).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
//
// Single binary, two modes:
//   parent (default): creates temp data dirs, spawns itself N times with
//     --node <name> + MESHLINK_* env overrides, drives scenarios by writing
//     line commands to each node's stdin and parsing "EV <type> <json>" lines.
//   node (--node <name>): plain QCoreApplication running Engine + command loop.
// All four nodes share one UDP discovery port on 127.0.0.1 via
// SO_REUSE and unicast bootstrap (UC-04 E2 path), with distinct TCP ports.
#include "core/account.h"
#include "core/chatmanager.h"
#include "core/crypto.h"
#include "core/downloader.h"
#include "core/engine.h"
#include "core/filemanager.h"
#include "core/lansegment.h"
#include "core/roommanager.h"
#include "core/settings.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRandomGenerator>
#include <QSet>
#include <QSocketNotifier>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <unistd.h>
#include <functional>
#include <memory>
#include <map>

using namespace meshlink;

// ---------------------------------------------------------------------------
// helpers shared by both modes
// ---------------------------------------------------------------------------
static QString sha256OfFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash h(QCryptographicHash::Sha256);
    while (!f.atEnd())
        h.addData(f.read(1 << 20));
    return QString::fromLatin1(h.result().toHex());
}

static QByteArray makeRandomFile(const QString &path, qint64 bytes) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return {};
    auto rng = QRandomGenerator::system();
    QByteArray buf;
    buf.resize(1 << 16);
    qint64 left = bytes;
    while (left > 0) {
        for (int i = 0; i < buf.size(); ++i)
            buf[i] = char(rng->bounded(256));
        const qint64 n = qMin(left, qint64(buf.size()));
        f.write(buf.constData(), n);
        left -= n;
    }
    f.close();
    return sha256OfFile(path).toLatin1();
}

static void makePng(const QString &path, int size) {
    QImage img(size, size, QImage::Format_ARGB32);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            img.setPixelColor(x, y, QColor((x * 7) % 256, (y * 13) % 256, (x + y) % 256));
    img.save(path, "PNG");
}

// ===========================================================================
// NODE MODE
// ===========================================================================
static void ev(const QString &type, const QJsonObject &o = {}) {
    QTextStream out(stdout);
    out << "EV " << type << " " << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << "\n";
    out.flush();
    std::fflush(stdout); // Qt does not fflush C stdio: piped events would batch in 8 KiB blocks
}

struct NodeCtl {
    Settings settings;
    Engine engine{&settings};
    QSet<QString> ownMsgIds;
    QSet<QString> watchedHashes; // image blobs we expect
};

static void wireEvents(NodeCtl *n) {
    auto *e = &n->engine;
    QObject::connect(e->discovery(), &Discovery::peerSeen, e, [n](const PeerInfo &p) {
        if (p.fingerprint == n->engine.account()->profile().fingerprint)
            return;
        ev("peerseen", {{"fp", p.fingerprint.left(32)}, {"nick", p.nickname}, {"tcp", int(p.tcpPort)}});
    });
    QObject::connect(e->discovery(), &Discovery::peerGone, e, [e](const QString &fp) {
        if (fp == e->account()->profile().fingerprint)
            return;
        ev("peerlost", {{"fp", fp.left(32)}});
    });
    QObject::connect(e->rooms(), &RoomManager::joinResult, e, [](const QString &roomId, JoinStatus st, const QString &detail) {
        ev("join", {{"room", roomId}, {"status", int(st)}, {"detail", detail}});
    });
    QObject::connect(e->rooms(), &RoomManager::joinRequested, e, [](const QString &roomId, const PendingJoin &p) {
        ev("joinreq", {{"room", roomId}, {"row", double(p.rowId)}, {"nick", p.requester.nickname}, {"fp", p.requester.fingerprint.left(32)}});
    });
    QObject::connect(e->rooms(), &RoomManager::kickedFromRoom, e, [](const QString &roomId, const QString &why) {
        ev("kicked", {{"room", roomId}, {"why", why}});
    });
    QObject::connect(e->rooms(), &RoomManager::roomClosedByHost, e, [](const QString &roomId) {
        ev("closed", {{"room", roomId}});
    });
    QObject::connect(e->rooms(), &RoomManager::adminCompleted, e, [](const QString &, const QString &, quint8, bool ok, const QString &err) {
        ev("opres", {{"ok", ok}, {"err", err}});
    });
    QObject::connect(e->chat(), &ChatManager::messageReceived, e, [n](const QString &roomId, const Msg &m) {
        const bool mine = n->ownMsgIds.contains(m.msgId);
        ev("msg", {{"room", roomId}, {"id", m.msgId}, {"seq", double(m.seq)}, {"kind", int(m.kind)},
                   {"sender", m.senderFp.left(8)}, {"nick", m.senderNick}, {"text", m.text.left(80)},
                   {"hash", m.contentHash}, {"mine", mine}, {"replyTo", m.replyTo},
                   {"recalled", m.recalled}});
    });
    QObject::connect(e->chat(), &ChatManager::messageRecalled, e,
                     [](const QString &roomId, const QString &msgId, const QString &byFp) {
                         ev("recalled", {{"room", roomId}, {"id", msgId}, {"by", byFp.left(8)}});
                     });
    QObject::connect(e->chat(), &ChatManager::recallFailed, e,
                     [](const QString &roomId, const QString &msgId, const QString &why) {
                         ev("recallfail", {{"room", roomId}, {"id", msgId}, {"why", why}});
                     });
    QObject::connect(e->chat(), &ChatManager::messageStatusChanged, e, [n](const QString &, const QString &msgId, quint8 st) {
        // always forward: synchronous host-echo status fires before sendText returns
        Q_UNUSED(n);
        ev("status", {{"id", msgId}, {"st", int(st)}});
    });
    QObject::connect(e->chat(), &ChatManager::blobReady, e, [n](const QString &hash) {
        if (n->watchedHashes.contains(hash))
            ev("blob", {{"hash", hash}});
    });
    QObject::connect(e->files(), &FileManager::announced, e, [](const QString &roomId, const AnnouncedFile &f) {
        ev("annrecv", {{"room", roomId}, {"hash", f.fileHash}, {"name", f.fileName}, {"size", double(f.fileSize)}});
    });
    QObject::connect(e->downloader(), &Downloader::taskChanged, e, [e](const QString &hash) {
        const auto t = e->downloader()->task(hash);
        ev("tchg", {{"hash", hash.left(8)}, {"status", int(t.status)}, {"done", double(t.doneChunks)}, {"err", t.error}});
    });
    QObject::connect(e->downloader(), &Downloader::taskCompleted, e, [](const QString &hash, const QString &dest) {
        ev("dl-done", {{"hash", hash}, {"dest", dest}});
    });
    QObject::connect(e->downloader(), &Downloader::taskFailed, e, [](const QString &hash, const QString &why) {
        ev("dl-fail", {{"hash", hash}, {"why", why}});
    });
    QObject::connect(e, &Engine::spoofingSuspected, e, [](const QString &fp) { ev("spoof", {{"fp", fp}}); });
}

static QJsonObject memberJson(const RoomMemberInfo &m) {
    return {{"fp", m.fp.left(32)}, {"nick", m.nickname}, {"role", int(m.role)}, {"muted", m.mutedUntil != 0}};
}

static void handleLine(NodeCtl *n, const QString &line) {
    const QStringList a = line.split(' ');
    if (a.isEmpty())
        return;
    const QString cmd = a.value(0);
    auto *e = &n->engine;
    const QString self = e->account()->profile().fingerprint;

    if (cmd == "ready") {
        ev("ready", {{"fp", self}, {"tcp", int(e->settings()->tcpPort())}});
    } else if (cmd == "create") { // create <name> <open|approval> [cap]
        const JoinPolicy p = a.value(2) == "approval" ? JoinPolicy::Approval : JoinPolicy::Open;
        QString err;
        const QString id = e->rooms()->createRoom(a.value(1), QStringLiteral("测试简介"), p, a.value(3).toInt(), &err);
        if (id.isEmpty()) {
            ev("err", {{"why", err}});
        } else {
            const RoomLocal created = e->rooms()->room(id);
            ev("room", {{"id", id}, {"code", created.inviteCode}, {"name", a.value(1)}, {"policy", int(p)}});
        }
    } else if (cmd == "rooms") {
        QJsonArray arr;
        for (const auto &p : e->discovery()->peers())
            for (const auto &r : p.hostedRooms)
                arr.append(QJsonObject{{"id", r.roomId}, {"code", r.inviteCode}, {"name", r.name},
                                       {"policy", int(r.policy)}, {"host", r.hostFp.left(8)}});
        for (const auto &r : e->rooms()->hostedSummaries())
            arr.append(QJsonObject{{"id", r.roomId}, {"code", r.inviteCode}, {"name", r.name},
                                   {"policy", int(r.policy)}, {"host", self.left(8)}, {"mine", true}});
        ev("rooms", {{"list", arr}});
    } else if (cmd == "join") { // join <roomid|invitecode>
        const QString key = a.value(1);
        std::optional<RoomSummary> found = RoomManager::isValidInviteCode(key)
                                               ? e->discovery()->roomByInviteCode(key)
                                               : e->discovery()->roomById(key);
        if (!found) {
            fprintf(stderr, "[node-cmd] join %s NOT-IN-LIST yet\n", qUtf8Printable(key.left(8)));
            ev("joincb", {{"room", key}, {"status", int(JoinStatus::RoomGone)}, {"detail", "not-in-list"}});
            return;
        }
        const RoomSummary s = *found;
        fprintf(stderr, "[node-cmd] join %s host=%s\n", qUtf8Printable(s.roomId.left(8)),
                qUtf8Printable(s.hostFp.left(8)));
        e->rooms()->joinRoom(s, [](JoinStatus st, const QString &detail) {
            fprintf(stderr, "[node-cmd] joincb st=%d why=%s\n", int(st), qUtf8Printable(detail));
            ev("joincb", {{"status", int(st)}, {"detail", detail}});
        });
    } else if (cmd == "segment") {
        const LanSegment seg = e->lanSegment();
        ev("segment", {{"ip", seg.ip},
                       {"prefix", seg.prefixLength},
                       {"broadcast", seg.broadcastAddress().toString()},
                       {"label", seg.label()}});
    } else if (cmd == "switchseg") { // switchseg <ip> <prefix>
        LanSegment seg;
        seg.interfaceName = QStringLiteral("test");
        seg.ip = a.value(1);
        seg.prefixLength = a.value(2).toInt();
        QString err;
        const bool ok = e->setLanSegment(seg, &err);
        ev("segswitched", {{"ok", ok}, {"err", err}, {"label", e->lanSegmentLabel()}});
    } else if (cmd == "peers") {
        QJsonArray fps;
        for (const auto &p : e->discovery()->peers())
            fps.append(p.fingerprint.left(8));
        ev("peers", {{"count", int(fps.size())}, {"fps", fps}});
    } else if (cmd == "members") {
        QJsonArray arr;
        for (const auto &m : e->rooms()->members(a.value(1)))
            arr.append(memberJson(m));
        ev("members", {{"room", a.value(1)}, {"list", arr}});
    } else if (cmd == "send") { // send <room> <text...>
        QString err;
        const QString text = line.section(' ', 2);
        const QString id = e->chat()->sendText(a.value(1), text, &err);
        if (id.isEmpty())
            ev("senderr", {{"why", err}});
        else
            n->ownMsgIds.insert(id);
    } else if (cmd == "sendq") { // sendq <room> <replyTo> <text...>
        QString err;
        const QString text = line.section(' ', 3);
        const QString id = e->chat()->sendText(a.value(1), text, &err, a.value(2));
        if (id.isEmpty())
            ev("senderr", {{"why", err}});
        else
            n->ownMsgIds.insert(id);
    } else if (cmd == "recall") { // recall <room> <msgId>
        QString err;
        const bool accepted = e->chat()->recallMessage(a.value(1), a.value(2), &err);
        ev("recallreq", {{"room", a.value(1)}, {"id", a.value(2)}, {"ok", accepted}, {"why", err}});
    } else if (cmd == "recallraw") { // host-side validation path, bypass local pre-check
        const RoomLocal r = e->rooms()->room(a.value(1));
        ChatRecallMsg req;
        req.roomId = a.value(1);
        req.msgId = a.value(2);
        const bool sent = !r.hostFp.isEmpty() &&
                          e->sendToPeer(r.hostFp, MsgType::ChatRecall, req.toPayload());
        ev("recallraw", {{"room", a.value(1)}, {"id", a.value(2)}, {"sent", sent}});
    } else if (cmd == "sendimage") { // sendimage <room> <path>
        QString err;
        const QString id = e->chat()->sendImage(a.value(1), a.value(2), &err);
        if (id.isEmpty())
            ev("senderr", {{"why", err}});
        else
            n->ownMsgIds.insert(id);
    } else if (cmd == "watchblob") {
        n->watchedHashes.insert(a.value(1));
    } else if (cmd == "annfile") { // annfile <room> <path>
        const QString path = a.value(2);
        e->files()->announceFile(a.value(1), path, [](const QString &msgId, const QString &err) {
            ev("ann", {{"id", msgId}, {"err", err}});
        });
    } else if (cmd == "msgs") {
        QJsonArray arr;
        for (const auto &m : e->chat()->history(a.value(1), 500))
            arr.append(QJsonObject{{"seq", double(m.seq)}, {"kind", int(m.kind)},
                                   {"sender", m.senderFp.left(8)}, {"text", m.text.left(40)},
                                   {"hash", m.contentHash}, {"id", m.msgId},
                                   {"replyTo", m.replyTo}, {"recalled", m.recalled}});
        ev("msgs", {{"list", arr}});
    } else if (cmd == "op") { // op <room> <targetfp> <opname> [ms]
        static const std::map<QString, AdminOp> ops = {
            {"mute", AdminOp::Mute},       {"unmute", AdminOp::Unmute},   {"kick", AdminOp::Kick},
            {"black", AdminOp::Blacklist}, {"unblack", AdminOp::Unblacklist}, {"setadmin", AdminOp::SetAdmin},
            {"unsetadmin", AdminOp::UnsetAdmin}, {"gmute", AdminOp::GlobalMuteOn}, {"gumute", AdminOp::GlobalMuteOff}};
        const AdminOp op = ops.at(a.value(3));
        // targets are matched by fingerprint prefix for test convenience
        QString target = a.value(2);
        for (const auto &m : e->rooms()->members(a.value(1)))
            if (m.fp.startsWith(target))
                target = m.fp;
        fprintf(stderr, "[node-cmd] op room=%s target=%s->%s opname=%s\n", qUtf8Printable(a.value(1)),
                qUtf8Printable(a.value(2).left(8)), qUtf8Printable(target.left(8)), qUtf8Printable(a.value(3)));
        e->rooms()->requestAdminOp(a.value(1), target, op, a.value(4).toLongLong(), [](bool ok, const QString &err) {
            fprintf(stderr, "[node-cmd] opres ok=%d err=%s\n", int(ok), qUtf8Printable(err));
            ev("opres", {{"ok", ok}, {"err", err}});
        });
    } else if (cmd == "approvals") {
        QJsonArray arr;
        for (const auto &p : e->rooms()->pendingApprovals(a.value(1)))
            arr.append(QJsonObject{{"row", double(p.rowId)}, {"nick", p.requester.nickname}, {"fp", p.requester.fingerprint.left(32)}});
        ev("approvals", {{"list", arr}});
    } else if (cmd == "approve") {
        e->rooms()->respondApproval(a.value(1).toLongLong(), a.value(2) == "1");
        ev("approvedone", {{"row", a.value(1)}});
    } else if (cmd == "leave") {
        e->rooms()->leaveRoom(a.value(1), [](bool ok, const QString &why) { ev("left", {{"ok", ok}, {"why", why}}); });
    } else if (cmd == "suppliers") { // suppliers <room> <hash>
        e->files()->querySuppliers(a.value(1), a.value(2), [](const QVector<SupplierInfo> &sup) {
            QJsonArray arr;
            for (const auto &s : sup)
                arr.append(QJsonObject{{"fp", s.fp.left(32)}, {"nick", s.nickname}, {"ip", s.ip}, {"port", int(s.port)}, {"online", s.online}});
            ev("sups", {{"hash", QString()}, {"list", arr}});
        });
    } else if (cmd == "download") { // download <room> <hash>
        const QString room = a.value(1), hash = a.value(2);
        QString name, mime;
        qint64 size = 0;
        for (const auto &f : e->files()->announcements(room)) {
            if (f.fileHash == hash) {
                name = f.fileName;
                mime = f.mime;
                size = f.fileSize;
            }
        }
        fprintf(stderr, "[node-cmd] download room=%s hash=%s meta=%lld name=%s\n", qUtf8Printable(room), qUtf8Printable(hash.left(8)),
                (long long)size, qUtf8Printable(name));
        e->files()->querySuppliers(room, hash, [room, hash, name, size, mime, e](const QVector<SupplierInfo> &sup) {
            fprintf(stderr, "[node-cmd] suppliers=%d\n", int(sup.size()));
            if (sup.isEmpty()) {
                ev("dl-fail", {{"hash", hash}, {"why", QStringLiteral("no suppliers")}});
                return;
            }
            e->downloader()->startTask(room, hash, name, size, mime, sup);
            fprintf(stderr, "[node-cmd] startTask returned\n");
            ev("dl-start", {{"hash", hash}, {"suppliers", int(sup.size())}});
        });
    } else if (cmd == "tstat") { // tstat <hash>
        const auto t = e->downloader()->task(a.value(1));
        ev("tstat", {{"hash", a.value(1)}, {"status", int(t.status)}, {"done", double(t.doneChunks)},
                     {"total", double(t.totalChunks)}, {"err", t.error}, {"size", double(t.size)}});
    } else if (cmd == "pause") {
        e->downloader()->pause(a.value(1));
    } else if (cmd == "resume") {
        e->downloader()->resume(a.value(1), {});
    } else if (cmd == "cancel") {
        e->downloader()->cancel(a.value(1), true);
    } else if (cmd == "hasfile") { // hasfile <hash> -> EV hasfile {path}
        QSqlQuery q = e->storage()->query("SELECT dest_path FROM local_files WHERE file_hash=?");
        q.bindValue(0, a.value(1));
        ev("hasfile", {{"hash", a.value(1)}, {"path", q.next() ? q.value(0).toString() : QString()}});
    } else if (cmd == "destroy") { // destroy [keep|wipe]
        QString err;
        // Engine::destroyAccount returns the remaining account count:
        // >= 0 success (0 = last account destroyed), -1 failure.
        const int remaining = e->destroyAccount(a.value(1) != "keep", &err);
        ev("destroyed", {{"ok", remaining >= 0}, {"left", double(remaining)}, {"err", err}});
    } else if (cmd == "nick") { // nick <newname>
        QString err;
        ev("nickres", {{"ok", e->account()->updateProfile(a.value(1), {}, &err)}, {"err", err}});
    } else if (cmd == "badnick") { // validation must reject
        QString err;
        ev("nickres", {{"ok", e->account()->updateProfile(QStringLiteral("短"), {}, &err)}, {"err", err}});
    } else if (cmd == "quit") {
        QCoreApplication::quit();
    }
}

static int nodeMain(QCoreApplication &app, const QString &nodeName) {
    auto *n = new NodeCtl();
    QString err;
    if (!n->engine.start(&err)) {
        ev("startfail", {{"why", err}});
        return 3;
    }
    wireEvents(n);
    // raw fd reader: QTextStream::atEnd() on a pipe blocks and freezes the
    // main loop; read() after activation never stalls and detects EOF so an
    // orphaned node exits instead of squatting ports.
    auto *notifier = new QSocketNotifier(0, QSocketNotifier::Read, &app);
    auto buf = std::make_shared<QByteArray>();
    QObject::connect(notifier, &QSocketNotifier::activated, &app, [n, notifier, buf] {
        char raw[4096];
        const ssize_t got = ::read(0, raw, sizeof raw);
        if (got <= 0) { // EOF or error: parent is gone -> stop being an orphan
            QCoreApplication::quit();
            return;
        }
        buf->append(raw, int(got));
        while (buf->contains('\n')) {
            const int nl = buf->indexOf('\n');
            const QString line = QString::fromUtf8(buf->left(nl)).trimmed();
            buf->remove(0, nl + 1);
            if (!line.isEmpty())
                handleLine(n, line);
        }
    });
    fprintf(stderr, "[node %s up]\n", qUtf8Printable(nodeName));
    fflush(stderr);
    // announce once the event loop runs: before exec() timers and socket IO
    // are dead, so heartbeats (and thus peerseen) would never fire
    QTimer::singleShot(0, &app, [n] {
        ev("ready", {{"fp", n->engine.account()->profile().fingerprint},
                     {"tcp", int(n->engine.settings()->tcpPort())}});
    });
    return app.exec();
}

// ===========================================================================
// PARENT MODE
// ===========================================================================
struct Node {
    QString name;
    QProcess *proc = nullptr;
    QStringList lines;
    QByteArray rest;
    QString fp; // full fingerprint once known

    void start(const QString &self, const QStringList &extraEnv) {
        if (!registry().contains(this))
            registry().append(this);
        proc = new QProcess;
        QProcessEnvironment envm = QProcessEnvironment::systemEnvironment();
        for (const auto &kv : extraEnv)
            envm.insert(kv.section('=', 0, 0), kv.section('=', 1));
        proc->setProcessEnvironment(envm);
        proc->setReadChannel(QProcess::StandardOutput);
        proc->setProcessChannelMode(QProcess::ForwardedErrorChannel); // child diagnostics to our stderr
        // Sandbox runs spawn through the host loader directly (spawning
        // /bin/bash with a host-glibc LD_LIBRARY_PATH trips stack smashing).
        // MESHLINK_LAUNCH_LOADER = "<ld.so path> --library-path <paths>".
        const QByteArray loader = qgetenv("MESHLINK_LAUNCH_LOADER");
        QString prog = self;
        QStringList a{QStringLiteral("--node"), name};
        if (!loader.isEmpty()) {
            const QList<QByteArray> parts = loader.simplified().split(' ');
            prog = QString::fromLocal8Bit(parts.value(0));
            QStringList prefix;
            for (int i = 1; i < parts.size(); ++i)
                prefix << QString::fromLocal8Bit(parts[i]);
            prefix << self;
            prefix << a; // "--node", name
            a = prefix;
        }
        proc->start(prog, a);
        if (!proc->waitForStarted(10000))
            fail(QStringLiteral("node %1 failed to start").arg(name));
    }
    void fail(const QString &why) {
        fprintf(stderr, "INTEGRATION FAIL (%s): %s\n", qUtf8Printable(name), qUtf8Printable(why));
        fflush(stderr);
        for (Node *n : registry()) // never leave orphans squatting the ports
            if (n->proc && n->proc->state() == QProcess::Running) {
                n->proc->kill();
                n->proc->waitForFinished(3000);
            }
        exit(1);
    }
    static QVector<Node *> &registry() {
        static QVector<Node *> v;
        return v;
    }
    void cmd(const QString &c) {
        if (!proc || proc->state() != QProcess::Running)
            fail(QStringLiteral("node not running: %1").arg(name));
        proc->write(c.toUtf8() + "\n");
        proc->waitForBytesWritten(2000);
    }
    // pump stdout; returns the next unparsed event line matching type (and optional field check)
    QJsonObject waitEvent(const QString &type, std::function<bool(const QJsonObject &)> pred, int timeoutMs) {
        QElapsedTimer et;
        et.start();
        while (et.elapsed() < timeoutMs) {
            for (int i = 0; i < lines.size(); ++i) {
                const QString &l = lines[i];
                if (!l.startsWith(QStringLiteral("EV ")))
                    continue;
                const QString t = l.section(' ', 1, 1);
                if (t != type)
                    continue;
                const QJsonDocument doc = QJsonDocument::fromJson(l.section(' ', 2).toUtf8());
                const QJsonObject o = doc.object();
                if (!pred || pred(o)) {
                    lines.removeAt(i);
                    return o;
                }
            }
            if (!proc->waitForReadyRead(100)) {
                if (proc->state() != QProcess::Running)
                    fail(QStringLiteral("node %1 died while waiting %2").arg(name, type));
                continue;
            }
            rest.append(proc->readAll());
            while (rest.contains('\n')) {
                const int nl = rest.indexOf('\n');
                lines.append(QString::fromUtf8(rest.left(nl)));
                rest.remove(0, nl + 1);
            }
            // (stderr is forwarded, nothing to drain here)
        }
        fail(QStringLiteral("timeout waiting for %1 on node %2 (pending: %3)").arg(type, name, lines.join(" | ").left(4000)));
        return {};
    }
    void killNow() {
        proc->kill();
        proc->waitForFinished(5000);
    }
};

static int parentMain(QCoreApplication &app) {
    Q_UNUSED(app);
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        fprintf(stderr, "no temp dir\n");
        return 1;
    }
    const QString root = tmp.path();
    // when this parent was exec'd through ld.so --library-path (sandbox host
    // loader), applicationFilePath()/proc/self/exe point at the loader; the
    // real binary path is argv[0] as set up by the loader.
    QString selfPath = QCoreApplication::applicationFilePath();
    if (selfPath.contains(QStringLiteral("ld-linux")))
        selfPath = QCoreApplication::arguments().value(0);
    if (!QFileInfo(selfPath).isAbsolute())
        selfPath = QDir::current().absoluteFilePath(selfPath);

    // shared LAN bootstrap UDP port for all nodes
    // Each node owns a distinct UDP discovery port and unicasts heartbeats to
    // every peer (the sandbox cannot rely on a shared broadcast port because
    // SO_REUSEADDR delivers unicast to a single socket). The selected LAN
    // segment is still the loopback subnet, so source-address filtering is
    // exercised end to end and S10 can flip it at runtime.
    const QStringList udpPorts = {QStringLiteral("24651"), QStringLiteral("24652"), QStringLiteral("24653"),
                                  QStringLiteral("24654")};
    QStringList bootstrapParts;
    for (const auto &p : udpPorts)
        bootstrapParts << QStringLiteral("127.0.0.1:%1").arg(p);
    const QString bootstrap = bootstrapParts.join(',');

    // sample payloads
    const QString file1 = root + "/file1.bin";
    const QString file2 = root + "/file2.bin";
    const QString file3 = root + "/file3.bin";
    const QString png = root + "/photo.png";
    const QByteArray h1 = makeRandomFile(file1, 3 * 1024 * 1024);
    const QByteArray h2 = makeRandomFile(file2, 7 * 1024 * 1024);
    const QByteArray h3 = makeRandomFile(file3, 12 * 1024 * 1024);
    makePng(png, 240);
    if (h1.isEmpty() || h2.isEmpty() || h3.isEmpty()) {
        fprintf(stderr, "sample files failed\n");
        return 1;
    }
    const QString h1s = QString::fromLatin1(h1), h2s = QString::fromLatin1(h2), h3s = QString::fromLatin1(h3);

    auto nodeEnv = [&](const QString &name, const QString &tcpPort, const QString &udpPort, bool evil) {
        QStringList e;
        e << QStringLiteral("MESHLINK_NODE=%1").arg(name)
          << QStringLiteral("MESHLINK_DATA_HOME=%1/%2").arg(root, name)
          << QStringLiteral("MESHLINK_CONFIG_HOME=%1/%2-cfg").arg(root, name)
          << QStringLiteral("MESHLINK_IP=127.0.0.1")
          << QStringLiteral("MESHLINK_DISCOVERY_PORT=%1").arg(udpPort)
          << QStringLiteral("MESHLINK_TCP_PORT=%1").arg(tcpPort)
          << QStringLiteral("MESHLINK_BOOTSTRAP=%1").arg(bootstrap)
          << QStringLiteral("MESHLINK_HEARTBEAT_MS=400")
          << QStringLiteral("MESHLINK_OFFLINE_PERIODS=4")
          << QStringLiteral("MESHLINK_CHUNK_KB=64")
          << QStringLiteral("MESHLINK_THREADS=3")
          << QStringLiteral("MESHLINK_CHUNK_DELAY_MS=50") // deterministic mid-transfer states
          << QStringLiteral("MESHLINK_RECALL_WINDOW_MS=1500"); // fast recall-expiry tests
        if (evil)
            e << QStringLiteral("MESHLINK_EVIL_CHUNKS=1");
        return e;
    };

    Node alice{"alice", {}, {}, {}, {}}, bob{"bob", {}, {}, {}, {}}, carol{"carol", {}, {}, {}, {}},
         mallory{"mallory", {}, {}, {}, {}};
    alice.start(selfPath, nodeEnv("alice", "24701", udpPorts[0], false));
    bob.start(selfPath, nodeEnv("bob", "24702", udpPorts[1], false));
    carol.start(selfPath, nodeEnv("carol", "24703", udpPorts[2], false));
    mallory.start(selfPath, nodeEnv("mallory", "24704", udpPorts[3], true));

    auto readyOf = [](Node *n) {
        const auto o = n->waitEvent("ready", {}, 15000);
        n->fp = o["fp"].toString();
        if (n->fp.size() != 32)
            n->fail("bad fingerprint length in ready");
    };
    readyOf(&alice);
    readyOf(&bob);
    readyOf(&carol);
    readyOf(&mallory);
    if (alice.fp == bob.fp)
        alice.fail("two nodes derived identical identities");

    // ---- S1 discovery mutual visibility (UC-03/04) ----
    alice.cmd("nick 爱丽丝");
    alice.waitEvent("nickres", [](const QJsonObject &o) { return o["ok"].toBool(); }, 5000);
    bob.cmd("nick 鲍勃");
    bob.waitEvent("nickres", [](const QJsonObject &o) { return o["ok"].toBool(); }, 5000);
    bob.cmd("badnick 短"); // UC-03 validation rule
    bob.waitEvent("nickres", [](const QJsonObject &o) { return !o["ok"].toBool() && o["err"].toString().contains(QStringLiteral("2–32")); }, 5000);

    for (Node *n : {&alice, &bob, &carol, &mallory}) {
        for (int k = 0; k < 3; ++k) // see three peers
            n->waitEvent("peerseen", {}, 20000);
        n->waitEvent("peerseen", {}, 20000);
    }
    printf("[S1 discovery OK]\n");

    // ---- S2 rooms + join + text chat (UC-05/06/08) ----
    alice.cmd("create 测试房间 open");
    const auto roomEvent = alice.waitEvent("room", {}, 5000);
    const QString roomA = roomEvent["id"].toString();
    const QString codeA = roomEvent["code"].toString();
    if (roomA.size() != kRoomIdHexLen)
        alice.fail("roomId is not a 32-hex room fingerprint");
    if (codeA.size() != kInviteCodeDigits)
        alice.fail("room invite code is not 6 digits");
    for (const QChar c : codeA)
        if (!c.isDigit())
            alice.fail("room invite code contains non-digit characters");
    // it must show up in everyone's heartbeat room lists
    for (Node *n : {&bob, &carol, &mallory}) {
        QDeadlineTimer until(20000);
        bool seen = false;
        while (!until.hasExpired() && !seen) {
            n->cmd("rooms");
            const auto o = n->waitEvent("rooms", {}, 10000);
            for (const auto &v : o["list"].toArray()) {
                const QJsonObject ro = v.toObject();
                if (ro["id"].toString() == roomA) {
                    seen = true;
                    if (ro["code"].toString() != codeA)
                        n->fail("heartbeat room summary lost the invite code");
                }
            }
            if (!seen)
                QThread::msleep(200);
        }
        if (!seen)
            n->fail("hosted room not discovered via heartbeat");
    }
    bob.cmd(QStringLiteral("join %1").arg(roomA));
    bob.waitEvent("joincb", [](const QJsonObject &o) { return o["status"].toInt() == int(JoinStatus::Accepted); }, 10000);
    // carol joins through the six-digit invite code path (LAN-local lookup).
    carol.cmd(QStringLiteral("join %1").arg(codeA));
    carol.waitEvent("joincb", [](const QJsonObject &o) { return o["status"].toInt() == int(JoinStatus::Accepted); }, 10000);
    {
        QDeadlineTimer until(15000);
        int cnt = 0;
        while (cnt < 3 && !until.hasExpired()) {
            alice.cmd(QStringLiteral("members %1").arg(roomA));
            cnt = alice.waitEvent("members", {}, 8000)["list"].toArray().size();
            QThread::msleep(150);
        }
        if (cnt < 3)
            alice.fail("room does not have 3 members");
    }
    alice.cmd(QStringLiteral("send %1 你好，织讯世界").arg(roomA));
    bob.waitEvent("msg", [&](const QJsonObject &o) { return o["text"].toString() == QStringLiteral("你好，织讯世界") && o["sender"].toString() == alice.fp.left(8); }, 10000);
    carol.waitEvent("msg", [&](const QJsonObject &o) { return o["text"].toString() == QStringLiteral("你好，织讯世界"); }, 10000);
    alice.waitEvent("status", [](const QJsonObject &o) { return o["st"].toInt() == int(MsgStatus::Delivered); }, 10000);
    // host assigns strictly increasing seq (BR-16)
    bob.cmd(QStringLiteral("send %1 第二条消息，来自bob").arg(roomA));
    bob.waitEvent("msg", [&](const QJsonObject &o) { return o["text"].toString().startsWith(QStringLiteral("第二条")); }, 10000);
    {
        bob.cmd(QStringLiteral("msgs %1").arg(roomA));
        const auto o = bob.waitEvent("msgs", {}, 8000)["list"].toArray();
        quint64 prev = 0;
        for (const auto &v : o) {
            const quint64 s = quint64(v.toObject()["seq"].toDouble());
            if (s <= prev && s != 0)
                bob.fail("seq not monotonic in history");
            if (s > prev)
                prev = s;
        }
    }
    printf("[S2 chat text OK]\n");

    // ---- S2b image message + on-demand blob fetch (BR-15/UC-08) ----
    carol.cmd(QStringLiteral("sendimage %1 %2").arg(roomA, png));
    const auto imgMsg = alice.waitEvent("msg", [](const QJsonObject &o) { return o["kind"].toInt() == int(MsgKind::Image); }, 10000);
    const QString imgHash = imgMsg["hash"].toString();
    alice.cmd(QStringLiteral("watchblob %1").arg(imgHash));
    // the fetch happens inside core (relay receive path); poll until it lands on disk
    const QString blobPath = root + "/alice/blobs/" + imgHash;
    QDeadlineTimer blobUntil(10000);
    while (!QFile::exists(blobPath) && !blobUntil.hasExpired())
        QThread::msleep(100);
    if (!QFile::exists(blobPath))
        alice.fail("image blob not fetched on demand");
    if (sha256OfFile(blobPath).toLatin1() != QByteArray(imgHash.toLatin1()))
        alice.fail("blob content mismatch");
    printf("[S2b image on-demand OK]\n");

    // ---- S3 approval room flow (UC-06 A1/UC-07 approve) ----
    carol.cmd("create 审批房间 approval");
    const QString roomC = carol.waitEvent("room", {}, 5000)["id"].toString();
    { // bob must see the room via heartbeat before joining
        QDeadlineTimer until(20000);
        bool seen = false;
        while (!seen && !until.hasExpired()) {
            bob.cmd("rooms");
            for (const auto &v : bob.waitEvent("rooms", {}, 10000)["list"].toArray())
                if (v.toObject()["id"].toString() == roomC)
                    seen = true;
            if (!seen)
                QThread::msleep(300);
        }
        if (!seen)
            bob.fail("approval room never appeared via heartbeat");
    }
    bob.cmd(QStringLiteral("join %1").arg(roomC));
    bob.waitEvent("joincb", [](const QJsonObject &o) { return o["status"].toInt() == int(JoinStatus::PendingApproval); }, 10000);
    qint64 row = -1;
    QString bfp;
    QDeadlineTimer apprUntil(15000);
    while (row < 0 && !apprUntil.hasExpired()) {
        const auto j = carol.waitEvent("joinreq", [&](const QJsonObject &o) { return o["room"].toString() == roomC; }, 8000);
        row = qint64(j["row"].toDouble());
        bfp = j["fp"].toString();
    }
    if (row < 0)
        carol.fail("no approval request surfaced to host");
    carol.cmd(QStringLiteral("approve %1 1").arg(row));
    bob.waitEvent("join", [](const QJsonObject &o) { return o["status"].toInt() == int(JoinStatus::Accepted); }, 15000);
    printf("[S3 approval OK]\n");

    // ---- S4 moderation: mute rejects send, kick, blacklist (UC-07/BR-13/BR-14) ----
    alice.cmd(QStringLiteral("op %1 %2 mute 0").arg(roomA, bob.fp)); // until manual
    alice.waitEvent("opres", [](const QJsonObject &o) { return o["ok"].toBool(); }, 8000);
    QThread::msleep(1200); // let the MemberEvent reach bob's mute state
    bob.cmd(QStringLiteral("send %1 我已被禁言").arg(roomA));
    bob.waitEvent("senderr", [](const QJsonObject &o) { return !o["why"].toString().isEmpty(); }, 10000);
    // non-host cannot mute others beyond rights: carol(member) tries -> rejected (403 path)
    carol.cmd(QStringLiteral("op %1 %2 mute 1000").arg(roomA, bob.fp));
    carol.waitEvent("opres", [](const QJsonObject &o) { return !o["ok"].toBool(); }, 8000);
    alice.cmd(QStringLiteral("op %1 %2 unmute 0").arg(roomA, bob.fp));
    alice.waitEvent("opres", [](const QJsonObject &o) { return o["ok"].toBool(); }, 8000);
    // admin grant: carol becomes admin, kicks bob
    alice.cmd(QStringLiteral("op %1 %2 setadmin 0").arg(roomA, carol.fp));
    alice.waitEvent("opres", [](const QJsonObject &o) { return o["ok"].toBool(); }, 8000);
    carol.cmd(QStringLiteral("op %1 %2 kick 0").arg(roomA, bob.fp));
    bob.waitEvent("kicked", [&](const QJsonObject &o) { return o["room"].toString() == roomA; }, 10000);
    // admin (carol) must not be able to mute the host (BR-13)
    carol.cmd(QStringLiteral("op %1 %2 mute 1000").arg(roomA, alice.fp));
    carol.waitEvent("opres", [](const QJsonObject &o) { return !o["ok"].toBool() && !o["err"].toString().isEmpty(); }, 8000);
    // blacklist blocks rejoin even with open policy (BR-14)
    alice.cmd(QStringLiteral("op %1 %2 black 0").arg(roomA, bob.fp));
    alice.waitEvent("opres", [](const QJsonObject &o) { return o["ok"].toBool(); }, 8000);
    bob.cmd(QStringLiteral("join %1").arg(roomA));
    bob.waitEvent("joincb", [](const QJsonObject &o) { return o["status"].toInt() == int(JoinStatus::Blacklisted); }, 10000);
    alice.cmd(QStringLiteral("op %1 %2 unblack 0").arg(roomA, bob.fp));
    alice.waitEvent("opres", [](const QJsonObject &o) { return o["ok"].toBool(); }, 8000);
    bob.cmd(QStringLiteral("join %1").arg(roomA));
    bob.waitEvent("joincb", [](const QJsonObject &o) { return o["status"].toInt() == int(JoinStatus::Accepted); }, 10000);
    printf("[S4 moderation OK]\n");

    // ---- S5 announce + download + auto supplier (UC-09/10/11/12, BR-17/18/24) ----
    bob.cmd(QStringLiteral("annfile %1 %2").arg(roomA, file1));
    const QString hash1 = QString::fromLatin1(h1);
    alice.waitEvent("annrecv", [&](const QJsonObject &o) { return o["hash"].toString() == hash1; }, 10000);
    alice.cmd(QStringLiteral("download %1 %2").arg(roomA, hash1));
    const auto done1 = alice.waitEvent("dl-done", [&](const QJsonObject &o) { return o["hash"].toString() == hash1; }, 90000);
    const QString dest1 = done1["dest"].toString();
    if (QFileInfo(dest1).fileName() != QStringLiteral("file1.bin"))
        alice.fail("verified file not delivered under announced name (BR-24)");
    if (sha256OfFile(dest1) != hash1)
        alice.fail("delivered file hash mismatch");
    if (QDir(root + "/alice/downloads/" + hash1).exists())
        alice.fail("chunk cache not cleaned after verification");
    // BR-17 dedupe + BR-18 supplier registry updated
    bob.cmd(QStringLiteral("suppliers %1 %2").arg(roomA, hash1));
    {
        QDeadlineTimer until(15000);
        bool found = false;
        while (!found && !until.hasExpired()) {
            const auto o = bob.waitEvent("sups", {}, 8000)["list"].toArray();
            for (const auto &v : o)
                if (v.toObject()["fp"].toString() == alice.fp.left(32))
                    found = true;
            if (!found) {
                QThread::msleep(300);
                bob.cmd(QStringLiteral("suppliers %1 %2").arg(roomA, hash1));
            }
        }
        if (!found)
            bob.fail("completer did not register as supplier (BR-18)");
    }
    printf("[S5 announce+download OK]\n");

    // ---- S6 crash-mid-download + faulty supplier switch + resume (UC-11 E1/E3, UC-12) ----
    bob.cmd(QStringLiteral("annfile %1 %2").arg(roomA, file2));
    mallory.cmd(QStringLiteral("annfile %1 %2").arg(roomA, file2));
    const QString hash2 = QString::fromLatin1(h2);
    alice.waitEvent("annrecv", [&](const QJsonObject &o) { return o["hash"].toString() == hash2; }, 15000);
    alice.cmd(QStringLiteral("download %1 %2").arg(roomA, hash2));
    // Kill both suppliers right after the task is created: loopback transfers
    // are fast enough to finish a 7 MiB file before a fixed sleep would fire.
    // The task must persist whatever landed and wait for a live supplier
    // (UC-11 E1/E3); restarting bob below then resumes it.
    alice.waitEvent("dl-start", [&](const QJsonObject &o) { return o["hash"].toString() == hash2; }, 15000);
    QThread::msleep(800); // let a few chunks land before the suppliers die
    bob.killNow(); // honest supplier crashes mid-transfer
    mallory.killNow(); // faulty supplier: killed before it can serve more bad chunks
    QThread::msleep(1500); // let the client notice both suppliers are gone
    alice.cmd(QStringLiteral("tstat %1").arg(hash2));
    const auto st = alice.waitEvent("tstat", {}, 8000);
    const int stv = st["status"].toInt();
    if (stv != int(TaskStatus::WaitingSupply) && stv != int(TaskStatus::Downloading))
        alice.fail(QStringLiteral("unexpected status after suppliers died: %1").arg(stv));
    const double doneMid = st["done"].toDouble();
    if (doneMid <= 0)
        alice.fail("no chunk progress was persisted before the supplier crash (BR-23)");
    // restart the honest supplier with its persisted data dir -> resume continues (BR-20/23)
    bob.start(selfPath, nodeEnv("bob", "24702", udpPorts[1], false));
    readyOf(&bob);
    const auto done2 = alice.waitEvent("dl-done", [&](const QJsonObject &o) { return o["hash"].toString() == hash2; }, 120000);
    if (sha256OfFile(done2["dest"].toString()) != hash2)
        alice.fail("resumed download produced corrupt merge");
    alice.cmd(QStringLiteral("tstat %1").arg(hash2));
    const auto st2 = alice.waitEvent("tstat", {}, 8000);
    if (st2["done"].toDouble() < doneMid)
        alice.fail("progress regressed vs pre-crash (BR-23 resume ignored persisted chunks)");
    printf("[S6 crash-resume+supplier-switch OK] (pre-crash chunks: %.0f)\n", doneMid);

    // ---- S7 pause/resume UI flow (UC-11 steps 6-7) ----
    carol.cmd(QStringLiteral("annfile %1 %2").arg(roomA, file3));
    const QString hash3 = QString::fromLatin1(h3);
    bob.waitEvent("annrecv", [&](const QJsonObject &o) { return o["hash"].toString() == hash3; }, 15000);
    bob.cmd(QStringLiteral("download %1 %2").arg(roomA, hash3));
    bob.waitEvent("dl-start", [&](const QJsonObject &o) { return o["hash"].toString() == hash3; }, 15000);
    QThread::msleep(900); // deterministic: paced chunks, some already on disk
    bob.cmd(QStringLiteral("pause %1").arg(hash3));
    QThread::msleep(600);
    bob.cmd(QStringLiteral("tstat %1").arg(hash3));
    const auto pst = bob.waitEvent("tstat", {}, 8000);
    if (pst["status"].toInt() != int(TaskStatus::Paused))
        bob.fail("pause did not take effect");
    if (pst["done"].toDouble() <= 0)
        bob.fail("no chunk was persisted before the pause (BR-23)");
    // QDir::isEmpty(NoDotAndDotDot) selects no entry types at all, so list files
    // explicitly: completed chunks must still be on disk while paused.
    const QStringList pausedChunks =
        QDir(root + "/bob/downloads/" + hash3).entryList(QDir::Files | QDir::NoDotAndDotDot);
    if (pausedChunks.isEmpty())
        bob.fail("chunk files vanished while paused");
    bob.cmd(QStringLiteral("resume %1").arg(hash3));
    const auto done3 = bob.waitEvent("dl-done", [&](const QJsonObject &o) { return o["hash"].toString() == hash3; }, 120000);
    if (sha256OfFile(done3["dest"].toString()) != hash3)
        bob.fail("resume-after-pause file corrupt");
    printf("[S7 pause/resume OK]\n");

    // ---- S7b crash during download + restart: persisted resume (UC-11 E2 / BR-20) ----
    // Alice is restarted with the same data dir right after the task is created.
    // Engine::start() must reload the identity (UC-01 A1) and recoverTasks()
    // must pick the persisted task up again from the persisted supplier list.
    alice.cmd(QStringLiteral("download %1 %2").arg(roomA, hash3));
    alice.waitEvent("dl-start", [&](const QJsonObject &o) { return o["hash"].toString() == hash3; }, 15000);
    QThread::msleep(600); // persist a few chunks, then crash
    alice.killNow(); // simulated crash (no clean shutdown, no pause)
    alice.start(selfPath, nodeEnv("alice", "24701", udpPorts[0], false));
    const QString aliceFpBeforeRestart = alice.fp;
    readyOf(&alice);
    if (alice.fp != aliceFpBeforeRestart)
        alice.fail("restart lost the local identity (UC-01 A1 loadAccount)");
    alice.cmd(QStringLiteral("tstat %1").arg(hash3));
    const auto rst = alice.waitEvent("tstat", {}, 8000);
    if (rst["done"].toDouble() <= 0)
        alice.fail("restart lost persisted chunk progress (BR-23)");
    const auto done3b = alice.waitEvent("dl-done", [&](const QJsonObject &o) { return o["hash"].toString() == hash3; }, 180000);
    if (sha256OfFile(done3b["dest"].toString()) != hash3)
        alice.fail("restart-resumed download produced a corrupt file (BR-20/BR-24)");
    printf("[S7b crash-restart resume OK]\n");

    // ---- S11 quote + recall (UC-08 extension): 2-minute self window,
    // host/admin override, tombstone persistence, non-moderator rejection ----
    alice.cmd(QStringLiteral("send %1 quote-target").arg(roomA));
    const auto targetEvent = bob.waitEvent(
        "msg", [](const QJsonObject &o) { return o["text"].toString() == QStringLiteral("quote-target"); },
        10000);
    const QString targetId = targetEvent["id"].toString();
    bob.cmd(QStringLiteral("sendq %1 %2 quoted-by-bob").arg(roomA, targetId));
    const auto quotedEvent = alice.waitEvent(
        "msg", [](const QJsonObject &o) { return o["text"].toString() == QStringLiteral("quoted-by-bob"); },
        10000);
    if (quotedEvent["replyTo"].toString() != targetId)
        alice.fail("quoted message lost its replyTo reference");
    const QString quotedId = quotedEvent["id"].toString();

    bob.cmd(QStringLiteral("recall %1 %2").arg(roomA, quotedId));
    bob.waitEvent("recallreq", [&](const QJsonObject &o) { return o["ok"].toBool(); }, 5000);
    alice.waitEvent("recalled",
                    [&](const QJsonObject &o) { return o["id"].toString() == quotedId; }, 10000);
    bob.waitEvent("recalled",
                  [&](const QJsonObject &o) { return o["id"].toString() == quotedId; }, 10000);
    alice.cmd(QStringLiteral("msgs %1").arg(roomA));
    {
        const auto list = alice.waitEvent("msgs", {}, 8000)["list"].toArray();
        bool found = false;
        for (const auto &v : list) {
            const auto m = v.toObject();
            if (m["id"].toString() != quotedId)
                continue;
            found = true;
            if (!m["recalled"].toBool())
                alice.fail("recall did not persist the tombstone flag");
            if (!m["text"].toString().isEmpty())
                alice.fail("recall did not clear the message body");
        }
        if (!found)
            alice.fail("recalled message disappeared from history");
    }

    // Sender window expiry: local pre-check rejects after 1.5s (test override).
    bob.cmd(QStringLiteral("send %1 expiring-bob").arg(roomA));
    const auto expiring = bob.waitEvent(
        "msg", [](const QJsonObject &o) { return o["text"].toString() == QStringLiteral("expiring-bob"); },
        10000);
    QThread::msleep(2200);
    bob.cmd(QStringLiteral("recall %1 %2").arg(roomA, expiring["id"].toString()));
    bob.waitEvent("recallreq", [](const QJsonObject &o) { return !o["ok"].toBool(); }, 5000);

    // Host-side window enforcement: bypass the local pre-check with a raw frame.
    bob.cmd(QStringLiteral("recallraw %1 %2").arg(roomA, expiring["id"].toString()));
    bob.waitEvent("recallfail",
                  [&](const QJsonObject &o) { return o["id"].toString() == expiring["id"].toString(); },
                  10000);

    // Host can recall any message at any time.
    bob.cmd(QStringLiteral("send %1 host-recalls").arg(roomA));
    const auto hostTarget = bob.waitEvent(
        "msg", [](const QJsonObject &o) { return o["text"].toString() == QStringLiteral("host-recalls"); },
        10000);
    QThread::msleep(1800);
    alice.cmd(QStringLiteral("recall %1 %2").arg(roomA, hostTarget["id"].toString()));
    bob.waitEvent("recalled",
                  [&](const QJsonObject &o) { return o["id"].toString() == hostTarget["id"].toString(); },
                  10000);

    // Administrator (carol) can recall a message from the host at any time.
    alice.cmd(QStringLiteral("send %1 admin-recalls").arg(roomA));
    const auto adminTarget = bob.waitEvent(
        "msg", [](const QJsonObject &o) { return o["text"].toString() == QStringLiteral("admin-recalls"); },
        10000);
    QThread::msleep(1800);
    carol.cmd(QStringLiteral("recall %1 %2").arg(roomA, adminTarget["id"].toString()));
    bob.waitEvent("recalled",
                  [&](const QJsonObject &o) { return o["id"].toString() == adminTarget["id"].toString(); },
                  10000);

    // A normal member cannot recall somebody else's (unrecalled) message.
    alice.cmd(QStringLiteral("send %1 member-cannot-recall").arg(roomA));
    const auto protectedTarget = bob.waitEvent(
        "msg", [](const QJsonObject &o) { return o["text"].toString() == QStringLiteral("member-cannot-recall"); },
        10000);
    bob.cmd(QStringLiteral("recallraw %1 %2").arg(roomA, protectedTarget["id"].toString()));
    bob.waitEvent("recallfail",
                  [&](const QJsonObject &o) { return o["id"].toString() == protectedTarget["id"].toString(); },
                  10000);
    printf("[S11 quote+recall OK]\n");

    // ---- S8 host dissolve + member cleanup (UC-13) ----
    carol.cmd(QStringLiteral("leave %1").arg(roomC));
    bob.waitEvent("closed", [&](const QJsonObject &o) { return o["room"].toString() == roomC; }, 12000);
    printf("[S8 dissolve OK]\n");

    // ---- S9 account destroy + fresh reinit (UC-02 + UC-01) ----
    // Keep a live bob<->alice channel first, then verify the host's destroy
    // broadcasts a best-effort dissolve before the identity disappears.
    bob.cmd(QStringLiteral("send %1 销毁前最后一条").arg(roomA));
    alice.waitEvent("msg", [&](const QJsonObject &o) { return o["text"].toString() == QStringLiteral("销毁前最后一条"); }, 10000);
    alice.cmd("destroy wipe");
    alice.waitEvent("destroyed", [](const QJsonObject &o) { return o["ok"].toBool(); }, 8000);
    bob.waitEvent("closed", [&](const QJsonObject &o) { return o["room"].toString() == roomA; }, 12000);
    alice.killNow();
    alice.start(selfPath, nodeEnv("alice", "24701", udpPorts[0], false)); // restart: auto-creates brand-new identity
    const QString oldFp = alice.fp;
    readyOf(&alice);
    if (alice.fp == oldFp)
        alice.fail("destroyAccount did not remove identity (new start reused old fp)");
    printf("[S9 destroy+reinit OK]\n");

    // ---- S10 live LAN segment switch (network selection at runtime) ----
    alice.cmd("segment");
    const auto seg0 = alice.waitEvent("segment", {}, 5000);
    if (seg0["ip"].toString() != QStringLiteral("127.0.0.1") || seg0["prefix"].toInt() != 8)
        alice.fail("unexpected initial LAN segment (MESHLINK_IP override not applied)");
    alice.cmd("switchseg 127.0.0.2 32"); // no existing peer can be inside this segment
    alice.waitEvent("segswitched", [](const QJsonObject &o) { return o["ok"].toBool(); }, 5000);
    QThread::msleep(300); // allow at least one heartbeat/filter cycle
    alice.cmd("peers");
    if (alice.waitEvent("peers", {}, 5000)["count"].toInt() != 0)
        alice.fail("segment switch did not clear out-of-segment peers");
    alice.cmd("switchseg 127.0.0.1 8"); // switch back to the shared test segment
    alice.waitEvent("segswitched", [](const QJsonObject &o) { return o["ok"].toBool(); }, 5000);
    {
        QDeadlineTimer until(20000);
        int count = 0;
        while (!until.hasExpired() && count < 2) {
            QThread::msleep(300);
            alice.cmd("peers");
            count = alice.waitEvent("peers", {}, 5000)["count"].toInt();
        }
        if (count < 2)
            alice.fail("peers did not reappear after switching back (unicast bootstrap broken)");
    }
    printf("[S10 segment switch OK]\n");

    for (Node *n : {&alice, &bob, &carol, &mallory}) {
        if (n->proc && n->proc->state() == QProcess::Running) {
            n->cmd("quit");
            n->proc->waitForFinished(4000);
            if (n->proc->state() == QProcess::Running)
                n->proc->kill();
        }
    }
    printf("tst_integration: ALL SCENARIOS OK\n");
    return 0;
}

// ===========================================================================
int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    const int ni = args.indexOf(QStringLiteral("--node"));
    if (ni >= 0 && ni + 1 < args.size())
        return nodeMain(app, args[ni + 1]);
    return parentMain(app);
}
