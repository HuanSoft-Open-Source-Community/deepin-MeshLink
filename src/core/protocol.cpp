// deepin-MeshLink — wire protocol implementation.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "protocol.h"

#include <QDataStream>
#include <QVariant>

namespace meshlink {

static QDataStream::Version kStreamVersion = QDataStream::Qt_6_0;

QByteArray payloadOf(const std::function<void(QDataStream &)> &writer) {
    QByteArray bytes;
    QDataStream ds(&bytes, QIODevice::WriteOnly);
    ds.setVersion(kStreamVersion);
    writer(ds);
    return bytes;
}

bool readPayload(const QByteArray &payload, const std::function<bool(QDataStream &)> &reader) {
    QDataStream ds(payload);
    ds.setVersion(kStreamVersion);
    if (!reader(ds))
        return false;
    return ds.status() == QDataStream::Ok || ds.atEnd();
}

QByteArray frameMessage(MsgType type, const QByteArray &payload) {
    QByteArray out;
    QDataStream ds(&out, QIODevice::WriteOnly);
    ds.setVersion(kStreamVersion);
    ds.setByteOrder(QDataStream::BigEndian);
    ds << kFrameMagic << kProtoVersion
       << static_cast<quint8>(type)
       << static_cast<quint16>(0)
       << static_cast<quint32>(payload.size());
    out.append(payload);
    return out;
}

bool tryParseFrame(const QByteArray &buffer, MsgType &type, QByteArray &payload, int &consumed) {
    consumed = 0;
    if (buffer.size() < kFrameHeaderSize)
        return false;
    QDataStream ds(buffer.left(kFrameHeaderSize));
    ds.setVersion(kStreamVersion);
    ds.setByteOrder(QDataStream::BigEndian);
    quint32 magic = 0, len = 0;
    quint8 ver = 0, t = 0;
    quint16 reserved = 0;
    ds >> magic >> ver >> t >> reserved >> len;
    if (magic != kFrameMagic || ver != kProtoVersion || len > kMaxFramePayload)
        return false;
    if (static_cast<quint32>(buffer.size()) < static_cast<quint32>(kFrameHeaderSize) + len)
        return false; // incomplete
    type = static_cast<MsgType>(t);
    payload = buffer.mid(kFrameHeaderSize, static_cast<int>(len));
    consumed = kFrameHeaderSize + static_cast<int>(len);
    return true;
}

// ---------- helpers for member lists / message lists ----------
static void writeMemberList(QDataStream &ds, const QVector<RoomMemberInfo> &members) {
    ds << static_cast<quint32>(members.size());
    for (const auto &m : members) {
        ds << m.fp << m.nickname << m.avatarHash << m.role << m.mutedUntil;
    }
}
static QVector<RoomMemberInfo> readMemberList(QDataStream &ds) {
    QVector<RoomMemberInfo> out;
    quint32 n = 0;
    ds >> n;
    if (n > 100000)
        return {};
    out.reserve(static_cast<int>(n));
    for (quint32 i = 0; i < n; ++i) {
        RoomMemberInfo m;
        ds >> m.fp >> m.nickname >> m.avatarHash >> m.role >> m.mutedUntil;
        out.append(m);
    }
    return out;
}

static void writeMsgList(QDataStream &ds, const QVector<Msg> &msgs) {
    ds << static_cast<quint32>(msgs.size());
    for (const auto &m : msgs) {
        ds << m.msgId << m.senderFp << m.senderNick << m.avatarHash << m.kind << m.seq << m.ts
           << m.text << m.contentHash << m.fileName << m.mime << m.fileSize << m.imgW << m.imgH;
    }
}
static QVector<Msg> readMsgList(QDataStream &ds) {
    QVector<Msg> out;
    quint32 n = 0;
    ds >> n;
    if (n > 100000)
        return {};
    out.reserve(static_cast<int>(n));
    for (quint32 i = 0; i < n; ++i) {
        Msg m;
        ds >> m.msgId >> m.senderFp >> m.senderNick >> m.avatarHash >> m.kind >> m.seq >> m.ts
           >> m.text >> m.contentHash >> m.fileName >> m.mime >> m.fileSize >> m.imgW >> m.imgH;
        out.append(m);
    }
    return out;
}

// ---------- message impls ----------
#define MESH_LINK_IMPL_SIMPLE(name, fields_write, fields_read)                                      \
    QByteArray name::toPayload() const {                                                            \
        return payloadOf([this](QDataStream &ds) { fields_write; });                                \
    }                                                                                               \
    bool name::fromPayload(const QByteArray &p, name &m) {                                          \
        return readPayload(p, [&m](QDataStream &ds) { fields_read; return ds.status() != QDataStream::ReadPastEnd; }); \
    }

MESH_LINK_IMPL_SIMPLE(HelloMsg,
                      (ds << fp << nickname << avatarHash << ed25519Pub),
                      (ds >> m.fp >> m.nickname >> m.avatarHash >> m.ed25519Pub))

MESH_LINK_IMPL_SIMPLE(AuthMsg,
                      (ds << fp << nickname << avatarHash << ed25519Pub << nonce << sig), (ds >> m.fp >> m.nickname >> m.avatarHash >> m.ed25519Pub >> m.nonce >> m.sig))

MESH_LINK_IMPL_SIMPLE(JoinRequestMsg, (ds << roomId << fingerprint << nickname),
                      (ds >> m.roomId >> m.fingerprint >> m.nickname))

MESH_LINK_IMPL_SIMPLE(JoinResponseMsg, (ds << roomId << fingerprint << status << message << assignedReqId), (ds >> m.roomId >> m.fingerprint >> m.status >> m.message >> m.assignedReqId))

QByteArray RoomSnapshotMsg::toPayload() const {
    return payloadOf([this](QDataStream &ds) {
        ds << roomId << name << intro << inviteCode << policy << globalMute << capacity << memberCount;
        writeMemberList(ds, members);
        writeMsgList(ds, recent);
    });
}
bool RoomSnapshotMsg::fromPayload(const QByteArray &p, RoomSnapshotMsg &m) {
    return readPayload(p, [&m](QDataStream &ds) {
        ds >> m.roomId >> m.name >> m.intro >> m.inviteCode >> m.policy >> m.globalMute >> m.capacity
           >> m.memberCount;
        m.members = readMemberList(ds);
        m.recent = readMsgList(ds);
        return ds.status() != QDataStream::ReadPastEnd;
    });
}

QByteArray ChatMessageMsg::toPayload() const {
    return payloadOf([this](QDataStream &ds) {
        ds << roomId << msgId << senderFp << senderNick << avatarHash << kind << seq << ts << text
           << contentHash << fileName << mime << fileSize << imgW << imgH;
    });
}
bool ChatMessageMsg::fromPayload(const QByteArray &p, ChatMessageMsg &m) {
    return readPayload(p, [&m](QDataStream &ds) {
        ds >> m.roomId >> m.msgId >> m.senderFp >> m.senderNick >> m.avatarHash >> m.kind >> m.seq
           >> m.ts >> m.text >> m.contentHash >> m.fileName >> m.mime >> m.fileSize >> m.imgW >> m.imgH;
        return ds.status() != QDataStream::ReadPastEnd;
    });
}

MESH_LINK_IMPL_SIMPLE(ChatAckMsg, (ds << msgId << seq << delivered), (ds >> m.msgId >> m.seq >> m.delivered))

MESH_LINK_IMPL_SIMPLE(FileAnnounceMsg,
                      (ds << roomId << msgId << fileHash << fileName << mime << announcerFp << announcerNick << avatarHash << fileSize << ts << mode), (ds >> m.roomId >> m.msgId >> m.fileHash >> m.fileName >> m.mime >> m.announcerFp >> m.announcerNick >> m.avatarHash >> m.fileSize >> m.ts >> m.mode))

MESH_LINK_IMPL_SIMPLE(SuppliersQueryMsg, (ds << fileHash), (ds >> m.fileHash))

QByteArray SuppliersResponseMsg::toPayload() const {
    return payloadOf([this](QDataStream &ds) {
        ds << fileHash << static_cast<quint32>(suppliers.size());
        for (const auto &s : suppliers)
            ds << s.fp << s.nickname << s.ip << s.port << s.online;
    });
}
bool SuppliersResponseMsg::fromPayload(const QByteArray &p, SuppliersResponseMsg &m) {
    return readPayload(p, [&m](QDataStream &ds) {
        ds >> m.fileHash;
        quint32 n = 0;
        ds >> n;
        if (n > 100000)
            return false;
        for (quint32 i = 0; i < n; ++i) {
            SupplierInfo s;
            ds >> s.fp >> s.nickname >> s.ip >> s.port >> s.online;
            m.suppliers.append(s);
        }
        return ds.status() != QDataStream::ReadPastEnd;
    });
}

QByteArray FileRequestMsg::toPayload() const {
    return payloadOf([this](QDataStream &ds) {
        ds << fileHash << chunkSize << static_cast<quint32>(have.size());
        for (auto h : have)
            ds << h;
    });
}
bool FileRequestMsg::fromPayload(const QByteArray &p, FileRequestMsg &m) {
    return readPayload(p, [&m](QDataStream &ds) {
        ds >> m.fileHash >> m.chunkSize;
        quint32 n = 0;
        ds >> n;
        if (n > 10u * 1024 * 1024)
            return false;
        m.have.reserve(static_cast<int>(n));
        for (quint32 i = 0; i < n; ++i) {
            quint64 v = 0;
            ds >> v;
            m.have.append(v);
        }
        return ds.status() != QDataStream::ReadPastEnd;
    });
}

QByteArray FileResponseMsg::toPayload() const {
    return payloadOf([this](QDataStream &ds) {
        ds << fileHash << fileSize << chunkSize << totalChunks << supportsChunkHash << err
           << static_cast<quint32>(chunkHashes.size());
        for (const auto &h : chunkHashes)
            ds << h;
    });
}
bool FileResponseMsg::fromPayload(const QByteArray &p, FileResponseMsg &m) {
    return readPayload(p, [&m](QDataStream &ds) {
        ds >> m.fileHash >> m.fileSize >> m.chunkSize >> m.totalChunks >> m.supportsChunkHash >> m.err;
        quint32 n = 0;
        ds >> n;
        if (n > 10u * 1024 * 1024)
            return false;
        m.chunkHashes.reserve(static_cast<int>(n));
        for (quint32 i = 0; i < n; ++i) {
            QString h;
            ds >> h;
            m.chunkHashes.append(h);
        }
        return ds.status() != QDataStream::ReadPastEnd;
    });
}

MESH_LINK_IMPL_SIMPLE(ChunkRequestMsg, (ds << fileHash << index << chunkSize), (ds >> m.fileHash >> m.index >> m.chunkSize))

MESH_LINK_IMPL_SIMPLE(ChunkDataMsg, (ds << fileHash << index << size << sha256 << data),
                      (ds >> m.fileHash >> m.index >> m.size >> m.sha256 >> m.data))

MESH_LINK_IMPL_SIMPLE(MemberEventMsg, (ds << roomId << targetFp << targetNick << op << role << mutedUntil << reason), (ds >> m.roomId >> m.targetFp >> m.targetNick >> m.op >> m.role >> m.mutedUntil >> m.reason))

MESH_LINK_IMPL_SIMPLE(AdminOpMsg, (ds << roomId << actorFp << targetFp << op << durationMs),
                      (ds >> m.roomId >> m.actorFp >> m.targetFp >> m.op >> m.durationMs))

MESH_LINK_IMPL_SIMPLE(BlobRequestMsg, (ds << hash), (ds >> m.hash))
MESH_LINK_IMPL_SIMPLE(BlobDataMsg, (ds << hash << data), (ds >> m.hash >> m.data))
MESH_LINK_IMPL_SIMPLE(ErrorMessageMsg, (ds << reason), (ds >> m.reason))

// ---------- map conversions (UI/DB boundaries) ----------
QVariantMap RoomMemberInfo::toMap() const {
    return {{"fp", fp}, {"nickname", nickname}, {"avatarHash", avatarHash},
            {"role", role}, {"mutedUntil", mutedUntil}, {"online", online}};
}
RoomMemberInfo RoomMemberInfo::fromMap(const QVariantMap &m) {
    RoomMemberInfo r;
    r.fp = m.value("fp").toString();
    r.nickname = m.value("nickname").toString();
    r.avatarHash = m.value("avatarHash").toString();
    r.role = static_cast<quint8>(m.value("role", 2).toInt());
    r.mutedUntil = m.value("mutedUntil").toLongLong();
    r.online = m.value("online").toBool();
    return r;
}

QVariantMap Msg::toMap() const {
    return {{"roomId", roomId}, {"msgId", msgId},  {"senderFp", senderFp},   {"senderNick", senderNick},
            {"avatarHash", avatarHash}, {"kind", kind}, {"seq", static_cast<qulonglong>(seq)}, {"ts", ts},
            {"status", status}, {"contentHash", contentHash}, {"fileName", fileName}, {"mime", mime},
            {"fileSize", fileSize}, {"imgW", imgW}, {"imgH", imgH}, {"text", text}};
}
Msg Msg::fromMap(const QVariantMap &m) {
    Msg x;
    x.roomId = m.value("roomId").toString();
    x.msgId = m.value("msgId").toString();
    x.senderFp = m.value("senderFp").toString();
    x.senderNick = m.value("senderNick").toString();
    x.avatarHash = m.value("avatarHash").toString();
    x.kind = static_cast<quint8>(m.value("kind").toInt());
    x.seq = m.value("seq").toULongLong();
    x.ts = m.value("ts").toLongLong();
    x.status = static_cast<quint8>(m.value("status").toInt());
    x.contentHash = m.value("contentHash").toString();
    x.fileName = m.value("fileName").toString();
    x.mime = m.value("mime").toString();
    x.fileSize = m.value("fileSize").toLongLong();
    x.imgW = m.value("imgW").toInt();
    x.imgH = m.value("imgH").toInt();
    x.text = m.value("text").toString();
    return x;
}

SupplierInfo SupplierInfo::fromMap(const QVariantMap &m) {
    SupplierInfo s;
    s.fp = m.value("fp").toString();
    s.nickname = m.value("nickname").toString();
    s.ip = m.value("ip").toString();
    s.port = static_cast<quint16>(m.value("port").toInt());
    s.online = m.value("online").toBool();
    return s;
}
QVariantMap SupplierInfo::toMap() const {
    return {{"fp", fp}, {"nickname", nickname}, {"ip", ip}, {"port", port}, {"online", online}};
}

} // namespace meshlink
