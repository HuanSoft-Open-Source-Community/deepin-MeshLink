// deepin-MeshLink — text/image messages (UC-08).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
//
// Relay model (UC-08 main path): member -> TLS -> host assigns seq & persists
// -> TLS fan-out to other online members. Image binaries never ride in the
// message itself: the message carries hash/size (BR-15), bytes are fetched
// over TLS on demand (BLOB request).
#pragma once

#include "common.h"
#include "protocol.h"
#include "securelink.h"
#include "storage.h"

#include <QObject>
#include <QImage>

namespace meshlink {

class AccountManager;
class RoomManager;
class Settings;

// shared message-log helpers (also used by FileManager for kind=File rows)
namespace MessageStore {
bool append(Storage *s, const Msg &m);
QVector<Msg> latest(Storage *s, const QString &roomId, int limit);
quint64 maxSeq(Storage *s, const QString &roomId);
bool setStatus(Storage *s, const QString &roomId, const QString &msgId, quint8 status);
bool find(Storage *s, const QString &roomId, const QString &msgId, Msg &out);
bool markRecalled(Storage *s, const QString &roomId, const QString &msgId,
                  const QString &byFp, qint64 ts);
} // namespace MessageStore

class ChatManager : public QObject {
    Q_OBJECT
public:
    using SendFn = std::function<bool(const QString &, MsgType, const QByteArray &)>;
    ChatManager(AccountManager *acct, Storage *storage, RoomManager *rooms, SecureLink *link,
                QObject *parent = nullptr);
    void setSend(SendFn fn) { m_send = std::move(fn); }

    // sender side
    QString sendText(const QString &roomId, const QString &text, QString *error = nullptr,
                     const QString &replyTo = {});
    // stores image to blob cache, returns msgId; bytes fetched on demand (BR-15)
    QString sendImage(const QString &roomId, const QString &imagePath, QString *error = nullptr,
                      const QString &replyTo = {});

    // Quote/recall: recallMessage() executes on the host or queues a request
    // over the TLS room channel; the outcome arrives via signals below.
    bool recallMessage(const QString &roomId, const QString &msgId, QString *error = nullptr);
    Msg message(const QString &roomId, const QString &msgId) const;

    // ask `fromFp` for blob bytes (avatar or image) over TLS; emits blobReady when stored
    void requestBlob(const QString &hash, const QString &fromFp);
    // returns local path of a cached blob, empty if absent
    static QString blobPath(const QString &hash);
    static bool hasBlob(const QString &hash);

    void handle(MsgType type, SecureChannel *ch, const QByteArray &payload);

    QVector<Msg> history(const QString &roomId, int limit) const;

signals:
    void messageReceived(const QString &roomId, const Msg &m);
    void messageStatusChanged(const QString &roomId, const QString &msgId, quint8 status);
    // a pending blob (avatar/image bytes) landed in the blob dir
    void blobReady(const QString &hash);
    void notice(const QString &roomId, const QString &text); // e.g. 被禁言拒收提示
    void messageRecalled(const QString &roomId, const QString &msgId, const QString &byFp);
    void recallFailed(const QString &roomId, const QString &msgId, const QString &error);

private:
    void serveBlob(SecureChannel *ch, const QByteArray &payload);
    void hostReceive(const ChatMessageMsg &cm, SecureChannel *ch); // authoritative append + fan-out
    bool hostRecall(const QString &roomId, const QString &msgId, const QString &actorFp,
                    QString *error);
    void handleRecall(SecureChannel *ch, const QByteArray &payload);
    void handleRecallAck(SecureChannel *ch, const QByteArray &payload);
    AccountManager *m_acct;
    Storage *m_storage;
    RoomManager *m_rooms;
    SecureLink *m_link;
    SendFn m_send;
};

} // namespace meshlink
