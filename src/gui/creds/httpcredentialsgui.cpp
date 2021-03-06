/*
 * Copyright (C) by Klaas Freitag <freitag@kde.org>
 * Copyright (C) by Olivier Goffart <ogoffart@woboq.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include <QInputDialog>
#include <QLabel>
#include <QDesktopServices>
#include <QNetworkReply>
#include <QTimer>
#include <QBuffer>
#include "creds/httpcredentialsgui.h"
#include "theme.h"
#include "account.h"
#include "networkjobs.h"
#include <QMessageBox>
#include "common/asserts.h"

using namespace QKeychain;

namespace OCC {

Q_LOGGING_CATEGORY(lcHttpCredentialsGui, "sync.credentials.http.gui", QtInfoMsg)

void HttpCredentialsGui::askFromUser()
{
    // This function can be called from AccountState::slotInvalidCredentials,
    // which (indirectly, through HttpCredentials::invalidateToken) schedules
    // a cache wipe of the qnam. We can only execute a network job again once
    // the cache has been cleared, otherwise we'd interfere with the job.
    QTimer::singleShot(100, this, &HttpCredentialsGui::askFromUserAsync);
}

void HttpCredentialsGui::askFromUserAsync()
{
    if (Theme::instance()->noUnauthedRequests()) {
        performOAuthProcess();
        return;
    }

    // First, we will check what kind of auth we need.
    auto job = new DetermineAuthTypeJob(_account->sharedFromThis(), this);
    QObject::connect(job, &DetermineAuthTypeJob::authType, this, [this](DetermineAuthTypeJob::AuthType type) {
        if (type == DetermineAuthTypeJob::OAuth) {
            performOAuthProcess();
        } else if (type == DetermineAuthTypeJob::Basic) {
            showDialog();
        } else {
            // Shibboleth?
            qCWarning(lcHttpCredentialsGui) << "Bad http auth type:" << type;
            emit asked();
        }
    });
    job->start();
}

void HttpCredentialsGui::performOAuthProcess()
{
    _asyncAuth.reset(new OAuth(_account, this));
    _asyncAuth->_expectedUser = _account->davUser();
    connect(_asyncAuth.data(), &OAuth::result,
        this, &HttpCredentialsGui::asyncAuthResult);
    connect(_asyncAuth.data(), &OAuth::destroyed,
        this, &HttpCredentialsGui::authorisationLinkChanged);
    _asyncAuth->start();
    emit authorisationLinkChanged();
}

void HttpCredentialsGui::asyncAuthResult(OAuth::Result r, const QString &user,
    const QString &token, const QString &refreshToken)
{
    switch (r) {
    case OAuth::NotSupported:
        showDialog();
        _asyncAuth.reset(0);
        return;
    case OAuth::Error:
        _asyncAuth.reset(0);
        emit asked();
        return;
    case OAuth::LoggedIn:
        break;
    }

    ASSERT(_user == user); // ensured by _asyncAuth

    _password = token;
    _refreshToken = refreshToken;
    _ready = true;
    persist();
    _asyncAuth.reset(0);
    emit asked();
}

void HttpCredentialsGui::showDialog()
{
    QString msg = tr("Por favor entre uma senha %1:"
                     "<br>"
                     "Usuário: %2&lt;br&gt;"
                     "Conta: %3&lt;br&gt;")
                      .arg(Utility::escape(Theme::instance()->appNameGUI()),
                          Utility::escape(_user),
                          Utility::escape(_account->displayName()));

    QString reqTxt = requestAppPasswordText(_account);
    if (!reqTxt.isEmpty()) {
        msg += QLatin1String("<br>") + reqTxt + QLatin1String("<br>");
    }
    if (!_fetchErrorString.isEmpty()) {
        msg += QLatin1String("<br>")
            + tr("Leitura de chaveiro falhou com o erro: &apos;%1&apos;")
                  .arg(Utility::escape(_fetchErrorString))
            + QLatin1String("<br>");
    }

    QInputDialog *dialog = new QInputDialog();
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    dialog->setWindowTitle(tr("Entrar Senha"));
    dialog->setLabelText(msg);
    dialog->setTextValue(_previousPassword);
    dialog->setTextEchoMode(QLineEdit::Password);
    if (QLabel *dialogLabel = dialog->findChild<QLabel *>()) {
        dialogLabel->setOpenExternalLinks(true);
        dialogLabel->setTextFormat(Qt::RichText);
    }

    dialog->open();
    connect(dialog, &QDialog::finished, this, [this, dialog](int result) {
        if (result == QDialog::Accepted) {
            _password = dialog->textValue();
            _refreshToken.clear();
            _ready = true;
            persist();
        }
        emit asked();
    });
}

QString HttpCredentialsGui::requestAppPasswordText(const Account *account)
{
    int version = account->serverVersionInt();
    QString path;

    // Version may not be available before login on new servers!
    if (!version || version >= Account::makeServerVersion(10, 0, 0)) {
        path = QLatin1String("/index.php/settings/personal?sectionid=security#apppasswords");
    } else if (version >= Account::makeServerVersion(9, 1, 0)) {
        path = QLatin1String("/index.php/settings/personal?section=apppasswords");
    } else {
        // Older server than 9.1 does not have the feature to request App Password
        return QString();
    }

    auto baseUrl = account->url().toString();
    if (baseUrl.endsWith('/'))
        baseUrl.chop(1);
    return tr("<a href=%1>Clique aqui</a> para solicitar uma senha de aplicativo na interface da web.").arg(baseUrl + path);
}
} // namespace OCC
