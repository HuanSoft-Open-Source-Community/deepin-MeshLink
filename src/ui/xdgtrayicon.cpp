// deepin-MeshLink — XDG tray icon implementation (SNI + minimal DBusMenu).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "xdgtrayicon.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMetaType>
#include <QDBusMessage>
#include <QTimer>

namespace meshlink {
namespace {

constexpr const char *kWatcherService = "org.kde.StatusNotifierWatcher";
constexpr const char *kWatcherPath = "/StatusNotifierWatcher";
constexpr const char *kWatcherInterface = "org.kde.StatusNotifierWatcher";
constexpr const char *kItemPath = "/StatusNotifierItem";
constexpr const char *kMenuPath = "/MenuBar";

} // namespace

// ---- SniMenuLayoutItem marshalling (i a{sv} av) ----
QDBusArgument &operator<<(QDBusArgument &arg, const SniMenuLayoutItem &item) {
    arg.beginStructure();
    arg << item.id << item.properties;
    arg.beginArray(qMetaTypeId<QDBusVariant>());
    for (const SniMenuLayoutItem &child : item.children)
        arg << QDBusVariant(QVariant::fromValue<SniMenuLayoutItem>(child));
    arg.endArray();
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, SniMenuLayoutItem &item) {
    arg.beginStructure();
    arg >> item.id >> item.properties;
    arg.beginArray();
    item.children.clear();
    while (!arg.atEnd()) {
        SniMenuLayoutItem child;
        arg >> child; // recursive deserialisation via the same operator
        item.children.append(child);
    }
    arg.endArray();
    arg.endStructure();
    return arg;
}

// ---- SniItem ----
SniItem::SniItem(QObject *parent) : QObject(parent) {}

void SniItem::setTitle(const QString &title) {
    if (m_title == title)
        return;
    m_title = title;
    emit NewTitle();
    emit titleChanged();
}

void SniItem::setIconName(const QString &name) {
    if (m_iconName == name)
        return;
    m_iconName = name;
    emit NewIcon();
}

void SniItem::Activate(int, int) {
    // routed through the owner below
    if (auto *tray = qobject_cast<XdgTrayIcon *>(parent()))
        emit tray->activateRequested();
}

void SniItem::SecondaryActivate(int, int) {
    if (auto *tray = qobject_cast<XdgTrayIcon *>(parent()))
        emit tray->secondaryActivateRequested();
}

void SniItem::Scroll(int, const QString &) {}

// ---- SniMenu ----
SniMenu::SniMenu(QObject *parent) : QObject(parent) {}

void SniMenu::setItems(const QVector<QPair<int, QString>> &items) {
    if (m_items == items)
        return;
    m_items = items;
    ++m_revision;
    emit LayoutUpdated(m_revision, 0);
}

void SniMenu::GetLayout(int parentId, int recursionDepth, const QStringList &propertyNames,
                        uint &revision, SniMenuLayoutItem &layout) {
    Q_UNUSED(propertyNames);
    revision = m_revision;
    layout = SniMenuLayoutItem{};
    layout.id = parentId;
    if (parentId != 0) {
        // flat menu: only the root has children
        return;
    }
    for (const auto &entry : m_items) {
        SniMenuLayoutItem child;
        child.id = entry.first;
        if (entry.second.isEmpty()) {
            child.properties.insert(QStringLiteral("type"), QStringLiteral("separator"));
        } else {
            child.properties.insert(QStringLiteral("label"), entry.second);
            child.properties.insert(QStringLiteral("enabled"), true);
        }
        child.properties.insert(QStringLiteral("visible"), true);
        if (recursionDepth != 0)
            layout.children.append(child);
    }
}

bool SniMenu::AboutToShow(int parentId) {
    return parentId == 0; // nothing changed since the last layout fetch
}

void SniMenu::Event(int id, const QString &eventId, const QDBusVariant &data, uint timestamp) {
    Q_UNUSED(data);
    if (eventId != QLatin1String("clicked"))
        return;
    if (auto *tray = qobject_cast<XdgTrayIcon *>(parent()))
        emit tray->menuTriggered(id);
    emit ItemActivationRequested(id, timestamp);
}

void SniMenu::GetGroupProperties(const QList<int> &ids, const QStringList &propertyNames,
                                 QVariantMap &itemProperties, QList<int> &missing) {
    Q_UNUSED(propertyNames);
    itemProperties.clear();
    missing.clear();
    for (int id : ids) {
        bool found = false;
        for (const auto &entry : m_items) {
            if (entry.first == id) {
                found = true;
                QVariantMap props;
                if (entry.second.isEmpty()) {
                    props.insert(QStringLiteral("type"), QStringLiteral("separator"));
                } else {
                    props.insert(QStringLiteral("label"), entry.second);
                    props.insert(QStringLiteral("enabled"), true);
                }
                props.insert(QStringLiteral("visible"), true);
                itemProperties.insert(QString::number(id), QVariant::fromValue(props));
                break;
            }
        }
        if (!found)
            missing.append(id);
    }
}

// ---- XdgTrayIcon ----
XdgTrayIcon::XdgTrayIcon(QObject *parent) : QObject(parent) {
    m_item = new SniItem(this);
    m_menu = new SniMenu(this);
    qDBusRegisterMetaType<SniMenuLayoutItem>();
    // Watch the tray host so the app can degrade when no panel is running.
    QDBusConnection::sessionBus().connect(
        QString::fromLatin1(kWatcherService), QString::fromLatin1(kWatcherPath),
        QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("PropertiesChanged"),
        this, SLOT(checkWatcher()));
    checkWatcher();
}

XdgTrayIcon::~XdgTrayIcon() { unregisterItem(); }

void XdgTrayIcon::checkWatcher() {
    auto *bus = QDBusConnection::sessionBus().interface();
    const bool watcherPresent =
        bus && bus->isServiceRegistered(QString::fromLatin1(kWatcherService));
    if (watcherPresent) {
        if (!m_registered || !m_available)
            m_available = registerItem();
    } else if (m_registered) {
        unregisterItem();
    }
    if (watcherPresent != m_available)
        emit availableChanged(m_available);
}

bool XdgTrayIcon::registerItem() {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.registerObject(QString::fromLatin1(kItemPath), m_item,
                            QDBusConnection::ExportAllContents))
        return false;
    if (!bus.registerObject(QString::fromLatin1(kMenuPath), m_menu,
                            QDBusConnection::ExportAllContents)) {
        bus.unregisterObject(QString::fromLatin1(kItemPath));
        return false;
    }
    QDBusMessage call = QDBusMessage::createMethodCall(
        QString::fromLatin1(kWatcherService), QString::fromLatin1(kWatcherPath),
        QString::fromLatin1(kWatcherInterface), QStringLiteral("RegisterStatusNotifierItem"));
    call << QString::fromLatin1(kItemPath);
    bus.asyncCall(call);
    m_registered = true;
    return true;
}

void XdgTrayIcon::unregisterItem() {
    if (!m_registered)
        return;
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.unregisterObject(QString::fromLatin1(kItemPath));
    bus.unregisterObject(QString::fromLatin1(kMenuPath));
    m_registered = false;
    m_available = false;
}

void XdgTrayIcon::setTitle(const QString &title) { m_item->setTitle(title); }

void XdgTrayIcon::setToolTip(const QString &tip) {
    // The SNI tooltip struct is optional; deepin's tray shows the Title text,
    // so keep the hint visible by folding it into the title.
    Q_UNUSED(tip);
}

void XdgTrayIcon::setIconName(const QString &name) { m_item->setIconName(name); }

void XdgTrayIcon::setMenuItems(const QVector<QPair<int, QString>> &items) {
    m_menu->setItems(items);
}

} // namespace meshlink
