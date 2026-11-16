// deepin-MeshLink — main window implementation.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "mainwindow.h"

#include "chatview.h"
#include "discoverview.h"
#include "firststartwizard.h"
#include "profilepage.h"
#include "roomsview.h"
#include "settingsview.h"
#include "transfersview.h"

#include <DDialog>
#include <DTitlebar>

#include <QApplication>
#include <QLabel>
#include <QListWidget>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

DWIDGET_USE_NAMESPACE

namespace meshlink {

MainWindow::MainWindow(Engine *engine, QWidget *parent) : DMainWindow(parent), m_engine(engine) {
    setWindowTitle(QStringLiteral("深度织讯"));
    resize(1024, 680);

    auto *split = new QSplitter(this);
    m_nav = new QListWidget(split);
    m_nav->setFrameShape(QFrame::NoFrame);
    m_nav->setMaximumWidth(180);
    m_nav->addItem(QStringLiteral("会话"));     // rooms & chat (UC-05..09/13)
    m_nav->addItem(QStringLiteral("发现"));     // LAN peers & joinable rooms (UC-04..06)
    m_nav->addItem(QStringLiteral("传输"));     // downloads (UC-10..12)
    m_nav->addItem(QStringLiteral("个人资料")); // UC-01/02/03
    m_nav->addItem(QStringLiteral("设置"));     // NFR parameters

    m_stack = new QStackedWidget(split);
    m_rooms = new RoomsView(m_engine, m_stack);
    m_discover = new DiscoverView(m_engine, m_stack);
    m_transfers = new TransfersView(m_engine, m_stack);
    m_profile = new ProfilePage(m_engine, m_stack);
    m_settings = new SettingsView(m_engine, m_stack);
    m_stack->addWidget(m_rooms);
    m_stack->addWidget(m_discover);
    m_stack->addWidget(m_transfers);
    m_stack->addWidget(m_profile);
    m_stack->addWidget(m_settings);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    setCentralWidget(split);

    connect(m_nav, &QListWidget::currentRowChanged, m_stack, &QStackedWidget::setCurrentIndex);

    // status bar: own endpoint + network size (UC-04 step 4/5 diagnostics)
    auto *bar = new QStatusBar(this);
    m_statusLabel = new QLabel(bar);
    m_warnLabel = new QLabel(bar);
    m_warnLabel->setStyleSheet(QStringLiteral("color:#e53935;"));
    bar->addWidget(m_statusLabel, 1);
    bar->addPermanentWidget(m_warnLabel);
    setStatusBar(bar);

    m_nav->setCurrentRow(0);

    QTimer::singleShot(2000, this, [this] { refreshStatus(); });
    connect(m_engine->discovery(), &Discovery::peerSeen, this, &MainWindow::refreshStatus);
    connect(m_engine->discovery(), &Discovery::peerGone, this, &MainWindow::refreshStatus);
    connect(m_engine->discovery(), &Discovery::peersChanged, this, &MainWindow::refreshStatus);
    connect(m_engine->rooms(), &RoomManager::roomsChanged, this, &MainWindow::refreshStatus);

    // UC-03 A1 / UC-04 E3: suspected MITM surfaced globally
    connect(m_engine, &Engine::spoofingSuspected, this, [this](const QString &fp) {
        m_warnLabel->setText(QStringLiteral("⚠ 检测到伪造嫌疑节点：%1…").arg(fp.left(12)));
    });
    // cross-page wiring: discover -> create dialog; chat download start -> transfers page
    connect(m_discover, &DiscoverView::createRoomRequested, this, [this] {
        m_nav->setCurrentRow(0);
        m_rooms->openCreateDialog();
    });
    connect(m_rooms, &RoomsView::downloadsStarted, this, [this] { m_nav->setCurrentRow(2); });
    // UC-02: after account destroy, app must restart to re-initialise
    connect(m_profile, &ProfilePage::accountDestroyed, this, [] {
        auto *d = new DDialog(nullptr);
        d->setTitle(QStringLiteral("账户已销毁"));
        d->setMessage(QStringLiteral("本地账户与密钥已安全擦除。请重新启动深度织讯以创建新账户。"));
        d->addButton(QStringLiteral("退出应用"), true, DDialog::ButtonNormal);
        connect(d, &DDialog::buttonClicked, d, [](int, const QString &) { qApp->quit(); });
        d->open();
    });
    connect(m_engine->link(), &SecureLink::mitmSuspected, this,
            [this](const QString &fp, const QString &detail) {
                auto *d = new DDialog(this);
                d->setTitle(QStringLiteral("安全警告"));
                d->setMessage(QStringLiteral("节点 %1 的证书与已固定指纹不一致：%2").arg(fp.left(16), detail));
                d->addButton(QStringLiteral("知道了"), true, DDialog::ButtonWarning);
                d->open();
            });

    refreshStatus();
}

void MainWindow::showStartupError(const QString &error) {
    QTimer::singleShot(0, this, [this, error] {
        if (error.contains(QStringLiteral("端口"))) { // UC-04 E1
            auto *d = new DDialog(this);
            d->setTitle(QStringLiteral("无法启动网络服务"));
            d->setMessage(error + QStringLiteral("\n请在“设置”中更换发现/服务端口后重启。"));
            d->addButton(QStringLiteral("打开设置"), true, DDialog::ButtonRecommend);
            d->addButton(QStringLiteral("退出"));
            connect(d, &DDialog::buttonClicked, this, [this](int index, const QString &) {
                if (index == 0)
                    m_nav->setCurrentRow(4);
                else
                    qApp->quit();
            });
            d->open();
        } else { // UC-01 A1: identity unreadable -> account wizard
            FirstStartWizard::runAsRecovery(m_engine, this);
        }
    });
}

void MainWindow::refreshStatus() {
    const auto prof = m_engine->account()->profile();
    const int online = m_engine->discovery()->peers().size();
    int hosted = 0;
    for (const auto &r : m_engine->rooms()->myRooms())
        if (r.hosted)
            ++hosted;
    m_statusLabel->setText(QStringLiteral("指纹 %1… · %2:%3 (UDP %4) · 在线节点 %5 · 我托管的房间 %6")
                               .arg(prof.fingerprint.left(12), m_engine->tcpEndpointIp())
                               .arg(m_engine->tcpEndpointPort())
                               .arg(m_engine->settings()->discoveryPort())
                               .arg(online)
                               .arg(hosted));
}

} // namespace meshlink
