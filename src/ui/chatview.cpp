// deepin-MeshLink — chat panel implementation.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "chatview.h"

#include "core/account.h"
#include "core/downloader.h"
#include "core/engine.h"
#include "core/filemanager.h"
#include "core/notifypolicy.h"
#include "core/roommanager.h"
#include "uiutils.h"

#include <DDialog>
#include <DFontSizeManager>
#include <DFrame>
#include <DGuiApplicationHelper>
#include <DIconButton>
#include <DLabel>
#include <DListView>
#include <DPaletteHelper>
#include <DPushButton>
#include <DStandardItem>
#include <DSuggestButton>
#include <DTextEdit>
#include <DToolButton>

#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QTextCursor>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>

#include <memory>

DWIDGET_USE_NAMESPACE
DGUI_USE_NAMESPACE

namespace meshlink {
namespace {

// A palette-aware chat bubble: incoming messages use ItemBackground, own
// messages use a translucent Highlight. The text stays on the default roles.
class MessageBubble : public QWidget {
public:
    explicit MessageBubble(bool mine, QWidget *parent = nullptr)
        : QWidget(parent), m_mine(mine) {
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        DPalette palette = DPaletteHelper::instance()->palette(this);
        QColor background = palette.color(DPalette::ItemBackground);
        if (m_mine) {
            background = palette.color(DPalette::Highlight);
            background.setAlphaF(0.18);
        }
        painter.setPen(QPen(palette.color(DPalette::FrameBorder), 1));
        painter.setBrush(background);
        painter.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 8, 8);
    }

private:
    bool m_mine;
};

QString statusText(quint8 status) {
    switch (status) {
    case static_cast<quint8>(MsgStatus::Pending):
        return QCoreApplication::translate("ChatView", "Pending");
    case static_cast<quint8>(MsgStatus::Sent):
        return QCoreApplication::translate("ChatView", "Sent");
    case static_cast<quint8>(MsgStatus::Delivered):
        return QCoreApplication::translate("ChatView", "Delivered");
    case static_cast<quint8>(MsgStatus::Failed):
        return QCoreApplication::translate("ChatView", "Not delivered");
    default:
        return {};
    }
}

} // namespace

ChatView::ChatView(Engine *engine, QWidget *parent) : QWidget(parent), m_engine(engine) {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 20);
    root->setSpacing(12);

    m_stack = new QStackedWidget(this);
    m_emptyPage = makeEmptyPage();
    m_conversation = makeConversationPage();
    m_stack->addWidget(m_emptyPage);
    m_stack->addWidget(m_conversation);
    root->addWidget(m_stack, 1);

    auto *chat = m_engine->chat();
    connect(chat, &ChatManager::messageReceived, this,
            [this](const QString &roomId, const Msg &message) {
                if (roomId == m_roomId)
                    appendMessage(message);
            });
    connect(chat, &ChatManager::messageStatusChanged, this,
            [this](const QString &roomId, const QString &, quint8) {
                if (roomId == m_roomId)
                    renderHistory(); // status glyphs re-painted from DB
            });
    connect(chat, &ChatManager::blobReady, this, [this](const QString &) {
        if (!m_roomId.isEmpty())
            renderHistory(); // images/avatars arrive asynchronously (BR-15)
    });
    connect(chat, &ChatManager::notice, this,
            [this](const QString &roomId, const QString &text) {
                if (roomId == m_roomId)
                    appendNotice(text);
            });
    connect(chat, &ChatManager::messageRecalled, this,
            [this](const QString &roomId, const QString &, const QString &) {
                if (roomId == m_roomId)
                    renderHistory();
            });
    connect(chat, &ChatManager::recallFailed, this,
            [this](const QString &roomId, const QString &, const QString &error) {
                if (roomId == m_roomId && !error.isEmpty())
                    appendNotice(error, true);
            });
    connect(m_engine->rooms(), &RoomManager::membersChanged, this,
            [this](const QString &roomId) {
                if (roomId == m_roomId) {
                    updateHeader();
                    refreshMuteState();
                }
            });
    connect(m_engine->rooms(), &RoomManager::roomDndChanged, this,
            [this](const QString &roomId, bool) {
                if (roomId == m_roomId)
                    updateDndButton();
            });
    connect(m_engine->settings(), &Settings::changed, this,
            &ChatView::updateComposerPlaceholder);
    // Mentions are painted with palette colours: re-render on theme flips.
    connect(DGuiApplicationHelper::instance(), &DGuiApplicationHelper::paletteTypeChanged,
            this, [this](DGuiApplicationHelper::ColorType) {
                if (!m_roomId.isEmpty())
                    renderHistory();
            });

    setRoom(QString());
}

QWidget *ChatView::makeEmptyPage() {
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(12);
    layout->setAlignment(Qt::AlignCenter);

    const QIcon chatIcon = UiUtils::icon(QStringLiteral("chat"), QStringLiteral("message"));
    auto *icon = new QLabel(page);
    icon->setAlignment(Qt::AlignCenter);
    if (!chatIcon.isNull())
        icon->setPixmap(chatIcon.pixmap(64, 64));
    else
        icon->hide();

    auto *title = new DLabel(tr("Select a room"), page);
    title->setAlignment(Qt::AlignCenter);
    title->setForegroundRole(DPalette::TextTitle);
    DFontSizeManager::instance()->bind(title, DFontSizeManager::T4, QFont::DemiBold);

    auto *hint = new DLabel(tr("Choose a room on the left, or create a new one to start chatting."),
                            page);
    hint->setAlignment(Qt::AlignCenter);
    hint->setWordWrap(true);
    hint->setForegroundRole(DPalette::TextTips);

    layout->addStretch(1);
    layout->addWidget(icon, 0, Qt::AlignHCenter);
    layout->addWidget(title);
    layout->addWidget(hint);
    layout->addStretch(1);
    return page;
}

QWidget *ChatView::makeConversationPage() {
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    // ---- room header ----
    auto *header = new QWidget(page);
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(12);
    auto *titles = new QVBoxLayout;
    titles->setContentsMargins(0, 0, 0, 0);
    titles->setSpacing(2);
    m_title = new DLabel(header);
    m_title->setForegroundRole(DPalette::TextTitle);
    DFontSizeManager::instance()->bind(m_title, DFontSizeManager::T4, QFont::DemiBold);
    m_subtitle = new DLabel(header);
    m_subtitle->setForegroundRole(DPalette::TextTips);
    titles->addWidget(m_title);
    titles->addWidget(m_subtitle);
    headerLayout->addLayout(titles, 1);

    // Per-room do-not-disturb: mutes message notifications for this room
    // only; being @-mentioned still notifies (handled by NotificationCenter).
    m_dndButton = new DToolButton(header);
    m_dndButton->setObjectName(QStringLiteral("chatView.dndButton"));
    m_dndButton->setCheckable(true);
    m_dndButton->setIcon(UiUtils::icon(QStringLiteral("notifications-disabled"),
                                       QStringLiteral("notification-disabled")));
    m_dndButton->setIconSize(QSize(18, 18));
    m_dndButton->setFixedSize(36, 36);
    m_dndButton->setToolTip(tr("Mute notifications for this room"));
    connect(m_dndButton, &DToolButton::clicked, this, [this](bool checked) {
        if (m_roomId.isEmpty()) {
            updateDndButton();
            return;
        }
        m_engine->rooms()->setRoomDnd(m_roomId, checked);
    });
    headerLayout->addWidget(m_dndButton, 0, Qt::AlignVCenter);

    m_manageButton = new DToolButton(header);
    m_manageButton->setObjectName(QStringLiteral("chatView.manageButton"));
    m_manageButton->setText(tr("Manage"));
    m_manageButton->setIcon(UiUtils::icon(QStringLiteral("application-menu"),
                                          QStringLiteral("preferences-other")));
    m_manageButton->setIconSize(QSize(18, 18));
    m_manageButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_manageButton->setMinimumHeight(36);
    m_manageButton->setToolTip(tr("Manage room"));
    connect(m_manageButton, &DToolButton::clicked, this, [this] {
        if (!m_roomId.isEmpty())
            emit manageRequested(m_roomId);
    });
    headerLayout->addWidget(m_manageButton, 0, Qt::AlignVCenter);
    layout->addWidget(header);
    auto *headerLine = new DHorizontalLine(page);
    headerLine->setFrameShadow(QFrame::Plain);
    layout->addWidget(headerLine);

    // ---- mute banner ----
    m_muteBanner = new DFrame(page);
    m_muteBanner->setLineWidth(0);
    m_muteBanner->setBackgroundRole(QPalette::ToolTipBase);
    m_muteBanner->setAutoFillBackground(true);
    auto *muteLayout = new QHBoxLayout(m_muteBanner);
    muteLayout->setContentsMargins(10, 8, 10, 8);
    muteLayout->setSpacing(8);
    const QIcon warningIcon = UiUtils::icon(QStringLiteral("dialog-warning"));
    auto *muteIcon = new QLabel(m_muteBanner);
    if (!warningIcon.isNull())
        muteIcon->setPixmap(warningIcon.pixmap(16, 16));
    else
        muteIcon->hide();
    m_muteText = new DLabel(m_muteBanner);
    m_muteText->setWordWrap(true);
    m_muteText->setForegroundRole(QPalette::ToolTipText);
    muteLayout->addWidget(muteIcon);
    muteLayout->addWidget(m_muteText, 1);
    m_muteBanner->hide();
    layout->addWidget(m_muteBanner);

    // ---- message stream ----
    m_scroll = new QScrollArea(page);
    m_scroll->setObjectName(QStringLiteral("chatView.scroll"));
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_history = new QWidget(m_scroll);
    m_history->setObjectName(QStringLiteral("chatView.history"));
    m_historyLayout = new QVBoxLayout(m_history);
    m_historyLayout->setContentsMargins(0, 0, 10, 0);
    m_historyLayout->setSpacing(10);
    m_historyLayout->addStretch(1);
    m_scroll->setWidget(m_history);
    layout->addWidget(m_scroll, 1);

    // ---- composer ----
    auto *composerLine = new DHorizontalLine(page);
    composerLine->setFrameShadow(QFrame::Plain);
    layout->addWidget(composerLine);

    auto *composer = new DFrame(page);
    composer->setLineWidth(0);
    composer->setBackgroundRole(DPalette::ItemBackground);
    auto *composerLayout = new QVBoxLayout(composer);
    composerLayout->setContentsMargins(10, 10, 10, 8);
    composerLayout->setSpacing(8);

    m_replyBar = new DFrame(composer);
    m_replyBar->setObjectName(QStringLiteral("chatView.replyBar"));
    m_replyBar->setLineWidth(0);
    m_replyBar->setBackgroundRole(DPalette::ItemBackground);
    auto *replyLayout = new QHBoxLayout(m_replyBar);
    replyLayout->setContentsMargins(8, 4, 4, 4);
    replyLayout->setSpacing(6);
    m_replyLabel = new DLabel(m_replyBar);
    m_replyLabel->setForegroundRole(DPalette::TextTips);
    m_replyLabel->setElideMode(Qt::ElideRight);
    DFontSizeManager::instance()->bind(m_replyLabel, DFontSizeManager::T8);
    auto *replyClose = new DIconButton(m_replyBar);
    replyClose->setObjectName(QStringLiteral("chatView.replyClose"));
    replyClose->setIcon(UiUtils::icon(QStringLiteral("window-close"),
                                      QStringLiteral("dialog-close")));
    replyClose->setIconSize(QSize(14, 14));
    replyClose->setFixedSize(22, 22);
    replyClose->setFlat(true);
    replyClose->setToolTip(tr("Cancel reply"));
    connect(replyClose, &DIconButton::clicked, this, &ChatView::clearReply);
    replyLayout->addWidget(m_replyLabel, 1);
    replyLayout->addWidget(replyClose, 0, Qt::AlignVCenter);
    m_replyBar->hide();
    composerLayout->addWidget(m_replyBar);

    m_input = new DTextEdit(composer);
    m_input->setObjectName(QStringLiteral("chatView.input"));
    m_input->setAcceptRichText(false);
    m_input->setFixedHeight(92);
    m_input->installEventFilter(this);
    updateComposerPlaceholder();
    composerLayout->addWidget(m_input);

    auto *buttons = new QHBoxLayout;
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->setSpacing(8);
    m_mentionBtn = new DToolButton(page);
    m_mentionBtn->setObjectName(QStringLiteral("chatView.mentionButton"));
    m_mentionBtn->setText(tr("@"));
    m_mentionBtn->setIcon(UiUtils::icon(QStringLiteral("user-available"),
                                        QStringLiteral("contact-available")));
    m_mentionBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_mentionBtn->setToolTip(tr("Mention a member (@)"));
    connect(m_mentionBtn, &DToolButton::clicked, this, [this] {
        m_input->setFocus();
        m_input->insertPlainText(QStringLiteral("@"));
        showMentionPopup(QString());
    });
    m_imageBtn = new DToolButton(page);
    m_imageBtn->setObjectName(QStringLiteral("chatView.imageButton"));
    m_imageBtn->setText(tr("Image"));
    m_imageBtn->setIcon(UiUtils::icon(QStringLiteral("image-x-generic")));
    m_imageBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    connect(m_imageBtn, &DToolButton::clicked, this, &ChatView::pickImage);
    m_fileBtn = new DToolButton(page);
    m_fileBtn->setObjectName(QStringLiteral("chatView.fileButton"));
    m_fileBtn->setText(tr("File"));
    m_fileBtn->setIcon(UiUtils::icon(QStringLiteral("document-open-folder"),
                                     QStringLiteral("mail-attachment")));
    m_fileBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    connect(m_fileBtn, &DToolButton::clicked, this, &ChatView::pickFile);
    m_sendBtn = new DSuggestButton(tr("Send"), page);
    m_sendBtn->setObjectName(QStringLiteral("chatView.sendButton"));
    connect(m_sendBtn, &DSuggestButton::clicked, this, &ChatView::sendText);
    buttons->addWidget(m_mentionBtn);
    buttons->addWidget(m_imageBtn);
    buttons->addWidget(m_fileBtn);
    buttons->addStretch(1);
    buttons->addWidget(m_sendBtn);
    composerLayout->addLayout(buttons);
    layout->addWidget(composer);

    // Live "@member" completion: opens while the caret sits right after an
    // unclosed mention token, closes as soon as the token is terminated.
    m_mentionMenu = new QMenu(this);
    m_mentionMenu->setObjectName(QStringLiteral("chatView.mentionMenu"));
    connect(m_mentionMenu, &QMenu::aboutToHide, this, [this] {
        if (m_mentionMenu->activeAction() == nullptr)
            m_mentionMenu->clear(); // keep entries only while choosing
    });
    connect(m_input, &QTextEdit::textChanged, this, [this] {
        const QString upToCursor =
            m_input->toPlainText().left(m_input->textCursor().position());
        const int at = upToCursor.lastIndexOf(QLatin1Char('@'));
        if (at < 0) {
            if (m_mentionMenu->isVisible())
                m_mentionMenu->close();
            return;
        }
        if (at > 0 && !NotifyPolicy::isBoundary(upToCursor.at(at - 1))) {
            if (m_mentionMenu->isVisible())
                m_mentionMenu->close();
            return;
        }
        const QString token = upToCursor.mid(at + 1);
        if (token.contains(QLatin1Char(' ')) || token.contains(QLatin1Char('\n'))) {
            if (m_mentionMenu->isVisible())
                m_mentionMenu->close();
            return;
        }
        showMentionPopup(token);
    });

    return page;
}

bool ChatView::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_input && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        const bool isEnter =
            key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter;
        if (isEnter) {
            const bool shift = key->modifiers().testFlag(Qt::ShiftModifier);
            const bool ctrl = key->modifiers().testFlag(Qt::ControlModifier);
            const bool send = m_engine->settings()->sendWithEnter() ? !shift : ctrl;
            if (send) {
                if (!key->isAutoRepeat())
                    sendText();
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ChatView::updateComposerPlaceholder() {
    if (!m_input)
        return;
    if (m_engine->settings()->sendWithEnter())
        m_input->setPlaceholderText(tr("Type a message. Enter to send; Shift+Enter for a new line."));
    else
        m_input->setPlaceholderText(tr("Type a message. Ctrl+Enter to send."));
}

void ChatView::setRoom(const QString &roomId) {
    m_roomId = roomId;
    m_avatarRequests.clear();
    clearReply();
    if (roomId.isEmpty()) {
        m_stack->setCurrentWidget(m_emptyPage);
        clearHistory();
        m_input->clear();
        return;
    }
    m_stack->setCurrentWidget(m_conversation);
    updateHeader();
    renderHistory();
    refreshMuteState();
    updateDndButton();
}

void ChatView::updateHeader() {
    if (m_roomId.isEmpty())
        return;
    const RoomLocal room = m_engine->rooms()->room(m_roomId);
    const int members = m_engine->rooms()->members(m_roomId).size();
    m_title->setText(room.name);

    QStringList meta;
    meta << tr("Members: %1").arg(members);
    if (room.hosted)
        meta << tr("You host this room");
    else
        meta << (room.hostIsOnline ? tr("Host online") : tr("Host offline"));
    if (room.policy == JoinPolicy::Approval)
        meta << tr("Needs approval");
    meta << tr("Invite code %1").arg(room.inviteCode);
    m_subtitle->setText(meta.join(QStringLiteral(" · ")));
    m_manageButton->setEnabled(room.joined);
}

void ChatView::refreshMuteState() {
    if (m_roomId.isEmpty())
        return;
    qint64 until = 0;
    const bool muted = m_engine->rooms()->isMuted(m_roomId, &until);
    m_muteBanner->setVisible(muted);
    if (muted) {
        m_muteText->setText(
            until == -1
                ? tr("You are muted until a moderator unmutes you.")
                : tr("You are muted for %1 more minutes.")
                      .arg(qMax<qint64>(1, (until - QDateTime::currentMSecsSinceEpoch()) / 60000)));
    }
    m_input->setEnabled(!muted);
    m_sendBtn->setEnabled(!muted);
    m_imageBtn->setEnabled(!muted);
    m_fileBtn->setEnabled(!muted);
    m_mentionBtn->setEnabled(!muted);
}

void ChatView::clearHistory() {
    m_seenIds.clear();
    if (!m_historyLayout)
        return;
    while (QLayoutItem *item = m_historyLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    m_historyLayout->addStretch(1);
}

void ChatView::renderHistory() {
    if (m_roomId.isEmpty())
        return;
    clearHistory();
    const QVector<Msg> messages = m_engine->chat()->history(m_roomId, 200); // NFR-15 bounded
    for (const Msg &message : messages)
        appendMessageRow(message);
    scrollToBottom();
}

void ChatView::appendMessage(const Msg &message) {
    if (m_roomId.isEmpty() || m_seenIds.contains(message.msgId))
        return;
    appendMessageRow(message);
    scrollToBottom();
}

void ChatView::appendMessageRow(const Msg &message) {
    if (m_seenIds.contains(message.msgId))
        return;
    m_seenIds.insert(message.msgId);
    QWidget *row = messageRow(message);
    m_historyLayout->insertWidget(m_historyLayout->count() - 1, row);
}

QWidget *ChatView::messageRow(const Msg &message) {
    const bool mine = message.senderFp == m_engine->account()->profile().fingerprint;
    auto *row = new QWidget(m_history);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(8);

    const Profile profile = m_engine->account()->profile();
    QString avatarHash = message.avatarHash;
    if (mine && avatarHash.isEmpty())
        avatarHash = profile.avatarHash;
    if (!mine && !avatarHash.isEmpty() && !ChatManager::hasBlob(avatarHash) &&
        !m_avatarRequests.contains(avatarHash)) {
        m_avatarRequests.insert(avatarHash);
        m_engine->chat()->requestBlob(avatarHash, message.senderFp);
    }
    auto *avatar = new QLabel(row);
    avatar->setObjectName(QStringLiteral("chatView.avatar"));
    avatar->setFixedSize(36, 36);
    avatar->setPixmap(UiUtils::avatarFor(message.senderFp, avatarHash,
                                         mine ? profile.avatarPath : QString(), 36, this));

    auto *content = new QWidget(row);
    content->setMaximumWidth(560);
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(4);

    auto *head = new QHBoxLayout;
    head->setContentsMargins(0, 0, 0, 0);
    head->setSpacing(8);
    if (mine)
        head->addStretch(1);

    auto *name = new DLabel(
        mine ? tr("%1 (you)").arg(message.senderNick) : message.senderNick, content);
    name->setForegroundRole(DPalette::TextTitle);
    DFontSizeManager::instance()->bind(name, DFontSizeManager::T6, QFont::DemiBold);
    head->addWidget(name);

    auto *time = new DLabel(UiUtils::formattedTime(message.ts), content);
    time->setForegroundRole(DPalette::TextTips);
    DFontSizeManager::instance()->bind(time, DFontSizeManager::T8);
    head->addWidget(time);

    if (mine && !message.recalled) {
        const QString status = statusText(message.status);
        if (!status.isEmpty()) {
            auto *statusLabel = new DLabel(status, content);
            statusLabel->setForegroundRole(message.status == static_cast<quint8>(MsgStatus::Failed)
                                               ? DPalette::TextWarning
                                               : DPalette::TextTips);
            DFontSizeManager::instance()->bind(statusLabel, DFontSizeManager::T8);
            head->addWidget(statusLabel);
        }
    }
    if (!mine)
        head->addStretch(1);
    contentLayout->addLayout(head);

    auto *bubble = new MessageBubble(mine, content);
    auto *body = new QVBoxLayout(bubble);
    body->setContentsMargins(12, 8, 12, 8);
    body->setSpacing(6);

    if (!message.replyTo.isEmpty()) {
        const Msg quoted = m_engine->chat()->message(m_roomId, message.replyTo);
        QString quoteText;
        if (quoted.msgId.isEmpty())
            quoteText = tr("Quoted message unavailable");
        else if (quoted.recalled)
            quoteText = tr("Quoted message recalled");
        else
            quoteText = tr("Reply to %1: %2").arg(quoted.senderNick, quotePreview(quoted));

        auto *quote = new DFrame(bubble);
        quote->setObjectName(QStringLiteral("chatView.quoteBlock"));
        quote->setLineWidth(1);
        quote->setFrameRounded(true);
        quote->setBackgroundRole(DPalette::ItemBackground);
        auto *quoteLayout = new QHBoxLayout(quote);
        quoteLayout->setContentsMargins(8, 4, 8, 4);
        auto *quoteLabel = new DLabel(quoteText, quote);
        quoteLabel->setForegroundRole(DPalette::TextTips);
        quoteLabel->setElideMode(Qt::ElideRight);
        quoteLabel->setToolTip(quoteText);
        DFontSizeManager::instance()->bind(quoteLabel, DFontSizeManager::T8);
        quoteLayout->addWidget(quoteLabel);
        body->addWidget(quote);
    }

    if (message.recalled) {
        auto *recalled = new DLabel(tr("Message recalled"), bubble);
        recalled->setObjectName(QStringLiteral("chatView.recalled"));
        recalled->setForegroundRole(DPalette::TextTips);
        body->addWidget(recalled);
    } else {
    switch (message.kind) {
    case static_cast<quint8>(MsgKind::Text): {
        auto *text = new DLabel(mentionRichText(message.text), bubble);
        text->setObjectName(QStringLiteral("chatView.messageText"));
        text->setWordWrap(true);
        text->setTextFormat(Qt::RichText);
        text->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
        body->addWidget(text);
        break;
    }
    case static_cast<quint8>(MsgKind::Image): {
        QPixmap image(ChatManager::blobPath(message.contentHash));
        if (image.isNull()) {
            // image bytes ride separately (BR-15): request from sender on demand
            m_engine->chat()->requestBlob(message.contentHash, message.senderFp);
            auto *loading = new DLabel(tr("Loading image…"), bubble);
            loading->setForegroundRole(DPalette::TextTips);
            body->addWidget(loading);
        } else {
            auto *label = new QLabel(bubble);
            label->setPixmap(image.scaledToWidth(qMin(320, image.width()), Qt::SmoothTransformation));
            body->addWidget(label);
        }
        break;
    }
    case static_cast<quint8>(MsgKind::File): {
        auto *line = new QHBoxLayout;
        line->setContentsMargins(0, 0, 0, 0);
        line->setSpacing(8);
        const QIcon fileIcon = UiUtils::icon(QStringLiteral("mail-attachment"));
        if (!fileIcon.isNull()) {
            auto *icon = new QLabel(bubble);
            icon->setPixmap(fileIcon.pixmap(24, 24));
            line->addWidget(icon);
        }
        auto *text = new DLabel(
            tr("%1\n%2").arg(message.fileName, UiUtils::formattedSize(message.fileSize)), bubble);
        text->setWordWrap(true);
        text->setTextInteractionFlags(Qt::TextSelectableByMouse);
        line->addWidget(text, 1);
        auto *download = new DPushButton(tr("Download"), bubble);
        connect(download, &DPushButton::clicked, this, [this, message] {
            askDownload(message.contentHash, message.fileName, message.fileSize, message.mime);
        });
        line->addWidget(download);
        body->addLayout(line);
        break;
    }
    default: {
        auto *text = new DLabel(message.text, bubble);
        text->setWordWrap(true);
        text->setTextFormat(Qt::PlainText);
        text->setForegroundRole(DPalette::TextTips);
        body->addWidget(text);
        break;
    }
    }
    }
    bubble->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(bubble, &QWidget::customContextMenuRequested, this,
            [this, message, bubble](const QPoint &pos) {
                showMessageMenu(message, bubble->mapToGlobal(pos));
            });
    // The whole row opens the same menu: the message actions live in the
    // context menu (there is no per-message toolbar button any more).
    row->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(row, &QWidget::customContextMenuRequested, this,
            [this, message, row](const QPoint &pos) {
                showMessageMenu(message, row->mapToGlobal(pos));
            });
    contentLayout->addWidget(bubble);

    if (mine) {
        rowLayout->addStretch(1);
        rowLayout->addWidget(content, 0, Qt::AlignTop);
        rowLayout->addWidget(avatar, 0, Qt::AlignTop);
    } else {
        rowLayout->addWidget(avatar, 0, Qt::AlignTop);
        rowLayout->addWidget(content, 0, Qt::AlignTop);
        rowLayout->addStretch(1);
    }
    return row;
}

void ChatView::appendNotice(const QString &text, bool warning) {
    auto *row = new QWidget(m_history);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 2, 0, 2);
    layout->setSpacing(0);
    auto *label = new DLabel(text, row);
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignCenter);
    label->setForegroundRole(warning ? DPalette::TextWarning : DPalette::TextTips);
    DFontSizeManager::instance()->bind(label, DFontSizeManager::T8);
    layout->addStretch(1);
    layout->addWidget(label, 2);
    layout->addStretch(1);
    m_historyLayout->insertWidget(m_historyLayout->count() - 1, row);
    scrollToBottom();
}

void ChatView::scrollToBottom() {
    QTimer::singleShot(0, this, [this] {
        if (m_scroll)
            m_scroll->verticalScrollBar()->setValue(m_scroll->verticalScrollBar()->maximum());
    });
}

void ChatView::sendText() {
    const QString text = m_input->toPlainText();
    if (text.trimmed().isEmpty() || m_roomId.isEmpty())
        return;
    QString error;
    const QString id = m_engine->chat()->sendText(m_roomId, text, &error, m_replyTo);
    if (id.isEmpty()) {
        if (!error.isEmpty())
            appendNotice(error, true);
        return;
    }
    m_input->clear();
    clearReply();
}

void ChatView::pickImage() {
    const QString file = UiUtils::openFile(
        this, tr("Choose an image"),
        tr("Images (*.png *.jpg *.jpeg *.bmp *.webp)"));
    if (file.isEmpty() || m_roomId.isEmpty())
        return;
    QString error;
    const QString id = m_engine->chat()->sendImage(m_roomId, file, &error, m_replyTo);
    if (id.isEmpty()) {
        if (!error.isEmpty())
            appendNotice(error, true);
        return;
    }
    clearReply();
}

void ChatView::pickFile() {
    const QString file = UiUtils::openFile(
        this, tr("Announce a file (contents are not sent)"), tr("All files (*)"));
    if (file.isEmpty() || m_roomId.isEmpty())
        return;
    const QString room = m_roomId;
    const QString reply = m_replyTo;
    QPointer<ChatView> guard(this);
    m_engine->files()->announceFile(
        room, file,
        [guard, room, reply](const QString &, const QString &error) {
            if (!guard || guard->m_roomId != room)
                return;
            if (!error.isEmpty()) {
                guard->appendNotice(error, true); // UC-09 E1: host offline etc.
                return;
            }
            if (guard->m_replyTo == reply)
                guard->clearReply();
        },
        reply);
}

void ChatView::beginReply(const Msg &message) {
    if (m_roomId.isEmpty() || message.msgId.isEmpty() || message.recalled)
        return;
    m_replyTo = message.msgId;
    const QString preview = quotePreview(message);
    m_replyLabel->setText(tr("Replying to %1: %2").arg(message.senderNick, preview));
    m_replyLabel->setToolTip(m_replyLabel->text());
    m_replyBar->show();
    m_input->setFocus();
}

void ChatView::clearReply() {
    m_replyTo.clear();
    if (m_replyBar)
        m_replyBar->hide();
    if (m_replyLabel) {
        m_replyLabel->clear();
        m_replyLabel->setToolTip(QString());
    }
}

QString ChatView::quotePreview(const Msg &message) const {
    if (message.recalled)
        return tr("Recalled message");
    switch (message.kind) {
    case static_cast<quint8>(MsgKind::Image):
        return tr("Image");
    case static_cast<quint8>(MsgKind::File):
        return message.fileName.isEmpty() ? tr("File") : message.fileName;
    default: {
        const QString text = message.text.simplified();
        return text.size() > 80 ? text.left(80) + QChar(0x2026) : text;
    }
    }
}

bool ChatView::canRecall(const Msg &message) const {
    if (m_roomId.isEmpty() || message.recalled || message.msgId.isEmpty())
        return false;
    const QString self = m_engine->account()->profile().fingerprint;
    const MemberRole role = m_engine->rooms()->myRole(m_roomId);
    const qint64 received = message.recvTs > 0 ? message.recvTs : message.ts;
    return canRecallMessage(role, self, message.senderFp, received, nowMs(),
                            selfRecallWindowMs());
}

QString ChatView::mentionRichText(const QString &text) const {
    QString escaped;
    escaped.reserve(text.size() + 16);
    for (const QChar c : text) {
        switch (c.unicode()) {
        case QLatin1Char('&').unicode(): escaped += QLatin1String("&amp;"); break;
        case QLatin1Char('<').unicode(): escaped += QLatin1String("&lt;"); break;
        case QLatin1Char('>').unicode(): escaped += QLatin1String("&gt;"); break;
        case QLatin1Char('"').unicode(): escaped += QLatin1String("&quot;"); break;
        default: escaped += c; break;
        }
    }
    if (m_roomId.isEmpty())
        return escaped;

    const QColor mention =
        DPaletteHelper::instance()->palette(this).color(DPalette::Highlight);
    const QString span = QStringLiteral(
        "<span style=\"color:%1;\">@%2</span>");
    QString html;
    qsizetype from = 0;
    while (from < text.size()) {
        const qsizetype at = text.indexOf(QLatin1Char('@'), from);
        if (at < 0)
            break;
        // find the token end (whitespace terminates it)
        qsizetype end = at + 1;
        while (end < text.size() && !text.at(end).isSpace())
            ++end;
        const QString token = text.mid(at + 1, end - at - 1);
        bool hit = false;
        if (!token.isEmpty()) {
            if (NotifyPolicy::textMentionsAll(QChar('@') + token)) {
                hit = true;
            } else {
                for (const auto &member : m_engine->rooms()->members(m_roomId)) {
                    if (member.nickname == token) {
                        hit = true;
                        break;
                    }
                }
            }
        }
        if (!hit) {
            from = at + 1;
            continue;
        }
        html += escaped.mid(from, at - from);
        html += span.arg(mention.name(), escaped.mid(at + 1, token.size()));
        from = at + 1 + token.size();
    }
    if (from < escaped.size())
        html += escaped.mid(from);
    return html;
}

void ChatView::showMentionPopup(const QString &filter) {
    if (!m_mentionMenu || m_roomId.isEmpty())
        return;
    m_mentionMenu->clear();
    const QString self = m_engine->account()->profile().fingerprint;
    const QVector<RoomMemberInfo> members = m_engine->rooms()->members(m_roomId);
    int shown = 0;
    for (const auto &member : members) {
        if (member.fp == self || member.nickname.isEmpty())
            continue;
        if (!filter.isEmpty() &&
            !member.nickname.contains(filter, Qt::CaseInsensitive))
            continue;
        QAction *action = m_mentionMenu->addAction(member.nickname);
        const QString nickname = member.nickname;
        connect(action, &QAction::triggered, this, [this, nickname] {
            insertMention(nickname);
        });
        if (++shown >= 12)
            break;
    }
    if (shown == 0) {
        if (m_mentionMenu->isVisible())
            m_mentionMenu->close();
        return;
    }
    m_mentionMenu->adjustSize();
    const QPoint anchor = m_mentionBtn->mapToGlobal(
        QPoint(0, -m_mentionMenu->height() - 4));
    m_mentionMenu->popup(anchor);
}

void ChatView::insertMention(const QString &nickname) {
    if (!m_input)
        return;
    const QString plain = m_input->toPlainText();
    const int cursor = m_input->textCursor().position();
    const QString upToCursor = plain.left(cursor);
    const int at = upToCursor.lastIndexOf(QLatin1Char('@'));
    if (at >= 0 && (at == 0 || NotifyPolicy::isBoundary(upToCursor.at(at - 1)))) {
        QTextCursor tc = m_input->textCursor();
        tc.setPosition(at);
        tc.setPosition(cursor, QTextCursor::KeepAnchor);
        tc.insertText(QChar('@') + nickname + QChar(' '));
    } else {
        m_input->insertPlainText(QChar('@') + nickname + QChar(' '));
    }
    m_input->setFocus();
    if (m_mentionMenu)
        m_mentionMenu->close();
}

void ChatView::updateDndButton() {
    if (!m_dndButton || m_roomId.isEmpty())
        return;
    const bool dnd = m_engine->rooms()->isRoomDnd(m_roomId);
    m_dndButton->setChecked(dnd);
    m_dndButton->setToolTip(dnd ? tr("Notifications muted for this room (mentions still notify)")
                                : tr("Mute notifications for this room"));
}

void ChatView::showMessageMenu(const Msg &message, const QPoint &globalPos) {
    if (message.msgId.isEmpty())
        return;
    QMenu menu(this);
    QAction *reply = nullptr;
    QAction *copy = nullptr;
    QAction *recall = nullptr;
    if (!message.recalled) {
        reply = menu.addAction(tr("Reply"));
        if (!message.text.isEmpty())
            copy = menu.addAction(tr("Copy text"));
        if (canRecall(message))
            recall = menu.addAction(tr("Recall message"));
    }
    if (menu.isEmpty())
        return;
    QAction *chosen = menu.exec(globalPos);
    if (chosen && chosen == reply) {
        beginReply(message);
    } else if (chosen && chosen == copy) {
        QGuiApplication::clipboard()->setText(message.text);
    } else if (chosen && chosen == recall) {
        const bool mine = message.senderFp == m_engine->account()->profile().fingerprint;
        if (!mine &&
            !UiUtils::confirm(this, tr("Recall message"),
                              tr("Recall this message for everyone? The content will be removed."),
                              tr("Recall"), true))
            return;
        QString error;
        if (!m_engine->chat()->recallMessage(m_roomId, message.msgId, &error) &&
            !error.isEmpty())
            appendNotice(error, true);
    }
}

// ---- UC-10: supplier dialog + start ----
void ChatView::askDownload(const QString &hash, const QString &name, qint64 size,
                           const QString &mime) {
    if (m_engine->files()->hasVerifiedFile(hash)) { // BR-17: identical content already local
        appendNotice(tr("A file with the same hash is already available locally."));
        return;
    }

    auto *dialog = new DDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setTitle(tr("Download %1").arg(name));
    dialog->setMessage(tr("Looking for supplier nodes…"));
    dialog->setIcon(UiUtils::icon(QStringLiteral("folder-download")));

    auto *content = new QWidget(dialog);
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(8);

    auto *status = new DLabel(tr("Looking for supplier nodes…"), content);
    status->setForegroundRole(DPalette::TextTips);
    status->setWordWrap(true);
    auto *list = new DListView(content);
    list->setObjectName(QStringLiteral("chatView.supplierList"));
    list->setFrameShape(QFrame::NoFrame);
    list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    list->setBackgroundType(DStyledItemDelegate::RoundedBackground);
    list->setItemMargins(QMargins(10, 6, 10, 6));
    list->setItemSpacing(3);
    list->setItemRadius(8);
    list->setMinimumHeight(120);
    auto *model = new QStandardItemModel(list);
    list->setModel(model);
    list->hide();
    contentLayout->addWidget(status);
    contentLayout->addWidget(list);
    content->setMinimumWidth(460);
    dialog->addContent(content);

    const int cancelIndex = dialog->addButton(tr("Cancel"));
    const int downloadIndex =
        dialog->addButton(tr("Start download"), true, DDialog::ButtonRecommend);
    if (auto *button = dialog->getButton(downloadIndex))
        button->setEnabled(false);

    auto suppliers = std::make_shared<QVector<SupplierInfo>>();
    QPointer<DDialog> guard(dialog);
    const QString roomId = m_roomId;

    m_engine->files()->querySuppliers(
        roomId, hash,
        [this, guard, list, model, status, suppliers, downloadIndex, cancelIndex](
            const QVector<SupplierInfo> &result) {
            if (!guard || !list || !model || !status)
                return;
            *suppliers = result;
            if (result.isEmpty()) { // UC-10 E1: no node holds it yet
                status->setText(tr("No supplier is online. The announcement stays available; retry "
                                   "when a holder comes online."));
                if (auto *button = guard->getButton(downloadIndex))
                    button->setEnabled(false);
                guard->setButtonText(cancelIndex, tr("Close"));
                return;
            }

            model->clear();
            int preferred = 0;
            for (int i = 0; i < result.size(); ++i) {
                const SupplierInfo &supplier = result.at(i);
                auto *item = new DStandardItem(
                    tr("%1 · %2:%3").arg(supplier.nickname, supplier.ip).arg(supplier.port));
                auto *state = new DViewItemAction(Qt::AlignLeft, QSize(0, 0), QSize(), false);
                state->setText(supplier.online ? tr("Online (default)") : tr("Offline"));
                state->setFontSize(DFontSizeManager::T8);
                state->setTextColorRole(DPalette::TextTips);
                item->setTextActionList({state});
                model->appendRow(item);
                if (supplier.online && preferred == 0)
                    preferred = i;
            }
            list->show();
            list->setCurrentIndex(model->index(preferred, 0));
            status->setText(tr("Choose a supplier node:"));
            if (auto *button = guard->getButton(downloadIndex))
                button->setEnabled(true);
        });

    connect(dialog, &DDialog::buttonClicked, this,
            [this, guard, list, model, suppliers, hash, name, size, mime, downloadIndex, roomId](
                int index, const QString &) {
                if (!guard || index != downloadIndex)
                    return;
                QVector<SupplierInfo> all = *suppliers;
                if (all.isEmpty())
                    return;
                const int row = list ? list->currentIndex().row() : -1;
                if (row > 0 && row < all.size())
                    std::swap(all[0], all[row]); // preferred supplier leads the fallback list
                m_engine->downloader()->startTask(roomId, hash, name, size, mime, all);
                guard->close();
                emit downloadsStarted();
            });

    dialog->open();
}

} // namespace meshlink
