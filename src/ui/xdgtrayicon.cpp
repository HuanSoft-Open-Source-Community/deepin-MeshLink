// deepin-MeshLink — XDG tray icon implementation (SNI + canonical DBusMenu).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
//
// Why the previous version was invisible and had no context menu with
// dde-tray-loader (deepin V25 application-tray plugin):
//   1. Only IconName was exported.  QIcon::fromTheme() finds no entry for a
//      build that was never installed, so the host fell back to its (empty)
//      IconPixmap cache and painted a blank slot.  The embedded product icon
//      is now exported as IconPixmap in ARGB32 network byte order, and the
//      host always has a usable pixmap.
//   2. GetLayout was never exported: QtDBus resolves non-const reference
//      parameters with QMetaType::fromName(), and the layout struct was
//      namespace-qualified while moc records the unqualified name, so the
//      method was dropped as "Unregistered output type in parameter list".
//      The host's DBusMenuImporter then got UnknownMethod and showed an empty
//      menu.  The payload types are now global (see xdgtrayicon.h) and are
//      registered before the objects are exported.
//   3. Registration ran once, synchronously in the constructor.  If the
//      watcher was not up yet (login race) or was restarted, the item was
//      never announced.  Registration is now deferred until the icon, title
//      and menu are configured and re-done on NameOwnerChanged and on the
//      watcher's PropertiesChanged.  Every announcement is tagged with the
//      watcher owner and a generation, so a success reply from a watcher that
//      has been replaced is ignored instead of making the item look available
//      to the current (not yet informed) watcher.
#include "xdgtrayicon.h"

#include "appicon.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QSet>
#include <QTimer>
#include <QtEndian>

namespace {

constexpr const char *kWatcherService = "org.kde.StatusNotifierWatcher";
constexpr const char *kWatcherPath = "/StatusNotifierWatcher";
constexpr const char *kWatcherInterface = "org.kde.StatusNotifierWatcher";
constexpr const char *kItemPath = "/StatusNotifierItem";
constexpr const char *kMenuPath = "/MenuBar";

// Pixel sizes shipped in IconPixmap.  The host picks the closest one; the
// theme name stays exported for hosts that can recolour a themed icon.
constexpr int kIconSizes[] = {16, 22, 24, 32, 48, 64};

// Per-object announcement generation.  It is kept as a dynamic QObject
// property (instead of a member) so this repair needs no change to the class
// layout: every RegisterStatusNotifierItem attempt stamps its generation here
// and a reply is only trusted while the stamp is still current.
constexpr const char *kAnnounceGenerationProperty = "_meshlinkAnnounceGeneration";

// QtDBus has to know every custom payload type before the objects are
// exported, otherwise properties get skipped and GetLayout is not exported.
void registerTrayMetaTypes() {
    static bool registered = false;
    if (registered)
        return;
    registered = true;
    qDBusRegisterMetaType<SniImage>();
    qDBusRegisterMetaType<SniImageList>();
    qDBusRegisterMetaType<SniToolTip>();
    qDBusRegisterMetaType<SniMenuLayoutItem>();
    qDBusRegisterMetaType<SniMenuItem>();
    qDBusRegisterMetaType<SniMenuItemList>();
}

// Render the embedded product icon at the SNI pixmap sizes.  Pixels are
// ARGB32 in network byte order, as required by the StatusNotifierItem spec
// (dde-tray-loader converts each 32-bit word with qFromBigEndian and decodes
// it as QImage::Format_ARGB32).
SniImageList buildIconPixmaps() {
    const QIcon icon = meshlink::meshlinkAppIcon();
    SniImageList images;
    for (int size : kIconSizes) {
        const QImage img = icon.pixmap(QSize(size, size), 1.0)
                               .toImage()
                               .convertToFormat(QImage::Format_ARGB32);
        if (img.isNull() || img.width() <= 0 || img.height() <= 0)
            continue;
        SniImage image;
        image.width = img.width();
        image.height = img.height();
        image.pixels.resize(img.width() * img.height() * 4);
        uchar *out = reinterpret_cast<uchar *>(image.pixels.data());
        for (int y = 0; y < img.height(); ++y) {
            const QRgb *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
            for (int x = 0; x < img.width(); ++x) {
                qToBigEndian<quint32>(static_cast<quint32>(line[x]), out);
                out += 4;
            }
        }
        images.append(image);
    }
    return images;
}

} // namespace

// ---- marshalling ---------------------------------------------------------

QDBusArgument &operator<<(QDBusArgument &arg, const SniImage &image) {
    arg.beginStructure();
    arg << image.width << image.height << image.pixels;
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, SniImage &image) {
    arg.beginStructure();
    arg >> image.width >> image.height >> image.pixels;
    arg.endStructure();
    return arg;
}

QDBusArgument &operator<<(QDBusArgument &arg, const SniToolTip &tip) {
    arg.beginStructure();
    arg << tip.iconName << tip.iconPixmap << tip.title << tip.description;
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, SniToolTip &tip) {
    arg.beginStructure();
    arg >> tip.iconName >> tip.iconPixmap >> tip.title >> tip.description;
    arg.endStructure();
    return arg;
}

// (ia{sv}av): children are variants wrapping nested (ia{sv}av) structs, which
// is what libdbusmenuqt parses (it extracts QDBusArgument from each variant).
QDBusArgument &operator<<(QDBusArgument &arg, const SniMenuLayoutItem &item) {
    arg.beginStructure();
    arg << item.id << item.properties;
    arg.beginArray(qMetaTypeId<QDBusVariant>());
    for (const SniMenuLayoutItem &child : item.children)
        arg << QDBusVariant(QVariant::fromValue(child));
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
        QDBusVariant variant;
        arg >> variant;
        QDBusArgument childArgument = variant.variant().value<QDBusArgument>();
        SniMenuLayoutItem child;
        childArgument >> child;
        item.children.append(child);
    }
    arg.endArray();
    arg.endStructure();
    return arg;
}

QDBusArgument &operator<<(QDBusArgument &arg, const SniMenuItem &item) {
    arg.beginStructure();
    arg << item.id << item.properties;
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, SniMenuItem &item) {
    arg.beginStructure();
    arg >> item.id >> item.properties;
    arg.endStructure();
    return arg;
}

namespace meshlink {

// ---- SniItem ----

SniItem::SniItem(XdgTrayIcon *tray, QObject *parent) : QObject(parent), m_tray(tray) {
    m_toolTip.description = QStringLiteral("deepin-MeshLink");
}

void SniItem::setTitle(const QString &title) {
    if (m_title == title)
        return;
    m_title = title;
    if (!m_toolTipExplicit) {
        m_toolTip.title = title;
        emit NewToolTip();
    }
    emit NewTitle();
}

void SniItem::setIconName(const QString &name) {
    if (m_iconName == name)
        return;
    m_iconName = name;
    emit NewIcon();
}

void SniItem::setIconPixmaps(const SniImageList &icon, const SniImageList &attention,
                             const SniImageList &overlay) {
    m_iconPixmap = icon;
    m_attentionIconPixmap = attention;
    m_overlayIconPixmap = overlay;
    emit NewIcon();
    if (!attention.isEmpty())
        emit NewAttentionIcon();
    if (!overlay.isEmpty())
        emit NewOverlayIcon();
}

void SniItem::setToolTip(const QString &tip) {
    m_toolTipExplicit = true;
    if (m_toolTip.title == tip)
        return;
    m_toolTip.title = tip;
    if (m_toolTip.description.isEmpty())
        m_toolTip.description = QStringLiteral("deepin-MeshLink");
    emit NewToolTip();
}

void SniItem::setStatus(const QString &status) {
    if (m_status == status)
        return;
    m_status = status;
    emit NewStatus(m_status);
}

void SniItem::Activate(int, int) {
    if (m_tray)
        emit m_tray->activateRequested();
}

void SniItem::SecondaryActivate(int, int) {
    if (m_tray)
        emit m_tray->secondaryActivateRequested();
}

void SniItem::ContextMenu(int, int) {
    // The context menu is exported through com.canonical.dbusmenu, so the host
    // only falls back here when the Menu property is unusable.  Nothing to do.
}

void SniItem::ProvideXdgActivationToken(const QString &token) {
    // Activation tokens are only meaningful for Wayland focus stealing; the
    // window is raised by MainWindow::showWindow() after activateRequested.
    Q_UNUSED(token);
}

void SniItem::Scroll(int, const QString &) {}

// ---- SniMenu ----

SniMenu::SniMenu(XdgTrayIcon *tray, QObject *parent) : QObject(parent), m_tray(tray) {}

void SniMenu::setItems(const QVector<QPair<int, QString>> &items) {
    if (m_items == items)
        return;
    m_items = items;
    rebuildDbusIds();
    ++m_revision;
    emit LayoutUpdated(m_revision, 0);
}

void SniMenu::rebuildDbusIds() {
    m_dbusIds.clear();
    m_dbusIds.reserve(m_items.size());
    QSet<int> used;
    for (const auto &entry : m_items) {
        if (entry.first != 0)
            used.insert(entry.first);
    }
    int nextSynthetic = 1;
    for (const auto &entry : m_items) {
        if (entry.first != 0) {
            m_dbusIds.append(entry.first);
            continue;
        }
        // DBusMenu id 0 is the root item; separators get fresh unique ids.
        while (used.contains(nextSynthetic))
            ++nextSynthetic;
        used.insert(nextSynthetic);
        m_dbusIds.append(nextSynthetic);
        ++nextSynthetic;
    }
}

int SniMenu::indexForDbusId(int id) const {
    return static_cast<int>(m_dbusIds.indexOf(id));
}

QVariantMap SniMenu::propertiesFor(int index, const QStringList &propertyNames) const {
    QVariantMap properties;
    if (index < 0 || index >= m_items.size())
        return properties;
    const auto &entry = m_items.at(index);
    if (entry.second.isEmpty()) {
        properties.insert(QStringLiteral("type"), QStringLiteral("separator"));
    } else {
        properties.insert(QStringLiteral("label"), entry.second);
        properties.insert(QStringLiteral("enabled"), true);
    }
    properties.insert(QStringLiteral("visible"), true);
    // This model is flat: no entry ever gets "children-display" = "submenu".
    if (propertyNames.isEmpty())
        return properties;
    QVariantMap filtered;
    for (const QString &name : propertyNames) {
        auto it = properties.constFind(name);
        if (it != properties.constEnd())
            filtered.insert(name, it.value());
    }
    return filtered;
}

void SniMenu::GetLayout(int parentId, int recursionDepth, const QStringList &propertyNames,
                        uint &revision, SniMenuLayoutItem &layout) {
    revision = m_revision;
    layout = SniMenuLayoutItem{};
    layout.id = parentId;
    if (parentId == 0) {
        if (!m_items.isEmpty()
            && (propertyNames.isEmpty() || propertyNames.contains(QStringLiteral("children-display"))))
            layout.properties.insert(QStringLiteral("children-display"), QStringLiteral("submenu"));
        if (recursionDepth == 0)
            return; // no recursion requested: children array stays empty
        for (int i = 0; i < m_items.size(); ++i) {
            SniMenuLayoutItem child;
            child.id = dbusIdAt(i);
            child.properties = propertiesFor(i, propertyNames);
            layout.children.append(child);
        }
        return;
    }
    // parentId != 0: a flat menu has no submenus, but a well-formed reply must
    // still describe the requested item instead of dropping the request.
    const int index = indexForDbusId(parentId);
    if (index >= 0)
        layout.properties = propertiesFor(index, propertyNames);
}

void SniMenu::GetGroupProperties(const QList<int> &ids, const QStringList &propertyNames,
                                 QList<SniMenuItem> &properties) {
    properties.clear();
    if (ids.isEmpty()) {
        for (int i = 0; i < m_items.size(); ++i)
            properties.append(SniMenuItem{dbusIdAt(i), propertiesFor(i, propertyNames)});
        return;
    }
    for (int id : ids) {
        const int index = indexForDbusId(id);
        if (index >= 0)
            properties.append(SniMenuItem{id, propertiesFor(index, propertyNames)});
    }
}

QDBusVariant SniMenu::GetProperty(int id, const QString &property) {
    if (id == 0) {
        if (property == QLatin1String("children-display") && !m_items.isEmpty())
            return QDBusVariant(QVariant(QStringLiteral("submenu")));
        return QDBusVariant(QVariant(QString()));
    }
    const int index = indexForDbusId(id);
    if (index < 0)
        return QDBusVariant(QVariant(QString()));
    const QVariantMap properties = propertiesFor(index, QStringList{property});
    auto it = properties.constFind(property);
    if (it == properties.constEnd())
        return QDBusVariant(QVariant(QString()));
    return QDBusVariant(it.value());
}

bool SniMenu::AboutToShow(int id) {
    // Layout is static while the model does not change; hosts only need a
    // refresh when the item is unknown to us.
    if (id == 0)
        return false;
    return indexForDbusId(id) < 0;
}

void SniMenu::AboutToShowGroup(const QList<int> &ids, QList<int> &updatesNeeded,
                               QList<int> &idErrors) {
    updatesNeeded.clear();
    idErrors.clear();
    for (int id : ids) {
        if (id != 0 && indexForDbusId(id) < 0)
            idErrors.append(id);
    }
}

bool SniMenu::dispatchEvent(int id, const QString &eventId, const QDBusVariant &data,
                            uint timestamp) {
    Q_UNUSED(data);
    Q_UNUSED(timestamp);
    if (id == 0)
        return true; // the DBusMenu root is known, but has nothing to activate
    const int index = indexForDbusId(id);
    if (index < 0)
        return false;
    if (eventId == QLatin1String("clicked")) {
        const int declaredId = m_items.at(index).first;
        if (declaredId != 0 && m_tray)
            emit m_tray->menuTriggered(declaredId);
    }
    return true;
}

void SniMenu::Event(int id, const QString &eventId, const QDBusVariant &data, uint timestamp) {
    dispatchEvent(id, eventId, data, timestamp);
}

void SniMenu::EventGroup(const QList<int> &ids, const QString &eventId, const QDBusVariant &data,
                         uint timestamp, QList<int> &idErrors) {
    idErrors.clear();
    for (int id : ids) {
        if (!dispatchEvent(id, eventId, data, timestamp))
            idErrors.append(id);
    }
}

// ---- XdgTrayIcon ----

XdgTrayIcon::XdgTrayIcon(QObject *parent) : QObject(parent) {
    registerTrayMetaTypes();

    m_item = new SniItem(this, this);
    m_menu = new SniMenu(this, this);
    // Export the embedded product icon so hosts without the hicolor theme
    // entry still render the item (deepin V25 dde-tray-loader falls back to
    // the cached IconPixmap only after QIcon::fromTheme() fails).
    m_item->setIconPixmaps(buildIconPixmaps());

    QDBusConnection bus = QDBusConnection::sessionBus();
    // A watcher can appear after the app (session startup race) or be
    // replaced when dde-shell/dde-daemon restarts: retry on ownership change,
    // not only on PropertiesChanged (which never fires for a missing service).
    bus.connect(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
                QStringLiteral("org.freedesktop.DBus"), QStringLiteral("NameOwnerChanged"), this,
                SLOT(onNameOwnerChanged(QString, QString, QString)));
    // A watcher restart also re-registers its hosts; keep listening for the
    // host-registered notification as a cheap re-registration trigger.
    bus.connect(QString::fromLatin1(kWatcherService), QString::fromLatin1(kWatcherPath),
                QStringLiteral("org.freedesktop.DBus.Properties"),
                QStringLiteral("PropertiesChanged"), this, SLOT(checkWatcher()));

    // Defer the first registration until MainWindow::createTray() has applied
    // setIconName()/setMenuItems()/setTitle(), so the host never caches an
    // empty icon or an empty menu.
    QTimer::singleShot(0, this, &XdgTrayIcon::checkWatcher);
}

XdgTrayIcon::~XdgTrayIcon() {
    unregisterObjects();
    m_available = false;
}

void XdgTrayIcon::setAvailable(bool available) {
    if (m_available == available)
        return;
    m_available = available;
    emit availableChanged(m_available);
}

void XdgTrayIcon::onNameOwnerChanged(const QString &service, const QString &oldOwner,
                                     const QString &newOwner) {
    Q_UNUSED(oldOwner);
    Q_UNUSED(newOwner);
    if (service == QLatin1String(kWatcherService))
        checkWatcher();
}

void XdgTrayIcon::checkWatcher() {
    QDBusConnection bus = QDBusConnection::sessionBus();
    QDBusConnectionInterface *iface = bus.interface();
    if (!bus.isConnected() || !iface) {
        unregisterObjects();
        m_watcherOwner.clear();
        m_announceRetries = 0;
        setAvailable(false);
        return;
    }

    const QString service = QString::fromLatin1(kWatcherService);
    // serviceOwner() returns an empty string when nobody owns the name, which
    // covers both "no panel running" and "watcher just exited".
    const QString owner = iface->serviceOwner(service);
    if (owner.isEmpty()) {
        // No panel running: stop serving and report the item as unavailable.
        unregisterObjects();
        m_watcherOwner.clear();
        m_announced = false;
        m_announceRetries = 0;
        setAvailable(false);
        return;
    }
    if (!m_objectsRegistered || owner != m_watcherOwner) {
        if (!m_objectsRegistered && !registerObjects()) {
            m_watcherOwner.clear();
            m_announced = false;
            m_announceRetries = 0;
            setAvailable(false);
            return;
        }
        // A (re)started watcher knows nothing about the already-exported
        // objects, so announce them again; the retry budget must not be
        // inherited from the previous watcher.
        m_watcherOwner = owner;
        m_announced = false;
        m_announceRetries = 0;
    }
    if (m_announced)
        return; // already announced to this watcher instance
    if (m_announcePending)
        return; // do not register twice concurrently (the watcher rejects that)
    announceToWatcher();
}

bool XdgTrayIcon::registerObjects() {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        qWarning("XdgTrayIcon: cannot register the StatusNotifierItem: session bus is not connected");
        return false;
    }
    if (!bus.registerObject(QString::fromLatin1(kMenuPath), m_menu,
                            QDBusConnection::ExportAllContents)) {
        qWarning("XdgTrayIcon: failed to register the DBusMenu object at %s", kMenuPath);
        return false;
    }
    if (!bus.registerObject(QString::fromLatin1(kItemPath), m_item,
                            QDBusConnection::ExportAllContents)) {
        qWarning("XdgTrayIcon: failed to register the StatusNotifierItem object at %s", kItemPath);
        bus.unregisterObject(QString::fromLatin1(kMenuPath));
        return false;
    }
    m_objectsRegistered = true;
    return true;
}

void XdgTrayIcon::unregisterObjects() {
    if (!m_objectsRegistered)
        return;
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.unregisterObject(QString::fromLatin1(kMenuPath));
    bus.unregisterObject(QString::fromLatin1(kItemPath));
    m_objectsRegistered = false;
    m_announced = false;
    m_announceRetries = 0;
}

void XdgTrayIcon::announceToWatcher() {
    if (!m_objectsRegistered || m_announcePending)
        return;
    m_announcePending = true;
    // Remember which watcher instance this attempt targets; a reply from a
    // watcher that has been replaced meanwhile says nothing about the current
    // owner, so it must not be treated as an accepted registration.
    const QString owner = m_watcherOwner;
    const quint64 generation = property(kAnnounceGenerationProperty).toULongLong() + 1;
    setProperty(kAnnounceGenerationProperty, QVariant::fromValue(generation));
    QDBusMessage call = QDBusMessage::createMethodCall(
        QString::fromLatin1(kWatcherService), QString::fromLatin1(kWatcherPath),
        QString::fromLatin1(kWatcherInterface), QStringLiteral("RegisterStatusNotifierItem"));
    // Object path form: the watcher pairs it with this connection's unique name.
    call << QString::fromLatin1(kItemPath);
    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(call, 5000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, owner, generation](QDBusPendingCallWatcher *callWatcher) {
                callWatcher->deleteLater();
                // Keep m_announcePending set while the (blocking) owner check
                // below runs, so a re-entrant checkWatcher() cannot start a
                // second registration.
                bool stale = property(kAnnounceGenerationProperty).toULongLong() != generation
                    || !m_objectsRegistered || m_watcherOwner != owner;
                if (!stale) {
                    // If feasible, also ask the bus: the owner notification may
                    // have been missed, and a reply from a watcher that no
                    // longer owns the name proves nothing.
                    QDBusConnection bus = QDBusConnection::sessionBus();
                    QDBusConnectionInterface *iface = bus.interface();
                    const QString liveOwner = iface
                        ? iface->serviceOwner(QString::fromLatin1(kWatcherService))
                        : QString();
                    stale = liveOwner.isEmpty() || liveOwner != owner || !m_objectsRegistered
                        || m_watcherOwner != owner;
                }
                m_announcePending = false;
                const QDBusPendingReply<> reply = *callWatcher;

                if (stale) {
                    // Late reply from a previous watcher instance: the current
                    // watcher has not accepted anything, so stay unavailable
                    // and let checkWatcher() announce to the live owner.
                    m_announced = false;
                    setAvailable(false);
                    QTimer::singleShot(0, this, &XdgTrayIcon::checkWatcher);
                    return;
                }
                if (reply.isError()) {
                    qWarning().noquote()
                        << "XdgTrayIcon: RegisterStatusNotifierItem failed:"
                        << reply.error().name() << reply.error().message();
                    m_announced = false;
                    setAvailable(false);
                    // The watcher may have been replaced while the call was in
                    // flight; retry a few times in case that event was missed.
                    if (m_announceRetries < 3) {
                        ++m_announceRetries;
                        QTimer::singleShot(2000, this, &XdgTrayIcon::checkWatcher);
                    }
                    return;
                }
                m_announceRetries = 0;
                m_announced = true;
                setAvailable(true);
            });
}

void XdgTrayIcon::setTitle(const QString &title) { m_item->setTitle(title); }

void XdgTrayIcon::setToolTip(const QString &tip) { m_item->setToolTip(tip); }

void XdgTrayIcon::setIconName(const QString &name) { m_item->setIconName(name); }

void XdgTrayIcon::setMenuItems(const QVector<QPair<int, QString>> &items) {
    m_menu->setItems(items);
}

} // namespace meshlink
