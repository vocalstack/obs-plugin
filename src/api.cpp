#include "api.hpp"

#include <plugin-support.h>

#include <QNetworkReply>
#include <QNetworkRequest>

static QString from_env(const char *name, const char *fallback)
{
	const QString v = qEnvironmentVariable(name).trimmed();
	return v.isEmpty() ? QString::fromLatin1(fallback) : v.endsWith('/') ? v.chopped(1) : v;
}

Api::Api(QObject *parent)
	: QObject(parent),
	  api(from_env("VOCALSTACK_API", "https://api.vocalstack.com")),
	  app(from_env("VOCALSTACK_APP", "https://app.vocalstack.com"))
{
}

void Api::get(const QString &path, Done done)
{
	send("GET", path, nullptr, nullptr, true, std::move(done));
}

void Api::post(const QString &path, const QJsonObject &body, Done done)
{
	const QByteArray json = QJsonDocument(body).toJson(QJsonDocument::Compact);
	send("POST", path, &json, "application/json", true, std::move(done));
}

void Api::put(const QString &path, Done done)
{
	send("PUT", path, nullptr, nullptr, true, std::move(done));
}

void Api::del(const QString &path, Done done)
{
	send("DELETE", path, nullptr, nullptr, true, std::move(done));
}

void Api::postForm(const QString &path, const QUrlQuery &form, Done done)
{
	const QByteArray body = form.toString(QUrl::FullyEncoded).toUtf8();
	send("POST", path, &body, "application/x-www-form-urlencoded", false, std::move(done));
}

// The message of an API error envelope ({ error: { message } }), OAuth's
// ({ error, error_description }), or the HTTP status.
static QString error_text(int status, const QJsonDocument &doc)
{
	const QJsonObject o = doc.object();
	const QJsonValue e = o.value("error");
	if (e.isObject() && e.toObject().value("message").isString())
		return e.toObject().value("message").toString();
	if (o.value("error_description").isString())
		return o.value("error_description").toString();
	if (o.value("message").isString())
		return o.value("message").toString();
	return QStringLiteral("VocalStack answered %1").arg(status);
}

void Api::send(const QString &method, const QString &path, const QByteArray *body, const char *contentType,
	       bool withKey, Done done)
{
	QNetworkRequest req(QUrl(api + path));
	req.setRawHeader("Accept", "application/json");
	req.setRawHeader("User-Agent", QByteArray("vocalstack-obs/") + PLUGIN_VERSION);
	if (contentType)
		req.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
	if (withKey && !apiKey.isEmpty())
		req.setRawHeader("Authorization", "Bearer " + apiKey.toUtf8());
	req.setTransferTimeout(20000);

	QNetworkReply *reply = net.sendCustomRequest(req, method.toLatin1(), body ? *body : QByteArray());
	connect(reply, &QNetworkReply::finished, this, [this, reply, withKey, done = std::move(done)]() {
		reply->deleteLater();
		const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
		if (status == 0) {
			done(0, doc, reply->errorString());
			return;
		}
		if (status >= 400) {
			if (status == 401 && withKey)
				emit unauthorized();
			done(status, doc, error_text(status, doc));
			return;
		}
		done(status, doc, QString());
	});
}
