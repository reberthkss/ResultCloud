/*
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
 * Copyright (C) by Krzesimir Nowak <krzesimir@endocode.com>
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

#include "account.h"
#include "config.h"
#include "configfile.h"
#include "theme.h"

#include "wizard/owncloudwizard.h"
#include "wizard/owncloudsetuppage.h"
#include "wizard/owncloudhttpcredspage.h"
#include "wizard/owncloudoauthcredspage.h"
#include "wizard/owncloudadvancedsetuppage.h"
#include "wizard/owncloudwizardresultpage.h"

#include "common/vfs.h"

#include "QProgressIndicator.h"

#include <QtCore>
#include <QtGui>
#include <QMessageBox>

#include <stdlib.h>

namespace OCC {

Q_LOGGING_CATEGORY(lcWizard, "gui.wizard", QtInfoMsg)

OwncloudWizard::OwncloudWizard(QWidget *parent)
    : QWizard(parent)
    , _account(0)
    , _setupPage(new OwncloudSetupPage(this))
    , _httpCredsPage(new OwncloudHttpCredsPage(this))
    , _browserCredsPage(new OwncloudOAuthCredsPage)
    , _advancedSetupPage(new OwncloudAdvancedSetupPage)
    , _resultPage(new OwncloudWizardResultPage)
    , _credentialsPage(0)
    , _setupLog()
{
    setObjectName("owncloudWizard");

    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    setPage(WizardCommon::Page_ServerSetup, _setupPage);
    setPage(WizardCommon::Page_HttpCreds, _httpCredsPage);
    setPage(WizardCommon::Page_OAuthCreds, _browserCredsPage);
    setPage(WizardCommon::Page_AdvancedSetup, _advancedSetupPage);
    setPage(WizardCommon::Page_Result, _resultPage);

    connect(this, &QDialog::finished, this, &OwncloudWizard::basicSetupFinished);

    // note: start Id is set by the calling class depending on if the
    // welcome text is to be shown or not.
    setWizardStyle(QWizard::ModernStyle);

    connect(this, &QWizard::currentIdChanged, this, &OwncloudWizard::slotCurrentPageChanged);
    connect(_setupPage, &OwncloudSetupPage::determineAuthType, this, &OwncloudWizard::determineAuthType);
    connect(_httpCredsPage, &OwncloudHttpCredsPage::connectToOCUrl, this, &OwncloudWizard::connectToOCUrl);
    connect(_browserCredsPage, &OwncloudOAuthCredsPage::connectToOCUrl, this, &OwncloudWizard::connectToOCUrl);
    connect(_advancedSetupPage, &OwncloudAdvancedSetupPage::createLocalAndRemoteFolders,
        this, &OwncloudWizard::createLocalAndRemoteFolders);


    Theme *theme = Theme::instance();
    setWindowTitle(tr("%Assistente de Conexões do %1").arg(theme->appNameGUI()));
    setWizardStyle(QWizard::ModernStyle);
    setPixmap(QWizard::BannerPixmap, theme->wizardHeaderBanner());
    setPixmap(QWizard::LogoPixmap, theme->wizardHeaderLogo());
    setOption(QWizard::NoBackButtonOnStartPage);
    setOption(QWizard::NoBackButtonOnLastPage);
    setOption(QWizard::NoCancelButton);
    setTitleFormat(Qt::RichText);
    setSubTitleFormat(Qt::RichText);
	setButtonText(QWizard::BackButton,tr("< Voltar "));
	setButtonText(QWizard::NextButton,tr("Próximo > "));
	

}

void OwncloudWizard::setAccount(AccountPtr account)
{
    _account = account;
}

AccountPtr OwncloudWizard::account() const
{
    return _account;
}

QString OwncloudWizard::localFolder() const
{
    return (_advancedSetupPage->localFolder());
}

QStringList OwncloudWizard::selectiveSyncBlacklist() const
{
    return _advancedSetupPage->selectiveSyncBlacklist();
}

bool OwncloudWizard::useVirtualFileSync() const
{
    return _advancedSetupPage->useVirtualFileSync();
}

bool OwncloudWizard::manualFolderConfig() const
{
    return _advancedSetupPage->manualFolderConfig();
}

bool OwncloudWizard::isConfirmBigFolderChecked() const
{
    return _advancedSetupPage->isConfirmBigFolderChecked();
}

QString OwncloudWizard::ocUrl() const
{
    QString url = field("OCUrl").toString().simplified();
    return url;
}

void OwncloudWizard::enableFinishOnResultWidget(bool enable)
{
    _resultPage->setComplete(enable);
}

void OwncloudWizard::setRemoteFolder(const QString &remoteFolder)
{
    _advancedSetupPage->setRemoteFolder(remoteFolder);
    _resultPage->setRemoteFolder(remoteFolder);
}

void OwncloudWizard::successfulStep()
{
    const int id(currentId());

    switch (id) {
    case WizardCommon::Page_HttpCreds:
        _httpCredsPage->setConnected();
        break;

    case WizardCommon::Page_OAuthCreds:
        _browserCredsPage->setConnected();
        break;

    case WizardCommon::Page_AdvancedSetup:
        _advancedSetupPage->directoriesCreated();
        break;

    case WizardCommon::Page_ServerSetup:
    case WizardCommon::Page_Result:
        qCWarning(lcWizard, "Should not happen at this stage.");
        break;
    }

    next();
}

void OwncloudWizard::setAuthType(DetermineAuthTypeJob::AuthType type)
{
    _setupPage->setAuthType(type);
    if (type == DetermineAuthTypeJob::OAuth) {
        _credentialsPage = _browserCredsPage;
    } else { // try Basic auth even for "Unknown"
        _credentialsPage = _httpCredsPage;
    }
    next();
}

// TODO: update this function
void OwncloudWizard::slotCurrentPageChanged(int id)
{
    qCDebug(lcWizard) << "Current Wizard page changed to " << id;

    if (id == WizardCommon::Page_ServerSetup) {
        emit clearPendingRequests();
    }

    if (id == WizardCommon::Page_Result) {
        disconnect(this, &QDialog::finished, this, &OwncloudWizard::basicSetupFinished);
        emit basicSetupFinished(QDialog::Accepted);
        appendToConfigurationLog(QString());
        // Immediately close on show, we currently don't want this page anymore
        done(Accepted);
    }

    if (id == WizardCommon::Page_AdvancedSetup && _credentialsPage == _browserCredsPage) {
        // For OAuth, disable the back button in the Page_AdvancedSetup because we don't want
        // to re-open the browser.
        button(QWizard::BackButton)->setEnabled(false);
    }
}

void OwncloudWizard::displayError(const QString &msg, bool retryHTTPonly)
{
    switch (currentId()) {
    case WizardCommon::Page_ServerSetup:
        _setupPage->setErrorString(msg, retryHTTPonly);
        break;

    case WizardCommon::Page_HttpCreds:
        _httpCredsPage->setErrorString(msg);
        break;

    case WizardCommon::Page_AdvancedSetup:
        _advancedSetupPage->setErrorString(msg);
        break;
    }
}

void OwncloudWizard::appendToConfigurationLog(const QString &msg, LogType /*type*/)
{
    _setupLog << msg;
    qCDebug(lcWizard) << "Setup-Log: " << msg;
}

void OwncloudWizard::setOCUrl(const QString &url)
{
    _setupPage->setServerUrl(url);
}

AbstractCredentials *OwncloudWizard::getCredentials() const
{
    if (_credentialsPage) {
        return _credentialsPage->getCredentials();
    }

    return 0;
}

void OwncloudWizard::askExperimentalVirtualFilesFeature(const std::function<void(bool enable)> &callback)
{
    const auto bestVfsMode = bestAvailableVfsMode();
    QMessageBox *msgBox = nullptr;
    if (bestVfsMode == Vfs::WindowsCfApi) {
        msgBox = new QMessageBox(
            QMessageBox::Warning,
            tr("Ativar recurso experimental?"),
            tr("Quando o modo &quot;arquivos virtuais&quot; estiver habilitado, nenhum arquivo será baixado inicialmente. "
               "Em vez disso, um pequeno arquivo &quot;%1&quot; será criado para cada arquivo existente no servidor. "
               "Quando um arquivo for aberto ele será baixado automaticamente "
               "Alternativamente, você pode habilitar para baixar manualmente"
               "\n\n"
               "Os arquivos virtuais é compativel com outras formas de sincronização "
               "Se não estiver selecionado, os arquivos serão encaminhados para o site"
               "e suas opções de sincronização serão resetadas."));
        msgBox->addButton(tr("Habilitar arquivos virtuais?"), QMessageBox::AcceptRole);
        msgBox->addButton(tr("Prossiga para selecionar opção de sincronização"), QMessageBox::RejectRole);
    } else {
        ASSERT(bestVfsMode == Vfs::WithSuffix)
        msgBox = new QMessageBox(
            QMessageBox::Warning,
            tr("Ativar recurso experimental?"),
            tr("Quando o modo &quot;arquivos virtuais&quot; estiver habilitado, nenhum arquivo será baixado inicialmente. "
               "Em vez disso, um pequeno arquivo &quot;%1&quot; será criado para cada arquivo existente no servidor. "
               "O conteúdo pode ser baixado executando esses arquivos ou usando seu menu de contexto."
               "\n\n"
               "O modo de arquivos virtuais é mutuamente exclusivo com a sincronização seletiva. "
               " As pastas atualmente não selecionadas serão traduzidas para pastas somente on-line e "
               "suas configurações de sincronização seletiva serão redefinidas. "
               "\n\n"
               "Mudar para este modo abortará qualquer sincronização em execução no momento."
               "\n\n"
               "Este é um novo modo experimental. Se você decidir usá-lo, relate quaisquer  "
               "problemas que surgirem.")
                .arg(APPLICATION_DOTVIRTUALFILE_SUFFIX));
        msgBox->addButton(tr("Habilitar o modo experimental"), QMessageBox::AcceptRole);
        msgBox->addButton(tr("Fique seguro"), QMessageBox::RejectRole);
    }
    connect(msgBox, &QMessageBox::finished, msgBox, [callback, msgBox](int result) {
        callback(result == QMessageBox::AcceptRole);
        msgBox->deleteLater();
    });
    msgBox->open();
}

} // end namespace
