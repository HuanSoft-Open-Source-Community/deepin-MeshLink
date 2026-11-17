// deepin-MeshLink — XDG desktop notification client (org.freedesktop.Notifications).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <functional>

class QDBusPendingCallWatcher;

namespace meshlink {

// Minimal client for the freedesktop desktop-notification specification as
// implemented by the deepin session (dde notification center). Works on both
// X11 and Wayland sessions; when no notification daemon is reachable every
// call degrades to a no-op instead of failing the caller.
class XdgNotifier : public QObject {
    Q_OBJECT
public:
    explicit XdgNotifier(QObject *parent = nullptr);

    bool available() const;

    // Shows a notification. `replacesId` updates an existing bubble (0 = new).
    // `actions` is a flat [key, label, ...] list; "default" is the body click.
    void notify(const QString &summary, const QString &body, const QStringList &actions,
                const QVariantMap &hints, int timeoutMs, quint32 replacesId,
                const std::function<void(quint32)> &idCallback = {});
    void close(quint32 id);

signals:
    void actionInvoked(quint32 id, const QString &actionKey);
    void notificationClosed(quint32 id, quint32 reason);

private slots:
    // daemon signal forwarding (connected by name in connectDaemonSignals)
    void actionInvokedReceived(quint32 id, const QString &actionKey);
    void notificationClosedReceived(quint32 id, quint32 reason);

private:
    void connectDaemonSignals();

    QString m_appName;
    bool m_available = false;
};

} // namespace meshlink
