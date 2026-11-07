#pragma once

#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QUrlQuery>

#include <functional>

// VocalStack's public API (/v1), authenticated with an API key. Every call
// answers on the Qt main thread.
class Api : public QObject {
	Q_OBJECT

public:
	// `status` is 0 when no answer came (network); `error` is the API's
	// message for a failed call, or the network's.
	using Done = std::function<void(int status, const QJsonDocument &body, const QString &error)>;

	explicit Api(QObject *parent = nullptr);

	// https://api.vocalstack.com and https://app.vocalstack.com, or the
	// VOCALSTACK_API / VOCALSTACK_APP environment overrides (e2e, local).
	QString apiBase() const { return api; }
	QString appBase() const { return app; }

	void setKey(const QString &key) { apiKey = key; }
	QString key() const { return apiKey; }

	void get(const QString &path, Done done);
	void post(const QString &path, const QJsonObject &body, Done done);
	// A post that is safe to send again (a timeout, a 503): the API
	// answers a repeat with the same `idempotencyKey` and body as it did
	// the first, instead of acting twice.
	void post(const QString &path, const QJsonObject &body, const QString &idempotencyKey, Done done);
	// A post whose refusal never signs the person out (background pings).
	void postQuiet(const QString &path, const QJsonObject &body, Done done);
	void patch(const QString &path, const QJsonObject &body, Done done);
	void put(const QString &path, Done done);
	void del(const QString &path, Done done);
	// A form post without the key (OAuth's token endpoint).
	void postForm(const QString &path, const QUrlQuery &form, Done done);
	// A GET of another site's JSON at `url`, without the key (GitHub's
	// releases, for the update check).
	void getUrl(const QString &url, Done done);
	// A GET of `url` (redirects followed, no key) saved to `path`; `done`
	// gets the HTTP status and an error, or none.
	using Saved = std::function<void(int status, const QString &error)>;
	void download(const QString &url, const QString &path, Saved done);

	// The API's error code of a failed call's body (`insufficient_credit`,
	// `not_found`…), empty when the body is not the API's.
	static QString errorCode(const QJsonDocument &body);
	// `data.details` of a failed call's body ({ blocked: true, … }).
	static QJsonObject errorDetails(const QJsonDocument &body);

signals:
	// The key was refused (revoked, deleted): the person must sign in again.
	void unauthorized();

private:
	struct Request {
		QString method, url;
		QByteArray body;
		bool hasBody = false;
		QByteArray contentType, idempotencyKey;
		bool withKey = true, quiet = false;
		// Sent again once after a 429 or 503 that says when (Retry-After):
		// reads, and posts that carry an idempotency key.
		bool retried = false;
	};

	void send(Request req, Done done);

	QString api, app, apiKey;
};
