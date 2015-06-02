/*
  This file is part of the PhantomJS project from Ofi Labs.

  Copyright (C) 2011 Ariya Hidayat <ariya.hidayat@gmail.com>
  Copyright (C) 2011 Ivan De Marino <ivan.de.marino@gmail.com>

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the <organization> nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  ARE DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
  DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
  (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
  THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <QAuthenticator>
#include <QDateTime>
#include <QDesktopServices>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslSocket>
#include <QSslCertificate>
#include <QSslCipher>
#include <QRegExp>

#include "phantom.h"
#include "config.h"
#include "cookiejar.h"
#include "networkaccessmanager.h"
#include "networkreplyproxy.h"

// 10 MB
const qint64 MAX_REQUEST_POST_BODY_SIZE = 10 * 1000 * 1000;

static const char *toString(QNetworkAccessManager::Operation op)
{
    const char *str = 0;
    switch (op) {
    case QNetworkAccessManager::HeadOperation:
        str = "HEAD";
        break;
    case QNetworkAccessManager::GetOperation:
        str = "GET";
        break;
    case QNetworkAccessManager::PutOperation:
        str = "PUT";
        break;
    case QNetworkAccessManager::PostOperation:
        str = "POST";
        break;
    case QNetworkAccessManager::DeleteOperation:
        str = "DELETE";
        break;
    default:
        str = "?";
        break;
    }
    return str;
}

// Stub QNetworkReply used when file:/// URLs are disabled.
// Somewhat cargo-culted from QDisabledNetworkReply.

NoFileAccessReply::NoFileAccessReply(QObject *parent, const QNetworkRequest &req, const QNetworkAccessManager::Operation op)
    : QNetworkReply(parent)
{
    setRequest(req);
    setUrl(req.url());
    setOperation(op);

    qRegisterMetaType<QNetworkReply::NetworkError>();
    QString msg = (QCoreApplication::translate("QNetworkReply", "Protocol \"%1\" is unknown")
                   .arg(req.url().scheme()));
    setError(ProtocolUnknownError, msg);

    QMetaObject::invokeMethod(this, "error", Qt::QueuedConnection,
                              Q_ARG(QNetworkReply::NetworkError, ProtocolUnknownError));
    QMetaObject::invokeMethod(this, "finished", Qt::QueuedConnection);
}

// The destructor must be out-of-line in order to trigger generation of the vtable.
NoFileAccessReply::~NoFileAccessReply() {}


TimeoutTimer::TimeoutTimer(QObject* parent)
    : QTimer(parent)
{
}


JsNetworkRequest::JsNetworkRequest(QNetworkRequest* request, QObject* parent)
    : QObject(parent)
{
    m_networkRequest = request;
}

void JsNetworkRequest::abort()
{
    if (m_networkRequest) {
        m_networkRequest->setUrl(QUrl());
    }
}

bool JsNetworkRequest::setHeader(const QString& name, const QVariant& value)
{
    if (!m_networkRequest)
        return false;

    // Pass `null` as the second argument to remove a HTTP header
    m_networkRequest->setRawHeader(name.toLatin1(), value.toByteArray());
    return true;
}

void JsNetworkRequest::changeUrl(const QString& address)
{
    if (m_networkRequest) {
        QUrl url = QUrl::fromEncoded(address.toLatin1());
        m_networkRequest->setUrl(url);
    }
}

struct ssl_protocol_option {
  const char* name;
  QSsl::SslProtocol proto;
};
const ssl_protocol_option ssl_protocol_options[] = {
  { "default", QSsl::SecureProtocols },
  { "tlsv1.2", QSsl::TlsV1_2 },
  { "tlsv1.1", QSsl::TlsV1_1 },
  { "tlsv1.0", QSsl::TlsV1_0 },
  { "tlsv1",   QSsl::TlsV1_0 },
  { "sslv3",   QSsl::SslV3 },
  { "any",     QSsl::AnyProtocol },
  { 0,         QSsl::UnknownProtocol }
};

// public:
NetworkAccessManager::NetworkAccessManager(QObject *parent, const Config *config)
    : QNetworkAccessManager(parent)
    , m_ignoreSslErrors(config->ignoreSslErrors())
    , m_localUrlAccessEnabled(config->localUrlAccessEnabled())
    , m_authAttempts(0)
    , m_maxAuthAttempts(3)
    , m_resourceTimeout(0)
    , m_idCounter(0)
    , m_networkDiskCache(0)
    , m_sslConfiguration(QSslConfiguration::defaultConfiguration())
    , m_replyTracker(this)
{
    if (config->diskCacheEnabled()) {
        m_networkDiskCache = new QNetworkDiskCache(this);
        m_networkDiskCache->setCacheDirectory(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
        if (config->maxDiskCacheSize() >= 0)
            m_networkDiskCache->setMaximumCacheSize(qint64(config->maxDiskCacheSize()) * 1024);
        setCache(m_networkDiskCache);
    }

    if (QSslSocket::supportsSsl()) {
        m_sslConfiguration = QSslConfiguration::defaultConfiguration();

        if (config->ignoreSslErrors()) {
            m_sslConfiguration.setPeerVerifyMode(QSslSocket::VerifyNone);
        }

        bool setProtocol = false;
        for (const ssl_protocol_option *proto_opt = ssl_protocol_options;
             proto_opt->name;
             proto_opt++) {
            if (config->sslProtocol() == proto_opt->name) {
                m_sslConfiguration.setProtocol(proto_opt->proto);
                setProtocol = true;
                break;
            }
        }
        // FIXME: actually object to an invalid setting.
        if (!setProtocol) {
            m_sslConfiguration.setProtocol(QSsl::SecureProtocols);
        }

        // Essentially the same as what QSslSocket::setCiphers(QString) does.
        // That overload isn't available on QSslConfiguration.
        if (!config->sslCiphers().isEmpty()) {
            QList<QSslCipher> cipherList;
            foreach (const QString &cipherName,
                     config->sslCiphers().split(QLatin1String(":"),
                                                QString::SkipEmptyParts)) {
                QSslCipher cipher(cipherName);
                if (!cipher.isNull())
                    cipherList << cipher;
            }
            if (!cipherList.isEmpty())
                m_sslConfiguration.setCiphers(cipherList);
        }

        if (!config->sslCertificatesPath().isEmpty()) {
          QList<QSslCertificate> caCerts = QSslCertificate::fromPath(
              config->sslCertificatesPath(), QSsl::Pem, QRegExp::Wildcard);

            m_sslConfiguration.setCaCertificates(caCerts);
        }
    }

    connect(this, SIGNAL(authenticationRequired(QNetworkReply*,QAuthenticator*)), SLOT(provideAuthentication(QNetworkReply*,QAuthenticator*)));

    connect(&m_replyTracker, SIGNAL(started(QNetworkReply*, int)), this,  SLOT(handleStarted(QNetworkReply*, int)));
    connect(&m_replyTracker, SIGNAL(sslErrors(QNetworkReply*, const QList<QSslError> &)), this, SLOT(handleSslErrors(QNetworkReply*, const QList<QSslError> &)));
    connect(&m_replyTracker, SIGNAL(error(QNetworkReply*, int, QNetworkReply::NetworkError)), this, SLOT(handleNetworkError(QNetworkReply*, int)));
    connect(&m_replyTracker, SIGNAL(finished(QNetworkReply *, int, int, const QString&, const QString&)), SLOT(handleFinished(QNetworkReply *, int, int, const QString&, const QString&)));
}

void NetworkAccessManager::setUserName(const QString &userName)
{
    m_userName = userName;
}

void NetworkAccessManager::setPassword(const QString &password)
{
    m_password = password;
}

void NetworkAccessManager::setResourceTimeout(int resourceTimeout)
{
    m_resourceTimeout = resourceTimeout;
}

void NetworkAccessManager::setMaxAuthAttempts(int maxAttempts)
{
    m_maxAuthAttempts = maxAttempts;
}

void NetworkAccessManager::setCustomHeaders(const QVariantMap &headers)
{
    m_customHeaders = headers;
}

QVariantMap NetworkAccessManager::customHeaders() const
{
    return m_customHeaders;
}

QStringList NetworkAccessManager::captureContent() const
{
    return m_captureContentPatterns;
}

void NetworkAccessManager::setCaptureContent(const QStringList &patterns)
{
    m_captureContentPatterns = patterns;

    compileCaptureContentPatterns();
}

void NetworkAccessManager::compileCaptureContentPatterns()
{
    for(QStringList::const_iterator it = m_captureContentPatterns.constBegin();
        it != m_captureContentPatterns.constEnd(); ++it) {

        m_compiledCaptureContentPatterns.append(QRegExp(*it, Qt::CaseInsensitive));
    }
}


void NetworkAccessManager::setCookieJar(QNetworkCookieJar *cookieJar)
{
    QNetworkAccessManager::setCookieJar(cookieJar);
    // Remove NetworkAccessManager's ownership of this CookieJar and
    // pass it to the PhantomJS Singleton object.
    // CookieJar is shared between multiple instances of NetworkAccessManager.
    // It shouldn't be deleted when the NetworkAccessManager is deleted, but
    // only when close is called on the cookie jar.
    cookieJar->setParent(Phantom::instance());
}

// protected:
QNetworkReply *NetworkAccessManager::createRequest(Operation op, const QNetworkRequest & request, QIODevice * outgoingData)
{
    QNetworkRequest req(request);
    QString scheme = req.url().scheme().toLower();
    bool isLocalFile = req.url().isLocalFile();

    if (!QSslSocket::supportsSsl()) {
      if (scheme == QLatin1String("https"))
        qWarning() << "Request using https scheme without SSL support";
    } else {
        req.setSslConfiguration(m_sslConfiguration);
    }

    // Get the URL string before calling the superclass. Seems to work around
    // segfaults in Qt 4.8: https://gist.github.com/1430393
    QByteArray url = req.url().toEncoded();
    QByteArray postData;

    // http://code.google.com/p/phantomjs/issues/detail?id=337
    if (op == QNetworkAccessManager::PostOperation) {
        if (outgoingData) postData = outgoingData->peek(MAX_REQUEST_POST_BODY_SIZE);
        QString contentType = req.header(QNetworkRequest::ContentTypeHeader).toString();
        if (contentType.isEmpty()) {
            req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
        }
    }

    // set custom HTTP headers
    QVariantMap::const_iterator i = m_customHeaders.begin();
    while (i != m_customHeaders.end()) {
        req.setRawHeader(i.key().toLatin1(), i.value().toByteArray());
        ++i;
    }

    m_idCounter++;

    QVariantList headers;
    foreach (QByteArray headerName, req.rawHeaderList()) {
        QVariantMap header;
        header["name"] = QString::fromUtf8(headerName);
        header["value"] = QString::fromUtf8(req.rawHeader(headerName));
        headers += header;
    }

    QVariantMap data;
    data["id"] = m_idCounter;
    data["url"] = url.data();
    data["method"] = toString(op);
    data["headers"] = headers;
    if (op == QNetworkAccessManager::PostOperation) data["postData"] = postData.data();
    data["time"] = QDateTime::currentDateTime();

    JsNetworkRequest jsNetworkRequest(&req, this);
    emit resourceRequested(data, &jsNetworkRequest);

    QNetworkReply *nested_reply = QNetworkAccessManager::createRequest(op, req, outgoingData);
    QNetworkReply *tracked_reply = m_replyTracker.trackReply(nested_reply,
                                                             m_idCounter,
                                                             shouldCaptureResponse(req.url().toString()));

    // reparent jsNetworkRequest to make sure that it will be destroyed with QNetworkReply
    jsNetworkRequest.setParent(tracked_reply);

    // If there is a timeout set, create a TimeoutTimer
    if(m_resourceTimeout > 0){

        TimeoutTimer *nt = new TimeoutTimer(tracked_reply);
        nt->reply = tracked_reply; // We need the reply object in order to abort it later on.
        nt->data = data;
        nt->setInterval(m_resourceTimeout);
        nt->setSingleShot(true);
        nt->start();

        connect(nt, SIGNAL(timeout()), this, SLOT(handleTimeout()));
    }

    return tracked_reply;
}

bool NetworkAccessManager::shouldCaptureResponse(const QString& url)
{
    for(QList<QRegExp>::const_iterator it = m_compiledCaptureContentPatterns.constBegin();
        it != m_compiledCaptureContentPatterns.constEnd(); ++it) {

        if(-1 != it->indexIn(url)) {
            return true;
        }
    }

    return false;
}

void NetworkAccessManager::handleTimeout()
{
    TimeoutTimer *nt = qobject_cast<TimeoutTimer*>(sender());

    if(!nt->reply)
        return;

    nt->data["errorCode"] = 408;
    nt->data["errorString"] = "Network timeout on resource.";

    emit resourceTimeout(nt->data);

    // Abort the reply that we attached to the Network Timeout
    nt->reply->abort();
}

void NetworkAccessManager::handleStarted(QNetworkReply* reply, int requestId)
{
    QVariantList headers;
    foreach (QByteArray headerName, reply->rawHeaderList()) {
        QVariantMap header;
        header["name"] = QString::fromUtf8(headerName);
        header["value"] = QString::fromUtf8(reply->rawHeader(headerName));
        headers += header;
    }

    QVariantMap data;
    data["stage"] = "start";
    data["id"] = requestId;
    data["url"] = reply->url().toEncoded().data();
    data["status"] = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    data["statusText"] = reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute);
    data["contentType"] = reply->header(QNetworkRequest::ContentTypeHeader);
    data["bodySize"] = reply->size();
    data["redirectURL"] = reply->header(QNetworkRequest::LocationHeader);
    data["headers"] = headers;
    data["time"] = QDateTime::currentDateTime();
    data["body"] = "";

    emit resourceReceived(data);
}

void NetworkAccessManager::provideAuthentication(QNetworkReply *reply, QAuthenticator *authenticator)
{
    if (m_authAttempts++ < m_maxAuthAttempts)
    {
        authenticator->setUser(m_userName);
        authenticator->setPassword(m_password);
    }
    else
    {
        m_authAttempts = 0;
        m_replyTracker.abort(reply, 401, "Authorization Required");
    }
}

void NetworkAccessManager::handleFinished(QNetworkReply *reply, int requestId, int status, const QString &statusText, const QString& body)
{
    QVariantList headers;
    foreach (QByteArray headerName, reply->rawHeaderList()) {
        QVariantMap header;
        header["name"] = QString::fromUtf8(headerName);
        header["value"] = QString::fromUtf8(reply->rawHeader(headerName));
        headers += header;
    }

    QVariantMap data;
    data["stage"] = "end";
    data["id"] = requestId;
    data["url"] = reply->url().toEncoded().data();
    data["status"] = status;
    data["statusText"] = statusText;
    data["contentType"] = reply->header(QNetworkRequest::ContentTypeHeader);
    data["redirectURL"] = reply->header(QNetworkRequest::LocationHeader);
    data["headers"] = headers;
    data["time"] = QDateTime::currentDateTime();
    data["body"] = body;
    data["bodySize"] = body.length();


    emit resourceReceived(data);
}

void NetworkAccessManager::handleSslErrors(QNetworkReply* reply, const QList<QSslError> &errors)
{
    foreach (QSslError e, errors) {
        qDebug() << "Network - SSL Error:" << e;
    }

    if (m_ignoreSslErrors)
        reply->ignoreSslErrors();
}

void NetworkAccessManager::handleNetworkError(QNetworkReply* reply, int requestId)
{
    qDebug() << "Network - Resource request error:"
             << reply->error()
             << "(" << reply->errorString() << ")"
             << "URL:" << reply->url().toString();

    QVariantMap data;
    data["id"] = requestId;
    data["url"] = reply->url().toString();
    data["errorCode"] = reply->error();
    data["errorString"] = reply->errorString();
    data["status"] = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    data["statusText"] = reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute);

    emit resourceError(data);
}
