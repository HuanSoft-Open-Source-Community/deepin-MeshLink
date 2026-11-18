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

    // Stage reached by the last start() failure. Lets the UI choose recovery
    // (identity) vs. network-settings diagnostics without matching translated
    // error strings.
    enum class StartStage { None, Storage, Identity, Tcp, Udp };
    bool start(QString *error);
    StartStage startStage() const { return m_startStage; }
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
    // Multi-account: create an additional identity and make it active; the
    // network rebinds to it in place when the engine is already running.
    qint64 createAdditionalAccount(const QString &nickname, QString *error);
    // Multi-account: switch the active identity (stops/restarts networking).
    // On any failure the previous identity, active id and network state are
    // restored; lastSwitchFailure() tells the UI which step failed without
    // parsing translated error strings.
    bool switchAccount(qint64 accountId, QString *error);
    enum class SwitchFailure { None, Identity, Network };
    SwitchFailure lastSwitchFailure() const { return m_lastSwitchFailure; }
    // UC-02: destroy local account, keys and history.
    int destroyAccount(bool alsoHistory, QString *error);
    // Destroy an arbitrary local account. A non-active account is removed
    // without stopping the network or dropping the loaded identity; the active
    // account keeps the historical stop/wipe/close behaviour.
    int destroyAccountById(qint64 id, bool alsoHistory, QString *error);
    bool networkRunning() const { return m_networkRunning; }

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
    void accountSwitched(qint64 accountId); // multi-account: identity reloaded

private:
    void route(MsgType type, SecureChannel *ch, const QByteArray &payload);
    // Network bring-up shared by start() and switchAccount(); the identity
    // must already be loaded when this runs.
    bool startNetwork(QString *error);
    void stopNetwork();
    // Reload `previousId`, restore Settings and (when it was running before)
    // restart networking. Used as the rollback path of switchAccount().
    bool restorePreviousAccount(qint64 previousId, bool wasRunning, QString *error);

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
    StartStage m_startStage = StartStage::None;
    SwitchFailure m_lastSwitchFailure = SwitchFailure::None;
    bool m_networkRunning = false;
    QTimer *m_resyncTimer = nullptr;
};

} // namespace meshlink
