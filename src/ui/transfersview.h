// deepin-MeshLink — transfers page: download tasks, progress & speed (UC-10/11/12).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#pragma once

#include <QHash>
#include <QWidget>

#include <dtkwidget_global.h>

class QLabel;
class QModelIndex;
class QStackedWidget;
class QStandardItemModel;
class QTableView;

DWIDGET_BEGIN_NAMESPACE
class DLabel;
DWIDGET_END_NAMESPACE

namespace meshlink {

class Engine;

class TransfersView : public QWidget {
    Q_OBJECT
public:
    explicit TransfersView(Engine *engine, QWidget *parent = nullptr);

private slots:
    void refresh();

private:
    void showTaskMenu(const QModelIndex &index);

    Engine *m_engine;
    QTableView *m_table = nullptr;
    QStandardItemModel *m_model = nullptr;
    QStackedWidget *m_stack = nullptr;
    QWidget *m_emptyPage = nullptr;
    DTK_WIDGET_NAMESPACE::DLabel *m_countLabel = nullptr;
    QHash<QString, qint64> m_lastBytes; // speed estimation window
    QHash<QString, qint64> m_lastMs;
};

} // namespace meshlink
