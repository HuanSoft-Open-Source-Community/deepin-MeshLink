// deepin-MeshLink — XDG tray icon: org.kde.StatusNotifierItem + DBusMenu.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
//
// Qt's QSystemTrayIcon only implements the StatusNotifierItem protocol on the
// xcb platform; the Wayland session of deepin V25 provides none. This module
// speaks the freedesktop SNI spec (as used by dde-shell's tray area) directly
// over QtDBus, so the resident icon behaves identically on both back ends.
#pragma once

#include <QDBusArgument>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QObject>
#include <QString>
#include <QVariantMap>

namespace meshlink {

class SniItem;
class SniMenu;

class XdgTrayIcon : public QObject {
    Q_OBJECT
public:
    explicit XdgTrayIcon(QObject *parent = nullptr);
    ~XdgTrayIcon() override;

    bool available() const { return m_available; }
    void setTitle(const QString &title);   // tray item title (also shown as label)
    void setToolTip(const QString &tip);   // merged into the title label
    void setIconName(const QString &name); // freedesktop icon theme name

    // Static menu model: (id, label) pairs; id 0 means "separator".
    void setMenuItems(const QVector<QPair<int, QString>> &items);

signals:
    void activateRequested();          // left click on the icon
    void secondaryActivateRequested(); // middle click
    void menuTriggered(int itemId);    // a menu entry was clicked
    void availableChanged(bool available);

private slots:
    void checkWatcher();

private:
    bool registerItem();
    void unregisterItem();

    SniItem *m_item = nullptr;
    SniMenu *m_menu = nullptr;
    bool m_available = false;
    bool m_registered = false;
};

// ---- DBus plumbing -------------------------------------------------------

// com.canonical.dbusmenu GetLayout reply item: (i id, a{sv} props, av children)
struct SniMenuLayoutItem {
    int id = 0;
    QVariantMap properties;
    QVector<SniMenuLayoutItem> children;
};

QDBusArgument &operator<<(QDBusArgument &arg, const SniMenuLayoutItem &item);
const QDBusArgument &operator>>(const QDBusArgument &arg, SniMenuLayoutItem &item);

class SniItem : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.StatusNotifierItem")
    // StatusNotifierItem properties (the pixmap variants are optional and the
    // theme icon name is always installed by the package).
    Q_PROPERTY(QString Category READ category CONSTANT)
    Q_PROPERTY(QString Id READ id CONSTANT)
    Q_PROPERTY(QString Title READ title NOTIFY titleChanged)
    Q_PROPERTY(QString Status READ status CONSTANT)
    Q_PROPERTY(quint32 WindowId READ windowId CONSTANT)
    Q_PROPERTY(QString IconName READ iconName NOTIFY iconChanged)
    Q_PROPERTY(QDBusObjectPath Menu READ menu CONSTANT)

public:
    explicit SniItem(QObject *parent = nullptr);

    QString category() const { return QStringLiteral("Communications"); }
    QString id() const { return QStringLiteral("deepin-meshlink"); }
    QString title() const { return m_title; }
    QString status() const { return QStringLiteral("Active"); }
    quint32 windowId() const { return 0; }
    QString iconName() const { return m_iconName; }
    QDBusObjectPath menu() const { return QDBusObjectPath(QStringLiteral("/MenuBar")); }

    void setTitle(const QString &title);
    void setIconName(const QString &name);

signals:
    void NewTitle();
    void NewIcon();
    void NewStatus(const QString &status);
    void titleChanged();
    void iconChanged();

public slots:
    void Activate(int x, int y);
    void SecondaryActivate(int x, int y);
    void Scroll(int dx, const QString &direction);

private:
    QString m_title;
    QString m_iconName;
};

class SniMenu : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.canonical.dbusmenu")
    Q_PROPERTY(uint Version READ version CONSTANT)
    Q_PROPERTY(QString Status READ status CONSTANT)

public:
    explicit SniMenu(QObject *parent = nullptr);
    uint version() const { return 3; }
    QString status() const { return QStringLiteral("normal"); }

    void setItems(const QVector<QPair<int, QString>> &items);
    const QVector<QPair<int, QString>> &items() const { return m_items; }

signals:
    void LayoutUpdated(uint revision, int parent);
    void ItemActivationRequested(int id, uint timestamp);

public slots:
    void GetLayout(int parentId, int recursionDepth, const QStringList &propertyNames,
                   uint &revision, SniMenuLayoutItem &layout);
    bool AboutToShow(int parentId);
    void Event(int id, const QString &eventId, const QDBusVariant &data, uint timestamp);
    void GetGroupProperties(const QList<int> &ids, const QStringList &propertyNames,
                            QVariantMap &itemProperties, QList<int> &missing);
    uint GetRevision() const { return m_revision; }

private:
    uint m_revision = 1;
    QVector<QPair<int, QString>> m_items; // id -> label; label empty = separator
};

} // namespace meshlink
