/*
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
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

#include "theme.h"
#include "configfile.h"
#include "common/utility.h"
#include "accessmanager.h"

#include "updater/ocupdater.h"

#include <QObject>
#include <QtCore>
#include <QtNetwork>
#include <QtGui>
#include <QtWidgets>

#include <stdio.h>

namespace OCC {

static const char updateAvailableC[] = "Updater/updateAvailable";
static const char updateTargetVersionC[] = "Updater/updateTargetVersion";
static const char updateTargetVersionStringC[] = "Updater/updateTargetVersionString";
static const char seenVersionC[] = "Updater/seenVersion";
static const char autoUpdateAttemptedC[] = "Updater/autoUpdateAttempted";


UpdaterScheduler::UpdaterScheduler(QObject *parent)
    : QObject(parent)
{
    connect(&_updateCheckTimer, &QTimer::timeout,
        this, &UpdaterScheduler::slotTimerFired);

    // Note: the sparkle-updater is not an OCUpdater
    if (OCUpdater *updater = qobject_cast<OCUpdater *>(Updater::instance())) {
        connect(updater, &OCUpdater::newUpdateAvailable,
            this, &UpdaterScheduler::updaterAnnouncement);
        connect(updater, &OCUpdater::requestRestart, this, &UpdaterScheduler::requestRestart);
    }

    // at startup, do a check in any case.
    QTimer::singleShot(3000, this, &UpdaterScheduler::slotTimerFired);

    ConfigFile cfg;
    auto checkInterval = cfg.updateCheckInterval();
    _updateCheckTimer.start(std::chrono::milliseconds(checkInterval).count());
}

void UpdaterScheduler::slotTimerFired()
{
    ConfigFile cfg;

    // re-set the check interval if it changed in the config file meanwhile
    auto checkInterval = std::chrono::milliseconds(cfg.updateCheckInterval()).count();
    if (checkInterval != _updateCheckTimer.interval()) {
        _updateCheckTimer.setInterval(checkInterval);
        qCInfo(lcUpdater) << "Setting new update check interval " << checkInterval;
    }

    // consider the skipUpdateCheck flag in the config.
    if (cfg.skipUpdateCheck()) {
        qCInfo(lcUpdater) << "Skipping update check because of config file";
        return;
    }

    Updater *updater = Updater::instance();
    if (updater) {
        updater->backgroundCheckForUpdate();
    }
}


/* ----------------------------------------------------------------- */

OCUpdater::OCUpdater(const QUrl &url)
    : Updater()
    , _updateUrl(url)
    , _state(Unknown)
    , _accessManager(new AccessManager(this))
    , _timeoutWatchdog(new QTimer(this))
{
}

void OCUpdater::setUpdateUrl(const QUrl &url)
{
    _updateUrl = url;
}

bool OCUpdater::performUpdate()
{
    ConfigFile cfg;
    QSettings settings(cfg.configFile(), QSettings::IniFormat);
    QString updateFile = settings.value(updateAvailableC).toString();
    if (!updateFile.isEmpty() && QFile(updateFile).exists()
        && !updateSucceeded() /* Someone might have run the updater manually between restarts */) {
        const QString name = Theme::instance()->appNameGUI();
        if (QMessageBox::information(0, tr("Nova %1 atualização pronta").arg(name),
                tr("Uma nova atualização %1 sera iniciada. Talvez o programa solicite\n"
                   "por privilegios adicionais durante a instalação.")
                    .arg(name),
                QMessageBox::Ok)) {
            slotStartInstaller();
            return true;
        }
    }
    return false;
}

void OCUpdater::backgroundCheckForUpdate()
{
    int dlState = downloadState();

    // do the real update check depending on the internal state of updater.
    switch (dlState) {
    case Unknown:
    case UpToDate:
    case DownloadFailed:
    case DownloadTimedOut:
        qCInfo(lcUpdater) << "Checking for available update";
        checkForUpdate();
        break;
    case DownloadComplete:
        qCInfo(lcUpdater) << "Update is downloaded, skip new check.";
        break;
    case UpdateOnlyAvailableThroughSystem:
        qCInfo(lcUpdater) << "Update is only available through system, skip check.";
        break;
    }
}

QString OCUpdater::statusString() const
{
    QString updateVersion = _updateInfo.versionString();

    switch (downloadState()) {
    case Downloading:
        return tr("Baixando %1. Aguarde...").arg(updateVersion);
    case DownloadComplete:
        return tr("%1 Disponivel. Reinicie o aplicativo para atualizar.").arg(Theme::instance()->appNameGUI(), updateVersion);
    case DownloadFailed:
        return tr("Não é possivel baixar. Por favor, clique <a href='%1'>aqui</a> para baixar manualmente").arg(_updateInfo.web());
    case DownloadTimedOut:
        return tr("Não foi possível verificar novas atualizações.");
    case UpdateOnlyAvailableThroughSystem:
        return tr("Nova %1 disponivel. Por avor utilize a ferramente de atualização para prosseguir.").arg(updateVersion);
    case CheckingServer:
        return tr("Checando atualizações no servidor...");
    case Unknown:
        return tr("Status da atualização: Não foi procurada novas atualizações.");
    case UpToDate:
    // fall through
    default:
        return tr("Sem atualizações disponiveis. Sua instalação está na versão atual.");
    }
}

int OCUpdater::downloadState() const
{
    return _state;
}

void OCUpdater::setDownloadState(DownloadState state)
{
    auto oldState = _state;
    _state = state;
    emit downloadStateChanged();

    // show the notification if the download is complete (on every check)
    // or once for system based updates.
    if (_state == OCUpdater::DownloadComplete || (oldState != OCUpdater::UpdateOnlyAvailableThroughSystem
                                                     && _state == OCUpdater::UpdateOnlyAvailableThroughSystem)) {
        emit newUpdateAvailable(tr("Verificar atualização"), statusString());
    }
}

void OCUpdater::slotStartInstaller()
{
    ConfigFile cfg;
    QSettings settings(cfg.configFile(), QSettings::IniFormat);
    QString updateFile = settings.value(updateAvailableC).toString();
    settings.setValue(autoUpdateAttemptedC, true);
    settings.sync();
    qCInfo(lcUpdater) << "Running updater" << updateFile;

    if(updateFile.endsWith(".exe")) {
        QProcess::startDetached(updateFile, QStringList() << "/S"
                                                          << "/launch");
    } else if(updateFile.endsWith(".msi")) {
        // When MSIs are installed without gui they cannot launch applications
        // as they lack the user context. That is why we need to run the client
        // manually here. We wrap the msiexec and client invocation in a powershell
        // script because owncloud.exe will be shut down for installation.
        // | Out-Null forces powershell to wait for msiexec to finish.
        auto preparePathForPowershell = [](QString path) {
            path.replace("'", "''");

            return QDir::toNativeSeparators(path);
        };

        QString msiLogFile = cfg.configPath() + "msi.log";
        QString command = QString("&{msiexec /norestart /passive /i '%1' /L*V '%2'| Out-Null ; &'%3'}")
             .arg(preparePathForPowershell(updateFile))
             .arg(preparePathForPowershell(msiLogFile))
             .arg(preparePathForPowershell(QCoreApplication::applicationFilePath()));

        QProcess::startDetached("powershell.exe", QStringList{"-Command", command});
    }
}

void OCUpdater::checkForUpdate()
{
    QNetworkReply *reply = _accessManager->get(QNetworkRequest(_updateUrl));
    connect(_timeoutWatchdog, &QTimer::timeout, this, &OCUpdater::slotTimedOut);
    _timeoutWatchdog->start(30 * 1000);
    connect(reply, &QNetworkReply::finished, this, &OCUpdater::slotVersionInfoArrived);

    setDownloadState(CheckingServer);
}

void OCUpdater::slotOpenUpdateUrl()
{
    QDesktopServices::openUrl(_updateInfo.web());
}

bool OCUpdater::updateSucceeded() const
{
    ConfigFile cfg;
    QSettings settings(cfg.configFile(), QSettings::IniFormat);

    qint64 targetVersionInt = Helper::stringVersionToInt(settings.value(updateTargetVersionC).toString());
    qint64 currentVersion = Helper::currentVersionToInt();
    return currentVersion >= targetVersionInt;
}

void OCUpdater::slotVersionInfoArrived()
{
    _timeoutWatchdog->stop();
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        qCWarning(lcUpdater) << "Failed to reach version check url: " << reply->errorString();
        setDownloadState(OCUpdater::Unknown);
        return;
    }

    QString xml = QString::fromUtf8(reply->readAll());

    bool ok;
    _updateInfo = UpdateInfo::parseString(xml, &ok);
    if (ok) {
        versionInfoArrived(_updateInfo);
    } else {
        qCWarning(lcUpdater) << "Could not parse update information.";
        setDownloadState(OCUpdater::Unknown);
    }
}

void OCUpdater::slotTimedOut()
{
    setDownloadState(DownloadTimedOut);
}

////////////////////////////////////////////////////////////////////////

NSISUpdater::NSISUpdater(const QUrl &url)
    : OCUpdater(url)
{
}

void NSISUpdater::slotWriteFile()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    if (_file->isOpen()) {
        _file->write(reply->readAll());
    }
}

void NSISUpdater::wipeUpdateData()
{
    ConfigFile cfg;
    QSettings settings(cfg.configFile(), QSettings::IniFormat);
    QString updateFileName = settings.value(updateAvailableC).toString();
    if (!updateFileName.isEmpty())
        QFile::remove(updateFileName);
    settings.remove(updateAvailableC);
    settings.remove(updateTargetVersionC);
    settings.remove(updateTargetVersionStringC);
    settings.remove(autoUpdateAttemptedC);
}

void NSISUpdater::slotDownloadFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        setDownloadState(DownloadFailed);
        return;
    }

    QUrl url(reply->url());
    _file->close();

    ConfigFile cfg;
    QSettings settings(cfg.configFile(), QSettings::IniFormat);

    // remove previously downloaded but not used installer
    QFile oldTargetFile(settings.value(updateAvailableC).toString());
    if (oldTargetFile.exists()) {
        oldTargetFile.remove();
    }

    QFile::copy(_file->fileName(), _targetFile);
    setDownloadState(DownloadComplete);
    qCInfo(lcUpdater) << "Downloaded" << url.toString() << "to" << _targetFile;
    settings.setValue(updateTargetVersionC, updateInfo().version());
    settings.setValue(updateTargetVersionStringC, updateInfo().versionString());
    settings.setValue(updateAvailableC, _targetFile);
}

void NSISUpdater::versionInfoArrived(const UpdateInfo &info)
{
    ConfigFile cfg;
    QSettings settings(cfg.configFile(), QSettings::IniFormat);
    qint64 infoVersion = Helper::stringVersionToInt(info.version());
    auto seenString = settings.value(seenVersionC).toString();
    qint64 seenVersion = Helper::stringVersionToInt(seenString);
    qint64 currVersion = Helper::currentVersionToInt();
    qCInfo(lcUpdater) << "Version info arrived:"
            << "Your version:" << currVersion
            << "Skipped version:" << seenVersion << seenString
            << "Available version:" << infoVersion << info.version()
            << "Available version string:" << info.versionString()
            << "Web url:" << info.web()
            << "Download url:" << info.downloadUrl();
    if (info.version().isEmpty())
    {
        qCInfo(lcUpdater) << "No version information available at the moment";
        setDownloadState(UpToDate);
    } else if (infoVersion <= currVersion
               || infoVersion <= seenVersion) {
        qCInfo(lcUpdater) << "Client is on latest version!";
        setDownloadState(UpToDate);
    } else {
        QString url = info.downloadUrl();
        if (url.isEmpty()) {
            showNoUrlDialog(info);
        } else {
            _targetFile = cfg.configPath() + url.mid(url.lastIndexOf('/')+1);
            if (QFile(_targetFile).exists()) {
                setDownloadState(DownloadComplete);
            } else {
                QNetworkReply *reply = qnam()->get(QNetworkRequest(QUrl(url)));
                connect(reply, &QIODevice::readyRead, this, &NSISUpdater::slotWriteFile);
                connect(reply, &QNetworkReply::finished, this, &NSISUpdater::slotDownloadFinished);
                setDownloadState(Downloading);
                _file.reset(new QTemporaryFile);
                _file->setAutoRemove(true);
                _file->open();
            }
        }
    }
}

void NSISUpdater::showNoUrlDialog(const UpdateInfo &info)
{
    // if the version tag is set, there is a newer version.
    QDialog *msgBox = new QDialog;
    msgBox->setAttribute(Qt::WA_DeleteOnClose);
    msgBox->setWindowFlags(msgBox->windowFlags() & ~Qt::WindowContextHelpButtonHint);

    QIcon infoIcon = msgBox->style()->standardIcon(QStyle::SP_MessageBoxInformation);
    int iconSize = msgBox->style()->pixelMetric(QStyle::PM_MessageBoxIconSize);

    msgBox->setWindowIcon(infoIcon);

    QVBoxLayout *layout = new QVBoxLayout(msgBox);
    QHBoxLayout *hlayout = new QHBoxLayout;
    layout->addLayout(hlayout);

    msgBox->setWindowTitle(tr("Nova Versão Disponínel"));

    QLabel *ico = new QLabel;
    ico->setFixedSize(iconSize, iconSize);
    ico->setPixmap(infoIcon.pixmap(iconSize));
    QLabel *lbl = new QLabel;
    QString txt = tr("&lt;p&gt;Uma nova versão %1 de Ciente está disponível.&lt;/p&gt;"
                     "&lt;p&gt;&lt;b&gt;%2&lt;/b&gt; está disponível para baixar. A versão instalada é a %3.&lt;/p&gt;")
                      .arg(Utility::escape(Theme::instance()->appNameGUI()),
                          Utility::escape(info.versionString()), Utility::escape(clientVersion()));

    lbl->setText(txt);
    lbl->setTextFormat(Qt::RichText);
    lbl->setWordWrap(true);

    hlayout->addWidget(ico);
    hlayout->addWidget(lbl);

    QDialogButtonBox *bb = new QDialogButtonBox;
    QPushButton *skip = bb->addButton(tr("Pule esta versão"), QDialogButtonBox::ResetRole);
    QPushButton *reject = bb->addButton(tr("Pular desta vez"), QDialogButtonBox::AcceptRole);
    QPushButton *getupdate = bb->addButton(tr("Atualizar"), QDialogButtonBox::AcceptRole);

    connect(skip, &QAbstractButton::clicked, msgBox, &QDialog::reject);
    connect(reject, &QAbstractButton::clicked, msgBox, &QDialog::reject);
    connect(getupdate, &QAbstractButton::clicked, msgBox, &QDialog::accept);

    connect(skip, &QAbstractButton::clicked, this, &NSISUpdater::slotSetSeenVersion);
    connect(getupdate, &QAbstractButton::clicked, this, &NSISUpdater::slotOpenUpdateUrl);

    layout->addWidget(bb);

    msgBox->open();
}

void NSISUpdater::showUpdateErrorDialog(const QString &targetVersion)
{
    QDialog *msgBox = new QDialog;
    msgBox->setAttribute(Qt::WA_DeleteOnClose);
    msgBox->setWindowFlags(msgBox->windowFlags() & ~Qt::WindowContextHelpButtonHint);

    QIcon infoIcon = msgBox->style()->standardIcon(QStyle::SP_MessageBoxInformation);
    int iconSize = msgBox->style()->pixelMetric(QStyle::PM_MessageBoxIconSize);

    msgBox->setWindowIcon(infoIcon);

    QVBoxLayout *layout = new QVBoxLayout(msgBox);
    QHBoxLayout *hlayout = new QHBoxLayout;
    layout->addLayout(hlayout);

    msgBox->setWindowTitle(tr("Atualização Falhou"));

    QLabel *ico = new QLabel;
    ico->setFixedSize(iconSize, iconSize);
    ico->setPixmap(infoIcon.pixmap(iconSize));
    QLabel *lbl = new QLabel;
    QString txt = tr("&lt;p&gt;Uma nova versão do %1 Cliente está disponível, mas o processo de atualização falhou.&lt;/p&gt;&lt;p&gt;&lt;b&gt;%2&lt;/b&gt; "
                     "foi baixado. A versão instalada é %3.&lt;/p&gt;")
                      .arg(Utility::escape(Theme::instance()->appNameGUI()),
                          Utility::escape(targetVersion), Utility::escape(clientVersion()));

    lbl->setText(txt);
    lbl->setTextFormat(Qt::RichText);
    lbl->setWordWrap(true);

    hlayout->addWidget(ico);
    hlayout->addWidget(lbl);

    QDialogButtonBox *bb = new QDialogButtonBox;
    QPushButton *skip = bb->addButton(tr("Pule esta versão"), QDialogButtonBox::ResetRole);
    QPushButton *askagain = bb->addButton(tr("Pergunte novamente mais tarde"), QDialogButtonBox::ResetRole);
    QPushButton *retry = bb->addButton(tr("Reinicie e atualize"), QDialogButtonBox::AcceptRole);
    QPushButton *getupdate = bb->addButton(tr("Atualizar manualmente"), QDialogButtonBox::AcceptRole);

    connect(skip, &QAbstractButton::clicked, msgBox, &QDialog::reject);
    connect(askagain, &QAbstractButton::clicked, msgBox, &QDialog::reject);
    connect(retry, &QAbstractButton::clicked, msgBox, &QDialog::accept);
    connect(getupdate, &QAbstractButton::clicked, msgBox, &QDialog::accept);

    connect(skip, &QAbstractButton::clicked, this, [this]() {
        wipeUpdateData();
        slotSetSeenVersion();
    });
    // askagain: do nothing
    connect(retry, &QAbstractButton::clicked, this, [this]() {
        slotStartInstaller();
        qApp->quit();
    });
    connect(getupdate, &QAbstractButton::clicked, this, [this]() {
        slotOpenUpdateUrl();
    });

    layout->addWidget(bb);

    msgBox->open();
}

bool NSISUpdater::handleStartup()
{
    ConfigFile cfg;
    QSettings settings(cfg.configFile(), QSettings::IniFormat);
    QString updateFileName = settings.value(updateAvailableC).toString();
    // has the previous run downloaded an update?
    if (!updateFileName.isEmpty() && QFile(updateFileName).exists()) {
        qCInfo(lcUpdater) << "An updater file is available";
        // did it try to execute the update?
        if (settings.value(autoUpdateAttemptedC, false).toBool()) {
            if (updateSucceeded()) {
                // success: clean up
                qCInfo(lcUpdater) << "The requested update attempt has succeeded"
                        << Helper::currentVersionToInt();
                wipeUpdateData();
                return false;
            } else {
                // auto update failed. Ask user what to do
                qCInfo(lcUpdater) << "The requested update attempt has failed"
                        << settings.value(updateTargetVersionC).toString();
                showUpdateErrorDialog(settings.value(updateTargetVersionStringC).toString());
                return false;
            }
        } else {
            qCInfo(lcUpdater) << "Triggering an update";
            return performUpdate();
        }
    }
    return false;
}

void NSISUpdater::slotSetSeenVersion()
{
    ConfigFile cfg;
    QSettings settings(cfg.configFile(), QSettings::IniFormat);
    settings.setValue(seenVersionC, updateInfo().version());
}

////////////////////////////////////////////////////////////////////////

PassiveUpdateNotifier::PassiveUpdateNotifier(const QUrl &url)
    : OCUpdater(url)
{
    // remember the version of the currently running binary. On Linux it might happen that the
    // package management updates the package while the app is running. This is detected in the
    // updater slot: If the installed binary on the hd has a different version than the one
    // running, the running app is restarted. That happens in folderman.
    _runningAppVersion = Utility::versionOfInstalledBinary();
}

void PassiveUpdateNotifier::backgroundCheckForUpdate()
{
    if (Utility::isLinux()) {
        // on linux, check if the installed binary is still the same version
        // as the one that is running. If not, restart if possible.
        const QByteArray fsVersion = Utility::versionOfInstalledBinary();
        if (!(fsVersion.isEmpty() || _runningAppVersion.isEmpty()) && fsVersion != _runningAppVersion) {
            emit requestRestart();
        }
    }

    OCUpdater::backgroundCheckForUpdate();
}

void PassiveUpdateNotifier::versionInfoArrived(const UpdateInfo &info)
{
    qint64 currentVer = Helper::currentVersionToInt();
    qint64 remoteVer = Helper::stringVersionToInt(info.version());

    if (info.version().isEmpty() || currentVer >= remoteVer) {
        qCInfo(lcUpdater) << "Client is on latest version!";
        setDownloadState(UpToDate);
    } else {
        setDownloadState(UpdateOnlyAvailableThroughSystem);
    }
}

} // ns mirall
