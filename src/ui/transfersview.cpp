// deepin-MeshLink — transfers page implementation.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "transfersview.h"

#include "core/downloader.h"
#include "core/engine.h"
#include "core/settings.h"
#include "uiutils.h"

#include <DFontSizeManager>
#include <DLabel>
#include <DPalette>
#include <DPaletteHelper>

#include <QApplication>
#include <QCursor>
#include <QDateTime>
#include <QDesktopServices>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMenu>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QStyle>
#include <QStyleOptionProgressBar>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

DWIDGET_USE_NAMESPACE
DGUI_USE_NAMESPACE

namespace meshlink {
namespace {

// Draw the progress column through the active style (chameleon), so it follows
// the system palette without per-row QProgressBar widgets.
class ProgressColumnDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        if (index.column() != 1) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        QStyleOptionProgressBar bar;
        bar.rect = option.rect.adjusted(6, 10, -6, -10);
        bar.minimum = 0;
        bar.maximum = 100;
        bar.progress = index.data(Qt::UserRole).toInt();
        bar.text = QStringLiteral("%1%").arg(bar.progress);
        bar.textVisible = true;
        bar.textAlignment = Qt::AlignCenter;
        bar.state = QStyle::State_Enabled;
        bar.direction = option.direction;
        bar.palette = option.palette;
        bar.fontMetrics = option.fontMetrics;
        QApplication::style()->drawControl(QStyle::CE_ProgressBar, &bar, painter);
    }
};

} // namespace

TransfersView::TransfersView(Engine *engine, QWidget *parent)
    : QWidget(parent), m_engine(engine) {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(24, 20, 24, 20);
    root->setSpacing(16);

    m_countLabel = new DLabel(tr("Tasks: 0"), this);
    m_countLabel->setForegroundRole(DPalette::TextTips);
    root->addWidget(UiUtils::pageHeader(tr("Transfers"), m_countLabel, this));

    m_stack = new QStackedWidget(this);

    m_emptyPage = new QWidget(this);
    auto *emptyLayout = new QVBoxLayout(m_emptyPage);
    emptyLayout->setContentsMargins(24, 24, 24, 24);
    emptyLayout->setSpacing(12);
    emptyLayout->setAlignment(Qt::AlignCenter);
    const QIcon emptyIcon = UiUtils::icon(QStringLiteral("folder-download"),
                                          QStringLiteral("download"));
    auto *icon = new QLabel(m_emptyPage);
    icon->setAlignment(Qt::AlignCenter);
    if (!emptyIcon.isNull())
        icon->setPixmap(emptyIcon.pixmap(64, 64));
    else
        icon->hide();
    auto *emptyTitle = new DLabel(tr("No download tasks"), m_emptyPage);
    emptyTitle->setAlignment(Qt::AlignCenter);
    emptyTitle->setForegroundRole(DPalette::TextTitle);
    DFontSizeManager::instance()->bind(emptyTitle, DFontSizeManager::T4, QFont::DemiBold);
    auto *emptyHint = new DLabel(
        tr("Files announced in a room can be downloaded from the message stream."), m_emptyPage);
    emptyHint->setAlignment(Qt::AlignCenter);
    emptyHint->setWordWrap(true);
    emptyHint->setForegroundRole(DPalette::TextTips);
    emptyLayout->addStretch(1);
    emptyLayout->addWidget(icon, 0, Qt::AlignHCenter);
    emptyLayout->addWidget(emptyTitle);
    emptyLayout->addWidget(emptyHint);
    emptyLayout->addStretch(1);

    m_table = new QTableView(this);
    m_table->setObjectName(QStringLiteral("transfersView.table"));
    UiUtils::setupTable(m_table);
    m_model = new QStandardItemModel(0, 6, this);
    m_table->setModel(m_model);
    m_table->setItemDelegateForColumn(1, new ProgressColumnDelegate(m_table));
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    m_table->horizontalHeader()->resizeSection(1, 170);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    m_table->horizontalHeader()->resizeSection(2, 100);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Fixed);
    m_table->horizontalHeader()->resizeSection(5, 48);
    connect(m_table, &QTableView::clicked, this, [this](const QModelIndex &index) {
        if (index.column() == 5)
            showTaskMenu(index);
    });

    m_stack->addWidget(m_table);
    m_stack->addWidget(m_emptyPage);
    root->addWidget(m_stack, 1);

    connect(m_engine->downloader(), &Downloader::taskChanged, this, &TransfersView::refresh);
    connect(m_engine->downloader(), &Downloader::taskCompleted, this,
            [this](const QString &, const QString &) { refresh(); });
    connect(m_engine->downloader(), &Downloader::taskFailed, this,
            [this](const QString &, const QString &) { refresh(); });
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &TransfersView::refresh);
    timer->start(1000); // UC-10 step 7: ~1s progress/speed refresh
    refresh();
}

void TransfersView::refresh() {
    const QList<DownloadTaskView> tasks = m_engine->downloader()->tasks();
    m_countLabel->setText(tr("Tasks: %1").arg(tasks.size()));
    if (tasks.isEmpty()) {
        m_stack->setCurrentWidget(m_emptyPage);
        return;
    }
    m_stack->setCurrentWidget(m_table);

    const int scroll = m_table->verticalScrollBar()->value();
    m_model->clear();
    m_model->setHorizontalHeaderLabels(
        {tr("File"), tr("Progress"), tr("Speed"), tr("Status"), tr("Source"), QString()});
    m_model->setRowCount(tasks.size());

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (int row = 0; row < tasks.size(); ++row) {
        const DownloadTaskView &task = tasks.at(row);

        auto *file = new QStandardItem(task.name);
        file->setData(task.hash, Qt::UserRole);
        if (!task.destPath.isEmpty())
            file->setToolTip(task.destPath);
        m_model->setItem(row, 0, file);

        const int percent = task.totalChunks ? int(task.doneChunks * 100 / task.totalChunks) : 0;
        auto *progress = new QStandardItem;
        progress->setData(percent, Qt::UserRole);
        m_model->setItem(row, 1, progress);

        const qint64 bytes = qint64(task.doneChunks) * qint64(task.chunkSize);
        QString speed;
        if (m_lastMs.contains(task.hash) && now > m_lastMs.value(task.hash)) {
            const qint64 delta = bytes - m_lastBytes.value(task.hash);
            const double seconds = qMax<qint64>(1, now - m_lastMs.value(task.hash)) / 1000.0;
            const double kb = delta / 1024.0 / seconds;
            if (kb >= 1024.0)
                speed = tr("%1 MB/s").arg(kb / 1024.0, 0, 'f', 1);
            else
                speed = tr("%1 KB/s").arg(kb, 0, 'f', 0);
        }
        if (task.status == TaskStatus::Downloading || task.status == TaskStatus::Verifying) {
            m_lastBytes[task.hash] = bytes;
            m_lastMs[task.hash] = now;
        }
        m_model->setItem(row, 2, new QStandardItem(speed));

        const QString state =
            task.status == TaskStatus::Queued        ? tr("Queued")
            : task.status == TaskStatus::Downloading ? tr("Downloading")
            : task.status == TaskStatus::Paused      ? tr("Paused")
            : task.status == TaskStatus::Verifying   ? tr("Verifying")
            : task.status == TaskStatus::Done        ? tr("Done")
            : task.status == TaskStatus::WaitingSupply ? tr("Waiting for supply")
                                                       : tr("Failed");
        auto *status = new QStandardItem(task.error.isEmpty()
                                             ? state
                                             : tr("%1: %2").arg(state, task.error));
        if (task.status == TaskStatus::Failed) {
            DPalette pal = DPaletteHelper::instance()->palette(m_table);
            status->setForeground(pal.color(DPalette::TextWarning));
        }
        if (!task.error.isEmpty())
            status->setToolTip(task.error);
        m_model->setItem(row, 3, status);

        const QString source = !task.currentSource.isEmpty()
                                   ? tr("Source: %1").arg(task.currentSource)
                                   : (task.destPath.isEmpty() ? QString() : tr("Delivered"));
        m_model->setItem(row, 4, new QStandardItem(source));

        auto *actions = new QStandardItem(UiUtils::icon(QStringLiteral("view-more")), QString());
        actions->setToolTip(tr("Actions"));
        m_model->setItem(row, 5, actions);
    }
    m_table->verticalScrollBar()->setValue(scroll);
}

void TransfersView::showTaskMenu(const QModelIndex &index) {
    if (!index.isValid())
        return;
    QStandardItem *file = m_model->item(index.row(), 0);
    if (!file)
        return;
    const QString hash = file->data(Qt::UserRole).toString();
    const DownloadTaskView task = m_engine->downloader()->task(hash);

    QMenu menu(this);
    if (task.status == TaskStatus::Downloading || task.status == TaskStatus::Queued)
        menu.addAction(tr("Pause"), [this, hash] { m_engine->downloader()->pause(hash); });
    if (task.status == TaskStatus::Paused || task.status == TaskStatus::WaitingSupply)
        menu.addAction(tr("Resume from downloaded chunks"),
                       [this, hash] { m_engine->downloader()->resume(hash, {}); });
    if (task.status == TaskStatus::Done || !task.destPath.isEmpty())
        menu.addAction(tr("Open containing folder"), [task] {
            QDesktopServices::openUrl(
                QUrl::fromLocalFile(QFileInfo(task.destPath).absolutePath()));
        });
    menu.addSeparator();
    menu.addAction(tr("Cancel and delete chunks"),
                   [this, hash] { m_engine->downloader()->cancel(hash, true); });
    menu.exec(QCursor::pos());
}

} // namespace meshlink
