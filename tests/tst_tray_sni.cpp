// deepin-MeshLink — independent regression test for the XDG StatusNotifierItem
// tray icon (defect 1: invisible icon, missing right-click DBusMenu).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
//
// The test is fully self-contained: it starts a private session bus
// (/usr/bin/dbus-daemon from the system, not the cross-toolchain copy), exports
// a mock org.kde.StatusNotifierWatcher on it, and then exercises the exported
// public D-Bus contract of meshlink::XdgTrayIcon:
//   * org.kde.StatusNotifierItem: IconPixmap / Menu / IconName properties,
//     Activate, SecondaryActivate, ContextMenu
//   * com.canonical.dbusmenu: GetLayout (ia{sv}av), GetGroupProperties (a(ia{sv})),
//     GetProperty, Event("clicked")
//   * watcher restart -> NameOwnerChanged -> re-registration
//
// Nothing here inspects the C++ implementation; all assertions go through the
// same wire formats a real dde-tray-loader host consumes.
#include "ui/appicon.h"
#include "ui/xdgtrayicon.h"

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QImage>
#include <QThread>
#include <QtEndian>
#include <QTimer>

#include <cstdio>
#include <cstdlib>
#include <functional>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const QString &what) {
    ++g_checks;
    if (ok) {
        std::printf("check: PASS  %s\n", qUtf8Printable(what));
        return;
    }
    ++g_failures;
    std::fprintf(stderr, "check: FAIL  %s\n", qUtf8Printable(what));
}

bool waitFor(const std::function<bool()> &predicate, int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (predicate())
            return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return predicate();
}

void processFor(int durationMs) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < durationMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
}

// A blocking bus.call() to an object exported by the same connection can
// deadlock (the serving side would need the blocked event loop). All calls from
// this test are therefore asynchronous and pumped.
QDBusMessage call(const QString &path, const QString &interface, const QString &method,
                  const QVariantList &arguments = {}, const QString &destination = QString()) {
    QDBusConnection bus = QDBusConnection::sessionBus();
    QDBusMessage message = QDBusMessage::createMethodCall(
        destination.isEmpty() ? bus.baseService() : destination, path, interface, method);
    for (const QVariant &argument : arguments)
        message << argument;
    QDBusPendingCall pending = bus.asyncCall(message, 5000);
    QDBusPendingCallWatcher watcher(pending);
    if (!watcher.isFinished()) {
        QEventLoop loop;
        QObject::connect(&watcher, &QDBusPendingCallWatcher::finished, &loop, &QEventLoop::quit);
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return watcher.reply();
}

bool isOk(const QDBusMessage &reply, const QString &what) {
    if (reply.type() == QDBusMessage::ErrorMessage) {
        check(false, QStringLiteral("%1 (DBus error %2: %3)")
                         .arg(what, reply.errorName(), reply.errorMessage()));
        return false;
    }
    return true;
}

// Mock StatusNotifierWatcher: records every item announcement and exposes the
// two canonical read-only properties.
class MockWatcher : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.StatusNotifierWatcher")
    Q_PROPERTY(bool IsStatusNotifierHostRegistered READ hostRegistered CONSTANT)
    Q_PROPERTY(QStringList RegisteredStatusNotifierItems READ registeredItems CONSTANT)

public:
    explicit MockWatcher(const QDBusConnection &connection = QDBusConnection::sessionBus(),
                         bool delayedReply = false, QObject *parent = nullptr)
        : QObject(parent), m_connection(connection), m_delayed(delayedReply) {}

    bool hostRegistered() const { return true; }
    QStringList registeredItems() const { return m_items; }
    int registrations() const { return m_registrations; } // slot entered
    int accepted() const { return m_accepted; }           // reply actually sent
    bool hasPendingReply() const
    {
        return m_pendingReply.type() != QDBusMessage::InvalidMessage;
    }

    // A delayed watcher holds the RegisterStatusNotifierItem reply until the
    // test releases it, which is how a watcher that is replaced mid-call is
    // simulated deterministically.
    void releasePendingReply()
    {
        if (!hasPendingReply())
            return;
        const QDBusMessage reply = m_pendingReply.createReply();
        m_pendingReply = QDBusMessage();
        m_connection.send(reply);
        ++m_accepted;
        std::printf("mock-watcher: released delayed Register reply\n");
    }

public slots:
    void RegisterStatusNotifierItem(const QString &service) {
        ++m_registrations;
        m_items.append(service);
        m_lastService = service;
        std::printf("mock-watcher: RegisterStatusNotifierItem('%s')%s\n",
                    qUtf8Printable(service), m_delayed ? " [reply delayed]" : "");
        if (m_delayed) {
            setDelayedReply(true);
            m_pendingReply = message();
            return;
        }
        ++m_accepted;
    }

private:
    QDBusConnection m_connection;
    bool m_delayed = false;
    int m_registrations = 0;
    int m_accepted = 0;
    QStringList m_items;
    QString m_lastService;
    QDBusMessage m_pendingReply;
};

struct BusDaemon {
    // The private bus must exist before QGuiApplication is constructed: Qt's
    // platform theme initialises QDBusConnection::sessionBus() during
    // application setup, which would otherwise bind the test to the desktop
    // session bus. A plain fork/exec keeps this dependency-free.
    pid_t pid = -1;
    QByteArray address;

    bool start() {
        int pipeFds[2];
        if (::pipe(pipeFds) != 0)
            return false;
        pid = ::fork();
        if (pid < 0) {
            ::close(pipeFds[0]);
            ::close(pipeFds[1]);
            return false;
        }
        if (pid == 0) {
            ::close(pipeFds[0]);
            ::dup2(pipeFds[1], STDOUT_FILENO);
            ::close(pipeFds[1]);
            // The system daemon must load the system libdbus, not the cross
            // toolchain libraries inherited by the test binary.
            ::unsetenv("LD_LIBRARY_PATH");
            ::unsetenv("LD_PRELOAD");
            ::unsetenv("DBUS_SESSION_BUS_ADDRESS");
            ::execl("/usr/bin/dbus-daemon", "dbus-daemon", "--session", "--nofork",
                    "--print-address=1", static_cast<char *>(nullptr));
            ::_exit(127);
        }

        ::close(pipeFds[1]);
        QByteArray output;
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 5000 && !output.contains('\n')) {
            struct pollfd pollFd;
            pollFd.fd = pipeFds[0];
            pollFd.events = POLLIN;
            pollFd.revents = 0;
            const int ready = ::poll(&pollFd, 1, 200);
            if (ready > 0) {
                char buffer[512];
                const ssize_t n = ::read(pipeFds[0], buffer, sizeof(buffer));
                if (n > 0)
                    output.append(buffer, int(n));
                else if (n == 0)
                    break;
            }
        }
        ::close(pipeFds[0]);
        address = output.split('\n').first().trimmed();
        if (!address.startsWith("unix:")) {
            std::fprintf(stderr, "tst_tray_sni: dbus-daemon gave no address; output='%s'\n",
                         output.constData());
            return false;
        }
        qputenv("DBUS_SESSION_BUS_ADDRESS", address);
        std::printf("private-session-bus: %s\n", address.constData());
        return true;
    }

    ~BusDaemon() {
        if (pid <= 0)
            return;
        ::kill(pid, SIGTERM);
        int status = 0;
        ::waitpid(pid, &status, 0);
    }
};

} // namespace

int main(int argc, char *argv[]) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // Start the private session bus and point DBUS_SESSION_BUS_ADDRESS at it
    // before QGuiApplication can initialise the default bus connection.
    BusDaemon daemon;
    if (!daemon.start()) {
        std::fprintf(stderr, "tst_tray_sni: FAILED: could not start the private session bus\n");
        return 1;
    }

    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("HuanSoft"));
    app.setApplicationName(QStringLiteral("tst_tray_sni"));

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        std::fprintf(stderr, "tst_tray_sni: FAILED: session bus not connected: %s\n",
                     qUtf8Printable(bus.lastError().message()));
        return 1;
    }
    check(bus.isConnected(), "private session bus is connected");
    std::printf("private-session-bus unique name: %s (env=%s)\n",
                qUtf8Printable(bus.baseService()),
                qgetenv("DBUS_SESSION_BUS_ADDRESS").constData());

    MockWatcher watcher;
    check(bus.registerObject(QStringLiteral("/StatusNotifierWatcher"), &watcher,
                             QDBusConnection::ExportAllContents),
          "mock watcher object exported at /StatusNotifierWatcher");
    const bool serviceOk =
        bus.registerService(QStringLiteral("org.kde.StatusNotifierWatcher"));
    if (!serviceOk)
        std::fprintf(stderr, "registerService error: %s: %s\n",
                     qUtf8Printable(bus.lastError().name()),
                     qUtf8Printable(bus.lastError().message()));
    check(serviceOk, "mock watcher service org.kde.StatusNotifierWatcher acquired");

    meshlink::XdgTrayIcon tray;
    tray.setTitle(QStringLiteral("deepin-MeshLink"));
    tray.setIconName(QStringLiteral("cn.org.huansoft.deepin-meshlink"));
    tray.setMenuItems({
        {1, QStringLiteral("Show main window")},
        {2, QStringLiteral("Settings")},
        {0, QString()}, // separator
        {3, QStringLiteral("Quit")},
    });

    check(waitFor([&] { return watcher.registrations() >= 1 && tray.available(); }, 7000),
          "tray item announced itself to the mock watcher and reported available()");
    check(watcher.registeredItems().contains(QStringLiteral("/StatusNotifierItem")),
          "watcher received the /StatusNotifierItem object path");

    const QString itemPath = QStringLiteral("/StatusNotifierItem");
    const QString itemInterface = QStringLiteral("org.kde.StatusNotifierItem");
    const QString menuPath = QStringLiteral("/MenuBar");
    const QString menuInterface = QStringLiteral("com.canonical.dbusmenu");

    // ---- SNI properties: non-empty IconPixmap + Menu path --------------------
    {
        QDBusMessage reply = call(itemPath, QStringLiteral("org.freedesktop.DBus.Properties"),
                                  QStringLiteral("GetAll"), {itemInterface});
        if (isOk(reply, QStringLiteral("Properties.GetAll(org.kde.StatusNotifierItem)"))) {
            check(reply.arguments().size() == 1, "GetAll returned one a{sv} argument");
            const QVariantMap properties = qdbus_cast<QVariantMap>(reply.arguments().value(0));

            const QString iconName = properties.value(QStringLiteral("IconName")).toString();
            check(!iconName.isEmpty(), "IconName property is non-empty");
            check(iconName == QStringLiteral("cn.org.huansoft.deepin-meshlink"),
                  "IconName property preserves the configured theme name");

            const QVariant pixmapValue = properties.value(QStringLiteral("IconPixmap"));
            check(pixmapValue.isValid(), "IconPixmap property is exported");
            const QDBusArgument pixmapArgument = qvariant_cast<QDBusArgument>(pixmapValue);
            check(pixmapArgument.currentSignature() == QStringLiteral("a(iiay)"),
                  QStringLiteral("IconPixmap signature is a(iiay) (got %1)")
                      .arg(pixmapArgument.currentSignature()));

            int imageCount = 0;
            bool everySizeValid = true;
            bool everyPixelArrayValid = true;
            int firstWidth = 0;
            int firstHeight = 0;
            int firstBytes = 0;
            QByteArray firstPixels;
            pixmapArgument.beginArray();
            while (!pixmapArgument.atEnd()) {
                pixmapArgument.beginStructure();
                int width = 0;
                int height = 0;
                QByteArray pixels;
                pixmapArgument >> width >> height >> pixels;
                pixmapArgument.endStructure();
                if (imageCount == 0) {
                    firstWidth = width;
                    firstHeight = height;
                    firstBytes = int(pixels.size());
                    firstPixels = pixels;
                }
                ++imageCount;
                if (width <= 0 || height <= 0 || width > 256 || height > 256)
                    everySizeValid = false;
                if (pixels.size() != width * height * 4 || pixels.isEmpty())
                    everyPixelArrayValid = false;
            }
            pixmapArgument.endArray();
            check(imageCount > 0, "IconPixmap contains at least one image");
            check(everySizeValid, "every IconPixmap image has sane width/height");
            check(everyPixelArrayValid,
                  "every IconPixmap image has width*height*4 ARGB32 bytes");
            std::printf("IconPixmap: %d image(s), first=%dx%d (%d bytes)\n", imageCount,
                        firstWidth, firstHeight, firstBytes);
            // Decode the first image exactly as a host must (big-endian ARGB32)
            // and compare it with the product icon rendered by Qt. This proves
            // the network byte order required by the SNI spec, not just sizes.
            QImage decoded(firstWidth, firstHeight, QImage::Format_ARGB32);
            for (int y = 0; y < firstHeight; ++y) {
                auto *line = reinterpret_cast<QRgb *>(decoded.scanLine(y));
                for (int x = 0; x < firstWidth; ++x) {
                    line[x] = qFromBigEndian<quint32>(
                        reinterpret_cast<const uchar *>(firstPixels.constData())
                        + (y * firstWidth + x) * 4);
                }
            }
            const QImage expected =
                meshlink::meshlinkAppIcon()
                    .pixmap(QSize(firstWidth, firstHeight), 1.0)
                    .toImage()
                    .convertToFormat(QImage::Format_ARGB32);
            check(!expected.isNull() && decoded == expected,
                  "IconPixmap pixels are network-byte-order ARGB32 of the product icon");

            const QVariant menuValue = properties.value(QStringLiteral("Menu"));
            check(menuValue.canConvert<QDBusObjectPath>(),
                  "Menu property is a D-Bus object path");
            const QString menuObjectPath = menuValue.value<QDBusObjectPath>().path();
            check(menuObjectPath == menuPath,
                  QStringLiteral("Menu property points at %1 (got '%2')")
                      .arg(menuPath, menuObjectPath));

            const QVariant itemIsMenu = properties.value(QStringLiteral("ItemIsMenu"));
            check(itemIsMenu.isValid() && !itemIsMenu.toBool(),
                  "ItemIsMenu is exported as false");

            const QVariant toolTipValue = properties.value(QStringLiteral("ToolTip"));
            check(toolTipValue.isValid(), "ToolTip property is exported");
            const SniToolTip toolTip = qdbus_cast<SniToolTip>(toolTipValue);
            check(!toolTip.title.isEmpty() && !toolTip.description.isEmpty(),
                  QStringLiteral("ToolTip carries a title and description (got '%1' / '%2')")
                      .arg(toolTip.title, toolTip.description));

            QDBusMessage introspect =
                call(itemPath, QStringLiteral("org.freedesktop.DBus.Introspectable"),
                     QStringLiteral("Introspect"));
            if (isOk(introspect, QStringLiteral("StatusNotifierItem Introspect"))) {
                const QString xml = introspect.arguments().value(0).toString();
                for (const char *method : {"Activate", "SecondaryActivate", "ContextMenu",
                                           "Scroll", "ProvideXdgActivationToken"})
                    check(xml.contains(QString::fromLatin1(method)),
                          QStringLiteral("SNI introspection exports %1")
                              .arg(QString::fromLatin1(method)));
                for (const char *signal : {"NewTitle", "NewIcon", "NewMenu", "NewToolTip",
                                           "NewAttentionIcon", "NewOverlayIcon", "NewStatus"})
                    check(xml.contains(QString::fromLatin1(signal)),
                          QStringLiteral("SNI introspection exports signal %1")
                              .arg(QString::fromLatin1(signal)));
                check(xml.contains(QStringLiteral("ToolTip")),
                      "SNI introspection exports the ToolTip property");
            }

            const QVariant titleValue = properties.value(QStringLiteral("Title"));
            check(titleValue.toString() == QStringLiteral("deepin-MeshLink"),
                  "Title property reflects setTitle()");
        }
    }

    // ---- com.canonical.dbusmenu GetLayout -----------------------------------
    int firstEntryId = -1;
    {
        QDBusMessage request = QDBusMessage::createMethodCall(
            bus.baseService(), menuPath, menuInterface, QStringLiteral("GetLayout"));
        request << 0 << 1 << QStringList();
        QDBusPendingCall pending = bus.asyncCall(request, 5000);
        QDBusPendingReply<uint, SniMenuLayoutItem> reply(pending);
        QDBusPendingCallWatcher watcher(pending);
        if (!watcher.isFinished()) {
            QEventLoop loop;
            QObject::connect(&watcher, &QDBusPendingCallWatcher::finished, &loop,
                             &QEventLoop::quit);
            QTimer::singleShot(5000, &loop, &QEventLoop::quit);
            loop.exec();
        }
        if (isOk(watcher.reply(), QStringLiteral("com.canonical.dbusmenu.GetLayout(0,1,[])"))) {
            check(reply.isValid(), "GetLayout typed reply decodes as (uint, ihsvav-layout)");
            check(watcher.reply().signature() == QStringLiteral("u(ia{sv}av)"),
                  QStringLiteral("GetLayout reply signature is u(ia{sv}av) (got '%1')")
                      .arg(watcher.reply().signature()));
            check(reply.count() == 2, "GetLayout reply has revision and layout arguments");

            const uint revision = reply.argumentAt<0>();
            check(revision >= 1, "GetLayout revision is a positive counter");
            const SniMenuLayoutItem root = reply.argumentAt<1>();
            check(root.id == 0, "GetLayout root item id is the DBusMenu root id 0");
            check(root.properties.value(QStringLiteral("children-display")).toString()
                      == QStringLiteral("submenu"),
                  "GetLayout root advertises children-display=submenu");

            const QVector<SniMenuLayoutItem> children = root.children;
            check(children.size() == 4,
                  QStringLiteral("GetLayout returns exactly 4 app entries (got %1)")
                      .arg(children.size()));
            QStringList labels;
            int separators = 0;
            for (const SniMenuLayoutItem &child : children) {
                if (child.properties.value(QStringLiteral("type")).toString()
                    == QStringLiteral("separator")) {
                    ++separators;
                } else {
                    labels << child.properties.value(QStringLiteral("label")).toString();
                    if (firstEntryId < 0)
                        firstEntryId = int(child.id);
                }
                check(child.children.isEmpty(), "flat app menu entries have no submenus");
            }
            check(labels
                      == QStringList({QStringLiteral("Show main window"),
                                      QStringLiteral("Settings"), QStringLiteral("Quit")}),
                  QStringLiteral("GetLayout labels are the 3 app actions (got '%1')")
                      .arg(labels.join(QStringLiteral(" | "))));
            check(separators == 1, "GetLayout contains the one separator");
            check(firstEntryId > 0, "menu entry ids never use the DBusMenu root id 0");
        }

        // GetLayout must also serve a non-root parentId (hosts call this when
        // opening a submenu); the answer is the requested item, not the root.
        const QDBusMessage parentRaw =
            call(menuPath, menuInterface, QStringLiteral("GetLayout"), {3, 1, QStringList()});
        QDBusPendingReply<uint, SniMenuLayoutItem> parentReply(parentRaw);
        check(parentReply.isValid(),
              "GetLayout(parentId=3) decodes as (uint, ihsvav-layout)");
        if (parentReply.isValid()) {
            const SniMenuLayoutItem parentItem = parentReply.argumentAt<1>();
            check(parentItem.id == 3,
                  QStringLiteral("GetLayout(parentId=3) returns item id 3 (got %1)")
                      .arg(parentItem.id));
            check(parentItem.children.isEmpty(),
                  "GetLayout(parentId=3) returns no children for a leaf action");
        }
    }

    // ---- remaining canonical DBusMenu calls used by tray hosts --------------
    {
        QDBusMessage about = call(menuPath, menuInterface, QStringLiteral("AboutToShow"), {0});
        check(isOk(about, QStringLiteral("com.canonical.dbusmenu.AboutToShow(0)")),
              "AboutToShow is callable");
        check(about.signature() == QStringLiteral("b"),
              QStringLiteral("AboutToShow returns a boolean (got '%1')").arg(about.signature()));

        QDBusMessage group = call(menuPath, menuInterface, QStringLiteral("AboutToShowGroup"),
                                  {QVariant::fromValue(QList<int>({1, 2, 3}))});
        check(isOk(group, QStringLiteral("com.canonical.dbusmenu.AboutToShowGroup([1,2,3])")),
              "AboutToShowGroup is callable");
        check(group.signature() == QStringLiteral("aiai"),
              QStringLiteral("AboutToShowGroup returns aiai (got '%1')").arg(group.signature()));
        check(group.arguments().size() == 2,
              "AboutToShowGroup returns the updates and idErrors arrays");

        int eventGroupTriggered = -1;
        int eventGroupCount = 0;
        QList<int> eventGroupIds;
        QObject::connect(&tray, &meshlink::XdgTrayIcon::menuTriggered,
                         [&](int id) {
                             eventGroupTriggered = id;
                             ++eventGroupCount;
                             eventGroupIds << id;
                         });
        QDBusMessage eventGroup = call(menuPath, menuInterface, QStringLiteral("EventGroup"),
                                       {QVariant::fromValue(QList<int>({1, 99})),
                                        QStringLiteral("clicked"),
                                        QVariant::fromValue(QDBusVariant(QVariant(0))), uint(0)});
        check(isOk(eventGroup, QStringLiteral("com.canonical.dbusmenu.EventGroup([1,99],clicked,v,0)")),
              "EventGroup is callable");
        check(eventGroup.signature() == QStringLiteral("ai"),
              QStringLiteral("EventGroup returns the failed ids as ai (got '%1')")
                  .arg(eventGroup.signature()));
        QList<int> eventGroupErrors;
        if (!eventGroup.arguments().isEmpty())
            eventGroupErrors = qdbus_cast<QList<int>>(eventGroup.arguments().constFirst());
        check(eventGroupErrors == QList<int>({99}),
              QStringLiteral("EventGroup reports exactly the unknown id 99 as failed (got %1)")
                  .arg(eventGroupErrors.size()));
        check(waitFor([&] { return eventGroupCount >= 1; }, 3000),
              "EventGroup('clicked') dispatched menuTriggered(1)");
        check(eventGroupCount == 1 && eventGroupIds == QList<int>({1}),
              QStringLiteral("EventGroup('clicked') dispatched menuTriggered exactly once "
                             "(got %1, ids %2)")
                  .arg(eventGroupCount)
                  .arg(eventGroupIds.size()));
    }

    // ---- GetGroupProperties: the previously broken (ai, as) signature --------
    {
        QDBusMessage reply = call(menuPath, menuInterface, QStringLiteral("GetGroupProperties"),
                                  QVariantList{QVariant::fromValue(QList<int>({1, 2, 3})),
                                                QVariant::fromValue(QStringList(QStringLiteral("label")))});
        if (isOk(reply, QStringLiteral("com.canonical.dbusmenu.GetGroupProperties(ai,as)"))) {
            check(reply.signature() == QStringLiteral("a(ia{sv})"),
                  QStringLiteral("GetGroupProperties reply signature is a(ia{sv}) (got '%1')")
                      .arg(reply.signature()));
            const QDBusArgument argument = qvariant_cast<QDBusArgument>(reply.arguments().value(0));
            QMap<int, QString> labels;
            argument.beginArray();
            while (!argument.atEnd()) {
                argument.beginStructure();
                int id = -1;
                QVariantMap properties;
                argument >> id >> properties;
                argument.endStructure();
                labels.insert(id, properties.value(QStringLiteral("label")).toString());
            }
            argument.endArray();
            check(labels.size() == 3,
                  QStringLiteral("GetGroupProperties returned a{sv} properties for 3 ids (got %1)")
                      .arg(labels.size()));
            check(labels.value(1) == QStringLiteral("Show main window"),
                  "GetGroupProperties id 1 label matches");
            check(labels.value(2) == QStringLiteral("Settings"),
                  "GetGroupProperties id 2 label matches");
            check(labels.value(3) == QStringLiteral("Quit"),
                  "GetGroupProperties id 3 label matches");
        }

        // Empty id list is the canonical "all properties" request.
        QDBusMessage allReply = call(menuPath, menuInterface,
                                     QStringLiteral("GetGroupProperties"),
                                     QVariantList{QVariant::fromValue(QList<int>()),
                                                QVariant::fromValue(QStringList())});
        if (isOk(allReply, QStringLiteral("GetGroupProperties([],[])"))) {
            const QDBusArgument argument =
                qvariant_cast<QDBusArgument>(allReply.arguments().value(0));
            int count = 0;
            argument.beginArray();
            while (!argument.atEnd()) {
                argument.beginStructure();
                int id = -1;
                QVariantMap properties;
                argument >> id >> properties;
                argument.endStructure();
                check(properties.contains(QStringLiteral("visible")),
                      "every GetGroupProperties entry has a visible property");
                ++count;
            }
            argument.endArray();
            check(count == 4, "GetGroupProperties([],[]) returns all 4 entries");
        }
    }

    // ---- GetProperty ---------------------------------------------------------
    {
        QDBusMessage reply =
            call(menuPath, menuInterface, QStringLiteral("GetProperty"),
                 {1, QStringLiteral("label")});
        if (isOk(reply, QStringLiteral("com.canonical.dbusmenu.GetProperty(1,label)"))) {
            check(reply.signature() == QStringLiteral("v"), "GetProperty returns a variant");
            const QDBusVariant value =
                qvariant_cast<QDBusVariant>(reply.arguments().value(0));
            check(value.variant().toString() == QStringLiteral("Show main window"),
                  "GetProperty(1,'label') is the first app entry");
        }
    }

    // ---- Event("clicked") dispatches menuTriggered(id) -----------------------
    {
        int triggeredId = -1;
        QObject::connect(&tray, &meshlink::XdgTrayIcon::menuTriggered,
                         [&](int id) { triggeredId = id; });
        QDBusMessage reply = call(menuPath, menuInterface, QStringLiteral("Event"),
                                  {1, QStringLiteral("clicked"),
                                   QVariant::fromValue(QDBusVariant(QVariant(0))), uint(0)});
        check(isOk(reply, QStringLiteral("com.canonical.dbusmenu.Event(1,'clicked',v,0)")),
              "Event('clicked') call is accepted");
        check(waitFor([&] { return triggeredId == 1; }, 3000),
              QStringLiteral("Event('clicked') on id 1 emitted menuTriggered(1) (got %1)")
                  .arg(triggeredId));
    }

    // ---- SNI activation methods ---------------------------------------------
    {
        int activateCount = 0;
        int secondaryCount = 0;
        QObject::connect(&tray, &meshlink::XdgTrayIcon::activateRequested,
                         [&] { ++activateCount; });
        QObject::connect(&tray, &meshlink::XdgTrayIcon::secondaryActivateRequested,
                         [&] { ++secondaryCount; });

        QDBusMessage activate =
            call(itemPath, itemInterface, QStringLiteral("Activate"), {0, 0});
        check(isOk(activate, QStringLiteral("org.kde.StatusNotifierItem.Activate")),
              "Activate is callable");
        check(waitFor([&] { return activateCount == 1; }, 3000),
              "Activate emitted activateRequested");

        QDBusMessage secondary =
            call(itemPath, itemInterface, QStringLiteral("SecondaryActivate"), {0, 0});
        check(isOk(secondary, QStringLiteral("org.kde.StatusNotifierItem.SecondaryActivate")),
              "SecondaryActivate is callable");
        check(waitFor([&] { return secondaryCount == 1; }, 3000),
              "SecondaryActivate emitted secondaryActivateRequested");

        QDBusMessage context =
            call(itemPath, itemInterface, QStringLiteral("ContextMenu"), {0, 0});
        check(isOk(context, QStringLiteral("org.kde.StatusNotifierItem.ContextMenu")),
              "ContextMenu is callable");

        QDBusMessage scroll = call(itemPath, itemInterface, QStringLiteral("Scroll"),
                                   {0, QStringLiteral("vertical")});
        check(isOk(scroll, QStringLiteral("org.kde.StatusNotifierItem.Scroll")),
              "Scroll is callable");

        QDBusMessage token = call(itemPath, itemInterface,
                                  QStringLiteral("ProvideXdgActivationToken"),
                                  {QStringLiteral("verifier-token")});
        check(isOk(token, QStringLiteral("org.kde.StatusNotifierItem.ProvideXdgActivationToken")),
              "ProvideXdgActivationToken is callable");
    }

    // ---- watcher restart: NameOwnerChanged -> re-registration ----------------
    {
        const int before = watcher.registrations();
        bus.unregisterObject(QStringLiteral("/StatusNotifierWatcher"));
        check(true, "mock watcher object removed (simulated host crash)");
        check(bus.unregisterService(QStringLiteral("org.kde.StatusNotifierWatcher")),
              "mock watcher service released (simulated host crash)");
        check(waitFor([&] { return !tray.available(); }, 7000),
              "tray reports unavailable after the watcher disappeared");

        check(bus.registerObject(QStringLiteral("/StatusNotifierWatcher"), &watcher,
                                 QDBusConnection::ExportAllContents),
              "mock watcher object re-exported");
        check(bus.registerService(QStringLiteral("org.kde.StatusNotifierWatcher")),
              "mock watcher service re-acquired");
        const bool reannounced =
            waitFor([&] { return watcher.registrations() > before && tray.available(); }, 9000);
        const int after = watcher.registrations();
        check(reannounced,
              QStringLiteral("restarted watcher received a fresh announcement (%1 -> %2)")
                  .arg(before)
                  .arg(after));
        check(after > before, "registration counter increased after the restart");
        check(watcher.registeredItems().size() >= 2,
              "both watcher instances received the item path");

        QDBusMessage properties = call(itemPath, QStringLiteral("org.freedesktop.DBus.Properties"),
                                       QStringLiteral("GetAll"), {itemInterface});
        check(isOk(properties, "Properties.GetAll after watcher restart"),
              "item object is exported again after the watcher restart");

        QDBusMessage layout = call(menuPath, menuInterface, QStringLiteral("GetLayout"),
                                   {0, 1, QStringList()});
        check(isOk(layout, "com.canonical.dbusmenu.GetLayout after watcher restart"),
              "menu object is exported again after the watcher restart");
    }

    // ---- stale RegisterStatusNotifierItem reply from a replaced watcher -----
    // Regression for the race where watcher A is replaced while its async
    // registration is still in flight: A's late success must not mark the item
    // available; only watcher B accepting the re-announcement may do that.
    {
        bus.unregisterObject(QStringLiteral("/StatusNotifierWatcher"));
        bus.unregisterService(QStringLiteral("org.kde.StatusNotifierWatcher"));
        check(waitFor([&] { return !tray.available(); }, 7000),
              "tray unavailable before the race scenario starts");

        QDBusConnection watcherBusA =
            QDBusConnection::connectToBus(QDBusConnection::SessionBus,
                                          QStringLiteral("meshlink-watcher-a"));
        QDBusConnection watcherBusB =
            QDBusConnection::connectToBus(QDBusConnection::SessionBus,
                                          QStringLiteral("meshlink-watcher-b"));
        check(watcherBusA.isConnected() && watcherBusB.isConnected(),
              "two independent mock-watcher bus connections are connected");
        std::printf("race: watcher A=%s watcher B=%s\n",
                    qUtf8Printable(watcherBusA.baseService()),
                    qUtf8Printable(watcherBusB.baseService()));

        MockWatcher watcherA(watcherBusA, /*delayedReply=*/true);
        check(watcherBusA.registerObject(QStringLiteral("/StatusNotifierWatcher"), &watcherA,
                                         QDBusConnection::ExportAllContents),
              "delayed watcher A object exported");
        check(watcherBusA.registerService(QStringLiteral("org.kde.StatusNotifierWatcher")),
              "delayed watcher A owns the watcher service");
        check(waitFor([&] { return watcherA.registrations() >= 1
                                   && watcherA.hasPendingReply(); }, 9000),
              "tray called RegisterStatusNotifierItem on watcher A");
        check(!tray.available(),
              "delayed watcher A reply keeps the item unavailable");

        // Replace A by B before A answers: in the field this is dde-shell
        // restarting while the registration call is in flight.
        watcherBusA.unregisterObject(QStringLiteral("/StatusNotifierWatcher"));
        watcherBusA.unregisterService(QStringLiteral("org.kde.StatusNotifierWatcher"));
        MockWatcher watcherB(watcherBusB, /*delayedReply=*/true);
        check(watcherBusB.registerObject(QStringLiteral("/StatusNotifierWatcher"), &watcherB,
                                         QDBusConnection::ExportAllContents),
              "replacement watcher B object exported");
        check(watcherBusB.registerService(QStringLiteral("org.kde.StatusNotifierWatcher")),
              "replacement watcher B owns the watcher service");
        processFor(400);
        check(watcherB.registrations() == 0,
              "no duplicate announcement while A's registration is still in flight");
        check(!tray.available(), "item stays unavailable while A's call is in flight");

        // Deliver A's late success. It belongs to the dead watcher and must be
        // discarded; the item then re-announces to B.
        watcherA.releasePendingReply();
        check(waitFor([&] { return watcherB.registrations() >= 1; }, 9000),
              "item re-announced to watcher B after A's stale reply");
        check(watcherB.registrations() == 1,
              QStringLiteral("watcher B received exactly one announcement (got %1)")
                  .arg(watcherB.registrations()));
        check(watcherB.hasPendingReply() && watcherB.accepted() == 0,
              "watcher B registration is pending (not accepted yet)");
        check(!tray.available(),
              "stale success from watcher A did not mark the item available");
        std::printf("race: after stale A reply, available=%s, B registrations=%d accepted=%d\n",
                    tray.available() ? "true" : "false", watcherB.registrations(),
                    watcherB.accepted());

        // Only B accepting the call may make the item available.
        watcherB.releasePendingReply();
        check(waitFor([&] { return watcherB.accepted() >= 1 && tray.available(); }, 9000),
              "item becomes available only after watcher B accepts");
        check(tray.available(), "item reported available after B's acceptance");

        // The exported surface is intact after the race.
        QDBusMessage layout = call(menuPath, menuInterface, QStringLiteral("GetLayout"),
                                   {0, 1, QStringList()});
        check(isOk(layout, "GetLayout after the stale-reply race"),
              "menu still answers after the stale-reply race");

        watcherBusA.unregisterObject(QStringLiteral("/StatusNotifierWatcher"));
        watcherBusB.unregisterObject(QStringLiteral("/StatusNotifierWatcher"));
        watcherBusA.unregisterService(QStringLiteral("org.kde.StatusNotifierWatcher"));
        watcherBusB.unregisterService(QStringLiteral("org.kde.StatusNotifierWatcher"));
        QDBusConnection::disconnectFromBus(QStringLiteral("meshlink-watcher-a"));
        QDBusConnection::disconnectFromBus(QStringLiteral("meshlink-watcher-b"));
    }

    std::printf("tst_tray_sni: %d check(s), %d failure(s)\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("tst_tray_sni: ALL OK\n");
        return 0;
    }
    std::fprintf(stderr, "tst_tray_sni: FAILED\n");
    return 1;
}

#include "tst_tray_sni.moc"
