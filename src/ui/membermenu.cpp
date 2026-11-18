// deepin-MeshLink — reusable room-member context menu implementation.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "membermenu.h"

#include "core/roommanager.h"
#include "uiutils.h"

#include <QAction>
#include <QCoreApplication>
#include <QMenu>
#include <QPointer>
#include <QWidget>

namespace meshlink {
namespace MemberMenu {
namespace {

void explain(QAction *action, const QString &why) {
    action->setToolTip(why);
    action->setStatusTip(why);
}

} // namespace

QMenu *create(QWidget *parent, RoomManager *rooms, const QString &roomId, const QString &selfFp,
              const QString &targetFp, MentionHandler onMention, ChangedHandler onChanged) {
    if (!rooms || roomId.isEmpty() || targetFp.isEmpty())
        return nullptr;

    QMenu *menu = new QMenu(parent);
    menu->setObjectName(QStringLiteral("memberMenu"));

    const QVector<RoomMemberInfo> members = rooms->members(roomId);
    RoomMemberInfo target;
    bool found = false;
    for (const RoomMemberInfo &member : members) {
        if (member.fp == targetFp) {
            target = member;
            found = true;
            break;
        }
    }
    const MemberRole targetRole =
        found ? static_cast<MemberRole>(target.role) : rooms->roleOf(roomId, targetFp);
    const QString nickname =
        target.nickname.isEmpty() ? UiUtils::shortFp(targetFp, 8) : target.nickname;
    const bool muted = found && (target.mutedUntil == -1 || target.mutedUntil > nowMs());

    // Async admin results may arrive after the surface that owns the menu is
    // gone; the warning for a failed op is only shown while it still exists.
    QPointer<QWidget> parentGuard(parent);
    auto request = [rooms, roomId, targetFp, onChanged, parentGuard](AdminOp op, qint64 durationMs) {
        rooms->requestAdminOp(
            roomId, targetFp, op, durationMs,
            [parentGuard, onChanged](bool ok, const QString &why) {
                if (!ok) {
                    if (!parentGuard)
                        return;
                    const QString detail =
                        why.isEmpty()
                            ? QCoreApplication::translate("MemberMenu",
                                                          "The host rejected the action.")
                            : why;
                    UiUtils::warning(
                        parentGuard.data(),
                        QCoreApplication::translate("MemberMenu", "Action rejected"), detail);
                    return;
                }
                if (onChanged)
                    onChanged();
            });
    };

    // Mention stays available for every member, including yourself.
    QAction *mention = menu->addAction(
        QCoreApplication::translate("MemberMenu", "Mention (@%1)").arg(nickname));
    mention->setObjectName(QStringLiteral("memberMenu.mention"));
    if (onMention)
        QObject::connect(mention, &QAction::triggered, menu, [onMention, nickname] { onMention(nickname); });
    else
        mention->setEnabled(false);

    menu->addSeparator();

    // Set/revoke administrator is host-only; the host itself has no entry.
    if (targetRole != MemberRole::Host) {
        const bool revoke = targetRole == MemberRole::Admin;
        const AdminOp op = revoke ? AdminOp::UnsetAdmin : AdminOp::SetAdmin;
        QAction *roleAction = menu->addAction(
            revoke ? QCoreApplication::translate("MemberMenu", "Revoke admin")
                   : QCoreApplication::translate("MemberMenu", "Set as admin"));
        roleAction->setObjectName(revoke ? QStringLiteral("memberMenu.revokeAdmin")
                                         : QStringLiteral("memberMenu.setAdmin"));
        QString why;
        if (!rooms->canManage(roomId, selfFp, targetFp, op, &why)) {
            roleAction->setEnabled(false);
            explain(roleAction, why);
        }
        QObject::connect(roleAction, &QAction::triggered, menu, [request, op] { request(op, 0); });
    }

    if (muted) {
        QAction *unmute =
            menu->addAction(QCoreApplication::translate("MemberMenu", "Unmute"));
        unmute->setObjectName(QStringLiteral("memberMenu.unmute"));
        QString why;
        if (!rooms->canManage(roomId, selfFp, targetFp, AdminOp::Unmute, &why)) {
            unmute->setEnabled(false);
            explain(unmute, why);
        }
        QObject::connect(unmute, &QAction::triggered, menu,
                [request] { request(AdminOp::Unmute, 0); });
    } else {
        QMenu *muteMenu = menu->addMenu(QCoreApplication::translate("MemberMenu", "Mute"));
        muteMenu->menuAction()->setObjectName(QStringLiteral("memberMenu.mute"));
        QString why;
        const bool allowed = rooms->canManage(roomId, selfFp, targetFp, AdminOp::Mute, &why);
        if (!allowed) {
            muteMenu->menuAction()->setEnabled(false);
            explain(muteMenu->menuAction(), why);
        }
        auto addMute = [&](const QString &text, const QString &objectName, qint64 durationMs) {
            QAction *action = muteMenu->addAction(text);
            action->setObjectName(objectName);
            action->setEnabled(allowed);
            if (!allowed)
                explain(action, why);
            QObject::connect(action, &QAction::triggered, muteMenu,
                    [request, durationMs] { request(AdminOp::Mute, durationMs); });
        };
        addMute(QCoreApplication::translate("MemberMenu", "10 minutes"),
                QStringLiteral("memberMenu.mute10Minutes"), 10 * 60 * 1000);
        addMute(QCoreApplication::translate("MemberMenu", "1 hour"),
                QStringLiteral("memberMenu.mute1Hour"), 60 * 60 * 1000);
        addMute(QCoreApplication::translate("MemberMenu", "Until unmuted"),
                QStringLiteral("memberMenu.muteUntilUnmuted"), 0);
    }

    menu->addSeparator();
    QAction *remove =
        menu->addAction(QCoreApplication::translate("MemberMenu", "Remove from room"));
    remove->setObjectName(QStringLiteral("memberMenu.remove"));
    {
        QString why;
        if (!rooms->canManage(roomId, selfFp, targetFp, AdminOp::Kick, &why)) {
            remove->setEnabled(false);
            explain(remove, why);
        }
    }
    QObject::connect(remove, &QAction::triggered, menu, [request, parentGuard, nickname] {
        if (!parentGuard)
            return;
        const bool confirmed = UiUtils::confirm(
            parentGuard.data(),
            QCoreApplication::translate("MemberMenu", "Remove from room"),
            QCoreApplication::translate("MemberMenu", "Remove %1 from this room?").arg(nickname),
            QCoreApplication::translate("MemberMenu", "Remove"), true);
        if (confirmed)
            request(AdminOp::Kick, 0);
    });

    return menu;
}

} // namespace MemberMenu
} // namespace meshlink
