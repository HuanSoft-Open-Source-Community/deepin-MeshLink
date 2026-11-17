// deepin-MeshLink — LAN segment helpers (pure, unit-testable).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "lansegment.h"

#include <QCoreApplication>
#include <QNetworkInterface>

#include <algorithm>

namespace meshlink {
namespace {

bool hostToIpv4(const QHostAddress &addr, quint32 *out) {
    if (addr.protocol() == QAbstractSocket::IPv4Protocol) {
        *out = addr.toIPv4Address();
        return true;
    }
    // QUdpSocket/QSslSocket may report IPv4 peers as IPv4-mapped IPv6.
    const QString text = addr.toString();
    if (text.startsWith(QLatin1String("::ffff:"), Qt::CaseInsensitive)) {
        const QHostAddress v4(text.mid(7));
        if (v4.protocol() == QAbstractSocket::IPv4Protocol) {
            *out = v4.toIPv4Address();
            return true;
        }
    }
    return false;
}

quint32 netmaskFor(int prefixLength) {
    if (prefixLength <= 0)
        return 0;
    if (prefixLength >= 32)
        return 0xFFFFFFFFu;
    return 0xFFFFFFFFu << (32 - prefixLength);
}

int segmentRank(const LanSegment &s) {
    // 0 = routable LAN, 1 = loopback, 2 = link-local / other fallback
    if (QHostAddress(s.ip).isLoopback())
        return 1;
    if (QHostAddress(s.ip).isInSubnet(QHostAddress(QStringLiteral("169.254.0.0")), 16))
        return 2;
    return 0;
}

} // namespace

bool LanSegment::isValid() const {
    QHostAddress a;
    if (ip.isEmpty() || !a.setAddress(ip) || a.protocol() != QAbstractSocket::IPv4Protocol)
        return false;
    return prefixLength >= 0 && prefixLength <= 32;
}

QHostAddress LanSegment::address() const { return QHostAddress(ip); }

QHostAddress LanSegment::broadcastAddress() const {
    if (!isValid())
        return QHostAddress(QHostAddress::Broadcast);
    const quint32 addr = QHostAddress(ip).toIPv4Address();
    // /31 and /32 have no broadcast address; loopback broadcast is useless.
    if (prefixLength >= 31 || QHostAddress(ip).isLoopback())
        return QHostAddress(QHostAddress::Broadcast);
    const quint32 mask = netmaskFor(prefixLength);
    const quint32 broadcast = (addr & mask) | ~mask;
    if (broadcast == 0)
        return QHostAddress(QHostAddress::Broadcast);
    return QHostAddress(broadcast);
}

bool LanSegment::contains(const QHostAddress &addr) const {
    if (!isValid())
        return false;
    quint32 value = 0;
    if (!hostToIpv4(addr, &value))
        return false;
    const quint32 mask = netmaskFor(prefixLength);
    return (value & mask) == (QHostAddress(ip).toIPv4Address() & mask);
}

QString LanSegment::label() const {
    if (!isValid())
        return QCoreApplication::translate("LanSegment", "No segment selected");
    return QCoreApplication::translate("LanSegment", "%1 · %2/%3 → broadcast %4")
        .arg(interfaceName.isEmpty() ? QStringLiteral("?") : interfaceName, ip)
        .arg(prefixLength)
        .arg(broadcastAddress().toString());
}

QString LanSegment::key() const {
    return QStringLiteral("%1|%2|%3").arg(interfaceName, ip).arg(prefixLength);
}

bool LanSegment::operator==(const LanSegment &other) const {
    return interfaceName == other.interfaceName && ip == other.ip && prefixLength == other.prefixLength;
}

namespace LanSegments {

LanSegment fromEnvironment() {
    const QByteArray forced = qgetenv("MESHLINK_IP");
    if (forced.isEmpty())
        return {};
    LanSegment s;
    s.interfaceName = QCoreApplication::translate("LanSegment", "(environment)");
    s.ip = QString::fromLatin1(forced);
    const QByteArray prefix = qgetenv("MESHLINK_LAN_PREFIX");
    s.prefixLength = prefix.isEmpty() ? (QHostAddress(s.ip).isLoopback() ? 8 : 24) : prefix.toInt();
    return s.isValid() ? s : LanSegment{};
}

QVector<LanSegment> enumerate() {
    QVector<LanSegment> out;
    for (const auto &iface : QNetworkInterface::allInterfaces()) {
        if (!iface.flags().testFlag(QNetworkInterface::IsUp))
            continue;
        for (const auto &entry : iface.addressEntries()) {
            const QHostAddress addr = entry.ip();
            if (addr.protocol() != QAbstractSocket::IPv4Protocol)
                continue;
            LanSegment s;
            s.interfaceName = iface.name();
            s.ip = addr.toString();
            s.prefixLength = entry.prefixLength();
            if (s.isValid())
                out.append(s);
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const LanSegment &a, const LanSegment &b) {
        const int ra = segmentRank(a), rb = segmentRank(b);
        if (ra != rb)
            return ra < rb;
        if (a.interfaceName != b.interfaceName)
            return a.interfaceName < b.interfaceName;
        return a.ip < b.ip;
    });
    const LanSegment env = fromEnvironment();
    if (env.isValid())
        out.prepend(env);
    return out;
}

LanSegment preferred() {
    const LanSegment env = fromEnvironment();
    if (env.isValid())
        return env;

    const auto all = enumerate();
    for (const auto &s : all)
        if (segmentRank(s) == 0)
            return s;
    for (const auto &s : all)
        if (segmentRank(s) == 1)
            return s;
    if (!all.isEmpty())
        return all.first();

    LanSegment fallback;
    fallback.interfaceName = QStringLiteral("lo");
    fallback.ip = QStringLiteral("127.0.0.1");
    fallback.prefixLength = 8;
    return fallback;
}

} // namespace LanSegments
} // namespace meshlink
