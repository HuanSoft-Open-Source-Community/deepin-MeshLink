// deepin-MeshLink — profile page (UC-01 display, UC-02 destroy, UC-03 edit).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#pragma once

#include <QWidget>

#include <dtkwidget_global.h>

class QLabel;

DWIDGET_BEGIN_NAMESPACE
class DComboBox;
class DLineEdit;
DWIDGET_END_NAMESPACE

namespace meshlink {

class Engine;

class ProfilePage : public QWidget {
    Q_OBJECT
public:
    explicit ProfilePage(Engine *engine, QWidget *parent = nullptr);

signals:
    void accountDestroyed();

private:
    void load();
    void save();
    void destroyAccount();
    void chooseAvatar();
    void randomAvatar();
    void refreshAccounts();
    void onAccountComboChanged(int index);

    Engine *m_engine;
    QLabel *m_avatar = nullptr;
    DTK_WIDGET_NAMESPACE::DLineEdit *m_nick = nullptr;
    DTK_WIDGET_NAMESPACE::DLineEdit *m_fp = nullptr;
    DTK_WIDGET_NAMESPACE::DLineEdit *m_ip = nullptr;
    DTK_WIDGET_NAMESPACE::DComboBox *m_accountCombo = nullptr;
    QString m_pendingAvatar; // chosen file, applied on save
};

} // namespace meshlink
