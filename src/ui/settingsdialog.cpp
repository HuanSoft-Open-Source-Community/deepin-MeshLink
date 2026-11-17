// deepin-MeshLink — DTK settings dialog implementation.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "settingsdialog.h"

#include "core/discovery.h"
#include "core/engine.h"
#include "core/settings.h"
#include "uiutils.h"

#include <DComboBox>
#include <DMessageManager>
#include <DSettings>
#include <DSettingsOption>
#include <DSettingsWidgetFactory>
#include <dsettingsbackend.h>

#include <QApplication>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QSignalBlocker>
#include <QTimer>
#include <QVariant>

#include <utility>

DWIDGET_USE_NAMESPACE
DCORE_USE_NAMESPACE
DGUI_USE_NAMESPACE

namespace meshlink {

namespace {

const QString kLanSegment = QStringLiteral("network.discovery.lanSegment");
const QString kUdpPort = QStringLiteral("network.discovery.udpPort");
const QString kHeartbeat = QStringLiteral("network.discovery.heartbeat");
const QString kOffline = QStringLiteral("network.discovery.offline");
const QString kTlsPort = QStringLiteral("network.tls.port");
const QString kChunkKb = QStringLiteral("files.downloads.chunkKb");
const QString kThreads = QStringLiteral("files.downloads.threads");
const QString kHistory = QStringLiteral("files.downloads.history");
const QString kHideNavLabels = QStringLiteral("interface.sidebar.hideNavLabels");
const QString kSendKey = QStringLiteral("interface.chat.sendKey");
const QString kTrayEnabled = QStringLiteral("behavior.window.enableTray");
const QString kCloseAction = QStringLiteral("behavior.window.closeAction");
const QString kNotifEnabled = QStringLiteral("notifications.general.enabled");
const QString kNotifPreview = QStringLiteral("notifications.general.preview");

} // namespace

// The DSettings backend lives in a worker thread (DSettings::setBackend moves
// it there), so it only owns an in-memory snapshot. Persisting the real value
// happens on the main thread in onSettingChanged().
class SettingsDialog::Backend : public DSettingsBackend {
public:
    explicit Backend(QHash<QString, QVariant> values) : m_values(std::move(values)) {}

    QStringList keys() const override {
        QMutexLocker locker(&m_mutex);
        return m_values.keys();
    }
    QVariant getOption(const QString &key) const override {
        QMutexLocker locker(&m_mutex);
        return m_values.value(key);
    }

protected:
    void doSync() override {}
    void doSetOption(const QString &key, const QVariant &value) override {
        QMutexLocker locker(&m_mutex);
        m_values.insert(key, value);
    }

private:
    mutable QMutex m_mutex;
    QHash<QString, QVariant> m_values;
};

SettingsDialog::SettingsDialog(Engine *engine, QWidget *parent)
    : DSettingsDialog(parent), m_engine(engine) {
    setWindowTitle(tr("Settings"));
    if (qApp)
        setIcon(qApp->windowIcon());

    auto *s = m_engine->settings();
    QHash<QString, QVariant> initial;
    initial.insert(kLanSegment, s->lanSegment().key());
    initial.insert(kUdpPort, int(s->discoveryPort()));
    initial.insert(kHeartbeat, s->heartbeatIntervalMs());
    initial.insert(kOffline, s->offlinePeriods());
    initial.insert(kTlsPort, int(s->tcpPort()));
    initial.insert(kChunkKb, int(s->chunkSize() / 1024));
    initial.insert(kThreads, s->downloadThreads());
    initial.insert(kHistory, s->maxRoomHistory());
    initial.insert(kHideNavLabels, s->hideNavLabels());
    initial.insert(kSendKey,
                   s->sendWithEnter() ? QStringLiteral("enter") : QStringLiteral("ctrlEnter"));
    initial.insert(kTrayEnabled, s->trayEnabled());
    initial.insert(kCloseAction,
                   s->closeAction() == Settings::CloseAction::Minimize
                       ? QStringLiteral("minimize")
                       : s->closeAction() == Settings::CloseAction::Exit
                             ? QStringLiteral("exit")
                             : QStringLiteral("ask"));
    initial.insert(kNotifEnabled, s->notificationsEnabled());
    initial.insert(kNotifPreview, s->notificationPreview());
    m_initialUdpPort = int(s->discoveryPort());
    m_initialTcpPort = int(s->tcpPort());

    m_backend = new Backend(initial);

    QPointer<DSettings> parsed =
        DSettings::fromJsonFile(QStringLiteral(":/settings/meshlink-settings.json"));
    if (parsed.isNull()) {
        delete m_backend;
        m_backend = nullptr;
        return;
    }
    m_settings = parsed.data();
    m_settings->setBackend(m_backend);

    widgetFactory()->registerWidget(
        QStringLiteral("meshlink-lan-segment"),
        [this](QObject *object) -> QPair<QWidget *, QWidget *> {
            auto *option = qobject_cast<DSettingsOption *>(object);
            if (!option)
                return {nullptr, nullptr};

            auto *combo = new DComboBox;
            combo->setObjectName(QStringLiteral("settingsDialog.lanSegmentCombo"));
            combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
            combo->setMinimumContentsLength(18);
            combo->setMinimumWidth(280);
            m_segmentCombo = combo;

            connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                    [option, combo](int index) {
                        if (!combo || index < 0)
                            return;
                        option->setValue(combo->itemData(index).toString());
                    });
            connect(option, &DSettingsOption::valueChanged, this, [combo](const QVariant &value) {
                if (!combo)
                    return;
                const int index = combo->findData(value.toString());
                if (index >= 0 && index != combo->currentIndex()) {
                    const QSignalBlocker blocker(combo);
                    combo->setCurrentIndex(index);
                }
            });

            refreshLanSegments();
            return DSettingsWidgetFactory::createStandardItem(
                QByteArrayLiteral("MeshLinkSettings"), option, combo);
        });

    updateSettings(QByteArrayLiteral("MeshLinkSettings"), m_settings);
    setResetVisible(false);
    connect(m_settings, &DSettings::valueChanged, this, &SettingsDialog::onSettingChanged);
    refreshLanSegments();
}

SettingsDialog::~SettingsDialog() {
    // Destroying DSettings stops and joins the backend worker thread before the
    // backend object itself is released.
    delete m_settings;
    m_settings = nullptr;
    delete m_backend;
    m_backend = nullptr;
}

void SettingsDialog::refreshLanSegments() {
    if (!m_segmentCombo || !m_settings)
        return;
    auto option = m_settings->option(kLanSegment);
    if (!option)
        return;

    const QSignalBlocker blocker(m_segmentCombo);
    const QString current = option->value().toString();
    const QVector<LanSegment> segments = LanSegments::enumerate();
    const bool forced = LanSegments::fromEnvironment().isValid();

    m_segmentCombo->clear();
    int selected = -1;
    for (const LanSegment &segment : segments) {
        // Keep the combo compact; the full label (with broadcast address) is
        // available as a tooltip.
        const QString label =
            forced ? QStringLiteral("%1/%2").arg(segment.ip).arg(segment.prefixLength)
                   : QStringLiteral("%1 · %2/%3")
                         .arg(segment.interfaceName, segment.ip)
                         .arg(segment.prefixLength);
        m_segmentCombo->addItem(label, segment.key());
        m_segmentCombo->setItemData(m_segmentCombo->count() - 1, segment.label(),
                                    Qt::ToolTipRole);
        if (segment.key() == current)
            selected = m_segmentCombo->count() - 1;
    }

    if (segments.isEmpty()) {
        m_segmentCombo->addItem(tr("No usable IPv4 interface"), QString());
        m_segmentCombo->setEnabled(false);
    } else {
        m_segmentCombo->setEnabled(!forced);
        if (selected < 0)
            selected = 0;
        m_segmentCombo->setCurrentIndex(selected);
        if (option->value().toString().isEmpty() && selected >= 0)
            option->setValue(m_segmentCombo->itemData(selected).toString());
    }
    m_segmentCombo->setToolTip(
        forced ? tr("Pinned by the MESHLINK_IP environment variable")
               : tr("Heartbeats are broadcast into and accepted only from this segment"));
}

QVariant SettingsDialog::currentValue(const QString &key) const {
    auto *s = m_engine->settings();
    if (key == kLanSegment)
        return s->lanSegment().key();
    if (key == kUdpPort)
        return int(s->discoveryPort());
    if (key == kHeartbeat)
        return s->heartbeatIntervalMs();
    if (key == kOffline)
        return s->offlinePeriods();
    if (key == kTlsPort)
        return int(s->tcpPort());
    if (key == kChunkKb)
        return int(s->chunkSize() / 1024);
    if (key == kThreads)
        return s->downloadThreads();
    if (key == kHistory)
        return s->maxRoomHistory();
    if (key == kHideNavLabels)
        return s->hideNavLabels();
    if (key == kSendKey)
        return s->sendWithEnter() ? QStringLiteral("enter") : QStringLiteral("ctrlEnter");
    if (key == kTrayEnabled)
        return s->trayEnabled();
    if (key == kCloseAction)
        return s->closeAction() == Settings::CloseAction::Minimize
                   ? QStringLiteral("minimize")
                   : s->closeAction() == Settings::CloseAction::Exit ? QStringLiteral("exit")
                                                                     : QStringLiteral("ask");
    if (key == kNotifEnabled)
        return s->notificationsEnabled();
    if (key == kNotifPreview)
        return s->notificationPreview();
    return {};
}

bool SettingsDialog::envPinned(const QString &key, QString *variable) const {
    const char *name = nullptr;
    if (key == kLanSegment)
        name = "MESHLINK_IP";
    else if (key == kUdpPort)
        name = "MESHLINK_DISCOVERY_PORT";
    else if (key == kHeartbeat)
        name = "MESHLINK_HEARTBEAT_MS";
    else if (key == kOffline)
        name = "MESHLINK_OFFLINE_PERIODS";
    else if (key == kTlsPort)
        name = "MESHLINK_TCP_PORT";
    else if (key == kChunkKb)
        name = "MESHLINK_CHUNK_KB";
    else if (key == kThreads)
        name = "MESHLINK_THREADS";
    if (!name || qgetenv(name).isEmpty())
        return false;
    if (variable)
        *variable = QString::fromLatin1(name);
    return true;
}

LanSegment SettingsDialog::segmentFromKey(const QString &key) const {
    if (key.isEmpty())
        return {};
    for (const LanSegment &segment : LanSegments::enumerate()) {
        if (segment.key() == key)
            return segment;
    }
    const QStringList parts = key.split(QLatin1Char('|'));
    if (parts.size() != 3)
        return {};
    LanSegment segment;
    segment.interfaceName = parts.at(0);
    segment.ip = parts.at(1);
    segment.prefixLength = parts.at(2).toInt();
    return segment.isValid() ? segment : LanSegment{};
}

void SettingsDialog::revertValue(const QString &key, const QVariant &value) {
    if (!m_settings || m_applying)
        return;
    m_applying = true;
    m_settings->setOption(key, value);
    m_applying = false;
}

void SettingsDialog::applyValue(const QString &key, const QVariant &value) {
    auto *s = m_engine->settings();
    if (key == kLanSegment) {
        const LanSegment segment = segmentFromKey(value.toString());
        if (!segment.isValid()) {
            revertValue(key, currentValue(key));
            UiUtils::warning(this, tr("LAN segment"),
                             tr("The selected LAN segment is no longer available."));
            return;
        }
        QString error;
        if (!m_engine->setLanSegment(segment, &error)) {
            revertValue(key, currentValue(key));
            UiUtils::warning(this, tr("LAN segment"),
                             error.isEmpty() ? tr("Could not switch the LAN segment.") : error);
        }
        return;
    }
    if (key == kUdpPort) {
        s->setDiscoveryPort(quint16(value.toInt()));
    } else if (key == kHeartbeat) {
        s->setHeartbeatIntervalMs(value.toInt());
        m_engine->discovery()->restartTimer();
    } else if (key == kOffline) {
        s->setOfflinePeriods(value.toInt());
    } else if (key == kTlsPort) {
        s->setTcpPort(quint16(value.toInt()));
    } else if (key == kChunkKb) {
        s->setChunkSizeKb(value.toInt());
    } else if (key == kThreads) {
        s->setDownloadThreads(value.toInt());
    } else if (key == kHistory) {
        s->setMaxRoomHistory(value.toInt());
    } else if (key == kHideNavLabels) {
        s->setHideNavLabels(value.toBool());
    } else if (key == kSendKey) {
        s->setSendWithEnter(value.toString() == QStringLiteral("enter"));
    } else if (key == kTrayEnabled) {
        s->setTrayEnabled(value.toBool());
    } else if (key == kCloseAction) {
        const QString raw = value.toString();
        s->setCloseAction(raw == QLatin1String("minimize") ? Settings::CloseAction::Minimize
                          : raw == QLatin1String("exit")   ? Settings::CloseAction::Exit
                                                           : Settings::CloseAction::Ask);
    } else if (key == kNotifEnabled) {
        s->setNotificationsEnabled(value.toBool());
    } else if (key == kNotifPreview) {
        s->setNotificationPreview(value.toBool());
    }
}

void SettingsDialog::onSettingChanged(const QString &key, const QVariant &value) {
    if (m_applying || !m_settings)
        return;
    const QVariant old = currentValue(key);
    if (!old.isValid())
        return; // title2 hints and unknown keys

    QString variable;
    if (envPinned(key, &variable)) {
        revertValue(key, old);
        DMessageManager::instance()->sendMessage(
            this, UiUtils::icon(QStringLiteral("dialog-warning")),
            tr("This setting is controlled by the %1 environment variable.").arg(variable));
        return;
    }
    applyValue(key, value);
}

bool SettingsDialog::takeRestartRequired() {
    auto *s = m_engine->settings();
    const int udp = int(s->discoveryPort());
    const int tcp = int(s->tcpPort());
    const bool required = udp != m_initialUdpPort || tcp != m_initialTcpPort;
    m_initialUdpPort = udp;
    m_initialTcpPort = tcp;
    return required;
}

} // namespace meshlink
