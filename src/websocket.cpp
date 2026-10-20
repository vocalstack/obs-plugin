#include "websocket.hpp"

#include "net.hpp"

#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QSocketNotifier>

// Larger frames than this are refused: captions are a few hundred bytes.
static const quint64 MAX_FRAME = 4 * 1024 * 1024;

WebSocket::WebSocket(QObject *parent) : QObject(parent)
{
	ping.setInterval(25000);
	connect(&ping, &QTimer::timeout, this, [this]() { sendFrame(0x9, QByteArray()); });
}

WebSocket::~WebSocket()
{
	teardown();
}

void WebSocket::open(const QUrl &url_, const QList<QPair<QByteArray, QByteArray>> &headers_)
{
	close();
	url = url_;
	headers = headers_;
	closing = false;
	buffer.clear();
	message.clear();
	outgoing.clear();
	state = State::Handshake;
	const bool tls = url.scheme() == "wss";
	const int port = url.port(tls ? 443 : 80);

	// CONNECT_ONLY: curl connects (and shakes TLS hands), then the bytes
	// are ours through curl_easy_send/recv.
	easy = curl_easy_init();
	const QByteArray target = (tls ? "https://" : "http://") + url.host().toUtf8() + ":" + QByteArray::number(port) + "/";
	curl_easy_setopt(easy, CURLOPT_URL, target.constData());
	curl_easy_setopt(easy, CURLOPT_CONNECT_ONLY, 1L);
	// HTTP/1.1 only: if TLS agreed on HTTP/2, curl would frame our bytes
	// as HTTP/2.
	curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, long(CURL_HTTP_VERSION_1_1));
	curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
	curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
	Net::instance().add(
		easy,
		[this](CURLcode result) {
			if (result != CURLE_OK) {
				fail(QString::fromUtf8(curl_easy_strerror(result)));
				return;
			}
			curl_socket_t fd = CURL_SOCKET_BAD;
			curl_easy_getinfo(easy, CURLINFO_ACTIVESOCKET, &fd);
			if (fd == CURL_SOCKET_BAD) {
				fail(tr("The connection closed"));
				return;
			}
			readable = new QSocketNotifier(qintptr(fd), QSocketNotifier::Read, this);
			connect(readable, &QSocketNotifier::activated, this, &WebSocket::onReadyRead);
			writable = new QSocketNotifier(qintptr(fd), QSocketNotifier::Write, this);
			writable->setEnabled(false);
			connect(writable, &QSocketNotifier::activated, this, &WebSocket::flush);
			onConnected();
		},
		true);
}

void WebSocket::close()
{
	if (state == State::Closed)
		return;
	closing = true;
	if (state == State::Open)
		sendFrame(0x8, QByteArray::fromHex("03e8")); // 1000, normal closure
	state = State::Closed;
	ping.stop();
	teardown();
	emit closed(QString());
}

void WebSocket::teardown()
{
	delete readable;
	delete writable;
	readable = writable = nullptr;
	if (easy) {
		Net::instance().remove(easy);
		curl_easy_cleanup(easy);
		easy = nullptr;
	}
}

void WebSocket::write(const QByteArray &data)
{
	outgoing += data;
	flush();
}

void WebSocket::flush()
{
	while (easy && !outgoing.isEmpty()) {
		size_t sent = 0;
		const CURLcode rc = curl_easy_send(easy, outgoing.constData(), size_t(outgoing.size()), &sent);
		if (rc == CURLE_AGAIN)
			break;
		if (rc != CURLE_OK) {
			fail(QString::fromUtf8(curl_easy_strerror(rc)));
			return;
		}
		outgoing.remove(0, qsizetype(sent));
	}
	if (writable)
		writable->setEnabled(!outgoing.isEmpty());
}

void WebSocket::disconnected()
{
	if (state == State::Closed)
		return;
	const bool wanted = closing;
	state = State::Closed;
	ping.stop();
	teardown();
	emit closed(wanted ? QString() : tr("The connection closed"));
}

void WebSocket::sendText(const QString &text)
{
	if (state == State::Open)
		sendFrame(0x1, text.toUtf8());
}

void WebSocket::onConnected()
{
	QByteArray nonce(16, 0);
	for (auto &b : nonce)
		b = char(QRandomGenerator::system()->bounded(256));
	const QByteArray key = nonce.toBase64();
	QByteArray proof = key;
	proof.append("258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
	accept = QCryptographicHash::hash(proof, QCryptographicHash::Sha1).toBase64();

	QByteArray req = "GET " + url.path(QUrl::FullyEncoded).toLatin1();
	if (url.hasQuery())
		req += "?" + url.query(QUrl::FullyEncoded).toLatin1();
	req += " HTTP/1.1\r\nHost: " + url.host().toLatin1();
	if (url.port() > 0)
		req += ":" + QByteArray::number(url.port());
	req += "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " +
	       key + "\r\n";
	for (const auto &h : headers)
		req += h.first + ": " + h.second + "\r\n";
	req += "\r\n";
	write(req);
}

void WebSocket::onReadyRead()
{
	// Until curl has nothing more: TLS may hold decrypted bytes the socket
	// no longer signals.
	for (;;) {
		char chunk[16384];
		size_t n = 0;
		const CURLcode rc = easy ? curl_easy_recv(easy, chunk, sizeof(chunk), &n) : CURLE_AGAIN;
		if (rc == CURLE_AGAIN)
			break;
		if (rc != CURLE_OK) {
			fail(QString::fromUtf8(curl_easy_strerror(rc)));
			return;
		}
		if (n == 0) {
			if (state == State::Open)
				readFrames(); // what came before the end
			disconnected();
			return;
		}
		buffer.append(chunk, qsizetype(n));
	}
	if (state == State::Handshake) {
		const int end = buffer.indexOf("\r\n\r\n");
		if (end < 0) {
			if (buffer.size() > 16384)
				fail(tr("The server's answer was not a WebSocket"));
			return;
		}
		const QList<QByteArray> lines = buffer.left(end).split('\n');
		buffer.remove(0, end + 4);
		const QList<QByteArray> status = lines.value(0).trimmed().split(' ');
		if (status.value(1) != "101") {
			fail(tr("The server refused the caption feed (HTTP %1)").arg(QString::fromLatin1(status.value(1))));
			return;
		}
		QByteArray got;
		for (const QByteArray &line : lines) {
			const int colon = line.indexOf(':');
			if (colon > 0 && line.left(colon).trimmed().toLower() == "sec-websocket-accept")
				got = line.mid(colon + 1).trimmed();
		}
		if (got != accept) {
			fail(tr("The server's WebSocket answer did not match (%1, want %2)")
				     .arg(QString::fromLatin1(got), QString::fromLatin1(accept)));
			return;
		}
		state = State::Open;
		ping.start();
		emit opened();
	}
	if (state == State::Open)
		readFrames();
}

void WebSocket::readFrames()
{
	while (state == State::Open && buffer.size() >= 2) {
		const auto b0 = quint8(buffer[0]), b1 = quint8(buffer[1]);
		const bool fin = b0 & 0x80;
		const quint8 opcode = b0 & 0x0f;
		const bool masked = b1 & 0x80;
		quint64 len = b1 & 0x7f;
		int at = 2;
		if (len == 126) {
			if (buffer.size() < 4)
				return;
			len = (quint64(quint8(buffer[2])) << 8) | quint8(buffer[3]);
			at = 4;
		} else if (len == 127) {
			if (buffer.size() < 10)
				return;
			len = 0;
			for (int i = 0; i < 8; i++)
				len = (len << 8) | quint8(buffer[2 + i]);
			at = 10;
		}
		if (len > MAX_FRAME) {
			fail(tr("A caption frame was too large"));
			return;
		}
		const int maskAt = at;
		if (masked)
			at += 4;
		if (quint64(buffer.size()) < at + len)
			return;
		QByteArray payload = buffer.mid(at, int(len));
		if (masked)
			for (int i = 0; i < payload.size(); i++)
				payload[i] = char(payload[i] ^ buffer[maskAt + (i % 4)]);
		buffer.remove(0, at + int(len));

		switch (opcode) {
		case 0x0: // continuation
		case 0x1: // text
			message += payload;
			if (fin) {
				emit textMessage(QString::fromUtf8(message));
				message.clear();
			}
			break;
		case 0x8:
			closing = true;
			disconnected();
			return;
		case 0x9:
			sendFrame(0xA, payload);
			break;
		default: // pong, binary: nothing to do
			break;
		}
	}
}

// A client frame is always masked (RFC 6455 §5.3).
void WebSocket::sendFrame(quint8 opcode, const QByteArray &payload)
{
	QByteArray frame;
	frame.append(char(0x80 | opcode));
	const auto n = quint64(payload.size());
	if (n < 126) {
		frame.append(char(0x80 | n));
	} else if (n < 65536) {
		frame.append(char(0x80 | 126));
		frame.append(char((n >> 8) & 0xff));
		frame.append(char(n & 0xff));
	} else {
		frame.append(char(0x80 | 127));
		for (int i = 7; i >= 0; i--)
			frame.append(char((n >> (8 * i)) & 0xff));
	}
	char mask[4];
	for (char &m : mask)
		m = char(QRandomGenerator::system()->bounded(256));
	frame.append(mask, 4);
	for (int i = 0; i < payload.size(); i++)
		frame.append(char(payload[i] ^ mask[i % 4]));
	write(frame);
}

void WebSocket::fail(const QString &reason)
{
	if (state == State::Closed)
		return;
	state = State::Closed;
	ping.stop();
	teardown();
	emit closed(reason);
}
