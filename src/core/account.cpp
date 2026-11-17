// deepin-MeshLink — account implementation.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "account.h"

#include "settings.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QHostInfo>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <unistd.h>

namespace meshlink {
namespace {

// Pack plaintext private material; AES-GCM-encrypted into account.keystore_blob.
Bytes packPrivate(const Identity &id, const TlsMaterial &tls) {
    Bytes out;
    auto append = [&out](const Bytes &b) { out.insert(out.end(), b.begin(), b.end()); };
    auto appendU32 = [&out](quint32 v) {
        out.push_back(uint8_t(v >> 24)); out.push_back(uint8_t(v >> 16));
        out.push_back(uint8_t(v >> 8));  out.push_back(uint8_t(v));
    };
    append(id.ed25519Priv);
    append(id.x25519Priv);
    const Bytes key(tls.keyPem.begin(), tls.keyPem.end());
    appendU32(static_cast<quint32>(key.size()));
    append(key);
    return out;
}

bool unpackPrivate(const Bytes &plain, Identity &id, std::string &tlsKeyPem) {
    if (plain.size() < 64 + 4)
        return false;
    id.ed25519Priv.assign(plain.begin(), plain.begin() + 32);
    id.x25519Priv.assign(plain.begin() + 32, plain.begin() + 64);
    auto rd = [&](size_t at) {
        return (uint32_t(plain[at]) << 24) | (uint32_t(plain[at + 1]) << 16) | (uint32_t(plain[at + 2]) << 8)
               | uint32_t(plain[at + 3]);
    };
    uint32_t n = rd(64);
    if (plain.size() < 68 + n)
        return false;
    tlsKeyPem.assign(plain.begin() + 68, plain.begin() + 68 + n);
    return true;
}

Bytes machineFactor() {
    QByteArray mid;
    QFile f("/etc/machine-id");
    if (f.open(QIODevice::ReadOnly))
        mid = f.readAll().trimmed();
    if (mid.isEmpty()) {
        QFile f2("/var/lib/dbus/machine-id");
        if (f2.open(QIODevice::ReadOnly))
            mid = f2.readAll().trimmed();
    }
    if (mid.isEmpty())
        mid = QHostInfo::localHostName().toUtf8();
    mid += QByteArray::number(static_cast<qint64>(::getuid()));
    return Bytes(mid.begin(), mid.end());
}

Bytes loadOrCreateSalt() {
    QFile f(Paths::keystoreSalt());
    if (f.open(QIODevice::ReadOnly)) {
        Bytes b;
        const QByteArray raw = f.readAll();
        b.assign(raw.begin(), raw.end());
        if (b.size() == 16)
            return b;
    }
    Bytes salt = Crypto::randomBytes(16);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(reinterpret_cast<const char *>(salt.data()), int(salt.size()));
        f.flush();
        QFile::setPermissions(Paths::keystoreSalt(), QFile::ReadOwner | QFile::WriteOwner);
    }
    return salt;
}

} // namespace

AccountManager::AccountManager(Storage *storage, QObject *parent) : QObject(parent), m_storage(storage) {}

bool AccountManager::hasAccount() {
    if (m_profile.valid)
        return true;
    QSqlQuery q = m_storage->query("SELECT COUNT(*) FROM account");
    q.exec();
    if (q.next())
        return q.value(0).toInt() > 0;
    return false;
}

QString AccountManager::defaultNickname() {
    return tr("deepin-MeshLink user_") + QString::fromLatin1(Crypto::randomHex(2).c_str()).toUpper();
}

bool AccountManager::validateNickname(const QString &nick, QString *why) {
    const int n = nick.trimmed().length();
    if (n < 2 || n > 32) {
        if (why)
            *why = tr("Nickname must be 2–32 characters long");
        return false;
    }
    return true;
}

Bytes AccountManager::kek() const { return Crypto::deriveKek(machineFactor(), loadOrCreateSalt()); }

QPixmap AccountManager::makeIdenticon(const QString &seed, int size) {
    QImage img(size, size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    const auto h = QCryptographicHash::hash(seed.toUtf8(), QCryptographicHash::Sha256);
    QColor base = QColor::fromHsv(quint8(h[0]) % 360, 160 + (quint8(h[1]) % 60), 190 + (quint8(h[2]) % 50));
    QPainterPath clip;
    clip.addEllipse(0, 0, size, size);
    p.setClipPath(clip);
    p.fillRect(0, 0, size, size, base.darker(140));
    p.setBrush(base.lighter(120));
    p.setPen(Qt::NoPen);
    const int grid = 5;
    const double cell = double(size) / grid;
    for (int y = 0; y < grid; ++y) {
        for (int x = 0; x < (grid + 1) / 2; ++x) {
            int idx = y * ((grid + 1) / 2) + x;
            if (quint8(h[3 + idx % (h.size() - 3)]) & 1) {
                double cx = x * cell + cell / 2, cy = y * cell + cell / 2;
                p.drawEllipse(QPointF(cx, cy), cell * 0.36, cell * 0.36);
                double mx = (grid - 1 - x) * cell + cell / 2; // mirror horizontally
                if (qAbs(mx - cx) > 0.01)
                    p.drawEllipse(QPointF(mx, cy), cell * 0.36, cell * 0.36);
            }
        }
    }
    p.end();
    return QPixmap::fromImage(img);
}

QVector<AccountEntry> AccountManager::listAccounts() const {
    QVector<AccountEntry> out;
    QSqlQuery q = m_storage->query(
        "SELECT a.id,a.fingerprint,a.nickname,a.created_ts,"
        " COALESCE(p.avatar_hash,''),COALESCE(p.avatar_path,'')"
        " FROM account a LEFT JOIN profile p ON p.account_id=a.id ORDER BY a.id");
    q.exec();
    while (q.next()) {
        AccountEntry e;
        e.id = q.value(0).toLongLong();
        e.fingerprint = q.value(1).toString();
        e.nickname = q.value(2).toString();
        e.createdTs = q.value(3).toLongLong();
        e.avatarHash = q.value(4).toString();
        e.avatarPath = q.value(5).toString();
        out.append(e);
    }
    return out;
}

qint64 AccountManager::createNewAccount(const QString &desiredNickname, QString *error) {
    Paths::ensureDirs();

    auto id = Crypto::generateIdentity();
    if (!id) {
        if (error)
            *error = tr("Key generation failed"); // E1
        return -1;
    }
    auto tls = Crypto::generateSelfSignedTls(id->fingerprint);
    if (!tls) {
        if (error)
            *error = tr("TLS certificate generation failed");
        return -1;
    }

    Bytes priv = packPrivate(*id, *tls);
    Bytes blob = Crypto::aesGcmEncrypt(priv, kek());
    if (blob.empty()) {
        if (error)
            *error = tr("Private key encryption failed");
        return -1;
    }

    m_profile = Profile{};
    m_profile.fingerprint = QString::fromStdString(id->fingerprint);
    m_profile.nickname = desiredNickname.trimmed().isEmpty() ? defaultNickname() : desiredNickname.trimmed();
    if (!validateNickname(m_profile.nickname, error))
        return -1;
    m_profile.ed25519Pub = QByteArray(reinterpret_cast<const char *>(id->ed25519Pub.data()), 32);
    m_profile.x25519Pub = QByteArray(reinterpret_cast<const char *>(id->x25519Pub.data()), 32);
    m_profile.tlsCertPem = QString::fromStdString(tls->certPem);
    m_profile.createdTs = nowMs();
    m_edPriv = id->ed25519Priv;
    m_tlsKeyPem = QString::fromStdString(tls->keyPem);
    m_profile.lanIp = Settings().lanIpAddress();

    QSqlQuery q = m_storage->query("INSERT INTO account (fingerprint,nickname,ed25519_pub,x25519_pub,keystore_blob,cert_pem,created_ts)"
                                   " VALUES (?,?,?,?,?,?,?)");
    q.bindValue(0, m_profile.fingerprint);
    q.bindValue(1, m_profile.nickname);
    q.bindValue(2, m_profile.ed25519Pub);
    q.bindValue(3, m_profile.x25519Pub);
    q.bindValue(4, QByteArray(reinterpret_cast<const char *>(blob.data()), int(blob.size())));
    q.bindValue(5, m_profile.tlsCertPem);
    q.bindValue(6, m_profile.createdTs);
    if (!q.exec()) {
        if (error)
            *error = tr("Could not write the account: %1").arg(q.lastError().text());
        return -1;
    }
    const qint64 rowId = q.lastInsertId().toLongLong();
    m_activeId = rowId;

    // default circular avatar derived from the fingerprint; multi-account
    // builds keep one avatar file per account (avatars/<id>.png).
    QDir().mkpath(Paths::avatarDir());
    m_profile.avatarPath = Paths::avatarDir() + QStringLiteral("/%1.png").arg(rowId);
    {
        QPixmap pm = makeIdenticon(m_profile.fingerprint);
        pm.save(m_profile.avatarPath, "PNG");
        QFile f(m_profile.avatarPath);
        if (f.open(QIODevice::ReadOnly)) {
            const QByteArray d = f.readAll();
            m_profile.avatarHash = QString::fromLatin1(Crypto::sha256Hex(reinterpret_cast<const uint8_t *>(d.data()), size_t(d.size())).c_str());
            // Publish to the blob cache: peers fetch avatars by hash (BR-06),
            // and the local chat view also reads avatars from this cache.
            QFile blob(Paths::blobDir() + "/" + m_profile.avatarHash);
            if (blob.open(QIODevice::WriteOnly))
                blob.write(d);
        }
    }

    QSqlQuery pq = m_storage->query("INSERT OR REPLACE INTO profile (account_id,avatar_path,avatar_hash,lan_ip) VALUES (?,?,?,?)");
    pq.bindValue(0, rowId);
    pq.bindValue(1, m_profile.avatarPath);
    pq.bindValue(2, m_profile.avatarHash);
    pq.bindValue(3, m_profile.lanIp);
    pq.exec();

    m_profile.valid = true;
    emit profileChanged(m_profile);
    return rowId;
}

bool AccountManager::loadAccount(qint64 id, QString *error) {
    if (m_profile.valid && m_activeId == id)
        return true;
    QSqlQuery q = m_storage->query("SELECT fingerprint,nickname,ed25519_pub,x25519_pub,keystore_blob,cert_pem,created_ts FROM account WHERE id=?");
    q.bindValue(0, id);
    q.exec(); // Storage::query() only prepares: execute before reading rows
    if (!q.next()) {
        if (error)
            *error = tr("No local account");
        return false;
    }
    Profile p;
    p.fingerprint = q.value(0).toString();
    p.nickname = q.value(1).toString();
    p.ed25519Pub = q.value(2).toByteArray();
    p.x25519Pub = q.value(3).toByteArray();
    const QByteArray blob = q.value(4).toByteArray();
    p.tlsCertPem = q.value(5).toString();
    p.createdTs = q.value(6).toLongLong();

    Bytes key(reinterpret_cast<const uint8_t *>(blob.constData()),
              reinterpret_cast<const uint8_t *>(blob.constData()) + blob.size());
    auto plain = Crypto::aesGcmDecrypt(key, kek());
    if (!plain) {
        if (error)
            *error = tr("Private key decryption failed (device or credentials changed)");
        return false;
    }
    Identity ident;
    std::string tlsKey;
    ident.ed25519Pub.assign(p.ed25519Pub.begin(), p.ed25519Pub.end());
    ident.x25519Pub.assign(p.x25519Pub.begin(), p.x25519Pub.end());
    if (!unpackPrivate(*plain, ident, tlsKey)) {
        if (error)
            *error = tr("Key material is corrupted");
        return false;
    }
    m_edPriv = ident.ed25519Priv;
    m_tlsKeyPem = QString::fromStdString(tlsKey);

    QSqlQuery pq = m_storage->query("SELECT avatar_path,avatar_hash,lan_ip FROM profile WHERE account_id=?");
    pq.bindValue(0, id);
    pq.exec();
    if (pq.next()) {
        p.avatarPath = pq.value(0).toString();
        p.avatarHash = pq.value(1).toString();
        p.lanIp = pq.value(2).toString();
    }
    p.valid = true;
    m_activeId = id;
    m_profile = p;
    ensureAvatarLocation(id);

    // Repair accounts created by older builds: the default avatar was saved
    // under avatars/current.png but never published to blobs/<hash>.
    if (!m_profile.avatarPath.isEmpty()) {
        QFile af(m_profile.avatarPath);
        if (af.open(QIODevice::ReadOnly)) {
            const QByteArray d = af.readAll();
            const QString hash = QString::fromLatin1(
                Crypto::sha256Hex(reinterpret_cast<const uint8_t *>(d.data()), size_t(d.size())).c_str());
            if (hash != m_profile.avatarHash) {
                m_profile.avatarHash = hash;
                QSqlQuery uq = m_storage->query("UPDATE profile SET avatar_hash=? WHERE account_id=?");
                uq.bindValue(0, m_profile.avatarHash);
                uq.bindValue(1, id);
                uq.exec();
            }
            QFile blob(Paths::blobDir() + "/" + m_profile.avatarHash);
            if (!blob.exists() && blob.open(QIODevice::WriteOnly))
                blob.write(d);
        }
    }

    emit profileChanged(m_profile);
    return true;
}

void AccountManager::setLanIp(const QString &ip) {
    if (m_profile.lanIp == ip)
        return;
    m_profile.lanIp = ip;
    QSqlQuery q = m_storage->query("UPDATE profile SET lan_ip=? WHERE account_id=?");
    q.bindValue(0, ip);
    q.bindValue(1, m_activeId);
    q.exec();
    emit profileChanged(m_profile);
}

bool AccountManager::updateProfile(const QString &nickname, const QString &avatarSourcePath, QString *error) {
    QString nn = nickname.isEmpty() ? m_profile.nickname : nickname.trimmed();
    if (!validateNickname(nn, error))
        return false;

    if (!avatarSourcePath.isEmpty()) {
        QImage src(avatarSourcePath);
        if (src.isNull()) {
            if (error)
                *error = tr("The avatar image could not be read");
            return false;
        }
        // centre-crop to square, resize to 256, keep circular via identicon clip path style.
        int side = qMin(src.width(), src.height());
        QImage cropped = src.copy((src.width() - side) / 2, (src.height() - side) / 2, side, side)
                             .scaled(256, 256, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                             .convertToFormat(QImage::Format_ARGB32_Premultiplied);
        QImage circular(256, 256, QImage::Format_ARGB32_Premultiplied);
        circular.fill(Qt::transparent);
        QPainter cp(&circular);
        cp.setRenderHint(QPainter::Antialiasing);
        QPainterPath path;
        path.addEllipse(0, 0, 256, 256);
        cp.setClipPath(path);
        cp.drawImage(0, 0, cropped);
        cp.end();
        QDir().mkpath(Paths::avatarDir());
        const QString dest = Paths::avatarDir() + QStringLiteral("/%1.png").arg(m_activeId);
        circular.save(dest, "PNG");
        QFile f(dest);
        if (f.open(QIODevice::ReadOnly)) {
            const QByteArray d = f.readAll();
            m_profile.avatarHash = QString::fromLatin1(
                Crypto::sha256Hex(reinterpret_cast<const uint8_t *>(d.data()), size_t(d.size())).c_str());
            m_profile.avatarPath = dest;
            // publish to blob store so peers can fetch it on demand (BR-06)
            QFile bf(Paths::blobDir() + "/" + m_profile.avatarHash);
            if (bf.open(QIODevice::WriteOnly))
                bf.write(d);
        }
    }

    m_profile.nickname = nn;
    QSqlQuery q = m_storage->query("UPDATE account SET nickname=? WHERE id=?");
    q.bindValue(0, nn);
    q.bindValue(1, m_activeId);
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    QSqlQuery pq = m_storage->query("UPDATE profile SET avatar_path=?,avatar_hash=? WHERE account_id=?");
    pq.bindValue(0, m_profile.avatarPath);
    pq.bindValue(1, m_profile.avatarHash);
    pq.bindValue(2, m_activeId);
    pq.exec();
    emit profileChanged(m_profile);
    return true;
}

void AccountManager::ensureAvatarLocation(qint64 id) {
    const QString expected = Paths::avatarDir() + QStringLiteral("/%1.png").arg(id);
    const QString legacy = Paths::avatarDir() + "/current.png";
    if (m_profile.avatarPath == expected)
        return;
    QDir().mkpath(Paths::avatarDir());
    // Migration: the single-account builds kept every avatar at current.png.
    if (!QFile::exists(expected) && QFile::exists(legacy) && m_profile.avatarPath == legacy) {
        if (QFile::rename(legacy, expected)) {
            m_profile.avatarPath = expected;
            QSqlQuery q = m_storage->query("UPDATE profile SET avatar_path=? WHERE account_id=?");
            q.bindValue(0, expected);
            q.bindValue(1, id);
            q.exec();
            return;
        }
    }
    if (QFile::exists(expected)) {
        m_profile.avatarPath = expected;
        QSqlQuery q = m_storage->query("UPDATE profile SET avatar_path=? WHERE account_id=?");
        q.bindValue(0, expected);
        q.bindValue(1, id);
        q.exec();
    }
}

int AccountManager::destroyAccount(bool wipeHistoryAndFiles, QString *error) {
    // Step 5: securely overwrite the keystore material.
    // Private key lives only inside account.keystore_blob, so we zero that row.
    const QString fingerprint = m_profile.fingerprint;
    QSqlQuery q = m_storage->query("UPDATE account SET keystore_blob=? WHERE id=?");
    q.bindValue(0, QByteArray(64, '\0'));
    q.bindValue(1, m_activeId);
    q.exec();
    // wipe the on-disk salt so the (already randomised) blob is unrecoverable
    QFile::remove(Paths::keystoreSalt());

    // Step 6/5: remove identity + (optionally) session data.
    if (!m_storage->wipeAccount(fingerprint, wipeHistoryAndFiles, error))
        return -1;

    // remove the avatar cache of the destroyed account
    if (!m_profile.avatarPath.isEmpty())
        QFile::remove(m_profile.avatarPath);

    m_profile = Profile{};
    m_activeId = 0;
    m_edPriv.clear();
    m_tlsKeyPem.clear();
    return listAccounts().size();
}

bool AccountManager::deleteAccount(qint64 id, bool wipeHistoryAndFiles, QString *error) {
    QSqlQuery q = m_storage->query("SELECT fingerprint FROM account WHERE id=?");
    q.bindValue(0, id);
    q.exec();
    if (!q.next()) {
        if (error)
            *error = tr("No local account");
        return false;
    }
    const QString fingerprint = q.value(0).toString();

    QSqlQuery zero = m_storage->query("UPDATE account SET keystore_blob=? WHERE id=?");
    zero.bindValue(0, QByteArray(64, '\0'));
    zero.bindValue(1, id);
    zero.exec();

    QString avatarPath;
    QSqlQuery pq = m_storage->query("SELECT avatar_path FROM profile WHERE account_id=?");
    pq.bindValue(0, id);
    pq.exec();
    if (pq.next())
        avatarPath = pq.value(0).toString();

    if (!m_storage->wipeAccount(fingerprint, wipeHistoryAndFiles, error))
        return false;
    if (!avatarPath.isEmpty())
        QFile::remove(avatarPath);
    if (m_activeId == id) {
        m_profile = Profile{};
        m_activeId = 0;
        m_edPriv.clear();
        m_tlsKeyPem.clear();
    }
    return true;
}

bool AccountManager::sign(const QByteArray &msg, QByteArray *sigOut) const {
    if (m_edPriv.empty())
        return false;
    Bytes sig;
    if (!Crypto::ed25519Sign(m_edPriv, Bytes(msg.begin(), msg.end()), sig))
        return false;
    *sigOut = QByteArray(reinterpret_cast<const char *>(sig.data()), int(sig.size()));
    return true;
}

bool AccountManager::verify(const QByteArray &ed25519Pub, const QByteArray &msg, const QByteArray &sig) {
    if (ed25519Pub.size() != 32 || sig.size() != 64)
        return false;
    return Crypto::ed25519Verify(Bytes(ed25519Pub.begin(), ed25519Pub.end()),
                                 Bytes(msg.begin(), msg.end()), Bytes(sig.begin(), sig.end()));
}

} // namespace meshlink
