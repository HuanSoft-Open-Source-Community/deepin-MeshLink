// deepin-MeshLink — settings implementation.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "settings.h"

#include "common.h"
#include "lansegment.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>

namespace meshlink {
namespace {
constexpr int kDefaultHeartbeatMs = 3000;   // UC-04 step 2
constexpr int kDefaultOfflinePeriods = 3;   // UC-04 step 5
constexpr quint64 kDefaultChunkBytes = 256 * 1024; // UC-11 step 1
constexpr int kDefaultThreads = 4;          // NFR-05 (4..8)
} // namespace

namespace Paths {
static QString envOr(const char *var, const QString &fallback) {
    const QByteArray v = qgetenv(var);
    return v.isEmpty() ? fallback : QString::fromLocal8Bit(v);
}
QString dataRoot() {
    return envOr("MESHLINK_DATA_HOME",
                 QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
}
QString configRoot() {
    return envOr("MESHLINK_CONFIG_HOME",
                 QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation));
}
QString databaseFile() { return dataRoot() + "/meshlink.db"; }
QString keystoreFile() { return dataRoot() + "/keystore/identity.bin"; }
QString keystoreSalt() { return dataRoot() + "/keystore/salt"; }
QString keystoreDeviceKey() { return dataRoot() + "/keystore/device.key"; }
QString avatarDir() { return dataRoot() + "/avatars"; }
QString blobDir() { return dataRoot() + "/blobs"; }
QString downloadDir() { return dataRoot() + "/downloads"; }
QString fileDir() { return dataRoot() + "/files"; }
QString logDir() { return dataRoot() + "/logs"; }
void ensureDirs() {
    for (const QString &d : {dataRoot(), configRoot(), dataRoot() + "/keystore", avatarDir(),
                             blobDir(), downloadDir(), fileDir(), logDir()}) {
        QDir().mkpath(d);
    }
    // The keystore directory holds the salt and the per-install device secret;
    // restrict it to the owner (0700) on every start.
    QFile::setPermissions(dataRoot() + "/keystore",
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
}
} // namespace Paths

Settings::Settings(QObject *parent)
    : QObject(parent), m_settings(Paths::configRoot() + "/meshlink.ini", QSettings::IniFormat) {
}

// Env overrides take precedence over the ini file. Production runs never set
// them; the integration harness uses them to give each node distinct ports and
// a shared loopback discovery bootstrap.
static QByteArray envOverride(const char *name) { return qgetenv(name); }

quint16 Settings::discoveryPort() const {
    const QByteArray e = envOverride("MESHLINK_DISCOVERY_PORT");
    return e.isEmpty() ? static_cast<quint16>(m_settings.value("network/discoveryPort", 2425).toUInt())
                       : static_cast<quint16>(e.toUShort());
}
quint16 Settings::tcpPort() const {
    const QByteArray e = envOverride("MESHLINK_TCP_PORT");
    return e.isEmpty() ? static_cast<quint16>(m_settings.value("network/tcpPort", 2426).toUInt())
                       : static_cast<quint16>(e.toUShort());
}
int Settings::heartbeatIntervalMs() const {
    const QByteArray e = envOverride("MESHLINK_HEARTBEAT_MS");
    return e.isEmpty() ? m_settings.value("network/heartbeatIntervalMs", kDefaultHeartbeatMs).toInt() : e.toInt();
}
int Settings::offlinePeriods() const {
    const QByteArray e = envOverride("MESHLINK_OFFLINE_PERIODS");
    return e.isEmpty() ? m_settings.value("network/offlinePeriods", kDefaultOfflinePeriods).toInt() : e.toInt();
}
quint64 Settings::chunkSize() const {
    const QByteArray e = envOverride("MESHLINK_CHUNK_KB");
    return e.isEmpty() ? m_settings.value("files/chunkSizeKb", int(kDefaultChunkBytes / 1024)).toULongLong() * 1024ull
                       : e.toULongLong() * 1024ull;
}
int Settings::downloadThreads() const {
    const QByteArray e = envOverride("MESHLINK_THREADS");
    return e.isEmpty() ? m_settings.value("files/downloadThreads", kDefaultThreads).toInt() : e.toInt();
}
int Settings::maxRoomHistory() const { return m_settings.value("chat/maxHistory", 200).toInt(); }
bool Settings::hideNavLabels() const { return m_settings.value("interface/hideNavLabels", false).toBool(); }
bool Settings::sendWithEnter() const { return m_settings.value("chat/sendWithEnter", true).toBool(); }
void Settings::setHideNavLabels(bool hide) { m_settings.setValue("interface/hideNavLabels", hide); emit changed(); }
void Settings::setSendWithEnter(bool enter) { m_settings.setValue("chat/sendWithEnter", enter); emit changed(); }

bool Settings::notificationsEnabled() const {
    return m_settings.value("notifications/enabled", true).toBool();
}
bool Settings::notificationPreview() const {
    return m_settings.value("notifications/preview", true).toBool();
}
bool Settings::trayEnabled() const { return m_settings.value("behavior/trayEnabled", true).toBool(); }
Settings::CloseAction Settings::closeAction() const {
    const QString raw = m_settings.value("behavior/closeAction", QStringLiteral("ask")).toString();
    if (raw == QLatin1String("minimize"))
        return CloseAction::Minimize;
    if (raw == QLatin1String("exit"))
        return CloseAction::Exit;
    return CloseAction::Ask;
}
void Settings::setNotificationsEnabled(bool on) { m_settings.setValue("notifications/enabled", on); emit changed(); }
void Settings::setNotificationPreview(bool show) { m_settings.setValue("notifications/preview", show); emit changed(); }
void Settings::setTrayEnabled(bool on) { m_settings.setValue("behavior/trayEnabled", on); emit changed(); }
void Settings::setCloseAction(CloseAction action) {
    const char *raw = action == CloseAction::Minimize ? "minimize"
                      : action == CloseAction::Exit   ? "exit"
                                                      : "ask";
    m_settings.setValue("behavior/closeAction", QString::fromLatin1(raw));
    emit changed();
}

qint64 Settings::activeAccountId() const {
    return m_settings.value("accounts/activeId", 1).toLongLong();
}
void Settings::setActiveAccountId(qint64 id) {
    m_settings.setValue("accounts/activeId", id);
    emit changed();
}

void Settings::setDiscoveryPort(quint16 v) { m_settings.setValue("network/discoveryPort", int(v)); emit changed(); }
void Settings::setTcpPort(quint16 v) { m_settings.setValue("network/tcpPort", int(v)); emit changed(); }
void Settings::setHeartbeatIntervalMs(int v) { m_settings.setValue("network/heartbeatIntervalMs", v); emit changed(); }
void Settings::setOfflinePeriods(int v) { m_settings.setValue("network/offlinePeriods", v); emit changed(); }
void Settings::setChunkSizeKb(int kb) { m_settings.setValue("files/chunkSizeKb", kb); emit changed(); }
void Settings::setDownloadThreads(int n) { m_settings.setValue("files/downloadThreads", n); emit changed(); }
void Settings::setMaxRoomHistory(int n) { m_settings.setValue("chat/maxHistory", n); emit changed(); }

QStringList Settings::bootstrapTargets() const {
    QStringList raw;
    const QByteArray e = qgetenv("MESHLINK_BOOTSTRAP");
    if (!e.isEmpty())
        raw << QString::fromLatin1(e).split(',', Qt::SkipEmptyParts);
    raw << m_settings.value("net/bootstrap").toStringList();
    QStringList out;
    for (const auto &v : raw) {
        QString entry = v.trimmed();
        if (entry.contains(':') && !entry.startsWith("[")) { // ipv4:port
            bool ok = false;
            const quint16 p = entry.section(':', 1).toUShort(&ok);
            const QString ip = entry.section(':', 0, 0);
            if (ok && p > 0 && !ip.isEmpty())
                out << QStringLiteral("%1:%2").arg(ip).arg(p);
        }
    }
    return out;
}

void Settings::addBootstrapTarget(const QString &ip, quint16 port) {
    const QString entry = QStringLiteral("%1:%2").arg(ip).arg(port);
    QStringList list = bootstrapTargets();
    if (!list.contains(entry))
        list << entry;
    setBootstrapTargets(list);
}

void Settings::setBootstrapTargets(const QStringList &entries) {
    m_settings.setValue("net/bootstrap", entries);
    emit changed();
}

LanSegment Settings::lanSegment() const {
    // Tests/headless runs pin the segment through the environment.
    const LanSegment env = LanSegments::fromEnvironment();
    if (env.isValid())
        return env;

    LanSegment saved;
    saved.interfaceName = m_settings.value("network/lanInterface").toString();
    saved.ip = m_settings.value("network/lanIp").toString();
    saved.prefixLength = m_settings.value("network/lanPrefix", 0).toInt();
    if (saved.isValid()) {
        // Keep the choice if that address still exists on this machine; refresh
        // the interface name in case the kernel renamed it.
        for (const auto &s : LanSegments::enumerate()) {
            if (s.ip == saved.ip && s.prefixLength == saved.prefixLength) {
                saved.interfaceName = s.interfaceName;
                return saved;
            }
        }
    }
    return LanSegments::preferred();
}

void Settings::setLanSegment(const LanSegment &segment) {
    if (!segment.isValid())
        return;
    m_settings.setValue("network/lanInterface", segment.interfaceName);
    m_settings.setValue("network/lanIp", segment.ip);
    m_settings.setValue("network/lanPrefix", segment.prefixLength);
    m_settings.sync();
    emit changed();
}

QString Settings::lanIpAddress() const { return lanSegment().ip; }

} // namespace meshlink
