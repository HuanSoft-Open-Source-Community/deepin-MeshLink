// deepin-MeshLink — translation markers for the DTK settings JSON.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
//
// DSettingsDialog translates group/option texts with qApp->translate() using the
// context passed to updateSettings(). lupdate cannot scan JSON, so keep every
// translatable JSON string listed here. Keep in sync with
// resources/settings/meshlink-settings.json.
#include <QCoreApplication>

namespace meshlink {
namespace {

[[maybe_unused]] const char *const kSettingsTranslationSources[] = {
    QT_TRANSLATE_NOOP("MeshLinkSettings", "Network"),
    QT_TRANSLATE_NOOP("MeshLinkSettings", "Discovery"),
    QT_TRANSLATE_NOOP("MeshLinkSettings", "LAN segment"),
    QT_TRANSLATE_NOOP("MeshLinkSettings",
                      "The LAN segment, heartbeat and offline threshold changes apply immediately."),
    QT_TRANSLATE_NOOP("MeshLinkSettings", "UDP discovery port"),
    QT_TRANSLATE_NOOP("MeshLinkSettings", "Heartbeat interval (ms)"),
    QT_TRANSLATE_NOOP("MeshLinkSettings", "Offline threshold (heartbeat periods)"),
    QT_TRANSLATE_NOOP("MeshLinkSettings", "TLS service"),
    QT_TRANSLATE_NOOP("MeshLinkSettings",
                      "Port changes take effect after restarting deepin-MeshLink."),
    QT_TRANSLATE_NOOP("MeshLinkSettings", "TLS service port"),
    QT_TRANSLATE_NOOP("MeshLinkSettings", "Interface"),
    QT_TRANSLATE_NOOP("MeshLinkSettings", "Sidebar"),
    QT_TRANSLATE_NOOP("MeshLinkSettings", "Hide navigation labels"),
    QT_TRANSLATE_NOOP("MeshLinkSettings", "Chat"),
    QT_TRANSLATE_NOOP("MeshLinkSettings", "Send shortcut"),
    QT_TRANSLATE_NOOP("MeshLinkSettings", "Enter"),
    QT_TRANSLATE_NOOP("MeshLinkSettings", "Ctrl+Enter"),
    QT_TRANSLATE_NOOP("MeshLinkSettings", "Files"),
    QT_TRANSLATE_NOOP("MeshLinkSettings", "Download defaults"),
    QT_TRANSLATE_NOOP("MeshLinkSettings",
                      "Chunk size and threads apply to new downloads; history limit to "
                      "snapshots."),
    QT_TRANSLATE_NOOP("MeshLinkSettings", "Chunk size (KiB)"),
    QT_TRANSLATE_NOOP("MeshLinkSettings", "Parallel download threads"),
    QT_TRANSLATE_NOOP("MeshLinkSettings", "Snapshot message limit"),
    QT_TRANSLATE_NOOP("MeshLinkSettings", "Behavior"),
    QT_TRANSLATE_NOOP("MeshLinkSettings", "Window and tray"),
    QT_TRANSLATE_NOOP("MeshLinkSettings", "Show the XDG tray icon (StatusNotifierItem)"),
    QT_TRANSLATE_NOOP("MeshLinkSettings", "When closing the window"),
    QT_TRANSLATE_NOOP("MeshLinkSettings", "Ask every time"),
    QT_TRANSLATE_NOOP("MeshLinkSettings", "Minimize to the tray"),
    QT_TRANSLATE_NOOP("MeshLinkSettings", "Exit the application"),
    QT_TRANSLATE_NOOP("MeshLinkSettings",
                      "Minimizing keeps deepin-MeshLink resident so messages still arrive."),
    QT_TRANSLATE_NOOP("MeshLinkSettings", "Notifications"),
    QT_TRANSLATE_NOOP("MeshLinkSettings", "Desktop notifications"),
    QT_TRANSLATE_NOOP("MeshLinkSettings", "Enable desktop notifications (XDG message system)"),
    QT_TRANSLATE_NOOP("MeshLinkSettings", "Show message previews in notifications"),
    QT_TRANSLATE_NOOP(
        "MeshLinkSettings",
        "Rooms set to do-not-disturb stay quiet unless someone @-mentions you."),
};

} // namespace
} // namespace meshlink
