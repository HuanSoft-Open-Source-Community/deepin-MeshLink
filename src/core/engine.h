// deepin-MeshLink — engine: owns and wires all core modules (headless-testable,
// the GUI binds to it; UC-01..UC-13 backbone).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#pragma once

#include "account.h"
#include "chatmanager.h"
#include "common.h"
#include "discovery.h"
#include "downloader.h"
#include "filemanager.h"
#include "protocol.h"
#include "roommanager.h"
#include "securelink.h"
#include "settings.h"
#include "storage.h"

#include <QObject>

namespace meshlink {

class Engine : public QObject {
    Q_OBJECT
public:
    explicit Engine(Settings *settings, QObject *parent = nullptr);
    ~Engine() override;

    // UC-01: create or unlock the local account, then bring up networking.
    bool start(QString *error);
    void shutdown(); // UC-04 farewell + stop listeners

    // UC-01: whether an account row already exists on disk (used by the
    // first-start wizard before start() loads the profile into memory).
    bool hasLocalAccount();

    // LAN segment: chosen in the wizard and switchable at runtime.
    bool setLanSegment(const LanSegment &segment, QString *error);
    const LanSegment &lanSegment() const { return m_segment; }
    QString lanSegmentLabel() const { return m_segment.label(); }

    // A2 (UC-01): create a brand-new identity, wiping any existing one.
    bool createFreshAccount(const QString &nickname, QString *error);
    // UC-02: destroy local account, keys and history.
    bool destroyAccount(bool alsoHistory, QString *error);

    // Deliver a frame to `fp`, dialing on demand when no channel is open yet.
    bool sendToPeer(const QString &fp, MsgType type, const QByteArray &payload);
    // Endpoint lookup against the live discovery table (BR-04).
    bool resolvePeer(const QString &fp, QString &ip, quint16 &port, bool &online);

    AccountManager *account() { return &m_account; }
    Storage *storage() { return &m_storage; }
    Discovery *discovery() { return &m_discovery; }
    SecureLink *link() { return &m_link; }
    RoomManager *rooms() { return &m_rooms; }
    ChatManager *chat() { return &m_chat; }
    FileManager *files() { return &m_files; }
    Downloader *downloader() { return &m_downloader; }
    Settings *settings() { return m_settings; }

    QString tcpEndpointIp() const { return m_lanIp; }
    quint16 tcpEndpointPort() const { return m_settings->tcpPort(); }

signals:
    void spoofingSuspected(const QString &fingerprint); // UC-03 A1 surfaced to UI
    void started(bool ok);
    void lanSegmentChanged(const QString &label);

private:
    void route(MsgType type, SecureChannel *ch, const QByteArray &payload);

    Settings *m_settings;
    Storage m_storage;
    AccountManager m_account;
    Discovery m_discovery;
    LanSegment m_segment;
    SecureLink m_link;
    RoomManager m_rooms;
    ChatManager m_chat;
    Downloader m_downloader; // before m_files: the file manager forwards serving
    FileManager m_files;
    QString m_lanIp;
    qint64 m_lastResyncMs = 0;
};

} // namespace meshlink
