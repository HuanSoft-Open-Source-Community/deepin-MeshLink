// deepin-MeshLink — first-start / recovery account wizard (UC-01).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#pragma once

#include "core/lansegment.h"

#include <DDialog>

#include <QVector>

DWIDGET_BEGIN_NAMESPACE
class DComboBox;
class DLineEdit;
class DTipLabel;
DWIDGET_END_NAMESPACE

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
    // Multi-account: create an additional identity (never destroys accounts)
    // and switch the engine to it.
    static bool createAdditionalAccount(Engine *engine, QWidget *parent);

private:
    void createAccount();
    void reloadSegments();
    LanSegment selectedSegment() const;

    Engine *m_engine;
    bool m_addMode = false;
    // DDialog::exec() returns the clicked button index (Create account = 0,
    // Quit = 1), not QDialog::Accepted/Rejected. The static helpers therefore
    // rely on this flag, which is only set after a successful creation.
    bool m_accepted = false;
    DTK_WIDGET_NAMESPACE::DLineEdit *m_nick = nullptr;
    DTK_WIDGET_NAMESPACE::DComboBox *m_segments = nullptr;
    DTK_WIDGET_NAMESPACE::DTipLabel *m_error = nullptr;
    QVector<LanSegment> m_segmentList;
};

} // namespace meshlink
