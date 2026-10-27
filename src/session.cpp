#include "session.hpp"

#include "api.hpp"

#include <plugin-support.h>

#include <QHostInfo>
#include <QJsonArray>
#include <QUrlQuery>

// A caption stays this long after the last words.
static const int CAPTION_HOLD_MS = 8000;
// A caption feed that closed is opened again after 3 s, then 6, 12… up
// to a minute while it keeps closing; at once when the server closed it to
// restart (1012: a relay draining for a deploy), where the same
// subscription goes on.
static const int RESUBSCRIBE_MS = 3000;
static const int RESUBSCRIBE_MAX_MS = 60000;
static const int CLOSE_RESTART = 1012;

Session::Session(Api *api_, Settings *settings_, QObject *parent)
	: QObject(parent),
	  api(api_),
	  settings(settings_),
	  resubscribeMs(RESUBSCRIBE_MS)
{
	connect(&output, &VocalStackOutput::started, this, [this]() {
		if (st == State::Starting)
			ensureTranscription([this]() { attachBroadcast([this]() { startRecording(false); }); });
	});
	connect(&output, &VocalStackOutput::reconnecting, this,
		[this]() { emit status(tr("Reconnecting to VocalStack…"), true); });
	connect(&output, &VocalStackOutput::stopped, this, [this](const QString &error, bool keyRefused) {
		if (keyRefused) {
			// Removed in the dashboard, or its connection revoked: the
			// next Start makes a new one.
			settings->streamKey.clear();
			settings->streamKeyId.clear();
			settings->save();
		}
		if (error.isEmpty() || st == State::Idle || st == State::Stopping)
			return;
		if (st == State::Starting)
			failStart(error);
		else
			end(error); // the stream died under a live session: end the rest too
	});

	connect(&feed, &WebSocket::textMessage, this, &Session::onFrame);
	connect(&feed, &WebSocket::opened, this, [this]() { resubscribeMs = RESUBSCRIBE_MS; });
	connect(&feed, &WebSocket::closed, this, [this](const QString &reason, int code) {
		if (st == State::Live && !reason.isEmpty()) {
			obs_log(LOG_WARNING, "caption feed closed: %s", reason.toUtf8().constData());
			if (code == CLOSE_RESTART) {
				resubscribe.start(0);
				return;
			}
			resubscribe.start(resubscribeMs);
			resubscribeMs = qMin(resubscribeMs * 2, RESUBSCRIBE_MAX_MS);
		}
	});
	resubscribe.setSingleShot(true);
	connect(&resubscribe, &QTimer::timeout, this, [this]() {
		if (st == State::Live)
			subscribe();
	});

	captionClear.setSingleShot(true);
	captionClear.setInterval(CAPTION_HOLD_MS);
	connect(&captionClear, &QTimer::timeout, this, &Session::clearCaption);

	ingestPoll.setInterval(5000);
	connect(&ingestPoll, &QTimer::timeout, this, &Session::pollIngest);
}

void Session::setState(State s)
{
	st = s;
	emit stateChanged(s);
}

void Session::start()
{
	if (st != State::Idle)
		return;
	setState(State::Starting);
	setNotes(QString(), QString());
	emit status(tr("Connecting to VocalStack…"), false);
	ensureStreamKey([this]() {
		if (!output.start(settings->serverUrl, settings->streamKey, settings->sendVideo))
			failStart(tr("OBS could not start the stream to VocalStack (see the OBS log)"));
		// Next: the output's started signal.
	});
}

void Session::failStart(const QString &error)
{
	output.stop();
	setState(State::Idle);
	emit status(error, true);
}

// The plugin's own stream key, made once (it is shown in full only then)
// and kept with the settings.
void Session::ensureStreamKey(Next next)
{
	if (!settings->streamKey.isEmpty() && !settings->serverUrl.isEmpty()) {
		next();
		return;
	}
	QJsonObject body{{"name", QStringLiteral("OBS on %1").arg(QHostInfo::localHostName()).left(100)}};
	api->post("/v1/rtmp/keys", body, [this, next](int, const QJsonDocument &doc, const QString &error) {
		if (st != State::Starting)
			return;
		if (!error.isEmpty()) {
			failStart(tr("Could not get a stream key: %1").arg(error));
			return;
		}
		const QJsonObject o = doc.object();
		settings->streamKey = o.value("streamKey").toString();
		settings->streamKeyId = o.value("id").toString();
		settings->serverUrl = o.value("serverUrl").toString();
		settings->save();
		if (settings->streamKey.isEmpty() || settings->serverUrl.isEmpty()) {
			failStart(tr("VocalStack's answer had no stream key"));
			return;
		}
		next();
	});
}

void Session::ensureTranscription(Next next)
{
	if (settings->transcriptionId.isEmpty()) {
		createTranscription(next);
		return;
	}
	// Still there? It may have been deleted in the dashboard.
	api->get("/v1/transcriptions/" + settings->transcriptionId,
		 [this, next](int status, const QJsonDocument &, const QString &error) {
			 if (st != State::Starting)
				 return;
			 if (status == 404) {
				 settings->transcriptionId.clear();
				 settings->attachedBroadcastId.clear();
				 createTranscription(next);
			 } else if (!error.isEmpty()) {
				 failStart(error);
			 } else {
				 next();
			 }
		 });
}

void Session::createTranscription(Next next)
{
	QJsonObject body{
		{"name", QStringLiteral("OBS on %1").arg(QHostInfo::localHostName())},
		{"source", QJsonObject{{"kind", "stream"}, {"url", settings->serverUrl}}},
	};
	api->post("/v1/transcriptions/live", body, [this, next](int, const QJsonDocument &doc, const QString &error) {
		if (st != State::Starting)
			return;
		if (!error.isEmpty()) {
			failStart(tr("Could not create the live transcription: %1").arg(error));
			return;
		}
		settings->transcriptionId = doc.object().value("id").toString();
		settings->attachedBroadcastId.clear();
		settings->save();
		next();
	});
}

// The chosen broadcast in, the one before it out.
void Session::attachBroadcast(Next next)
{
	const QString base = "/v1/transcriptions/" + settings->transcriptionId + "/broadcasts/";
	const QString want = settings->broadcastId, had = settings->attachedBroadcastId;
	if (want == had) {
		next();
		return;
	}
	auto attach = [this, base, want, next]() {
		if (want.isEmpty()) {
			settings->attachedBroadcastId.clear();
			settings->save();
			next();
			return;
		}
		api->put(base + want, [this, want, next](int, const QJsonDocument &, const QString &error) {
			if (st != State::Starting)
				return;
			if (!error.isEmpty()) {
				failStart(tr("Could not link the broadcast: %1").arg(error));
				return;
			}
			settings->attachedBroadcastId = want;
			settings->save();
			next();
		});
	};
	if (had.isEmpty()) {
		attach();
		return;
	}
	// A detach that fails (the broadcast is gone) changes nothing that matters.
	api->del(base + had, [attach](int, const QJsonDocument &, const QString &) { attach(); });
}

void Session::startRecording(bool takeover)
{
	QJsonObject recordingSettings;
	if (!settings->spokenLanguage.isEmpty())
		recordingSettings.insert("spokenLanguage", settings->spokenLanguage);
	QJsonObject body;
	if (!recordingSettings.isEmpty())
		body.insert("settings", recordingSettings);
	if (takeover)
		body.insert("takeover", true);
	api->post("/v1/transcriptions/" + settings->transcriptionId + "/recordings", body,
		  [this, takeover](int code, const QJsonDocument &doc, const QString &error) {
			  if (st != State::Starting)
				  return;
			  if (code == 409 && !takeover) {
				  // Another device holds this transcription: OBS takes it.
				  startRecording(true);
				  return;
			  }
			  if (!error.isEmpty()) {
				  failStart(tr("Could not start recording: %1").arg(error));
				  return;
			  }
			  recordingId = doc.object().value("recording").toObject().value("id").toString();
			  lastSeq = -1;
			  sentences = false;
			  resubscribeMs = RESUBSCRIBE_MS;
			  setState(State::Live);
			  emit status(tr("Live: VocalStack is transcribing"), false);
			  ingestPoll.start();
			  if (settings->showCaptions)
				  subscribe();
		  });
}

void Session::stop()
{
	end(QString());
}

// Stop, and say `error` once stopped when the stop was not asked for.
void Session::end(const QString &error)
{
	if (st == State::Idle || st == State::Stopping)
		return;
	const bool wasLive = st == State::Live;
	setState(State::Stopping);
	ingestPoll.stop();
	resubscribe.stop();
	feed.close();
	captionClear.stop();
	clearCaption();
	output.stop();

	auto done = [this, error]() {
		recordingId.clear();
		setState(State::Idle);
		if (error.isEmpty())
			emit status(tr("Stopped"), false);
		else
			emit status(error, true);
	};
	if (!wasLive || recordingId.isEmpty()) {
		done();
		return;
	}
	// A live recording is paused before it can be stopped.
	const QString base = "/v1/recordings/" + recordingId;
	api->post(base + "/pause", {}, [this, base, done](int, const QJsonDocument &, const QString &) {
		api->post(base + "/stop", {}, [this, done](int, const QJsonDocument &, const QString &stopError) {
			done();
			if (!stopError.isEmpty())
				emit status(tr("Stopped, but VocalStack said: %1").arg(stopError), true);
		});
	});
}

void Session::subscribe()
{
	if (recordingId.isEmpty())
		return;
	QUrl url(api->apiBase() + "/v1/recordings/" + recordingId + "/subscribe");
	url.setScheme(url.scheme() == "http" ? "ws" : "wss");
	if (!settings->captionLanguage.isEmpty()) {
		QUrlQuery q;
		q.addQueryItem("lang", settings->captionLanguage);
		url.setQuery(q);
	}
	feed.open(url, {{"Authorization", "Bearer " + api->key().toUtf8()},
			{"User-Agent", QByteArray("vocalstack-obs/") + PLUGIN_VERSION}});
}

void Session::setCaptionLanguage(const QString &lang)
{
	settings->captionLanguage = lang;
	settings->save();
	if (st == State::Live && settings->showCaptions && !feed.isOpen())
		subscribe();
	else if (feed.isOpen())
		feed.sendText(QString::fromUtf8(
			QJsonDocument(QJsonObject{{"type", "subscribe.update"}, {"lang", lang}}).toJson(QJsonDocument::Compact)));
}

// The words to show from a row: its translation in the caption language
// when one is chosen and has come, else what was said.
static QString shown(const QJsonObject &row, const QString &lang)
{
	if (!lang.isEmpty()) {
		const QString t = row.value("translations").toObject().value(lang).toString();
		if (!t.isEmpty())
			return t;
	}
	return row.value("text").toString();
}

static QList<qint64> seqsOf(const QJsonValue &v)
{
	QList<qint64> seqs;
	for (const QJsonValue s : v.toArray())
		seqs.append(s.toInteger());
	return seqs;
}

void Session::onFrame(const QString &text)
{
	const QJsonObject f = QJsonDocument::fromJson(text.toUtf8()).object();
	const QString type = f.value("type").toString();
	const QString lang = settings->captionLanguage;
	if (type == "transcript.committed") {
		const QJsonObject seg = f.value("segment").toObject();
		lastSeq = seg.value("seq").toInteger(lastSeq);
		// Waiting for the translation keeps the original off the screen.
		if (!sentences && (lang.isEmpty() || seg.value("translations").toObject().contains(lang)))
			showCaption(shown(seg, lang), {lastSeq});
	} else if (type == "transcript.translated" && !lang.isEmpty()) {
		const qint64 seq = f.value("seq").toInteger(-1);
		if (!sentences && seq == lastSeq)
			showCaption(f.value("translations").toObject().value(lang).toString(), {seq});
	} else if (type == "transcript.sentence") {
		// Sent again as the sentence grows and when its translation comes.
		sentences = true;
		const QJsonObject sentence = f.value("sentence").toObject();
		if (lang.isEmpty() || sentence.value("translations").toObject().contains(lang))
			showCaption(shown(sentence, lang), seqsOf(sentence.value("seqs")));
	} else if (type == "transcript.amended") {
		// The row on screen, written again. A translated caption waits for
		// the amend's own translation (transcript.translated, same seq).
		const qint64 seq = f.value("seq").toInteger(-1);
		if (lang.isEmpty() && shownSeqs == QList<qint64>{seq})
			showCaption(f.value("text").toString(), {seq});
	} else if (type == "transcript.retracted") {
		// Rows taken back (another language than the engine read); the
		// engine that reads it sends them again.
		const QList<qint64> seqs = seqsOf(f.value("seqs"));
		if (seqs.contains(lastSeq))
			lastSeq = -1; // its translation, if late, is not shown either
		for (qint64 seq : seqs)
			if (shownSeqs.contains(seq)) {
				captionClear.stop();
				clearCaption();
				break;
			}
	} else if (type == "transcript.held") {
		// Rows from here wait until the language is known: no caption
		// meanwhile (the last one would sit over speech it is not).
		if (f.value("held").toBool()) {
			captionClear.stop();
			clearCaption();
		}
	} else if (type == "transcript.languageStatus") {
		const QString state = f.value("state").toString();
		if (state == "mismatch" || state == "change_unsupported")
			setNotes(f.value("message").toString(), feedNote);
		else if (state == "matches")
			setNotes(QString(), feedNote);
	} else if (type == "error") {
		const QString code = f.value("code").toString();
		const QString message = f.value("message").toString();
		obs_log(LOG_WARNING, "caption feed error %s: %s", code.toUtf8().constData(),
			message.toUtf8().constData());
		// engine_lost ends the stream's transcription (the feed closes
		// next); another retryable error is the feed's own, and the
		// resubscribe that follows the close mends it.
		if (code == "engine_lost" || !f.value("retryable").toBool())
			setNotes(languageNote,
				 message.isEmpty() ? tr("VocalStack stopped transcribing (%1)").arg(code) : message);
	} else if (type == "recording.state") {
		const QString s = f.value("state").toString();
		if (st == State::Live && (s == "ended" || s == "finalized")) {
			// Nothing more is transcribed: stop streaming to VocalStack
			// too. The recording is over, so it is not paused and stopped.
			const QString reason = f.value("reason").toString();
			obs_log(LOG_WARNING, "VocalStack ended the recording (%s)", reason.toUtf8().constData());
			recordingId.clear();
			end(tr("VocalStack ended the recording"));
		}
	}
}

void Session::showCaption(const QString &words, const QList<qint64> &seqs)
{
	if (words.isEmpty())
		return;
	shownSeqs = seqs;
	emit caption(words);
	captionClear.start();
}

void Session::clearCaption()
{
	shownSeqs.clear();
	emit caption(QString());
}

void Session::setNotes(const QString &language, const QString &feed)
{
	languageNote = language;
	feedNote = feed;
	QStringList notes;
	for (const QString &n : {feedNote, languageNote})
		if (!n.isEmpty())
			notes.append(n);
	emit notice(notes.join("\n\n"));
}

// Whether VocalStack hears the stream: the plugin's key's live state.
void Session::pollIngest()
{
	api->get("/v1/rtmp/keys", [this](int, const QJsonDocument &doc, const QString &error) {
		if (st != State::Live || !error.isEmpty())
			return;
		for (const QJsonValue v : doc.object().value("keys").toArray()) {
			const QJsonObject k = v.toObject();
			if (k.value("id").toString() != settings->streamKeyId)
				continue;
			const QString audio = k.value("audio").toString();
			if (k.value("live").toBool())
				emit status(tr("Live: VocalStack hears %1").arg(audio.isEmpty() ? tr("the stream") : audio),
					    false);
			else
				emit status(audio.isEmpty() ? tr("Live, but no audio is reaching VocalStack yet") : audio,
					    true);
		}
	});
}
