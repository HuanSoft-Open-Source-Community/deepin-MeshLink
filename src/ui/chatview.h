// deepin-MeshLink — chat panel: message stream + composer (UC-08, UC-09 UI).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#pragma once

#include "core/chatmanager.h"
#include "core/common.h"

#include <QWidget>

class QListWidget;
class QLabel;
class QPushButton;
class QPlainTextEdit;

namespace meshlink {

class Engine;

class ChatView : public QWidget {
    Q_OBJECT
public:
    explicit ChatView(Engine *engine, QWidget *parent = nullptr);

    void setRoom(const QString &roomId);
    void appendMessage(const Msg &m); // live
    void refreshMuteState();

signals:
    void downloadsStarted(); // MainWindow: jump to transfers page

private:
    bool eventFilter(QObject *o, QEvent *e) override;
    QWidget *messageWidget(const Msg &m);
    void renderHistory();
    void sendText();
    void pickImage();
    void pickFile(); // UC-09: announce only, never push bytes (BR-19)
    void askDownload(const QString &hash, const QString &name, qint64 size, const QString &mime); // UC-10

    Engine *m_engine;
    QString m_roomId;
    QLabel *m_header = nullptr;
    QLabel *m_muteBanner = nullptr;
    QListWidget *m_list = nullptr;
    QPlainTextEdit *m_input = nullptr;
    QPushButton *m_sendBtn = nullptr;
};

} // namespace meshlink
