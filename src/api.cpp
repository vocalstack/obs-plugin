#include "api.hpp"

#include <plugin-support.h>

#include "net.hpp"

#include <QPointer>

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

void Api::postQuiet(const QString &path, const QJsonObject &body, Done done)
{
	const QByteArray json = QJsonDocument(body).toJson(QJsonDocument::Compact);
	send("POST", path, &json, "application/json", true, std::move(done), true);
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

static size_t collect(char *data, size_t size, size_t n, void *into)
{
	static_cast<QByteArray *>(into)->append(data, qsizetype(size * n));
	return size * n;
}

void Api::send(const QString &method, const QString &path, const QByteArray *body, const char *contentType,
	       bool withKey, Done done, bool quiet)
{
	CURL *easy = curl_easy_init();
	auto *received = new QByteArray;
	struct curl_slist *headers = curl_slist_append(nullptr, "Accept: application/json");
	if (contentType)
		headers = curl_slist_append(headers, (QByteArray("Content-Type: ") + contentType).constData());
	if (withKey && !apiKey.isEmpty())
		headers = curl_slist_append(headers, ("Authorization: Bearer " + apiKey.toUtf8()).constData());

	curl_easy_setopt(easy, CURLOPT_URL, (api + path).toUtf8().constData());
	curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(easy, CURLOPT_USERAGENT, (QByteArray("vocalstack-obs/") + PLUGIN_VERSION).constData());
	curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, 20000L);
	curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, collect);
	curl_easy_setopt(easy, CURLOPT_WRITEDATA, received);
	if (method != "GET") {
		// A body always, if empty: PUT and DELETE then send Content-Length: 0.
		curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, long(body ? body->size() : 0));
		curl_easy_setopt(easy, CURLOPT_COPYPOSTFIELDS, body ? body->constData() : "");
		if (method != "POST")
			curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, method.toLatin1().constData());
	}

	QPointer<Api> self(this);
	const bool signOut = withKey && !quiet;
	Net::instance().add(easy, [self, easy, headers, received, signOut, done = std::move(done)](CURLcode result) {
		long status = 0;
		curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
		const QJsonDocument doc = QJsonDocument::fromJson(*received);
		curl_easy_cleanup(easy);
		curl_slist_free_all(headers);
		delete received;
		if (!self)
			return;
		if (result != CURLE_OK) {
			done(0, doc, QString::fromUtf8(curl_easy_strerror(result)));
			return;
		}
		if (status >= 400) {
			if (status == 401 && signOut)
				emit self->unauthorized();
			done(int(status), doc, error_text(int(status), doc));
			return;
		}
		done(int(status), doc, QString());
	});
}
