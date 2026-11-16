// deepin-MeshLink — settings page: network/transfer tunables + about (NFRs).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#pragma once

#include "core/lansegment.h"

#include <QVector>
#include <QWidget>

class QSpinBox;
class QLabel;
class QComboBox;

namespace meshlink {

class Engine;

class SettingsView : public QWidget {
    Q_OBJECT
public:
    SettingsView(Engine *engine, QWidget *parent = nullptr);

private:
    void load();
    void apply();
    void reloadSegments();

    Engine *m_engine;
    QSpinBox *m_udp = nullptr, *m_tcp = nullptr, *m_hb = nullptr, *m_offline = nullptr;
    QSpinBox *m_chunk = nullptr, *m_threads = nullptr, *m_history = nullptr;
    QComboBox *m_segments = nullptr;
    QVector<LanSegment> m_segmentList;
    QLabel *m_status = nullptr;
};

} // namespace meshlink
