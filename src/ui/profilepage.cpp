// deepin-MeshLink — profile page implementation.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#include "profilepage.h"

#include "core/account.h"
#include "core/engine.h"
#include "core/settings.h"
#include "firststartwizard.h"
#include "uiutils.h"

#include <DComboBox>
#include <DDciIcon>
#include <DDialog>
#include <DFontSizeManager>
#include <DFrame>
#include <DIconButton>
#include <DLabel>
#include <DLineEdit>
#include <DMessageManager>
#include <DPaletteHelper>
#include <DPushButton>
#include <DSuggestButton>
#include <DWarningButton>

#include <QCheckBox>
#include <QClipboard>
#include <QDir>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QRandomGenerator>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QVBoxLayout>

DWIDGET_USE_NAMESPACE
DGUI_USE_NAMESPACE

namespace meshlink {

namespace {

// Theme icons are not guaranteed: uninstalled builds, minimal icon themes and
// headless sessions can all return a null QIcon, and a flat DIconButton with a
// null icon paints nothing (the button then looks missing). Resolve the refresh
// glyph through every available source and finally draw one, so the control is
// always visible.
QIcon refreshGlyphIcon(QWidget *paletteSource) {
    QIcon icon = UiUtils::icon(QStringLiteral("view-refresh"),
                               QStringLiteral("view-refresh-symbolic"));
    if (icon.isNull())
        icon = UiUtils::icon(QStringLiteral("view-refresh"), QStringLiteral("edit-redo"));
    if (icon.isNull()) {
        // DTK keeps DCI icons next to the icon theme; use it when available.
        const DDciIcon dci = DDciIcon::fromTheme(QStringLiteral("view-refresh"));
        if (!dci.isNull()) {
            const qreal dpr = paletteSource ? qMax<qreal>(1.0, paletteSource->devicePixelRatioF()) : 1.0;
            const QPixmap pm = dci.pixmap(dpr, 24, DDciIcon::Light, DDciIcon::Normal);
            if (!pm.isNull())
                icon = QIcon(pm);
        }
    }
    if (!icon.isNull())
        return icon;

    // Last resort: a palette-aware circular arrow, never a blank button.
    constexpr int kSide = 24;
    const qreal dpr = paletteSource ? qMax<qreal>(1.0, paletteSource->devicePixelRatioF()) : 1.0;
    QPixmap pm(qRound(kSide * dpr), qRound(kSide * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter painter(&pm);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const DPalette palette = DPaletteHelper::instance()->palette(paletteSource);
    const QColor ink = palette.color(DPalette::Text);
    QPen pen(ink);
    pen.setWidthF(2.0);
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);
    painter.drawArc(QRectF(3.5, 3.5, kSide - 7.0, kSide - 7.0), 40 * 16, 285 * 16);
    QPainterPath head;
    head.moveTo(kSide - 5.5, 5.0);
    head.lineTo(kSide - 10.5, 5.5);
    head.lineTo(kSide - 7.5, 9.5);
    head.closeSubpath();
    painter.setPen(Qt::NoPen);
    painter.setBrush(ink);
    painter.drawPath(head);
    painter.end();
    return QIcon(pm);
}

// Selectable read-only fingerprint text. The label shrinks its monospace font
// to the available width so the full 32 characters stay visible instead of
// being clipped (the profile page can be narrow under the 900px main window).
class FingerprintLabel : public DTK_WIDGET_NAMESPACE::DLabel {
public:
    explicit FingerprintLabel(QWidget *parent) : DLabel(parent) {
        setTextFormat(Qt::PlainText);
        setWordWrap(false);
        setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        setFocusPolicy(Qt::ClickFocus);
        setForegroundRole(DPalette::Text);
        // Ignored horizontal policy lets the layout shrink the row below the
        // label's natural size; fitToWidth() then picks a readable font size.
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        setMinimumWidth(0);
    }

    void fitToWidth() {
        QFont fitted = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        fitted.setStyleHint(QFont::Monospace);
        fitted.setFixedPitch(true);
        const int available = qMax(1, contentsRect().width());
        int pixelSize = 14;
        for (; pixelSize > 8; --pixelSize) {
            fitted.setPixelSize(pixelSize);
            if (QFontMetrics(fitted).horizontalAdvance(text()) + 2 <= available)
                break;
        }
        fitted.setPixelSize(pixelSize);
        if (font() != fitted)
            setFont(fitted);
    }

protected:
    void resizeEvent(QResizeEvent *event) override {
        DLabel::resizeEvent(event);
        fitToWidth();
    }
};

// Copy glyph fallback mirroring refreshGlyphIcon(): theme -> DCI -> painted.
QIcon copyGlyphIcon(QWidget *paletteSource) {
    QIcon icon = UiUtils::icon(QStringLiteral("edit-copy"),
                               QStringLiteral("edit-copy-symbolic"));
    if (icon.isNull())
        icon = UiUtils::icon(QStringLiteral("edit-copy"), QStringLiteral("copy"));
    if (icon.isNull()) {
        const DDciIcon dci = DDciIcon::fromTheme(QStringLiteral("edit-copy"));
        if (!dci.isNull()) {
            const qreal dpr = paletteSource ? qMax<qreal>(1.0, paletteSource->devicePixelRatioF()) : 1.0;
            const QPixmap pm = dci.pixmap(dpr, 20, DDciIcon::Light, DDciIcon::Normal);
            if (!pm.isNull())
                icon = QIcon(pm);
        }
    }
    if (!icon.isNull())
        return icon;

    constexpr int kSide = 20;
    const qreal dpr = paletteSource ? qMax<qreal>(1.0, paletteSource->devicePixelRatioF()) : 1.0;
    QPixmap pm(qRound(kSide * dpr), qRound(kSide * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter painter(&pm);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor ink = DPaletteHelper::instance()->palette(paletteSource).color(DPalette::Text);
    QPen pen(ink);
    pen.setWidthF(1.6);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(4.0, 3.0, 10.0, 10.0), 2.0, 2.0);
    painter.drawRoundedRect(QRectF(7.5, 6.5, 9.0, 9.0), 2.0, 2.0);
    painter.end();
    return QIcon(pm);
}

} // namespace

ProfilePage::ProfilePage(Engine *engine, QWidget *parent) : QWidget(parent), m_engine(engine) {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(24, 20, 24, 20);
    root->setSpacing(16);
    root->addWidget(UiUtils::pageHeader(tr("Profile"), nullptr, this));

    // ---- accounts card (multi-account) ----
    auto *accounts = new DFrame(this);
    accounts->setObjectName(QStringLiteral("profilePage.accountsCard"));
    accounts->setLineWidth(1);
    auto *accountsLayout = new QHBoxLayout(accounts);
    accountsLayout->setContentsMargins(16, 14, 16, 14);
    accountsLayout->setSpacing(16);
    auto *accountsText = new QVBoxLayout;
    accountsText->setContentsMargins(0, 0, 0, 0);
    accountsText->setSpacing(4);
    auto *accountsTitle = new DLabel(tr("Accounts"), accounts);
    accountsTitle->setForegroundRole(DPalette::TextTitle);
    DFontSizeManager::instance()->bind(accountsTitle, DFontSizeManager::T6, QFont::DemiBold);
    accountsText->addWidget(accountsTitle);
    m_accountCombo = new DComboBox(accounts);
    m_accountCombo->setObjectName(QStringLiteral("profilePage.accountCombo"));
    m_accountCombo->setToolTip(
        tr("Switch the active local identity; rooms belong to the selected account"));
    accountsText->addWidget(m_accountCombo, 1);
    auto *addAccount = new DPushButton(tr("Add account…"), accounts);
    addAccount->setObjectName(QStringLiteral("profilePage.addAccountButton"));
    addAccount->setIcon(UiUtils::icon(QStringLiteral("list-add"), QStringLiteral("contact-new")));
    connect(addAccount, &DPushButton::clicked, this, [this] {
        if (!FirstStartWizard::createAdditionalAccount(m_engine, this))
            return;
        refreshAccounts();
        load();
    });
    accountsLayout->addLayout(accountsText, 1);
    accountsLayout->addWidget(addAccount, 0, Qt::AlignVCenter);
    root->addWidget(accounts);
    connect(m_accountCombo, &DComboBox::currentIndexChanged, this,
            &ProfilePage::onAccountComboChanged);

    auto *card = new DFrame(this);
    card->setLineWidth(1);
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(20, 20, 20, 20);
    cardLayout->setSpacing(16);

    auto *top = new QHBoxLayout;
    top->setContentsMargins(0, 0, 0, 0);
    top->setSpacing(24);

    auto *avatarColumn = new QVBoxLayout;
    avatarColumn->setContentsMargins(0, 0, 0, 0);
    avatarColumn->setSpacing(10);
    m_avatar = new QLabel(card);
    m_avatar->setFixedSize(96, 96);
    m_avatar->setAlignment(Qt::AlignCenter);
    auto *avatarButtons = new QHBoxLayout;
    avatarButtons->setContentsMargins(0, 0, 0, 0);
    avatarButtons->setSpacing(8);
    auto *change = new DPushButton(tr("Change avatar…"), card);
    change->setIcon(UiUtils::icon(QStringLiteral("camera-photo")));
    connect(change, &DPushButton::clicked, this, &ProfilePage::chooseAvatar);
    auto *randomAvatar = new DIconButton(card);
    randomAvatar->setObjectName(QStringLiteral("profilePage.randomAvatarButton"));
    // A null theme icon used to make this flat button invisible; the helper
    // always returns a drawable refresh glyph.
    randomAvatar->setIcon(refreshGlyphIcon(card));
    randomAvatar->setIconSize(QSize(20, 20));
    randomAvatar->setToolTip(tr("Random avatar"));
    randomAvatar->setAccessibleName(tr("Random avatar"));
    randomAvatar->setFlat(true);
    randomAvatar->setFixedSize(36, 36);
    connect(randomAvatar, &DIconButton::clicked, this, &ProfilePage::randomAvatar);
    avatarButtons->addWidget(change, 1);
    avatarButtons->addWidget(randomAvatar, 0, Qt::AlignVCenter);
    avatarColumn->addWidget(m_avatar, 0, Qt::AlignHCenter);
    avatarColumn->addLayout(avatarButtons);
    avatarColumn->addStretch(1);
    top->addLayout(avatarColumn);

    auto *form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(12);
    m_nick = new DLineEdit(card);
    m_nick->setObjectName(QStringLiteral("profilePage.nickname"));
    m_nick->setPlaceholderText(tr("2–32 characters"));
    m_nick->lineEdit()->setMaxLength(32);
    form->addRow(tr("Nickname"), m_nick);

    m_ip = new DLineEdit(card);
    m_ip->setObjectName(QStringLiteral("profilePage.lanAddress"));
    m_ip->lineEdit()->setReadOnly(true);
    m_ip->setClearButtonEnabled(false);
    form->addRow(tr("LAN address"), m_ip);

    top->addLayout(form, 1);
    cardLayout->addLayout(top);

    // The fingerprint is 32 hex characters: give it a full-width row so it is
    // never clipped, with the copy action at its right.
    auto *fingerprintBox = new QVBoxLayout;
    fingerprintBox->setContentsMargins(0, 0, 0, 0);
    fingerprintBox->setSpacing(6);
    auto *fingerprintLabel = new DLabel(tr("Account fingerprint"), card);
    fingerprintLabel->setForegroundRole(DPalette::TextTips);
    fingerprintBox->addWidget(fingerprintLabel);

    auto *fingerprintRow = new QHBoxLayout;
    fingerprintRow->setContentsMargins(0, 0, 0, 0);
    fingerprintRow->setSpacing(8);
    auto *fp = new FingerprintLabel(card);
    fp->setObjectName(QStringLiteral("profilePage.fingerprint"));
    fp->setAccessibleName(tr("Account fingerprint"));
    m_fp = fp;
    auto *copy = new DIconButton(card);
    copy->setObjectName(QStringLiteral("profilePage.copyFingerprintButton"));
    // Same theme-independence requirement as the random-avatar button.
    copy->setIcon(copyGlyphIcon(card));
    copy->setIconSize(QSize(18, 18));
    copy->setToolTip(tr("Copy fingerprint"));
    copy->setAccessibleName(tr("Copy fingerprint"));
    copy->setFlat(true);
    copy->setFixedSize(32, 32);
    connect(copy, &DIconButton::clicked, this, [this] {
        const QString fingerprint = m_engine->account()->profile().fingerprint;
        if (fingerprint.isEmpty())
            return;
        QGuiApplication::clipboard()->setText(fingerprint);
        DMessageManager::instance()->sendMessage(
            this, UiUtils::icon(QStringLiteral("dialog-ok")),
            tr("Fingerprint copied to the clipboard."));
    });
    fingerprintRow->addWidget(m_fp, 1);
    fingerprintRow->addWidget(copy, 0, Qt::AlignVCenter);
    fingerprintBox->addLayout(fingerprintRow);
    cardLayout->addLayout(fingerprintBox);

    root->addWidget(card);

    auto *actions = new QHBoxLayout;
    actions->setContentsMargins(0, 0, 0, 0);
    actions->setSpacing(8);
    auto *save = new DSuggestButton(tr("Save"), this);
    save->setObjectName(QStringLiteral("profilePage.saveButton"));
    save->setIcon(UiUtils::icon(QStringLiteral("document-save")));
    connect(save, &DSuggestButton::clicked, this, &ProfilePage::save);
    actions->addWidget(save);
    actions->addStretch(1);
    root->addLayout(actions);

    auto *danger = new DFrame(this);
    danger->setLineWidth(1);
    auto *dangerLayout = new QHBoxLayout(danger);
    dangerLayout->setContentsMargins(16, 14, 16, 14);
    dangerLayout->setSpacing(16);
    auto *dangerText = new QVBoxLayout;
    dangerText->setContentsMargins(0, 0, 0, 0);
    dangerText->setSpacing(4);
    auto *dangerTitle = new DLabel(tr("Danger zone"), danger);
    dangerTitle->setForegroundRole(DPalette::TextTitle);
    DFontSizeManager::instance()->bind(dangerTitle, DFontSizeManager::T6, QFont::DemiBold);
    auto *dangerHint = new DLabel(
        tr("Destroying the account securely erases the private key and account record; chat "
           "history and cached files can be removed as well."),
        danger);
    dangerHint->setWordWrap(true);
    dangerHint->setForegroundRole(DPalette::TextTips);
    dangerText->addWidget(dangerTitle);
    dangerText->addWidget(dangerHint);
    auto *destroy = new DWarningButton(danger);
    destroy->setObjectName(QStringLiteral("profilePage.destroyButton"));
    destroy->setText(tr("Destroy current account…"));
    connect(destroy, &DWarningButton::clicked, this, &ProfilePage::destroyAccount);
    dangerLayout->addLayout(dangerText, 1);
    dangerLayout->addWidget(destroy, 0, Qt::AlignVCenter);
    root->addWidget(danger);
    root->addStretch(1);

    connect(m_nick->lineEdit(), &QLineEdit::textChanged, this, [this] { m_nick->setAlert(false); });

    refreshAccounts();
    load();
}

void ProfilePage::reload() {
    refreshAccounts();
    load();
}

void ProfilePage::refreshAccounts() {
    if (!m_accountCombo)
        return;
    const QSignalBlocker blocker(m_accountCombo);
    m_accountCombo->clear();
    const auto accounts = m_engine->account()->listAccounts();
    const qint64 active = m_engine->account()->activeRowId();
    for (const AccountEntry &entry : accounts) {
        const QString label = QStringLiteral("%1  ·  %2")
                                  .arg(entry.nickname, UiUtils::shortFp(entry.fingerprint, 12));
        m_accountCombo->addItem(label);
        if (entry.id == active)
            m_accountCombo->setCurrentIndex(m_accountCombo->count() - 1);
    }
    m_accountCombo->setEnabled(accounts.size() > 1);
}

void ProfilePage::onAccountComboChanged(int index) {
    if (!m_accountCombo || index < 0)
        return;
    const auto accounts = m_engine->account()->listAccounts();
    if (index >= accounts.size())
        return;
    const AccountEntry entry = accounts.at(index);
    if (entry.id == m_engine->account()->activeRowId())
        return;
    if (!UiUtils::confirm(this, tr("Switch account"),
                          tr("Switch the active identity to “%1”? The network reconnects with "
                             "the new identity and rooms of other accounts stay untouched.")
                              .arg(entry.nickname),
                          tr("Switch"))) {
        refreshAccounts();
        return;
    }
    QString error;
    if (!m_engine->switchAccount(entry.id, &error)) {
        // Rollback already restored the previous identity; put the combo and
        // the profile fields back on it before asking the user what to do.
        refreshAccounts();
        load();
        if (m_engine->lastSwitchFailure() == Engine::SwitchFailure::Identity) {
            offerDestroyUnlockable(entry.id, entry.nickname);
        } else {
            UiUtils::warning(this, tr("Switch account"),
                             error.isEmpty() ? tr("The account switch failed.") : error);
        }
        return;
    }
    load();
}

void ProfilePage::offerDestroyUnlockable(qint64 accountId, const QString &nickname) {
    const qint64 activeBefore = m_engine->account()->activeRowId();
    DDialog dialog(this);
    dialog.setTitle(tr("Identity cannot be unlocked"));
    dialog.setMessage(tr("The local keystore of “%1” cannot be decrypted on this device. Its "
                         "credentials may have changed or the key material may be damaged, so "
                         "this identity cannot be unlocked here.")
                          .arg(nickname));
    dialog.setIcon(UiUtils::icon(QStringLiteral("dialog-warning")));
    auto *wipe = new QCheckBox(
        tr("Also delete all chat history, files and download caches"), &dialog);
    dialog.addContent(wipe);
    dialog.addButton(tr("Cancel"), true, DDialog::ButtonNormal);
    dialog.addButton(tr("Destroy account"), false, DDialog::ButtonWarning);
    connect(&dialog, &DDialog::buttonClicked, this,
            [this, &dialog, wipe, accountId, nickname, activeBefore](int index, const QString &) {
                if (index != 1)
                    return;
                QString error;
                const int remaining =
                    m_engine->destroyAccountById(accountId, wipe->isChecked(), &error);
                // -1 can still mean "removed, but the remaining count failed":
                // only report failure when the row is demonstrably still there.
                bool removed = remaining >= 0;
                if (!removed) {
                    removed = true;
                    for (const AccountEntry &other : m_engine->account()->listAccounts()) {
                        if (other.id == accountId) {
                            removed = false;
                            break;
                        }
                    }
                }
                if (!removed) {
                    dialog.setMessage(tr("Destroy failed: %1").arg(error));
                    return;
                }
                dialog.accept();
                refreshAccounts();
                load();
                const bool activeKept =
                    m_engine->account()->activeRowId() == activeBefore && activeBefore > 0;
                if (remaining >= 0 && activeKept) {
                    DMessageManager::instance()->sendMessage(
                        this, UiUtils::icon(QStringLiteral("dialog-ok")),
                        tr("The unlockable account “%1” was destroyed.").arg(nickname));
                } else {
                    DMessageManager::instance()->sendMessage(
                        this, UiUtils::icon(QStringLiteral("dialog-warning")),
                        tr("The unlockable account “%1” was removed, but the active identity or "
                           "the remaining accounts could not be verified: %2")
                            .arg(nickname, error));
                }
            });
    dialog.exec();
}

void ProfilePage::randomAvatar() {
    // Deterministic identicon with a random seed, stored as a pending avatar
    // so it only becomes the identity after "Save".
    const QString seed = QString::number(QRandomGenerator::system()->generate64(), 16);
    const QPixmap avatar = AccountManager::makeIdenticon(seed);
    if (avatar.isNull())
        return;
    const QString path = Paths::avatarDir() +
                         QStringLiteral("/random-%1.png")
                             .arg(QString::number(QRandomGenerator::system()->generate64(), 16));
    QDir().mkpath(Paths::avatarDir());
    avatar.save(path, "PNG");
    m_pendingAvatar = path;
    m_avatar->setPixmap(UiUtils::circularAvatar(avatar, m_nick->text(), 96, this));
}

void ProfilePage::load() {
    const Profile profile = m_engine->account()->profile();
    m_nick->setText(profile.nickname);
    // Never leave the fingerprint row blank: a loaded identity always has the
    // 32-char value, and the no-account transient state shows a dash.
    m_fp->setText(profile.fingerprint.isEmpty() ? QStringLiteral("—") : profile.fingerprint);
    m_fp->setToolTip(profile.fingerprint);
    static_cast<FingerprintLabel *>(m_fp)->fitToWidth();
    m_ip->setText(tr("%1 · port %2")
                      .arg(m_engine->tcpEndpointIp())
                      .arg(m_engine->settings()->tcpPort()));
    m_ip->setToolTip(m_ip->text());

    m_pendingAvatar.clear();
    QPixmap avatar(profile.avatarPath);
    m_avatar->setPixmap(UiUtils::circularAvatar(avatar, profile.nickname, 96, this));
}

void ProfilePage::chooseAvatar() {
    const QString file = UiUtils::openFile(
        this, tr("Choose an avatar"), tr("Images (*.png *.jpg *.jpeg *.bmp *.webp)"));
    if (file.isEmpty())
        return;
    QPixmap avatar(file);
    if (avatar.isNull()) {
        UiUtils::warning(this, tr("Avatar"), tr("The selected image could not be read."));
        return;
    }
    m_pendingAvatar = file;
    m_avatar->setPixmap(UiUtils::circularAvatar(avatar, m_nick->text(), 96, this));
}

void ProfilePage::save() {
    m_nick->setAlert(false);
    QString error;
    // UC-03: nickname rules + circular avatar re-render + new blob hash.
    if (!m_engine->account()->updateProfile(m_nick->text(), m_pendingAvatar, &error)) {
        m_nick->setAlert(true);
        m_nick->showAlertMessage(error.isEmpty() ? tr("Could not save the profile.") : error);
        return;
    }
    m_pendingAvatar.clear();
    load();
    DMessageManager::instance()->sendMessage(
        this, UiUtils::icon(QStringLiteral("dialog-ok")),
        tr("Profile saved; it will be broadcast with heartbeats."));
}

void ProfilePage::destroyAccount() {
    const bool moreAccounts = m_engine->account()->listAccounts().size() > 1;
    DDialog dialog(this);
    dialog.setTitle(tr("Destroy local account"));
    dialog.setMessage(moreAccounts
                          ? tr("The private key and account record of the current account will "
                               "be securely erased; this cannot be undone.\nOther accounts stay "
                               "available and the next one becomes active.")
                          : tr("The private key and account record will be securely erased; this "
                               "cannot be undone.\nYou will return to the first-start screen to "
                               "create a new identity."));
    dialog.setIcon(UiUtils::icon(QStringLiteral("dialog-warning")));
    auto *wipe = new QCheckBox(
        tr("Also delete all chat history, files and download caches"), &dialog);
    dialog.addContent(wipe);
    dialog.addButton(tr("Cancel"));
    dialog.addButton(tr("Destroy"), true, DDialog::ButtonWarning);
    connect(&dialog, &DDialog::buttonClicked, this, [this, &dialog, wipe](int index, const QString &) {
        if (index != 1)
            return;
        QString error;
        const int remaining = m_engine->destroyAccount(wipe->isChecked(), &error);
        if (remaining < 0) {
            dialog.setMessage(tr("Destroy failed: %1").arg(error));
            return;
        }
        dialog.accept();
        emit accountDestroyed(remaining);
    });
    dialog.exec();
}

} // namespace meshlink
