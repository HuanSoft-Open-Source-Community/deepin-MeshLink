// deepin-MeshLink — first-start wizard implementation (UC-01).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "firststartwizard.h"

#include "core/engine.h"
#include "core/lansegment.h"
#include "core/settings.h"
#include "uiutils.h"

#include <DComboBox>
#include <DFontSizeManager>
#include <DLabel>
#include <DLineEdit>
#include <DPushButton>
#include <DTipLabel>

#include <QApplication>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>

DWIDGET_USE_NAMESPACE
DGUI_USE_NAMESPACE

namespace meshlink {

FirstStartWizard::FirstStartWizard(Engine *engine, QWidget *parent)
    : DDialog(parent), m_engine(engine) {
    setTitle(tr("Welcome to deepin-MeshLink"));
    setWindowTitle(tr("deepin-MeshLink — Set up account"));
    setIcon(qApp->windowIcon());
    setMessage(tr("A local identity (Ed25519 key pair + fingerprint) will be generated on this "
                  "machine. Nothing is uploaded to a server."));

    auto *content = new QWidget(this);
    auto *form = new QFormLayout(content);
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(12);

    m_nick = new DLineEdit(content);
    m_nick->setPlaceholderText(tr("Leave empty to auto-generate (2–32 characters)"));
    m_nick->lineEdit()->setMaxLength(32);

    auto *segmentRow = new QWidget(content);
    auto *segmentLayout = new QHBoxLayout(segmentRow);
    segmentLayout->setContentsMargins(0, 0, 0, 0);
    segmentLayout->setSpacing(8);
    m_segments = new DComboBox(segmentRow);
    auto *refresh = new DPushButton(tr("Refresh"), segmentRow);
    segmentLayout->addWidget(m_segments, 1);
    segmentLayout->addWidget(refresh);

    m_error = new DTipLabel(QString(), content);
    m_error->setWordWrap(true);
    m_error->setForegroundRole(DPalette::TextWarning);
    m_error->hide();

    form->addRow(tr("Nickname"), m_nick);
    form->addRow(tr("LAN segment"), segmentRow);
    form->addRow(m_error);
    content->setMinimumWidth(460);
    addContent(content);

    addButton(tr("Create account"), true, DDialog::ButtonRecommend);
    addButton(tr("Quit"));

    connect(refresh, &DPushButton::clicked, this, &FirstStartWizard::reloadSegments);
    connect(this, &DDialog::buttonClicked, this, [this](int index, const QString &) {
        if (index == 0)
            createAccount();
        else
            reject();
    });
    reloadSegments();
}

void FirstStartWizard::createAccount() {
    // Persist the LAN segment first: account creation snapshots the LAN address
    // (UC-01 step 7) and Engine::start() will bring discovery up on this segment.
    m_engine->settings()->setLanSegment(selectedSegment());
    m_nick->setAlert(false);
    m_error->hide();
    QString error;
    // UC-01 steps: keygen -> fingerprint -> encrypted keystore -> profile.
    // Add-mode keeps other accounts; fresh mode replaces the (broken) identity.
    const bool ok = m_addMode ? m_engine->createAdditionalAccount(m_nick->text().trimmed(), &error) >= 0
                              : m_engine->createFreshAccount(m_nick->text().trimmed(), &error);
    if (!ok) {
        const QString text = error.isEmpty() ? tr("Key generation failed. Please try again.") : error;
        m_nick->setAlert(true);
        m_error->setText(text); // E1
        m_error->show();
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
        const LanSegment &segment = m_segmentList.at(i);
        const QString label =
            forced ? QStringLiteral("%1/%2").arg(segment.ip).arg(segment.prefixLength)
                   : QStringLiteral("%1 · %2/%3")
                         .arg(segment.interfaceName, segment.ip)
                         .arg(segment.prefixLength);
        m_segments->addItem(label);
        m_segments->setItemData(i, segment.label(), Qt::ToolTipRole);
        if (segment.ip == current.ip && segment.prefixLength == current.prefixLength)
            select = i;
    }
    if (select < 0 && !m_segmentList.isEmpty())
        select = 0;
    if (select >= 0)
        m_segments->setCurrentIndex(select);
    // MESHLINK_IP pins the segment for tests/headless runs; the UI must not
    // pretend a different choice would be applied.
    m_segments->setEnabled(!forced);
    m_segments->setToolTip(
        forced ? tr("Pinned by the MESHLINK_IP environment variable")
               : tr("Heartbeats are broadcast into and accepted only from this segment; it can be "
                    "changed later in Settings"));
    if (m_segmentList.isEmpty()) {
        m_error->setText(tr("No usable IPv4 interface was found; the loopback address will be used."));
        m_error->show();
    } else {
        m_error->hide();
    }
}

LanSegment FirstStartWizard::selectedSegment() const {
    const int row = m_segments ? m_segments->currentIndex() : -1;
    if (row >= 0 && row < m_segmentList.size())
        return m_segmentList.at(row);
    return m_engine->settings()->lanSegment();
}

bool FirstStartWizard::ensureAccount(Engine *engine, QWidget *parent) {
    // Before Engine::start() the in-memory profile is not loaded yet, so ask the
    // engine whether an account row already exists in the local database.
    if (engine->hasLocalAccount())
        return true;
    FirstStartWizard wizard(engine, parent);
    return wizard.exec() == QDialog::Accepted;
}

bool FirstStartWizard::runAsRecovery(Engine *engine, QWidget *parent) {
    FirstStartWizard wizard(engine, parent);
    wizard.setMessage(
        tr("The local account could not be decrypted (device or credentials changed).\n"
           "Creating a new account will erase the old account's local data; this cannot be "
           "undone."));
    if (wizard.exec() != QDialog::Accepted)
        return false;
    QString error;
    if (!engine->start(&error)) { // retry bringing up networking with the fresh identity
        UiUtils::warning(parent, tr("Startup failed"), error);
        return false;
    }
    return true;
}

bool FirstStartWizard::createAdditionalAccount(Engine *engine, QWidget *parent) {
    FirstStartWizard wizard(engine, parent);
    wizard.m_addMode = true;
    wizard.setTitle(tr("Add account"));
    wizard.setWindowTitle(tr("deepin-MeshLink — Add account"));
    wizard.setMessage(tr("Another local identity (Ed25519 key pair + fingerprint) will be "
                         "generated. Existing accounts stay available and can be switched to "
                         "any time."));
    wizard.setButtonText(1, tr("Cancel")); // "Quit" reads wrong when adding
    if (wizard.exec() != QDialog::Accepted)
        return false;
    return engine->account()->profile().valid;
}

} // namespace meshlink
