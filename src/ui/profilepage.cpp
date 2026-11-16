// deepin-MeshLink — profile page implementation.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "profilepage.h"

#include "core/account.h"
#include "core/engine.h"
#include "core/settings.h"

#include <DDialog>

#include <QCheckBox>
#include <QClipboard>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>

DWIDGET_USE_NAMESPACE

namespace meshlink {

ProfilePage::ProfilePage(Engine *engine, QWidget *parent) : QWidget(parent), m_engine(engine) {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(24, 24, 24, 24);

    auto *avatarRow = new QHBoxLayout;
    m_avatar = new QLabel(this);
    m_avatar->setFixedSize(96, 96);
    auto *change = new QPushButton(QStringLiteral("更换头像…"), this); // UC-03
    connect(change, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, QStringLiteral("选择头像图片"),
                                                       QDir::homePath(),
                                                       QStringLiteral("图片 (*.png *.jpg *.jpeg *.bmp *.webp)"));
        if (f.isEmpty())
            return;
        m_pendingAvatar = f;
        QPixmap pm(f);
        m_avatar->setPixmap(pm.scaled(96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    });
    avatarRow->addWidget(m_avatar);
    avatarRow->addWidget(change);
    avatarRow->addStretch();
    root->addLayout(avatarRow);

    auto *form = new QFormLayout;
    m_nick = new QLineEdit(this);
    m_nick->setPlaceholderText(QStringLiteral("2–32 个字符"));
    form->addRow(QStringLiteral("昵称："), m_nick);
    m_fp = new QLabel(this);
    m_fp->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_fp->setStyleSheet(QStringLiteral("font-family: monospace;"));
    auto *copy = new QPushButton(QStringLiteral("复制"), this);
    connect(copy, &QPushButton::clicked, this, [this] {
        QGuiApplication::clipboard()->setText(m_engine->account()->profile().fingerprint);
        m_status->setText(QStringLiteral("指纹已复制到剪贴板"));
    });
    auto *fpRow = new QHBoxLayout;
    fpRow->addWidget(m_fp, 1);
    fpRow->addWidget(copy);
    form->addRow(QStringLiteral("账户指纹："), fpRow);
    m_ip = new QLabel(this);
    form->addRow(QStringLiteral("当前 LAN 地址："), m_ip); // UC-01 step 7
    root->addLayout(form);

    auto *btnRow = new QHBoxLayout;
    auto *save = new QPushButton(QStringLiteral("保存"), this);
    connect(save, &QPushButton::clicked, this, &ProfilePage::save);
    auto *destroy = new QPushButton(QStringLiteral("销毁账户…"), this);
    destroy->setStyleSheet(QStringLiteral("color:#e53935;"));
    connect(destroy, &QPushButton::clicked, this, &ProfilePage::destroyAccount); // UC-02
    btnRow->addWidget(save);
    btnRow->addStretch();
    btnRow->addWidget(destroy);
    root->addLayout(btnRow);

    m_status = new QLabel(this);
    m_status->setStyleSheet(QStringLiteral("color:#43a047;"));
    m_status->setWordWrap(true);
    root->addWidget(m_status);
    root->addStretch();

    load();
}

void ProfilePage::load() {
    const auto p = m_engine->account()->profile();
    m_nick->setText(p.nickname);
    m_fp->setText(p.fingerprint);
    m_ip->setText(QStringLiteral("%1 : %2").arg(m_engine->tcpEndpointIp())
                      .arg(m_engine->settings()->tcpPort()));
    QPixmap pm(Paths::avatarDir() + "/current.png");
    if (!pm.isNull())
        m_avatar->setPixmap(pm.scaled(96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void ProfilePage::save() {
    QString err;
    // UC-03: nickname rules + circular avatar re-render + new blob hash
    if (!m_engine->account()->updateProfile(m_nick->text(), m_pendingAvatar, &err)) {
        m_status->setStyleSheet(QStringLiteral("color:#e53935;"));
        m_status->setText(err); // validation error surfaced inline
        return;
    }
    m_pendingAvatar.clear();
    m_status->setStyleSheet(QStringLiteral("color:#43a047;"));
    m_status->setText(QStringLiteral("已保存，将随心跳广播"));
    load();
}

void ProfilePage::destroyAccount() {
    auto *d = new DDialog(this);
    d->setTitle(QStringLiteral("销毁本地账户"));
    d->setMessage(QStringLiteral("将安全擦除私钥并删除账户记录，此操作不可恢复。\n销毁后需要重新启动应用以创建新账户。"));
    auto *wipe = new QCheckBox(QStringLiteral("同时清除所有聊天记录、文件与下载缓存"), d);
    d->addContent(wipe);
    d->addButton(QStringLiteral("取消"));
    d->addButton(QStringLiteral("确认销毁"), true, DDialog::ButtonWarning);
    connect(d, &DDialog::buttonClicked, this, [this, d, wipe](int index, const QString &) {
        if (index != 1)
            return;
        QString err;
        if (!m_engine->destroyAccount(wipe->isChecked(), &err)) {
            d->setMessage(QStringLiteral("销毁失败：%1").arg(err));
            return;
        }
        emit accountDestroyed();
    });
    d->open();
}

} // namespace meshlink
