// deepin-MeshLink — SQLite persistence (NFR-07/NFR-08).
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#pragma once

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>

namespace meshlink {

class Storage {
public:
    static constexpr const char *kConnectionName = "meshlink-db";

    // Opens (creating dirs/schema when needed) the SQLite database.
    bool open(const QString &filePath, QString *error = nullptr);
    void close();
    bool isOpen() const;
    QSqlDatabase db() const;

    // Small helpers used across managers.
    bool exec(const QString &sql, QString *error = nullptr);
    // IMPORTANT: query() only *prepares* the statement. Callers must bind their
    // values and then call QSqlQuery::exec() before next()/value(); a prepared
    // query is not executed by next() (Qt SQL).
    QSqlQuery query(const QString &sql);

    // Securely wipe every record related to a fingerprint (account destroy).
    bool wipeAccount(const QString &fingerprint, bool alsoChatHistoryAndFiles, QString *error = nullptr);

private:
    bool createSchema(QString *error);
    // Forward-only migration: adds invite_code, rekeys legacy 16-hex room ids to
    // 32-hex, backfills missing invite codes. Idempotent.
    bool migrate(QString *error);
};

} // namespace meshlink
