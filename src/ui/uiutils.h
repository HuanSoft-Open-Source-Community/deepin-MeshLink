// deepin-MeshLink — small DTK-native UI helpers shared by the pages.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#pragma once

#include "core/common.h"

#include <QIcon>
#include <QPixmap>
#include <QString>
#include <QWidget>

class QTableView;
class QVariant;

namespace meshlink::UiUtils {

// Theme icon with an optional fallback theme name. Never invents colors.
QIcon icon(const QString &name, const QString &fallbackName = QString());

// DTK-flavoured QTableView defaults: no frame/grid, row selection, 40px rows.
void setupTable(QTableView *view);

// Page heading (T4/DemiBold) with an optional right-aligned action widget.
QWidget *pageHeader(const QString &title, QWidget *action, QWidget *parent);

// "abcdef12…" -- empty fingerprints render as an em dash.
QString shortFp(const QString &fp, int chars = 8);

// Local text for a join result; the remote `detail` is never used for i18n.
QString joinStatusText(JoinStatus status);

QString formattedTime(qint64 msEpoch);
QString formattedSize(qint64 bytes);

// Circular avatar from `source`, or a palette-aware initial placeholder.
QPixmap circularAvatar(const QPixmap &source, const QString &fallbackText, int size,
                       QWidget *paletteSource);
// Resolve an avatar from the blob cache, the local profile file, or a
// deterministic identicon derived from the fingerprint.
QPixmap avatarFor(const QString &fingerprint, const QString &avatarHash,
                  const QString &avatarPath, int size, QWidget *paletteSource = nullptr);

// DDialog replacements for QMessageBox; warning/confirm use ButtonWarning.
void info(QWidget *parent, const QString &title, const QString &text);
void warning(QWidget *parent, const QString &title, const QString &text);
bool confirm(QWidget *parent, const QString &title, const QString &text,
             const QString &confirmText, bool danger = false);

// DFileDialog replacement for the static QFileDialog helpers.
QString openFile(QWidget *parent, const QString &title, const QString &filter,
                 const QString &directory = QString());

} // namespace meshlink::UiUtils
