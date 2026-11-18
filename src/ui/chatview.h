// deepin-MeshLink — chat panel: message stream + composer (UC-08, UC-09 UI).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#pragma once

#include "core/chatmanager.h"
#include "core/common.h"

#include <QSet>
#include <QWidget>

#include <dtkwidget_global.h>

class QLabel;
class QMenu;
class QScrollArea;
class QStackedWidget;
class QVBoxLayout;

DWIDGET_BEGIN_NAMESPACE
class DFrame;
class DLabel;
class DTextEdit;
class DSuggestButton;
class DIconButton;
class DToolButton;
DWIDGET_END_NAMESPACE

namespace meshlink {

class Engine;

class ChatView : public QWidget {
    Q_OBJECT
public:
    explicit ChatView(Engine *engine, QWidget *parent = nullptr);

    void setRoom(const QString &roomId);
    void appendMessage(const Msg &m); // live
    void refreshMuteState();
    // Inserts "@nickname " at the composer caret and gives it focus (used by
    // the avatar member menu and by the management dialog's Mention action).
    void mentionMember(const QString &nickname);

signals:
    void downloadsStarted();                 // MainWindow: jump to transfers page
    void manageRequested(const QString &roomId);

private:
    bool eventFilter(QObject *o, QEvent *e) override;
    QWidget *makeEmptyPage();
    QWidget *makeConversationPage();
    QWidget *messageRow(const Msg &m);
    void updateHeader();
    void clearHistory();
    void renderHistory();
    void appendMessageRow(const Msg &m);
    void appendNotice(const QString &text, bool warning = false);
    void updateComposerPlaceholder();
    void beginReply(const Msg &message);
    void clearReply();
    bool canRecall(const Msg &message) const;
    void showMessageMenu(const Msg &message, const QPoint &globalPos);
    void showMemberMenu(const QString &targetFp, const QPoint &globalPos);
    // @-mention composer support: a member picker next to the input plus a
    // live completion popup while typing "@".
    void showMentionPopup(const QString &filter);
    void insertMention(const QString &nickname);
    void updateDndButton();
    // Plain text -> rich text with @-mentions painted in the palette colour.
    QString mentionRichText(const QString &text) const;
    QString quotePreview(const Msg &message) const;
    void scrollToBottom();
    void sendText();
    void pickImage();
    void pickFile(); // UC-09: announce only, never push bytes (BR-19)
    void askDownload(const QString &hash, const QString &name, qint64 size,
                     const QString &mime); // UC-10

    Engine *m_engine;
    QString m_roomId;
    QSet<QString> m_seenIds;
    QSet<QString> m_avatarRequests;
    QStackedWidget *m_stack = nullptr;
    QWidget *m_emptyPage = nullptr;
    QWidget *m_conversation = nullptr;
    DTK_WIDGET_NAMESPACE::DLabel *m_title = nullptr;
    DTK_WIDGET_NAMESPACE::DLabel *m_subtitle = nullptr;
    DTK_WIDGET_NAMESPACE::DToolButton *m_manageButton = nullptr;
    DTK_WIDGET_NAMESPACE::DToolButton *m_dndButton = nullptr;
    DTK_WIDGET_NAMESPACE::DFrame *m_muteBanner = nullptr;
    DTK_WIDGET_NAMESPACE::DLabel *m_muteText = nullptr;
    QScrollArea *m_scroll = nullptr;
    QWidget *m_history = nullptr;
    QVBoxLayout *m_historyLayout = nullptr;
    DTK_WIDGET_NAMESPACE::DFrame *m_replyBar = nullptr;
    DTK_WIDGET_NAMESPACE::DLabel *m_replyLabel = nullptr;
    QString m_replyTo;
    DTK_WIDGET_NAMESPACE::DTextEdit *m_input = nullptr;
    DTK_WIDGET_NAMESPACE::DSuggestButton *m_sendBtn = nullptr;
    DTK_WIDGET_NAMESPACE::DToolButton *m_imageBtn = nullptr;
    DTK_WIDGET_NAMESPACE::DToolButton *m_fileBtn = nullptr;
    DTK_WIDGET_NAMESPACE::DToolButton *m_mentionBtn = nullptr;
    QMenu *m_mentionMenu = nullptr;
};

} // namespace meshlink
