#include "caption-feeds.hpp"

#include "api.hpp"
#include "caption-track.hpp"
#include "websocket.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <QJsonDocument>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

// As Session's own feed: opened again after 3 s, then 6, 12… up to a
// minute while it keeps closing; at once after a restart (1012), a socket
// open too long (4002) or one that fell behind (4004).
static const int RETRY_MS = 3000;
static const int RETRY_MAX_MS = 60000;
static const int CLOSE_POLICY = 1008;
static const int CLOSE_RESTART = 1012;
static const int CLOSE_TOO_LONG = 4002;
static const int CLOSE_ACCESS = 4003;
static const int CLOSE_BEHIND = 4004;

// One language's subscribe socket and what it captions.
class CaptionFeed : public QObject {
	Q_OBJECT

public:
	CaptionFeed(Api *api_, const QString &lang_, QObject *parent) : QObject(parent), api(api_), lang(lang_)
	{
		track.setLanguage(lang);
		retry.setSingleShot(true);
		connect(&retry, &QTimer::timeout, this, &CaptionFeed::open);
		connect(&track, &CaptionTrack::caption, this,
			[this](const QString &speaker, const QString &text) { emit caption(lang, speaker, text); });
		connect(&socket, &WebSocket::opened, this, [this]() { retryMs = RETRY_MS; });
		connect(&socket, &WebSocket::textMessage, this, [this](const QString &text) {
			const QJsonObject f = QJsonDocument::fromJson(text.toUtf8()).object();
			if (f.value("cursor").isDouble() && f.value("cursor").toInteger() <= readyCursor)
				return; // seen before the socket was opened again
			if (f.value("type").toString() == "subscribe.ready")
				readyCursor = f.value("lastSeq").toInteger(-1);
			track.onFrame(f);
		});
		connect(&socket, &WebSocket::closed, this, [this](const QString &reason, int code) {
			if (liveId.isEmpty() || reason.isEmpty())
				return;
			obs_log(LOG_WARNING, "caption feed (%s) closed (%d): %s",
				lang.isEmpty() ? "spoken" : lang.toUtf8().constData(), code, reason.toUtf8().constData());
			if (code == CLOSE_ACCESS)
				return; // Session's own feed says so, and signs out if need be
			if (code == CLOSE_POLICY && !track.language().isEmpty()) {
				// The transcription has all the languages it may: as spoken.
				track.setLanguage(QString());
				retry.start(0);
				return;
			}
			if (code == CLOSE_RESTART || code == CLOSE_TOO_LONG || code == CLOSE_BEHIND) {
				retry.start(0);
				return;
			}
			retry.start(retryMs);
			retryMs = qMin(retryMs * 2, RETRY_MAX_MS);
		});
	}

	void start(const QString &id)
	{
		liveId = id;
		retryMs = RETRY_MS;
		track.setLanguage(lang);
		track.reset();
		open();
	}

	void stop()
	{
		liveId.clear();
		retry.stop();
		socket.close();
		track.clear();
	}

signals:
	void caption(const QString &lang, const QString &speaker, const QString &text);

private:
	void open()
	{
		if (liveId.isEmpty())
			return;
		QUrl url(api->apiBase() + "/v1/transcriptions/" + liveId + "/subscribe");
		url.setScheme(url.scheme() == "http" ? "ws" : "wss");
		if (!track.language().isEmpty()) {
			QUrlQuery q;
			q.addQueryItem("lang", track.language());
			url.setQuery(q);
		}
		readyCursor = -1;
		socket.open(url, {{"Authorization", "Bearer " + api->key().toUtf8()},
				  {"User-Agent", QByteArray("vocalstack-obs/") + PLUGIN_VERSION}});
	}

	Api *api;
	const QString lang;
	QString liveId;
	WebSocket socket;
	CaptionTrack track;
	QTimer retry;
	int retryMs = RETRY_MS;
	qint64 readyCursor = -1;
};

CaptionFeeds::CaptionFeeds(Api *api_, QObject *parent) : QObject(parent), api(api_) {}

CaptionFeeds::~CaptionFeeds() = default;

void CaptionFeeds::acquire(const QString &lang)
{
	if (wanted[lang]++ > 0)
		return;
	auto *feed = new CaptionFeed(api, lang, this);
	connect(feed, &CaptionFeed::caption, this, &CaptionFeeds::caption);
	feeds.insert(lang, feed);
	if (!liveId.isEmpty())
		feed->start(liveId);
}

void CaptionFeeds::release(const QString &lang)
{
	auto it = wanted.find(lang);
	if (it == wanted.end() || --it.value() > 0)
		return;
	wanted.erase(it);
	if (CaptionFeed *feed = feeds.take(lang)) {
		feed->stop();
		feed->deleteLater();
	}
	emit caption(lang, QString(), QString());
}

void CaptionFeeds::start(const QString &transcriptionId)
{
	liveId = transcriptionId;
	for (CaptionFeed *feed : feeds)
		feed->start(liveId);
}

void CaptionFeeds::stop()
{
	liveId.clear();
	for (CaptionFeed *feed : feeds)
		feed->stop();
}

#include "caption-feeds.moc"
