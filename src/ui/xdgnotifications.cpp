// deepin-MeshLink — XDG desktop notification client implementation.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "xdgnotifications.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>

namespace meshlink {
namespace {
constexpr const char *kService = "org.freedesktop.Notifications";
constexpr const char *kPath = "/org/freedesktop/Notifications";
constexpr const char *kInterface = "org.freedesktop.Notifications";
} // namespace

XdgNotifier::XdgNotifier(QObject *parent) : QObject(parent) {
    m_appName = QStringLiteral("deepin-meshlink");
    connectDaemonSignals();
}

void XdgNotifier::connectDaemonSignals() {
    auto *bus = QDBusConnection::sessionBus().interface();
    m_available = bus && bus->isServiceRegistered(QString::fromLatin1(kService));
    if (!m_available)
        return;
    QDBusConnection::sessionBus().connect(QString::fromLatin1(kService),
                                          QString::fromLatin1(kPath),
                                          QString::fromLatin1(kInterface),
                                          QStringLiteral("ActionInvoked"), this,
                                          SLOT(actionInvokedReceived(quint32, QString)));
    QDBusConnection::sessionBus().connect(QString::fromLatin1(kService),
                                          QString::fromLatin1(kPath),
                                          QString::fromLatin1(kInterface),
                                          QStringLiteral("NotificationClosed"), this,
                                          SLOT(notificationClosedReceived(quint32, quint32)));
}

// Qt DBus signal forwarding slots (declared in the header, connected by name).
void XdgNotifier::actionInvokedReceived(quint32 id, const QString &key) {
    emit actionInvoked(id, key);
}
void XdgNotifier::notificationClosedReceived(quint32 id, quint32 reason) {
    emit notificationClosed(id, reason);
}

bool XdgNotifier::available() const { return m_available; }

void XdgNotifier::notify(const QString &summary, const QString &body, const QStringList &actions,
                         const QVariantMap &hints, int timeoutMs, quint32 replacesId,
                         const std::function<void(quint32)> &idCallback) {
    if (!m_available) {
        if (idCallback)
            idCallback(0);
        return;
    }
    QDBusMessage call = QDBusMessage::createMethodCall(
        QString::fromLatin1(kService), QString::fromLatin1(kPath),
        QString::fromLatin1(kInterface), QStringLiteral("Notify"));
    call << m_appName << replacesId << QStringLiteral("cn.org.huansoft.deepin-meshlink")
         << summary << body << actions << hints << timeoutMs;
    QDBusPendingCall pending = QDBusConnection::sessionBus().asyncCall(call);
    if (!idCallback)
        return;
    auto *watcher = new QDBusPendingCallWatcher(pending, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [watcher, idCallback](QDBusPendingCallWatcher *) {
                QDBusPendingReply<quint32> reply = *watcher;
                idCallback(reply.isError() ? 0 : reply.value());
                watcher->deleteLater();
            });
}

void XdgNotifier::close(quint32 id) {
    if (!m_available || id == 0)
        return;
    QDBusMessage call = QDBusMessage::createMethodCall(
        QString::fromLatin1(kService), QString::fromLatin1(kPath),
        QString::fromLatin1(kInterface), QStringLiteral("CloseNotification"));
    call << id;
    QDBusConnection::sessionBus().asyncCall(call);
}

} // namespace meshlink
