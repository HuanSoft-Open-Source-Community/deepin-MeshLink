// deepin-MeshLink — rooms sidebar + management dialogs (UC-05/06/07/13).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#pragma once

#include <QWidget>

class QListWidget;

namespace meshlink {

class ChatView;
class Engine;

class RoomsView : public QWidget {
    Q_OBJECT
public:
    RoomsView(Engine *engine, QWidget *parent = nullptr);

    void openCreateDialog();            // UC-05
    void openManagementDialog(const QString &roomId); // UC-07

signals:
    void downloadsStarted();

private:
    void refreshRooms();
    void leaveSelected(); // UC-13

    Engine *m_engine;
    QListWidget *m_roomList = nullptr;
    ChatView *m_chat = nullptr;
};

} // namespace meshlink
