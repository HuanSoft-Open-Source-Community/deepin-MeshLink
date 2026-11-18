// deepin-MeshLink — main window: navigation shell, status bar, global notices.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#pragma once

#include "DMainWindow"

#include "core/engine.h"

class QButtonGroup;
class QCloseEvent;
class QLabel;
class QStackedWidget;
class QWidget;

DWIDGET_BEGIN_NAMESPACE
class DLabel;
DWIDGET_END_NAMESPACE

namespace meshlink {

class DiscoverView;
class NavButton;
class NotificationCenter;
class RoomsView;
class TransfersView;
class ProfilePage;
class SettingsDialog;
class XdgTrayIcon;

class MainWindow : public DTK_WIDGET_NAMESPACE::DMainWindow {
    Q_OBJECT
public:
    enum class Page { Chats = 0, Discover = 1, Transfers = 2, Profile = 3 };

    explicit MainWindow(Engine *engine, QWidget *parent = nullptr);
    ~MainWindow() override;

    void showStartupError(const QString &error); // UC-01 A1 / UC-04 E1 surface
    void selectPage(Page page);
    void openSettings(const QString &group = QString());

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void refreshStatus();

private:
    void setupTitlebar();
    void setupStatusBar();
    void setupTrayAndNotifications();
    void createTray();
    void updateTrayPresence();
    void applyNavLabelVisibility();
    void showRestartDialog();
    void showSecurityWarning(const QString &text);
    void showWindow();
    void updateTrayTitle();
    void handleAccountDestroyed(int remainingAccounts);
    bool switchToRemainingAccount(QString *error);
    void refreshAfterAccountChange();

    Engine *m_engine;
    QWidget *m_sidebar = nullptr;
    QButtonGroup *m_navGroup = nullptr;
    QVector<NavButton *> m_navButtons;
    NavButton *m_settingsButton = nullptr;
    QStackedWidget *m_stack = nullptr;
    DTK_WIDGET_NAMESPACE::DLabel *m_statusLabel = nullptr;
    DTK_WIDGET_NAMESPACE::DLabel *m_segmentLabel = nullptr;
    DTK_WIDGET_NAMESPACE::DLabel *m_warnLabel = nullptr;
    DiscoverView *m_discover = nullptr;
    RoomsView *m_rooms = nullptr;
    TransfersView *m_transfers = nullptr;
    ProfilePage *m_profile = nullptr;
    SettingsDialog *m_settingsDialog = nullptr;
    XdgTrayIcon *m_tray = nullptr;
    NotificationCenter *m_notifier = nullptr;
    int m_unreadMentions = 0;
};

} // namespace meshlink
