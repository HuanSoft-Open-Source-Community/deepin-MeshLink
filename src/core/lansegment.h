// deepin-MeshLink — LAN segment (IPv4 subnet) selection helpers.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
//
// A PC may be attached to several networks at once (Wi-Fi + wired + VPN +
// docker bridges). Discovery must broadcast into — and accept heartbeats from —
// exactly one user-selected segment, and the user must be able to switch it
// while the app keeps running. This module is pure (no sockets) so the address
// math can be unit-tested.
#pragma once

#include <QHostAddress>
#include <QString>
#include <QVector>

namespace meshlink {

struct LanSegment {
    QString interfaceName; // enp3s0 / lo / "(环境变量)"
    QString ip;            // IPv4 literal
    int prefixLength = 24;

    bool isValid() const;
    QHostAddress address() const;
    // Directed broadcast for the segment. Loopback and /31-/32 fall back to
    // the limited broadcast address 255.255.255.255.
    QHostAddress broadcastAddress() const;
    bool contains(const QHostAddress &addr) const;
    QString label() const; // "enp3s0 · 192.168.1.23/24 → 广播 192.168.1.255"
    QString key() const;   // interfaceName|ip|prefix
    bool operator==(const LanSegment &other) const;
    bool operator!=(const LanSegment &other) const { return !(*this == other); }
};

namespace LanSegments {
// All usable IPv4 addresses: non-loopback, non-link-local, up interfaces first,
// loopback last. Deterministic ordering (interface name then address).
QVector<LanSegment> enumerate();
// Sensible default when the user has not chosen anything yet (loopback fallback).
LanSegment preferred();
// MESHLINK_IP / optional MESHLINK_LAN_PREFIX override used by tests and headless
// runs; invalid/empty when no override is set.
LanSegment fromEnvironment();
} // namespace LanSegments

} // namespace meshlink
