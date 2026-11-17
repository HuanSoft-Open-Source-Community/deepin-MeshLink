// deepin-MeshLink — DTK settings dialog (DSettingsDialog + meshlink::Settings).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#pragma once

#include "core/lansegment.h"

#include <DSettingsDialog>

#include <QPointer>
#include <QString>
#include <QVariant>

class QComboBox;

namespace meshlink {

class Engine;

// Settings are edited live through DTK's JSON-driven DSettingsDialog while the
// existing QSettings-backed meshlink::Settings stays the single source of truth.
class SettingsDialog : public DTK_WIDGET_NAMESPACE::DSettingsDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(Engine *engine, QWidget *parent = nullptr);
    ~SettingsDialog() override;

    bool isValid() const { return m_settings != nullptr; }

    // Re-enumerate LAN interfaces before every open (network can change at runtime).
    void refreshLanSegments();

    // True when UDP/TLS ports changed since the last call; clears the baseline.
    bool takeRestartRequired();

private slots:
    void onSettingChanged(const QString &key, const QVariant &value);

private:
    class Backend;

    void applyValue(const QString &key, const QVariant &value);
    void revertValue(const QString &key, const QVariant &value);
    QVariant currentValue(const QString &key) const;
    bool envPinned(const QString &key, QString *variable) const;
    LanSegment segmentFromKey(const QString &key) const;

    Engine *m_engine;
    DTK_CORE_NAMESPACE::DSettings *m_settings = nullptr;
    Backend *m_backend = nullptr;
    QPointer<QComboBox> m_segmentCombo;
    bool m_applying = false;
    int m_initialUdpPort = 0;
    int m_initialTcpPort = 0;
};

} // namespace meshlink
