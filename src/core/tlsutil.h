// deepin-MeshLink — TLS config + AUTH content helpers shared by the async
// channel (securelink.cpp) and the blocking worker client (downloader.cpp).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#pragma once

#include <QByteArray>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QString>

#include "crypto.h"

namespace meshlink {

inline QSslConfiguration makeMeshlinkTlsConfig(const QString &certPem, const QString &keyPem) {
    QSslConfiguration cfg;
    cfg.setProtocol(QSsl::TlsV1_3); // NFR-10: exactly TLS 1.3, no plaintext fallback
    cfg.setLocalCertificate(QSslCertificate(certPem.toUtf8(), QSsl::Pem));
    cfg.setPrivateKey(QSslKey(keyPem.toUtf8(), QSsl::Ec, QSsl::Pem, QSsl::PrivateKey));
    // self-signed without CA: application AUTH + TOFU provide authentication.
    cfg.setPeerVerifyMode(QSslSocket::VerifyNone);
    return cfg;
}

inline QString certFpHex(const QSslCertificate &c) {
    const QByteArray der = c.toDer();
    return QString::fromLatin1(Crypto::sha256Hex(reinterpret_cast<const uint8_t *>(der.constData()),
                                                 size_t(der.size())).c_str());
}

inline QString certCn(const QSslCertificate &c) {
    const auto names = c.subjectInfo(QSslCertificate::CommonName);
    return names.isEmpty() ? QString() : names.first();
}

// session-bound auth signature content (both peers can reconstruct it)
inline QByteArray makeAuthContent(const QString &fp, const QByteArray &nonce, const QString &peerCertFp) {
    QByteArray c = "ML-A1";
    c += fp.toLatin1();
    c += nonce;
    c += peerCertFp.toLatin1();
    return c;
}

} // namespace meshlink
