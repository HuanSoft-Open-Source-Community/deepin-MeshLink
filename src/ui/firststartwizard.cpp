// deepin-MeshLink — first-start wizard implementation (UC-01).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "firststartwizard.h"

#include "core/engine.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
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
    m_error = new QLabel(form);
    m_error->setWordWrap(true);
    m_error->setStyleSheet(QStringLiteral("color:#e53935;"));
    lay->addRow(QStringLiteral("昵称："), m_nick);
    lay->addRow(m_error);
    addContent(form);

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
    QString err;
    // UC-01 steps: keygen -> fingerprint -> encrypted keystore -> profile
    if (!m_engine->createFreshAccount(m_nick->text().trimmed(), &err)) {
        m_error->setText(err.isEmpty() ? QStringLiteral("密钥生成失败，请重试") : err); // E1
        return;
    }
    accept();
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
