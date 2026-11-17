// deepin-MeshLink — small DTK-native UI helpers shared by the pages.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "uiutils.h"

#include "core/account.h"
#include "core/chatmanager.h"

#include <DDialog>
#include <DFileDialog>
#include <DFontSizeManager>
#include <DIconTheme>
#include <DLabel>
#include <DPalette>
#include <DPaletteHelper>

#include <QAbstractItemView>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QTableView>

DWIDGET_USE_NAMESPACE
DGUI_USE_NAMESPACE

namespace meshlink::UiUtils {

QIcon icon(const QString &name, const QString &fallbackName) {
    QIcon result = DIconTheme::findQIcon(name);
    if (result.isNull() && !fallbackName.isEmpty())
        result = DIconTheme::findQIcon(fallbackName);
    return result;
}

void setupTable(QTableView *view) {
    if (!view)
        return;
    view->setFrameShape(QFrame::NoFrame);
    view->setShowGrid(false);
    view->setWordWrap(false);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setSelectionMode(QAbstractItemView::SingleSelection);
    view->setAlternatingRowColors(false);
    view->setCornerButtonEnabled(false);
    view->setTextElideMode(Qt::ElideRight);
    view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);

    if (auto *header = view->horizontalHeader()) {
        header->setHighlightSections(false);
        header->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        header->setStretchLastSection(false);
        header->setSectionsMovable(false);
    }
    if (auto *header = view->verticalHeader()) {
        header->setVisible(false);
        header->setDefaultSectionSize(40);
        header->setMinimumSectionSize(32);
    }
}

QWidget *pageHeader(const QString &title, QWidget *action, QWidget *parent) {
    auto *header = new QWidget(parent);
    auto *layout = new QHBoxLayout(header);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    auto *label = new DLabel(title, header);
    label->setForegroundRole(DPalette::TextTitle);
    DFontSizeManager::instance()->bind(label, DFontSizeManager::T4, QFont::DemiBold);
    layout->addWidget(label);
    layout->addStretch(1);
    if (action)
        layout->addWidget(action);
    return header;
}

QString shortFp(const QString &fp, int chars) {
    if (fp.isEmpty())
        return QStringLiteral("—");
    if (chars <= 0 || fp.size() <= chars)
        return fp;
    return fp.left(chars) + QChar(0x2026);
}

QString joinStatusText(JoinStatus status) {
    switch (status) {
    case JoinStatus::Accepted:
        return QCoreApplication::translate("UiUtils", "Joined");
    case JoinStatus::PendingApproval:
        return QCoreApplication::translate("UiUtils",
                                           "Request submitted; waiting for the host to approve.");
    case JoinStatus::Denied:
        return QCoreApplication::translate("UiUtils", "The host rejected the request.");
    case JoinStatus::RoomFull:
        return QCoreApplication::translate("UiUtils", "The room is full.");
    case JoinStatus::Blacklisted:
        return QCoreApplication::translate("UiUtils", "You are blacklisted in this room.");
    case JoinStatus::RoomGone:
        return QCoreApplication::translate(
            "UiUtils", "The room is unavailable (host offline or room dissolved).");
    }
    return QCoreApplication::translate("UiUtils", "Unknown join result.");
}

QString formattedTime(qint64 msEpoch) {
    if (msEpoch <= 0)
        return QStringLiteral("—");
    return QLocale::system().toString(QDateTime::fromMSecsSinceEpoch(msEpoch), QLocale::ShortFormat);
}

QString formattedSize(qint64 bytes) {
    if (bytes < 0)
        return QStringLiteral("—");
    return QLocale::system().formattedDataSize(bytes);
}

QPixmap avatarFor(const QString &fingerprint, const QString &avatarHash,
                  const QString &avatarPath, int size, QWidget *paletteSource) {
    QPixmap source;
    if (!avatarHash.isEmpty())
        source.load(ChatManager::blobPath(avatarHash));
    if (source.isNull() && !avatarPath.isEmpty())
        source.load(avatarPath);
    if (source.isNull() && !fingerprint.isEmpty())
        source = AccountManager::makeIdenticon(fingerprint, qMax(64, size * 2));
    return circularAvatar(source, fingerprint.left(1), size, paletteSource);
}

QPixmap circularAvatar(const QPixmap &source, const QString &fallbackText, int size,
                       QWidget *paletteSource) {
    if (size <= 0)
        return {};
    const qreal dpr = paletteSource ? qMax<qreal>(1.0, paletteSource->devicePixelRatioF()) : 1.0;
    QPixmap out(qRound(size * dpr), qRound(size * dpr));
    out.setDevicePixelRatio(dpr);
    out.fill(Qt::transparent);

    DPalette pal;
    if (paletteSource)
        pal = DPaletteHelper::instance()->palette(paletteSource);

    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    QPainterPath clip;
    clip.addEllipse(QRectF(0, 0, size, size));
    painter.setClipPath(clip);

    if (!source.isNull()) {
        const int device = qRound(size * dpr);
        const QPixmap scaled =
            source.scaled(device, device, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        const QRect src((scaled.width() - device) / 2, (scaled.height() - device) / 2, device, device);
        painter.drawPixmap(QRectF(0, 0, size, size), scaled, QRectF(src));
    } else {
        painter.fillPath(clip, pal.color(DPalette::ItemBackground));

        QFont font = paletteSource ? paletteSource->font() : QFont();
        font.setPixelSize(qMax(10, size / 2));
        font.setWeight(QFont::DemiBold);
        painter.setFont(font);
        painter.setPen(pal.color(DPalette::TextTips));
        painter.drawText(QRectF(0, 0, size, size), Qt::AlignCenter,
                         fallbackText.trimmed().left(1).toUpper());
    }

    painter.setClipping(false);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(pal.color(DPalette::FrameBorder), 1));
    painter.drawEllipse(QRectF(0.5, 0.5, size - 1.0, size - 1.0));
    painter.end();
    return out;
}

void info(QWidget *parent, const QString &title, const QString &text) {
    DDialog dialog(parent);
    dialog.setTitle(title);
    dialog.setMessage(text);
    dialog.setIcon(icon(QStringLiteral("dialog-information")));
    dialog.addButton(QCoreApplication::translate("UiUtils", "OK"), true, DDialog::ButtonNormal);
    dialog.exec();
}

void warning(QWidget *parent, const QString &title, const QString &text) {
    DDialog dialog(parent);
    dialog.setTitle(title);
    dialog.setMessage(text);
    dialog.setIcon(icon(QStringLiteral("dialog-warning")));
    dialog.addButton(QCoreApplication::translate("UiUtils", "OK"), true, DDialog::ButtonNormal);
    dialog.exec();
}

bool confirm(QWidget *parent, const QString &title, const QString &text,
             const QString &confirmText, bool danger) {
    DDialog dialog(parent);
    dialog.setTitle(title);
    dialog.setMessage(text);
    dialog.setIcon(icon(danger ? QStringLiteral("dialog-warning")
                               : QStringLiteral("dialog-information")));
    dialog.addButton(QCoreApplication::translate("UiUtils", "Cancel"));
    dialog.addButton(confirmText, true,
                     danger ? DDialog::ButtonWarning : DDialog::ButtonRecommend);
    return dialog.exec() == 1;
}

QString openFile(QWidget *parent, const QString &title, const QString &filter,
                 const QString &directory) {
    DFileDialog dialog(parent, title, directory.isEmpty() ? QDir::homePath() : directory, filter);
    dialog.setFileMode(QFileDialog::ExistingFile);
    dialog.setAcceptMode(QFileDialog::AcceptOpen);
    if (dialog.exec() != QDialog::Accepted)
        return {};
    return dialog.selectedFiles().value(0);
}

} // namespace meshlink::UiUtils
