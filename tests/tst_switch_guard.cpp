// deepin-MeshLink — independent regression test for the account-switch
// rollback, destroy-by-id, profile fingerprint prompt and random room name.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
//
// The test drives the real Engine/MainWindow paths with a temporary data home:
//   * corrupt a non-active account's keystore row, fail switchAccount(), verify
//     the previous identity/settings/network are restored and destroy only that
//     account through destroyAccountById();
//   * verify the checked COUNT guard preserves the install-wide secrets on the
//     last-account path, and unknown ids are rejected without side effects;
//   * verify RoomManager::createRoom() generates a default name for empty and
//     whitespace-only input, rejects a 1-char name, and the RoomsView create
//     dialog with an empty field creates that generated room;
//   * verify profilePage.fingerprint always shows the full 32-char fingerprint
//     with rendered text pixels in light/dark palettes, after a failed switch
//     and Cancel, and after destroying the corrupt account via the prompt.
#include "core/account.h"
#include "core/engine.h"
#include "core/roommanager.h"
#include "core/settings.h"
#include "core/storage.h"
#include "ui/mainwindow.h"
#include "ui/profilepage.h"
#include "ui/roomsview.h"

#include "DApplication"
#include "DDialog"
#include "DLineEdit"
#include "DGuiApplicationHelper"

#include <QAbstractButton>
#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QHostAddress>
#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QRegularExpression>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUdpSocket>
#include <QWidget>

#include <cstdio>
#include <cstdlib>
#include <functional>

DWIDGET_USE_NAMESPACE
DGUI_USE_NAMESPACE
using namespace meshlink;

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

[[noreturn]] void setupFailed(const QString &what) {
    std::fprintf(stderr, "tst_switch_guard: FAILED setup: %s\n", qUtf8Printable(what));
    std::exit(1);
}

void require(bool condition, const QString &what) {
    if (!condition)
        setupFailed(what);
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

void processFor(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

template <typename Predicate>
bool waitFor(Predicate predicate, int timeoutMs) {
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

void configureDataHome(const QTemporaryDir &home) {
    qputenv("MESHLINK_DATA_HOME", home.filePath(QStringLiteral("data")).toUtf8());
    qputenv("MESHLINK_CONFIG_HOME", home.filePath(QStringLiteral("config")).toUtf8());
    qputenv("MESHLINK_IP", "127.0.0.1");
    qputenv("MESHLINK_DISCOVERY_PORT", QByteArray::number(freeUdpPort()));
    qputenv("MESHLINK_TCP_PORT", QByteArray::number(freeTcpPort()));
    qunsetenv("MESHLINK_BOOTSTRAP");
    qunsetenv("MESHLINK_LEGACY_MACHINE_FACTOR");
    qunsetenv("MESHLINK_TEST_FAIL_ACCOUNT_COUNT");
}

QString normalizeLabel(const QString &value) {
    QString out;
    for (const QChar character : value) {
        if (character != QLatin1Char('&') && !character.isSpace())
            out.append(character);
    }
    return out;
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

bool clickButton(QWidget *root, const QString &text) {
    if (!root)
        return false;
    const QString wanted = normalizeLabel(text);
    for (QAbstractButton *button : root->findChildren<QAbstractButton *>()) {
        if (normalizeLabel(button->text()) == wanted) {
            button->click();
            return true;
        }
    }
    return false;
}

int textPixelCount(const QImage &source) {
    const QImage image = source.convertToFormat(QImage::Format_ARGB32);
    if (image.isNull() || image.width() < 2 || image.height() < 2)
        return 0;
    // QWidget::grab() paints the widget background; the corner is stable and
    // contains no glyph because the label has a left margin. If the background
    // is transparent, every painted glyph pixel is text.
    const QColor background = image.pixelColor(1, 1);
    int text = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.alpha() < 16)
                continue;
            if (background.alpha() < 16) {
                ++text;
                continue;
            }
            const int distance = qAbs(pixel.red() - background.red())
                                 + qAbs(pixel.green() - background.green())
                                 + qAbs(pixel.blue() - background.blue());
            if (distance > 80)
                ++text;
        }
    }
    return text;
}

void testSwitchRollbackAndRooms() {
    QTemporaryDir home;
    require(home.isValid(), QStringLiteral("switch: temporary data directory"));
    configureDataHome(home);
    const QString dataRoot = QString::fromUtf8(qgetenv("MESHLINK_DATA_HOME"));

    Settings settings;
    Engine engine(&settings);
    QString error;
    require(engine.createFreshAccount(QStringLiteral("Alpha"), &error),
            QStringLiteral("switch: create account A: %1").arg(error));
    const qint64 accountA = engine.account()->activeRowId();
    require(accountA > 0, QStringLiteral("switch: account A row id"));
    const QString fingerprintA = engine.account()->profile().fingerprint;
    require(fingerprintA.size() == 32, QStringLiteral("switch: account A fingerprint"));
    require(engine.start(&error), QStringLiteral("switch: start engine: %1").arg(error));
    const bool preSwitchNetwork = engine.networkRunning();
    check(preSwitchNetwork, "switch: engine network is running before the failed switch");

    const qint64 accountB = engine.createAdditionalAccount(QStringLiteral("Beta"), &error);
    require(accountB > 0, QStringLiteral("switch: create account B: %1").arg(error));
    check(engine.account()->activeRowId() == accountB, "switch: account B becomes active on creation");
    const QString betaRoom = engine.rooms()->createRoom(
        QStringLiteral("Beta room"), QString(), JoinPolicy::Open, 0, &error);
    check(!betaRoom.isEmpty(), QStringLiteral("switch: Beta hosted room created (%1)").arg(error));

    require(engine.switchAccount(accountA, &error),
            QStringLiteral("switch: switch back to account A: %1").arg(error));
    const QString alphaRoom = engine.rooms()->createRoom(
        QStringLiteral("Alpha room"), QString(), JoinPolicy::Open, 0, &error);
    check(!alphaRoom.isEmpty(), QStringLiteral("switch: Alpha hosted room created (%1)").arg(error));

    // Deterministic corruption without crypto shortcuts: a short, invalid GCM
    // blob makes loadAccount() fail authentication for that row only.
    QSqlQuery corrupt = engine.storage()->query(
        QStringLiteral("UPDATE account SET keystore_blob=? WHERE id=?"));
    corrupt.bindValue(0, QByteArray("corrupt-keystore-blob"));
    corrupt.bindValue(1, accountB);
    check(corrupt.exec(), "switch: corrupt account B keystore_blob with SQL");

    const QString legacyPath =
        dataRoot + QStringLiteral("/keystore/legacy/%1.blob").arg(accountB);
    QDir().mkpath(QFileInfo(legacyPath).absolutePath());
    {
        QFile legacy(legacyPath);
        require(legacy.open(QIODevice::WriteOnly | QIODevice::Truncate),
                QStringLiteral("switch: create account B retained legacy copy"));
        legacy.write(QByteArray("retained-legacy-ciphertext"));
        legacy.close();
        QFile::setPermissions(legacyPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
    check(QFile::exists(legacyPath), "switch: account B retained legacy copy exists");

    const qint64 activeBefore = engine.account()->activeRowId();
    const QString fingerprintBefore = engine.account()->profile().fingerprint;
    QString switchError;
    check(!engine.switchAccount(accountB, &switchError),
          "switch: switching to the corrupt account B fails");
    check(engine.lastSwitchFailure() == Engine::SwitchFailure::Identity,
          "switch: failure is typed as Identity");
    check(engine.account()->activeRowId() == accountA,
          "switch: active row id rolled back to account A");
    check(engine.account()->profile().fingerprint == fingerprintA,
          "switch: active profile rolled back to account A");
    check(engine.settings()->activeAccountId() == accountA,
          "switch: settings activeAccountId rolled back to account A");
    check(engine.networkRunning() == preSwitchNetwork,
          "switch: networkRunning restored to the pre-switch value");
    check(!switchError.isEmpty(), "switch: the original failure error is reported");
    check(activeBefore == accountA && fingerprintBefore == fingerprintA,
          "switch: pre-switch active identity was account A");

    const QByteArray salt = [&] {
        QFile file(Paths::keystoreSalt());
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }();
    const QByteArray deviceKey = [&] {
        QFile file(Paths::keystoreDeviceKey());
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }();
    const QByteArray deviceTag = [&] {
        QFile file(Paths::keystoreDeviceKey() + QStringLiteral(".tag"));
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }();
    check(salt.size() == 16 && deviceKey.size() == 32 && deviceTag.size() == 32,
          "switch: install salt/device.key/tag exist before destroy-by-id");

    const int accountsBefore = engine.account()->listAccounts().size();
    QString unknownError;
    check(engine.destroyAccountById(0x7fffffff, false, &unknownError) == -1,
          "destroy: unknown id is rejected");
    check(!unknownError.isEmpty(), "destroy: unknown id returns a diagnostic");
    check(engine.account()->listAccounts().size() == accountsBefore,
          "destroy: unknown id changed no account rows");
    check(engine.account()->activeRowId() == accountA
              && engine.account()->profile().fingerprint == fingerprintA,
          "destroy: unknown id left the active identity untouched");

    QString destroyError;
    const int remaining = engine.destroyAccountById(accountB, true, &destroyError);
    check(remaining == 1, QStringLiteral("destroy: non-active B removed, one account left (%1)")
                              .arg(remaining));
    bool betaStillListed = false;
    for (const AccountEntry &entry : engine.account()->listAccounts()) {
        if (entry.id == accountB)
            betaStillListed = true;
    }
    check(!betaStillListed, "destroy: account B row is gone");
    check(engine.account()->activeRowId() == accountA,
          "destroy: account A remains the loaded active row");
    check(engine.account()->profile().fingerprint == fingerprintA,
          "destroy: account A profile is untouched");
    check(engine.networkRunning() == preSwitchNetwork,
          "destroy: non-active destroy did not stop the running network");
    check(QFile::exists(Paths::keystoreSalt()) && QFile::exists(Paths::keystoreDeviceKey())
              && QFile::exists(Paths::keystoreDeviceKey() + QStringLiteral(".tag")),
          "destroy: install salt/device.key/tag survive while A remains");
    check(!QFile::exists(legacyPath),
          "destroy: account B retained legacy copy was removed");

    auto roomCount = [&](const QString &roomId) {
        QSqlQuery query = engine.storage()->query(
            QStringLiteral("SELECT COUNT(*) FROM rooms WHERE room_id=?"));
        query.bindValue(0, roomId);
        return query.exec() && query.next() ? query.value(0).toInt() : -1;
    };
    check(roomCount(betaRoom) == 0, "destroy: account B room was removed");
    check(roomCount(alphaRoom) == 1, "destroy: active account A room was untouched");

    engine.account()->resetLoaded();
    check(engine.account()->loadAccount(accountA, &error),
          QStringLiteral("destroy: active account A still loads (%1)").arg(error));
    check(engine.account()->profile().fingerprint == fingerprintA,
          "destroy: reloaded account A keeps its fingerprint");

    // ---- empty/whitespace room names and the create dialog -----------------
    const QRegularExpression generatedName(
        QStringLiteral("^deepin-MeshLink room_[0-9A-F]{4}$"));
    QString roomError;
    const QString emptyRoom = engine.rooms()->createRoom(
        QString(), QString(), JoinPolicy::Open, 0, &roomError);
    check(!emptyRoom.isEmpty(),
          QStringLiteral("room: createRoom(\"\") succeeds (%1)").arg(roomError));
    const RoomLocal emptyLocal = engine.rooms()->room(emptyRoom);
    check(generatedName.match(emptyLocal.name).hasMatch(),
          QStringLiteral("room: empty name is generated (%1)").arg(emptyLocal.name));
    check(emptyLocal.name.size() >= 2 && emptyLocal.name.size() <= 64,
          "room: generated empty name length is 2-64");

    QString oneCharError;
    check(engine.rooms()->createRoom(QStringLiteral("x"), QString(), JoinPolicy::Open, 0,
                                     &oneCharError)
              .isEmpty(),
          "room: explicit 1-char name is still rejected");
    check(!oneCharError.isEmpty(), "room: 1-char rejection carries a diagnostic");

    QString whitespaceError;
    const QString whitespaceRoom = engine.rooms()->createRoom(
        QStringLiteral("   "), QString(), JoinPolicy::Open, 0, &whitespaceError);
    check(!whitespaceRoom.isEmpty(),
          QStringLiteral("room: whitespace-only name succeeds (%1)").arg(whitespaceError));
    const RoomLocal whitespaceLocal = engine.rooms()->room(whitespaceRoom);
    check(generatedName.match(whitespaceLocal.name).hasMatch(),
          QStringLiteral("room: whitespace-only name is generated (%1)").arg(whitespaceLocal.name));

    RoomsView roomsView(&engine);
    roomsView.resize(900, 600);
    roomsView.show();
    processFor(250);
    const int roomsBefore = engine.rooms()->myRooms().size();
    QString createdName;
    bool sawCreateDialog = false;
    bool sawInfoDialog = false;
    bool infoClosed = false;
    QTimer dialogDriver;
    dialogDriver.setInterval(20);
    QObject::connect(&dialogDriver, &QTimer::timeout, [&] {
        QWidget *modal = visibleModal();
        if (!modal)
            return;
        static QString lastTitle;
        if (lastTitle != modal->windowTitle()) {
            lastTitle = modal->windowTitle();
            std::printf("room-dialog: modal class=%s title='%s' buttons=",
                        modal->metaObject()->className(),
                        qUtf8Printable(modal->windowTitle()));
            for (QAbstractButton *button : modal->findChildren<QAbstractButton *>())
                std::printf("'%s' ", qUtf8Printable(button->text()));
            std::printf("\n");
        }
        if (auto *nameEdit = modal->findChild<DLineEdit *>(
                QStringLiteral("roomsView.createdRoomName"))) {
            sawInfoDialog = true;
            createdName = nameEdit->text();
            clickButton(modal, QStringLiteral("OK"));
            infoClosed = true;
            return;
        }
        if (clickButton(modal, QStringLiteral("Create"))) {
            sawCreateDialog = true;
            return;
        }
    });
    dialogDriver.start();
    roomsView.openCreateDialog();
    waitFor([&] { return infoClosed; }, 5000);
    dialogDriver.stop();
    check(sawCreateDialog, "room-dialog: create dialog opened");
    check(sawInfoDialog && infoClosed, "room-dialog: created-room info dialog was completed");
    check(!createdName.isEmpty() && generatedName.match(createdName).hasMatch(),
          QStringLiteral("room-dialog: info dialog shows the generated name (%1)").arg(createdName));
    check(engine.rooms()->myRooms().size() == roomsBefore + 1,
          "room-dialog: empty name created exactly one room");
    bool createdListed = false;
    for (const RoomLocal &room : engine.rooms()->myRooms()) {
        if (room.name == createdName)
            createdListed = true;
    }
    check(createdListed, "room-dialog: stored room name equals the info-dialog name");

    // ---- checked COUNT failure on the last account must preserve secrets ---
    qputenv("MESHLINK_TEST_FAIL_ACCOUNT_COUNT", "1");
    QString countError;
    const int failedDestroy = engine.destroyAccountById(accountA, false, &countError);
    check(failedDestroy == -1,
          QStringLiteral("destroy: checked COUNT failure reports -1 (%1)").arg(failedDestroy));
    check(!countError.isEmpty(), "destroy: checked COUNT failure carries a diagnostic");
    check(QFile::exists(Paths::keystoreSalt()) && QFile::exists(Paths::keystoreDeviceKey())
              && QFile::exists(Paths::keystoreDeviceKey() + QStringLiteral(".tag")),
          "destroy: checked COUNT failure preserves salt/device.key/tag");
    qunsetenv("MESHLINK_TEST_FAIL_ACCOUNT_COUNT");

    // The failed active destroy removed the row (the caller is told) but the
    // checked-count failure must keep the install-wide secrets so the data is
    // recoverable. Reopen storage and confirm the account is gone, not silently
    // left in a half-destroyed state.
    if (!engine.storage()->isOpen()) {
        check(engine.storage()->open(Paths::databaseFile(), &error),
              QStringLiteral("destroy: reopen storage after COUNT failure (%1)").arg(error));
    }
    engine.account()->resetLoaded();
    check(engine.account()->listAccounts().isEmpty(),
          "destroy: checked COUNT failure removed the last account row");
    check(QFile::exists(Paths::keystoreSalt()) && QFile::exists(Paths::keystoreDeviceKey())
              && QFile::exists(Paths::keystoreDeviceKey() + QStringLiteral(".tag")),
          "destroy: checked COUNT failure still preserves salt/device.key/tag");

    // With the fault cleared, a fresh account can be created with the preserved
    // install secrets and the successful last-account destroy removes them.
    const qint64 replacement =
        engine.account()->createNewAccount(QStringLiteral("Replacement"), &error);
    check(replacement > 0,
          QStringLiteral("destroy: replacement account created after COUNT failure (%1)")
              .arg(error));
    QString lastError;
    const int afterLast = engine.destroyAccountById(replacement, false, &lastError);
    check(afterLast == 0,
          QStringLiteral("destroy: destroying the last account reports zero (%1)").arg(afterLast));
    check(!QFile::exists(Paths::keystoreSalt())
              && !QFile::exists(Paths::keystoreDeviceKey())
              && !QFile::exists(Paths::keystoreDeviceKey() + QStringLiteral(".tag")),
          "destroy: checked-count last account removes salt/device.key/tag");
}

void testProfileFingerprintAndPrompt() {
    QTemporaryDir home;
    require(home.isValid(), QStringLiteral("profile: temporary data directory"));
    configureDataHome(home);

    Settings settings;
    Engine engine(&settings);
    QString error;
    require(engine.createFreshAccount(QStringLiteral("Alpha"), &error),
            QStringLiteral("profile: create account A: %1").arg(error));
    const qint64 accountA = engine.account()->activeRowId();
    const QString fingerprintA = engine.account()->profile().fingerprint;
    require(fingerprintA.size() == 32, QStringLiteral("profile: account A fingerprint"));
    require(engine.start(&error), QStringLiteral("profile: start engine: %1").arg(error));
    const qint64 accountB = engine.createAdditionalAccount(QStringLiteral("Beta"), &error);
    require(accountB > 0, QStringLiteral("profile: create account B: %1").arg(error));
    require(engine.switchAccount(accountA, &error),
            QStringLiteral("profile: switch back to A: %1").arg(error));
    {
        QSqlQuery corrupt = engine.storage()->query(
            QStringLiteral("UPDATE account SET keystore_blob=? WHERE id=?"));
        corrupt.bindValue(0, QByteArray("corrupt-keystore-blob"));
        corrupt.bindValue(1, accountB);
        require(corrupt.exec(), QStringLiteral("profile: corrupt account B keystore"));
    }

    MainWindow window(&engine);
    window.show();
    window.activateWindow();
    window.selectPage(MainWindow::Page::Profile);
    bool quitRequested = false;
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, [&] { quitRequested = true; });
    processFor(450);

    auto *fingerprintLabel =
        window.findChild<QLabel *>(QStringLiteral("profilePage.fingerprint"));
    auto *combo = window.findChild<QComboBox *>(QStringLiteral("profilePage.accountCombo"));
    require(fingerprintLabel != nullptr, QStringLiteral("profile: fingerprint label is missing"));
    require(combo != nullptr, QStringLiteral("profile: account combo is missing"));
    require(combo->count() == 2, QStringLiteral("profile: account combo lists A and B"));

    const auto checkFingerprint = [&](const char *when) {
        auto *label =
            window.findChild<QLabel *>(QStringLiteral("profilePage.fingerprint"));
        check(label != nullptr, QStringLiteral("%1: fingerprint label exists").arg(when));
        if (!label)
            return;
        check(label->text() == fingerprintA,
              QStringLiteral("%1: fingerprint text is the full active fingerprint (%2)")
                  .arg(QString::fromLatin1(when), label->text()));
        check(label->isVisible() && label->width() > 0,
              QStringLiteral("%1: fingerprint row is visible").arg(when));
        const int pixels = textPixelCount(label->grab().toImage());
        check(pixels > 50,
              QStringLiteral("%1: rendered fingerprint has text pixels (got %2)")
                  .arg(QString::fromLatin1(when))
                  .arg(pixels));
    };

    DGuiApplicationHelper::instance()->setPaletteType(DGuiApplicationHelper::LightType);
    processFor(250);
    checkFingerprint("light");
    DGuiApplicationHelper::instance()->setPaletteType(DGuiApplicationHelper::DarkType);
    processFor(250);
    checkFingerprint("dark");
    DGuiApplicationHelper::instance()->setPaletteType(DGuiApplicationHelper::LightType);
    processFor(200);

    const auto indexOfBeta = [&] {
        for (int index = 0; index < combo->count(); ++index) {
            if (combo->itemText(index).contains(QStringLiteral("Beta")))
                return index;
        }
        return -1;
    };
    auto betaIndex = indexOfBeta();
    require(betaIndex >= 0, QStringLiteral("profile: Beta entry in the account combo"));

    bool sawSwitchConfirm = false;
    bool sawUnlockPrompt = false;
    bool promptHandled = false;
    bool promptDone = false;
    bool destroyMode = false;
    QString unexpectedModal;
    QTimer driver;
    driver.setInterval(20);
    QObject::connect(&driver, &QTimer::timeout, [&] {
        QWidget *modal = visibleModal();
        if (!modal)
            return;
        // DTK's DDialog::setTitle() fills the title label but not the window
        // title, so identify the dialogs by their unique action buttons.
        const auto hasButton = [&](const QString &text) {
            const QString wanted = normalizeLabel(text);
            for (QAbstractButton *button : modal->findChildren<QAbstractButton *>()) {
                if (normalizeLabel(button->text()) == wanted)
                    return true;
            }
            return false;
        };
        if (clickButton(modal, QStringLiteral("Switch"))) {
            sawSwitchConfirm = true;
            return;
        }
        if (hasButton(QStringLiteral("Destroy account"))) {
            sawUnlockPrompt = true;
            if (promptHandled)
                return;
            promptHandled = true;
            if (destroyMode)
                clickButton(modal, QStringLiteral("Destroy account"));
            else
                clickButton(modal, QStringLiteral("Cancel"));
            promptDone = true;
            return;
        }
        if (!unexpectedModal.isEmpty())
            return;
        unexpectedModal = modal->metaObject()->className();
        if (!clickButton(modal, QStringLiteral("OK")))
            clickButton(modal, QStringLiteral("Cancel"));
    });

    // Failed switch + Cancel: the combo reverts and the fingerprint must stay
    // complete in both palettes.
    driver.start();
    combo->setCurrentIndex(betaIndex);
    waitFor([&] { return promptDone; }, 5000);
    driver.stop();
    processFor(200);
    check(sawSwitchConfirm, "profile: selecting Beta asks for switch confirmation");
    check(sawUnlockPrompt, "profile: Identity failure opens the unlockable-account prompt");
    check(promptDone, "profile: Cancel closed the unlock prompt");
    check(engine.account()->activeRowId() == accountA,
          "profile: Cancel keeps account A active");
    check(combo->currentIndex() == 0, "profile: Cancel reverts the account combo to A");
    checkFingerprint("after-cancel-light");

    DGuiApplicationHelper::instance()->setPaletteType(DGuiApplicationHelper::DarkType);
    processFor(250);
    checkFingerprint("after-cancel-dark");
    DGuiApplicationHelper::instance()->setPaletteType(DGuiApplicationHelper::LightType);
    processFor(200);

    // Destroy path: select Beta again; the prompt's Destroy button removes only
    // the corrupt account while A stays active and the app keeps running.
    betaIndex = indexOfBeta();
    require(betaIndex >= 0, QStringLiteral("profile: Beta entry after Cancel"));
    sawSwitchConfirm = false;
    sawUnlockPrompt = false;
    promptHandled = false;
    promptDone = false;
    destroyMode = true;
    driver.start();
    combo->setCurrentIndex(betaIndex);
    waitFor([&] { return promptDone; }, 5000);
    driver.stop();
    processFor(300);
    check(sawSwitchConfirm, "profile: selecting Beta for destruction confirms the switch");
    check(sawUnlockPrompt, "profile: destruction path reuses the unlock prompt");
    check(promptDone, "profile: Destroy account closed the prompt");
    check(unexpectedModal.isEmpty(),
          QStringLiteral("profile: no unexpected modal opened (%1)").arg(unexpectedModal));
    bool betaRemoved = true;
    for (const AccountEntry &entry : engine.account()->listAccounts()) {
        if (entry.id == accountB)
            betaRemoved = false;
    }
    check(betaRemoved, "profile: Destroy removes the corrupt account B");
    check(engine.account()->activeRowId() == accountA,
          "profile: Destroy keeps account A active");
    check(engine.account()->profile().fingerprint == fingerprintA,
          "profile: Destroy does not change account A's identity");
    check(combo->count() == 1 && combo->currentIndex() == 0,
          "profile: account combo contains only the surviving account A");
    check(window.isVisible(), "profile: main window remains visible after destroying B");
    check(!quitRequested, "profile: destroying B emitted no aboutToQuit signal");
    check(QCoreApplication::instance() != nullptr && !QCoreApplication::closingDown(),
          "profile: destroying B did not quit the application");
    checkFingerprint("after-destroy");
    DGuiApplicationHelper::instance()->setPaletteType(DGuiApplicationHelper::DarkType);
    processFor(250);
    checkFingerprint("after-destroy-dark");
    DGuiApplicationHelper::instance()->setPaletteType(DGuiApplicationHelper::LightType);
    processFor(100);
}

} // namespace

int main(int argc, char *argv[]) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    DApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("HuanSoft"));
    app.setOrganizationDomain(QStringLiteral("huansoft.org.cn"));
    app.setApplicationName(QStringLiteral("deepin-meshlink"));
    app.setApplicationVersion(QStringLiteral(MESHLINK_VERSION));
    DGuiApplicationHelper::setAttribute(DGuiApplicationHelper::DontSaveApplicationTheme, true);

    testSwitchRollbackAndRooms();
    testProfileFingerprintAndPrompt();

    std::printf("tst_switch_guard: %d check(s), %d failure(s)\n", g_checks, g_failures);
    if (g_failures == 0)
        std::printf("tst_switch_guard: ALL OK\n");
    return g_failures == 0 ? 0 : 1;
}
