#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPair>
#include <QSslSocket>
#include <QTimer>
#include <QUrl>

// A WebSocket client (RFC 6455) for the caption feed, on Qt's TLS socket:
// OBS ships QtNetwork but not QtWebSockets. Text frames in, text frames
// out, pings answered and sent; no extensions.
class WebSocket : public QObject {
	Q_OBJECT

public:
	explicit WebSocket(QObject *parent = nullptr);

	// `wss://` or `ws://`; `headers` ride the opening handshake.
	void open(const QUrl &url, const QList<QPair<QByteArray, QByteArray>> &headers);
	void close();
	void sendText(const QString &text);
	bool isOpen() const { return state == State::Open; }

signals:
	void opened();
	void textMessage(const QString &text);
	// The socket is gone; `reason` says why when it was not close().
	void closed(const QString &reason);

private:
	enum class State { Closed, Handshake, Open };

	void onConnected();
	void onReadyRead();
	void readFrames();
	void sendFrame(quint8 opcode, const QByteArray &payload);
	void fail(const QString &reason);

	QSslSocket socket;
	QTimer ping;
	State state = State::Closed;
	QUrl url;
	QList<QPair<QByteArray, QByteArray>> headers;
	QByteArray accept, buffer, message;
	bool closing = false;
};
