// deepin-MeshLink — profile page (UC-01 display, UC-02 destroy, UC-03 edit).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;

namespace meshlink {

class Engine;

class ProfilePage : public QWidget {
    Q_OBJECT
public:
    ProfilePage(Engine *engine, QWidget *parent = nullptr);

signals:
    void accountDestroyed();

private:
    void load();
    void save();
    void destroyAccount();

    Engine *m_engine;
    QLabel *m_avatar = nullptr;
    QLineEdit *m_nick = nullptr;
    QLabel *m_fp = nullptr;
    QLabel *m_ip = nullptr;
    QLabel *m_status = nullptr;
    QString m_pendingAvatar; // chosen file, applied on save
};

} // namespace meshlink
