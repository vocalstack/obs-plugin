#pragma once

#include "workspace.hpp"

#include <QObject>
#include <QPointer>
#include <QTcpServer>
#include <QTimer>

class Api;

// One-click sign-in: VocalStack Connect (OAuth 2.0 authorization code +
// PKCE, RFC 8252's loopback redirect). The browser opens the consent
// screen; the answer comes back to a listener on 127.0.0.1, and the code
// is redeemed for an API key.
class ConnectLogin : public QObject {
	Q_OBJECT

public:
	explicit ConnectLogin(Api *api, QObject *parent = nullptr);

	// Opens the browser; finished() follows, once. `workspaceId`: the
	// workspace the plugin works in now, which the consent screen offers
	// first; empty on a first sign-in.
	void start(const QString &workspaceId = QString());
	void cancel();

signals:
	// The key, the account's name and the workspace the key belongs to
	// (unknown from an older API), or an error (empty when cancelled).
	void finished(const QString &key, const QString &account, const Workspace &workspace, const QString &error);

private:
	void onConnection();
	void answer(class QTcpSocket *socket, const QByteArray &requestLine);
	void finish(const QString &key, const QString &account, const Workspace &workspace, const QString &error);

	Api *api;
	QTcpServer server;
	QTimer timeout;
	QString verifier, state, redirectUri;
	bool done = true;
};
