// deepin-MeshLink — discovery view: LAN peers + joinable rooms (UC-04/05/06).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#pragma once

#include "core/discovery.h"

#include <QWidget>

#include <dtkwidget_global.h>

class QLabel;
class QStandardItemModel;
class QTableView;

DWIDGET_BEGIN_NAMESPACE
class DLabel;
class DLineEdit;
DWIDGET_END_NAMESPACE

namespace meshlink {

class Engine;

class DiscoverView : public QWidget {
    Q_OBJECT
public:
    explicit DiscoverView(Engine *engine, QWidget *parent = nullptr);

signals:
    void createRoomRequested();              // MainWindow forwards to RoomsView dialog
    void settingsRequested(const QString &group);

private:
    void refreshPeers();
    void refreshRooms();
    void refreshSegment();
    void joinSelected();
    void joinByInviteCode();
    void joinSummary(const RoomSummary &summary);
    void createRoomHere();

    Engine *m_engine;
    DTK_WIDGET_NAMESPACE::DLabel *m_segment = nullptr;
    QTableView *m_peers = nullptr;
    QStandardItemModel *m_peerModel = nullptr;
    QTableView *m_rooms = nullptr;
    QStandardItemModel *m_roomModel = nullptr;
    DTK_WIDGET_NAMESPACE::DLineEdit *m_inviteCode = nullptr;
};

} // namespace meshlink
