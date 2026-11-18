// deepin-MeshLink — main window implementation.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "mainwindow.h"

#include "chatview.h"
#include "discoverview.h"
#include "firststartwizard.h"
#include "notificationcenter.h"
#include "profilepage.h"
#include "roomsview.h"
#include "settingsdialog.h"
#include "transfersview.h"
#include "uiutils.h"
#include "xdgtrayicon.h"

#include <DDialog>
#include <DFontSizeManager>
#include <DLabel>
#include <DFrame>
#include <DPaletteHelper>
#include <DTitlebar>
#include <DToolButton>

#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QHash>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

DWIDGET_USE_NAMESPACE

namespace meshlink {

// Square navigation button: icon above the label, palette-aware selected and
// hover states. Text can be hidden at runtime; the label remains the tooltip.
class NavButton : public DTK_WIDGET_NAMESPACE::DToolButton {
public:
    explicit NavButton(QWidget *parent = nullptr) : DToolButton(parent) {
        setCheckable(true);
        setFixedSize(72, 72);
        setIconSize(QSize(24, 24));
        setShowText(true);
    }

    void setShowText(bool show) {
        m_showText = show;
        setProperty("showText", show);
        update();
    }

protected:
    void enterEvent(QEnterEvent *event) override {
        m_hover = true;
        update();
        DToolButton::enterEvent(event);
    }
    void leaveEvent(QEvent *event) override {
        m_hover = false;
        update();
        DToolButton::leaveEvent(event);
    }
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const DPalette palette = DPaletteHelper::instance()->palette(this);
        const QRect backgroundRect = rect().adjusted(2, 2, -2, -2);
        if (isChecked()) {
            QColor background = palette.color(DPalette::Highlight);
            background.setAlphaF(0.18);
            painter.setPen(Qt::NoPen);
            painter.setBrush(background);
            painter.drawRoundedRect(backgroundRect, 8, 8);
        } else if (m_hover && isEnabled()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(palette.color(DPalette::ItemBackground));
            painter.drawRoundedRect(backgroundRect, 8, 8);
        }

        const QSize requested = iconSize();
        const int iconSide = qMax(1, qMin(requested.width(), requested.height()));
        const QRect iconRect((width() - iconSide) / 2,
                             m_showText ? 8 : (height() - iconSide) / 2, iconSide,
                             iconSide);
        QIcon::Mode mode = isEnabled() ? QIcon::Normal : QIcon::Disabled;
        icon().paint(&painter, iconRect, Qt::AlignCenter, mode, QIcon::Off);

        if (!m_showText)
            return;
        painter.setFont(DFontSizeManager::instance()->t8(font()));
        painter.setPen(isChecked() ? palette.color(DPalette::Highlight)
                                   : (isEnabled() ? palette.color(DPalette::Text)
                                                  : palette.color(DPalette::TextTips)));
        const QString label = fontMetrics().elidedText(text(), Qt::ElideRight, width() - 8);
        painter.drawText(QRect(0, 39, width(), height() - 40), Qt::AlignHCenter | Qt::AlignTop,
                         label);
    }

private:
    bool m_showText = true;
    bool m_hover = false;
};

MainWindow::MainWindow(Engine *engine, QWidget *parent) : DMainWindow(parent), m_engine(engine) {
    setWindowTitle(QCoreApplication::translate("MainWindow", "deepin-MeshLink"));
    setMinimumSize(900, 600);
    resize(1120, 760);

    setupTitlebar();

    auto *central = new QWidget(this);
    auto *root = new QHBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ---- sidebar: narrow square navigation rail ----
    auto *sidebar = new QWidget(central);
    sidebar->setObjectName(QStringLiteral("mainWindow.sidebar"));
    sidebar->setFixedWidth(88);
    m_sidebar = sidebar;
    auto *sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(8, 16, 8, 16);
    sideLayout->setSpacing(8);

    m_navGroup = new QButtonGroup(this);
    m_navGroup->setExclusive(true);

    auto addNav = [this, sidebar, sideLayout](Page page, const QString &text,
                                              const QString &iconName, const QString &fallback) {
        auto *button = new NavButton(sidebar);
        button->setObjectName(QStringLiteral("mainWindow.nav%1").arg(int(page)));
        button->setText(text);
        button->setToolTip(text);
        button->setIcon(UiUtils::icon(iconName, fallback));
        button->setIconSize(QSize(24, 24));
        button->setCheckable(true);
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setFixedSize(72, 72);
        m_navGroup->addButton(button, int(page));
        m_navButtons.append(button);
        sideLayout->addWidget(button, 0, Qt::AlignHCenter);
    };
    addNav(Page::Chats, tr("Chats"), QStringLiteral("chat"), QStringLiteral("message"));
    addNav(Page::Discover, tr("Discover"), QStringLiteral("network-wired"),
           QStringLiteral("network-workgroup"));
    addNav(Page::Transfers, tr("Transfers"), QStringLiteral("folder-download"),
           QStringLiteral("download"));
    addNav(Page::Profile, tr("Profile"), QStringLiteral("user-info"), QStringLiteral("contact-new"));

    sideLayout->addStretch(1);

    // The settings entry follows the rail's show/hide-label mode just like the
    // page buttons; it is not part of the exclusive page selection group.
    auto *settingsButton = new NavButton(sidebar);
    settingsButton->setCheckable(false);
    settingsButton->setObjectName(QStringLiteral("mainWindow.settingsButton"));
    settingsButton->setText(tr("Settings"));
    settingsButton->setToolTip(tr("Settings"));
    settingsButton->setIcon(UiUtils::icon(QStringLiteral("preferences-system"),
                                          QStringLiteral("settings")));
    settingsButton->setIconSize(QSize(22, 22));
    connect(settingsButton, &NavButton::clicked, this, [this] { openSettings(); });
    m_settingsButton = settingsButton;
    sideLayout->addWidget(settingsButton, 0, Qt::AlignHCenter);

    auto *separator = new DVerticalLine(central);

    // ---- pages ----
    m_stack = new QStackedWidget(central);
    m_rooms = new RoomsView(m_engine, m_stack);
    m_discover = new DiscoverView(m_engine, m_stack);
    m_transfers = new TransfersView(m_engine, m_stack);
    m_profile = new ProfilePage(m_engine, m_stack);
    m_stack->addWidget(m_rooms);
    m_stack->addWidget(m_discover);
    m_stack->addWidget(m_transfers);
    m_stack->addWidget(m_profile);

    root->addWidget(sidebar);
    root->addWidget(separator);
    root->addWidget(m_stack, 1);
    setCentralWidget(central);

    connect(m_navGroup, &QButtonGroup::idClicked, this,
            [this](int id) { m_stack->setCurrentIndex(id); });
    connect(m_engine->settings(), &Settings::changed, this,
            &MainWindow::applyNavLabelVisibility);

    // ---- cross-page navigation ----
    connect(m_discover, &DiscoverView::createRoomRequested, this, [this] {
        selectPage(Page::Chats);
        m_rooms->openCreateDialog();
    });
    connect(m_discover, &DiscoverView::settingsRequested, this,
            [this](const QString &group) { openSettings(group); });
    connect(m_rooms, &RoomsView::downloadsStarted, this,
            [this] { selectPage(Page::Transfers); });
    connect(m_profile, &ProfilePage::accountDestroyed, this,
            [this](int remainingAccounts) {
                // Let the destroy confirmation dialog finish unwinding before a
                // possible nested modal (first-start wizard) is opened.
                QTimer::singleShot(0, this, [this, remainingAccounts] {
                    handleAccountDestroyed(remainingAccounts);
                });
            });

    // ---- global status / security notices ----
    setupStatusBar();
    refreshStatus();

    QTimer::singleShot(0, this, [this] { refreshStatus(); });
    connect(m_engine->discovery(), &Discovery::peerSeen, this, &MainWindow::refreshStatus);
    connect(m_engine->discovery(), &Discovery::peerGone, this, &MainWindow::refreshStatus);
    connect(m_engine->discovery(), &Discovery::peersChanged, this, &MainWindow::refreshStatus);
    connect(m_engine->rooms(), &RoomManager::roomsChanged, this, &MainWindow::refreshStatus);
    connect(m_engine, &Engine::lanSegmentChanged, this, &MainWindow::refreshStatus);
    connect(m_engine, &Engine::started, this, [this](bool ok) {
        if (!ok)
            return;
        refreshStatus();
        updateTrayTitle();
    });

    connect(m_engine, &Engine::spoofingSuspected, this, [this](const QString &fp) {
        showSecurityWarning(tr("Spoofed peer suspected: %1").arg(UiUtils::shortFp(fp, 12)));
    });
    connect(m_engine->link(), &SecureLink::mitmSuspected, this,
            [this](const QString &fp, const QString &detail) {
                const QString text =
                    tr("Certificate mismatch for node %1: %2")
                        .arg(UiUtils::shortFp(fp, 16), detail);
                showSecurityWarning(text);
                DDialog dialog(this);
                dialog.setTitle(tr("Security warning"));
                dialog.setMessage(text);
                dialog.setIcon(UiUtils::icon(QStringLiteral("dialog-warning")));
                dialog.addButton(tr("OK"), true, DDialog::ButtonWarning);
                dialog.exec();
            });

    applyNavLabelVisibility();
    selectPage(Page::Chats);
    setupTrayAndNotifications();
}

MainWindow::~MainWindow() = default;

void MainWindow::setupTrayAndNotifications() {
    // ---- notifications: policy + XDG delivery ----
    m_notifier = new NotificationCenter(
        m_engine, [this] { return isVisible() && isActiveWindow(); },
        [this] { return m_rooms ? m_rooms->currentRoomId() : QString(); }, this);
    connect(m_notifier, &NotificationCenter::activateRoom, this, [this](const QString &roomId) {
        showWindow();
        selectPage(Page::Chats);
        m_rooms->selectRoom(roomId);
    });
    connect(m_rooms, &RoomsView::roomActivated, m_notifier, &NotificationCenter::clearRoom);
    connect(m_notifier, &NotificationCenter::unreadMentionsChanged, this,
            [this](int total, const QHash<QString, int> &counts) {
                m_rooms->setMentionCounts(counts);
                m_unreadMentions = total;
                updateTrayTitle();
            });

    // ---- XDG tray icon (StatusNotifierItem), gated by behavior/trayEnabled ----
    connect(m_engine->settings(), &Settings::changed, this,
            [this] { updateTrayPresence(); });
    updateTrayPresence();
}

void MainWindow::updateTrayPresence() {
    const bool wanted = m_engine->settings()->trayEnabled();
    if (wanted && !m_tray) {
        createTray();
        updateTrayTitle();
    } else if (!wanted && m_tray) {
        m_tray->deleteLater();
        m_tray = nullptr;
    }
}

void MainWindow::createTray() {
    m_tray = new XdgTrayIcon(this);
    m_tray->setIconName(QStringLiteral("cn.org.huansoft.deepin-meshlink"));
    m_tray->setMenuItems({
        {1, tr("Show main window")},
        {2, tr("Settings")},
        {0, QString()}, // separator
        {3, tr("Quit")},
    });
    connect(m_tray, &XdgTrayIcon::activateRequested, this, [this] {
        if (isVisible() && isActiveWindow())
            hide();
        else
            showWindow();
    });
    connect(m_tray, &XdgTrayIcon::menuTriggered, this, [this](int id) {
        switch (id) {
        case 1:
            showWindow();
            break;
        case 2:
            showWindow();
            openSettings();
            break;
        case 3:
            qApp->quit();
            break;
        default:
            break;
        }
    });
    connect(m_tray, &XdgTrayIcon::availableChanged, this, [this](bool) { updateTrayTitle(); });
    updateTrayTitle();
}

void MainWindow::updateTrayTitle() {
    if (!m_tray)
        return;
    const QString base = tr("deepin-MeshLink");
    m_tray->setTitle(m_unreadMentions > 0
                         ? QStringLiteral("%1 (@%2)").arg(base).arg(m_unreadMentions)
                         : base);
}

void MainWindow::showWindow() {
    show();
    raise();
    activateWindow();
    setWindowState(windowState() & ~Qt::WindowMinimized);
    if (m_notifier)
        m_notifier->clearAll();
}

void MainWindow::closeEvent(QCloseEvent *event) {
    const bool trayUsable = m_tray && m_tray->available();
    const auto action = m_engine->settings()->closeAction();
    if (!trayUsable || action == Settings::CloseAction::Exit) {
        event->accept();
        return;
    }
    if (action == Settings::CloseAction::Minimize) {
        event->ignore();
        hide();
        return;
    }
    // Ask: minimize to tray or exit (rememberable).
    DDialog dialog(this);
    dialog.setTitle(tr("Close deepin-MeshLink?"));
    dialog.setMessage(tr("The app can keep running in the system tray so you still receive "
                         "messages and @-mentions."));
    dialog.setIcon(UiUtils::icon(QStringLiteral("dialog-question")));
    auto *remember = new QCheckBox(tr("Remember my choice"), &dialog);
    dialog.addContent(remember);
    dialog.addButton(tr("Exit"), true, DDialog::ButtonWarning);
    dialog.addButton(tr("Minimize to tray"), false, DDialog::ButtonRecommend);
    const int choice = dialog.exec();
    if (choice == 1) { // minimize
        if (remember->isChecked())
            m_engine->settings()->setCloseAction(Settings::CloseAction::Minimize);
        event->ignore();
        hide();
        return;
    }
    if (choice == 0) { // exit
        if (remember->isChecked())
            m_engine->settings()->setCloseAction(Settings::CloseAction::Exit);
        event->accept();
        return;
    }
    event->ignore(); // cancel
}

void MainWindow::setupTitlebar() {
    auto *bar = titlebar();
    if (!bar)
        return;
    bar->setTitle(tr("deepin-MeshLink"));
    bar->setIcon(qApp->windowIcon());

    auto *menu = new QMenu(this);
    auto *settings = menu->addAction(UiUtils::icon(QStringLiteral("preferences-system"),
                                                   QStringLiteral("settings")),
                                     tr("Settings"));
    settings->setShortcut(QKeySequence(QStringLiteral("Ctrl+,")));
    settings->setMenuRole(QAction::PreferencesRole);
    connect(settings, &QAction::triggered, this, [this] { openSettings(); });
    addAction(settings); // make the shortcut work while the menu is closed
    menu->addSeparator();
    bar->setMenu(menu);
    // DTitlebar appends the standard Theme/Help/About/Quit items on first show.
}

void MainWindow::setupStatusBar() {
    auto *bar = new QStatusBar(this);
    bar->setSizeGripEnabled(false);

    m_statusLabel = new DLabel(bar);
    m_statusLabel->setForegroundRole(DPalette::TextTips);
    m_segmentLabel = new DLabel(bar);
    m_segmentLabel->setForegroundRole(DPalette::TextTips);
    m_warnLabel = new DLabel(bar);
    m_warnLabel->setForegroundRole(DPalette::TextWarning);
    m_warnLabel->setElideMode(Qt::ElideRight);
    m_warnLabel->hide();

    bar->addWidget(m_statusLabel, 1);
    bar->addPermanentWidget(m_segmentLabel);
    bar->addPermanentWidget(m_warnLabel);
    setStatusBar(bar);
}

void MainWindow::selectPage(Page page) {
    const int row = int(page);
    if (m_navGroup) {
        if (auto *button = m_navGroup->button(row))
            button->setChecked(true);
    }
    if (m_stack)
        m_stack->setCurrentIndex(row);
}

void MainWindow::applyNavLabelVisibility() {
    if (!m_engine || !m_engine->settings())
        return;
    const bool hide = m_engine->settings()->hideNavLabels();
    const QSize size = hide ? QSize(56, 56) : QSize(72, 72);
    for (auto *button : m_navButtons) {
        if (!button)
            continue;
        button->setShowText(!hide);
        button->setFixedSize(size);
    }
    if (m_settingsButton) {
        m_settingsButton->setShowText(!hide);
        m_settingsButton->setFixedSize(size);
    }
    if (m_sidebar)
        m_sidebar->setFixedWidth(hide ? 64 : 88);
}

void MainWindow::openSettings(const QString &group) {
    if (!m_settingsDialog) {
        m_settingsDialog = new SettingsDialog(m_engine, this);
        if (!m_settingsDialog->isValid()) {
            UiUtils::warning(this, tr("Settings"),
                             tr("The settings description could not be loaded."));
            delete m_settingsDialog;
            m_settingsDialog = nullptr;
            return;
        }
    }
    if (m_settingsDialog->isVisible()) {
        m_settingsDialog->raise();
        m_settingsDialog->activateWindow();
        return;
    }

    m_settingsDialog->refreshLanSegments();
    m_settingsDialog->show();
    if (!group.isEmpty())
        m_settingsDialog->scrollToGroup(group);
    m_settingsDialog->exec();
    if (m_settingsDialog->takeRestartRequired())
        showRestartDialog();
}

void MainWindow::showRestartDialog() {
    DDialog dialog(this);
    dialog.setTitle(tr("Restart required"));
    dialog.setMessage(tr("Discovery and TLS port changes take effect after the application restarts."));
    dialog.setIcon(UiUtils::icon(QStringLiteral("dialog-information")));
    dialog.addButton(tr("Quit now"), true, DDialog::ButtonRecommend);
    dialog.addButton(tr("Later"));
    if (dialog.exec() == 0)
        qApp->quit();
}

void MainWindow::showSecurityWarning(const QString &text) {
    if (!m_warnLabel)
        return;
    m_warnLabel->setText(text);
    m_warnLabel->setToolTip(text);
    m_warnLabel->show();
}

void MainWindow::handleAccountDestroyed(int remainingAccounts) {
    if (remainingAccounts > 0) {
        // Multi-account: keep the session alive on a surviving identity.
        QString error;
        if (!switchToRemainingAccount(&error)) {
            UiUtils::warning(this, tr("Account switch failed"),
                             error.isEmpty() ? tr("The account switch failed.") : error);
        }
        refreshAfterAccountChange();
        return;
    }

    // UC-02 step 7: with no identity left the app returns to the first-start
    // state in-process. Storage was closed by Engine::destroyAccount(); the
    // wizard's create flow reopens it and generates a fresh identity.
    if (!FirstStartWizard::ensureAccount(m_engine, this)) {
        // The wizard's "Quit" button (or closing it) is the only explicit way
        // to leave here; destroying an account must never quit on its own.
        qApp->quit();
        return;
    }
    QString error;
    const bool started = m_engine->start(&error);
    refreshAfterAccountChange();
    if (!started)
        showStartupError(error);
}

bool MainWindow::switchToRemainingAccount(QString *error) {
    // Engine::destroyAccount() closes storage while wiping, so the account
    // table cannot be queried until it is reopened.
    if (!m_engine->storage()->isOpen()
        && !m_engine->storage()->open(Paths::databaseFile(), error))
        return false;
    const auto remaining = m_engine->account()->listAccounts();
    if (remaining.isEmpty()) {
        if (error)
            *error = tr("No local account");
        return false;
    }
    if (!m_engine->switchAccount(remaining.first().id, error))
        return false;
    // destroyAccount() stopped networking; switchAccount() only rebinds when
    // the network was running, so bring it back on the surviving identity.
    if (!m_engine->networkRunning() && !m_engine->start(error))
        return false;
    return true;
}

void MainWindow::refreshAfterAccountChange() {
    // @-mention badges and the tray title belong to the previous identity.
    m_unreadMentions = 0;
    if (m_notifier)
        m_notifier->clearAll();
    if (m_profile)
        m_profile->reload();
    if (m_rooms)
        m_rooms->setMentionCounts({}); // public entry point re-renders the room list
    refreshStatus();
    updateTrayTitle();
}

void MainWindow::showStartupError(const QString &error) {
    QTimer::singleShot(0, this, [this, error] {
        const Engine::StartStage stage = m_engine->startStage();
        // UC-01 A1: identity unreadable -> explicit account rebuild.
        if (stage == Engine::StartStage::Identity) {
            FirstStartWizard::runAsRecovery(m_engine, this);
            return;
        }

        const bool storage = stage == Engine::StartStage::Storage;
        DDialog dialog(this);
        dialog.setTitle(tr("Could not start the network service"));
        dialog.setMessage(
            storage ? tr("%1\nCheck the data directory permissions and try again.").arg(error)
                    : tr("%1\nCheck the discovery and TLS ports in Settings, then restart the "
                         "application.")
                          .arg(error));
        dialog.setIcon(UiUtils::icon(QStringLiteral("dialog-warning")));
        if (!storage)
            dialog.addButton(tr("Open settings"), true, DDialog::ButtonRecommend);
        dialog.addButton(tr("Quit"));
        connect(&dialog, &DDialog::buttonClicked, this, [this, &dialog, storage](int index, const QString &) {
            if (!storage && index == 0) {
                dialog.close();
                QTimer::singleShot(0, this, [this] { openSettings(QStringLiteral("network.tls")); });
            } else {
                qApp->quit();
            }
        });
        dialog.exec();
    });
}

void MainWindow::refreshStatus() {
    const int online = m_engine->discovery()->peers().size();
    int hosted = 0;
    for (const auto &room : m_engine->rooms()->myRooms()) {
        if (room.hosted)
            ++hosted;
    }
    m_statusLabel->setText(tr("Online nodes: %1 · Hosted rooms: %2").arg(online).arg(hosted));

    const LanSegment segment = m_engine->lanSegment();
    const QString segmentText = segment.isValid()
                                    ? QStringLiteral("%1/%2").arg(segment.ip).arg(segment.prefixLength)
                                    : tr("unselected");
    m_segmentLabel->setText(tr("Segment: %1").arg(segmentText));
}

} // namespace meshlink
