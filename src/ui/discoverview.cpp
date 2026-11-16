// deepin-MeshLink — discovery view implementation (UC-04/05/06).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "discoverview.h"

#include "core/discovery.h"
#include "core/engine.h"
#include "core/roommanager.h"
#include "core/settings.h"

#include <DDialog>

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

DWIDGET_USE_NAMESPACE

namespace meshlink {

DiscoverView::DiscoverView(Engine *engine, QWidget *parent) : QWidget(parent), m_engine(engine) {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);

    // ---- UC-04: live LAN nodes ----
    root->addWidget(new QLabel(QStringLiteral("<b>局域网节点</b>（自动发现；超过 3 个心跳周期未再见则置灰）"), this));
    m_peers = new QTableWidget(0, 5, this);
    m_peers->setHorizontalHeaderLabels({QStringLiteral("昵称"), QStringLiteral("指纹"), QStringLiteral("IPv4"),
                                        QStringLiteral("TLS 端口"), QStringLiteral("状态")});
    m_peers->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_peers->verticalHeader()->setVisible(false);
    m_peers->setEditTriggers(QAbstractItemView::NoEditTriggers);
    root->addWidget(m_peers, 1);

    auto *boot = new QHBoxLayout;
    auto *bootEdit = new QLineEdit(this);
    bootEdit->setPlaceholderText(QStringLiteral("广播被防火墙阻断时：手动添加节点 IP（UC-04 E2）"));
    auto *bootAdd = new QPushButton(QStringLiteral("添加"), this);
    connect(bootAdd, &QPushButton::clicked, this, [this, bootEdit] {
        const QString ip = bootEdit->text().trimmed();
        if (ip.isEmpty())
            return;
        m_engine->settings()->addBootstrapTarget(ip, m_engine->settings()->discoveryPort());
        m_engine->discovery()->addManualTarget(ip, m_engine->settings()->discoveryPort());
        bootEdit->clear();
    });
    boot->addWidget(bootEdit);
    boot->addWidget(bootAdd);
    root->addLayout(boot);

    // ---- UC-05/06: joinable rooms (from peer heartbeats) ----
    root->addWidget(new QLabel(QStringLiteral("<b>可加入的聊天室</b>（随心跳广播到达，最多 8 间/节点）"), this));
    m_rooms = new QTableWidget(0, 5, this);
    m_rooms->setHorizontalHeaderLabels({QStringLiteral("房间名"), QStringLiteral("房间 ID"), QStringLiteral("成员"),
                                        QStringLiteral("策略"), QStringLiteral("托管者")});
    m_rooms->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_rooms->verticalHeader()->setVisible(false);
    m_rooms->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_rooms->setSelectionBehavior(QAbstractItemView::SelectRows);
    root->addWidget(m_rooms, 1);

    auto *row = new QHBoxLayout;
    auto *join = new QPushButton(QStringLiteral("加入所选聊天室"), this);
    connect(join, &QPushButton::clicked, this, &DiscoverView::joinSelected);
    auto *create = new QPushButton(QStringLiteral("创建聊天室…"), this);
    connect(create, &QPushButton::clicked, this, &DiscoverView::createRoomHere); // UC-05
    row->addWidget(join);
    row->addWidget(create);
    row->addStretch();
    root->addLayout(row);

    connect(m_engine->discovery(), &Discovery::peerSeen, this, [this] { refreshPeers(); refreshRooms(); });
    connect(m_engine->discovery(), &Discovery::peerGone, this, [this] { refreshPeers(); refreshRooms(); });
    connect(m_engine->discovery(), &Discovery::peersChanged, this, [this] { refreshPeers(); });
    refreshPeers();
    refreshRooms();
}

void DiscoverView::refreshPeers() {
    const auto peers = m_engine->discovery()->peers();
    m_peers->setRowCount(peers.size());
    for (int i = 0; i < peers.size(); ++i) {
        const auto &p = peers[i];
        m_peers->setItem(i, 0, new QTableWidgetItem(p.nickname));
        m_peers->setItem(i, 1, new QTableWidgetItem(p.fingerprint.left(16) + "…"));
        m_peers->setItem(i, 2, new QTableWidgetItem(p.ip));
        m_peers->setItem(i, 3, new QTableWidgetItem(QString::number(p.tcpPort)));
        auto *st = new QTableWidgetItem(p.online ? QStringLiteral("在线") : QStringLiteral("离线"));
        if (!p.online)
            st->setForeground(Qt::gray); // E1: grey, never silently drop (UC-03)
        m_peers->setItem(i, 4, st);
    }
}

void DiscoverView::refreshRooms() {
    QVector<RoomSummary> all;
    for (const auto &p : m_engine->discovery()->peers()) {
        for (const auto &r : p.hostedRooms)
            all.append(r);
    }
    m_rooms->setRowCount(all.size());
    for (int i = 0; i < all.size(); ++i) {
        const auto &r = all[i];
        QString host;
        for (const auto &p : m_engine->discovery()->peers())
            if (p.fingerprint == r.hostFp)
                host = p.nickname;
        m_rooms->setItem(i, 0, new QTableWidgetItem(r.name));
        m_rooms->setItem(i, 1, new QTableWidgetItem(r.roomId));
        m_rooms->setItem(i, 2, new QTableWidgetItem(QString::number(r.members)));
        m_rooms->setItem(i, 3, new QTableWidgetItem(r.policy == JoinPolicy::Approval ? QStringLiteral("需批准")
                                                                                     : QStringLiteral("自由加入")));
        m_rooms->setItem(i, 4, new QTableWidgetItem(host.isEmpty() ? r.hostFp.left(12) : host));
    }
}

void DiscoverView::joinSelected() {
    const int row = m_rooms->currentRow();
    if (row < 0)
        return;
    RoomSummary s;
    s.name = m_rooms->item(row, 0)->text();
    s.roomId = m_rooms->item(row, 1)->text();
    s.policy = m_rooms->item(row, 3)->text() == QStringLiteral("需批准") ? JoinPolicy::Approval : JoinPolicy::Open;
    // resolve host fingerprint from the host-nickname column bookkeeping
    for (const auto &p : m_engine->discovery()->peers()) {
        for (const auto &r : p.hostedRooms)
            if (r.roomId == s.roomId)
                s.hostFp = r.hostFp;
    }
    auto *d = new DDialog(this);
    d->setTitle(QStringLiteral("加入聊天室"));
    d->setMessage(QStringLiteral("正在连接托管节点…"));
    d->addButton(QStringLiteral("关闭"));
    d->open();
    m_engine->rooms()->joinRoom(s, [d](JoinStatus st, const QString &detail) {
        // UC-06 outcomes incl. E1 host gone / E2 full / A1 pending approval
        const QString text = st == JoinStatus::Accepted        ? QStringLiteral("已成功加入")
                             : st == JoinStatus::PendingApproval ? QStringLiteral("申请已提交，等待托管者批准")
                             : QStringLiteral("未能加入：%1").arg(detail);
        d->setMessage(text);
    });
}

void DiscoverView::createRoomHere() {
    // the full UC-05 dialog lives in RoomsView; MainWindow wires the hop
    emit createRoomRequested();
}

} // namespace meshlink
