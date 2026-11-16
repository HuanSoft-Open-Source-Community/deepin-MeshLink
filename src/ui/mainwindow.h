// deepin-MeshLink — main window: navigation shell, status bar, global notices.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#pragma once

#include "DMainWindow"

#include "core/engine.h"

class QLabel;
class QListWidget;
class QStackedWidget;

namespace meshlink {

class DiscoverView;
class RoomsView;
class TransfersView;
class ProfilePage;
class SettingsView;

class MainWindow : public DTK_WIDGET_NAMESPACE::DMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(Engine *engine, QWidget *parent = nullptr);

    void showStartupError(const QString &error); // UC-01 A1 / UC-04 E1 surface

private slots:
    void refreshStatus();

private:
    Engine *m_engine;
    QListWidget *m_nav = nullptr;
    QStackedWidget *m_stack = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_warnLabel = nullptr;
    DiscoverView *m_discover = nullptr;
    RoomsView *m_rooms = nullptr;
    TransfersView *m_transfers = nullptr;
    ProfilePage *m_profile = nullptr;
    SettingsView *m_settings = nullptr;
};

} // namespace meshlink
