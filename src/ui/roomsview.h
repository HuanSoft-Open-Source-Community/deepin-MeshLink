// deepin-MeshLink — rooms sidebar + management dialogs (UC-05/06/07/13).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#pragma once

#include <QHash>
#include <QWidget>

#include <dtkwidget_global.h>

class QLabel;
class QStandardItemModel;

DWIDGET_BEGIN_NAMESPACE
class DLabel;
class DListView;
DWIDGET_END_NAMESPACE

namespace meshlink {

class ChatView;
class Engine;

class RoomsView : public QWidget {
    Q_OBJECT
public:
    explicit RoomsView(Engine *engine, QWidget *parent = nullptr);

    void openCreateDialog();                          // UC-05
    void openManagementDialog(const QString &roomId); // UC-07/UC-13
    QString currentRoomId() const;
    // Jump to a room (desktop-notification activation) and clear its badge.
    void selectRoom(const QString &roomId);
    // Unread @-mention counters per room (rendered as row badges).
    void setMentionCounts(const QHash<QString, int> &counts);

signals:
    void downloadsStarted();
    void roomActivated(const QString &roomId); // badge reset trigger

private:
    void refreshRooms();
    void showRoomContextMenu(const QPoint &globalPos, const QString &roomId);

    Engine *m_engine;
    DTK_WIDGET_NAMESPACE::DListView *m_roomList = nullptr;
    QStandardItemModel *m_roomModel = nullptr;
    DTK_WIDGET_NAMESPACE::DLabel *m_emptyLabel = nullptr;
    ChatView *m_chat = nullptr;
    QHash<QString, int> m_mentions; // roomId -> unread mention count
    QString m_pendingSelection;
};

} // namespace meshlink
