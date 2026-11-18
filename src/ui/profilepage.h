// deepin-MeshLink — profile page (UC-01 display, UC-02 destroy, UC-03 edit).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#pragma once

#include <QWidget>

#include <dtkwidget_global.h>

class QLabel;

DWIDGET_BEGIN_NAMESPACE
class DComboBox;
class DLabel;
class DLineEdit;
DWIDGET_END_NAMESPACE

namespace meshlink {

class Engine;

class ProfilePage : public QWidget {
    Q_OBJECT
public:
    explicit ProfilePage(Engine *engine, QWidget *parent = nullptr);

    // Re-read the active identity: account combo + editable profile fields.
    void reload();

signals:
    // `remainingAccounts` is Engine::destroyAccount()'s return value; querying
    // the account table is impossible at this point because the engine closes
    // storage right after wiping the identity.
    void accountDestroyed(int remainingAccounts);

private:
    void load();
    void save();
    void destroyAccount();
    void chooseAvatar();
    void randomAvatar();
    void refreshAccounts();
    void onAccountComboChanged(int index);
    // A failed switch because the target keystore cannot be decrypted on this
    // device: offer to remove that dead identity without touching the active one.
    void offerDestroyUnlockable(qint64 accountId, const QString &nickname);

    Engine *m_engine;
    QLabel *m_avatar = nullptr;
    DTK_WIDGET_NAMESPACE::DLineEdit *m_nick = nullptr;
    DTK_WIDGET_NAMESPACE::DLabel *m_fp = nullptr;
    DTK_WIDGET_NAMESPACE::DLineEdit *m_ip = nullptr;
    DTK_WIDGET_NAMESPACE::DComboBox *m_accountCombo = nullptr;
    QString m_pendingAvatar; // chosen file, applied on save
};

} // namespace meshlink
