#include "api.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include "net.hpp"

#include <QPointer>
#include <QTimer>
#include <QUrl>

#include <cstdio>

// The longest Retry-After waited for before giving the error back.
static const int RETRY_MAX_S = 15;

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
	Request r;
	r.method = "GET";
	r.url = api + path;
	send(std::move(r), std::move(done));
}

void Api::post(const QString &path, const QJsonObject &body, Done done)
{
	post(path, body, QString(), std::move(done));
}

void Api::post(const QString &path, const QJsonObject &body, const QString &idempotencyKey, Done done)
{
	Request r;
	r.method = "POST";
	r.url = api + path;
	r.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
	r.hasBody = true;
	r.contentType = "application/json";
	r.idempotencyKey = idempotencyKey.toUtf8();
	send(std::move(r), std::move(done));
}

void Api::postQuiet(const QString &path, const QJsonObject &body, Done done)
{
	Request r;
	r.method = "POST";
	r.url = api + path;
	r.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
	r.hasBody = true;
	r.contentType = "application/json";
	r.quiet = true;
	send(std::move(r), std::move(done));
}

void Api::patch(const QString &path, const QJsonObject &body, Done done)
{
	Request r;
	r.method = "PATCH";
	r.url = api + path;
	r.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
	r.hasBody = true;
	r.contentType = "application/json";
	send(std::move(r), std::move(done));
}

void Api::put(const QString &path, Done done)
{
	Request r;
	r.method = "PUT";
	r.url = api + path;
	r.hasBody = true;
	send(std::move(r), std::move(done));
}

void Api::del(const QString &path, Done done)
{
	Request r;
	r.method = "DELETE";
	r.url = api + path;
	r.hasBody = true;
	send(std::move(r), std::move(done));
}

void Api::delQuiet(const QString &path, Done done)
{
	Request r;
	r.method = "DELETE";
	r.url = api + path;
	r.hasBody = true;
	r.quiet = true;
	send(std::move(r), std::move(done));
}

void Api::postForm(const QString &path, const QUrlQuery &form, Done done)
{
	Request r;
	r.method = "POST";
	r.url = api + path;
	r.body = form.toString(QUrl::FullyEncoded).toUtf8();
	r.hasBody = true;
	r.contentType = "application/x-www-form-urlencoded";
	r.withKey = false;
	send(std::move(r), std::move(done));
}

void Api::getUrl(const QString &url, Done done)
{
	Request r;
	r.method = "GET";
	r.url = url;
	r.withKey = false;
	send(std::move(r), std::move(done));
}

static size_t to_file(char *data, size_t size, size_t n, void *into)
{
	return fwrite(data, size, n, static_cast<FILE *>(into));
}

void Api::download(const QString &url, const QString &path, Saved done)
{
	FILE *file = fopen(path.toUtf8().constData(), "wb");
	if (!file) {
		done(0, tr("Could not write %1").arg(path));
		return;
	}
	CURL *easy = curl_easy_init();
	curl_easy_setopt(easy, CURLOPT_URL, url.toUtf8().constData());
	curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 10L);
	curl_easy_setopt(easy, CURLOPT_USERAGENT, (QByteArray("vocalstack-obs/") + PLUGIN_VERSION).constData());
	curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, 10 * 60 * 1000L);
	curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, to_file);
	curl_easy_setopt(easy, CURLOPT_WRITEDATA, file);
	QPointer<Api> self(this);
	Net::instance().add(easy, [self, easy, file, done = std::move(done)](CURLcode result) {
		long status = 0;
		curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
		curl_easy_cleanup(easy);
		fclose(file);
		if (!self)
			return;
		if (result != CURLE_OK)
			done(0, QString::fromUtf8(curl_easy_strerror(result)));
		else if (status >= 400)
			done(int(status), tr("The download answered %1").arg(status));
		else
			done(int(status), QString());
	});
}

// An icon is a few kilobytes; anything far larger is not one.
static const qsizetype FETCH_MAX = 4 * 1024 * 1024;

static size_t collect_capped(char *data, size_t size, size_t n, void *into)
{
	auto *bytes = static_cast<QByteArray *>(into);
	if (bytes->size() + qsizetype(size * n) > FETCH_MAX)
		return 0;
	bytes->append(data, qsizetype(size * n));
	return size * n;
}

void Api::fetch(const QString &link, Fetched done)
{
	const QUrl url = QUrl(api + "/").resolved(QUrl(link));
	CURL *easy = curl_easy_init();
	auto *received = new QByteArray;
	curl_easy_setopt(easy, CURLOPT_URL, url.toString(QUrl::FullyEncoded).toUtf8().constData());
	curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 5L);
	curl_easy_setopt(easy, CURLOPT_USERAGENT, (QByteArray("vocalstack-obs/") + PLUGIN_VERSION).constData());
	curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, 20000L);
	curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, collect_capped);
	curl_easy_setopt(easy, CURLOPT_WRITEDATA, received);
	QPointer<Api> self(this);
	Net::instance().add(easy, [self, easy, received, done = std::move(done)](CURLcode result) {
		long status = 0;
		curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
		const QByteArray body = result == CURLE_OK && status == 200 ? *received : QByteArray();
		curl_easy_cleanup(easy);
		delete received;
		if (self)
			done(body);
	});
}

QString Api::errorCode(const QJsonDocument &body)
{
	const QJsonObject o = body.object();
	if (o.value("code").isString())
		return o.value("code").toString();
	return o.value("error").toObject().value("code").toString();
}

QJsonObject Api::errorDetails(const QJsonDocument &body)
{
	return body.object().value("data").toObject().value("details").toObject();
}

// The message of an API error body ({ code, message, requestId }), an
// envelope ({ error: { message } }), OAuth's ({ error, error_description }),
// or the HTTP status.
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

// The response headers the plugin reads: Retry-After and X-Request-Id.
struct Headers {
	int retryAfterS = -1;
	QByteArray requestId;
};

static size_t collect_header(char *data, size_t size, size_t n, void *into)
{
	auto *h = static_cast<Headers *>(into);
	const QByteArray line = QByteArray(data, qsizetype(size * n)).trimmed();
	const int colon = line.indexOf(':');
	if (colon > 0) {
		const QByteArray name = line.left(colon).trimmed().toLower(), value = line.mid(colon + 1).trimmed();
		if (name == "retry-after") {
			bool ok = false;
			const int s = value.toInt(&ok);
			if (ok)
				h->retryAfterS = s;
		} else if (name == "x-request-id") {
			h->requestId = value;
		}
	}
	return size * n;
}

void Api::send(Request req, Done done)
{
	CURL *easy = curl_easy_init();
	auto *received = new QByteArray;
	auto *headers = new Headers;
	struct curl_slist *list = curl_slist_append(nullptr, "Accept: application/json");
	if (!req.contentType.isEmpty())
		list = curl_slist_append(list, ("Content-Type: " + req.contentType).constData());
	if (req.withKey && !apiKey.isEmpty())
		list = curl_slist_append(list, ("Authorization: Bearer " + apiKey.toUtf8()).constData());
	if (!req.idempotencyKey.isEmpty())
		list = curl_slist_append(list, ("Idempotency-Key: " + req.idempotencyKey).constData());

	curl_easy_setopt(easy, CURLOPT_URL, req.url.toUtf8().constData());
	curl_easy_setopt(easy, CURLOPT_HTTPHEADER, list);
	curl_easy_setopt(easy, CURLOPT_USERAGENT, (QByteArray("vocalstack-obs/") + PLUGIN_VERSION).constData());
	curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, 20000L);
	curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, collect);
	curl_easy_setopt(easy, CURLOPT_WRITEDATA, received);
	curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, collect_header);
	curl_easy_setopt(easy, CURLOPT_HEADERDATA, headers);
	if (req.hasBody) {
		// A body always, if empty: PUT and DELETE then send Content-Length: 0.
		curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, long(req.body.size()));
		curl_easy_setopt(easy, CURLOPT_COPYPOSTFIELDS, req.body.constData());
		if (req.method != "POST")
			curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, req.method.toLatin1().constData());
	}

	QPointer<Api> self(this);
	const bool signOut = req.withKey && !req.quiet;
	const bool mayRetry = !req.retried && (req.method == "GET" || !req.idempotencyKey.isEmpty());
	Net::instance().add(easy, [self, easy, list, received, headers, signOut, mayRetry, req = std::move(req),
				   done = std::move(done)](CURLcode result) mutable {
		long status = 0;
		curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
		const QJsonDocument doc = QJsonDocument::fromJson(*received);
		const Headers got = *headers;
		curl_easy_cleanup(easy);
		curl_slist_free_all(list);
		delete received;
		delete headers;
		if (!self)
			return;
		if (result != CURLE_OK) {
			done(0, doc, QString::fromUtf8(curl_easy_strerror(result)));
			return;
		}
		if (status >= 400) {
			obs_log(LOG_WARNING, "%s %s: %ld %s (request %s)", req.method.toUtf8().constData(),
				req.url.toUtf8().constData(), status, Api::errorCode(doc).toUtf8().constData(),
				got.requestId.isEmpty() ? "-" : got.requestId.constData());
			// Rate limited or waking: once more when the API says when.
			if ((status == 429 || status == 503) && mayRetry && got.retryAfterS >= 0 &&
			    got.retryAfterS <= RETRY_MAX_S) {
				req.retried = true;
				QTimer::singleShot(got.retryAfterS * 1000 + 200, self,
						   [self, req = std::move(req), done = std::move(done)]() mutable {
							   if (self)
								   self->send(std::move(req), std::move(done));
						   });
				return;
			}
			if (status == 401 && signOut)
				emit self->unauthorized();
			done(int(status), doc, error_text(int(status), doc));
			return;
		}
		done(int(status), doc, QString());
	});
}
