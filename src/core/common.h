// deepin-MeshLink — common types, ids, enums, defaults.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#pragma once

#include <QByteArray>
#include <QColor>
#include <QDateTime>
#include <QString>
#include <QVector>

#include <cstdint>

namespace meshlink {

// ---- application identity ----
inline constexpr const char *kAppId = "cn.org.huansoft.deepin-meshlink";
inline constexpr const char *kAppName = "deepin-meshlink";
inline constexpr const char *kDisplayName = "\xe6\xb7\xb1\xe5\xba\xa6\xe7\xbb\x87\xe8\xae\xaf"; // 深度织讯
inline constexpr const char *kVersion = "1.0.0";
inline constexpr const char *kOrgName = "HuanSoft Open Source Community";
inline constexpr const char *kOrgDomain = "huansoft.org.cn";

// ---- wire constants ----
inline constexpr quint32 kFrameMagic = 0x4D4C4E4B; // "MLNK"
inline constexpr quint8 kProtoVersion = 1;
inline constexpr int kFrameHeaderSize = 12; // magic4+ver1+type1+reserved2+len4
inline constexpr quint32 kMaxFramePayload = 8u * 1024 * 1024; // 8 MiB
inline constexpr quint32 kMaxUdpPacket = 1400;                // stays under MTU (BR-08)
inline constexpr int kMaxHeartbeatRooms = 8;                  // room summaries per heartbeat

using MsgId = QString;   // random 16-hex
using FileHash = QString; // 64-hex sha256
using PeerId = QString;  // 32-hex account fingerprint
using RoomId = QString;  // 16-hex, derived on creation

enum class JoinPolicy : quint8 { Open = 0, Approval = 1 };

enum class MemberRole : quint8 { Host = 0, Admin = 1, Member = 2 };

enum class MuteMode : quint8 { None = 0, Timed = 1, UntilManual = 2 };

enum class MsgKind : quint8 { Text = 0, Image = 1, File = 2, System = 3 };

enum class MsgStatus : quint8 { Pending = 0, Sent = 1, Delivered = 2, Failed = 3 };

enum class TaskStatus : quint8 { Queued = 0, Downloading = 1, Paused = 2, Verifying = 3,
                                 Done = 4, Failed = 5, WaitingSupply = 6 };

enum class ChunkStatus : quint8 { Pending = 0, Done = 1 };

enum class JoinStatus : quint8 { Accepted = 0, PendingApproval = 1, Denied = 2,
                                 RoomFull = 3, Blacklisted = 4, RoomGone = 5 };

enum class FileErr : quint8 { Ok = 0, NotFound = 1, NotShared = 2, TooLarge = 3, Busy = 4 };

// Admin operations on members (UC-07).
enum class AdminOp : quint8 { SetAdmin = 0, UnsetAdmin = 1, Mute = 2, Unmute = 3,
                              Kick = 4, Blacklist = 5, Unblacklist = 6, GlobalMuteOn = 7,
                              GlobalMuteOff = 8 };

struct AdminOpResult {
    bool ok = false;
    QString error;
};

inline qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

// Short display form of a fingerprint: first 8 hex.
inline QString fpShort(const QString &fp) { return fp.left(8); }

} // namespace meshlink
