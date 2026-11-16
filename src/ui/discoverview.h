// deepin-MeshLink — discovery view: LAN peers + joinable rooms (UC-04/05/06).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#pragma once

#include "core/discovery.h"

#include <QWidget>

class QTableWidget;
class QLabel;
class QLineEdit;

namespace meshlink {

class Engine;

class DiscoverView : public QWidget {
    Q_OBJECT
public:
    DiscoverView(Engine *engine, QWidget *parent = nullptr);

private:
    void refreshPeers();
    void refreshRooms();
    void refreshSegment();
    void joinSelected();
    void joinByInviteCode();
    void joinSummary(const RoomSummary &summary);
    void createRoomHere();

    Engine *m_engine;
    QLabel *m_segment = nullptr;
    QTableWidget *m_peers = nullptr;
    QTableWidget *m_rooms = nullptr;
    QLineEdit *m_inviteCode = nullptr;

signals:
    void createRoomRequested(); // MainWindow forwards to RoomsView dialog
};

} // namespace meshlink
