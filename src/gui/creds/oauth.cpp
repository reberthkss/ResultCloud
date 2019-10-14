/*
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

#include <QDesktopServices>
#include <QNetworkReply>
#include <QTimer>
#include <QBuffer>
#include "account.h"
#include "creds/oauth.h"
#include <QJsonObject>
#include <QJsonDocument>
#include "theme.h"
#include "networkjobs.h"
#include "creds/httpcredentials.h"

namespace OCC {

Q_LOGGING_CATEGORY(lcOauth, "sync.credentials.oauth", QtInfoMsg)

OAuth::~OAuth()
{
}

static void httpReplyAndClose(QTcpSocket *socket, const char *code, const char *html,
    const char *moreHeaders = nullptr)
{
    if (!socket)
        return; // socket can have been deleted if the browser was closed
    socket->write("HTTP/1.1 ");
    socket->write(code);
    socket->write("\r\nContent-Type: text/html\r\nConnection: close\r\nContent-Length: ");
    socket->write(QByteArray::number(qstrlen(html)));
    if (moreHeaders) {
        socket->write("\r\n");
        socket->write(moreHeaders);
    }
    socket->write("\r\n\r\n");
    socket->write(html);
    socket->disconnectFromHost();
    // We don't want that deleting the server too early prevent queued data to be sent on this socket.
    // The socket will be deleted after disconnection because disconnected is connected to deleteLater
    socket->setParent(nullptr);
}

void OAuth::start()
{
    // Listen on the socket to get a port which will be used in the redirect_uri
    if (!_server.listen(QHostAddress::LocalHost)) {
        emit result(NotSupported, QString());
        return;
    }

    if (!openBrowser())
        return;

    QObject::connect(&_server, &QTcpServer::newConnection, this, [this] {
        while (QPointer<QTcpSocket> socket = _server.nextPendingConnection()) {
            QObject::connect(socket.data(), &QTcpSocket::disconnected, socket.data(), &QTcpSocket::deleteLater);
            QObject::connect(socket.data(), &QIODevice::readyRead, this, [this, socket] {
                QByteArray peek = socket->peek(qMin(socket->bytesAvailable(), 4000LL)); //The code should always be within the first 4K
                if (peek.indexOf('\n') < 0)
                    return; // wait until we find a \n
                QRegExp rx("^GET /\\?code=([a-zA-Z0-9]+)[& ]"); // Match a  /?code=...  URL
                if (rx.indexIn(peek) != 0) {
                    httpReplyAndClose(socket, "404 Not Found", "<html><head><title>404 Not Found</title></head><body><center><h1>404 Not Found</h1></center></body></html>");
                    return;
                }

                QString code = rx.cap(1); // The 'code' is the first capture of the regexp

                QUrl requestToken = Utility::concatUrlPath(_account->url().toString(), QLatin1String("/index.php/apps/oauth2/api/v1/token"));
                QNetworkRequest req;
                req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");

                QString basicAuth = QString("%1:%2").arg(
                    Theme::instance()->oauthClientId(), Theme::instance()->oauthClientSecret());
                req.setRawHeader("Authorization", "Basic " + basicAuth.toUtf8().toBase64());
                // We just added the Authorization header, don't let HttpCredentialsAccessManager tamper with it
                req.setAttribute(HttpCredentials::DontAddCredentialsAttribute, true);

                auto requestBody = new QBuffer;
                QUrlQuery arguments(QString(
                    "grant_type=authorization_code&code=%1&redirect_uri=http://localhost:%2")
                                        .arg(code, QString::number(_server.serverPort())));
                requestBody->setData(arguments.query(QUrl::FullyEncoded).toLatin1());

                auto job = _account->sendRequest("POST", requestToken, req, requestBody);
                job->setTimeout(qMin(30 * 1000ll, job->timeoutMsec()));
                QObject::connect(job, &SimpleNetworkJob::finishedSignal, this, [this, socket](QNetworkReply *reply) {
                    auto jsonData = reply->readAll();
                    QJsonParseError jsonParseError;
                    QJsonObject json = QJsonDocument::fromJson(jsonData, &jsonParseError).object();
                    QString accessToken = json["access_token"].toString();
                    QString refreshToken = json["refresh_token"].toString();
                    QString user = json["user_id"].toString();
                    QUrl messageUrl = json["message_url"].toString();

                    if (reply->error() != QNetworkReply::NoError || jsonParseError.error != QJsonParseError::NoError
                        || jsonData.isEmpty() || json.isEmpty() || refreshToken.isEmpty() || accessToken.isEmpty()
                        || json["token_type"].toString() != QLatin1String("Bearer")) {
                        QString errorReason;
                        QString errorFromJson = json["error"].toString();
                        if (!errorFromJson.isEmpty()) {
                            errorReason = tr("Erro retornado do servidor: &lt;em&gt;%1&lt;/em&gt;")
                                              .arg(errorFromJson.toHtmlEscaped());
                        } else if (reply->error() != QNetworkReply::NoError) {
                            errorReason = tr("Ocorreu um erro ao acessar o ponto final do token: &lt;br&gt;&lt;em&gt;%1&lt;/em&gt;")
                                              .arg(reply->errorString().toHtmlEscaped());
                        } else if (jsonData.isEmpty()) {
                            // Can happen if a funky load balancer strips away POST data, e.g. BigIP APM my.policy
                            errorReason = tr("JSON vazio do redirecionamento OAuth2");
                            // We explicitly have this as error case since the json qcWarning output below is misleading,
                            // it will show a fake json will null values that actually never was received like this as
                            // soon as you access json["whatever"] the debug output json will claim to have "whatever":null
                        } else if (jsonParseError.error != QJsonParseError::NoError) {
                            errorReason = tr("Não foi possível analisar o JSON retornado do servidor: &lt;br&gt;&lt;em&gt;%1&lt;/em&gt;")
                                              .arg(jsonParseError.errorString());
                        } else {
                            errorReason = tr("A resposta do servidor não continha todos os campos esperados");
                        }
                        qCWarning(lcOauth) << "Error when getting the accessToken" << json << errorReason;
                        httpReplyAndClose(socket, "500 Internal Server Error",
                            tr("&lt;h1&gt;Erro de Login&lt;/h1&gt;&lt;p&gt;%1&lt;/p&gt;").arg(errorReason).toUtf8().constData());
                        emit result(Error);
                        return;
                    }
                    if (!_expectedUser.isNull() && user != _expectedUser) {
                        // Connected with the wrong user
                        QString message = tr("&lt;h1&gt;Usuário errado&lt;/h1&gt;"
                                             "&lt;p&gt;Você fez logon com o usuário &lt;em&gt;%1&lt;/em&gt;, mas deve fazer login com o usuário &lt;em&gt;%2&lt;/em&gt;.&lt;br&gt;"
                                             "Faça o logout de %3 em outra guia, então&lt;a href=&apos;%4&apos;&gt;clique aqui&lt;/a&gt; "
                                             "e faça o login como usuário%2&lt;/p&gt;")
                                              .arg(user, _expectedUser, Theme::instance()->appNameGUI(),
                                                  authorisationLink().toString(QUrl::FullyEncoded));
                        httpReplyAndClose(socket, "200 OK", message.toUtf8().constData());
                        // We are still listening on the socket so we will get the new connection
                        return;
                    }
                    const char *loginSuccessfullHtml = "<h1>Logado com sucesso! </h1><p>Você pode fechar está janela</p>";
                    if (messageUrl.isValid()) {
                        httpReplyAndClose(socket, "303 See Other", loginSuccessfullHtml,
                            QByteArray("Location: " + messageUrl.toEncoded()).constData());
                    } else {
                        httpReplyAndClose(socket, "200 OK", loginSuccessfullHtml);
                    }
                    emit result(LoggedIn, user, accessToken, refreshToken);
                });
            });
        }
    });
}

QUrl OAuth::authorisationLink() const
{
    Q_ASSERT(_server.isListening());
    QUrlQuery query;
    query.setQueryItems({ { QLatin1String("response_type"), QLatin1String("code") },
        { QLatin1String("client_id"), Theme::instance()->oauthClientId() },
        { QLatin1String("redirect_uri"), QLatin1String("http://localhost:") + QString::number(_server.serverPort()) } });
    if (!_expectedUser.isNull())
        query.addQueryItem("user", _expectedUser);
    QUrl url = Utility::concatUrlPath(_account->url(), QLatin1String("/index.php/apps/oauth2/authorize"), query);
    return url;
}

bool OAuth::openBrowser()
{
    if (!QDesktopServices::openUrl(authorisationLink())) {
        // We cannot open the browser, then we claim we don't support OAuth.
        emit result(NotSupported, QString());
        return false;
    }
    return true;
}

} // namespace OCC
