// deepin-MeshLink — notification orchestration: policy + XDG delivery.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#pragma once

#include "core/common.h"
#include "core/protocol.h"

#include <QHash>
#include <QObject>
#include <QPointer>

#include <functional>

class QWidget;

namespace meshlink {

class Engine;
class XdgNotifier;

// Decides when a desktop notification is shown (per-room do-not-disturb with
// an @-mention override, in-app suppression, global switch) and delivers it
// through the XDG org.freedesktop.Notifications interface. Lives only in the
// GUI process; the headless engine never constructs one.
class NotificationCenter : public QObject {
    Q_OBJECT
public:
    // `windowActive`/`currentRoom` are queried live so the policy always sees
    // the current main-window state.
    NotificationCenter(Engine *engine, std::function<bool()> windowActive,
                       std::function<QString()> currentRoom, QObject *parent = nullptr);

    bool daemonAvailable() const;

public slots:
    // The user opened `roomId` (or activated the window): drop its badge.
    void clearRoom(const QString &roomId);
    void clearAll();

signals:
    void activateRoom(const QString &roomId); // notification body clicked
    void unreadMentionsChanged(int total, const QHash<QString, int> &perRoom);

private:
    void onMessage(const QString &roomId, const Msg &message);
    void onJoinRequest(const QString &roomId);
    void bumpMention(const QString &roomId);
    void emitMentions();

    Engine *m_engine = nullptr;
    XdgNotifier *m_notifier = nullptr;
    std::function<bool()> m_windowActive;
    std::function<QString()> m_currentRoom;
    QHash<QString, quint32> m_replaceIds; // roomId -> last notification id
    QHash<QString, int> m_mentions;       // roomId -> unread mention count
};

} // namespace meshlink
