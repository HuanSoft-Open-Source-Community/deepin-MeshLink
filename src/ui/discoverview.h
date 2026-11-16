// deepin-MeshLink — discovery view: LAN peers + joinable rooms (UC-04/05/06).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#pragma once

#include <QWidget>

class QTableWidget;

namespace meshlink {

class Engine;

class DiscoverView : public QWidget {
    Q_OBJECT
public:
    DiscoverView(Engine *engine, QWidget *parent = nullptr);

private:
    void refreshPeers();
    void refreshRooms();
    void joinSelected();
    void createRoomHere();

    Engine *m_engine;
    QTableWidget *m_peers = nullptr;
    QTableWidget *m_rooms = nullptr;

signals:
    void createRoomRequested(); // MainWindow forwards to RoomsView dialog
};

} // namespace meshlink
