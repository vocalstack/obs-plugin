#include "connect-login.hpp"

#include "api.hpp"

#include <QCryptographicHash>
#include <QDesktopServices>
#include <QHostInfo>
#include <QPointer>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>

// A PKCE verifier or a state: `n` characters of RFC 7636's unreserved set.
static QString random_token(int n)
{
	static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
	QString s;
	s.reserve(n);
	for (int i = 0; i < n; i++)
		s += QLatin1Char(alphabet[QRandomGenerator::system()->bounded(int(sizeof(alphabet) - 1))]);
	return s;
}

static QString s256(const QString &verifier)
{
	return QString::fromLatin1(QCryptographicHash::hash(verifier.toLatin1(), QCryptographicHash::Sha256)
					   .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

ConnectLogin::ConnectLogin(Api *api_, QObject *parent) : QObject(parent), api(api_)
{
	connect(&server, &QTcpServer::newConnection, this, &ConnectLogin::onConnection);
	timeout.setSingleShot(true);
	connect(&timeout, &QTimer::timeout, this,
		[this]() { finish({}, {}, tr("Sign-in timed out. Try again.")); });
}

void ConnectLogin::start()
{
	cancel();
	if (!server.listen(QHostAddress::LocalHost, 0)) {
		emit finished({}, {}, tr("Could not listen on this computer: %1").arg(server.errorString()));
		return;
	}
	done = false;
	verifier = random_token(64);
	state = random_token(32);
	redirectUri = QStringLiteral("http://127.0.0.1:%1/callback").arg(server.serverPort());

	QUrlQuery q;
	q.addQueryItem("response_type", "code");
	q.addQueryItem("client_name", QStringLiteral("OBS on %1").arg(QHostInfo::localHostName()).left(100));
	q.addQueryItem("redirect_uri", redirectUri);
	q.addQueryItem("state", state);
	q.addQueryItem("code_challenge", s256(verifier));
	q.addQueryItem("code_challenge_method", "S256");
	// Record: the plugin starts and stops recordings. Account reach: it
	// picks from the account's existing broadcasts.
	q.addQueryItem("scope", "read write recordings:publish");
	q.addQueryItem("reach", "account");
	QUrl url(api->appBase() + "/connect");
	url.setQuery(q);

	timeout.start(10 * 60 * 1000);
	QDesktopServices::openUrl(url);
}

void ConnectLogin::cancel()
{
	if (!done)
		finish({}, {}, {});
}

void ConnectLogin::onConnection()
{
	while (QTcpSocket *socket = server.nextPendingConnection()) {
		connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
		connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
			if (!socket->canReadLine())
				return;
			// The request line is all it takes; the headers after it are not read.
			disconnect(socket, &QTcpSocket::readyRead, this, nullptr);
			answer(socket, socket->readLine().trimmed());
		});
	}
}

// `closeTab`: the page tries to close itself. A browser lets it only where
// a script opened the tab, so the message stays for the rest.
static void reply_page(QTcpSocket *socket, int status, const QString &message, bool closeTab = false)
{
	if (!socket)
		return;
	const QByteArray html = QStringLiteral("<!doctype html><meta charset=utf-8><title>VocalStack</title>"
					       "<body style=\"font:16px system-ui;margin:3em;text-align:center\">"
					       "<p>%1</p>%2</body>")
					.arg(message.toHtmlEscaped(),
					     closeTab ? "<script>setTimeout(function(){window.close()},500)</script>" : "")
					.toUtf8();
	socket->write(QStringLiteral("HTTP/1.1 %1 %2\r\nContent-Type: text/html; charset=utf-8\r\n"
				     "Content-Length: %3\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n")
			      .arg(status)
			      .arg(status == 200 ? "OK" : "Bad Request")
			      .arg(html.size())
			      .toLatin1());
	socket->write(html);
	socket->disconnectFromHost();
}

// `GET /callback?code=…&state=… HTTP/1.1` from the browser.
void ConnectLogin::answer(QTcpSocket *socket, const QByteArray &requestLine)
{
	const QList<QByteArray> parts = requestLine.split(' ');
	const QUrl target(parts.size() >= 2 ? QString::fromLatin1(parts[1]) : QString());
	if (done || target.path() != "/callback") {
		reply_page(socket, 400, tr("This sign-in link is not waiting for an answer."));
		return;
	}
	const QUrlQuery q(target);
	if (q.queryItemValue("state") != state) {
		reply_page(socket, 400, tr("This answer does not belong to the sign-in OBS started."));
		return;
	}
	if (q.hasQueryItem("error")) {
		reply_page(socket, 200, tr("Sign-in cancelled. You can close this tab."));
		finish({}, {}, {});
		return;
	}
	const QString code = q.queryItemValue("code");

	QUrlQuery form;
	form.addQueryItem("grant_type", "authorization_code");
	form.addQueryItem("code", code);
	form.addQueryItem("code_verifier", verifier);
	form.addQueryItem("redirect_uri", redirectUri);
	server.close();
	// The page answers once the code is redeemed, so it says how it went.
	api->postForm("/v1/connect/token", form, [this, page = QPointer<QTcpSocket>(socket)](int, const QJsonDocument &body, const QString &error) {
		if (!error.isEmpty()) {
			reply_page(page, 200, tr("OBS could not finish signing in: %1").arg(error));
			finish({}, {}, error);
			return;
		}
		reply_page(page, 200, tr("Signed in. You can close this tab and go back to OBS."), true);
		const QJsonObject o = body.object();
		finish(o.value("access_token").toString(), o.value("account").toObject().value("name").toString(), {});
	});
}

void ConnectLogin::finish(const QString &key, const QString &account, const QString &error)
{
	if (done)
		return;
	done = true;
	timeout.stop();
	server.close();
	emit finished(key, account, error);
}
