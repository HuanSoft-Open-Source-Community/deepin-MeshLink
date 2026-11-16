// deepin-MeshLink — discovery view implementation (UC-04/05/06).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "discoverview.h"

#include "core/discovery.h"
#include "core/engine.h"
#include "core/lansegment.h"
#include "core/roommanager.h"
#include "core/settings.h"

#include <DDialog>

#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

DWIDGET_USE_NAMESPACE

namespace meshlink {

DiscoverView::DiscoverView(Engine *engine, QWidget *parent) : QWidget(parent), m_engine(engine) {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);

    // ---- UC-04: current LAN segment + live nodes ----
    m_segment = new QLabel(this);
    root->addWidget(m_segment);
    root->addWidget(new QLabel(QStringLiteral("<b>局域网节点</b>（自动发现；超过离线阈值未再见则置灰）"), this));
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
    m_rooms = new QTableWidget(0, 6, this);
    m_rooms->setHorizontalHeaderLabels({QStringLiteral("房间名"), QStringLiteral("邀请码"), QStringLiteral("房间指纹"),
                                        QStringLiteral("成员"), QStringLiteral("策略"), QStringLiteral("托管者")});
    m_rooms->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_rooms->verticalHeader()->setVisible(false);
    m_rooms->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_rooms->setSelectionBehavior(QAbstractItemView::SelectRows);
    root->addWidget(m_rooms, 1);

    auto *codeRow = new QHBoxLayout;
    m_inviteCode = new QLineEdit(this);
    m_inviteCode->setMaxLength(kInviteCodeDigits);
    m_inviteCode->setPlaceholderText(QStringLiteral("输入 6 位数字邀请码"));
    auto *codeJoin = new QPushButton(QStringLiteral("按邀请码加入"), this);
    connect(codeJoin, &QPushButton::clicked, this, &DiscoverView::joinByInviteCode);
    codeRow->addWidget(m_inviteCode);
    codeRow->addWidget(codeJoin);
    codeRow->addStretch();
    root->addLayout(codeRow);

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
    connect(m_engine->discovery(), &Discovery::peersChanged, this, [this] { refreshPeers(); refreshRooms(); });
    connect(m_engine, &Engine::lanSegmentChanged, this, [this] { refreshSegment(); });
    refreshSegment();
    refreshPeers();
    refreshRooms();
}

void DiscoverView::refreshSegment() {
    const LanSegment segment = m_engine->lanSegment();
    m_segment->setText(QStringLiteral("<b>当前网段</b>：%1（只接受该网段内的心跳；可在“设置”中随时切换）")
                           .arg(segment.label()));
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
    const auto all = m_engine->discovery()->roomCatalogue();
    m_rooms->setRowCount(all.size());
    for (int i = 0; i < all.size(); ++i) {
        const auto &r = all[i];
        QString host;
        for (const auto &p : m_engine->discovery()->peers())
            if (p.fingerprint == r.hostFp)
                host = p.nickname;
        auto *name = new QTableWidgetItem(r.name);
        name->setData(Qt::UserRole, r.roomId);
        m_rooms->setItem(i, 0, name);
        auto *code = new QTableWidgetItem(r.inviteCode);
        code->setToolTip(QStringLiteral("邀请码：%1").arg(r.inviteCode));
        m_rooms->setItem(i, 1, code);
        auto *id = new QTableWidgetItem(r.roomId.left(16) + "…");
        id->setToolTip(r.roomId);
        m_rooms->setItem(i, 2, id);
        m_rooms->setItem(i, 3, new QTableWidgetItem(QString::number(r.members)));
        m_rooms->setItem(i, 4, new QTableWidgetItem(r.policy == JoinPolicy::Approval ? QStringLiteral("需批准")
                                                                                      : QStringLiteral("自由加入")));
        m_rooms->setItem(i, 5, new QTableWidgetItem(host.isEmpty() ? r.hostFp.left(12) : host));
    }
}

void DiscoverView::joinSelected() {
    const int row = m_rooms->currentRow();
    if (row < 0)
        return;
    const QString roomId = m_rooms->item(row, 0)->data(Qt::UserRole).toString();
    if (auto summary = m_engine->discovery()->roomById(roomId))
        joinSummary(*summary);
}

void DiscoverView::joinByInviteCode() {
    const QString code = m_inviteCode->text().trimmed();
    if (code.size() != kInviteCodeDigits) {
        QMessageBox::warning(this, QStringLiteral("邀请码"), QStringLiteral("邀请码必须是 6 位数字。"));
        return;
    }
    QVector<RoomSummary> hits;
    for (const auto &r : m_engine->discovery()->roomCatalogue())
        if (r.inviteCode == code)
            hits.append(r);
    if (hits.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("邀请码"),
                             QStringLiteral("当前网段未发现该邀请码对应的聊天室。\n"
                                            "请确认双方在同一网段、聊天室托管节点在线且客户端版本一致。"));
        return;
    }
    if (hits.size() == 1) {
        joinSummary(hits.first());
        return;
    }
    QStringList labels;
    for (const auto &r : hits)
        labels << QStringLiteral("%1 · 托管者 %2… · %3 人").arg(r.name, r.hostFp.left(8)).arg(r.members);
    bool ok = false;
    const QString picked = QInputDialog::getItem(this, QStringLiteral("邀请码命中多个聊天室"),
                                                 QStringLiteral("请选择要加入的聊天室："), labels, 0, false, &ok);
    if (!ok || picked.isEmpty())
        return;
    const int index = labels.indexOf(picked);
    if (index >= 0 && index < hits.size())
        joinSummary(hits[index]);
}

void DiscoverView::joinSummary(const RoomSummary &summary) {
    auto *d = new DDialog(this);
    d->setTitle(QStringLiteral("加入聊天室"));
    d->setMessage(QStringLiteral("正在连接托管节点…"));
    d->addButton(QStringLiteral("关闭"));
    d->open();
    m_engine->rooms()->joinRoom(summary, [d](JoinStatus st, const QString &detail) {
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
