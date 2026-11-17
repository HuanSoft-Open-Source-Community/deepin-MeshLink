// deepin-MeshLink — discovery view implementation (UC-04/05/06).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "discoverview.h"

#include "core/discovery.h"
#include "core/engine.h"
#include "core/lansegment.h"
#include "core/roommanager.h"
#include "core/settings.h"
#include "uiutils.h"

#include <DDialog>
#include <DFontSizeManager>
#include <DFrame>
#include <DLabel>
#include <DLineEdit>
#include <DMessageManager>
#include <DPaletteHelper>
#include <DPushButton>
#include <DSuggestButton>
#include <dinputdialog.h>

#include <QHeaderView>
#include <QHostAddress>
#include <QHBoxLayout>
#include <QPointer>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

DWIDGET_USE_NAMESPACE
DGUI_USE_NAMESPACE

namespace meshlink {
namespace {

DLabel *sectionLabel(const QString &text, QWidget *parent) {
    auto *label = new DLabel(text, parent);
    label->setForegroundRole(DPalette::TextTitle);
    DFontSizeManager::instance()->bind(label, DFontSizeManager::T5, QFont::DemiBold);
    return label;
}

} // namespace

DiscoverView::DiscoverView(Engine *engine, QWidget *parent) : QWidget(parent), m_engine(engine) {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(24, 20, 24, 20);
    root->setSpacing(14);

    auto *create = new DSuggestButton(tr("Create room"), this);
    create->setObjectName(QStringLiteral("discoverView.createRoomButton"));
    create->setIcon(UiUtils::icon(QStringLiteral("list-add")));
    connect(create, &DSuggestButton::clicked, this, &DiscoverView::createRoomHere);
    root->addWidget(UiUtils::pageHeader(tr("Discover"), create, this));

    // ---- current LAN segment ----
    auto *segmentCard = new DFrame(this);
    segmentCard->setLineWidth(1);
    auto *segmentLayout = new QHBoxLayout(segmentCard);
    segmentLayout->setContentsMargins(12, 10, 12, 10);
    segmentLayout->setSpacing(10);
    const QIcon networkIcon = UiUtils::icon(QStringLiteral("network-wired"),
                                            QStringLiteral("network-workgroup"));
    auto *segmentIcon = new QLabel(segmentCard);
    if (!networkIcon.isNull())
        segmentIcon->setPixmap(networkIcon.pixmap(24, 24));
    else
        segmentIcon->hide();
    m_segment = new DLabel(segmentCard);
    m_segment->setWordWrap(true);
    m_segment->setForegroundRole(DPalette::TextTips);
    auto *change = new DPushButton(tr("Change…"), segmentCard);
    connect(change, &DPushButton::clicked, this, [this] {
        emit settingsRequested(QStringLiteral("network.discovery"));
    });
    segmentLayout->addWidget(segmentIcon);
    segmentLayout->addWidget(m_segment, 1);
    segmentLayout->addWidget(change);
    root->addWidget(segmentCard);

    // ---- peers ----
    root->addWidget(sectionLabel(tr("LAN nodes"), this));
    m_peers = new QTableView(this);
    m_peers->setObjectName(QStringLiteral("discoverView.peersTable"));
    UiUtils::setupTable(m_peers);
    m_peerModel = new QStandardItemModel(0, 5, this);
    m_peerModel->setHorizontalHeaderLabels(
        {tr("Nickname"), tr("Fingerprint"), tr("IPv4"), tr("TLS port"), tr("Status")});
    m_peers->setModel(m_peerModel);
    m_peers->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 5; ++column)
        m_peers->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    m_peers->setMinimumHeight(130);
    root->addWidget(m_peers, 1);

    // ---- joinable rooms ----
    auto *roomsHeader = new QHBoxLayout;
    roomsHeader->setContentsMargins(0, 0, 0, 0);
    roomsHeader->setSpacing(8);
    roomsHeader->addWidget(sectionLabel(tr("Joinable rooms"), this));
    roomsHeader->addStretch(1);
    auto *join = new DPushButton(tr("Join selected"), this);
    connect(join, &DPushButton::clicked, this, &DiscoverView::joinSelected);
    roomsHeader->addWidget(join);
    root->addLayout(roomsHeader);

    m_rooms = new QTableView(this);
    m_rooms->setObjectName(QStringLiteral("discoverView.roomsTable"));
    UiUtils::setupTable(m_rooms);
    m_roomModel = new QStandardItemModel(0, 6, this);
    m_roomModel->setHorizontalHeaderLabels({tr("Room"), tr("Invite code"), tr("Room fingerprint"),
                                            tr("Members"), tr("Policy"), tr("Host")});
    m_rooms->setModel(m_roomModel);
    m_rooms->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 6; ++column)
        m_rooms->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    m_rooms->setMinimumHeight(130);
    root->addWidget(m_rooms, 1);

    // ---- join by invite code ----
    auto *codeRow = new QHBoxLayout;
    codeRow->setContentsMargins(0, 0, 0, 0);
    codeRow->setSpacing(8);
    m_inviteCode = new DLineEdit(this);
    m_inviteCode->setObjectName(QStringLiteral("discoverView.inviteEdit"));
    m_inviteCode->setPlaceholderText(tr("Enter the 6-digit invite code"));
    m_inviteCode->lineEdit()->setMaxLength(kInviteCodeDigits);
    m_inviteCode->setInputMethodHints(Qt::ImhDigitsOnly);
    m_inviteCode->lineEdit()->setValidator(
        new QRegularExpressionValidator(QRegularExpression(QStringLiteral("\\d{0,6}")), m_inviteCode));
    auto *joinByCode = new DPushButton(tr("Join by invite code"), this);
    joinByCode->setObjectName(QStringLiteral("discoverView.joinByCodeButton"));
    connect(joinByCode, &DPushButton::clicked, this, &DiscoverView::joinByInviteCode);
    codeRow->addWidget(m_inviteCode, 1);
    codeRow->addWidget(joinByCode);
    root->addLayout(codeRow);

    // ---- manual bootstrap ----
    auto *bootstrapHint = new DLabel(
        tr("Broadcast blocked? Add a node IP as a unicast discovery target."), this);
    bootstrapHint->setForegroundRole(DPalette::TextTips);
    root->addWidget(bootstrapHint);

    auto *bootRow = new QHBoxLayout;
    bootRow->setContentsMargins(0, 0, 0, 0);
    bootRow->setSpacing(8);
    auto *bootEdit = new DLineEdit(this);
    bootEdit->setPlaceholderText(tr("Node IPv4 address"));
    auto *bootAdd = new DPushButton(tr("Add"), this);
    connect(bootAdd, &DPushButton::clicked, this, [this, bootEdit] {
        const QString ip = bootEdit->text().trimmed();
        const QHostAddress address(ip);
        if (address.protocol() != QAbstractSocket::IPv4Protocol) {
            bootEdit->setAlert(true);
            bootEdit->showAlertMessage(tr("Enter a valid IPv4 address."));
            return;
        }
        m_engine->settings()->addBootstrapTarget(ip, m_engine->settings()->discoveryPort());
        m_engine->discovery()->addManualTarget(ip, m_engine->settings()->discoveryPort());
        bootEdit->clear();
        bootEdit->setAlert(false);
        DMessageManager::instance()->sendMessage(
            this, UiUtils::icon(QStringLiteral("dialog-ok")),
            tr("Discovery target added: %1").arg(ip));
    });
    connect(bootEdit->lineEdit(), &QLineEdit::textChanged, bootEdit,
            [bootEdit] { bootEdit->setAlert(false); });
    bootRow->addWidget(bootEdit, 1);
    bootRow->addWidget(bootAdd);
    root->addLayout(bootRow);

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
    m_segment->setText(segment.isValid() ? tr("Current segment: %1").arg(segment.label())
                                         : tr("No usable IPv4 segment is selected."));
    m_segment->setToolTip(tr("Heartbeats are broadcast into and accepted only from this segment."));
}

void DiscoverView::refreshPeers() {
    const QVector<PeerInfo> peers = m_engine->discovery()->peers();
    m_peerModel->setRowCount(peers.size());
    const QColor tips = DPaletteHelper::instance()->palette(m_peers).color(DPalette::TextTips);
    for (int row = 0; row < peers.size(); ++row) {
        const PeerInfo &peer = peers.at(row);
        m_peerModel->setItem(row, 0, new QStandardItem(peer.nickname));
        m_peerModel->setItem(row, 1,
                             new QStandardItem(UiUtils::shortFp(peer.fingerprint, 16)));
        m_peerModel->setItem(row, 2, new QStandardItem(peer.ip));
        m_peerModel->setItem(row, 3, new QStandardItem(QString::number(peer.tcpPort)));
        auto *status = new QStandardItem(peer.online ? tr("Online") : tr("Offline"));
        if (!peer.online)
            status->setForeground(tips); // E1: grey, never silently drop (UC-03)
        m_peerModel->setItem(row, 4, status);
        m_peerModel->item(row, 1)->setToolTip(peer.fingerprint);
    }
}

void DiscoverView::refreshRooms() {
    const QVector<RoomSummary> rooms = m_engine->discovery()->roomCatalogue();
    m_roomModel->setRowCount(rooms.size());
    for (int row = 0; row < rooms.size(); ++row) {
        const RoomSummary &room = rooms.at(row);
        QString host;
        for (const PeerInfo &peer : m_engine->discovery()->peers()) {
            if (peer.fingerprint == room.hostFp) {
                host = peer.nickname;
                break;
            }
        }
        auto *name = new QStandardItem(room.name);
        name->setData(room.roomId, Qt::UserRole);
        m_roomModel->setItem(row, 0, name);
        auto *code = new QStandardItem(room.inviteCode);
        code->setToolTip(tr("Invite code: %1").arg(room.inviteCode));
        m_roomModel->setItem(row, 1, code);
        auto *fingerprint = new QStandardItem(UiUtils::shortFp(room.roomId, 16));
        fingerprint->setToolTip(room.roomId);
        m_roomModel->setItem(row, 2, fingerprint);
        m_roomModel->setItem(row, 3, new QStandardItem(QString::number(room.members)));
        m_roomModel->setItem(
            row, 4,
            new QStandardItem(room.policy == JoinPolicy::Approval ? tr("Needs approval")
                                                                : tr("Open")));
        m_roomModel->setItem(row, 5,
                             new QStandardItem(host.isEmpty() ? UiUtils::shortFp(room.hostFp, 12)
                                                              : host));
    }
}

void DiscoverView::joinSelected() {
    const QModelIndex index = m_rooms->currentIndex();
    if (!index.isValid())
        return;
    const QString roomId = m_roomModel->item(index.row(), 0)->data(Qt::UserRole).toString();
    if (auto summary = m_engine->discovery()->roomById(roomId))
        joinSummary(*summary);
}

void DiscoverView::joinByInviteCode() {
    const QString code = m_inviteCode->text().trimmed();
    if (code.size() != kInviteCodeDigits) {
        m_inviteCode->setAlert(true);
        m_inviteCode->showAlertMessage(tr("The invite code must be exactly 6 digits."));
        return;
    }
    QVector<RoomSummary> hits;
    for (const RoomSummary &room : m_engine->discovery()->roomCatalogue()) {
        if (room.inviteCode == code)
            hits.append(room);
    }
    if (hits.isEmpty()) {
        UiUtils::warning(
            this, tr("Invite code"),
            tr("No room with this invite code was found on the current segment.\n"
               "Make sure both nodes are on the same segment, the host is online, and both "
               "clients are up to date."));
        return;
    }
    if (hits.size() == 1) {
        joinSummary(hits.first());
        return;
    }
    QStringList labels;
    for (const RoomSummary &room : hits)
        labels << tr("%1 · host %2 · %3 members")
                      .arg(room.name, UiUtils::shortFp(room.hostFp, 8))
                      .arg(room.members);
    bool ok = false;
    const QString picked =
        DInputDialog::getItem(this, tr("Multiple rooms share this code"),
                              tr("Choose the room to join:"), labels, 0, false, &ok);
    if (!ok || picked.isEmpty())
        return;
    const int index = labels.indexOf(picked);
    if (index >= 0 && index < hits.size())
        joinSummary(hits.at(index));
}

void DiscoverView::joinSummary(const RoomSummary &summary) {
    auto *dialog = new DDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setTitle(tr("Join room"));
    dialog->setMessage(tr("Connecting to the host…"));
    dialog->addButton(tr("Close"));
    QPointer<DDialog> guard(dialog);
    guard->open();

    m_engine->rooms()->joinRoom(summary, [guard](JoinStatus status, const QString &) {
        // UC-06 outcomes incl. E1 host gone / E2 full / A1 pending approval.
        // Derive local text from the status; never trust the peer's language.
        if (!guard)
            return;
        guard->setMessage(UiUtils::joinStatusText(status));
        guard->setIcon(UiUtils::icon(status == JoinStatus::Accepted ||
                                             status == JoinStatus::PendingApproval
                                         ? QStringLiteral("dialog-information")
                                         : QStringLiteral("dialog-warning")));
    });
}

void DiscoverView::createRoomHere() {
    // the full UC-05 dialog lives in RoomsView; MainWindow wires the hop
    emit createRoomRequested();
}

} // namespace meshlink
