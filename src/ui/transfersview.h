// deepin-MeshLink — transfers page: download tasks, progress & speed (UC-10/11/12).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#pragma once

#include <QHash>
#include <QWidget>

class QTableWidget;

namespace meshlink {

class Engine;

class TransfersView : public QWidget {
    Q_OBJECT
public:
    TransfersView(Engine *engine, QWidget *parent = nullptr);

private:
    void refresh();

    Engine *m_engine;
    QTableWidget *m_table = nullptr;
    QHash<QString, qint64> m_lastBytes; // speed estimation window
    QHash<QString, qint64> m_lastMs;
};

} // namespace meshlink
