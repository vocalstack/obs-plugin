#include "session.hpp"

#include "api.hpp"

#include <plugin-support.h>

#include <QHostInfo>
#include <QJsonArray>
#include <QUrlQuery>
#include <QUuid>

// A caption stays this long after the last words.
static const int CAPTION_HOLD_MS = 8000;
// A feed that closed is opened again after 3 s, then 6, 12… up to a
// minute while it keeps closing; at once when the server closed it to
// restart (1012: a relay draining for a deploy), where the same
// subscription goes on.
static const int RESUBSCRIBE_MS = 3000;
static const int RESUBSCRIBE_MAX_MS = 60000;
static const int CLOSE_RESTART = 1012;
// The server refused what the socket asked (a caption language past the
// recording's limit of languages).
static const int CLOSE_POLICY = 1008;

Session::Session(Api *api_, Settings *settings_, QObject *parent)
	: QObject(parent),
	  api(api_),
	  settings(settings_),
	  resubscribeMs(RESUBSCRIBE_MS),
	  reopenEventsMs(RESUBSCRIBE_MS)
{
	connect(&output, &VocalStackOutput::started, this, [this]() {
		if (st == State::Starting)
			refreshLimits([this]() {
				ensureTranscription([this]() {
					applyTranscriptionSettings(
						[this]() { attachBroadcast([this]() { startRecording(false); }); });
				});
			});
	});
	connect(&output, &VocalStackOutput::reconnecting, this,
		[this]() { emit status(tr("Reconnecting…"), Session::Level::Warn); });
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
		if (st != State::Live || reason.isEmpty())
			return;
		obs_log(LOG_WARNING, "caption feed closed (%d): %s", code, reason.toUtf8().constData());
		if (code == CLOSE_RESTART) {
			resubscribe.start(0);
			return;
		}
		if (code == CLOSE_POLICY && !settings->captionLanguage.isEmpty()) {
			// The caption language was refused (the recording has all the
			// languages it may): captions as spoken instead, at once.
			settings->captionLanguage.clear();
			settings->save();
			emit captionLanguageRefused(reason);
			resubscribe.start(0);
			return;
		}
		resubscribe.start(resubscribeMs);
		resubscribeMs = qMin(resubscribeMs * 2, RESUBSCRIBE_MAX_MS);
	});
	resubscribe.setSingleShot(true);
	connect(&resubscribe, &QTimer::timeout, this, [this]() {
		if (st == State::Live && settings->showCaptions)
			subscribe();
	});

	connect(&events, &WebSocket::textMessage, this, &Session::onEvent);
	connect(&events, &WebSocket::opened, this, [this]() {
		reopenEventsMs = RESUBSCRIBE_MS;
		ingestPoll.stop(); // the feed says it now
	});
	connect(&events, &WebSocket::closed, this, [this](const QString &reason, int code) {
		if (st != State::Live || reason.isEmpty())
			return;
		obs_log(LOG_WARNING, "event feed closed (%d): %s", code, reason.toUtf8().constData());
		ingestPoll.start(); // until it is back
		if (code == CLOSE_RESTART) {
			reopenEvents.start(0);
			return;
		}
		reopenEvents.start(reopenEventsMs);
		reopenEventsMs = qMin(reopenEventsMs * 2, RESUBSCRIBE_MAX_MS);
	});
	reopenEvents.setSingleShot(true);
	connect(&reopenEvents, &QTimer::timeout, this, [this]() {
		if (st == State::Live)
			openEvents();
	});

	captionClear.setSingleShot(true);
	captionClear.setInterval(CAPTION_HOLD_MS);
	connect(&captionClear, &QTimer::timeout, this, &Session::clearCaption);

	// Whether VocalStack hears the stream, while the event feed is not
	// there to say so.
	ingestPoll.setInterval(5000);
	connect(&ingestPoll, &QTimer::timeout, this, &Session::pollIngest);

	broadcastPoll.setInterval(15000);
	connect(&broadcastPoll, &QTimer::timeout, this, &Session::pollBroadcast);
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
	emit status(tr("Connecting…"), Session::Level::Warn);
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
	emit status(error, Session::Level::Error);
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

// The plan's longest live recording: a recording ends there, so the
// transcription asks for all of it (VocalStack's own default is 2 hours).
void Session::refreshLimits(std::function<void()> then)
{
	api->get("/v1/realtime/capabilities", [this, then](int, const QJsonDocument &doc, const QString &error) {
		if (error.isEmpty()) {
			const QJsonValue v = doc.object().value("limits").toObject().value("recordingMaxDurationS");
			const int s = v.isDouble() ? int(v.toDouble()) : 0;
			if (s > 0 && s != settings->maxDurationS) {
				settings->maxDurationS = s;
				settings->save();
				emit limitsChanged();
			}
		}
		if (then)
			then();
	});
}

void Session::ensureTranscription(Next next)
{
	if (settings->transcriptionId.isEmpty()) {
		createTranscription(next);
		return;
	}
	// Still there? It may have been deleted in the dashboard, or
	// disabled by VocalStack: a new one then.
	api->get("/v1/transcriptions/" + settings->transcriptionId,
		 [this, next](int status, const QJsonDocument &doc, const QString &error) {
			 if (st != State::Starting)
				 return;
			 const bool blocked = status == 403 && Api::errorDetails(doc).value("blocked").toBool();
			 if (status == 404 || blocked) {
				 if (blocked)
					 obs_log(LOG_WARNING, "VocalStack disabled transcription %s: %s",
						 settings->transcriptionId.toUtf8().constData(),
						 error.toUtf8().constData());
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

// "OBS on Alexs-MacBook-Pro": the computer's name, without macOS's .local.
static QString default_name()
{
	QString host = QHostInfo::localHostName();
	if (host.endsWith(".local"))
		host.chop(6);
	return QStringLiteral("OBS on %1").arg(host);
}

// VocalStack's capture settings for the transcription, from the dock.
QJsonObject Session::recordingSettings() const
{
	QJsonObject s{
		{"spokenLanguage", settings->spokenLanguage.isEmpty() ? QJsonValue() : QJsonValue(settings->spokenLanguage)},
		{"speakerLabels", settings->speakerLabels.isEmpty() ? "auto" : settings->speakerLabels},
		{"latencyMode", settings->latencyMode.isEmpty() ? "auto" : settings->latencyMode},
		// The language model's title and summary of each recording: on,
		// as the dashboard has it (VocalStack's own default is off).
		{"summary", true},
		{"title", true},
	};
	if (settings->maxDurationS > 0)
		s.insert("maxDurationS", settings->maxDurationS);
	return s;
}

// VocalStack would not take the recording length asked for (the plan's
// limit it gave before has changed): once more without it. True when so.
bool Session::dropLimit(const QJsonDocument &doc)
{
	if (settings->maxDurationS <= 0 || Api::errorCode(doc) != "invalid_options")
		return false;
	obs_log(LOG_WARNING, "VocalStack refused maxDurationS %d: asking without it", settings->maxDurationS);
	settings->maxDurationS = 0;
	settings->save();
	emit limitsChanged();
	return true;
}

void Session::createTranscription(Next next)
{
	QJsonObject body{
		{"name", settings->transcriptionName.isEmpty() ? default_name() : settings->transcriptionName},
		{"source", QJsonObject{{"kind", "stream"}, {"url", settings->serverUrl}}},
		{"settings", recordingSettings()},
	};
	api->post("/v1/transcriptions/live", body, [this, next](int, const QJsonDocument &doc, const QString &error) {
		if (st != State::Starting)
			return;
		if (!error.isEmpty()) {
			if (dropLimit(doc)) {
				createTranscription(next);
				return;
			}
			failStart(tr("Could not create the live transcription: %1").arg(error));
			return;
		}
		settings->transcriptionId = doc.object().value("id").toString();
		settings->attachedBroadcastId.clear();
		settings->save();
		next();
	});
}

// The dock's name, lock and recording settings, written to the
// transcription each Start: what it was made with may have changed since.
void Session::applyTranscriptionSettings(Next next)
{
	QJsonObject body{
		{"name", settings->transcriptionName.isEmpty() ? default_name() : settings->transcriptionName},
		{"locked", settings->locked},
		{"settings", recordingSettings()},
	};
	api->patch("/v1/transcriptions/" + settings->transcriptionId, body,
		   [this, next](int, const QJsonDocument &doc, const QString &error) {
			   if (st != State::Starting)
				   return;
			   if (!error.isEmpty()) {
				   if (dropLimit(doc)) {
					   applyTranscriptionSettings(next);
					   return;
				   }
				   failStart(tr("Could not set up the transcription: %1").arg(error));
				   return;
			   }
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

// Why a recording could not start, for the dock: the plan's credit or
// caps link to the app's billing page.
static QString start_error(const QString &appBase, const QString &code, const QString &message)
{
	if (code == "insufficient_credit" || code == "usage_cap_reached" || code == "quota_exceeded")
		return QStringLiteral("%1 <a href=\"%2/billing\">%3</a>")
			.arg(message.toHtmlEscaped(), appBase, QObject::tr("Open billing"));
	return QObject::tr("Could not start recording: %1").arg(message);
}

void Session::startRecording(bool takeover)
{
	QJsonObject body;
	if (takeover)
		body.insert("takeover", true);
	// A start sent again after a timeout is answered as the first was,
	// not recorded twice.
	const QString key = QUuid::createUuid().toString(QUuid::WithoutBraces);
	api->post("/v1/transcriptions/" + settings->transcriptionId + "/recordings", body, key,
		  [this, takeover](int code, const QJsonDocument &doc, const QString &error) {
			  if (st != State::Starting)
				  return;
			  if (code == 409 && !takeover && Api::errorCode(doc) == "conflict") {
				  // Another device holds this transcription: OBS takes it.
				  startRecording(true);
				  return;
			  }
			  if (!error.isEmpty()) {
				  failStart(start_error(api->appBase(), Api::errorCode(doc), error));
				  return;
			  }
			  recordingId = doc.object().value("recording").toObject().value("id").toString();
			  lastSeq = -1;
			  readyCursor = -1;
			  sentences = false;
			  resuming = false;
			  speakers.clear();
			  inSentence.clear();
			  emit transcriptCleared();
			  emitSpeakers();
			  shownSpeaker = -1;
			  resubscribeMs = reopenEventsMs = RESUBSCRIBE_MS;
			  setState(State::Live);
			  emit status(tr("Transcribing"), Session::Level::Ok);
			  ingestPoll.start();
			  openEvents();
			  if (settings->showCaptions)
				  subscribe();
			  if (!settings->broadcastId.isEmpty()) {
				  broadcastPoll.start();
				  pollBroadcast();
			  }
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
	broadcastPoll.stop();
	resubscribe.stop();
	reopenEvents.stop();
	feed.close();
	events.close();
	captionClear.stop();
	clearCaption();
	output.stop();
	emit listeners(-1);

	auto done = [this, error]() {
		recordingId.clear();
		setState(State::Idle);
		if (error.isEmpty())
			emit status(tr("Stopped"), Session::Level::Ok);
		else
			emit status(error, Session::Level::Error);
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
				emit status(tr("Stopped, but VocalStack said: %1").arg(stopError), Session::Level::Warn);
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
	readyCursor = -1;
	feed.open(url, {{"Authorization", "Bearer " + api->key().toUtf8()},
			{"User-Agent", QByteArray("vocalstack-obs/") + PLUGIN_VERSION}});
}

// The transcription's event feed: where the stream's audio stands
// (stream.ingest), the recording ending (recording.ended).
void Session::openEvents()
{
	if (settings->transcriptionId.isEmpty())
		return;
	QUrl url(api->apiBase() + "/v1/transcriptions/" + settings->transcriptionId + "/events");
	url.setScheme(url.scheme() == "http" ? "ws" : "wss");
	events.open(url, {{"Authorization", "Bearer " + api->key().toUtf8()},
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

static int speakerOf(const QJsonObject &row)
{
	return row.value("speaker").isDouble() ? row.value("speaker").toInt() : -1;
}

// "Name" or "Speaker 2", from VocalStack's numbering; empty until more
// than one speaker has been heard (one speaker needs no label).
QString Session::speakerLabel(int speaker) const
{
	if (speaker < 0 || speakers.size() < 2 || !speakers.contains(speaker))
		return QString();
	const Speaker &s = speakers.value(speaker);
	return s.name.isEmpty() ? tr("Speaker %1").arg(s.number) : s.name;
}

void Session::onFrame(const QString &text)
{
	const QJsonObject f = QJsonDocument::fromJson(text.toUtf8()).object();
	const QString type = f.value("type").toString();
	const QString lang = settings->captionLanguage;
	// A feed opened again replays what was sent before its cursor.
	if (f.value("cursor").isDouble() && f.value("cursor").toInteger() <= readyCursor)
		return;
	if (type == "subscribe.ready") {
		readyCursor = f.value("lastSeq").toInteger(-1);
		const QString s = f.value("state").toString();
		if (st == State::Live && (s == "ended" || s == "finalized")) {
			recordingId.clear();
			end(tr("VocalStack ended the recording"));
		}
	} else if (type == "transcript.committed") {
		const QJsonObject seg = f.value("segment").toObject();
		lastSeq = seg.value("seq").toInteger(lastSeq);
		if (!inSentence.contains(lastSeq))
			emit transcriptLine(QStringLiteral("r:%1").arg(lastSeq), seg.value("start").toDouble(-1),
					    speakerOf(seg), seg.value("text").toString());
		// Shown the moment it is heard, as the dashboard does; the sentence
		// it belongs to takes its place when it comes. A translated
		// caption waits for its translation.
		if (lang.isEmpty() || seg.value("translations").toObject().contains(lang))
			showCaption(shown(seg, lang), {lastSeq}, speakerOf(seg));
	} else if (type == "transcript.translated" && !lang.isEmpty()) {
		const qint64 seq = f.value("seq").toInteger(-1);
		if (!sentences && seq == lastSeq)
			showCaption(f.value("translations").toObject().value(lang).toString(), {seq}, shownSpeaker);
	} else if (type == "transcript.sentence") {
		// Sent again as the sentence grows and when its translation comes.
		sentences = true;
		const QJsonObject sentence = f.value("sentence").toObject();
		const QString id = QStringLiteral("s:%1").arg(sentence.value("sentenceId").toInteger());
		if (sentence.value("status").toString() == "dropped") {
			emit transcriptRemoved({id});
			return;
		}
		const QList<qint64> seqs = seqsOf(sentence.value("seqs"));
		QStringList rows;
		for (qint64 q : seqs)
			if (!inSentence.contains(q)) {
				inSentence.insert(q);
				rows.append(QStringLiteral("r:%1").arg(q));
			}
		if (!rows.isEmpty())
			emit transcriptRemoved(rows);
		emit transcriptLine(id, sentence.value("start").toDouble(-1), speakerOf(sentence),
				    sentence.value("text").toString());
		// The caption follows the sentence that holds the newest row; an
		// older one, finished late, does not pull the caption back.
		if (seqs.contains(lastSeq) &&
		    (lang.isEmpty() || sentence.value("translations").toObject().contains(lang)))
			showCaption(shown(sentence, lang), seqs, speakerOf(sentence));
	} else if (type == "transcript.amended") {
		// The row on screen, written again. A translated caption waits for
		// the amend's own translation (transcript.translated, same seq).
		const qint64 seq = f.value("seq").toInteger(-1);
		if (!inSentence.contains(seq))
			emit transcriptLine(QStringLiteral("r:%1").arg(seq), -1,
					    f.value("speaker").isDouble() ? f.value("speaker").toInt() : -1,
					    f.value("text").toString());
		if (lang.isEmpty() && shownSeqs == QList<qint64>{seq})
			showCaption(f.value("text").toString(), {seq},
				    f.value("speaker").isDouble() ? f.value("speaker").toInt() : shownSpeaker);
	} else if (type == "transcript.corrected") {
		// Somebody corrected a line in the dashboard.
		const qint64 seq = f.value("seq").toInteger(-1);
		if (f.value("sentenceId").isDouble() && f.value("sentenceText").isString())
			emit transcriptLine(QStringLiteral("s:%1").arg(f.value("sentenceId").toInteger()), -1, -1,
					    f.value("sentenceText").toString());
		else if (!inSentence.contains(seq))
			emit transcriptLine(QStringLiteral("r:%1").arg(seq), -1, -1, f.value("text").toString());
		if (lang.isEmpty() && shownSeqs.contains(seq)) {
			const QString sentenceText = f.value("sentenceText").toString();
			showCaption(shownSeqs.size() > 1 && !sentenceText.isEmpty() ? sentenceText
										   : f.value("text").toString(),
				    shownSeqs, shownSpeaker);
		}
	} else if (type == "transcript.retracted") {
		// Rows taken back (another language than the engine read); the
		// engine that reads it sends them again.
		const QList<qint64> seqs = seqsOf(f.value("seqs"));
		QStringList rows;
		for (qint64 q : seqs)
			rows.append(QStringLiteral("r:%1").arg(q));
		emit transcriptRemoved(rows);
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
	} else if (type == "transcript.speakerNumbers") {
		// Every speaker's number and name, in full.
		speakers.clear();
		for (const QJsonValue v : f.value("speakers").toArray()) {
			const QJsonObject s = v.toObject();
			speakers.insert(s.value("speaker").toInt(),
					{s.value("number").toInt(), s.value("name").toString()});
		}
		emitSpeakers();
	} else if (type == "transcript.speakerNamed") {
		const int speaker = f.value("speaker").toInt();
		if (speakers.contains(speaker)) {
			speakers[speaker].name = f.value("name").toString();
			emitSpeakers();
		}
	} else if (type == "transcript.languageStatus") {
		const QString state = f.value("state").toString();
		if (state == "mismatch" || state == "change_unsupported")
			setNotes(f.value("message").toString() + " " +
					 tr("Change the spoken language after you stop."),
				 feedNote);
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
		if (code == "engine_lost" || !f.value("retryable").toBool()) {
			feedError = true;
			setNotes(languageNote,
				 message.isEmpty() ? tr("VocalStack stopped transcribing (%1)").arg(code) : message);
		}
	} else if (type == "recording.settings") {
		// Changed in the dashboard while recording: the dock follows.
		const QJsonObject s = f.value("settings").toObject();
		if (s.contains("spokenLanguage")) {
			const QString l = s.value("spokenLanguage").toString();
			if (l != settings->spokenLanguage) {
				settings->spokenLanguage = l;
				settings->save();
				emit spokenLanguageChanged(l);
			}
		}
	} else if (type == "recording.expiring") {
		const int minutes = qMax(1, int(f.value("endsInS").toDouble() / 60));
		feedError = false;
		setNotes(languageNote, tr("VocalStack ends this recording in %n minute(s): the plan's longest recording. "
					  "Stop and start again for a new one.",
					  nullptr, minutes));
	} else if (type == "recording.silence") {
		const int silent = int(f.value("silentS").toDouble()), in = int(f.value("pausesInS").toDouble());
		emit status(tr("No speech for %1 s. Pausing in %2 s.").arg(silent).arg(in), Session::Level::Warn);
	} else if (type == "recording.state") {
		const QString s = f.value("state").toString();
		if (st != State::Live)
			return;
		if (s == "ended" || s == "finalized") {
			// Nothing more is transcribed: stop streaming to VocalStack
			// too. The recording is over, so it is not paused and stopped.
			const QString reason = f.value("reason").toString();
			obs_log(LOG_WARNING, "VocalStack ended the recording (%s)", reason.toUtf8().constData());
			recordingId.clear();
			end(tr("VocalStack ended the recording"));
		} else if (s == "paused") {
			// Paused by VocalStack (silence) or in the dashboard, while OBS
			// still streams: left paused it would end, so it goes on.
			resume();
		} else if (s == "live") {
			resuming = false;
			emit status(tr("Transcribing"), Session::Level::Ok);
		}
	}
}

void Session::resume()
{
	if (resuming || recordingId.isEmpty())
		return;
	resuming = true;
	emit status(tr("Paused by VocalStack · resuming…"), Session::Level::Warn);
	api->post("/v1/recordings/" + recordingId + "/resume", {},
		  [this](int, const QJsonDocument &, const QString &error) {
			  if (st != State::Live)
				  return;
			  if (!error.isEmpty()) {
				  resuming = false;
				  emit status(tr("Paused, and VocalStack would not resume: %1").arg(error),
					      Session::Level::Error);
			  }
		  });
}

void Session::onEvent(const QString &text)
{
	const QJsonObject f = QJsonDocument::fromJson(text.toUtf8()).object();
	const QString type = f.value("type").toString();
	if (st != State::Live)
		return;
	if (type == "stream.ingest") {
		if (f.value("recordingId").toString() == recordingId)
			onIngest(f);
	} else if (type == "recording.ended") {
		if (f.value("recordingId").toString() == recordingId) {
			obs_log(LOG_WARNING, "VocalStack ended the recording");
			recordingId.clear();
			end(tr("VocalStack ended the recording"));
		}
	}
}

// Where VocalStack's pull of the stream stands.
void Session::onIngest(const QJsonObject &f)
{
	const QString state = f.value("state").toString();
	const QString detected = f.value("detected").toString(), error = f.value("error").toString();
	if (state == "streaming")
		emit status(tr("Hearing %1").arg(detected.isEmpty() ? tr("the stream") : detected), Session::Level::Ok);
	else if (state == "connecting")
		emit status(error.isEmpty() ? tr("Waiting for audio…") : error, Session::Level::Warn);
	else if (state == "reconnecting")
		emit status(error.isEmpty() ? tr("Audio lost · reconnecting…") : tr("Audio lost (%1) · reconnecting…").arg(error),
			    Session::Level::Warn);
	else if (state == "failed")
		end(error.isEmpty() ? tr("VocalStack gave up on the stream") : tr("VocalStack gave up on the stream: %1").arg(error));
}

void Session::emitSpeakers()
{
	QHash<int, QString> labels;
	for (auto it = speakers.cbegin(); it != speakers.cend(); ++it)
		labels.insert(it.key(), speakerLabel(it.key()));
	emit speakerLabels(labels);
}

void Session::showCaption(const QString &words, const QList<qint64> &seqs, int speaker)
{
	if (words.isEmpty())
		return;
	shownSeqs = seqs;
	shownSpeaker = speaker;
	const QString label = speakerLabel(speaker);
	emit caption(label, words);
	captionClear.start();
}

void Session::clearCaption()
{
	shownSeqs.clear();
	shownSpeaker = -1;
	emit caption(QString(), QString());
}

void Session::setNotes(const QString &language, const QString &feed)
{
	languageNote = language;
	feedNote = feed;
	QStringList warnings, errors;
	if (!feedNote.isEmpty())
		(feedError ? errors : warnings).append(feedNote);
	if (!languageNote.isEmpty())
		warnings.append(languageNote);
	emit notices(warnings, errors);
}

// Whether VocalStack hears the stream: the plugin's key's live state.
void Session::pollIngest()
{
	api->get("/v1/rtmp/keys", [this](int, const QJsonDocument &doc, const QString &error) {
		if (st != State::Live || !error.isEmpty() || events.isOpen())
			return;
		for (const QJsonValue v : doc.object().value("keys").toArray()) {
			const QJsonObject k = v.toObject();
			if (k.value("id").toString() != settings->streamKeyId)
				continue;
			const QString audio = k.value("audio").toString();
			if (k.value("live").toBool())
				emit status(tr("Hearing %1").arg(audio.isEmpty() ? tr("the stream") : audio), Session::Level::Ok);
			else
				emit status(audio.isEmpty() ? tr("Waiting for audio…") : audio, Session::Level::Warn);
		}
	});
}

// People on the broadcast's link.
void Session::pollBroadcast()
{
	if (settings->broadcastId.isEmpty())
		return;
	api->get("/v1/broadcasts/" + settings->broadcastId, [this](int, const QJsonDocument &doc, const QString &error) {
		if (st != State::Live || !error.isEmpty())
			return;
		const QJsonValue n = doc.object().value("listenersNow");
		emit listeners(n.isDouble() ? n.toInt() : 0);
	});
}
