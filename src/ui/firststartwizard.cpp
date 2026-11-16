// deepin-MeshLink — first-start wizard implementation (UC-01).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "firststartwizard.h"

#include "core/engine.h"
#include "core/lansegment.h"
#include "core/settings.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

DWIDGET_USE_NAMESPACE

namespace meshlink {

FirstStartWizard::FirstStartWizard(Engine *engine, QWidget *parent) : DDialog(parent), m_engine(engine) {
    setTitle(QStringLiteral("欢迎使用深度织讯"));
    setWindowTitle(QStringLiteral("深度织讯 — 初始化账户"));

    auto *form = new QWidget(this);
    auto *lay = new QFormLayout(form);
    m_nick = new QLineEdit(form);
    m_nick->setPlaceholderText(QStringLiteral("留空则自动生成（2–32 字符，UC-03 规则校验）"));
    m_segments = new QComboBox(form);
    auto *segRow = new QWidget(form);
    auto *segLay = new QHBoxLayout(segRow);
    segLay->setContentsMargins(0, 0, 0, 0);
    segLay->addWidget(m_segments, 1);
    auto *refresh = new QPushButton(QStringLiteral("刷新"), segRow);
    segLay->addWidget(refresh);
    m_error = new QLabel(form);
    m_error->setWordWrap(true);
    m_error->setStyleSheet(QStringLiteral("color:#e53935;"));
    lay->addRow(QStringLiteral("昵称："), m_nick);
    lay->addRow(QStringLiteral("局域网网段："), segRow);
    lay->addRow(m_error);
    addContent(form);

    connect(refresh, &QPushButton::clicked, this, &FirstStartWizard::reloadSegments);
    reloadSegments();

    addButton(QStringLiteral("创建账户"), true, DDialog::ButtonRecommend);
    addButton(QStringLiteral("退出"));

    connect(this, &DDialog::buttonClicked, this, [this](int index, const QString &) {
        if (index == 0)
            createAccount();
        else
            reject();
    });
}

void FirstStartWizard::createAccount() {
    // Persist the LAN segment first: account creation snapshots the LAN address
    // (UC-01 step 7) and Engine::start() will bring discovery up on this segment.
    m_engine->settings()->setLanSegment(selectedSegment());
    QString err;
    // UC-01 steps: keygen -> fingerprint -> encrypted keystore -> profile
    if (!m_engine->createFreshAccount(m_nick->text().trimmed(), &err)) {
        m_error->setText(err.isEmpty() ? QStringLiteral("密钥生成失败，请重试") : err); // E1
        return;
    }
    accept();
}

void FirstStartWizard::reloadSegments() {
    const LanSegment current = m_engine->settings()->lanSegment();
    const bool forced = LanSegments::fromEnvironment().isValid();
    m_segmentList = LanSegments::enumerate();
    m_segments->clear();
    int select = -1;
    for (int i = 0; i < m_segmentList.size(); ++i) {
        const auto &s = m_segmentList[i];
        m_segments->addItem(s.label());
        if (s.ip == current.ip && s.prefixLength == current.prefixLength)
            select = i;
    }
    if (select < 0 && !m_segmentList.isEmpty())
        select = 0;
    if (select >= 0)
        m_segments->setCurrentIndex(select);
    // MESHLINK_IP pins the segment for tests/headless runs; UI must not pretend
    // a different choice would be applied.
    m_segments->setEnabled(!forced);
    m_segments->setToolTip(forced ? QStringLiteral("当前由 MESHLINK_IP 环境变量固定，界面选择不生效")
                                  : QStringLiteral("心跳只在该网段广播并接收；运行中可在设置页随时切换"));
    if (m_segmentList.isEmpty())
        m_error->setText(QStringLiteral("未发现可用 IPv4 网卡，将使用本机回环地址"));
}

LanSegment FirstStartWizard::selectedSegment() const {
    const int row = m_segments ? m_segments->currentIndex() : -1;
    if (row >= 0 && row < m_segmentList.size())
        return m_segmentList[row];
    return m_engine->settings()->lanSegment();
}

bool FirstStartWizard::ensureAccount(Engine *engine, QWidget *parent) {
    // Before Engine::start() the in-memory profile is not loaded yet, so ask the
    // engine whether an account row already exists in the local database.
    if (engine->hasLocalAccount())
        return true;
    FirstStartWizard w(engine, parent);
    return w.exec() == QDialog::Accepted;
}

bool FirstStartWizard::runAsRecovery(Engine *engine, QWidget *parent) {
    FirstStartWizard w(engine, parent);
    w.setMessage(QStringLiteral("本机账户无法解密（设备或凭据已变更）。\n创建新账户将清除旧账户的本地数据，且不可恢复。"));
    if (w.exec() != QDialog::Accepted)
        return false;
    QString err;
    if (!engine->start(&err)) { // retry bringing up networking with the fresh identity
        w.setTitle(QStringLiteral("启动失败"));
        w.m_error->setText(err);
        return false;
    }
    return true;
}

} // namespace meshlink
