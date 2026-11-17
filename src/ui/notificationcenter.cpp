// deepin-MeshLink — notification orchestration implementation.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "notificationcenter.h"

#include "core/engine.h"
#include "core/notifypolicy.h"
#include "core/roommanager.h"
#include "xdgnotifications.h"

#include <QCoreApplication>

namespace meshlink {

NotificationCenter::NotificationCenter(Engine *engine, std::function<bool()> windowActive,
                                       std::function<QString()> currentRoom, QObject *parent)
    : QObject(parent), m_engine(engine), m_windowActive(std::move(windowActive)),
      m_currentRoom(std::move(currentRoom)) {
    m_notifier = new XdgNotifier(this);

    auto *chat = m_engine->chat();
    auto *rooms = m_engine->rooms();
    connect(chat, &ChatManager::messageReceived, this,
            [this](const QString &roomId, const Msg &message) { onMessage(roomId, message); });
    // Host-side pending approval: notify even for muted rooms (it is an
    // action request, not a chat message).
    connect(rooms, &RoomManager::joinRequested, this,
            [this](const QString &roomId, const PendingJoin &) { onJoinRequest(roomId); });
    connect(rooms, &RoomManager::kickedFromRoom, this, [this](const QString &roomId,
                                                              const QString &reason) {
        NotifyPolicy::Context c;
        c.globalEnabled = m_engine->settings()->notificationsEnabled();
        if (NotifyPolicy::decide(c) == NotifyPolicy::Action::Skip)
            return;
        const RoomLocal room = m_engine->rooms()->room(roomId);
        m_notifier->notify(tr("Removed from room"),
                           tr("%1: %2").arg(room.name, reason), {}, {}, 5000, 0);
    });
    connect(rooms, &RoomManager::roomClosedByHost, this, [this](const QString &roomId) {
        NotifyPolicy::Context c;
        c.globalEnabled = m_engine->settings()->notificationsEnabled();
        if (NotifyPolicy::decide(c) == NotifyPolicy::Action::Skip)
            return;
        const RoomLocal room = m_engine->rooms()->room(roomId);
        m_notifier->notify(tr("Room closed"),
                           tr("%1 was dissolved by the host.").arg(room.name), {}, {}, 5000,
                           0);
        m_replaceIds.remove(roomId);
    });

    // Notification body click ("default" action): jump to the room it belongs
    // to. replaces_id maps the bubble back to its room.
    connect(m_notifier, &XdgNotifier::actionInvoked, this,
            [this](quint32 id, const QString &actionKey) {
                if (actionKey != QLatin1String("default") || id == 0)
                    return;
                const QString roomId = m_replaceIds.key(id);
                if (!roomId.isEmpty()) {
                    m_replaceIds.remove(roomId);
                    emit activateRoom(roomId);
                }
            });
}

bool NotificationCenter::daemonAvailable() const { return m_notifier->available(); }

void NotificationCenter::onMessage(const QString &roomId, const Msg &message) {
    if (message.kind == static_cast<quint8>(MsgKind::System))
        return; // notices render in-app only
    const QString self = m_engine->account()->profile().fingerprint;
    const bool mentions = NotifyPolicy::textMentionsMe(
        message.text, m_engine->rooms()->members(roomId), self);

    NotifyPolicy::Context c;
    c.globalEnabled = m_engine->settings()->notificationsEnabled();
    c.windowActive = m_windowActive && m_windowActive();
    c.viewingRoom = m_currentRoom && m_currentRoom() == roomId;
    c.roomDnd = m_engine->rooms()->isRoomDnd(roomId);
    c.mentionsMe = mentions;
    c.fromSelf = message.senderFp == self;
    if (NotifyPolicy::decide(c) == NotifyPolicy::Action::Skip)
        return;

    const RoomLocal room = m_engine->rooms()->room(roomId);
    const QString roomName = room.name.isEmpty() ? tr("Room") : room.name;

    // Mentions raise urgency-style attention; keep one bubble per room.
    const QString summary =
        mentions ? tr("%1 mentioned you · %2").arg(message.senderNick, roomName)
                 : QStringLiteral("%1 · %2").arg(roomName, message.senderNick);
    QString body;
    if (m_engine->settings()->notificationPreview()) {
        switch (message.kind) {
        case static_cast<quint8>(MsgKind::Image):
            body = tr("Sent an image");
            break;
        case static_cast<quint8>(MsgKind::File):
            body = message.fileName.isEmpty()
                       ? tr("Shared a file")
                       : tr("Shared a file: %1").arg(message.fileName);
            break;
        default:
            body = message.text.simplified();
            if (body.size() > 120)
                body = body.left(120) + QChar(0x2026);
            break;
        }
    } else {
        body = tr("New message");
    }

    QVariantMap hints;
    hints.insert(QStringLiteral("desktop-entry"),
                 QStringLiteral("cn.org.huansoft.deepin-meshlink"));
    if (mentions)
        hints.insert(QStringLiteral("urgency"), 2); // critical -> visible even in DDE DND

    const quint32 replaces = m_replaceIds.value(roomId, 0);
    m_notifier->notify(summary, body, {QStringLiteral("default"), tr("Open")}, hints, 5000,
                       replaces, [this, roomId](quint32 id) {
                           if (id != 0)
                               m_replaceIds.insert(roomId, id);
                       });
    if (mentions && (!c.viewingRoom || !c.windowActive))
        bumpMention(roomId);
}

void NotificationCenter::onJoinRequest(const QString &roomId) {
    NotifyPolicy::Context c;
    c.globalEnabled = m_engine->settings()->notificationsEnabled();
    c.windowActive = m_windowActive && m_windowActive();
    c.viewingRoom = m_currentRoom && m_currentRoom() == roomId;
    if (NotifyPolicy::decide(c) == NotifyPolicy::Action::Skip)
        return;
    const RoomLocal room = m_engine->rooms()->room(roomId);
    m_notifier->notify(tr("Join request"),
                       tr("%1: a node asked to join; open the room to approve.").arg(room.name),
                       {QStringLiteral("default"), tr("Open")}, {}, 5000, 0);
}

void NotificationCenter::bumpMention(const QString &roomId) {
    m_mentions[roomId] = m_mentions.value(roomId, 0) + 1;
    emitMentions();
}

void NotificationCenter::emitMentions() {
    int total = 0;
    for (auto it = m_mentions.constBegin(); it != m_mentions.constEnd(); ++it)
        total += it.value();
    emit unreadMentionsChanged(total, m_mentions);
}

void NotificationCenter::clearRoom(const QString &roomId) {
    if (m_mentions.remove(roomId) > 0)
        emitMentions();
}

void NotificationCenter::clearAll() {
    if (m_mentions.isEmpty())
        return;
    m_mentions.clear();
    emitMentions();
}

} // namespace meshlink
