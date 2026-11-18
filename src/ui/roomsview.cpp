// deepin-MeshLink — rooms sidebar implementation.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "roomsview.h"

#include "chatview.h"
#include "core/account.h"
#include "core/chatmanager.h"
#include "core/engine.h"
#include "core/roommanager.h"
#include "membermenu.h"
#include "uiutils.h"

#include <DComboBox>
#include <DDialog>
#include <DFrame>
#include <DFontSizeManager>
#include <DIconButton>
#include <DLabel>
#include <DLineEdit>
#include <DListView>
#include <DPushButton>
#include <DSpinBox>
#include <DStandardItem>
#include <DStyledItemDelegate>
#include <DSuggestButton>
#include <DWarningButton>
#include <dinputdialog.h>

#include <QCursor>
#include <QDialog>
#include <QFormLayout>
#include <QClipboard>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QItemSelectionModel>
#include <QMenu>
#include <QPointer>
#include <QSet>
#include <QSplitter>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <memory>

DWIDGET_USE_NAMESPACE
DGUI_USE_NAMESPACE

namespace meshlink {
namespace {

// Keep conversation rows compact regardless of translated text length.
class RoomDelegate : public DTK_WIDGET_NAMESPACE::DStyledItemDelegate {
public:
    using DStyledItemDelegate::DStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        QSize size = DStyledItemDelegate::sizeHint(option, index);
        size.setHeight(52);
        return size;
    }
};

} // namespace

RoomsView::RoomsView(Engine *engine, QWidget *parent) : QWidget(parent), m_engine(engine) {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(24, 20, 24, 20);
    root->setSpacing(16);

    auto *newRoom = new DSuggestButton(tr("New room"), this);
    newRoom->setObjectName(QStringLiteral("roomsView.createRoomButton"));
    newRoom->setIcon(UiUtils::icon(QStringLiteral("list-add")));
    connect(newRoom, &DSuggestButton::clicked, this, &RoomsView::openCreateDialog);
    root->addWidget(UiUtils::pageHeader(tr("Chats"), newRoom, this));

    auto *split = new QSplitter(Qt::Horizontal, this);
    split->setChildrenCollapsible(false);
    split->setHandleWidth(1);

    auto *leftContainer = new QWidget(split);
    auto *leftContainerLayout = new QHBoxLayout(leftContainer);
    leftContainerLayout->setContentsMargins(0, 0, 0, 0);
    leftContainerLayout->setSpacing(0);

    auto *left = new QWidget(leftContainer);
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(8);

    m_emptyLabel = new DLabel(tr("No rooms yet. Create one, or discover rooms on your LAN."), left);
    m_emptyLabel->setWordWrap(true);
    m_emptyLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_emptyLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_emptyLabel->setForegroundRole(DPalette::TextTips);
    leftLayout->addWidget(m_emptyLabel, 0, Qt::AlignTop);

    m_roomList = new DListView(left);
    m_roomList->setObjectName(QStringLiteral("roomsView.roomList"));
    m_roomList->setFrameShape(QFrame::NoFrame);
    m_roomList->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_roomList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_roomList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_roomList->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_roomList->setBackgroundType(DStyledItemDelegate::RoundedBackground);
    m_roomList->setItemMargins(QMargins(6, 2, 6, 2));
    m_roomList->setItemSpacing(2);
    m_roomList->setItemRadius(8);
    m_roomList->setIconSize(QSize(24, 24));
    m_roomList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_roomList->setItemDelegate(new RoomDelegate(m_roomList));
    m_roomList->setMinimumWidth(220);
    m_roomList->setMaximumWidth(300);
    m_roomModel = new QStandardItemModel(this);
    m_roomList->setModel(m_roomModel);
    leftLayout->addWidget(m_roomList, 1);

    m_chat = new ChatView(m_engine, split);
    connect(m_chat, &ChatView::downloadsStarted, this, &RoomsView::downloadsStarted);
    connect(m_chat, &ChatView::manageRequested, this, &RoomsView::openManagementDialog);

    leftContainerLayout->addWidget(left, 1);
    leftContainerLayout->addWidget(new DVerticalLine(leftContainer));

    split->addWidget(leftContainer);
    split->addWidget(m_chat);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({240, 760});
    root->addWidget(split, 1);

    connect(m_roomList->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex &current, const QModelIndex &) {
                const QString roomId =
                    current.isValid() ? current.data(Qt::UserRole).toString() : QString();
                m_chat->setRoom(roomId);
                if (!roomId.isEmpty())
                    emit roomActivated(roomId);
            });
    // Room-level actions (manage / mute / copy identifiers) live in the
    // context menu to keep the rows clean.
    m_roomList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_roomList, &DListView::customContextMenuRequested, this,
            [this](const QPoint &pos) {
                const QModelIndex index = m_roomList->indexAt(pos);
                const QString roomId =
                    index.isValid() ? index.data(Qt::UserRole).toString() : QString();
                if (!roomId.isEmpty() && index != m_roomList->currentIndex())
                    selectRoom(roomId);
                showRoomContextMenu(m_roomList->mapToGlobal(pos), roomId);
            });
    connect(m_roomList, &DListView::doubleClicked, this, [this](const QModelIndex &index) {
        const QString roomId = index.data(Qt::UserRole).toString();
        if (!roomId.isEmpty())
            openManagementDialog(roomId);
    });

    auto *rooms = m_engine->rooms();
    connect(rooms, &RoomManager::roomsChanged, this, &RoomsView::refreshRooms);
    connect(rooms, &RoomManager::kickedFromRoom, this,
            [this](const QString &roomId, const QString &reason) {
                if (currentRoomId() == roomId)
                    m_chat->setRoom(QString());
                refreshRooms();
                UiUtils::warning(this, tr("Room"),
                                 tr("You were removed from the room: %1").arg(reason));
            });
    connect(rooms, &RoomManager::roomClosedByHost, this, [this](const QString &roomId) {
        if (currentRoomId() == roomId)
            m_chat->setRoom(QString());
        refreshRooms();
        UiUtils::info(this, tr("Room"),
                      tr("The host dissolved this room. Local room records were removed."));
    });
    connect(rooms, &RoomManager::joinRequested, this,
            [this](const QString &, const PendingJoin &) { refreshRooms(); });
    connect(rooms, &RoomManager::joinResult, this,
            [this](const QString &roomId, JoinStatus status, const QString &) {
                if (status == JoinStatus::Accepted || status == JoinStatus::PendingApproval)
                    UiUtils::info(this, tr("Join room"), UiUtils::joinStatusText(status));
                else
                    UiUtils::warning(this, tr("Join room"), UiUtils::joinStatusText(status));
                if (!roomId.isEmpty())
                    refreshRooms();
            });

    refreshRooms();
}

QString RoomsView::currentRoomId() const {
    const QModelIndex index = m_roomList ? m_roomList->currentIndex() : QModelIndex();
    return index.isValid() ? index.data(Qt::UserRole).toString() : QString();
}

void RoomsView::selectRoom(const QString &roomId) {
    if (roomId.isEmpty() || !m_roomModel)
        return;
    for (int row = 0; row < m_roomModel->rowCount(); ++row) {
        if (m_roomModel->index(row, 0).data(Qt::UserRole).toString() == roomId) {
            m_roomList->setCurrentIndex(m_roomModel->index(row, 0));
            return;
        }
    }
    // Room not rendered yet (roomsChanged is async): remember and apply on the
    // next refresh.
    m_pendingSelection = roomId;
}

void RoomsView::setMentionCounts(const QHash<QString, int> &counts) {
    m_mentions = counts;
    refreshRooms();
}

void RoomsView::showRoomContextMenu(const QPoint &globalPos, const QString &roomId) {
    if (roomId.isEmpty())
        return;
    const RoomLocal room = m_engine->rooms()->room(roomId);
    if (room.roomId.isEmpty())
        return;
    QMenu menu(this);
    QAction *manage = menu.addAction(tr("Manage room…"));
    QAction *copyCode = menu.addAction(tr("Copy invite code"));
    QAction *copyFp = menu.addAction(tr("Copy room fingerprint"));
    menu.addSeparator();
    const bool dnd = m_engine->rooms()->isRoomDnd(roomId);
    QAction *dndAction = menu.addAction(dnd ? tr("Notifications: muted")
                                            : tr("Notifications: on"));
    dndAction->setCheckable(true);
    dndAction->setChecked(dnd);

    QAction *chosen = menu.exec(globalPos);
    if (!chosen)
        return;
    if (chosen == manage) {
        openManagementDialog(roomId);
    } else if (chosen == copyCode) {
        QGuiApplication::clipboard()->setText(room.inviteCode);
    } else if (chosen == copyFp) {
        QGuiApplication::clipboard()->setText(room.roomId);
    } else if (chosen == dndAction) {
        m_engine->rooms()->setRoomDnd(roomId, !dnd);
    }
}

void RoomsView::refreshRooms() {
    if (!m_roomList || !m_roomModel)
        return;
    const QString keep = currentRoomId();
    m_roomModel->clear();

    const QVector<RoomLocal> rooms = m_engine->rooms()->myRooms();
    int selected = -1;
    for (int row = 0; row < rooms.size(); ++row) {
        const RoomLocal &room = rooms.at(row);
        int pending = 0;
        for (const PendingJoin &join : m_engine->rooms()->pendingApprovals(room.roomId)) {
            Q_UNUSED(join);
            ++pending;
        }

        auto *item = new DStandardItem(room.name);
        item->setData(room.roomId, Qt::UserRole);
        item->setFontSize(DFontSizeManager::T8);
        item->setToolTip(tr("Invite code: %1\nRoom fingerprint: %2")
                             .arg(room.inviteCode, room.roomId));
        if (room.hosted)
            item->setIcon(UiUtils::icon(QStringLiteral("emblem-favorite"),
                                        QStringLiteral("starred")));

        QStringList meta;
        meta << (room.hosted ? tr("Hosted") : tr("Member"));
        meta << tr("Members: %1").arg(m_engine->rooms()->members(room.roomId).size());
        if (pending > 0)
            meta << tr("Pending: %1").arg(pending);

        auto *subtitle = new DViewItemAction(Qt::AlignLeft, QSize(0, 0), QSize(), false);
        subtitle->setText(meta.join(QStringLiteral(" · ")));
        subtitle->setFontSize(DFontSizeManager::T8);
        subtitle->setTextColorRole(DPalette::TextTips);
        QList<DViewItemAction *> actions{subtitle};

        // Row badges: unread @-mentions (warning colour) and the per-room
        // do-not-disturb state, both right-aligned.
        const int mentions = m_mentions.value(room.roomId, 0);
        if (mentions > 0) {
            auto *badge = new DViewItemAction(Qt::AlignRight, QSize(0, 0), QSize(), false);
            badge->setText(tr("@%1").arg(mentions));
            badge->setFontSize(DFontSizeManager::T8);
            badge->setTextColorRole(DPalette::TextWarning);
            actions.append(badge);
        }
        if (m_engine->rooms()->isRoomDnd(room.roomId)) {
            auto *dnd = new DViewItemAction(Qt::AlignRight, QSize(0, 0), QSize(), false);
            dnd->setIcon(UiUtils::icon(QStringLiteral("notifications-disabled"),
                                       QStringLiteral("notification-disabled")));
            dnd->setToolTip(tr("Notifications muted for this room"));
            actions.append(dnd);
        }
        item->setTextActionList(actions);

        m_roomModel->appendRow(item);
        if (room.roomId == keep)
            selected = row;
    }

    const bool empty = m_roomModel->rowCount() == 0;
    m_emptyLabel->setVisible(empty);
    m_roomList->setVisible(!empty);
    if (empty) {
        m_chat->setRoom(QString());
        return;
    }

    if (selected < 0 && !m_pendingSelection.isEmpty()) {
        for (int row = 0; row < rooms.size(); ++row) {
            if (rooms.at(row).roomId == m_pendingSelection) {
                selected = row;
                break;
            }
        }
    }
    m_pendingSelection.clear();
    if (selected < 0)
        selected = 0;
    m_roomList->setCurrentIndex(m_roomModel->index(selected, 0));
}

// ---- UC-05: create dialog ----
void RoomsView::openCreateDialog() {
    DDialog dialog(this);
    dialog.setTitle(tr("New room"));
    dialog.setIcon(UiUtils::icon(QStringLiteral("list-add")));

    auto *content = new QWidget(&dialog);
    auto *form = new QFormLayout(content);
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(12);

    auto *name = new DLineEdit(content);
    name->setPlaceholderText(tr("Room name (2–64 characters, leave empty for a random name)"));
    name->lineEdit()->setMaxLength(64);
    auto *intro = new DLineEdit(content);
    intro->setPlaceholderText(tr("Description (optional)"));
    intro->lineEdit()->setMaxLength(200);

    auto *policy = new DComboBox(content);
    policy->addItem(tr("Open to everyone"), int(JoinPolicy::Open));
    policy->addItem(tr("Requires host approval"), int(JoinPolicy::Approval));

    auto *capacity = new DSpinBox(content);
    capacity->setRange(0, 10000);
    capacity->setSpecialValueText(tr("No limit"));
    capacity->setSuffix(tr(" people"));

    form->addRow(tr("Name"), name);
    form->addRow(tr("Description"), intro);
    form->addRow(tr("Join policy"), policy);
    form->addRow(tr("Capacity"), capacity);
    content->setMinimumWidth(400);
    dialog.addContent(content);

    dialog.addButton(tr("Cancel"));
    dialog.addButton(tr("Create"), true, DDialog::ButtonRecommend);
    connect(&dialog, &DDialog::buttonClicked, this, [&](int index, const QString &) {
        if (index != 1)
            return;
        QString error;
        const QString id = m_engine->rooms()->createRoom(
            name->text(), intro->text(), JoinPolicy(policy->currentData().toInt()),
            capacity->value(), &error);
        if (id.isEmpty()) {
            if (error.isEmpty())
                error = tr("Could not create the room.");
            name->setAlert(true);
            name->showAlertMessage(error);
            return;
        }

        dialog.accept();
        refreshRooms();
        const RoomLocal created = m_engine->rooms()->room(id);
        QTimer::singleShot(0, this, [this, created] {
            DDialog info(this);
            info.setTitle(tr("Room created"));
            info.setMessage(tr("Share the invite code with people on the same LAN segment."));
            info.setIcon(UiUtils::icon(QStringLiteral("dialog-information")));

            auto *box = new QWidget(&info);
            auto *form = new QFormLayout(box);
            form->setContentsMargins(0, 0, 0, 0);
            form->setSpacing(10);

            // Show which name the room actually got, which matters when the
            // create dialog left the field empty and a random name was chosen.
            auto *roomName = new DLineEdit(box);
            roomName->setObjectName(QStringLiteral("roomsView.createdRoomName"));
            roomName->lineEdit()->setReadOnly(true);
            roomName->setText(created.name);

            auto *code = new DLineEdit(box);
            code->lineEdit()->setReadOnly(true);
            code->setText(created.inviteCode);
            auto *copyCode = new DIconButton(box);
            copyCode->setIcon(UiUtils::icon(QStringLiteral("edit-copy")));
            copyCode->setFlat(true);
            connect(copyCode, &DIconButton::clicked, &info, [&info, created] {
                QGuiApplication::clipboard()->setText(created.inviteCode);
                info.setMessage(tr("Invite code copied to the clipboard."));
            });
            code->setRightWidgets({copyCode});

            auto *fingerprint = new DLineEdit(box);
            fingerprint->lineEdit()->setReadOnly(true);
            fingerprint->setText(created.roomId);
            auto *copyFingerprint = new DIconButton(box);
            copyFingerprint->setIcon(UiUtils::icon(QStringLiteral("edit-copy")));
            copyFingerprint->setFlat(true);
            connect(copyFingerprint, &DIconButton::clicked, &info, [&info, created] {
                QGuiApplication::clipboard()->setText(created.roomId);
                info.setMessage(tr("Room fingerprint copied to the clipboard."));
            });
            fingerprint->setRightWidgets({copyFingerprint});

            form->addRow(tr("Room name"), roomName);
            form->addRow(tr("Invite code"), code);
            form->addRow(tr("Room fingerprint"), fingerprint);
            box->setMinimumWidth(440);
            info.addContent(box);
            info.addButton(tr("OK"), true, DDialog::ButtonRecommend);
            info.exec();
        });
    });
    dialog.exec();
}

// ---- UC-07/UC-13: management dialog ----
void RoomsView::openManagementDialog(const QString &roomId) {
    const RoomLocal room = m_engine->rooms()->room(roomId);
    if (!room.joined)
        return;

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Room management — %1").arg(room.name));
    dialog.setMinimumSize(620, 540);
    auto *root = new QVBoxLayout(&dialog);
    root->setContentsMargins(20, 20, 20, 20);
    root->setSpacing(12);

    auto *title = new DLabel(room.name, &dialog);
    title->setForegroundRole(DPalette::TextTitle);
    DFontSizeManager::instance()->bind(title, DFontSizeManager::T4, QFont::DemiBold);
    root->addWidget(title);

    auto *meta = new DLabel(tr("Invite code: %1 · Room fingerprint: %2")
                                .arg(room.inviteCode, room.roomId),
                            &dialog);
    meta->setTextInteractionFlags(Qt::TextSelectableByMouse);
    meta->setForegroundRole(DPalette::TextTips);
    root->addWidget(meta);

    auto *tabs = new QTabWidget(&dialog);
    root->addWidget(tabs, 1);

    // ---- members tab ----
    auto *membersPage = new QWidget(tabs);
    auto *membersLayout = new QVBoxLayout(membersPage);
    membersLayout->setContentsMargins(0, 12, 0, 0);
    membersLayout->setSpacing(8);

    auto *members = new DListView(membersPage);
    members->setFrameShape(QFrame::NoFrame);
    members->setEditTriggers(QAbstractItemView::NoEditTriggers);
    members->setSelectionMode(QAbstractItemView::SingleSelection);
    members->setBackgroundType(DStyledItemDelegate::RoundedBackground);
    members->setItemMargins(QMargins(10, 6, 10, 6));
    members->setItemSpacing(3);
    members->setItemRadius(8);
    members->setIconSize(QSize(32, 32));
    members->setContextMenuPolicy(Qt::CustomContextMenu);
    auto *membersModel = new QStandardItemModel(members);
    members->setModel(membersModel);
    membersLayout->addWidget(members, 1);

    auto *hint = new DLabel(tr("Right-click or double-click a member for moderation actions."),
                            membersPage);
    hint->setForegroundRole(DPalette::TextTips);
    membersLayout->addWidget(hint);
    tabs->addTab(membersPage, tr("Members"));

    const bool iAmHost = room.hosted;
    const bool iAmAdmin = m_engine->rooms()->myRole(roomId) == MemberRole::Admin;
    const QString selfFp = m_engine->account()->profile().fingerprint;
    // Avatar blobs arrive asynchronously; request each hash once per dialog.
    auto requestedAvatars = std::make_shared<QSet<QString>>();
    // A mention picked in the dialog is applied after exec() returns, when the
    // modal window is gone and the composer can really take keyboard focus.
    QString pendingMention;

    auto fillMembers = [this, roomId, membersModel, members, requestedAvatars] {
        membersModel->clear();
        const QVector<RoomMemberInfo> list = m_engine->rooms()->members(roomId);
        for (const RoomMemberInfo &member : list) {
            const QString role = member.role == static_cast<quint8>(MemberRole::Host)
                                     ? tr("Host")
                                     : member.role == static_cast<quint8>(MemberRole::Admin)
                                           ? tr("Admin")
                                           : tr("Member");
            QString mute;
            if (member.mutedUntil == -1) {
                mute = tr("muted until manually unmuted");
            } else if (member.mutedUntil > nowMs()) {
                mute = tr("muted until %1").arg(UiUtils::formattedTime(member.mutedUntil));
            }

            auto *item = new DStandardItem(
                tr("%1 · %2").arg(member.nickname, role));
            item->setData(member.fp, Qt::UserRole);
            item->setData(int(member.role), Qt::UserRole + 1);
            if (!member.avatarHash.isEmpty() && !ChatManager::hasBlob(member.avatarHash) &&
                !requestedAvatars->contains(member.avatarHash)) {
                requestedAvatars->insert(member.avatarHash);
                m_engine->chat()->requestBlob(member.avatarHash, member.fp);
            }
            item->setData(QVariant::fromValue(QIcon(UiUtils::avatarFor(
                              member.fp, member.avatarHash, QString(), 32, members))),
                          Qt::DecorationRole);
            QStringList meta;
            meta << UiUtils::shortFp(member.fp, 12);
            if (!mute.isEmpty())
                meta << mute;
            auto *sub = new DViewItemAction(Qt::AlignLeft, QSize(0, 0), QSize(), false);
            sub->setText(meta.join(QStringLiteral(" · ")));
            sub->setFontSize(DFontSizeManager::T8);
            sub->setTextColorRole(DPalette::TextTips);
            item->setTextActionList({sub});
            membersModel->appendRow(item);
        }
    };
    fillMembers();

    auto runOp = [this, roomId, &dialog, fillMembers](AdminOp op, const QString &target, qint64 duration) {
        QPointer<QDialog> guard(&dialog); // host->member ADMIN_OP result is async
        m_engine->rooms()->requestAdminOp(
            roomId, target, op, duration, [guard, fillMembers](bool ok, QString why) {
                if (!guard)
                    return;
                if (!ok)
                    UiUtils::warning(guard.data(), tr("Action rejected"), why);
                fillMembers();
            });
    };

    // Same member menu as the chat pane: right-click a member row/avatar.
    connect(members, &DListView::customContextMenuRequested, &dialog,
            [this, &dialog, members, roomId, selfFp, fillMembers,
             &pendingMention](const QPoint &pos) {
                const QModelIndex index = members->indexAt(pos);
                const QString target =
                    index.isValid() ? index.data(Qt::UserRole).toString() : QString();
                if (target.isEmpty())
                    return;
                QPointer<QDialog> guard(&dialog);
                QMenu *menu = MemberMenu::create(
                    &dialog, m_engine->rooms(), roomId, selfFp, target,
                    [guard, &pendingMention](const QString &nickname) {
                        pendingMention = nickname;
                        if (guard)
                            guard->accept();
                    },
                    [this, guard, fillMembers] {
                        if (!guard)
                            return;
                        fillMembers();
                        refreshRooms();
                    });
                if (!menu)
                    return;
                menu->setAttribute(Qt::WA_DeleteOnClose);
                menu->popup(members->viewport()->mapToGlobal(pos));
            });

    // Avatar blobs are fetched on demand: repaint the member rows when they land.
    connect(m_engine->chat(), &ChatManager::blobReady, &dialog,
            [fillMembers](const QString &) { fillMembers(); });

    // host/admin toolbar
    if (iAmHost || iAmAdmin) {
        auto *tools = new QHBoxLayout;
        bool globalMuted = room.globalMute;
        auto *globalMute =
            new DPushButton(globalMuted ? tr("Disable global mute") : tr("Enable global mute"),
                            membersPage);
        connect(globalMute, &DPushButton::clicked, &dialog, [&] {
            globalMuted = !globalMuted;
            globalMute->setText(globalMuted ? tr("Disable global mute")
                                            : tr("Enable global mute"));
            runOp(globalMuted ? AdminOp::GlobalMuteOn : AdminOp::GlobalMuteOff, roomId, 0);
            refreshRooms();
        });
        tools->addWidget(globalMute);

        if (iAmHost) {
            auto *blacklist = new DPushButton(tr("Blacklist…"), membersPage);
            connect(blacklist, &DPushButton::clicked, &dialog, [&] {
                const QVector<QString> entries = m_engine->rooms()->blacklist(roomId);
                if (entries.isEmpty()) {
                    UiUtils::info(&dialog, tr("Blacklist"), tr("The blacklist is empty."));
                    return;
                }
                QStringList names;
                for (const QString &fp : entries)
                    names << UiUtils::shortFp(fp, 12);
                bool ok = false;
                const QString picked = DInputDialog::getItem(
                    &dialog, tr("Blacklist"), tr("Select a node to unblock:"), names, 0, false, &ok);
                if (!ok || picked.isEmpty())
                    return;
                const int index = names.indexOf(picked);
                if (index >= 0 && index < entries.size())
                    runOp(AdminOp::Unblacklist, entries.at(index), 0);
            });
            tools->addWidget(blacklist);
        }
        tools->addStretch();
        membersLayout->insertLayout(0, tools);
    }

    connect(members, &DListView::doubleClicked, &dialog, [&](const QModelIndex &index) {
        if (!iAmHost && !iAmAdmin)
            return;
        const QString target = index.data(Qt::UserRole).toString();
        const int role = index.data(Qt::UserRole + 1).toInt();
        QMenu menu(&dialog);
        if (role == int(MemberRole::Member))
            menu.addAction(tr("Make administrator"),
                           [=] { runOp(AdminOp::SetAdmin, target, 0); });
        if (role == int(MemberRole::Admin))
            menu.addAction(tr("Remove administrator"),
                           [=] { runOp(AdminOp::UnsetAdmin, target, 0); });
        menu.addAction(tr("Mute for 10 minutes"),
                       [=] { runOp(AdminOp::Mute, target, 10 * 60 * 1000); });
        menu.addAction(tr("Mute for 1 hour"),
                       [=] { runOp(AdminOp::Mute, target, 3600 * 1000); });
        menu.addAction(tr("Mute until manually unmuted"),
                       [=] { runOp(AdminOp::Mute, target, 0); });
        menu.addAction(tr("Unmute"), [=] { runOp(AdminOp::Unmute, target, 0); });
        menu.addSeparator();
        menu.addAction(tr("Remove from room"), [=] { runOp(AdminOp::Kick, target, 0); });
        menu.addAction(tr("Remove and block"), [=] { runOp(AdminOp::Blacklist, target, 0); });
        menu.exec(QCursor::pos());
    });

    connect(m_engine->rooms(), &RoomManager::membersChanged, &dialog,
            [&](const QString &changedRoom) {
                if (changedRoom == roomId)
                    fillMembers();
            });

    // ---- pending approvals tab (host only) ----
    if (iAmHost) {
        auto *requestsPage = new QWidget(tabs);
        auto *requestsLayout = new QVBoxLayout(requestsPage);
        requestsLayout->setContentsMargins(0, 12, 0, 0);
        requestsLayout->setSpacing(8);

        auto *requests = new DListView(requestsPage);
        requests->setFrameShape(QFrame::NoFrame);
        requests->setEditTriggers(QAbstractItemView::NoEditTriggers);
        requests->setSelectionMode(QAbstractItemView::SingleSelection);
        requests->setBackgroundType(DStyledItemDelegate::RoundedBackground);
        requests->setItemMargins(QMargins(10, 6, 10, 6));
        requests->setItemSpacing(3);
        requests->setItemRadius(8);
        auto *requestsModel = new QStandardItemModel(requests);
        requests->setModel(requestsModel);
        requestsLayout->addWidget(requests, 1);

        auto fillRequests = [this, roomId, requestsModel] {
            requestsModel->clear();
            for (const PendingJoin &pending : m_engine->rooms()->pendingApprovals(roomId)) {
                auto *item = new DStandardItem(
                    tr("%1 · %2:%3 · %4")
                        .arg(pending.requester.nickname, pending.requester.ip)
                        .arg(pending.requester.tcpPort)
                        .arg(UiUtils::formattedTime(pending.requester.upSince)));
                item->setData(pending.rowId, Qt::UserRole);
                requestsModel->appendRow(item);
            }
        };
        fillRequests();

        auto *actions = new QHBoxLayout;
        auto *approve = new DSuggestButton(tr("Approve"), requestsPage);
        auto *reject = new DPushButton(tr("Reject"), requestsPage);
        connect(approve, &DSuggestButton::clicked, &dialog, [&] {
            const QModelIndex index = requests->currentIndex();
            if (!index.isValid())
                return;
            m_engine->rooms()->respondApproval(index.data(Qt::UserRole).toLongLong(), true);
            fillRequests();
            refreshRooms();
        });
        connect(reject, &DPushButton::clicked, &dialog, [&] {
            const QModelIndex index = requests->currentIndex();
            if (!index.isValid())
                return;
            m_engine->rooms()->respondApproval(index.data(Qt::UserRole).toLongLong(), false);
            fillRequests();
        });
        actions->addWidget(approve);
        actions->addWidget(reject);
        actions->addStretch();
        requestsLayout->addLayout(actions);
        tabs->addTab(requestsPage, tr("Requests"));

        connect(m_engine->rooms(), &RoomManager::joinRequested, &dialog,
                [&](const QString &changedRoom, const PendingJoin &) {
                    if (changedRoom == roomId)
                        fillRequests();
                });
    }

    auto *bottom = new QHBoxLayout;
    auto *leave = new DWarningButton(&dialog);
    leave->setText(room.hosted ? tr("Dissolve room…") : tr("Leave room…"));
    auto *close = new DPushButton(tr("Close"), &dialog);
    bottom->addWidget(leave);
    bottom->addStretch();
    bottom->addWidget(close);
    root->addLayout(bottom);

    connect(close, &DPushButton::clicked, &dialog, &QDialog::accept);
    connect(leave, &DWarningButton::clicked, &dialog, [&] {
        const bool ok = UiUtils::confirm(
            &dialog, tr("Confirm"),
            room.hosted
                ? tr("Dissolving the room erases all local room records and notifies every member. "
                     "Continue?")
                : tr("Leave this room?"),
            room.hosted ? tr("Dissolve") : tr("Leave"), true);
        if (!ok)
            return;
        QPointer<QDialog> guard(&dialog);
        m_engine->rooms()->leaveRoom(roomId, [this, guard](bool left, QString why) {
            if (!guard)
                return;
            if (!left) {
                UiUtils::warning(guard.data(), tr("Leave failed"), why);
                return;
            }
            guard->accept();
            refreshRooms();
        });
    });

    dialog.exec();

    if (!pendingMention.isEmpty()) {
        const QString nickname = pendingMention;
        // Queue past the dialog teardown so focus lands on the composer.
        QTimer::singleShot(0, this, [this, roomId, nickname] {
            selectRoom(roomId);
            if (m_chat)
                m_chat->mentionMember(nickname);
        });
    }
}

} // namespace meshlink
