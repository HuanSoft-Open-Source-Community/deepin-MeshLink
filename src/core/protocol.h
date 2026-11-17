// deepin-MeshLink — wire protocol: framing + typed messages.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
//
// Every TCP payload (post-TLS, NFR-10) is framed as:
//   magic(4) | version(1) | msgType(1) | reserved(2) | payloadLen(4) | payload
// Payload itself is a QDataStream (Qt_6_0) of type-specific fields.
// The SAME message structs are also serialised (detached, no frame) for the
// signed UDP heartbeat wrapper (see discovery.h / NFR-14).
#pragma once

#include "common.h"

#include <QByteArray>
#include <QDataStream>
#include <QIODevice>
#include <QString>
#include <QVariantList>
#include <QVector>

#include <functional>

namespace meshlink {

enum class MsgType : quint8 {
    // handshake / identity
    Hello = 1,          // a->b  {fp, ed25519pub, nickname, avatarHash}
    HelloAck = 2,       // b->a  {fp, ed25519pub, nickname, avatarHash}
    // rooms
    RoomCreate = 10,    // local-only request (UI -> engine), not sent
    RoomList = 11,      // query hosted rooms (unused in V1, discovery carries them)
    JoinRequest = 12,   // member -> host
    JoinResponse = 13,  // host -> member
    RoomSnapshot = 14,  // host -> member
    RoomStateSync = 15, // member -> host (pull) ; host -> member (resp)
    LeaveRoom = 16,     // member -> host ; host -> member (confirm)
    MemberEvent = 17,   // host -> members (role/mute/kick/blacklist changes)
    RoomClosed = 18,    // host -> members (host destroyed room)
    AdminOp = 19,       // admin/host -> host
    // chat
    ChatMessage = 30,   // member -> host ; host -> members
    ChatAck = 31,       // host -> sender
    ChatRecall = 32,    // member -> host (request) ; host -> members (event)
    ChatRecallAck = 33, // host -> actor (failure / result)
    // files
    FileAnnounce = 40,  // member -> host -> members (announcement only, no bytes)
    SuppliersQuery = 41,// member -> host
    SuppliersResponse = 42,
    FileRequest = 43,   // downloader -> supplier {hash, chunkSize, have[] }
    FileResponse = 44,  // supplier -> downloader {size, chunkSize, totalChunks, perChunkHash?}
    ChunkRequest = 45,  // downloader -> supplier {index}
    ChunkData = 46,     // supplier -> downloader {index, size, sha256?, data}
    FileMissing = 47,   // supplier -> downloader
    BlobRequest = 48,   // any -> peer (avatar/image by hash)
    BlobData = 49,
    // transfer
    Heartbeat = 100,    // UDP only
    HeartbeatBye = 101, // UDP only (A1 farewell)
    Error = 127,
};

// ---- framing helpers ----
QByteArray frameMessage(MsgType type, const QByteArray &payload);
// returns false if buffer does not yet contain a full frame.
bool tryParseFrame(const QByteArray &buffer, MsgType &type, QByteArray &payload, int &consumed);
QByteArray payloadOf(const std::function<void(QDataStream &)> &writer);
bool readPayload(const QByteArray &payload, const std::function<bool(QDataStream &)> &reader);

// ---- message payloads ----
struct HelloMsg {
    QString fp, nickname, avatarHash;
    QByteArray ed25519Pub; // 32 raw bytes
    QByteArray toPayload() const;
    static bool fromPayload(const QByteArray &, HelloMsg &);
};

// Post-TLS identity authentication frame (session-bound, see securelink.cpp).
// signed = Ed25519( "ML-A1" || fp || nonce || sha256(ourCertPem) || sha256(peerCertPem) )
struct AuthMsg {
    QString fp, nickname, avatarHash;
    QByteArray ed25519Pub; // 32 raw bytes
    QByteArray nonce;      // 16 random bytes
    QByteArray sig;        // 64 bytes
    QByteArray toPayload() const;
    static bool fromPayload(const QByteArray &, AuthMsg &);
};

struct JoinRequestMsg {
    QString roomId, fingerprint, nickname;
    QByteArray toPayload() const;
    static bool fromPayload(const QByteArray &, JoinRequestMsg &);
};

struct JoinResponseMsg {
    QString roomId, fingerprint;
    quint8 status = 0; // JoinStatus
    QString message;
    qint64 assignedReqId = 0;
    QByteArray toPayload() const;
    static bool fromPayload(const QByteArray &, JoinResponseMsg &);
};

struct RoomMemberInfo {
    QString fp, nickname, avatarHash;
    quint8 role = 2;
    qint64 mutedUntil = 0; // ms epoch; 0 = not muted; -1 = until manual
    bool online = false;
    QVariantMap toMap() const;
    static RoomMemberInfo fromMap(const QVariantMap &);
};

// Generic chat message record stored locally.
struct Msg {
    QString roomId, msgId, senderFp, senderNick, avatarHash, text;
    quint8 kind = 0;
    quint64 seq = 0;
    qint64 ts = 0;
    quint8 status = 0;
    QString contentHash, fileName, mime;
    QString replyTo;       // quoted msgId (empty = none)
    bool recalled = false; // tombstone; content fields are cleared
    QString recalledBy;
    qint64 recalledTs = 0;
    qint64 recvTs = 0;     // host-authoritative receipt time (recall window)
    qint64 fileSize = -1;
    int imgW = 0, imgH = 0;
    QVariantMap toMap() const;
    static Msg fromMap(const QVariantMap &);
};

struct RoomSnapshotMsg {
    QString roomId, name, intro, inviteCode;
    quint8 policy = 0;
    bool globalMute = false;
    int capacity = 0, memberCount = 0;
    QVector<RoomMemberInfo> members;
    QVector<Msg> recent; // last messages (see chatmanager)
    QByteArray toPayload() const;
    static bool fromPayload(const QByteArray &, RoomSnapshotMsg &);
};

struct ChatMessageMsg {
    QString roomId, msgId, senderFp, senderNick, avatarHash, text;
    quint8 kind = 0;      // MsgKind
    quint64 seq = 0;      // assigned by host (BR-16)
    qint64 ts = 0;
    // image/file metadata embedded in the *announcement* only (no binaries, BR-15/17)
    QString contentHash, fileName, mime;
    QString replyTo;       // quoted msgId
    qint64 fileSize = -1, mutedUntilEcho = 0;
    int imgW = 0, imgH = 0;
    QByteArray toPayload() const;
    static bool fromPayload(const QByteArray &, ChatMessageMsg &);
};

struct ChatAckMsg {
    QString msgId;
    quint64 seq = 0;
    bool delivered = false;
    QByteArray toPayload() const;
    static bool fromPayload(const QByteArray &, ChatAckMsg &);
};

struct FileAnnounceMsg {
    QString roomId, msgId, fileHash, fileName, mime, announcerFp, announcerNick, avatarHash;
    QString replyTo; // quoted msgId
    qint64 fileSize = 0, ts = 0;
    quint8 mode = 0; // 0 = new announcement (message + supplier), 1 = supplier registration only
    QByteArray toPayload() const;
    static bool fromPayload(const QByteArray &, FileAnnounceMsg &);
};

struct ChatRecallMsg {
    QString roomId, msgId;
    QByteArray toPayload() const;
    static bool fromPayload(const QByteArray &, ChatRecallMsg &);
};

struct ChatRecallAckMsg {
    QString roomId, msgId;
    bool ok = false;
    QString error;
    QByteArray toPayload() const;
    static bool fromPayload(const QByteArray &, ChatRecallAckMsg &);
};

struct SuppliersQueryMsg {
    QString fileHash;
    QByteArray toPayload() const;
    static bool fromPayload(const QByteArray &, SuppliersQueryMsg &);
};

struct SupplierInfo {
    QString fp, nickname, ip;
    quint16 port = 0;
    bool online = false;
    QVariantMap toMap() const;
    static SupplierInfo fromMap(const QVariantMap &);
};

struct SuppliersResponseMsg {
    QString fileHash;
    QVector<SupplierInfo> suppliers;
    QByteArray toPayload() const;
    static bool fromPayload(const QByteArray &, SuppliersResponseMsg &);
};

struct FileRequestMsg {
    QString fileHash;
    quint64 chunkSize = 0;
    QVector<quint64> have; // chunk indices already local (resume)
    QByteArray toPayload() const;
    static bool fromPayload(const QByteArray &, FileRequestMsg &);
};

struct FileResponseMsg {
    QString fileHash;
    qint64 fileSize = 0;
    quint64 chunkSize = 0, totalChunks = 0;
    bool supportsChunkHash = true;
    QVector<QString> chunkHashes; // optional
    quint8 err = 0;               // FileErr
    QByteArray toPayload() const;
    static bool fromPayload(const QByteArray &, FileResponseMsg &);
};

struct ChunkRequestMsg {
    QString fileHash;
    quint64 index = 0;
    quint64 chunkSize = 0; // negotiated layout (meta handshake)
    QByteArray toPayload() const;
    static bool fromPayload(const QByteArray &, ChunkRequestMsg &);
};

struct ChunkDataMsg {
    QString fileHash;
    quint64 index = 0, size = 0;
    QString sha256;
    QByteArray data;
    QByteArray toPayload() const;
    static bool fromPayload(const QByteArray &, ChunkDataMsg &);
};

struct MemberEventMsg {
    QString roomId, targetFp, targetNick;
    quint8 op = 0;     // AdminOp or extended kEvJoined/kEvLeft
    quint8 role = 2;
    qint64 mutedUntil = 0;
    QString reason;
    QByteArray toPayload() const;
    static bool fromPayload(const QByteArray &, MemberEventMsg &);
};

struct AdminOpMsg {
    QString roomId, actorFp, targetFp;
    quint8 op = 0;
    qint64 durationMs = 0; // for mute
    QByteArray toPayload() const;
    static bool fromPayload(const QByteArray &, AdminOpMsg &);
};

struct BlobRequestMsg {
    QString hash;
    QByteArray toPayload() const;
    static bool fromPayload(const QByteArray &, BlobRequestMsg &);
};

struct BlobDataMsg {
    QString hash;
    QByteArray data;
    QByteArray toPayload() const;
    static bool fromPayload(const QByteArray &, BlobDataMsg &);
};

struct ErrorMessageMsg {
    QString reason;
    QByteArray toPayload() const;
    static bool fromPayload(const QByteArray &, ErrorMessageMsg &);
};

} // namespace meshlink
