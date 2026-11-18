// deepin-MeshLink — independent keystore hardening regression tests.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
//
// Covers the two keystore defects found during review of the device-independent
// credentials fix:
//   F1: a rotated/valid-but-wrong 32-byte device.key must not be able to
//       rewrite a legacy machine-id blob through the legacy fallback. Legacy
//       fallback is read-only until the device key is proven by a new-format
//       account, and normal migration must still rewrite when it is proven.
//   F2: destroying one of several accounts must never delete the install-wide
//       salt/device.key, including when the destruction fails.
//
// This file is verifier-owned; it intentionally does not modify tst_logic.cpp.

#include <QFile>
#include <QGuiApplication>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>
#include <unistd.h>

#include "core/account.h"
#include "core/crypto.h"
#include "core/settings.h"
#include "core/storage.h"

using namespace meshlink;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const QString &what) {
    ++g_checks;
    if (ok) {
        std::printf("check: PASS  %s\n", qUtf8Printable(what));
        return;
    }
    ++g_failures;
    std::fprintf(stderr, "check: FAIL  %s\n", qUtf8Printable(what));
}

QByteArray rawBytes(const Bytes &bytes) {
    return QByteArray(reinterpret_cast<const char *>(bytes.data()), int(bytes.size()));
}

Bytes bytesOf(const QByteArray &bytes) {
    return Bytes(reinterpret_cast<const uint8_t *>(bytes.constData()),
                 reinterpret_cast<const uint8_t *>(bytes.constData()) + bytes.size());
}

QByteArray readFileBytes(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

bool writeFileBytes(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const bool ok = file.write(bytes) == bytes.size();
    file.close();
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return ok;
}

QString deviceKeyTagPath() {
    return Paths::keystoreDeviceKey() + QStringLiteral(".tag");
}

QString legacyKeystoreDir() {
    return Paths::dataRoot() + QStringLiteral("/keystore/legacy");
}

QString legacyKeystoreCopy(qint64 id) {
    return legacyKeystoreDir() + QStringLiteral("/%1.blob").arg(id);
}

QByteArray deviceKeyTagFor(const QByteArray &key) {
    const QByteArray context("meshlink.keystore.device-key.v1");
    return rawBytes(Crypto::hmacSha256(bytesOf(key), bytesOf(context)));
}

QByteArray accountBlob(Storage &storage, qint64 id) {
    QSqlQuery query = storage.query(QStringLiteral("SELECT keystore_blob FROM account WHERE id=?"));
    query.bindValue(0, id);
    if (!query.exec() || !query.next())
        return {};
    return query.value(0).toByteArray();
}

// Historical plaintext layout used by the pre-device-key build.
QByteArray packLegacyPrivate(const Identity &identity, const TlsMaterial &tls) {
    QByteArray out;
    out.append(rawBytes(identity.ed25519Priv));
    out.append(rawBytes(identity.x25519Priv));
    const quint32 length = quint32(tls.keyPem.size());
    out.append(char(quint8(length >> 24)));
    out.append(char(quint8(length >> 16)));
    out.append(char(quint8(length >> 8)));
    out.append(char(quint8(length)));
    out.append(tls.keyPem.data(), int(tls.keyPem.size()));
    return out;
}

QByteArray legacyIdentity() {
    return QByteArrayLiteral("keystore-guard-machine-id");
}

QByteArray legacyFactor() {
    return legacyIdentity() + QByteArray::number(static_cast<qint64>(::getuid()));
}

bool installLegacyRow(Storage &storage, AccountManager &accounts, qint64 row,
                      const QByteArray &salt, const QByteArray &factor,
                      Identity *identityOut = nullptr) {
    if (row <= 0)
        return false;
    const auto identity = Crypto::generateIdentity();
    const auto tls = Crypto::generateSelfSignedTls(identity->fingerprint);
    if (!identity || !tls)
        return false;
    const Bytes blob = Crypto::aesGcmEncrypt(bytesOf(packLegacyPrivate(*identity, *tls)),
                                             Crypto::deriveKek(bytesOf(factor), bytesOf(salt)));
    if (blob.empty())
        return false;
    QSqlQuery update = storage.query(
        QStringLiteral("UPDATE account SET fingerprint=?,ed25519_pub=?,x25519_pub=?,"
                       "keystore_blob=?,cert_pem=? WHERE id=?"));
    update.bindValue(0, QString::fromStdString(identity->fingerprint));
    update.bindValue(1, rawBytes(identity->ed25519Pub));
    update.bindValue(2, rawBytes(identity->x25519Pub));
    update.bindValue(3, rawBytes(blob));
    update.bindValue(4, QString::fromStdString(tls->certPem));
    update.bindValue(5, row);
    if (!update.exec())
        return false;
    if (identityOut)
        *identityOut = *identity;
    return true;
}

void configureDataHome(const QString &root) {
    qputenv("MESHLINK_DATA_HOME", (root + QStringLiteral("/data")).toUtf8());
    qputenv("MESHLINK_CONFIG_HOME", (root + QStringLiteral("/config")).toUtf8());
    qputenv("MESHLINK_LEGACY_MACHINE_FACTOR", legacyIdentity());
}

bool forceDestroyFailure(Storage &storage, AccountManager &accounts, const QString &dbPath,
                         qint64 survivor, QString *errorOut) {
    // Hold an exclusive SQLite write transaction so destroyAccount's UPDATE and
    // wipeAccount() transaction fail. A failed destruction must preserve the
    // install-wide salt/device.key for the surviving account.
    {
        QSqlQuery pragma = storage.db().exec(QStringLiteral("PRAGMA busy_timeout=250"));
        Q_UNUSED(pragma);
    }
    QSqlDatabase lock = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                  QStringLiteral("keystore-guard-lock"));
    lock.setDatabaseName(dbPath);
    if (!lock.open())
        return false;
    bool began = false;
    {
        QSqlQuery pragma(lock);
        pragma.exec(QStringLiteral("PRAGMA busy_timeout=250"));
        QSqlQuery begin(lock);
        began = begin.exec(QStringLiteral("BEGIN EXCLUSIVE"));
    }
    QString error;
    const int remaining = accounts.destroyAccount(false, &error);
    if (errorOut)
        *errorOut = error;
    {
        QSqlQuery rollback(lock);
        rollback.exec(QStringLiteral("ROLLBACK"));
    }
    lock.close();
    lock = QSqlDatabase();
    QSqlDatabase::removeDatabase(QStringLiteral("keystore-guard-lock"));

    const bool failed = !began || remaining < 0;
    accounts.resetLoaded();
    QString reloadError;
    const bool survivorStillLoads = accounts.loadAccount(survivor, &reloadError);
    check(failed, QStringLiteral("destroyAccount reports failure while the database is locked "
                                 "(remaining=%1)").arg(remaining));
    check(QFile::exists(Paths::keystoreSalt()) && QFile::exists(Paths::keystoreDeviceKey())
              && QFile::exists(deviceKeyTagPath()),
          "failed destruction preserved the shared salt, device secret and tag");
    check(survivorStillLoads,
          QStringLiteral("surviving account still decrypts after a failed destruction (%1)")
              .arg(reloadError));
    return survivorStillLoads;
}

} // namespace

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("MESHLINK_IP", "127.0.0.1");

    // ---------------------------------------------------------------- F1 ----
    {
        QTemporaryDir home;
        check(home.isValid(), "F1: temporary data directory created");
        configureDataHome(home.path());
        const QString dbPath = home.filePath(QStringLiteral("guard-f1.db"));
        Storage storage;
        QString error;
        check(storage.open(dbPath, &error),
              QStringLiteral("F1: database opened (%1)").arg(error));
        AccountManager accounts(&storage);

        // A new-format account under the original device key proves that key.
        const qint64 anchor = accounts.createNewAccount(QStringLiteral("Anchor"), &error);
        check(anchor > 0, QStringLiteral("F1: anchor account created (%1)").arg(error));
        const QByteArray trustedKey = readFileBytes(Paths::keystoreDeviceKey());
        const QByteArray salt = readFileBytes(Paths::keystoreSalt());
        const QByteArray trustedTag = readFileBytes(deviceKeyTagPath());
        check(trustedKey.size() == 32 && salt.size() == 16,
              "F1: trusted device secret and salt have the expected sizes");
        check(trustedTag.size() == 32,
              "F1: a created device secret writes its provenance tag (32 bytes)");

        const qint64 migrateRow = accounts.createNewAccount(QStringLiteral("Migrate"), &error);
        const qint64 wrongKeyRow = accounts.createNewAccount(QStringLiteral("WrongKey"), &error);
        check(migrateRow > 0 && wrongKeyRow > 0,
              QStringLiteral("F1: legacy fixture account rows created (%1)").arg(error));
        check(installLegacyRow(storage, accounts, migrateRow, salt, legacyFactor()),
              "F1: migration fixture has a legacy machine-id blob");
        check(installLegacyRow(storage, accounts, wrongKeyRow, salt, legacyFactor()),
              "F1: wrong-key fixture has a legacy machine-id blob");
        const QByteArray originalMigrateBlob = accountBlob(storage, migrateRow);
        const QByteArray originalWrongKeyBlob = accountBlob(storage, wrongKeyRow);
        check(originalMigrateBlob.size() > 0 && originalWrongKeyBlob.size() > 0,
              "F1: legacy blobs are stored");

        // (c) Normal migration with the proven device key: legacy blobs are
        // re-encrypted and the old machine-id blob is replaced.
        accounts.resetLoaded();
        check(accounts.loadAccount(migrateRow, &error),
              QStringLiteral("F1: legacy blob loads with the proven device key (%1)").arg(error));
        const QByteArray migratedBlob = accountBlob(storage, migrateRow);
        check(migratedBlob != originalMigrateBlob,
              "F1: proven-key legacy load rewrites the blob (normal migration)");
        check(migratedBlob.size() > 0 && !migratedBlob.contains("PRIVATE KEY"),
              "F1: migrated blob stays encrypted");

        // (a) Replace the device key with a different valid 32-byte secret.
        // The legacy fallback may decrypt the old blob, but must not rewrite it
        // with the unproven key.
        QByteArray rotatedKey(32, '\x41');
        check(writeFileBytes(Paths::keystoreDeviceKey(), rotatedKey),
              "F1: rotated 32-byte device key installed");
        check(readFileBytes(deviceKeyTagPath()) == trustedTag,
              "F1: the rotated key did not overwrite the original provenance tag");
        accounts.resetLoaded();
        QString wrongKeyError;
        check(accounts.loadAccount(wrongKeyRow, &wrongKeyError),
              QStringLiteral("F1: legacy blob still loads read-only under the rotated key (%1)")
                  .arg(wrongKeyError));
        const QByteArray afterRotatedLoad = accountBlob(storage, wrongKeyRow);
        check(afterRotatedLoad == originalWrongKeyBlob,
              "F1: rotated device key did not rewrite the legacy blob");
        const Bytes legacyKek = Crypto::deriveKek(bytesOf(legacyFactor()), bytesOf(salt));
        check(Crypto::aesGcmDecrypt(bytesOf(afterRotatedLoad), legacyKek).has_value(),
              "F1: original legacy blob is still decryptable with the legacy factor");

        // A new-format account must fail cleanly with the rotated key, and the
        // existing device key must never be overwritten by the failed load.
        accounts.resetLoaded();
        check(accounts.loadAccount(anchor, &error) == false,
              "F1: new-format account cannot load with a rotated device key");
        check(readFileBytes(Paths::keystoreDeviceKey()) == rotatedKey,
              "F1: failed load never rewrites the existing device key");

        // Restoring the trusted key must recover both the anchor and the
        // legacy identity. If the legacy blob had been rewritten by the rotated
        // key this fails.
        check(writeFileBytes(Paths::keystoreDeviceKey(), trustedKey),
              "F1: original device key restored");
        check(readFileBytes(deviceKeyTagPath()) == trustedTag,
              "F1: the trusted provenance tag survives the rotation test");
        accounts.resetLoaded();
        QString anchorError;
        check(accounts.loadAccount(anchor, &anchorError),
              QStringLiteral("F1: anchor account loads again after restoring the key (%1)")
                  .arg(anchorError));
        accounts.resetLoaded();
        QString legacyError;
        check(accounts.loadAccount(wrongKeyRow, &legacyError),
              QStringLiteral("F1: legacy account loads after restoring the key (%1)")
                  .arg(legacyError));
        const QByteArray afterRestore = accountBlob(storage, wrongKeyRow);
        const Bytes trustedKek = Crypto::deriveKek(bytesOf(trustedKey), bytesOf(salt));
        check(Crypto::aesGcmDecrypt(bytesOf(afterRestore), trustedKek).has_value(),
              "F1: restored-key load kept decryptable data");
        storage.close();
    }
    qunsetenv("MESHLINK_LEGACY_MACHINE_FACTOR");

    // ------------------------------------- retained legacy copy (t17/t14-F1/F2) ----
    {
        QTemporaryDir home;
        check(home.isValid(), "F3: temporary data directory created");
        configureDataHome(home.path());
        const QString dbPath = home.filePath(QStringLiteral("guard-f3.db"));
        Storage storage;
        QString error;
        check(storage.open(dbPath, &error),
              QStringLiteral("F3: database opened (%1)").arg(error));
        AccountManager accounts(&storage);

        const qint64 anchor = accounts.createNewAccount(QStringLiteral("CopyAnchor"), &error);
        const qint64 legacyRow = accounts.createNewAccount(QStringLiteral("CopyLegacy"), &error);
        check(anchor > 0 && legacyRow > 0,
              QStringLiteral("F3: anchor and legacy fixture created (%1)").arg(error));
        const QByteArray trustedKey = readFileBytes(Paths::keystoreDeviceKey());
        const QByteArray trustedTag = readFileBytes(deviceKeyTagPath());
        const QByteArray salt = readFileBytes(Paths::keystoreSalt());
        check(trustedKey.size() == 32 && trustedTag.size() == 32 && salt.size() == 16,
              "F3: trusted key/tag/salt are present");
        check(installLegacyRow(storage, accounts, legacyRow, salt, legacyFactor()),
              "F3: legacy fixture has a machine-id blob");
        const QByteArray originalBlob = accountBlob(storage, legacyRow);
        const QString copyPath = legacyKeystoreCopy(legacyRow);

        // Proven migration must retain the only legacy ciphertext before
        // replacing the row.
        accounts.resetLoaded();
        check(accounts.loadAccount(legacyRow, &error),
              QStringLiteral("F3: proven-key legacy load succeeds (%1)").arg(error));
        const QByteArray migratedBlob = accountBlob(storage, legacyRow);
        check(!migratedBlob.isEmpty() && migratedBlob != originalBlob,
              "F3: proven-key migration rewrote the row");
        check(QFile::exists(copyPath), "F3: legacy ciphertext was retained before rewrite");
        check(readFileBytes(copyPath) == originalBlob,
              "F3: retained copy is byte-identical to the original legacy ciphertext");
        const QFile::Permissions copyPerms = QFile::permissions(copyPath);
        check(copyPerms.testFlag(QFileDevice::ReadOwner)
                  && copyPerms.testFlag(QFileDevice::WriteOwner)
                  && !copyPerms.testFlag(QFileDevice::ReadGroup)
                  && !copyPerms.testFlag(QFileDevice::WriteGroup)
                  && !copyPerms.testFlag(QFileDevice::ExeOwner),
              "F3: retained copy is 0600 (owner read/write only)");
        const QFile::Permissions legacyDirPerms =
            QFile::permissions(legacyKeystoreDir());
        check(legacyDirPerms.testFlag(QFileDevice::ReadOwner)
                  && legacyDirPerms.testFlag(QFileDevice::WriteOwner)
                  && legacyDirPerms.testFlag(QFileDevice::ExeOwner)
                  && !legacyDirPerms.testFlag(QFileDevice::ReadGroup)
                  && !legacyDirPerms.testFlag(QFileDevice::ExeGroup)
                  && !legacyDirPerms.testFlag(QFileDevice::ReadOther),
              "F3: retained legacy directory is 0700");

        // t14-F1: device.key disappears but its tag survives. No replacement
        // key may be created; the migrated row stays untouched and the retained
        // legacy copy keeps the account readable.
        check(QFile::remove(Paths::keystoreDeviceKey()),
              "F3: device key removed, tag kept (partial restore)");
        accounts.resetLoaded();
        QString missingKeyError;
        check(accounts.loadAccount(legacyRow, &missingKeyError),
              QStringLiteral("F3: missing-key load succeeds via retained copy (%1)")
                  .arg(missingKeyError));
        check(!QFile::exists(Paths::keystoreDeviceKey()),
              "F3: no replacement device key was created while the tag remained");
        check(readFileBytes(deviceKeyTagPath()) == trustedTag,
              "F3: missing-key load left the provenance tag untouched");
        check(accountBlob(storage, legacyRow) == migratedBlob,
              "F3: missing-key load did not rewrite the stored row");
        check(readFileBytes(copyPath) == originalBlob,
              "F3: missing-key load left the retained copy unchanged");

        // Restoring the intended key must recover the account through the
        // stored row without needing the copy.
        check(writeFileBytes(Paths::keystoreDeviceKey(), trustedKey),
              "F3: original device key restored");
        accounts.resetLoaded();
        QString restoredError;
        check(accounts.loadAccount(legacyRow, &restoredError),
              QStringLiteral("F3: restored-key load succeeds (%1)").arg(restoredError));
        check(accountBlob(storage, legacyRow) == migratedBlob,
              "F3: restored-key load kept the migrated row byte-identical");
        check(readFileBytes(copyPath) == originalBlob,
              "F3: restored-key load kept the retained copy byte-identical");

        // t14-F2: a valid-but-wrong 32-byte key accompanied by its own matching
        // tag must still not be trusted; the migrated row is unreadable with it,
        // so recovery goes through the retained legacy copy and the row is not
        // rewritten.
        const QByteArray wrongKey(32, '\x5A');
        check(writeFileBytes(Paths::keystoreDeviceKey(), wrongKey),
              "F3: valid wrong 32-byte device key installed");
        check(writeFileBytes(deviceKeyTagPath(), deviceKeyTagFor(wrongKey)),
              "F3: wrong key has a self-consistent provenance tag");
        accounts.resetLoaded();
        QString wrongKeyError;
        check(accounts.loadAccount(legacyRow, &wrongKeyError),
              QStringLiteral("F3: wrong-key load succeeds via retained copy (%1)")
                  .arg(wrongKeyError));
        check(accountBlob(storage, legacyRow) == migratedBlob,
              "F3: wrong-key load did not rewrite the stored row");
        check(readFileBytes(copyPath) == originalBlob,
              "F3: wrong-key load left the retained copy unchanged");
        check(readFileBytes(Paths::keystoreDeviceKey()) == wrongKey,
              "F3: wrong-key load did not replace the existing key");

        // Restoring both the key and tag recovers the normal path.
        check(writeFileBytes(Paths::keystoreDeviceKey(), trustedKey)
                  && writeFileBytes(deviceKeyTagPath(), trustedTag),
              "F3: original key and tag restored");
        accounts.resetLoaded();
        check(accounts.loadAccount(legacyRow, &error),
              QStringLiteral("F3: restored key/tag load succeeds (%1)").arg(error));
        check(accountBlob(storage, legacyRow) == migratedBlob,
              "F3: restored key/tag left the migrated row unchanged");

        // Destroying the account removes its retained copy while shared
        // secrets survive for the anchor. Destroying the last account also
        // removes the legacy directory.
        check(accounts.activeRowId() == legacyRow, "F3: legacy account is active");
        const int remaining = accounts.destroyAccount(false, &error);
        check(remaining == 1,
              QStringLiteral("F3: destroying the legacy account leaves one (%1)").arg(remaining));
        check(!QFile::exists(copyPath), "F3: retained copy was removed on destroy");
        check(QFile::exists(Paths::keystoreDeviceKey()) && QFile::exists(deviceKeyTagPath()),
              "F3: shared secrets survive the per-account destroy");
        accounts.resetLoaded();
        check(accounts.loadAccount(anchor, &error),
              QStringLiteral("F3: anchor still loads after per-account destroy (%1)").arg(error));
        check(accounts.destroyAccount(false, &error) == 0,
              "F3: destroying the last account reports zero");
        check(!QDir(legacyKeystoreDir()).exists(),
              "F3: legacy directory is removed with the last account");
        storage.close();
    }

    // ---------------------------------------------------------------- F2 ----
    {
        QTemporaryDir home;
        check(home.isValid(), "F2: temporary data directory created");
        configureDataHome(home.path());
        const QString dbPath = home.filePath(QStringLiteral("guard-f2.db"));
        Storage storage;
        QString error;
        check(storage.open(dbPath, &error),
              QStringLiteral("F2: database opened (%1)").arg(error));
        AccountManager accounts(&storage);

        const qint64 first = accounts.createNewAccount(QStringLiteral("Keep"), &error);
        const qint64 second = accounts.createNewAccount(QStringLiteral("Drop"), &error);
        check(first > 0 && second > 0,
              QStringLiteral("F2: two accounts created (%1)").arg(error));
        check(accounts.activeRowId() == second, "F2: second account is active");

        const int remaining = accounts.destroyAccount(false, &error);
        check(remaining == 1, QStringLiteral("F2: destroying one of two reports one left (%1)")
                                  .arg(remaining));
        check(QFile::exists(Paths::keystoreSalt()) && QFile::exists(Paths::keystoreDeviceKey())
                  && QFile::exists(deviceKeyTagPath()),
              "F2: shared secrets and tag survive while one account remains");
        accounts.resetLoaded();
        check(accounts.loadAccount(first, &error),
              QStringLiteral("F2: surviving account still loads (%1)").arg(error));

        // Last account: the shared install secrets are removed.
        const int last = accounts.destroyAccount(false, &error);
        check(last == 0, QStringLiteral("F2: destroying the last account reports zero (%1)")
                             .arg(last));
        check(!QFile::exists(Paths::keystoreSalt())
                  && !QFile::exists(Paths::keystoreDeviceKey())
                  && !QFile::exists(deviceKeyTagPath()),
              "F2: shared secrets and tag are wiped with the last account");
        storage.close();
    }

    // Failure path: a locked database must abort destruction and preserve the
    // shared secrets + the surviving account's decryptability.
    {
        QTemporaryDir home;
        check(home.isValid(), "F2 failure: temporary data directory created");
        configureDataHome(home.path());
        const QString dbPath = home.filePath(QStringLiteral("guard-f2-failure.db"));
        Storage storage;
        QString error;
        check(storage.open(dbPath, &error),
              QStringLiteral("F2 failure: database opened (%1)").arg(error));
        AccountManager accounts(&storage);
        const qint64 first = accounts.createNewAccount(QStringLiteral("Survivor"), &error);
        const qint64 second = accounts.createNewAccount(QStringLiteral("LockedDrop"), &error);
        check(first > 0 && second > 0,
              QStringLiteral("F2 failure: two accounts created (%1)").arg(error));
        check(forceDestroyFailure(storage, accounts, dbPath, first, &error),
              "F2 failure: survivor remains usable after the aborted destruction");
        storage.close();
    }

    // Count-failure path: even after the account row is removed, a failed
    // checked COUNT(*) must preserve the install-wide secrets and report an
    // error instead of treating the unknown count as zero.
    {
        QTemporaryDir home;
        check(home.isValid(), "F2 count failure: temporary data directory created");
        configureDataHome(home.path());
        const QString dbPath = home.filePath(QStringLiteral("guard-f2-count.db"));
        Storage storage;
        QString error;
        check(storage.open(dbPath, &error),
              QStringLiteral("F2 count failure: database opened (%1)").arg(error));
        AccountManager accounts(&storage);
        const qint64 first = accounts.createNewAccount(QStringLiteral("CountKeep"), &error);
        const qint64 second = accounts.createNewAccount(QStringLiteral("CountDrop"), &error);
        check(first > 0 && second > 0,
              QStringLiteral("F2 count failure: two accounts created (%1)").arg(error));
        qputenv("MESHLINK_TEST_FAIL_ACCOUNT_COUNT", QByteArrayLiteral("1"));
        const int remaining = accounts.destroyAccount(false, &error);
        qunsetenv("MESHLINK_TEST_FAIL_ACCOUNT_COUNT");
        check(remaining < 0,
              QStringLiteral("F2 count failure: destroy reports an error (%1)").arg(remaining));
        check(QFile::exists(Paths::keystoreSalt()) && QFile::exists(Paths::keystoreDeviceKey())
                  && QFile::exists(deviceKeyTagPath()),
              "F2 count failure: shared secrets and tag survive an unverifiable count");
        accounts.resetLoaded();
        QString survivorError;
        check(accounts.loadAccount(first, &survivorError),
              QStringLiteral("F2 count failure: survivor still loads (%1)").arg(survivorError));
        check(!error.isEmpty(), "F2 count failure: a diagnostic was returned to the caller");
        storage.close();
    }

    std::printf("tst_keystore_guard: %d check(s), %d failure(s)\n", g_checks, g_failures);
    if (g_failures == 0)
        std::printf("tst_keystore_guard: ALL OK\n");
    return g_failures == 0 ? 0 : 1;
}
