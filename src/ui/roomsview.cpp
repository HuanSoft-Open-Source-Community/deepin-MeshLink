// deepin-MeshLink — rooms sidebar implementation.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "roomsview.h"

#include "chatview.h"
#include "core/engine.h"
#include "core/roommanager.h"
#include "core/settings.h"

#include <DDialog>

#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QVBoxLayout>

DWIDGET_USE_NAMESPACE

namespace meshlink {

RoomsView::RoomsView(Engine *engine, QWidget *parent) : QWidget(parent), m_engine(engine) {
    auto *split = new QSplitter(this);
    auto *left = new QWidget(split);
    auto *lly = new QVBoxLayout(left);
    lly->setContentsMargins(6, 6, 6, 6);

    auto *btnRow = new QHBoxLayout;
    auto *create = new QPushButton(QStringLiteral("新建聊天室"), left);
    create->setToolTip(QStringLiteral("UC-05"));
    connect(create, &QPushButton::clicked, this, &RoomsView::openCreateDialog);
    auto *browse = new QPushButton(QStringLiteral("发现房间"), left);
    connect(browse, &QPushButton::clicked, this, [this] { m_roomList->setFocus(); });
    btnRow->addWidget(create);
    lly->addLayout(btnRow);

    m_roomList = new QListWidget(left);
    m_roomList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_roomList, &QListWidget::currentRowChanged, this, [this](int) {
        const int row = m_roomList->currentRow();
        m_chat->setRoom(row < 0 ? QString() : m_roomList->item(row)->data(Qt::UserRole).toString());
    });
    connect(m_roomList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *it) {
        openManagementDialog(it->data(Qt::UserRole).toString());
    });
    lly->addWidget(m_roomList, 1);
    split->addWidget(left);

    m_chat = new ChatView(m_engine, split);
    connect(m_chat, &ChatView::downloadsStarted, this, &RoomsView::downloadsStarted);
    split->addWidget(m_chat);
    split->setSizes({240, 640});

    auto *rootly = new QHBoxLayout(this);
    rootly->setContentsMargins(0, 0, 0, 0);
    rootly->addWidget(split);

    connect(m_engine->rooms(), &RoomManager::roomsChanged, this, &RoomsView::refreshRooms);
    connect(m_engine->rooms(), &RoomManager::kickedFromRoom, this, [this](const QString &roomId, const QString &reason) {
        refreshRooms();
        if (m_chat)
            m_chat->setRoom(m_roomList->currentRow() < 0 ? QString() : m_roomList->item(m_roomList->currentRow())->data(Qt::UserRole).toString());
        QMessageBox::warning(this, QStringLiteral("聊天室"),
                             QStringLiteral("你已被移出聊天室：%1").arg(reason)); // UC-07 踢出/黑名单反馈
    });
    connect(m_engine->rooms(), &RoomManager::roomClosedByHost, this, [this](const QString &) {
        refreshRooms();
        QMessageBox::information(this, QStringLiteral("聊天室"), QStringLiteral("托管者已解散该聊天室，本地记录已清理。")); // UC-13/UC-05
    });
    connect(m_engine->rooms(), &RoomManager::joinRequested, this, [this](const QString &roomId, const PendingJoin &) {
        refreshRooms(); // badge 角标提示待批准（host UI）
    });
    connect(m_engine->rooms(), &RoomManager::joinResult, this, [this](const QString &, JoinStatus st, const QString &detail) {
        if (st == JoinStatus::Accepted)
            QMessageBox::information(this, QStringLiteral("加入聊天室"), QStringLiteral("已成功加入。"));
        else if (st == JoinStatus::PendingApproval)
            QMessageBox::information(this, QStringLiteral("加入聊天室"), QStringLiteral("申请已提交，等待托管者批准。"));
        else
            QMessageBox::warning(this, QStringLiteral("加入聊天室"), QStringLiteral("未能加入：%1").arg(detail));
        refreshRooms();
    });
    refreshRooms();
}

void RoomsView::refreshRooms() {
    const int keep = m_roomList->currentRow();
    const QString keepId = keep >= 0 && m_roomList->item(keep) ? m_roomList->item(keep)->data(Qt::UserRole).toString() : QString();
    m_roomList->clear();
    for (const auto &r : m_engine->rooms()->myRooms()) {
        int pend = 0;
        for (const auto &p : m_engine->rooms()->pendingApprovals(r.roomId))
            ++pend;
        auto *it = new QListWidgetItem(QStringLiteral("%1%2%3").arg(r.name,
                                                                     r.hosted ? QStringLiteral(" ★") : QString(),
                                                                     pend > 0 ? QStringLiteral(" ⚑%1").arg(pend) : QString()),
                                       m_roomList);
        it->setData(Qt::UserRole, r.roomId);
        it->setToolTip(QStringLiteral("邀请码：%1\n房间指纹：%2").arg(r.inviteCode, r.roomId));
        if (r.roomId == keepId)
            m_roomList->setCurrentItem(it);
    }
    if (m_roomList->count() && !m_roomList->currentItem())
        m_roomList->setCurrentRow(0);
}

// ---- UC-05: create dialog ----
void RoomsView::openCreateDialog() {
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("新建聊天室（UC-05）"));
    auto *form = new QVBoxLayout(&dlg);
    auto *name = new QLineEdit(&dlg);
    name->setPlaceholderText(QStringLiteral("房间名称（2–64 字符）"));
    auto *intro = new QLineEdit(&dlg);
    intro->setPlaceholderText(QStringLiteral("简介（可选）"));
    auto *policy = new QComboBox(&dlg);
    policy->addItem(QStringLiteral("自由加入"), int(JoinPolicy::Open));
    policy->addItem(QStringLiteral("需托管者批准"), int(JoinPolicy::Approval)); // UC-06 A1
    auto *cap = new QSpinBox(&dlg);
    cap->setRange(0, 10000);
    cap->setSpecialValueText(QStringLiteral("不限制"));
    cap->setSuffix(QStringLiteral(" 人"));
    form->addWidget(new QLabel(QStringLiteral("名称："), &dlg));
    form->addWidget(name);
    form->addWidget(new QLabel(QStringLiteral("简介："), &dlg));
    form->addWidget(intro);
    form->addWidget(new QLabel(QStringLiteral("加入策略："), &dlg));
    form->addWidget(policy);
    form->addWidget(new QLabel(QStringLiteral("容量上限："), &dlg));
    form->addWidget(cap);
    auto *bb = new QHBoxLayout;
    auto *ok = new QPushButton(QStringLiteral("创建"), &dlg);
    auto *cancel = new QPushButton(QStringLiteral("取消"), &dlg);
    bb->addStretch();
    bb->addWidget(ok);
    bb->addWidget(cancel);
    form->addLayout(bb);
    connect(cancel, &QPushButton::clicked, &dlg, &QDialog::reject);
    connect(ok, &QPushButton::clicked, &dlg, [&] {
        QString err;
        // roomId = SHA256(host|name|inviteCode|ts)[0:32] + unique 6-digit invite code
        const QString id = m_engine->rooms()->createRoom(name->text(), intro->text(),
                                                         JoinPolicy(policy->currentData().toInt()), cap->value(), &err);
        if (id.isEmpty()) {
            QMessageBox::warning(&dlg, QStringLiteral("创建失败"), err); // E1 name rules
            return;
        }
        dlg.accept();
        refreshRooms();
        const RoomLocal created = m_engine->rooms()->room(id);
        QMessageBox::information(this, QStringLiteral("聊天室已创建"),
                                 QStringLiteral("邀请码：%1\n房间指纹：%2\n\n同一网段的成员可输入邀请码加入。")
                                     .arg(created.inviteCode, created.roomId));
    });
    dlg.exec();
}

// ---- UC-07/UC-13: management dialog (members, admin ops, approvals, blacklist) ----
void RoomsView::openManagementDialog(const QString &roomId) {
    const RoomLocal r = m_engine->rooms()->room(roomId);
    if (!r.joined)
        return;
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("聊天室管理 — %1").arg(r.name));
    dlg.resize(480, 520);
    auto *root = new QVBoxLayout(&dlg);

    root->addWidget(new QLabel(QStringLiteral("邀请码：<b>%1</b> · 房间指纹：<span style='font-family:monospace'>%2</span>")
                                   .arg(r.inviteCode, r.roomId),
                               &dlg));
    root->addWidget(new QLabel(QStringLiteral("成员（双击成员行执行管理操作；权限遵循 UC-07 矩阵）"), &dlg));
    auto *members = new QListWidget(&dlg);
    auto fillMembers = [&] {
        members->clear();
        for (const auto &m : m_engine->rooms()->members(roomId)) {
            QString tag = m.role == static_cast<quint8>(MemberRole::Host) ? QStringLiteral("【托管者】")
                        : m.role == static_cast<quint8>(MemberRole::Admin) ? QStringLiteral("【管理员】") : QString();
            if (m.mutedUntil != 0)
                tag += m.mutedUntil == -1 ? QStringLiteral("【禁言中】") : QStringLiteral("【限时禁言】");
            auto *it = new QListWidgetItem(QStringLiteral("%1 %2 · %3…%4").arg(m.nickname, tag, m.fp.left(8), QString()));
            it->setData(Qt::UserRole, m.fp);
            it->setData(Qt::UserRole + 1, int(m.role));
            members->addItem(it);
        }
    };
    fillMembers();
    root->addWidget(members, 2);

    auto runOp = [&](AdminOp op, const QString &target, qint64 dur) {
        QPointer<QDialog> gd = &dlg; // host->member ADMIN_OP result is async
        m_engine->rooms()->requestAdminOp(roomId, target, op, dur, [gd, this, fillMembers](bool ok, QString why) mutable {
            if (!gd)
                return;
            if (!ok)
                QMessageBox::warning(gd, QStringLiteral("操作被拒绝"), why); // BR-13 越权提示
            fillMembers();
        });
    };

    connect(members, &QListWidget::itemDoubleClicked, &dlg, [&](QListWidgetItem *it) {
        const QString target = it->data(Qt::UserRole).toString();
        const int role = it->data(Qt::UserRole + 1).toInt();
        QMenu menu(&dlg);
        const bool iAmHost = m_engine->rooms()->myRole(roomId) == MemberRole::Host;
        const bool iAmAdmin = m_engine->rooms()->myRole(roomId) == MemberRole::Admin;
        if (!iAmHost && !iAmAdmin)
            return;
        if (role == int(MemberRole::Member))
            menu.addAction(QStringLiteral("设为管理员"), [=] { runOp(AdminOp::SetAdmin, target, 0); });
        if (role == int(MemberRole::Admin))
            menu.addAction(QStringLiteral("取消管理员"), [=] { runOp(AdminOp::UnsetAdmin, target, 0); });
        menu.addAction(QStringLiteral("禁言 10 分钟"), [=] { runOp(AdminOp::Mute, target, 10 * 60 * 1000); });
        menu.addAction(QStringLiteral("禁言 1 小时"), [=] { runOp(AdminOp::Mute, target, 3600 * 1000); });
        menu.addAction(QStringLiteral("禁言直至手动解除"), [=] { runOp(AdminOp::Mute, target, 0); });
        menu.addAction(QStringLiteral("解除禁言"), [=] { runOp(AdminOp::Unmute, target, 0); });
        menu.addSeparator();
        menu.addAction(QStringLiteral("移出聊天室"), [=] { runOp(AdminOp::Kick, target, 0); });
        menu.addAction(QStringLiteral("移出并拉黑"), [=] { runOp(AdminOp::Blacklist, target, 0); });
        menu.exec(QCursor::pos());
    });

    // host/admin extra toolbar
    auto *tools = new QHBoxLayout;
    if (r.hosted || m_engine->rooms()->myRole(roomId) != MemberRole::Member) {
        auto *gm = new QPushButton(r.globalMute ? QStringLiteral("关闭全员禁言") : QStringLiteral("开启全员禁言"), &dlg);
        connect(gm, &QPushButton::clicked, &dlg, [&] {
            runOp(r.globalMute ? AdminOp::GlobalMuteOff : AdminOp::GlobalMuteOn, roomId, 0); // target fp unused for global
            refreshRooms();
        });
        tools->addWidget(gm);
    }
    if (r.hosted) {
        auto *bl = new QPushButton(QStringLiteral("黑名单…"), &dlg);
        connect(bl, &QPushButton::clicked, &dlg, [&] {
            QStringList names;
            for (const auto &fp : m_engine->rooms()->blacklist(roomId))
                names << QStringLiteral("%1…").arg(fp.left(12));
            bool removed = false;
            if (names.isEmpty()) {
                QMessageBox::information(&dlg, QStringLiteral("黑名单"), QStringLiteral("黑名单为空。"));
                return;
            }
            const QString pick = QInputDialog::getItem(&dlg, QStringLiteral("黑名单"), QStringLiteral("选择要解除拉黑的节点："), names, 0, false, &removed);
            if (!pick.isEmpty()) {
                const QString fp = m_engine->rooms()->blacklist(roomId)[names.indexOf(pick)];
                runOp(AdminOp::Unblacklist, fp, 0);
            }
        });
        tools->addWidget(bl);
    }
    root->addLayout(tools);

    if (r.hosted) {
        // approvals queue (UC-06 A1 / UC-07 step 3)
        root->addWidget(new QLabel(QStringLiteral("待批准申请："), &dlg));
        auto *ap = new QListWidget(&dlg);
        auto fillAp = [&] {
            ap->clear();
            for (const auto &p : m_engine->rooms()->pendingApprovals(roomId)) {
                auto *it = new QListWidgetItem(QStringLiteral("%1 · %2:%3 · %4")
                                                   .arg(p.requester.nickname, p.requester.ip)
                                                   .arg(p.requester.tcpPort)
                                                   .arg(QDateTime::fromMSecsSinceEpoch(p.requester.upSince).toString(QStringLiteral("HH:mm"))));
                it->setData(Qt::UserRole, p.rowId);
                ap->addItem(it);
            }
        };
        fillAp();
        root->addWidget(ap, 1);
        connect(m_engine->rooms(), &RoomManager::joinRequested, &dlg, [&](const QString &rid, const PendingJoin &) {
            if (rid == roomId)
                fillAp();
        });
        auto *apRow = new QHBoxLayout;
        auto *acc = new QPushButton(QStringLiteral("批准"), &dlg);
        auto *rej = new QPushButton(QStringLiteral("拒绝"), &dlg);
        connect(acc, &QPushButton::clicked, &dlg, [&] {
            if (auto *it = ap->currentItem()) {
                m_engine->rooms()->respondApproval(it->data(Qt::UserRole).toLongLong(), true);
                fillAp();
                refreshRooms();
            }
        });
        connect(rej, &QPushButton::clicked, &dlg, [&] {
            if (auto *it = ap->currentItem()) {
                m_engine->rooms()->respondApproval(it->data(Qt::UserRole).toLongLong(), false);
                fillAp();
            }
        });
        apRow->addWidget(acc);
        apRow->addWidget(rej);
        apRow->addStretch();
        root->addLayout(apRow);
    }

    auto *bottom = new QHBoxLayout;
    auto *leave = new QPushButton(r.hosted ? QStringLiteral("解散聊天室…") : QStringLiteral("退出聊天室…"), &dlg);
    leave->setStyleSheet(QStringLiteral("color:#e53935;"));
    connect(leave, &QPushButton::clicked, &dlg, [&] {
        const auto res = QMessageBox::question(
            &dlg, QStringLiteral("确认"),
            r.hosted ? QStringLiteral("解散将清除本地全部房间数据并通知所有成员（UC-13 步骤5）。确定？")
                     : QStringLiteral("确定退出该聊天室？"));
        if (res != QMessageBox::Yes)
            return;
        QPointer<QDialog> gd = &dlg;
        m_engine->rooms()->leaveRoom(roomId, [this, gd](bool ok, QString why) {
            if (!gd)
                return;
            if (!ok)
                QMessageBox::warning(gd, QStringLiteral("退出失败"), why);
            gd->reject();
            refreshRooms();
        });
    });
    auto *closeb = new QPushButton(QStringLiteral("关闭"), &dlg);
    connect(closeb, &QPushButton::clicked, &dlg, &QDialog::accept);
    bottom->addStretch();
    bottom->addWidget(leave);
    bottom->addWidget(closeb);
    root->addLayout(bottom);

    dlg.exec();
}

void RoomsView::leaveSelected() {
    const int row = m_roomList->currentRow();
    if (row < 0)
        return;
    openManagementDialog(m_roomList->item(row)->data(Qt::UserRole).toString());
}

} // namespace meshlink
