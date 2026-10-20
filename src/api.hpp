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
	void put(const QString &path, Done done);
	void del(const QString &path, Done done);
	// A form post without the key (OAuth's token endpoint).
	void postForm(const QString &path, const QUrlQuery &form, Done done);

signals:
	// The key was refused (revoked, deleted): the person must sign in again.
	void unauthorized();

private:
	void send(const QString &method, const QString &path, const QByteArray *body, const char *contentType,
		  bool withKey, Done done);

	QString api, app, apiKey;
};
