// deepin-MeshLink — reusable room-member context menu (UC-06/UC-07 UI).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#pragma once

#include <QString>

#include <functional>

class QMenu;
class QWidget;

namespace meshlink {

class RoomManager;

namespace MemberMenu {

using MentionHandler = std::function<void(const QString &nickname)>;
using ChangedHandler = std::function<void()>;

// Builds the full avatar/member context menu for `targetFp` in `roomId`.
//
// The menu is populated and wired but never exec()'d here, so both the chat
// pane and the management dialog can popup() it themselves and tests can walk
// its actions without blocking. Availability of every moderation entry is
// derived from RoomManager::canManage() (plain members cannot moderate,
// administrators cannot touch the host/other administrators, nobody can target
// themselves); disabled entries carry the rejection reason as a tooltip.
//
// `onMention` inserts "@nickname " into the composer, `onChanged` refreshes
// local views after a successful admin operation.
QMenu *create(QWidget *parent, RoomManager *rooms, const QString &roomId,
              const QString &selfFp, const QString &targetFp, MentionHandler onMention,
              ChangedHandler onChanged = {});

} // namespace MemberMenu
} // namespace meshlink
