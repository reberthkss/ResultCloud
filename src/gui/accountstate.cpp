/*
 * Copyright (C) by Daniel Molkentin <danimo@owncloud.com>
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

#include "accountstate.h"
#include "accountmanager.h"
#include "account.h"
#include "creds/abstractcredentials.h"
#include "creds/httpcredentials.h"
#include "logger.h"
#include "configfile.h"

#include <QSettings>
#include <QTimer>
#include <qfontmetrics.h>

namespace OCC {

Q_LOGGING_CATEGORY(lcAccountState, "gui.account.state", QtInfoMsg)

AccountState::AccountState(AccountPtr account)
    : QObject()
    , _account(account)
    , _state(AccountState::Disconnected)
    , _connectionStatus(ConnectionValidator::Undefined)
    , _waitingForNewCredentials(false)
    , _maintenanceToConnectedDelay(60000 + (qrand() % (4 * 60000))) // 1-5min delay
{
    qRegisterMetaType<AccountState *>("AccountState*");

    connect(account.data(), &Account::invalidCredentials,
        this, &AccountState::slotInvalidCredentials);
    connect(account.data(), &Account::credentialsFetched,
        this, &AccountState::slotCredentialsFetched);
    connect(account.data(), &Account::credentialsAsked,
        this, &AccountState::slotCredentialsAsked);
    _timeSinceLastETagCheck.invalidate();
}

AccountState::~AccountState()
{
}

AccountState *AccountState::loadFromSettings(AccountPtr account, QSettings & /*settings*/)
{
    auto accountState = new AccountState(account);
    return accountState;
}

void AccountState::writeToSettings(QSettings & /*settings*/)
{
}

AccountPtr AccountState::account() const
{
    return _account;
}

AccountState::ConnectionStatus AccountState::connectionStatus() const
{
    return _connectionStatus;
}

QStringList AccountState::connectionErrors() const
{
    return _connectionErrors;
}

AccountState::State AccountState::state() const
{
    return _state;
}

void AccountState::setState(State state)
{
    if (_state != state) {
        qCInfo(lcAccountState) << "AccountState state change: "
                               << stateString(_state) << "->" << stateString(state);
        State oldState = _state;
        _state = state;

        if (_state == SignedOut) {
            _connectionStatus = ConnectionValidator::Undefined;
            _connectionErrors.clear();
        } else if (oldState == SignedOut && _state == Disconnected) {
            // If we stop being voluntarily signed-out, try to connect and
            // auth right now!
            checkConnectivity();
        } else if (_state == ServiceUnavailable) {
            // Check if we are actually down for maintenance.
            // To do this we must clear the connection validator that just
            // produced the 503. It's finished anyway and will delete itself.
            _connectionValidator.clear();
            checkConnectivity();
        }
        if (oldState == Connected || _state == Connected) {
            emit isConnectedChanged();
        }
    }

    // might not have changed but the underlying _connectionErrors might have
    emit stateChanged(_state);
}

QString AccountState::stateString(State state)
{
    switch (state) {
    case SignedOut:
        return tr("Desconectado");
    case Disconnected:
        return tr("Desconectado");
    case Connected:
        return tr("Conectado");
    case ServiceUnavailable:
        return tr("Serviço indisponível");
    case MaintenanceMode:
        return tr("Modo de manutenção");
    case NetworkError:
        return tr("Erro de rede");
    case ConfigurationError:
        return tr("Erro de configuração");
    case AskingCredentials:
        return tr("Solicitando Credenciais");
    }
    return tr("Estado da conta desconhecido");
}

bool AccountState::isSignedOut() const
{
    return _state == SignedOut;
}

void AccountState::signOutByUi()
{
    account()->credentials()->forgetSensitiveData();
    setState(SignedOut);
}

void AccountState::freshConnectionAttempt()
{
    if (isConnected())
        setState(Disconnected);
    checkConnectivity();
}

void AccountState::signIn()
{
    if (_state == SignedOut) {
        _waitingForNewCredentials = false;
        setState(Disconnected);
    }
}

bool AccountState::isConnected() const
{
    return _state == Connected;
}

void AccountState::tagLastSuccessfullETagRequest()
{
    _timeSinceLastETagCheck.start();
}

void AccountState::checkConnectivity()
{
    if (isSignedOut() || _waitingForNewCredentials) {
        return;
    }

    if (_connectionValidator) {
        qCWarning(lcAccountState) << "ConnectionValidator already running, ignoring" << account()->displayName();
        return;
    }

    // If we never fetched credentials, do that now - otherwise connection attempts
    // make little sense, we might be missing client certs.
    if (!account()->credentials()->wasFetched()) {
        _waitingForNewCredentials = true;
        account()->credentials()->fetchFromKeychain();
        return;
    }

    // IF the account is connected the connection check can be skipped
    // if the last successful etag check job is not so long ago.
    ConfigFile cfg;
    std::chrono::milliseconds polltime = cfg.remotePollInterval();

    if (isConnected() && _timeSinceLastETagCheck.isValid()
        && !_timeSinceLastETagCheck.hasExpired(polltime.count())) {
        qCDebug(lcAccountState) << account()->displayName() << "The last ETag check succeeded within the last " << polltime.count() / 1000 << " secs. No connection check needed!";
        return;
    }

    ConnectionValidator *conValidator = new ConnectionValidator(account());
    _connectionValidator = conValidator;
    connect(conValidator, &ConnectionValidator::connectionResult,
        this, &AccountState::slotConnectionValidatorResult);
    if (isConnected()) {
        // Use a small authed propfind as a minimal ping when we're
        // already connected.
        conValidator->checkAuthentication();
    } else {
        // Check the server and then the auth.

        // Let's try this for all OS and see if it fixes the Qt issues we have on Linux  #4720 #3888 #4051
        //#ifdef Q_OS_WIN
        // There seems to be a bug in Qt on Windows where QNAM sometimes stops
        // working correctly after the computer woke up from sleep. See #2895 #2899
        // and #2973.
        // As an attempted workaround, reset the QNAM regularly if the account is
        // disconnected.
        account()->resetNetworkAccessManager();

        // If we don't reset the ssl config a second CheckServerJob can produce a
        // ssl config that does not have a sensible certificate chain.
        account()->setSslConfiguration(QSslConfiguration());
        //#endif
        conValidator->checkServerAndAuth();
    }
}

void AccountState::slotConnectionValidatorResult(ConnectionValidator::Status status, const QStringList &errors)
{
    if (isSignedOut()) {
        qCWarning(lcAccountState) << "Signed out, ignoring" << status << _account->url().toString();
        return;
    }

    // Come online gradually from 503 or maintenance mode
    if (status == ConnectionValidator::Connected
        && (_connectionStatus == ConnectionValidator::ServiceUnavailable
               || _connectionStatus == ConnectionValidator::MaintenanceMode)) {
        if (!_timeSinceMaintenanceOver.isValid()) {
            qCInfo(lcAccountState) << "AccountState reconnection: delaying for"
                                   << _maintenanceToConnectedDelay << "ms";
            _timeSinceMaintenanceOver.start();
            QTimer::singleShot(_maintenanceToConnectedDelay + 100, this, &AccountState::checkConnectivity);
            return;
        } else if (_timeSinceMaintenanceOver.elapsed() < _maintenanceToConnectedDelay) {
            qCInfo(lcAccountState) << "AccountState reconnection: only"
                                   << _timeSinceMaintenanceOver.elapsed() << "ms have passed";
            return;
        }
    }

    if (_connectionStatus != status) {
        qCInfo(lcAccountState) << "AccountState connection status change: "
                               << _connectionStatus << "->"
                               << status;
        _connectionStatus = status;
    }
    _connectionErrors = errors;

    switch (status) {
    case ConnectionValidator::Connected:
        if (_state != Connected) {
            setState(Connected);
        }
        break;
    case ConnectionValidator::Undefined:
    case ConnectionValidator::NotConfigured:
        setState(Disconnected);
        break;
    case ConnectionValidator::ServerVersionMismatch:
        setState(ConfigurationError);
        break;
    case ConnectionValidator::StatusNotFound:
        // This can happen either because the server does not exist
        // or because we are having network issues. The latter one is
        // much more likely, so keep trying to connect.
        setState(NetworkError);
        break;
    case ConnectionValidator::CredentialsWrong:
    case ConnectionValidator::CredentialsNotReady:
        slotInvalidCredentials();
        break;
    case ConnectionValidator::SslError:
        setState(SignedOut);
        break;
    case ConnectionValidator::ServiceUnavailable:
        _timeSinceMaintenanceOver.invalidate();
        setState(ServiceUnavailable);
        break;
    case ConnectionValidator::MaintenanceMode:
        _timeSinceMaintenanceOver.invalidate();
        setState(MaintenanceMode);
        break;
    case ConnectionValidator::Timeout:
        setState(NetworkError);
        break;
    }
}

void AccountState::slotInvalidCredentials()
{
    if (isSignedOut() || _waitingForNewCredentials)
        return;

    qCInfo(lcAccountState) << "Invalid credentials for" << _account->url().toString()
                           << "asking user";

    _waitingForNewCredentials = true;
    setState(AskingCredentials);

    if (account()->credentials()->ready()) {
        account()->credentials()->invalidateToken();
    }
    if (auto creds = qobject_cast<HttpCredentials *>(account()->credentials())) {
        if (creds->refreshAccessToken())
            return;
    }
    account()->credentials()->askFromUser();
}

void AccountState::slotCredentialsFetched(AbstractCredentials *)
{
    // Make a connection attempt, no matter whether the credentials are
    // ready or not - we want to check whether we can get an SSL connection
    // going before bothering the user for a password.
    qCInfo(lcAccountState) << "Fetched credentials for" << _account->url().toString()
                           << "attempting to connect";
    _waitingForNewCredentials = false;
    checkConnectivity();
}

void AccountState::slotCredentialsAsked(AbstractCredentials *credentials)
{
    qCInfo(lcAccountState) << "Credentials asked for" << _account->url().toString()
                           << "are they ready?" << credentials->ready();

    _waitingForNewCredentials = false;

    if (!credentials->ready()) {
        // User canceled the connection or did not give a password
        setState(SignedOut);
        return;
    }

    if (_connectionValidator) {
        // When new credentials become available we always want to restart the
        // connection validation, even if it's currently running.
        _connectionValidator->deleteLater();
        _connectionValidator = 0;
    }

    checkConnectivity();
}

std::unique_ptr<QSettings> AccountState::settings()
{
    auto s = ConfigFile::settingsWithGroup(QLatin1String("Accounts"));
    s->beginGroup(_account->id());
    return s;
}

} // namespace OCC
