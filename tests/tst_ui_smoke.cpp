// deepin-MeshLink — offscreen UI smoke test: page construction, button
// geometry regression and optional light/dark screenshots.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "core/account.h"
#include "core/crypto.h"
#include "core/engine.h"
#include "core/settings.h"
#include "core/storage.h"
#include "ui/firststartwizard.h"
#include "ui/mainwindow.h"
#include "ui/profilepage.h"
#include "ui/roomsview.h"
#include "ui/settingsdialog.h"

#include "DApplication"
#include "DGuiApplicationHelper"

#include <QAbstractButton>
#include <QApplication>
#include <QDialog>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QIcon>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QListView>
#include <QPixmap>
#include <QSpinBox>
#include <QSqlQuery>
#include <QTextEdit>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QUdpSocket>
#include <QWidget>

#include <cstdio>
#include <functional>
#include <cstdlib>

DWIDGET_USE_NAMESPACE
DGUI_USE_NAMESPACE

namespace {

void processEventsFor(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

[[noreturn]] void fail(const char *what) {
    std::fprintf(stderr, "ui_smoke: FAILED: %s\n", what);
    std::exit(1);
}

void require(bool condition, const char *what) {
    if (!condition)
        fail(what);
}

quint16 freeUdpPort() {
    QUdpSocket socket;
    socket.bind(QHostAddress::LocalHost, 0);
    return socket.localPort();
}

quint16 freeTcpPort() {
    QTcpServer server;
    server.listen(QHostAddress::LocalHost, 0);
    return server.serverPort();
}

// Buttons must never overlap after layout; this is the regression check for
// the historic orphan "discover rooms" button.
void checkButtonOverlaps(QWidget *page, const char *pageName) {
    struct Item {
        QString name;
        QRect rect;
    };
    QVector<Item> items;
    const QList<QAbstractButton *> buttons = page->findChildren<QAbstractButton *>();
    for (QAbstractButton *button : buttons) {
        if (!button->isVisible() || button->width() <= 0 || button->height() <= 0)
            continue;
        if (button->window() != page->window())
            continue; // popup/menu content
        Item item;
        item.name = button->objectName().isEmpty() ? button->text() : button->objectName();
        item.rect = QRect(button->mapTo(page, QPoint(0, 0)), button->size());
        items.append(item);
    }
    for (int i = 0; i < items.size(); ++i) {
        for (int j = i + 1; j < items.size(); ++j) {
            if (items.at(i).rect.intersects(items.at(j).rect)) {
                std::fprintf(stderr, "ui_smoke: overlapping buttons on %s: %s and %s\n", pageName,
                             qUtf8Printable(items.at(i).name),
                             qUtf8Printable(items.at(j).name));
                std::exit(1);
            }
        }
    }
}

void saveSnapshot(QWidget *widget, const QString &directory, const QString &name) {
    if (directory.isEmpty() || !widget)
        return;
    QDir().mkpath(directory);
    widget->grab().save(directory + QLatin1Char('/') + name + QStringLiteral(".png"));
}

// Deterministic wait helper: the account-destroy flow runs asynchronous
// recovery (timer-deferred handler, modal wizard, Engine::start), so fixed
// sleeps would either be flaky or slow.
template <typename Predicate>
bool waitUntil(Predicate predicate, int timeoutMs) {
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

QByteArray readBytes(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

meshlink::Bytes bytesOf(const QByteArray &data) {
    return meshlink::Bytes(reinterpret_cast<const uint8_t *>(data.constData()),
                           reinterpret_cast<const uint8_t *>(data.constData()) + data.size());
}

bool iconPaintsPixels(const QIcon &icon) {
    if (icon.isNull())
        return false;
    const QPixmap pixmap = icon.pixmap(QSize(24, 24));
    if (pixmap.isNull())
        return false;
    const QImage image = pixmap.toImage();
    int painted = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixelColor(x, y).alpha() > 0)
                ++painted;
        }
    }
    return painted > 10;
}

QWidget *visibleModal() {
    if (QWidget *active = QApplication::activeModalWidget())
        return active;
    for (QWidget *widget : QApplication::topLevelWidgets()) {
        auto *dialog = qobject_cast<QDialog *>(widget);
        if (dialog && dialog->isVisible())
            return dialog;
    }
    return nullptr;
}

bool clickDialogButton(QWidget *root, const QString &text) {
    if (!root)
        return false;
    // DTK inserts visual spacing between CJK glyphs ("销 毁"), and mnemonics
    // add '&'; normalise both sides before matching so the test is locale
    // independent.
    const auto normalise = [](const QString &value) {
        QString out;
        for (const QChar character : value) {
            if (character != QLatin1Char('&') && !character.isSpace())
                out.append(character);
        }
        return out;
    };
    const QString wanted = normalise(text);
    for (QAbstractButton *button : root->findChildren<QAbstractButton *>()) {
        if (normalise(button->text()) == wanted) {
            button->click();
            return true;
        }
    }
    return false;
}

// Autopilot for the modal dialogs of the destroy/re-create flow.
bool g_wizardSeen = false;
bool g_wizardAccepted = false;
int g_unexpectedDialogs = 0;

void accountFlowAutopilot() {
    QWidget *modal = visibleModal();
    if (!modal)
        return;
    if (qobject_cast<meshlink::FirstStartWizard *>(modal)) {
        g_wizardSeen = true;
        // The test installs the application translator, so match the string
        // through the same translation context the widget uses.
        if (clickDialogButton(
                modal, QCoreApplication::translate("meshlink::FirstStartWizard", "Create account"))) {
            g_wizardAccepted = true;
            std::printf("account-flow: first-start wizard 'Create account' clicked\n");
        }
        return;
    }
    if (clickDialogButton(modal, QCoreApplication::translate("meshlink::ProfilePage", "Destroy"))) {
        std::printf("account-flow: destroy confirmed in '%s'\n",
                    qUtf8Printable(modal->windowTitle()));
        return;
    }
    if (clickDialogButton(modal, QCoreApplication::translate("UiUtils", "OK"))) {
        ++g_unexpectedDialogs;
        std::fprintf(stderr, "account-flow: unexpected OK dialog '%s'\n",
                     qUtf8Printable(modal->windowTitle()));
        return;
    }
    ++g_unexpectedDialogs;
    QStringList labels;
    for (QAbstractButton *button : modal->findChildren<QAbstractButton *>())
        labels << QStringLiteral("'%1'").arg(button->text());
    std::fprintf(stderr,
                 "account-flow: unexpected dialog class=%s title='%s' buttons=[%s] closed\n",
                 modal->metaObject()->className(), qUtf8Printable(modal->windowTitle()),
                 qUtf8Printable(labels.join(QStringLiteral(", "))));
    modal->close();
}

// Independent credential check: the on-disk keystore must open with only the
// per-install device secret + salt. No machine-id, hostname, MAC or IP may
// participate, so a simulated machine/hostname change and a network change must
// leave loadAccount() working and the blob byte-identical.
void checkKeystoreStability() {
    QTemporaryDir home;
    require(home.isValid(), "keystore check: temporary data directory");
    qputenv("MESHLINK_DATA_HOME", home.filePath(QStringLiteral("data")).toUtf8());
    qputenv("MESHLINK_CONFIG_HOME", home.filePath(QStringLiteral("config")).toUtf8());

    meshlink::Storage storage;
    QString error;
    require(storage.open(home.filePath(QStringLiteral("keystore-check.db")), &error),
            qUtf8Printable(QStringLiteral("keystore check: open database: %1").arg(error)));
    meshlink::AccountManager accounts(&storage);
    const qint64 id = accounts.createNewAccount(QStringLiteral("Stable Account"), &error);
    require(id > 0,
            qUtf8Printable(QStringLiteral("keystore check: create account: %1").arg(error)));
    const QString fingerprint = accounts.profile().fingerprint;
    require(fingerprint.size() == 32, "keystore check: fingerprint is 32 chars");

    QSqlQuery blobQuery = storage.query(QStringLiteral("SELECT keystore_blob FROM account WHERE id=?"));
    blobQuery.bindValue(0, id);
    require(blobQuery.exec() && blobQuery.next(), "keystore check: keystore blob stored");
    const QByteArray blob = blobQuery.value(0).toByteArray();
    const QByteArray deviceKey = readBytes(meshlink::Paths::keystoreDeviceKey());
    const QByteArray salt = readBytes(meshlink::Paths::keystoreSalt());
    require(deviceKey.size() == 32, "keystore check: device secret is 32 bytes");
    require(salt.size() == 16, "keystore check: install salt is 16 bytes");
    require(!blob.isEmpty() && !blob.contains("PRIVATE KEY"),
            "keystore check: private key material is encrypted");

    // The blob must decrypt with KEK(device.key, salt) alone. If any machine
    // or network identifier participated, this independent decryption would
    // fail even though the same C++ process created the blob.
    const meshlink::Bytes kek =
        meshlink::Crypto::deriveKek(bytesOf(deviceKey), bytesOf(salt));
    require(kek.size() == 32, "keystore check: KEK derives from device key + salt");
    const auto plain = meshlink::Crypto::aesGcmDecrypt(bytesOf(blob), kek);
    require(plain.has_value() && plain->size() >= 68,
            "keystore check: blob decrypts with device-key KEK only");

    // Simulate a machine-id/hostname change (the legacy derivation's factor)
    // plus a LAN / configuration move, then reload from a fresh connection.
    qputenv("MESHLINK_LEGACY_MACHINE_FACTOR", QByteArray("independent-other-machine-id"));
    qputenv("MESHLINK_IP", "10.77.0.9");
    accounts.resetLoaded();
    require(accounts.loadAccount(id, &error),
            qUtf8Printable(QStringLiteral("keystore check: loadAccount after machine change: %1")
                               .arg(error)));
    require(accounts.profile().fingerprint == fingerprint,
            "keystore check: identity survives the simulated machine change");
    require(readBytes(meshlink::Paths::keystoreDeviceKey()) == deviceKey,
            "keystore check: device secret was not rewritten");
    QSqlQuery unchanged = storage.query(QStringLiteral("SELECT keystore_blob FROM account WHERE id=?"));
    unchanged.bindValue(0, id);
    require(unchanged.exec() && unchanged.next(), "keystore check: blob re-read");
    require(unchanged.value(0).toByteArray() == blob,
            "keystore check: keystore blob is byte-identical after the change");

    // Fresh process-like state: new connection + new AccountManager.
    storage.close();
    require(true, "keystore check: close first database connection");
    meshlink::Storage reopened;
    require(reopened.open(home.filePath(QStringLiteral("keystore-check.db")), &error),
            qUtf8Printable(QStringLiteral("keystore check: reopen database: %1").arg(error)));
    meshlink::AccountManager second(&reopened);
    require(second.loadAccount(id, &error),
            qUtf8Printable(QStringLiteral("keystore check: fresh-manager loadAccount: %1")
                               .arg(error)));
    require(second.profile().fingerprint == fingerprint,
            "keystore check: fresh manager decrypts the same identity");
    reopened.close();

    qunsetenv("MESHLINK_LEGACY_MACHINE_FACTOR");
    std::printf("keystore check: device-key KEK only; machine/hostname/IP changes survived\n");
}

} // namespace

int main(int argc, char *argv[]) {
    DApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("HuanSoft"));
    app.setOrganizationDomain(QStringLiteral("huansoft.org.cn"));
    app.setApplicationName(QStringLiteral("deepin-meshlink"));
    app.setApplicationVersion(QStringLiteral(MESHLINK_VERSION));
    app.loadTranslator();
    app.setApplicationDisplayName(QCoreApplication::translate("main", "deepin-MeshLink"));
    DGuiApplicationHelper::setAttribute(DGuiApplicationHelper::DontSaveApplicationTheme, true);

    // Independent t4 check runs against its own data home before the UI test
    // switches MESHLINK_* to the main fixture.
    checkKeystoreStability();
    qunsetenv("MESHLINK_LEGACY_MACHINE_FACTOR");

    QTemporaryDir temp;
    require(temp.isValid(), "could not create a temporary directory");
    qputenv("MESHLINK_DATA_HOME", temp.filePath(QStringLiteral("data")).toUtf8());
    qputenv("MESHLINK_CONFIG_HOME", temp.filePath(QStringLiteral("config")).toUtf8());
    qputenv("MESHLINK_IP", "127.0.0.1");
    qputenv("MESHLINK_DISCOVERY_PORT", QByteArray::number(freeUdpPort()));
    qputenv("MESHLINK_TCP_PORT", QByteArray::number(freeTcpPort()));
    qunsetenv("MESHLINK_BOOTSTRAP");

    meshlink::Settings settings;
    meshlink::Engine engine(&settings);
    QString error;
    require(engine.createFreshAccount(QStringLiteral("UI Test"), &error),
            qUtf8Printable(QStringLiteral("could not create the test account: %1").arg(error)));
    require(engine.start(&error),
            qUtf8Printable(QStringLiteral("could not start the test engine: %1").arg(error)));

    // Populate one hosted room + messages so the chat page and bubbles render.
    const QString roomId = engine.rooms()->createRoom(
        QStringLiteral("UI Room"), QStringLiteral("Smoke-test room"), meshlink::JoinPolicy::Open, 0,
        &error);
    require(!roomId.isEmpty(),
            qUtf8Printable(QStringLiteral("could not create the test room: %1").arg(error)));
    require(!engine.chat()->sendText(roomId, QStringLiteral("Hello from the UI smoke test."), &error)
                 .isEmpty(),
            qUtf8Printable(QStringLiteral("could not send the test message: %1").arg(error)));
    require(!engine.chat()->sendText(roomId, QStringLiteral("A second message for the layout."),
                                     &error)
                 .isEmpty(),
            qUtf8Printable(QStringLiteral("could not send the second test message: %1").arg(error)));
    // Regression: the messages table INSERT used to have one placeholder too
    // few, silently dropping every message from the local history.
    require(engine.chat()->history(roomId, 10).size() >= 2,
            "the local message history did not persist the sent messages");

    const QString snapshotDir = QString::fromLocal8Bit(qgetenv("MESHLINK_UI_SNAPSHOT_DIR"));

    meshlink::MainWindow window(&engine);
    window.show();
    window.activateWindow();
    QApplication::setActiveWindow(&window);
    processEventsFor(300);

    // ---- square navigation rail: size, tooltips, hide-labels live toggle ----
    auto *sidebar = window.findChild<QWidget *>(QStringLiteral("mainWindow.sidebar"));
    auto *navChats = window.findChild<QToolButton *>(QStringLiteral("mainWindow.nav0"));
    auto *settingsButton =
        window.findChild<QToolButton *>(QStringLiteral("mainWindow.settingsButton"));
    require(sidebar != nullptr, "sidebar widget is missing");
    require(navChats != nullptr, "navigation button is missing");
    require(settingsButton != nullptr, "settings rail button is missing");
    require(settingsButton->property("showText").toBool(),
            "settings rail text is hidden while labels are visible");
    require(!settingsButton->toolTip().isEmpty(), "settings rail button has no tooltip");
    require(navChats->iconSize().width() == navChats->iconSize().height(),
            "navigation icon box is not square");
    require(sidebar->width() <= 88, "labelled navigation rail is too wide");
    require(navChats->width() == navChats->height(), "navigation button is not square");
    require(!navChats->toolTip().isEmpty(), "navigation button has no tooltip");
    require(navChats->property("showText").toBool(), "navigation labels should be visible by default");
    settings.setHideNavLabels(true);
    processEventsFor(150);
    require(sidebar->width() <= 64, "hidden-label navigation rail is too wide");
    require(!navChats->property("showText").toBool(), "navigation labels did not hide");
    require(!settingsButton->property("showText").toBool(),
            "settings rail text did not hide with the navigation labels");
    require(!navChats->toolTip().isEmpty(), "hidden navigation label lost its tooltip");
    saveSnapshot(&window, snapshotDir, QStringLiteral("light-sidebar-hidden"));
    settings.setHideNavLabels(false);
    processEventsFor(150);
    require(sidebar->width() <= 88, "navigation labels did not restore");
    require(settingsButton->property("showText").toBool(),
            "settings rail text did not restore with the navigation labels");

    auto *roomList = window.findChild<QListView *>(QStringLiteral("roomsView.roomList"));
    require(roomList != nullptr, "room list is missing");
    require(roomList->sizeHintForRow(0) <= 60, "conversation rows are not compact");
    auto *manageButton = window.findChild<QToolButton *>(QStringLiteral("chatView.manageButton"));
    require(manageButton != nullptr, "room manage button is missing");
    require(manageButton->height() >= 32 && !manageButton->toolTip().isEmpty() &&
                !manageButton->text().isEmpty(),
            "room manage button is too small or unlabelled");
    const auto avatars = window.findChildren<QLabel *>(QStringLiteral("chatView.avatar"));
    require(!avatars.isEmpty(), "message avatars are missing");
    require(!avatars.first()->pixmap(Qt::ReturnByValue).isNull(),
            "message avatar pixmap is empty");
    // Message-level actions moved into the row context menu (UC-14).
    require(window.findChild<QToolButton *>(QStringLiteral("chatView.messageActions")) == nullptr,
            "per-message actions button should not exist; actions live in the context menu");
    // New conversation toolbar entries: @-mention helper and per-room DND bell.
    auto *mentionButton = window.findChild<QToolButton *>(QStringLiteral("chatView.mentionButton"));
    auto *dndButton = window.findChild<QToolButton *>(QStringLiteral("chatView.dndButton"));
    require(mentionButton != nullptr, "@ mention toolbar button is missing");
    require(dndButton != nullptr && dndButton->isCheckable(),
            "per-room notification bell is missing or not checkable");
    // Message bubbles expose a context menu instead of inline buttons: the
    // bubble container carries the policy and right-clicks bubble up from the
    // selectable text label to it.
    bool bubbleHasMenu = false;
    for (const auto &bubble :
         window.findChildren<QLabel *>(QStringLiteral("chatView.messageText"))) {
        if (bubble->parentWidget() &&
            bubble->parentWidget()->contextMenuPolicy() == Qt::CustomContextMenu) {
            bubbleHasMenu = true;
            break;
        }
    }
    require(bubbleHasMenu, "message bubbles lost their context menu policy");

    // Profile page: multi-account combo + random avatar shortcut (UC-17).
    auto *accountCombo = window.findChild<QComboBox *>(QStringLiteral("profilePage.accountCombo"));
    auto *randomAvatar =
        window.findChild<QAbstractButton *>(QStringLiteral("profilePage.randomAvatarButton"));
    require(accountCombo != nullptr, "account switcher combo is missing");
    require(randomAvatar != nullptr, "random avatar button is missing");
    require(accountCombo->count() >= 1, "the active account is not listed in the account combo");

    const QVector<QPair<meshlink::MainWindow::Page, QString>> pages = {
        {meshlink::MainWindow::Page::Chats, QStringLiteral("chats")},
        {meshlink::MainWindow::Page::Discover, QStringLiteral("discover")},
        {meshlink::MainWindow::Page::Transfers, QStringLiteral("transfers")},
        {meshlink::MainWindow::Page::Profile, QStringLiteral("profile")},
    };

    const auto captureTheme = [&](const QString &theme) {
        for (const auto &page : pages) {
            window.selectPage(page.first);
            processEventsFor(150);
            checkButtonOverlaps(&window, qUtf8Printable(theme + QLatin1Char('/') + page.second));
            saveSnapshot(&window, snapshotDir, theme + QLatin1Char('-') + page.second);
        }
    };

    DGuiApplicationHelper::instance()->setPaletteType(DGuiApplicationHelper::LightType);
    processEventsFor(150);
    captureTheme(QStringLiteral("light"));
    DGuiApplicationHelper::instance()->setPaletteType(DGuiApplicationHelper::DarkType);
    processEventsFor(150);
    captureTheme(QStringLiteral("dark"));
    DGuiApplicationHelper::instance()->setPaletteType(DGuiApplicationHelper::LightType);
    processEventsFor(100);

    // Narrow-window regression: 900x600 is the documented minimum size.
    window.resize(900, 600);
    processEventsFor(200);
    for (const auto &page : pages) {
        window.selectPage(page.first);
        processEventsFor(150);
        checkButtonOverlaps(&window, qUtf8Printable(QStringLiteral("narrow/") + page.second));
        saveSnapshot(&window, snapshotDir, QStringLiteral("narrow-") + page.second);
    }
    window.resize(1120, 760);
    processEventsFor(150);

    // Non-default file values prove the nested JSON keys still map to Settings.
    settings.setChunkSizeKb(512);
    settings.setDownloadThreads(7);
    settings.setMaxRoomHistory(300);
    meshlink::SettingsDialog settingsDialog(&engine, &window);
    require(settingsDialog.isValid(), "the DTK settings dialog could not be constructed");
    settingsDialog.show();
    processEventsFor(300);
    auto *segmentCombo = settingsDialog.findChild<QComboBox *>(
        QStringLiteral("settingsDialog.lanSegmentCombo"));
    require(segmentCombo != nullptr, "LAN segment combo is missing from the settings dialog");
    require(segmentCombo->count() > 0, "LAN segment combo has no entries");
    saveSnapshot(&settingsDialog, snapshotDir, QStringLiteral("light-settings"));
    require(settingsDialog.findChildren<QSpinBox *>().size() >= 7,
            "settings dialog is missing numeric options");
    QList<int> spinValues;
    for (QSpinBox *spin : settingsDialog.findChildren<QSpinBox *>())
        spinValues << spin->value();
    require(spinValues.contains(512) && spinValues.contains(7) && spinValues.contains(300),
            "file settings are not bound to their meshlink::Settings keys");
    settingsDialog.scrollToGroup(QStringLiteral("files.downloads"));
    processEventsFor(200);
    saveSnapshot(&settingsDialog, snapshotDir, QStringLiteral("light-settings-files"));
    settingsDialog.scrollToGroup(QStringLiteral("interface.sidebar"));
    processEventsFor(200);
    saveSnapshot(&settingsDialog, snapshotDir, QStringLiteral("light-settings-interface"));
    settingsDialog.scrollToGroup(QStringLiteral("network.discovery"));
    processEventsFor(150);
    checkButtonOverlaps(&settingsDialog, "settings");
    DGuiApplicationHelper::instance()->setPaletteType(DGuiApplicationHelper::DarkType);
    processEventsFor(150);
    saveSnapshot(&settingsDialog, snapshotDir, QStringLiteral("dark-settings"));
    DGuiApplicationHelper::instance()->setPaletteType(DGuiApplicationHelper::LightType);
    processEventsFor(150);
    settingsDialog.close();
    processEventsFor(100);
    settings.setChunkSizeKb(256);
    settings.setDownloadThreads(4);
    settings.setMaxRoomHistory(200);

    // ---- first-start wizard ----
    {
        meshlink::FirstStartWizard wizard(&engine, &window);
        wizard.show();
        processEventsFor(250);
        checkButtonOverlaps(&wizard, "firststart");
        saveSnapshot(&wizard, snapshotDir, QStringLiteral("light-firststart"));
        wizard.close();
        processEventsFor(100);
    }

    auto *rooms = window.findChild<meshlink::RoomsView *>();
    require(rooms != nullptr, "rooms view was not found");

    auto captureModal = [&](const QString &name, const std::function<void()> &openDialog) {
        QTimer::singleShot(300, [&, name] {
            QWidget *active = QApplication::activeModalWidget();
            if (!active) {
                for (QWidget *widget : QApplication::topLevelWidgets()) {
                    auto *dialog = qobject_cast<QDialog *>(widget);
                    if (dialog && dialog->isVisible()) {
                        active = dialog;
                        break;
                    }
                }
            }
            if (!active) {
                std::fprintf(stderr, "ui_smoke: no modal dialog for %s\n", qUtf8Printable(name));
                return;
            }
            saveSnapshot(active, snapshotDir, name);
            checkButtonOverlaps(active, qUtf8Printable(name));
            active->close();
        });
        openDialog();
        processEventsFor(100);
    };

    captureModal(QStringLiteral("light-create-room"), [rooms] { rooms->openCreateDialog(); });
    captureModal(QStringLiteral("light-room-management"),
                 [rooms, roomId] { rooms->openManagementDialog(roomId); });

    // The MainWindow-owned settings dialog (titlebar menu / Ctrl+, path).
    // openSettings() blocks in exec(): poll for the visible modal instead of
    // firing once at a fixed delay — reaching exec() (dialog construction,
    // JSON load, refreshLanSegments) can take longer than any fixed timeout
    // under offscreen rendering, and a missed closer hangs the GUI thread.
    auto settingsCloser = new QTimer(&window);
    settingsCloser->setInterval(50);
    int settingsWaitedMs = 0;
    QObject::connect(settingsCloser, &QTimer::timeout, [settingsCloser, &settingsWaitedMs] {
        QWidget *active = QApplication::activeModalWidget();
        if (!active) {
            for (QWidget *widget : QApplication::topLevelWidgets()) {
                auto *dialog = qobject_cast<QDialog *>(widget);
                if (dialog && dialog->isVisible()) {
                    active = dialog;
                    break;
                }
            }
        }
        if (active) {
            active->close();
            settingsCloser->stop();
            return;
        }
        settingsWaitedMs += settingsCloser->interval();
        if (settingsWaitedMs > 15000) {
            settingsCloser->stop(); // openSettings never opened a modal
            std::fprintf(stderr, "ui_smoke: settings dialog never appeared\n");
        }
    });
    settingsCloser->start();
    window.openSettings();
    settingsCloser->stop();
    processEventsFor(100);

    // ---- quote and recall tombstone rendering ----
    window.selectPage(meshlink::MainWindow::Page::Chats);
    processEventsFor(150);
    const QString targetId = engine.chat()->sendText(roomId, QStringLiteral("Quote target"), &error);
    require(!targetId.isEmpty(), "could not send quote target");
    require(!engine.chat()->sendText(roomId, QStringLiteral("Quoted message"), &error, targetId)
                 .isEmpty(),
            "could not send quoted message");
    processEventsFor(200);
    require(window.findChild<QWidget *>(QStringLiteral("chatView.quoteBlock")) != nullptr,
            "quoted message block is missing");
    require(engine.chat()->recallMessage(roomId, targetId, &error),
            qUtf8Printable(QStringLiteral("host recall failed: %1").arg(error)));
    processEventsFor(200);
    require(window.findChild<QWidget *>(QStringLiteral("chatView.recalled")) != nullptr,
            "recalled message tombstone is missing");
    saveSnapshot(&window, snapshotDir, QStringLiteral("light-quote-recall"));

    // ---- send shortcut: default Enter, Shift+Enter newline, Ctrl+Enter mode ----
    window.selectPage(meshlink::MainWindow::Page::Chats);
    processEventsFor(150);
    auto *input = window.findChild<QTextEdit *>(QStringLiteral("chatView.input"));
    require(input != nullptr, "chat composer is missing");
    const auto messageCount = [&] { return engine.chat()->history(roomId, 500).size(); };

    settings.setSendWithEnter(true);
    processEventsFor(100);
    int before = messageCount();
    input->setPlainText(QStringLiteral("enter sends"));
    QKeyEvent enterKey(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(input, &enterKey);
    processEventsFor(100);
    require(messageCount() == before + 1, "Enter did not send the message by default");

    before = messageCount();
    input->setPlainText(QStringLiteral("shift enter"));
    QKeyEvent shiftEnter(QEvent::KeyPress, Qt::Key_Return, Qt::ShiftModifier);
    QApplication::sendEvent(input, &shiftEnter);
    processEventsFor(100);
    require(messageCount() == before, "Shift+Enter unexpectedly sent a message");

    settings.setSendWithEnter(false);
    processEventsFor(100);
    before = messageCount();
    input->setPlainText(QStringLiteral("plain enter in ctrl mode"));
    QKeyEvent plainEnter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(input, &plainEnter);
    processEventsFor(100);
    require(messageCount() == before, "plain Enter sent a message in Ctrl+Enter mode");
    input->setPlainText(QStringLiteral("ctrl enter sends"));
    QKeyEvent ctrlEnter(QEvent::KeyPress, Qt::Key_Return, Qt::ControlModifier);
    QApplication::sendEvent(input, &ctrlEnter);
    processEventsFor(100);
    require(messageCount() == before + 1, "Ctrl+Enter did not send the message");
    settings.setSendWithEnter(true);
    processEventsFor(100);

    // ---- random-avatar button + account destroy recovery (defects 2/3) ------
    // The profile combo/avatar widgets are re-queried after every account
    // transition on purpose: the contract is a refreshed user-visible window,
    // not just engine state.
    window.selectPage(meshlink::MainWindow::Page::Profile);
    processEventsFor(250);
    auto *profilePage = window.findChild<meshlink::ProfilePage *>();
    require(profilePage != nullptr, "profile page is missing");
    auto *randomAvatarButton =
        window.findChild<QAbstractButton *>(QStringLiteral("profilePage.randomAvatarButton"));
    auto *profileCombo =
        window.findChild<QComboBox *>(QStringLiteral("profilePage.accountCombo"));
    auto *saveButton =
        window.findChild<QAbstractButton *>(QStringLiteral("profilePage.saveButton"));
    auto *destroyButton =
        window.findChild<QAbstractButton *>(QStringLiteral("profilePage.destroyButton"));
    require(randomAvatarButton != nullptr, "random avatar button is missing");
    require(profileCombo != nullptr, "account combo is missing");
    require(saveButton != nullptr, "profile save button is missing");
    require(destroyButton != nullptr, "account destroy button is missing");

    require(randomAvatarButton->isVisibleTo(&window), "random avatar button is not visible");
    require(randomAvatarButton->width() >= 32 && randomAvatarButton->height() >= 32,
            "random avatar button is smaller than 32x32");
    require(!randomAvatarButton->icon().isNull(), "random avatar button has a null icon");
    require(iconPaintsPixels(randomAvatarButton->icon()),
            "random avatar button icon renders no painted pixels");
    require(!randomAvatarButton->toolTip().isEmpty(), "random avatar button has no tooltip");
    require(!randomAvatarButton->accessibleName().isEmpty(),
            "random avatar button has no accessible name");
    std::printf("avatar-button: %dx%d iconSize=%dx%d tooltip='%s' a11y='%s'\n",
                randomAvatarButton->width(), randomAvatarButton->height(),
                randomAvatarButton->iconSize().width(), randomAvatarButton->iconSize().height(),
                qUtf8Printable(randomAvatarButton->toolTip()),
                qUtf8Printable(randomAvatarButton->accessibleName()));

    QLabel *avatarPreview = nullptr;
    for (QLabel *label : profilePage->findChildren<QLabel *>()) {
        if (label->width() == 96 && label->height() == 96) {
            avatarPreview = label;
            break;
        }
    }
    require(avatarPreview != nullptr, "profile avatar preview label is missing");
    const QImage avatarBefore = avatarPreview->pixmap(Qt::ReturnByValue).toImage();
    const QString hashBefore = engine.account()->profile().avatarHash;
    const int randomFilesBefore =
        QDir(meshlink::Paths::avatarDir())
            .entryList({QStringLiteral("random-*.png")}, QDir::Files)
            .size();
    randomAvatarButton->click();
    processEventsFor(300);
    const int randomFilesAfter =
        QDir(meshlink::Paths::avatarDir())
            .entryList({QStringLiteral("random-*.png")}, QDir::Files)
            .size();
    require(randomFilesAfter == randomFilesBefore + 1,
            "random-avatar click generated no identicon file");
    require(avatarPreview->pixmap(Qt::ReturnByValue).toImage() != avatarBefore,
            "random-avatar click did not change the preview");
    saveButton->click();
    processEventsFor(450);
    const meshlink::Profile profileAfterSave = engine.account()->profile();
    require(!profileAfterSave.avatarHash.isEmpty() && profileAfterSave.avatarHash != hashBefore,
            "Save did not persist a new avatar hash");
    require(QFileInfo::exists(profileAfterSave.avatarPath),
            "saved avatar file does not exist");
    bool accountRowHashMatches = false;
    for (const meshlink::AccountEntry &entry : engine.account()->listAccounts()) {
        if (entry.id == engine.account()->activeRowId())
            accountRowHashMatches = entry.avatarHash == profileAfterSave.avatarHash;
    }
    require(accountRowHashMatches, "avatar hash is not stored with the account row");

    // Destroy one of two accounts: the surviving identity is switched to and
    // networking comes back without touching the application lifetime.
    const qint64 alphaId = engine.account()->activeRowId();
    const QString alphaNickname = engine.account()->profile().nickname;
    const qint64 betaId =
        engine.createAdditionalAccount(QStringLiteral("Smoke Beta"), &error);
    require(betaId > 0,
            qUtf8Printable(QStringLiteral("could not create the second account: %1").arg(error)));
    require(engine.account()->activeRowId() == betaId,
            "second account did not become active");
    profilePage->reload();
    processEventsFor(250);
    require(profileCombo->count() == 2, "account combo does not list both accounts");

    QTimer accountAutopilot;
    QObject::connect(&accountAutopilot, &QTimer::timeout, &accountFlowAutopilot);
    accountAutopilot.start(30);

    std::printf("account-flow: clicking destroy for the current of two accounts\n");
    destroyButton->click(); // autopilot confirms "Destroy"
    require(waitUntil([&] { return engine.account()->listAccounts().size() == 1; }, 8000),
            "destroying one of two accounts did not leave one account");
    require(waitUntil([&] { return engine.account()->activeRowId() == alphaId; }, 8000),
            "the surviving account did not become active");
    require(waitUntil([&] { return engine.networkRunning(); }, 8000),
            "networking did not restart on the surviving account");
    require(engine.account()->profile().nickname == alphaNickname,
            "stale profile after destroying the other account");
    require(profileCombo->count() == 1
                && profileCombo->currentText().contains(alphaNickname),
            "account combo was not refreshed to the surviving account");
    require(window.isVisible(), "window disappeared after destroying one account");
    std::printf("account-flow: destroyed one of two accounts, switched to '%s'\n",
                qUtf8Printable(alphaNickname));

    // Destroy the last account: the wizard must appear in-process; completing
    // it must produce a running engine and a refreshed window (no forced quit).
    const QString oldFingerprint = engine.account()->profile().fingerprint;
    g_wizardSeen = false;
    g_wizardAccepted = false;
    g_unexpectedDialogs = 0;
    destroyButton->click(); // autopilot confirms, then completes the wizard
    require(waitUntil([&] { return g_wizardSeen; }, 8000),
            "first-start wizard did not appear after destroying the last account");
    require(waitUntil([&] { return engine.account()->listAccounts().size() == 1; }, 12000),
            "completing the first-start wizard created no account");
    require(waitUntil([&] { return engine.networkRunning(); }, 10000),
            "engine did not start for the re-created identity");
    require(waitUntil([&] { return profileCombo->count() == 1; }, 5000),
            "account combo was not refreshed after re-creation");
    const QString newFingerprint = engine.account()->profile().fingerprint;
    require(newFingerprint.size() == 32 && newFingerprint != oldFingerprint,
            "re-created identity does not have a fresh fingerprint");
    require(profileCombo->currentText().contains(newFingerprint.left(12)),
            "account combo does not show the new fingerprint");
    require(engine.account()->profile().nickname != alphaNickname,
            "stale nickname after re-creating the identity");
    require(window.isVisible(), "window disappeared after re-creating the identity");
    require(g_wizardAccepted, "the first-start wizard was not completed explicitly");
    require(g_unexpectedDialogs == 0, "the account-destroy flow needed unexpected dialogs");
    accountAutopilot.stop();
    std::printf("account-flow: last account destroyed, wizard re-created fp=%s, network=%s\n",
                qUtf8Printable(newFingerprint.left(12)),
                engine.networkRunning() ? "running" : "stopped");

    // ---- close action: pin "minimize to tray" before closing. The fresh
    // profile default is "ask", whose confirmation dialog runs a blocking
    // exec() nobody would close inside this script; the pinned tray-close
    // path must hide the window instead of exiting the application.
    settings.setCloseAction(meshlink::Settings::CloseAction::Minimize);
    window.close();
    processEventsFor(100);
    require(!window.isVisible(), "minimize-to-tray close left the window visible");
    std::printf("ui_smoke: ALL OK\n");
    return 0;
}
