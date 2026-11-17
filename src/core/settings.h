// deepin-MeshLink — application settings & data paths (NFR-06, NFR-08).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#pragma once

#include "lansegment.h"

#include <QObject>
#include <QSettings>
#include <QString>
#include <QStringList>

namespace meshlink {

// Filesystem layout helpers. MESHLINK_DATA_HOME / MESHLINK_CONFIG_HOME
// environment overrides exist for tests and portable installs.
namespace Paths {
QString dataRoot();      // ~/.local/share/deepin-meshlink
QString configRoot();    // ~/.config/deepin-meshlink
QString databaseFile();  // <data>/meshlink.db
QString keystoreFile();  // <data>/keystore/identity.bin
QString keystoreSalt();  // <data>/keystore/salt
QString avatarDir();     // <data>/avatars
QString blobDir();       // <data>/blobs   (avatars + images shared by hash)
QString downloadDir();   // <data>/downloads  (task working dirs)
QString fileDir();       // <data>/files      (verified finished files)
QString logDir();        // <data>/logs
void ensureDirs();
} // namespace Paths

class Settings : public QObject {
    Q_OBJECT
public:
    explicit Settings(QObject *parent = nullptr);

    // ---- desktop notifications / tray behaviour ----
    enum class CloseAction { Ask = 0, Minimize = 1, Exit = 2 };

    bool notificationsEnabled() const;  // global XDG desktop notifications
    bool notificationPreview() const;   // show message text in the popup
    bool trayEnabled() const;           // XDG StatusNotifierItem tray icon
    CloseAction closeAction() const;    // what a window close means
    void setNotificationsEnabled(bool on);
    void setNotificationPreview(bool show);
    void setTrayEnabled(bool on);
    void setCloseAction(CloseAction action);

    // ---- multi-account ----
    qint64 activeAccountId() const;     // account row to load at start
    void setActiveAccountId(qint64 id);

    // ---- network ----
    quint16 discoveryPort() const;      // UDP heartbeat port (default 2425)
    quint16 tcpPort() const;            // TLS listener port (default 2426)
    int heartbeatIntervalMs() const;    // default 3000
    int offlinePeriods() const;         // N missed heartbeats -> offline, default 3

    // ---- files / transfers ----
    quint64 chunkSize() const;          // bytes, default 256 KiB (256*1024)
    int downloadThreads() const;        // parallel chunk workers, default 4
    int maxRoomHistory() const;         // messages kept in snapshot, default 200

    // ---- interface / composer ----
    bool hideNavLabels() const;         // icon-only navigation rail, default false
    bool sendWithEnter() const;         // Enter sends (Shift+Enter = newline), default true
    void setHideNavLabels(bool hide);
    void setSendWithEnter(bool enter);

    void setDiscoveryPort(quint16 v);
    void setTcpPort(quint16 v);
    void setHeartbeatIntervalMs(int v);
    void setOfflinePeriods(int v);
    void setChunkSizeKb(int kb);
    void setDownloadThreads(int n);
    void setMaxRoomHistory(int n);

    // UC-04 E2: manual bootstrap endpoints when broadcast is blocked on the LAN.
    QStringList bootstrapTargets() const; // entries "ip:port"
    void addBootstrapTarget(const QString &ip, quint16 port);
    void setBootstrapTargets(const QStringList &entries);

    // Selected LAN segment (persisted; MESHLINK_IP overrides it for tests).
    LanSegment lanSegment() const;
    void setLanSegment(const LanSegment &segment);
    QString lanIpAddress() const; // segment address (UC-01 step 7)

signals:
    void changed();

private:
    QSettings m_settings;
};

} // namespace meshlink
