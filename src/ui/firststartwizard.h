// deepin-MeshLink — first-start / recovery account wizard (UC-01).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#pragma once

#include <DDialog>

class QLineEdit;
class QLabel;

namespace meshlink {

class Engine;

class FirstStartWizard : public DTK_WIDGET_NAMESPACE::DDialog {
    Q_OBJECT
public:
    explicit FirstStartWizard(Engine *engine, QWidget *parent = nullptr);

    // Modal helper: returns true when an account is available afterwards.
    static bool ensureAccount(Engine *engine, QWidget *parent);
    // UC-01 A1 recovery: existing keystore unreadable -> explicit rebuild (A2).
    static bool runAsRecovery(Engine *engine, QWidget *parent);

private:
    void createAccount();
    Engine *m_engine;
    QLineEdit *m_nick = nullptr;
    QLabel *m_error = nullptr;
};

} // namespace meshlink
