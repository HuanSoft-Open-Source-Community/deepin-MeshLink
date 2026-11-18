// deepin-MeshLink — independent regression test for message/member context
// menus (defects 4 and 5).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
//
// The test builds a real Engine with a temporary data home, a hosted room and
// three member rows, then drives the user-visible surface:
//   * a right-click on the message bubble, its selectable text and a quoted
//     reply opens the MESSAGE actions (Reply/Copy/Recall), never the member menu;
//   * a right-click on the avatar opens the MEMBER menu (Mention/...), never
//     message actions;
//   * MemberMenu contents and enabled states are correct for host, plain member
//     and self targets, with Mute durations and Remove routed through
//     RoomManager (database state + refresh callback observed);
//   * the management dialog's members tab also opens the same member menu and
//     its Mention action reaches the composer.
//
// QMenu::exec() is never called by the test itself (only by production code
// where a nested event loop is part of the contract); a deterministic timer
// captures and closes the popup.
#include "core/engine.h"
#include "core/settings.h"
#include "core/storage.h"
#include "ui/chatview.h"
#include "ui/membermenu.h"
#include "ui/roomsview.h"

#include <DApplication>
#include <DGuiApplicationHelper>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDialog>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QIcon>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QSqlError>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QThread>
#include <QTimer>
#include <QUdpSocket>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>

DWIDGET_USE_NAMESPACE
DGUI_USE_NAMESPACE

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
    std::fprintf(stderr, "tst_chat_menus: FAILED setup: %s\n", qUtf8Printable(what));
    std::exit(1);
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

void processFor(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
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

QAction *findAction(QMenu *menu, const QString &needle) {
    if (!menu)
        return nullptr;
    for (QAction *action : menu->actions()) {
        if (action->text().contains(needle))
            return action;
        if (action->menu()) {
            if (QAction *nested = findAction(action->menu(), needle))
                return nested;
        }
    }
    return nullptr;
}

QStringList actionTexts(QMenu *menu) {
    QStringList texts;
    if (!menu)
        return texts;
    for (QAction *action : menu->actions()) {
        if (!action->isSeparator())
            texts << action->text();
        if (action->menu())
            texts << actionTexts(action->menu());
    }
    return texts;
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
    for (QAbstractButton *button : root->findChildren<QAbstractButton *>()) {
        QString label = button->text();
        label.remove(QLatin1Char('&'));
        if (label == text) {
            button->click();
            return true;
        }
    }
    return false;
}

struct CapturedMenu {
    bool saw = false;
    QStringList actions;
};

// Opens a blocking QMenu through the production path and captures its entries
// from a timer inside the nested event loop, then closes it. A watchdog closes
// any popup that was not recognised, so a regression can never hang the suite.
CapturedMenu captureBlockingMenu(const std::function<void()> &openMenu) {
    auto captured = std::make_shared<CapturedMenu>();
    QTimer::singleShot(150, [captured] {
        if (auto *popup = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
            captured->saw = true;
            captured->actions = actionTexts(popup);
            popup->close();
        }
    });
    QTimer::singleShot(5000, [] {
        if (auto *popup = QApplication::activePopupWidget())
            popup->close();
    });
    openMenu();
    processFor(50);
    return *captured;
}

CapturedMenu captureOpenPopup() {
    CapturedMenu captured;
    if (auto *popup = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
        captured.saw = true;
        captured.actions = actionTexts(popup);
        popup->close();
    }
    return captured;
}

void rightClick(QWidget *widget) {
    const QPoint local(5, 5);
    QContextMenuEvent event(QContextMenuEvent::Mouse, local, widget->mapToGlobal(local));
    QApplication::sendEvent(widget, &event);
}

} // namespace

int main(int argc, char *argv[]) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    DApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("HuanSoft"));
    app.setOrganizationDomain(QStringLiteral("huansoft.org.cn"));
    app.setApplicationName(QStringLiteral("tst_chat_menus"));
    app.setApplicationVersion(QStringLiteral(MESHLINK_VERSION));
    DGuiApplicationHelper::setAttribute(DGuiApplicationHelper::DontSaveApplicationTheme, true);

    QTemporaryDir temp;
    if (!temp.isValid())
        setupFailed(QStringLiteral("temporary data directory"));
    qputenv("MESHLINK_DATA_HOME", temp.filePath(QStringLiteral("data")).toUtf8());
    qputenv("MESHLINK_CONFIG_HOME", temp.filePath(QStringLiteral("config")).toUtf8());
    qputenv("MESHLINK_IP", "127.0.0.1");
    qputenv("MESHLINK_DISCOVERY_PORT", QByteArray::number(freeUdpPort()));
    qputenv("MESHLINK_TCP_PORT", QByteArray::number(freeTcpPort()));
    qunsetenv("MESHLINK_BOOTSTRAP");

    meshlink::Settings settings;
    meshlink::Engine engine(&settings);
    QString error;
    if (!engine.createFreshAccount(QStringLiteral("Host Nick"), &error))
        setupFailed(QStringLiteral("create account: %1").arg(error));
    if (!engine.start(&error))
        setupFailed(QStringLiteral("start engine: %1").arg(error));
    const QString roomId = engine.rooms()->createRoom(QStringLiteral("Menu Room"), QString(),
                                                      meshlink::JoinPolicy::Open, 0, &error);
    if (roomId.isEmpty())
        setupFailed(QStringLiteral("create room: %1").arg(error));
    const QString selfFp = engine.account()->profile().fingerprint;

    const QString peerFp = QStringLiteral("peer-fingerprint-0001");
    const QString adminFp = QStringLiteral("admin-fingerprint-001");
    const QString managedFp = QStringLiteral("managed-fingerprint-01");
    auto insertMember = [&](const QString &fp, const QString &nick, meshlink::MemberRole role,
                            qint64 mutedUntil) {
        QSqlQuery q = engine.storage()->query(
            "INSERT OR REPLACE INTO members "
            "(room_id,fingerprint,nickname,role,muted_until,joined_ts) VALUES (?,?,?,?,?,?)");
        q.bindValue(0, roomId);
        q.bindValue(1, fp);
        q.bindValue(2, nick);
        q.bindValue(3, int(role));
        q.bindValue(4, mutedUntil);
        q.bindValue(5, meshlink::nowMs());
        if (!q.exec())
            setupFailed(QStringLiteral("insert member %1: %2").arg(nick, q.lastError().text()));
    };
    insertMember(peerFp, QStringLiteral("Peer Nick"), meshlink::MemberRole::Member, 0);
    insertMember(adminFp, QStringLiteral("Admin Nick"), meshlink::MemberRole::Admin, 0);
    insertMember(managedFp, QStringLiteral("Menu Peer"), meshlink::MemberRole::Member, 0);

    // Real surfaces always pass their owning widget: the remove action uses it
    // as the confirmation-dialog parent (and as a lifetime guard for async
    // admin responses). Mirror that here instead of a nullptr parent.
    QWidget menuParent;
    menuParent.resize(360, 240);
    menuParent.show();
    QApplication::setActiveWindow(&menuParent);

    QString mentioned;
    int changedCount = 0;
    auto buildMenu = [&](const QString &actor, const QString &target) {
        mentioned.clear();
        changedCount = 0;
        return meshlink::MemberMenu::create(
            &menuParent, engine.rooms(), roomId, actor, target,
            [&](const QString &nickname) { mentioned = nickname; },
            [&] { ++changedCount; });
    };

    // ---- message context menu: bubble, selectable text and quoted reply ------
    {
        const QString firstId =
            engine.chat()->sendText(roomId, QStringLiteral("Bubble text check"), &error);
        check(!firstId.isEmpty(), QStringLiteral("sent plain message (%1)").arg(error));
        check(!engine.chat()
                   ->sendText(roomId, QStringLiteral("Quoted reply check"), &error, firstId)
                   .isEmpty(),
              QStringLiteral("sent quoted reply (%1)").arg(error));

        meshlink::ChatView chat(&engine);
        chat.resize(900, 700);
        chat.show();
        QApplication::setActiveWindow(&chat);
        chat.activateWindow();
        chat.setRoom(roomId);
        processFor(300);

        const QList<QLabel *> textLabels =
            chat.findChildren<QLabel *>(QStringLiteral("chatView.messageText"));
        check(textLabels.size() >= 2,
              QStringLiteral("chat view renders both text bubbles (got %1)").arg(textLabels.size()));
        QLabel *text = textLabels.value(0);
        check(text != nullptr, "selectable message text label found");
        QWidget *bubble = text ? text->parentWidget() : nullptr;
        check(bubble && bubble->contextMenuPolicy() == Qt::CustomContextMenu,
              "bubble container owns the Qt::CustomContextMenu policy");

        auto *quoteBlock = chat.findChild<QWidget *>(QStringLiteral("chatView.quoteBlock"));
        check(quoteBlock != nullptr, "quoted reply renders a quote block");
        QLabel *quoteLabel = quoteBlock ? quoteBlock->findChild<QLabel *>() : nullptr;
        check(quoteLabel != nullptr, "quote block contains a child label");

        if (text) {
            const CapturedMenu captured =
                captureBlockingMenu([text] { rightClick(text); });
            check(captured.saw, "right-click on selectable message text opened a menu");
            check(captured.actions.contains(QStringLiteral("Reply")),
                  QStringLiteral("text right-click opened message actions (%1)")
                      .arg(captured.actions.join(QStringLiteral(", "))));
            check(!captured.actions.contains(QStringLiteral("Mention (@Host Nick)")),
                  "text right-click did not open the member menu");
        }

        if (bubble) {
            const CapturedMenu captured =
                captureBlockingMenu([bubble] { rightClick(bubble); });
            check(captured.saw, "right-click on the bubble container opened a menu");
            check(captured.actions.contains(QStringLiteral("Reply")),
                  QStringLiteral("bubble container opened message actions (%1)")
                      .arg(captured.actions.join(QStringLiteral(", "))));
        }

        if (quoteLabel) {
            const CapturedMenu captured =
                captureBlockingMenu([quoteLabel] { rightClick(quoteLabel); });
            check(captured.saw, "right-click on the quoted-reply widget opened a menu");
            check(captured.actions.contains(QStringLiteral("Reply")),
                  QStringLiteral("quote widget opened message actions (%1)")
                      .arg(captured.actions.join(QStringLiteral(", "))));
            check(!captured.actions.contains(QStringLiteral("Mention (@Host Nick)")),
                  "quote widget did not open the member menu");
        }

        // ---- avatar: member menu only ---------------------------------------
        auto *avatar = chat.findChild<QLabel *>(QStringLiteral("chatView.avatar"));
        check(avatar != nullptr, "message avatar label found");
        check(avatar && avatar->contextMenuPolicy() == Qt::CustomContextMenu,
              "avatar owns Qt::CustomContextMenu");
        if (avatar) {
            rightClick(avatar);
            processFor(150);
            const CapturedMenu captured = captureOpenPopup();
            check(captured.saw, "right-click on the avatar opened a menu");
            check(captured.actions.contains(QStringLiteral("Mention (@Host Nick)")),
                  QStringLiteral("avatar opened the member menu (%1)")
                      .arg(captured.actions.join(QStringLiteral(", "))));
            check(!captured.actions.contains(QStringLiteral("Reply")),
                  "avatar never triggers message actions");
            check(!captured.actions.contains(QStringLiteral("Copy text")),
                  "avatar never triggers the message copy action");
        }

        // Mention helper used by the avatar menu and management dialog.
        chat.mentionMember(QStringLiteral("Peer Nick"));
        auto *input = chat.findChild<QTextEdit *>(QStringLiteral("chatView.input"));
        check(input != nullptr, "chat composer found");
        check(input && input->toPlainText() == QStringLiteral("@Peer Nick "),
              QStringLiteral("mentionMember inserted '@Peer Nick ' (got '%1')")
                  .arg(input ? input->toPlainText() : QString()));
        check(input && input->hasFocus(), "mentionMember focused the composer");
        chat.close();
        processFor(50);
    }

    // ---- member menu contents / enabled states ------------------------------
    {
        QMenu *hostToMember = buildMenu(selfFp, peerFp);
        check(hostToMember != nullptr, "host -> member menu built");
        if (hostToMember) {
            QAction *mention = findAction(hostToMember, QStringLiteral("Mention"));
            check(mention && mention->text() == QStringLiteral("Mention (@Peer Nick)"),
                  "mention entry names the target nickname");
            check(mention && mention->isEnabled(), "host can mention a member");
            QAction *setAdmin = findAction(hostToMember, QStringLiteral("Set as admin"));
            check(setAdmin && setAdmin->isEnabled(), "host can set a member as admin");
            QAction *mute = findAction(hostToMember, QStringLiteral("Mute"));
            check(mute && mute->isEnabled(), "host can mute a member");
            QAction *tenMinutes = findAction(hostToMember, QStringLiteral("10 minutes"));
            QAction *oneHour = findAction(hostToMember, QStringLiteral("1 hour"));
            QAction *untilUnmuted =
                findAction(hostToMember, QStringLiteral("Until unmuted"));
            check(tenMinutes && tenMinutes->isEnabled(), "10-minute mute entry is available");
            check(oneHour && oneHour->isEnabled(), "1-hour mute entry is available");
            check(untilUnmuted && untilUnmuted->isEnabled(),
                  "until-unmuted mute entry is available");
            QAction *remove = findAction(hostToMember, QStringLiteral("Remove from room"));
            check(remove && remove->isEnabled(), "host can remove a member");
            mention->trigger();
            check(mentioned == QStringLiteral("Peer Nick"), "mention handler received nickname");
            delete hostToMember;
        }

        QMenu *hostToAdmin = buildMenu(selfFp, adminFp);
        check(hostToAdmin != nullptr, "host -> admin menu built");
        if (hostToAdmin) {
            QAction *revoke = findAction(hostToAdmin, QStringLiteral("Revoke admin"));
            check(revoke && revoke->isEnabled(), "host can revoke an administrator");
            check(findAction(hostToAdmin, QStringLiteral("Set as admin")) == nullptr,
                  "administrator target offers Revoke, not Set");
            delete hostToAdmin;
        }

        QMenu *plainActor = buildMenu(peerFp, adminFp);
        check(plainActor != nullptr, "plain-member actor menu built");
        if (plainActor) {
            QAction *revoke = findAction(plainActor, QStringLiteral("Revoke admin"));
            check(revoke && !revoke->isEnabled(),
                  "plain member cannot revoke an administrator");
            check(revoke && !revoke->toolTip().isEmpty(),
                  "disabled revoke explains the rejection reason");
            QAction *remove = findAction(plainActor, QStringLiteral("Remove from room"));
            check(remove && !remove->isEnabled(), "plain member cannot remove members");
            check(remove && !remove->toolTip().isEmpty(),
                  "disabled remove explains the rejection reason");
            QAction *mention = findAction(plainActor, QStringLiteral("Mention"));
            check(mention && mention->isEnabled(), "plain member can still mention");
            delete plainActor;
        }

        QMenu *adminActor = buildMenu(adminFp, peerFp);
        check(adminActor != nullptr, "administrator actor menu built");
        if (adminActor) {
            QAction *setAdmin = findAction(adminActor, QStringLiteral("Set as admin"));
            check(setAdmin && !setAdmin->isEnabled(),
                  "administrator cannot set another administrator (host-only)");
            QAction *mute = findAction(adminActor, QStringLiteral("Mute"));
            check(mute && mute->isEnabled(), "administrator can mute members");
            QAction *remove = findAction(adminActor, QStringLiteral("Remove from room"));
            check(remove && remove->isEnabled(), "administrator can remove members");
            delete adminActor;
        }

        QMenu *selfMenu = buildMenu(selfFp, selfFp);
        check(selfMenu != nullptr, "self-target menu built");
        if (selfMenu) {
            check(findAction(selfMenu, QStringLiteral("Mention (@Host Nick)")) != nullptr,
                  "self menu offers a mention entry");
            check(findAction(selfMenu, QStringLiteral("Set as admin")) == nullptr,
                  "host gets no Set-admin entry for itself");
            QAction *mute = findAction(selfMenu, QStringLiteral("Mute"));
            check(mute && !mute->isEnabled(), "self mute is disabled");
            QAction *remove = findAction(selfMenu, QStringLiteral("Remove from room"));
            check(remove && !remove->isEnabled(), "self remove is disabled");
            delete selfMenu;
        }

        // Muted target: Unmute replaces the Mute submenu.
        {
            QSqlQuery q = engine.storage()->query(
                "UPDATE members SET muted_until=? WHERE room_id=? AND fingerprint=?");
            q.bindValue(0, meshlink::nowMs() + 600000);
            q.bindValue(1, roomId);
            q.bindValue(2, peerFp);
            check(q.exec(), "fixture: mute peer for 10 minutes");
            QMenu *menu = buildMenu(selfFp, peerFp);
            QAction *unmute = findAction(menu, QStringLiteral("Unmute"));
            check(unmute && unmute->isEnabled(), "muted target offers Unmute");
            check(findAction(menu, QStringLiteral("Mute")) == nullptr,
                  "muted target does not offer another Mute");
            delete menu;
            QSqlQuery clear = engine.storage()->query(
                "UPDATE members SET muted_until=0 WHERE room_id=? AND fingerprint=?");
            clear.bindValue(0, roomId);
            clear.bindValue(1, peerFp);
            check(clear.exec(), "fixture: clear the mute");
        }
    }

    // ---- management dialog: avatar decorations + right-click member menu -----
    {
        meshlink::RoomsView rooms(&engine);
        rooms.resize(1000, 720);
        rooms.show();
        QApplication::setActiveWindow(&rooms);
        rooms.activateWindow();
        processFor(200);

        bool sawDialog = false;
        bool sawMembers = false;
        bool avatarsPresent = true;
        bool memberMenuSeen = false;
        QStringList managementMenu;
        QTimer::singleShot(350, [&] {
            QWidget *modal = visibleModal();
            if (!modal)
                return;
            sawDialog = true;
            for (QListView *view : modal->findChildren<QListView *>()) {
                QAbstractItemModel *model = view->model();
                if (!model || model->rowCount() < 2)
                    continue;
                int managedRow = -1;
                for (int row = 0; row < model->rowCount(); ++row) {
                    const QModelIndex index = model->index(row, 0);
                    if (index.data(Qt::UserRole).toString() == managedFp)
                        managedRow = row;
                    const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
                    if (icon.isNull())
                        avatarsPresent = false;
                }
                if (managedRow < 0)
                    continue;
                sawMembers = true;
                const QRect rect = view->visualRect(model->index(managedRow, 0));
                const QPoint local = rect.center();
                QContextMenuEvent event(QContextMenuEvent::Mouse, local,
                                        view->viewport()->mapToGlobal(local));
                QApplication::sendEvent(view->viewport(), &event);
                auto *popup = qobject_cast<QMenu *>(QApplication::activePopupWidget());
                if (!popup)
                    return;
                memberMenuSeen = true;
                managementMenu = actionTexts(popup);
                if (QAction *mention =
                        findAction(popup, QStringLiteral("Mention (@Menu Peer)")))
                    mention->trigger();
                if (popup)
                    popup->close();
                return;
            }
        });

        rooms.openManagementDialog(roomId);
        // Offscreen has no window manager: restore activation like the real
        // main window does when the modal dialog closes.
        QApplication::setActiveWindow(&rooms);
        rooms.activateWindow();
        processFor(300);

        check(sawDialog, "room management dialog opened");
        check(sawMembers, "members tab lists the room members");
        check(avatarsPresent, "every member row carries a non-null avatar decoration");
        check(memberMenuSeen, "right-click on a member row opened the member menu");
        check(managementMenu.contains(QStringLiteral("Mention (@Menu Peer)")),
              QStringLiteral("management menu offers Mention (%1)")
                  .arg(managementMenu.join(QStringLiteral(", "))));
        check(managementMenu.contains(QStringLiteral("Remove from room")),
              "management menu offers Remove from room");
        auto *managementInput =
            rooms.findChild<QTextEdit *>(QStringLiteral("chatView.input"));
        check(managementInput != nullptr, "rooms view composer exists");
        check(managementInput
                  && managementInput->toPlainText() == QStringLiteral("@Menu Peer "),
              QStringLiteral("management mention reached the composer (got '%1')")
                  .arg(managementInput ? managementInput->toPlainText() : QString()));
        check(rooms.currentRoomId() == roomId, "management mention selected the room");
    }

    // ---- moderation actions really go through RoomManager --------------------
    {
        // Set admin / revoke admin
        QMenu *menu = buildMenu(selfFp, peerFp);
        QAction *setAdmin = findAction(menu, QStringLiteral("Set as admin"));
        if (setAdmin)
            setAdmin->trigger();
        delete menu;
        processFor(100);
        check(engine.rooms()->roleOf(roomId, peerFp) == meshlink::MemberRole::Admin,
              "Set as admin reached RoomManager::applyAdminOp");
        check(changedCount > 0, "successful admin op invoked the refresh callback");

        menu = buildMenu(selfFp, peerFp);
        QAction *revoke = findAction(menu, QStringLiteral("Revoke admin"));
        if (revoke)
            revoke->trigger();
        delete menu;
        processFor(100);
        check(engine.rooms()->roleOf(roomId, peerFp) == meshlink::MemberRole::Member,
              "Revoke admin reached RoomManager::applyAdminOp");

        // Mute with the selected duration
        menu = buildMenu(selfFp, peerFp);
        QAction *tenMinutes = findAction(menu, QStringLiteral("10 minutes"));
        const qint64 before = meshlink::nowMs();
        if (tenMinutes)
            tenMinutes->trigger();
        delete menu;
        processFor(100);
        qint64 mutedUntil = 0;
        for (const meshlink::RoomMemberInfo &member : engine.rooms()->members(roomId)) {
            if (member.fp == peerFp)
                mutedUntil = member.mutedUntil;
        }
        check(mutedUntil >= before + 9 * 60 * 1000 && mutedUntil <= before + 11 * 60 * 1000,
              QStringLiteral("10-minute mute duration reached RoomManager (until=%1)")
                  .arg(mutedUntil));
        check(changedCount > 0, "mute invoked the refresh callback");

        menu = buildMenu(selfFp, peerFp);
        QAction *unmute = findAction(menu, QStringLiteral("Unmute"));
        if (unmute)
            unmute->trigger();
        delete menu;
        processFor(100);
        bool muted = false;
        for (const meshlink::RoomMemberInfo &member : engine.rooms()->members(roomId)) {
            if (member.fp == peerFp && member.mutedUntil != 0)
                muted = true;
        }
        check(!muted, "Unmute reached RoomManager");

        // Remove: the confirmation dialog is part of the production path.
        menu = buildMenu(selfFp, peerFp);
        QAction *remove = findAction(menu, QStringLiteral("Remove from room"));
        bool confirmSeen = false;
        QTimer::singleShot(150, [&] {
            QWidget *modal = visibleModal();
            if (!modal)
                return;
            confirmSeen = true;
            clickButton(modal, QStringLiteral("Remove"));
        });
        QTimer::singleShot(5000, [] {
            if (QWidget *modal = visibleModal())
                modal->close();
        });
        if (remove)
            remove->trigger(); // blocks in UiUtils::confirm until the timer clicks
        delete menu;
        processFor(250);
        check(confirmSeen, "Remove from room asked for confirmation");
        bool stillMember = false;
        for (const meshlink::RoomMemberInfo &member : engine.rooms()->members(roomId)) {
            if (member.fp == peerFp)
                stillMember = true;
        }
        check(!stillMember, "Remove from room deleted the member through RoomManager");
        check(changedCount > 0, "remove invoked the refresh callback");
    }

    std::printf("tst_chat_menus: %d check(s), %d failure(s)\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("tst_chat_menus: ALL OK\n");
        return 0;
    }
    std::fprintf(stderr, "tst_chat_menus: FAILED\n");
    return 1;
}
