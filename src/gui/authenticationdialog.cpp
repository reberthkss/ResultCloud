/*
 * Copyright (C) 2014 by Daniel Molkentin <danimo@owncloud.com>
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

#include "authenticationdialog.h"

#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QDialogButtonBox>

namespace OCC {

AuthenticationDialog::AuthenticationDialog(const QString &realm, const QString &domain, QWidget *parent)
    : QDialog(parent)
    , _user(new QLineEdit)
    , _password(new QLineEdit)
{
    setWindowTitle(tr("Autenticação é Requerida"));
    QVBoxLayout *lay = new QVBoxLayout(this);
    QLabel *label = new QLabel(tr("Entrar com o nome do usuário e senha para &apos;%1&apos; em %2.").arg(realm, domain));
    label->setTextFormat(Qt::PlainText);
    lay->addWidget(label);

    QFormLayout *form = new QFormLayout;
    form->addRow(tr("&amp;Usuário:"), _user);
    form->addRow(tr("&amp;Senha:"), _password);
    lay->addLayout(form);
    _password->setEchoMode(QLineEdit::Password);

    QDialogButtonBox *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(box);
}

QString AuthenticationDialog::user() const
{
    return _user->text();
}

QString AuthenticationDialog::password() const
{
    return _password->text();
}

} // namespace OCC
