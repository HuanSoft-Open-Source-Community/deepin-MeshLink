// deepin-MeshLink — settings page implementation.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "settingsview.h"

#include "core/engine.h"
#include "core/lansegment.h"
#include "core/settings.h"

#include <DDialog>

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

DWIDGET_USE_NAMESPACE

namespace meshlink {

SettingsView::SettingsView(Engine *engine, QWidget *parent) : QWidget(parent), m_engine(engine) {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(24, 24, 24, 24);
    root->addWidget(new QLabel(QStringLiteral("<b>网络与传输参数</b>（持久化于用户配置；端口类修改需重启生效）"), this));

    auto *form = new QFormLayout;
    m_segments = new QComboBox(this);
    auto *segRow = new QWidget(this);
    auto *segLay = new QHBoxLayout(segRow);
    segLay->setContentsMargins(0, 0, 0, 0);
    segLay->addWidget(m_segments, 1);
    auto *refresh = new QPushButton(QStringLiteral("刷新"), segRow);
    segLay->addWidget(refresh);
    connect(refresh, &QPushButton::clicked, this, &SettingsView::reloadSegments);
    form->addRow(QStringLiteral("局域网网段："), segRow);
    m_udp = new QSpinBox(this);
    m_udp->setRange(1024, 65535);
    m_udp->setSuffix(QStringLiteral("（默认 2425）"));
    form->addRow(QStringLiteral("UDP 发现端口："), m_udp);
    m_tcp = new QSpinBox(this);
    m_tcp->setRange(1024, 65535);
    m_tcp->setSuffix(QStringLiteral("（默认 2426）"));
    form->addRow(QStringLiteral("TLS 服务端口："), m_tcp);
    m_hb = new QSpinBox(this);
    m_hb->setRange(500, 60000);
    m_hb->setSingleStep(500);
    m_hb->setSuffix(QStringLiteral(" ms"));
    form->addRow(QStringLiteral("心跳间隔："), m_hb);
    m_offline = new QSpinBox(this);
    m_offline->setRange(2, 10);
    m_offline->setSuffix(QStringLiteral(" 个周期"));
    form->addRow(QStringLiteral("离线判定："), m_offline);
    m_chunk = new QSpinBox(this);
    m_chunk->setRange(64, 1024);
    m_chunk->setSingleStep(64);
    m_chunk->setSuffix(QStringLiteral(" KiB（64–1024，推荐区间）"));
    form->addRow(QStringLiteral("分片大小："), m_chunk);
    m_threads = new QSpinBox(this);
    m_threads->setRange(1, 16);
    form->addRow(QStringLiteral("下载并行线程："), m_threads);
    m_history = new QSpinBox(this);
    m_history->setRange(20, 1000);
    m_history->setSingleStep(20);
    form->addRow(QStringLiteral("快照消息上限："), m_history);
    root->addLayout(form);

    auto *bb = new QDialogButtonBox(this);
    auto *apply = bb->addButton(QStringLiteral("应用"), QDialogButtonBox::AcceptRole);
    bb->addButton(QStringLiteral("重启应用"), QDialogButtonBox::ResetRole);
    connect(apply, &QPushButton::clicked, this, &SettingsView::apply);
    connect(bb, &QDialogButtonBox::rejected, this, [] {
        auto *d = new DDialog(nullptr);
        d->setTitle(QStringLiteral("即将退出"));
        d->setMessage(QStringLiteral("端口/间隔类设置将在下次启动时生效。"));
        d->addButton(QStringLiteral("现在退出"), true, DDialog::ButtonWarning);
        d->addButton(QStringLiteral("稍后"));
        connect(d, &DDialog::buttonClicked, d, [](int index, const QString &) {
            if (index == 0)
                qApp->closeAllWindows();
        });
        d->open();
    });
    root->addWidget(bb);

    m_status = new QLabel(this);
    m_status->setStyleSheet(QStringLiteral("color:#43a047;"));
    root->addWidget(m_status);

    auto *about = new QLabel(
        QStringLiteral("<b>深度织讯 deepin-MeshLink</b> v" MESHLINK_VERSION " " MESHLINK_STAGE "<br>"
                       "开发主体：<a href='https://huansoft.org.cn'>HuanSoft 开源社区</a><br>"
                       "许可：GPL-3.0-or-later · 全链路 TLS 1.3 · 无中心服务器"),
        this);
    about->setOpenExternalLinks(true);
    root->addWidget(about);
    root->addStretch();

    load();
}

void SettingsView::load() {
    auto *s = m_engine->settings();
    reloadSegments();
    m_udp->setValue(s->discoveryPort());
    m_tcp->setValue(s->tcpPort());
    m_hb->setValue(s->heartbeatIntervalMs());
    m_offline->setValue(s->offlinePeriods());
    m_chunk->setValue(int(s->chunkSize() / 1024));
    m_threads->setValue(s->downloadThreads());
    m_history->setValue(s->maxRoomHistory());
}

void SettingsView::reloadSegments() {
    auto *s = m_engine->settings();
    const LanSegment active = s->lanSegment();
    const bool forced = LanSegments::fromEnvironment().isValid();
    m_segmentList = LanSegments::enumerate();
    m_segments->clear();
    int select = -1;
    for (int i = 0; i < m_segmentList.size(); ++i) {
        const auto &seg = m_segmentList[i];
        m_segments->addItem(seg.label());
        if (seg.ip == active.ip && seg.prefixLength == active.prefixLength)
            select = i;
    }
    if (select < 0 && !m_segmentList.isEmpty())
        select = 0;
    if (select >= 0)
        m_segments->setCurrentIndex(select);
    m_segments->setEnabled(!forced);
    m_segments->setToolTip(forced ? QStringLiteral("当前由 MESHLINK_IP 环境变量固定，界面选择不生效")
                                  : QStringLiteral("切换后立即在新网段广播心跳，旧网段 peer 会被清空"));
}

void SettingsView::apply() {
    auto *s = m_engine->settings();
    const int row = m_segments->currentIndex();
    if (!LanSegments::fromEnvironment().isValid() && row >= 0 && row < m_segmentList.size()) {
        QString segError;
        if (!m_engine->setLanSegment(m_segmentList[row], &segError)) {
            m_status->setStyleSheet(QStringLiteral("color:#e53935;"));
            m_status->setText(segError);
            return;
        }
    }
    s->setDiscoveryPort(quint16(m_udp->value()));
    s->setTcpPort(quint16(m_tcp->value()));
    s->setHeartbeatIntervalMs(m_hb->value());
    s->setOfflinePeriods(m_offline->value());
    s->setChunkSizeKb(m_chunk->value());
    s->setDownloadThreads(m_threads->value());
    s->setMaxRoomHistory(m_history->value());
    // heartbeat cadence applies live to the running discovery loop:
    m_engine->discovery()->restartTimer();
    m_status->setStyleSheet(QStringLiteral("color:#43a047;"));
    m_status->setText(QStringLiteral("已保存：网段与心跳即时生效，端口改动需重启后监听新端口"));
}

} // namespace meshlink
