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

#include "logbrowser.h"

#include "stdio.h"
#include <iostream>

#include <QDialogButtonBox>
#include <QLayout>
#include <QPushButton>
#include <QLabel>
#include <QDir>
#include <QTextStream>
#include <QMessageBox>
#include <QCoreApplication>
#include <QSettings>
#include <QAction>
#include <QDesktopServices>

#include "configfile.h"
#include "logger.h"

namespace OCC {

// ==============================================================================

const std::chrono::hours defaultExpireDuration(4);

LogBrowser::LogBrowser(QWidget *parent)
    : QDialog(parent)
{
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    setObjectName("LogBrowser"); // for save/restoreGeometry()
    setWindowTitle(tr("Log Output"));
    setMinimumWidth(600);

    QVBoxLayout *mainLayout = new QVBoxLayout;

    auto label = new QLabel(
        tr("O cliente pode gravar logs de depuração em uma pasta temporária.  "
           "Esses logs são muito úteis para diagnosticar problemas.\n"
           "Como os arquivos de log podem ficar grandes, o cliente iniciará um novo para cada  "
           " execução de sincronização e compactará os mais antigos. Também é recomendável ativar a exclusão de arquivos de log "
           "após algumas horas para evitar o consumo excessivo de espaço em disco.\n"
           "Se ativado, os registros serão gravados em %1")
        .arg(Logger::instance()->temporaryFolderLogDirPath()));
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::MinimumExpanding);
    mainLayout->addWidget(label);

    // button to permanently save logs
    auto enableLoggingButton = new QCheckBox;
    enableLoggingButton->setText(tr("Ativar o log para a pasta temporária"));
    enableLoggingButton->setChecked(ConfigFile().automaticLogDir());
    connect(enableLoggingButton, &QCheckBox::toggled, this, &LogBrowser::togglePermanentLogging);
    mainLayout->addWidget(enableLoggingButton);

    auto deleteLogsButton = new QCheckBox;
    deleteLogsButton->setText(tr("Excluir logs com mais de %1 horas").arg(QString::number(defaultExpireDuration.count())));
    deleteLogsButton->setChecked(bool(ConfigFile().automaticDeleteOldLogsAge()));
    connect(deleteLogsButton, &QCheckBox::toggled, this, &LogBrowser::toggleLogDeletion);
    mainLayout->addWidget(deleteLogsButton);

    label = new QLabel(
        tr("Essas configurações persistem nas reinicializações do cliente.\n"
           "Observe que o uso de qualquer opção de linha de comando de registro substituirá as configurações."));
    label->setWordWrap(true);
    label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::MinimumExpanding);
    mainLayout->addWidget(label);

    auto openFolderButton = new QPushButton;
    openFolderButton->setText(tr("Abrir pasta"));
    connect(openFolderButton, &QPushButton::clicked, this, []() {
        QString path = Logger::instance()->temporaryFolderLogDirPath();
        QDir().mkpath(path);
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    });
    mainLayout->addWidget(openFolderButton);

    QDialogButtonBox *btnbox = new QDialogButtonBox;
    QPushButton *closeBtn = btnbox->addButton(tr("FECHAR"),QDialogButtonBox::Close);
    connect(closeBtn, &QAbstractButton::clicked, this, &QWidget::close);

    mainLayout->addStretch();
    mainLayout->addWidget(btnbox);

    setLayout(mainLayout);

    setModal(false);

    QAction *showLogWindow = new QAction(this);
    showLogWindow->setShortcut(QKeySequence("F12"));
    connect(showLogWindow, &QAction::triggered, this, &QWidget::close);
    addAction(showLogWindow);

    ConfigFile cfg;
    cfg.restoreGeometry(this);
}

LogBrowser::~LogBrowser()
{
}

void LogBrowser::setupLoggingFromConfig()
{
    ConfigFile config;
    auto logger = Logger::instance();

    if (config.automaticLogDir()) {
        // Don't override other configured logging
        if (logger->isLoggingToFile())
            return;

        logger->setupTemporaryFolderLogDir();
        if (auto deleteOldLogsHours = config.automaticDeleteOldLogsAge()) {
            logger->setLogExpire(*deleteOldLogsHours);
        } else {
            logger->setLogExpire(std::chrono::hours(0));
        }
        logger->enterNextLogFile();
    } else {
        logger->disableTemporaryFolderLogDir();
    }
}

void LogBrowser::togglePermanentLogging(bool enabled)
{
    ConfigFile config;
    config.setAutomaticLogDir(enabled);
    setupLoggingFromConfig();
}

void LogBrowser::toggleLogDeletion(bool enabled)
{
    ConfigFile config;
    auto logger = Logger::instance();

    if (enabled) {
        config.setAutomaticDeleteOldLogsAge(defaultExpireDuration);
        logger->setLogExpire(defaultExpireDuration);
    } else {
        config.setAutomaticDeleteOldLogsAge({});
        logger->setLogExpire(std::chrono::hours(0));
    }
}

} // namespace
