// deepin-MeshLink — application entry.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "DApplication"
#include "DMainWindow"

#include "core/engine.h"
#include "core/settings.h"
#include "ui/appicon.h"
#include "ui/firststartwizard.h"
#include "ui/mainwindow.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QIcon>

DWIDGET_USE_NAMESPACE

int main(int argc, char *argv[]) {
    // Qt6 high-DPI is automatic; keep AA_* calls out (deprecated since 6.7).
    DApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("HuanSoft"));
    app.setOrganizationDomain(QStringLiteral("huansoft.org.cn"));
    app.setApplicationName(QStringLiteral("deepin-meshlink"));
    app.setApplicationVersion(QStringLiteral(MESHLINK_VERSION));

    // The application name must be set first: loadTranslator() looks for
    // <applicationName>_<locale>.qm. It must run before any UI is created.
    app.loadTranslator();

    app.setApplicationDisplayName(QCoreApplication::translate("main", "deepin-MeshLink"));
    app.setProductName(QCoreApplication::translate("main", "deepin-MeshLink"));
    app.setApplicationDescription(
        QCoreApplication::translate("main", "Decentralised LAN instant messaging: every node "
                                            "is a server. All traffic is protected by TLS 1.3."));
    app.setApplicationHomePage(QStringLiteral("https://huansoft.org.cn"));
    app.setApplicationLicense(QStringLiteral("GPL-3.0-or-later"));

    // Prefer the desktop icon theme; fall back to the embedded PNG so the
    // window icon never depends on the Qt SVG image plugin being installed.
    QIcon productIcon = QIcon::fromTheme(QStringLiteral("cn.org.huansoft.deepin-meshlink"));
    if (productIcon.isNull())
        productIcon = meshlink::meshlinkAppIcon();
    app.setProductIcon(productIcon);
    app.setWindowIcon(productIcon);

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption bootstrap(
        QStringList() << QStringLiteral("bootstrap"),
        QCoreApplication::translate("main", "Send discovery heartbeats to a host "
                                            "(for broadcast-blocked LANs): ip[:port]"),
        QCoreApplication::translate("main", "host"));
    parser.addOption(bootstrap);
    QCommandLineOption headless(
        QStringList() << QStringLiteral("headless"),
        QCoreApplication::translate("main", "Run only the core engine (integration tests)"));
    parser.addOption(headless);
    parser.process(app);

    meshlink::Settings settings;
    if (parser.isSet(bootstrap)) {
        const QString spec = parser.value(bootstrap);
        const int sep = spec.lastIndexOf(QLatin1Char(':'));
        const QString ip = sep > 0 ? spec.left(sep) : spec;
        const quint16 port =
            sep > 0 ? quint16(spec.mid(sep + 1).toUShort()) : settings.discoveryPort();
        settings.addBootstrapTarget(ip, port);
    }

    meshlink::Engine engine(&settings);
    // UC-01 first run: optional nickname dialog before the engine auto-creates
    // a default identity. Declining keeps the user out of the main window (E1).
    if (!parser.isSet(headless) && !meshlink::FirstStartWizard::ensureAccount(&engine, nullptr))
        return 1;
    QString startError;
    if (!engine.start(&startError)) {
        if (parser.isSet(headless)) {
            qCritical("meshlink failed to start: %s", qUtf8Printable(startError));
            return 2;
        }
        // UC-01 A1 / UC-04 E1 diagnostics before any window exists
        qWarning("meshlink: %s", qUtf8Printable(startError));
    }

    if (parser.isSet(headless)) {
        QObject::connect(&engine, &meshlink::Engine::started, &app, [](bool ok) {
            qInfo("meshlink headless ready=%d", int(ok));
        });
        if (!parser.isSet(bootstrap) && startError.isEmpty())
            qInfo("meshlink headless running; Ctrl-C to stop.");
        return app.exec();
    }

    meshlink::MainWindow window(&engine);
    if (!startError.isEmpty())
        window.showStartupError(startError); // e.g. port conflict -> settings hint
    window.show();
    return app.exec();
}
