// deepin-MeshLink — @-mention matching & desktop-notification policy (pure).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
//
// Header-only so the rules can be unit-tested without sockets or GUI, and so
// both the ChatManager/NotificationCenter (decision) and ChatView (rendering)
// share one definition of "what counts as a mention".
#pragma once

#include "protocol.h"

#include <QChar>
#include <QString>
#include <QVector>

#include <algorithm>

namespace meshlink {
namespace NotifyPolicy {

// A character that may surround an @-mention token without extending it.
inline bool isBoundary(QChar c) { return !c.isLetterOrNumber() && c != QChar('_'); }

// True when `text` contains "@nickname" with identifier boundaries on both
// sides, so "@张三" never matches the member "张三丰". Multi-word nicknames
// are matched as a whole ("@Zhang San"); matching is case-insensitive.
inline bool textMentions(const QString &text, const QString &nickname) {
    if (text.isEmpty() || nickname.isEmpty())
        return false;
    const QString needle = QChar('@') + nickname;
    qsizetype from = 0;
    while (true) {
        const qsizetype at = text.indexOf(needle, from, Qt::CaseInsensitive);
        if (at < 0)
            return false;
        const bool leftOk = at == 0 || isBoundary(text.at(at - 1));
        const qsizetype end = at + needle.size();
        const bool rightOk = end >= text.size() || isBoundary(text.at(end));
        if (leftOk && rightOk)
            return true;
        from = at + 1;
    }
}

// Mention-all tokens: notify (and highlight) everyone in the room.
inline bool textMentionsAll(const QString &text) {
    return textMentions(text, QStringLiteral("all")) ||
           textMentions(text, QStringLiteral("所有人")) ||
           textMentions(text, QStringLiteral("全体成员"));
}

// True when `text` @-mentions this client in the context of the room roster.
// The sender is excluded: another member's nickname only counts as a mention
// of me when *I* am that member (or a mention-all token was used).
inline bool textMentionsMe(const QString &text, const QVector<RoomMemberInfo> &members,
                           const QString &selfFp) {
    if (textMentionsAll(text))
        return true;
    for (const auto &member : members) {
        if (member.fp != selfFp || member.nickname.isEmpty())
            continue;
        if (textMentions(text, member.nickname))
            return true;
    }
    return false;
}

// ---- desktop-notification decision (XDG org.freedesktop.Notifications) ----

struct Context {
    bool globalEnabled = false; // Settings: notifications enabled
    bool windowActive = false;  // main window currently has focus
    bool viewingRoom = false;   // the room the event belongs to is on screen
    bool roomDnd = false;       // per-room do-not-disturb is on
    bool mentionsMe = false;    // an @-mention (or mention-all) was detected
    bool fromSelf = false;      // the event was caused by this node itself
};

enum class Action { Skip, Notify };

inline Action decide(const Context &c) {
    if (!c.globalEnabled || c.fromSelf)
        return Action::Skip;
    if (c.windowActive && c.viewingRoom)
        return Action::Skip; // visible in-app already
    if (c.roomDnd && !c.mentionsMe)
        return Action::Skip; // muted room, but @-mentions still notify
    return Action::Notify;
}

} // namespace NotifyPolicy
} // namespace meshlink
