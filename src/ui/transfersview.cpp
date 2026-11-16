// deepin-MeshLink — transfers page implementation.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "transfersview.h"

#include "core/downloader.h"
#include "core/engine.h"
#include "core/settings.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace meshlink {

TransfersView::TransfersView(Engine *engine, QWidget *parent) : QWidget(parent), m_engine(engine) {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->addWidget(new QLabel(QStringLiteral("<b>下载任务</b>（分片断点续传；校验通过后才按原名交付 — UC-11/UC-12）"), this));

    m_table = new QTableWidget(0, 6, this);
    m_table->setHorizontalHeaderLabels({QStringLiteral("文件"), QStringLiteral("进度"), QStringLiteral("速率"),
                                        QStringLiteral("状态"), QStringLiteral("供给"), QStringLiteral("操作")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    root->addWidget(m_table, 1);

    connect(m_engine->downloader(), &Downloader::taskChanged, this, &TransfersView::refresh);
    connect(m_engine->downloader(), &Downloader::taskCompleted, this, [this](const QString &, const QString &) { refresh(); });
    connect(m_engine->downloader(), &Downloader::taskFailed, this, [this](const QString &, const QString &) { refresh(); });
    auto *t = new QTimer(this);
    connect(t, &QTimer::timeout, this, &TransfersView::refresh);
    t->start(1000); // UC-10 step 7: ~1s progress/speed refresh
    refresh();
}

void TransfersView::refresh() {
    const auto tasks = m_engine->downloader()->tasks();
    m_table->setRowCount(tasks.size());
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (int i = 0; i < tasks.size(); ++i) {
        const auto &tk = tasks[i];
        m_table->setItem(i, 0, new QTableWidgetItem(tk.name));
        const int pct = tk.totalChunks ? int(tk.doneChunks * 100 / tk.totalChunks) : 0;
        auto *bar = new QProgressBar(m_table);
        bar->setRange(0, 100);
        bar->setValue(pct);
        m_table->setCellWidget(i, 1, bar);

        const qint64 bytes = qint64(tk.doneChunks) * qint64(tk.chunkSize);
        QString speed;
        if (m_lastMs.contains(tk.hash) && now > m_lastMs[tk.hash]) {
            const double kb = (bytes - m_lastBytes[tk.hash]) * 10.0 / 1024.0; // ~1s window
            if (kb >= 1024)
                speed = QStringLiteral("%1 MB/s").arg(kb / 1024.0, 0, 'f', 1);
            else
                speed = QStringLiteral("%1 KB/s").arg(kb, 0, 'f', 0);
        }
        if (tk.status == TaskStatus::Downloading || tk.status == TaskStatus::Verifying) {
            m_lastBytes[tk.hash] = bytes;
            m_lastMs[tk.hash] = now;
        }
        m_table->setItem(i, 2, new QTableWidgetItem(speed));

        const QString st = tk.status == TaskStatus::Queued        ? QStringLiteral("排队中")
                           : tk.status == TaskStatus::Downloading ? QStringLiteral("下载中（多线程）")
                           : tk.status == TaskStatus::Paused      ? QStringLiteral("已暂停")
                           : tk.status == TaskStatus::Verifying   ? QStringLiteral("正在整体校验")
                           : tk.status == TaskStatus::Done        ? QStringLiteral("已完成 ✓")
                           : tk.status == TaskStatus::WaitingSupply ? QStringLiteral("等待供给")
                           : QStringLiteral("失败");
        auto *stItem = new QTableWidgetItem(tk.error.isEmpty() ? st : QStringLiteral("%1：%2").arg(st, tk.error));
        if (tk.status == TaskStatus::Failed)
            stItem->setForeground(QColor("#e53935"));
        m_table->setItem(i, 3, stItem);

        // UC-10 step 3/7: show where the bytes are coming from (or the delivered state).
        const QString supply = !tk.currentSource.isEmpty()
                                   ? QStringLiteral("来源：%1").arg(tk.currentSource)
                                   : (tk.destPath.isEmpty() ? QString() : QStringLiteral("已交付"));
        m_table->setItem(i, 4, new QTableWidgetItem(supply));

        auto *ops = new QPushButton(QStringLiteral("⋮"), m_table);
        ops->setFlat(true);
        connect(ops, &QPushButton::clicked, this, [this, tk, ops] {
            QMenu menu(ops);
            if (tk.status == TaskStatus::Downloading || tk.status == TaskStatus::Queued)
                menu.addAction(QStringLiteral("暂停"), [this, tk] { m_engine->downloader()->pause(tk.hash); });
            if (tk.status == TaskStatus::Paused || tk.status == TaskStatus::WaitingSupply)
                menu.addAction(QStringLiteral("继续（从已得分片续传）"), [this, tk] { m_engine->downloader()->resume(tk.hash, {}); });
            if (tk.status == TaskStatus::Done || !tk.destPath.isEmpty())
                menu.addAction(QStringLiteral("打开所在目录"), [tk] { QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(tk.destPath).absolutePath())); });
            menu.addSeparator();
            menu.addAction(QStringLiteral("取消并删除分片"), [this, tk] { m_engine->downloader()->cancel(tk.hash, true); });
            menu.exec(ops->mapToGlobal(QPoint(0, ops->height())));
        });
        m_table->setCellWidget(i, 5, ops);
    }
}

} // namespace meshlink
