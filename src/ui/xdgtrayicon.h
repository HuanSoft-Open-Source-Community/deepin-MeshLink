// deepin-MeshLink — XDG tray icon: org.kde.StatusNotifierItem + DBusMenu.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
//
// Qt's QSystemTrayIcon only implements the StatusNotifierItem protocol on the
// xcb platform; the Wayland session of deepin V25 provides none. This module
// speaks the freedesktop SNI spec (as used by dde-shell's tray area) directly
// over QtDBus, so the resident icon behaves identically on both back ends.
//
// The item exports both an icon theme name and the embedded product pixmaps:
// dde-tray-loader (the V25 tray host) resolves the icon as
//   QIcon::fromTheme(IconName)  ->  IconThemePath + IconName  ->  IconPixmap
// so a build whose icon is not installed into the hicolor theme would show a
// blank slot without the pixmap fallback.
//
// The right-click menu is served through the canonical com.canonical.dbusmenu
// interface (the dialect spoken by libdbusmenuqt/dde-tray-loader): GetLayout,
// GetGroupProperties, GetProperty, AboutToShow, AboutToShowGroup, Event and
// EventGroup.
#pragma once

#include <QByteArray>
#include <QDBusArgument>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

// ---- DBus payload types (signatures follow the SNI / DBusMenu specs) ------

// SNI image: (iiay) width, height and ARGB32 pixels in network byte order.
struct SniImage {
    int width = 0;
    int height = 0;
    QByteArray pixels;
};
using SniImageList = QList<SniImage>;

// SNI tooltip: (sa(iiay)ss) icon name, icon pixmap, title, description.
struct SniToolTip {
    QString iconName;
    SniImageList iconPixmap;
    QString title;
    QString description;
};

// com.canonical.dbusmenu GetLayout item: (ia{sv}av).
struct SniMenuLayoutItem {
    int id = 0;
    QVariantMap properties;
    QList<SniMenuLayoutItem> children;
};

// com.canonical.dbusmenu GetGroupProperties entry: (ia{sv}).
struct SniMenuItem {
    int id = 0;
    QVariantMap properties;
};
using SniMenuItemList = QList<SniMenuItem>;

QDBusArgument &operator<<(QDBusArgument &arg, const SniImage &image);
const QDBusArgument &operator>>(const QDBusArgument &arg, SniImage &image);

QDBusArgument &operator<<(QDBusArgument &arg, const SniToolTip &tip);
const QDBusArgument &operator>>(const QDBusArgument &arg, SniToolTip &tip);

QDBusArgument &operator<<(QDBusArgument &arg, const SniMenuLayoutItem &item);
const QDBusArgument &operator>>(const QDBusArgument &arg, SniMenuLayoutItem &item);

QDBusArgument &operator<<(QDBusArgument &arg, const SniMenuItem &item);
const QDBusArgument &operator>>(const QDBusArgument &arg, SniMenuItem &item);

namespace meshlink {

class SniItem;
class SniMenu;
class XdgTrayIcon;

class XdgTrayIcon : public QObject {
    Q_OBJECT
public:
    explicit XdgTrayIcon(QObject *parent = nullptr);
    ~XdgTrayIcon() override;

    // True once the item is actually announced to a live
    // org.kde.StatusNotifierWatcher (MainWindow::closeEvent relies on this to
    // decide whether hiding to the tray is meaningful).
    bool available() const { return m_available; }

    void setTitle(const QString &title);   // tray item title (also the tooltip)
    void setToolTip(const QString &tip);   // explicit tooltip text (SNI ToolTip)
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
    void onNameOwnerChanged(const QString &service, const QString &oldOwner,
                            const QString &newOwner);

private:
    bool registerObjects();
    void unregisterObjects();
    void announceToWatcher();
    void setAvailable(bool available);

    SniItem *m_item = nullptr;
    SniMenu *m_menu = nullptr;
    bool m_available = false;
    bool m_objectsRegistered = false;
    bool m_announced = false;       // RegisterStatusNotifierItem succeeded for m_watcherOwner
    bool m_announcePending = false; // a RegisterStatusNotifierItem call is in flight
    int m_announceRetries = 0;
    QString m_watcherOwner; // unique bus name of the watcher we announced to
};

class SniItem : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.StatusNotifierItem")

    // Properties used by dde-tray-loader's application-tray plugin (it reads
    // IconName, IconThemePath, IconPixmap, Menu, Title, Status, ToolTip,
    // ItemIsMenu, OverlayIcon*, AttentionIcon*).  WindowId is "i" per the spec.
    Q_PROPERTY(QString Category READ category CONSTANT)
    Q_PROPERTY(QString Id READ id CONSTANT)
    Q_PROPERTY(QString Title READ title)
    Q_PROPERTY(QString Status READ status)
    Q_PROPERTY(int WindowId READ windowId CONSTANT)
    Q_PROPERTY(QString IconThemePath READ iconThemePath CONSTANT)
    Q_PROPERTY(QDBusObjectPath Menu READ menu CONSTANT)
    Q_PROPERTY(bool ItemIsMenu READ itemIsMenu CONSTANT)
    Q_PROPERTY(QString IconName READ iconName)
    Q_PROPERTY(SniImageList IconPixmap READ iconPixmap)
    Q_PROPERTY(QString OverlayIconName READ overlayIconName)
    Q_PROPERTY(SniImageList OverlayIconPixmap READ overlayIconPixmap)
    Q_PROPERTY(QString AttentionIconName READ attentionIconName)
    Q_PROPERTY(SniImageList AttentionIconPixmap READ attentionIconPixmap)
    Q_PROPERTY(QString AttentionMovieName READ attentionMovieName)
    Q_PROPERTY(SniToolTip ToolTip READ toolTip)

public:
    explicit SniItem(XdgTrayIcon *tray, QObject *parent = nullptr);

    QString category() const { return QStringLiteral("Communications"); }
    QString id() const { return QStringLiteral("deepin-meshlink"); }
    QString title() const { return m_title; }
    QString status() const { return m_status; }
    int windowId() const { return 0; }
    QString iconThemePath() const { return QString(); }
    QDBusObjectPath menu() const { return QDBusObjectPath(QStringLiteral("/MenuBar")); }
    bool itemIsMenu() const { return false; }
    QString iconName() const { return m_iconName; }
    SniImageList iconPixmap() const { return m_iconPixmap; }
    QString overlayIconName() const { return m_overlayIconName; }
    SniImageList overlayIconPixmap() const { return m_overlayIconPixmap; }
    QString attentionIconName() const { return m_attentionIconName; }
    SniImageList attentionIconPixmap() const { return m_attentionIconPixmap; }
    QString attentionMovieName() const { return QString(); }
    SniToolTip toolTip() const { return m_toolTip; }

    void setTitle(const QString &title);
    void setIconName(const QString &name);
    void setIconPixmaps(const SniImageList &icon, const SniImageList &attention = {},
                        const SniImageList &overlay = {});
    void setToolTip(const QString &tip);
    void setStatus(const QString &status);

public slots:
    void Activate(int x, int y);
    void SecondaryActivate(int x, int y);
    void ContextMenu(int x, int y);
    void ProvideXdgActivationToken(const QString &token);
    void Scroll(int delta, const QString &orientation);

signals:
    void NewTitle();
    void NewIcon();
    void NewMenu();
    void NewToolTip();
    void NewAttentionIcon();
    void NewOverlayIcon();
    void NewStatus(const QString &status);

private:
    XdgTrayIcon *m_tray = nullptr;
    QString m_title;
    QString m_status = QStringLiteral("Active");
    QString m_iconName;
    QString m_overlayIconName;
    QString m_attentionIconName;
    SniImageList m_iconPixmap;
    SniImageList m_attentionIconPixmap;
    SniImageList m_overlayIconPixmap;
    SniToolTip m_toolTip;
    bool m_toolTipExplicit = false;
};

class SniMenu : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.canonical.dbusmenu")

    Q_PROPERTY(uint Version READ version CONSTANT)
    Q_PROPERTY(QString Status READ status CONSTANT)

public:
    explicit SniMenu(XdgTrayIcon *tray, QObject *parent = nullptr);

    uint version() const { return 3; }
    QString status() const { return QStringLiteral("normal"); }

    void setItems(const QVector<QPair<int, QString>> &items);
    const QVector<QPair<int, QString>> &items() const { return m_items; }

signals:
    void LayoutUpdated(uint revision, int parent);
    void ItemActivationRequested(int id, uint timestamp);

public slots:
    // Canonical com.canonical.dbusmenu methods.  Output parameters are
    // declared as non-const references, which is how QtDBus exports multiple
    // out arguments on a directly exported QObject.
    void GetLayout(int parentId, int recursionDepth, const QStringList &propertyNames,
                   uint &revision, SniMenuLayoutItem &layout);
    void GetGroupProperties(const QList<int> &ids, const QStringList &propertyNames,
                            QList<SniMenuItem> &properties);
    QDBusVariant GetProperty(int id, const QString &property);
    bool AboutToShow(int id);
    void AboutToShowGroup(const QList<int> &ids, QList<int> &updatesNeeded,
                          QList<int> &idErrors);
    void Event(int id, const QString &eventId, const QDBusVariant &data, uint timestamp);
    // Canonical form: in ai ids, in s eventId, in v data, in u timestamp,
    // out ai idErrors.
    void EventGroup(const QList<int> &ids, const QString &eventId, const QDBusVariant &data,
                    uint timestamp, QList<int> &idErrors);

private:
    // Shared Event()/EventGroup() dispatch; false means the id is unknown.
    bool dispatchEvent(int id, const QString &eventId, const QDBusVariant &data, uint timestamp);

    // DBusMenu id 0 is reserved for the root item, but the public model uses
    // id 0 for separators; map the latter to unique synthetic ids.
    void rebuildDbusIds();
    int dbusIdAt(int index) const { return m_dbusIds.value(index, 0); }
    int indexForDbusId(int id) const;
    QVariantMap propertiesFor(int index, const QStringList &propertyNames) const;

    XdgTrayIcon *m_tray = nullptr;
    uint m_revision = 1;
    QVector<QPair<int, QString>> m_items; // id -> label; label empty = separator
    QVector<int> m_dbusIds;               // index -> exported DBusMenu id
};

} // namespace meshlink

// The payload types are deliberately at global scope: moc records D-Bus method
// parameter names without namespace qualifiers, and QtDBus resolves output
// parameters with QMetaType::fromName(), so a namespaced type would be skipped
// as "Unregistered output type in parameter list" and GetLayout would never be
// exported at all.  The metatype declarations below let moc/property code
// resolve the same names.
Q_DECLARE_METATYPE(SniImage)
Q_DECLARE_METATYPE(SniToolTip)
Q_DECLARE_METATYPE(SniMenuLayoutItem)
Q_DECLARE_METATYPE(SniMenuItem)
