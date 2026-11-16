// deepin-MeshLink — chat panel implementation.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "chatview.h"

#include "core/account.h"
#include "core/downloader.h"
#include "core/engine.h"
#include "core/filemanager.h"
#include "core/roommanager.h"

#include <DDialog>

#include <QDateTime>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QVBoxLayout>

DWIDGET_USE_NAMESPACE

namespace meshlink {

static QPixmap circularAvatar(const QString &hash, int size = 32) {
    QPixmap base;
    if (!hash.isEmpty())
        base.load(ChatManager::blobPath(hash));
    if (base.isNull())
        base = QPixmap(size, size), base.fill(QColor(90, 100, 110));
    QPixmap out(size, size);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    QRectF r(0, 0, size, size);
    p.drawEllipse(r);
    p.setClipRegion(QRegion(r.toRect(), QRegion::Ellipse));
    p.drawPixmap(r.toRect(), base.scaled(size, size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    return out;
}

ChatView::ChatView(Engine *engine, QWidget *parent) : QWidget(parent), m_engine(engine) {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);

    m_header = new QLabel(QStringLiteral("选择或创建一个聊天室"), this);
    QFont hf = m_header->font();
    hf.setBold(true);
    hf.setPointSize(hf.pointSize() + 2);
    m_header->setFont(hf);
    root->addWidget(m_header);

    m_muteBanner = new QLabel(this);
    m_muteBanner->setStyleSheet(QStringLiteral("background:#fff3cd;color:#856404;padding:4px;border-radius:4px;"));
    m_muteBanner->setVisible(false);
    root->addWidget(m_muteBanner);

    m_list = new QListWidget(this);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    root->addWidget(m_list, 1);

    auto *composer = new QHBoxLayout;
    m_input = new QPlainTextEdit(this);
    m_input->setPlaceholderText(QStringLiteral("输入消息，Ctrl+Enter 发送（UC-08）"));
    m_input->setFixedHeight(64);
    composer->addWidget(m_input);
    auto *col = new QVBoxLayout;
    m_sendBtn = new QPushButton(QStringLiteral("发送"), this);
    auto *imgBtn = new QPushButton(QStringLiteral("图片"), this);
    auto *fileBtn = new QPushButton(QStringLiteral("通告文件…"), this); // UC-09: metadata only
    connect(m_sendBtn, &QPushButton::clicked, this, &ChatView::sendText);
    connect(imgBtn, &QPushButton::clicked, this, &ChatView::pickImage);
    connect(fileBtn, &QPushButton::clicked, this, &ChatView::pickFile);
    col->addWidget(m_sendBtn);
    col->addWidget(imgBtn);
    col->addWidget(fileBtn);
    composer->addLayout(col);
    root->addLayout(composer);

    m_input->installEventFilter(this);

    // live wiring
    connect(m_engine->chat(), &ChatManager::messageReceived, this, [this](const QString &roomId, const Msg &m) {
        if (roomId == m_roomId)
            appendMessage(m);
    });
    connect(m_engine->chat(), &ChatManager::messageStatusChanged, this, [this](const QString &roomId, const QString &, quint8) {
        if (roomId == m_roomId)
            renderHistory(); // status glyphs re-painted from DB
    });
    connect(m_engine->chat(), &ChatManager::blobReady, this, [this](const QString &) {
        renderHistory(); // images/avatars arrive asynchronously (BR-15)
    });
    connect(m_engine->chat(), &ChatManager::notice, this, [this](const QString &roomId, const QString &text) {
        if (roomId != m_roomId)
            return;
        auto *it = new QListWidgetItem(m_list);
        auto *l = new QLabel(text, m_list);
        l->setStyleSheet(QStringLiteral("color:gray;"));
        it->setSizeHint(l->sizeHint());
        m_list->setItemWidget(it, l);
        m_list->scrollToBottom();
    });
    connect(m_engine->rooms(), &RoomManager::membersChanged, this, [this](const QString &roomId) {
        if (roomId == m_roomId) {
            const auto ms = m_engine->rooms()->members(roomId);
            const RoomLocal r = m_engine->rooms()->room(roomId);
            m_header->setText(QStringLiteral("%1 · %2 人 · %3%4 · 邀请码 %5")
                                  .arg(r.name)
                                  .arg(ms.size())
                                  .arg(r.hosted ? QStringLiteral("我托管") : QStringLiteral("托管者在线"))
                                  .arg(r.policy == JoinPolicy::Approval ? QStringLiteral(" · 需批准") : QString())
                                  .arg(r.inviteCode));
            refreshMuteState();
        }
    });
}

bool ChatView::eventFilter(QObject *o, QEvent *e) {
    if (o == m_input && e->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(e);
        if (ke->matches(QKeySequence::Paste) == false && ke->key() == Qt::Key_Return &&
            (ke->modifiers() & Qt::ControlModifier)) {
            sendText();
            return true;
        }
    }
    return QWidget::eventFilter(o, e);
}

void ChatView::setRoom(const QString &roomId) {
    m_roomId = roomId;
    if (roomId.isEmpty()) {
        m_header->setText(QStringLiteral("选择或创建一个聊天室"));
        m_list->clear();
        m_input->setEnabled(false);
        m_sendBtn->setEnabled(false);
        return;
    }
    m_input->setEnabled(true);
    m_sendBtn->setEnabled(true);
    const RoomLocal r = m_engine->rooms()->room(roomId);
    m_header->setText(QStringLiteral("%1 · %2%3 · 邀请码 %4")
                          .arg(r.name,
                               r.hosted ? QStringLiteral("（我托管）") : QStringLiteral("（成员）"),
                               r.policy == JoinPolicy::Approval ? QStringLiteral(" · 需批准加入") : QString())
                          .arg(r.inviteCode));
    renderHistory();
    refreshMuteState();
}

void ChatView::refreshMuteState() {
    qint64 until = 0;
    const bool muted = m_roomId.isEmpty() ? false : m_engine->rooms()->isMuted(m_roomId, &until);
    m_muteBanner->setVisible(muted);
    if (muted) {
        m_muteBanner->setText(until == -1 ? QStringLiteral("你已被禁言（直至手动解除）")
                                          : QStringLiteral("你已被禁言，剩余 %1 分钟")
                                                .arg(qMax(qint64(1), (until - QDateTime::currentMSecsSinceEpoch()) / 60000)));
    }
    m_input->setEnabled(!muted);
    m_sendBtn->setEnabled(!muted);
}

void ChatView::renderHistory() {
    if (m_roomId.isEmpty())
        return;
    m_list->clear();
    const auto msgs = m_engine->chat()->history(m_roomId, 200); // NFR-15 bounded
    for (const auto &m : msgs) {
        auto *it = new QListWidgetItem(m_list);
        auto *w = messageWidget(m);
        it->setSizeHint(w->sizeHint());
        m_list->setItemWidget(it, w);
    }
    m_list->scrollToBottom();
}

void ChatView::appendMessage(const Msg &m) {
    for (int i = 0; i < m_list->count(); ++i) {
        if (m_list->item(i)->data(Qt::UserRole).toString() == m.msgId)
            return; // dedupe snapshot vs live
    }
    auto *it = new QListWidgetItem(m_list);
    it->setData(Qt::UserRole, m.msgId);
    auto *w = messageWidget(m);
    it->setSizeHint(w->sizeHint());
    m_list->setItemWidget(it, w);
    m_list->scrollToBottom();
}

QWidget *ChatView::messageWidget(const Msg &m) {
    auto *row = new QWidget(m_list);
    auto *lay = new QHBoxLayout(row);
    lay->setContentsMargins(4, 2, 4, 2);

    auto *av = new QLabel(row);
    av->setPixmap(circularAvatar(m.avatarHash));
    av->setFixedSize(32, 32);
    lay->addWidget(av, 0, Qt::AlignTop);

    auto *col = new QVBoxLayout;
    const QString mine = m.senderFp == m_engine->account()->profile().fingerprint ? QStringLiteral("（我）") : QString();
    const QString statusTxt = [&] {
        if (mine.isEmpty())
            return QString();
        switch (m.status) {
        case static_cast<quint8>(MsgStatus::Pending): return QStringLiteral(" · 待送达");
        case static_cast<quint8>(MsgStatus::Sent): return QStringLiteral(" · 已发送");
        case static_cast<quint8>(MsgStatus::Delivered): return QStringLiteral(" · 已送达 ✓✓");
        case static_cast<quint8>(MsgStatus::Failed): return QStringLiteral(" · 未送达（红色提示）");
        }
        return QString();
    }();
    const QString color = m.status == static_cast<quint8>(MsgStatus::Failed) ? QStringLiteral("#e53935") : QStringLiteral("black");
    auto *head = new QLabel(QStringLiteral("<span style='color:%3'><b>%1</b> %2%4</span>")
                                .arg(m.senderNick.toHtmlEscaped(),
                                     QDateTime::fromMSecsSinceEpoch(m.ts).toString(QStringLiteral("MM-dd HH:mm")),
                                     color, statusTxt.toHtmlEscaped()),
                            row);
    head->setTextFormat(Qt::RichText);
    col->addWidget(head);

    switch (m.kind) {
    case static_cast<quint8>(MsgKind::Text): {
        auto *body = new QLabel(m.text, row);
        body->setWordWrap(true);
        body->setTextInteractionFlags(Qt::TextSelectableByMouse);
        col->addWidget(body);
        break;
    }
    case static_cast<quint8>(MsgKind::Image): {
        QPixmap pm(ChatManager::blobPath(m.contentHash));
        if (pm.isNull()) {
            // image bytes ride separately (BR-15): request from sender on demand
            m_engine->chat()->requestBlob(m.contentHash, m.senderFp);
            auto *l = new QLabel(QStringLiteral("图片加载中…"), row);
            col->addWidget(l);
        } else {
            auto *img = new QLabel(row);
            img->setPixmap(pm.scaledToWidth(qMin(320, pm.width()), Qt::SmoothTransformation));
            col->addWidget(img);
        }
        break;
    }
    case static_cast<quint8>(MsgKind::File): {
        auto *line = new QHBoxLayout;
        const QString human = m.fileSize > 1024 * 1024 ? QStringLiteral("%1 MB").arg(m.fileSize / 1048576.0, 0, 'f', 1)
                                                       : QStringLiteral("%1 KB").arg(m.fileSize / 1024.0, 0, 'f', 1);
        auto *l = new QLabel(QStringLiteral("📎 %1（%2）").arg(m.fileName.toHtmlEscaped(), human), row);
        line->addWidget(l);
        auto *dl = new QPushButton(QStringLiteral("下载"), row);
        const QString hash = m.contentHash, name = m.fileName, mime = m.mime;
        const qint64 size = m.fileSize;
        connect(dl, &QPushButton::clicked, this, [this, hash, name, size, mime] { askDownload(hash, name, size, mime); });
        line->addWidget(dl);
        col->addLayout(line);
        break;
    }
    default:
        break;
    }
    lay->addSpacing(8);
    lay->addLayout(col, 1);
    return row;
}

void ChatView::sendText() {
    const QString text = m_input->toPlainText();
    if (text.trimmed().isEmpty() || m_roomId.isEmpty())
        return;
    QString err;
    // UC-08 steps 1-2: compose + send to host over TLS
    const QString id = m_engine->chat()->sendText(m_roomId, text, &err);
    if (id.isEmpty()) {
        if (!err.isEmpty()) {
            auto *it = new QListWidgetItem(m_list);
            auto *l = new QLabel(err, m_list);
            l->setStyleSheet(QStringLiteral("color:#e53935;"));
            it->setSizeHint(l->sizeHint());
            m_list->setItemWidget(it, l);
        }
        return;
    }
    m_input->clear();
}

void ChatView::pickImage() {
    const QString f = QFileDialog::getOpenFileName(this, QStringLiteral("选择图片"), QDir::homePath(),
                                                   QStringLiteral("图片 (*.png *.jpg *.jpeg *.bmp *.webp)"));
    if (f.isEmpty() || m_roomId.isEmpty())
        return;
    QString err;
    if (m_engine->chat()->sendImage(m_roomId, f, &err).isEmpty() && !err.isEmpty()) {
        auto *it = new QListWidgetItem(m_list);
        auto *l = new QLabel(err, m_list);
        l->setStyleSheet(QStringLiteral("color:#e53935;"));
        it->setSizeHint(l->sizeHint());
        m_list->setItemWidget(it, l);
    }
}

void ChatView::pickFile() {
    const QString f = QFileDialog::getOpenFileName(this, QStringLiteral("通告文件（不发送内容）"), QDir::homePath());
    if (f.isEmpty() || m_roomId.isEmpty())
        return;
    m_engine->files()->announceFile(m_roomId, f, [this](const QString &msgId, const QString &err) {
        if (!err.isEmpty()) {
            auto *it = new QListWidgetItem(m_list);
            auto *l = new QLabel(err, m_list); // UC-09 E1: host offline etc.
            l->setStyleSheet(QStringLiteral("color:#e53935;"));
            it->setSizeHint(l->sizeHint());
            m_list->setItemWidget(it, l);
        }
        (void)msgId;
    });
}

// ---- UC-10: supplier dialog + start ----
void ChatView::askDownload(const QString &hash, const QString &name, qint64 size, const QString &mime) {
    if (m_engine->files()->hasVerifiedFile(hash)) { // BR-17: identical content already local
        auto *it = new QListWidgetItem(m_list);
        auto *l = new QLabel(QStringLiteral("本机已存在同一哈希的文件（校验值一致），无需重复下载。"), m_list);
        l->setStyleSheet(QStringLiteral("color:gray;"));
        it->setSizeHint(l->sizeHint());
        m_list->setItemWidget(it, l);
        return;
    }
    auto *d = new DDialog(this);
    d->setTitle(QStringLiteral("下载 %1").arg(name));
    d->setMessage(QStringLiteral("正在查询候选供给节点…"));
    d->open();

    const QString roomId = m_roomId;
    m_engine->files()->querySuppliers(roomId, hash, [this, d, roomId, hash, name, size, mime](const QVector<SupplierInfo> &sup) {
        if (sup.isEmpty()) { // UC-10 E1: no node holds it yet
            d->setTitle(QStringLiteral("等待供给"));
            d->setMessage(QStringLiteral("该文件当前没有可用供给节点。保持通告存在，任一持有节点上线后即可重试。"));
            d->addButton(QStringLiteral("知道了"));
            return;
        }
        auto *list = new QListWidget(d);
        for (const auto &s : sup)
            list->addItem(QStringLiteral("%1 · %2:%3 %4")
                              .arg(s.nickname, s.ip)
                              .arg(s.port)
                              .arg(s.online ? QStringLiteral("（在线，默认）") : QStringLiteral("（暂离线）")));
        // UC-10 step 3: default-check the online node, confirm before start
        for (int i = 0; i < list->count(); ++i) {
            if (list->item(i)->text().contains(QStringLiteral("在线"))) {
                list->setCurrentRow(i);
                break;
            }
        }
        if (list->currentRow() < 0)
            list->setCurrentRow(0);
        d->setMessage(QStringLiteral("选择供给节点："));
        d->addContent(list);
        d->addButton(QStringLiteral("取消"));
        d->addButton(QStringLiteral("开始下载"), true, DDialog::ButtonRecommend);
        connect(d, &DDialog::buttonClicked, this, [this, list, sup, d, roomId, hash, name, size, mime](int index, const QString &) {
            if (index != 1)
                return;
            QVector<SupplierInfo> all = sup;
            const int row = list->currentRow();
            if (row > 0 && row < all.size())
                std::swap(all[0], all[row]); // preferred supplier leads the fallback list
            m_engine->downloader()->startTask(roomId, hash, name, size, mime, all);
            d->close();
            emit downloadsStarted();
        });
    });
}

} // namespace meshlink
