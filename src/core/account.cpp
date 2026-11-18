// deepin-MeshLink — account implementation.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "account.h"

#include "settings.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostInfo>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace meshlink {
namespace {

constexpr size_t kKeystoreDeviceKeyBytes = 32;
constexpr size_t kKeystoreSaltBytes = 16;

enum class SecretIo { Ok, Missing, Invalid, Error };

// Read a small secret file without creating or changing it. `Invalid` means
// the file exists but is unreadable or has an unexpected size: callers must not
// replace it, otherwise existing encrypted blobs would lose their KEK.
SecretIo readSecretFile(const QByteArray &nativePath, size_t expected, Bytes &out) {
    int fd = ::open(nativePath.constData(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return errno == ENOENT ? SecretIo::Missing : SecretIo::Error;
    uint8_t buf[64];
    size_t total = 0;
    bool tooLong = false;
    for (;;) {
        const ssize_t n = ::read(fd, buf + total, sizeof(buf) - total);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            ::close(fd);
            return SecretIo::Error;
        }
        if (n == 0)
            break;
        total += size_t(n);
        if (total > expected) {
            tooLong = true;
            break;
        }
    }
    ::close(fd);
    if (tooLong || total != expected)
        return SecretIo::Invalid;
    out.assign(buf, buf + total);
    return SecretIo::Ok;
}

bool ensureKeystoreDir() {
    const QString dir = QFileInfo(Paths::keystoreDeviceKey()).absolutePath();
    if (!QDir().mkpath(dir))
        return false;
    // Secrets inside are 0600; keep the containing directory owner-only too.
    ::chmod(QFile::encodeName(dir).constData(), S_IRWXU);
    return true;
}

// Create a secret exactly once. O_EXCL makes sure an existing key is never
// overwritten, even when two processes start concurrently; if another process
// wins the race we read its value instead of replacing it. `created` reports
// whether this call actually wrote the file (false when an existing one was
// adopted after the race).
Bytes createSecretFileExclusive(const QByteArray &nativePath, size_t size, bool *created = nullptr) {
    if (created)
        *created = false;
    Bytes value = Crypto::randomBytes(size);
    int fd = ::open(nativePath.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        if (errno == EEXIST) {
            Bytes existing;
            return readSecretFile(nativePath, size, existing) == SecretIo::Ok ? existing : Bytes{};
        }
        return {};
    }
    size_t written = 0;
    while (written < value.size()) {
        const ssize_t n = ::write(fd, value.data() + written, value.size() - written);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break;
        written += size_t(n);
    }
    ::fchmod(fd, S_IRUSR | S_IWUSR); // 0600, independent of the process umask
    if (written == value.size())
        ::fsync(fd);
    ::close(fd);
    if (written != value.size()) {
        ::unlink(nativePath.constData());
        return {};
    }
    if (created)
        *created = true;
    return value;
}

QString keystoreDirPath() { return QFileInfo(Paths::keystoreDeviceKey()).absolutePath(); }

QString deviceKeyTagPath() { return Paths::keystoreDeviceKey() + QStringLiteral(".tag"); }

// Retained copy of the last legacy-format ciphertext of an account. It is only
// a fallback recovery path: the live row is still the source of truth.
QString legacyKeystoreDirPath() { return keystoreDirPath() + QStringLiteral("/legacy"); }

QString legacyKeystorePath(qint64 id) {
    return legacyKeystoreDirPath() + QStringLiteral("/%1.blob").arg(id);
}

constexpr char kDeviceKeyTagContext[] = "meshlink.keystore.device-key.v1";

Bytes deviceKeyTagFor(const Bytes &key) {
    const QByteArray context(kDeviceKeyTagContext);
    return Crypto::hmacSha256(key, Bytes(context.begin(), context.end()));
}

bool fileExists(const QByteArray &native) { return ::access(native.constData(), F_OK) == 0; }

bool ensureOwnerOnlyDir(const QString &path) {
    if (!QDir().mkpath(path))
        return false;
    ::chmod(QFile::encodeName(path).constData(), S_IRWXU);
    return true;
}

// Write a secret atomically (temp file + rename) with 0600 permissions and
// fsync both the file and its parent directory. `exclusive` writes the final
// path with O_EXCL and never replaces an existing file (used for the tag, which
// must stay immutable once it exists); the default overwrites atomically (used
// for the retained legacy ciphertext).
bool writeSecretFileAtomic(const QString &path, const Bytes &data, QString *error,
                           bool exclusive = false) {
    if (data.empty()) {
        if (error)
            *error = QStringLiteral("empty secret");
        return false;
    }
    const QByteArray native = QFile::encodeName(path);
    const QByteArray tmp = exclusive
                               ? native
                               : native + QByteArrayLiteral(".tmp.")
                                             + QByteArray::number(static_cast<qlonglong>(::getpid()));
    if (!exclusive)
        ::unlink(tmp.constData());
    int fd = ::open(tmp.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        if (error)
            *error = errno == EEXIST ? QStringLiteral("secret already exists")
                                     : QStringLiteral("cannot create secret file");
        return false;
    }
    size_t written = 0;
    while (written < data.size()) {
        const ssize_t n = ::write(fd, data.data() + written, data.size() - written);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break;
        written += size_t(n);
    }
    ::fchmod(fd, S_IRUSR | S_IWUSR);
    if (written == data.size())
        ::fsync(fd);
    ::close(fd);
    if (written != data.size()) {
        ::unlink(tmp.constData());
        if (error)
            *error = QStringLiteral("cannot write secret file");
        return false;
    }
    if (!exclusive && ::rename(tmp.constData(), native.constData()) != 0) {
        ::unlink(tmp.constData());
        if (error)
            *error = QStringLiteral("cannot replace secret file");
        return false;
    }
    ::chmod(native.constData(), S_IRUSR | S_IWUSR);
    const QByteArray parent = QFile::encodeName(QFileInfo(path).absolutePath());
    int dfd = ::open(parent.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        ::fsync(dfd);
        ::close(dfd);
    }
    return true;
}

bool writeDeviceKeyTag(const Bytes &key) {
    if (key.size() != kKeystoreDeviceKeyBytes)
        return false;
    return writeSecretFileAtomic(deviceKeyTagPath(), deviceKeyTagFor(key), nullptr, true);
}

// Per-install random device secret: the stable factor of the current KEK.
// Created once (O_EXCL) and never overwritten. A missing key with a surviving
// tag is an ambiguous partial restore: no replacement key is generated and the
// tag is left untouched, so the original key can still be restored. Existing
// keys never gain provenance from the co-located tag alone; loadAccount()
// requires proof tied to this account database (a device-key-encrypted row) or
// the fresh first-upgrade case where both key and tag were absent.
struct DeviceSecretState {
    Bytes key;
    bool createdThisProcess = false;
    bool provenanceVerified = false;
};

DeviceSecretState loadOrCreateDeviceKeyState() {
    DeviceSecretState state;
    if (!ensureKeystoreDir())
        return state;
    const QByteArray native = QFile::encodeName(Paths::keystoreDeviceKey());
    switch (readSecretFile(native, kKeystoreDeviceKeyBytes, state.key)) {
    case SecretIo::Ok:
        ::chmod(native.constData(), S_IRUSR | S_IWUSR); // tighten old permissions
        return state;
    case SecretIo::Missing: {
        // A stale tag proves a previous key existed. Do not overwrite it and do
        // not bless a replacement: keep the legacy keystore read-only until the
        // intended key is restored.
        if (fileExists(QFile::encodeName(deviceKeyTagPath()))) {
            qWarning("meshlink: keystore device secret is missing but its tag remains; "
                     "refusing to create a replacement key and leaving the tag untouched");
            return state;
        }
        bool created = false;
        state.key = createSecretFileExclusive(native, kKeystoreDeviceKeyBytes, &created);
        state.createdThisProcess = created;
        if (state.key.size() == kKeystoreDeviceKeyBytes && created) {
            // Fresh first upgrade: both key and tag were absent, so this key is
            // the intended device key for the database being created.
            state.provenanceVerified = writeDeviceKeyTag(state.key);
            if (!state.provenanceVerified)
                qWarning("meshlink: created the keystore device secret but could not persist its tag; "
                         "legacy keystore migration stays disabled");
        }
        return state;
    }
    case SecretIo::Invalid:
        qWarning("meshlink: keystore device secret %s has an unexpected size; refusing to replace it",
                 qUtf8Printable(Paths::keystoreDeviceKey()));
        return state;
    case SecretIo::Error:
        qWarning("meshlink: cannot read the keystore device secret %s",
                 qUtf8Printable(Paths::keystoreDeviceKey()));
        return state;
    }
    return state;
}

Bytes loadOrCreateDeviceKey() { return loadOrCreateDeviceKeyState().key; }

bool retainLegacyKeystoreBlob(qint64 id, const QByteArray &blob, QString *error) {
    if (blob.isEmpty()) {
        if (error)
            *error = QStringLiteral("empty legacy blob");
        return false;
    }
    if (!ensureOwnerOnlyDir(legacyKeystoreDirPath())) {
        if (error)
            *error = QStringLiteral("cannot create legacy keystore directory");
        return false;
    }
    const Bytes cipher(reinterpret_cast<const uint8_t *>(blob.constData()),
                       reinterpret_cast<const uint8_t *>(blob.constData()) + blob.size());
    return writeSecretFileAtomic(legacyKeystorePath(id), cipher, error);
}

Bytes loadOrCreateSalt() {
    if (!ensureKeystoreDir())
        return {};
    const QByteArray native = QFile::encodeName(Paths::keystoreSalt());
    Bytes salt;
    switch (readSecretFile(native, kKeystoreSaltBytes, salt)) {
    case SecretIo::Ok:
        ::chmod(native.constData(), S_IRUSR | S_IWUSR);
        return salt;
    case SecretIo::Missing:
        return createSecretFileExclusive(native, kKeystoreSaltBytes);
    default:
        qWarning("meshlink: keystore salt %s is unreadable; refusing to replace it",
                 qUtf8Printable(Paths::keystoreSalt()));
        return {};
    }
}

QByteArray readMachineIdFile(const char *path) {
    QFile f(QString::fromLatin1(path));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll().trimmed();
}

// Legacy machine-bound factors exactly as the pre-device-key build derived
// them: machine-id, the dbus machine-id copy, or the hostname, each followed
// by the login uid. Kept only to migrate old blobs; never used to encrypt.
// MESHLINK_LEGACY_MACHINE_FACTOR is a test-only override that simulates a
// machine-id/hostname change deterministically; it is ignored in normal runs
// and never participates in the new device-key derivation.
QVector<Bytes> legacyMachineFactors() {
    QVector<QByteArray> identities;
    const QByteArray override = qgetenv("MESHLINK_LEGACY_MACHINE_FACTOR");
    if (!override.isEmpty()) {
        identities << override;
    } else {
        for (const char *path : {"/etc/machine-id", "/var/lib/dbus/machine-id"}) {
            const QByteArray id = readMachineIdFile(path);
            if (!id.isEmpty() && !identities.contains(id))
                identities << id;
        }
        const QByteArray host = QHostInfo::localHostName().toUtf8();
        if (!host.isEmpty() && !identities.contains(host))
            identities << host;
    }
    const QByteArray uid = QByteArray::number(static_cast<qint64>(::getuid()));
    QVector<Bytes> factors;
    for (const QByteArray &identity : identities) {
        const QByteArray raw = identity + uid;
        factors.append(Bytes(raw.begin(), raw.end()));
    }
    return factors;
}

// Checked account count used after a destroy. Both exec() and next() must
// succeed; a failed query must never be mistaken for "no accounts left"
// (which would delete the install-wide keystore secrets). The environment
// override is test-only fault injection for the failure path.
bool countAccountsChecked(Storage *storage, int *count, QString *why) {
    if (qEnvironmentVariableIsSet("MESHLINK_TEST_FAIL_ACCOUNT_COUNT")) {
        if (why)
            *why = QStringLiteral("injected COUNT(*) failure");
        return false;
    }
    QSqlQuery q = storage->query("SELECT COUNT(*) FROM account");
    if (!q.exec()) {
        if (why)
            *why = q.lastError().text();
        return false;
    }
    if (!q.next()) {
        if (why)
            *why = QStringLiteral("COUNT(*) returned no row");
        return false;
    }
    *count = q.value(0).toInt();
    return true;
}

// Independent provenance for an existing device key: at least one *other*
// account row in this database is already encrypted with it. AES-GCM
// authentication makes a successful decrypt proof that this database was
// written with the key; the co-located tag alone cannot provide that.
bool databaseHasDeviceKeyProof(Storage *storage, qint64 skipId, const Bytes &kek) {
    if (kek.size() != kKeystoreDeviceKeyBytes)
        return false;
    QSqlQuery q = storage->query("SELECT id,keystore_blob FROM account WHERE id<>?");
    q.bindValue(0, skipId);
    if (!q.exec()) {
        qWarning("meshlink: could not inspect account blobs for device-key provenance: %s",
                 qUtf8Printable(q.lastError().text()));
        return false;
    }
    while (q.next()) {
        const QByteArray blob = q.value(1).toByteArray();
        if (blob.size() <= 16 + 12 + 16) // shorter than salt||iv||tag
            continue;
        const Bytes cipher(reinterpret_cast<const uint8_t *>(blob.constData()),
                           reinterpret_cast<const uint8_t *>(blob.constData()) + blob.size());
        if (Crypto::aesGcmDecrypt(cipher, kek))
            return true;
    }
    return false;
}

QByteArray readRetainedLegacyBlob(qint64 id) {
    QFile f(legacyKeystorePath(id));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    if (f.size() > 4 * 1024 * 1024) // account blobs are tiny; refuse junk
        return {};
    return f.readAll();
}

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

Bytes AccountManager::kek() const {
    // Stable per-install factor + install salt. No machine-id, hostname, MAC or
    // IP is involved: moving the data directory to the same machine or changing
    // network identity cannot invalidate the keystore.
    const Bytes salt = loadOrCreateSalt();
    if (salt.size() != kKeystoreSaltBytes)
        return {};
    const Bytes deviceSecret = loadOrCreateDeviceKey();
    if (deviceSecret.size() != kKeystoreDeviceKeyBytes)
        return {};
    return Crypto::deriveKek(deviceSecret, salt);
}

bool AccountManager::rewriteKeystoreBlob(qint64 id, const Bytes &blob, QString *error) {
    QSqlQuery q = m_storage->query("UPDATE account SET keystore_blob=? WHERE id=?");
    q.bindValue(0, QByteArray(reinterpret_cast<const char *>(blob.data()), int(blob.size())));
    q.bindValue(1, id);
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    return true;
}

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

    // New blobs may only ever be encrypted with the device-secret KEK. If the
    // local keystore cannot be prepared, fail instead of falling back to a
    // machine-bound key.
    const Bytes newKek = kek();
    if (newKek.size() != 32) {
        if (error)
            *error = tr("Private key encryption failed");
        return -1;
    }

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
    Bytes blob = Crypto::aesGcmEncrypt(priv, newKek);
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
    const Bytes salt = loadOrCreateSalt();
    const DeviceSecretState device = loadOrCreateDeviceKeyState();
    Bytes newKek;
    if (salt.size() == kKeystoreSaltBytes && device.key.size() == kKeystoreDeviceKeyBytes)
        newKek = Crypto::deriveKek(device.key, salt);

    std::optional<Bytes> plain;
    // The only non-database provenance is a genuinely fresh first upgrade
    // (both device.key and its tag were absent). For every existing key,
    // provenance comes from decrypting a device-key blob tied to this database.
    bool deviceKeyProven = device.provenanceVerified;
    if (!newKek.empty()) {
        plain = Crypto::aesGcmDecrypt(key, newKek);
        if (plain)
            deviceKeyProven = true; // this row itself is device-key bound
    }

    // Migration fallback 1: keystores written before the per-install device
    // secret used machine-id/hostname + uid as the KDF factor. Try every legacy
    // factor this machine can still produce.
    bool legacyFromRow = false;
    bool legacyFromCopy = false;
    if (!plain && salt.size() == kKeystoreSaltBytes) {
        for (const Bytes &factor : legacyMachineFactors()) {
            auto candidate = Crypto::aesGcmDecrypt(key, Crypto::deriveKek(factor, salt));
            if (candidate) {
                plain = candidate;
                legacyFromRow = true;
                break;
            }
        }
    }

    // Migration fallback 2: the retained ciphertext of the last legacy blob.
    // This keeps the account readable if the live row was already migrated but
    // its device key was later lost, rotated or replaced by a wrong one.
    if (!plain && salt.size() == kKeystoreSaltBytes) {
        const QByteArray retained = readRetainedLegacyBlob(id);
        if (!retained.isEmpty() && retained != blob) {
            const Bytes retainedCipher(reinterpret_cast<const uint8_t *>(retained.constData()),
                                       reinterpret_cast<const uint8_t *>(retained.constData())
                                           + retained.size());
            for (const Bytes &factor : legacyMachineFactors()) {
                auto candidate = Crypto::aesGcmDecrypt(retainedCipher,
                                                       Crypto::deriveKek(factor, salt));
                if (candidate) {
                    plain = candidate;
                    legacyFromCopy = true;
                    break;
                }
            }
        }
    }

    if (!plain) {
        // Keep the historical, non-destructive error: no crash, no wipe.
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
    if (legacyFromRow && !deviceKeyProven && !newKek.empty()) {
        // The current row is legacy; require another device-key-encrypted row
        // in this exact database before rewriting the only legacy ciphertext.
        deviceKeyProven = databaseHasDeviceKeyProof(m_storage, id, newKek);
    }
    if (legacyFromRow) {
        if (newKek.empty()) {
            qWarning("meshlink: legacy keystore of account %lld could not be migrated: "
                     "the per-install device secret is unavailable",
                     static_cast<long long>(id));
        } else if (!deviceKeyProven) {
            qWarning("meshlink: legacy keystore of account %lld was left unchanged: the current "
                     "device secret is not proven by any device-key-encrypted account in this database",
                     static_cast<long long>(id));
        } else {
            // Keep a recoverable copy of the only legacy ciphertext before
            // replacing the row. If retaining fails, do not rewrite anything.
            QString retainError;
            if (!retainLegacyKeystoreBlob(id, blob, &retainError)) {
                qWarning("meshlink: legacy keystore of account %lld was left unchanged because its "
                         "recovery copy could not be retained: %s",
                         static_cast<long long>(id), qUtf8Printable(retainError));
            } else {
                Bytes reencrypted = Crypto::aesGcmEncrypt(*plain, newKek);
                QString migrationError;
                if (reencrypted.empty() || !rewriteKeystoreBlob(id, reencrypted, &migrationError)) {
                    qWarning("meshlink: could not migrate the legacy keystore of account %lld: %s",
                             static_cast<long long>(id),
                             reencrypted.empty() ? "encryption failed"
                                                 : qUtf8Printable(migrationError));
                }
            }
        }
    } else if (legacyFromCopy) {
        // The stored row stays untouched: it may be valid for the intended key
        // and must not be replaced by a copy made with the current one.
        qWarning("meshlink: account %lld was loaded from its retained legacy keystore copy; "
                 "the stored row was left unchanged",
                 static_cast<long long>(id));
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
    if (m_activeId <= 0 || m_profile.fingerprint.isEmpty()) {
        if (error)
            *error = tr("No local account");
        return -1;
    }
    return destroyAccountInternal(m_activeId, m_profile.fingerprint, m_profile.avatarPath, true,
                                  wipeHistoryAndFiles, error);
}

int AccountManager::destroyAccountById(qint64 id, bool wipeHistoryAndFiles, QString *error) {
    if (id <= 0) {
        if (error)
            *error = tr("No local account with id %1").arg(id);
        return -1;
    }
    QSqlQuery lookup = m_storage->query("SELECT fingerprint FROM account WHERE id=?");
    lookup.bindValue(0, id);
    if (!lookup.exec() || !lookup.next()) {
        if (error)
            *error = tr("No local account with id %1").arg(id);
        return -1;
    }
    const QString fingerprint = lookup.value(0).toString();

    QString avatarPath;
    QSqlQuery pq = m_storage->query("SELECT avatar_path FROM profile WHERE account_id=?");
    pq.bindValue(0, id);
    if (pq.exec() && pq.next())
        avatarPath = pq.value(0).toString();

    return destroyAccountInternal(id, fingerprint, avatarPath, id == m_activeId,
                                  wipeHistoryAndFiles, error);
}

int AccountManager::destroyAccountInternal(qint64 id, const QString &fingerprint,
                                           const QString &avatarPath, bool active,
                                           bool wipeHistoryAndFiles, QString *error) {
    // Step 5: securely overwrite the keystore material.
    // Private key lives only inside account.keystore_blob, so we zero that row.
    QSqlQuery q = m_storage->query("UPDATE account SET keystore_blob=? WHERE id=?");
    q.bindValue(0, QByteArray(64, '\0'));
    q.bindValue(1, id);
    q.exec();

    // Step 6/5: remove identity + (optionally) session data.
    if (!m_storage->wipeAccount(fingerprint, wipeHistoryAndFiles, error))
        return -1;

    // remove the avatar cache and the retained legacy ciphertext of the
    // destroyed account (the latter is only a recovery fallback)
    if (!avatarPath.isEmpty())
        QFile::remove(avatarPath);
    QFile::remove(legacyKeystorePath(id));

    if (active) {
        m_profile = Profile{};
        m_activeId = 0;
        m_edPriv.clear();
        m_tlsKeyPem.clear();
    }

    // The install-wide salt and device secret are shared by every local
    // account: remove them only once the last account is gone. Deleting them
    // earlier would make the remaining accounts undecryptable. Both exec() and
    // next() must succeed; a failed count is an error, never "zero left".
    int remaining = 0;
    QString countError;
    if (!countAccountsChecked(m_storage, &remaining, &countError)) {
        qWarning("meshlink: could not verify the remaining accounts after destroying %s: %s; "
                 "keeping the install-wide keystore secrets",
                 qUtf8Printable(fingerprint.left(8)), qUtf8Printable(countError));
        if (error)
            *error = tr("The account was removed, but the remaining accounts could not be verified; "
                        "the local keystore was preserved");
        return -1;
    }
    if (remaining == 0) {
        QFile::remove(Paths::keystoreSalt());
        QFile::remove(Paths::keystoreDeviceKey());
        QFile::remove(deviceKeyTagPath());
        if (!QDir(legacyKeystoreDirPath()).removeRecursively()
            && QDir(legacyKeystoreDirPath()).exists()) {
            qWarning("meshlink: could not remove the retained legacy keystore directory %s",
                     qUtf8Printable(legacyKeystoreDirPath()));
        }
    }
    return remaining;
}

bool AccountManager::deleteAccount(qint64 id, bool wipeHistoryAndFiles, QString *error) {
    return destroyAccountById(id, wipeHistoryAndFiles, error) >= 0;
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
