#pragma once

#include "settings.hpp"
#include "vocalstack-output.hpp"
#include "websocket.hpp"

#include <QHash>
#include <QSet>
#include <QJsonDocument>
#include <QObject>
#include <QTimer>

#include <functional>

class Api;

// One VocalStack session: the stream to VocalStack, the recording on the
// plugin's live transcription, the transcription's event feed and the
// caption feed. Start runs: stream key (made once, kept) → output →
// the plan's limits → transcription (made once, its settings written each
// time) → broadcast attached → recording started → events and captions
// subscribed. Stop runs it back.
class Session : public QObject {
	Q_OBJECT

public:
	enum class State { Idle, Starting, Live, Stopping };
	// How a status reads: as it should be, a passing warning (reconnecting,
	// silence), or a failure.
	enum class Level { Ok, Warn, Error };

	Session(Api *api, Settings *settings, QObject *parent = nullptr);

	State state() const { return st; }
	void start();
	void stop();
	// The broadcast script the next Start follows (an id from
	// GET /v1/broadcasts/{id}/scripts); empty: none. Used once.
	void setScript(const QString &id) { scriptId = id; }
	// The script the live recording follows, if any.
	QString followingScript() const { return followedScript; }
	// The caption language changed while live: follow it without a restart.
	void setCaptionLanguage(const QString &lang);
	// Ask VocalStack the plan's longest live recording (kept in the
	// settings); on sign-in, and before each Start.
	void refreshLimits(std::function<void()> then = {});

signals:
	void stateChanged(Session::State state);
	// What the dock shows about the session. The text may carry a link
	// (HTML).
	void status(const QString &text, Session::Level level);
	// The words to caption, and who said them ("Speaker 2", a name) once
	// more than one speaker has been heard; empty words clear it.
	void caption(const QString &speaker, const QString &text);
	// Notes that stay until the next Start, one per item: warnings (the
	// speech is not the spoken language set, the plan's cutoff is near)
	// and errors (VocalStack stopped transcribing). Empty hides them.
	void notices(const QStringList &warnings, const QStringList &errors);
	// The running transcript, for the transcript panel: a line added or
	// rewritten (`id` "r:<seq>" for a row, "s:<id>" for a sentence that
	// takes its rows' place; `start` in media seconds, -1 to keep its
	// place), lines taken back, and a new recording starting afresh.
	void transcriptLine(const QString &id, double start, int speaker, const QString &text);
	void transcriptRemoved(const QStringList &ids);
	void transcriptCleared();
	// What each speaker is called ("Speaker 2", a name), once more than one
	// has been heard; empty otherwise.
	void speakerLabels(const QHash<int, QString> &labels);
	// VocalStack refused the caption language (its limit of languages a
	// recording, say): captions went back to the spoken language.
	void captionLanguageRefused(const QString &message);
	// The spoken language was changed in VocalStack's dashboard while
	// recording; the settings follow it.
	void spokenLanguageChanged(const QString &lang);
	// People on the chosen broadcast's link while live; -1 when not live
	// or no broadcast is chosen.
	void listeners(int count);
	// settings->maxDurationS was read from VocalStack.
	void limitsChanged();

private:
	using Next = std::function<void()>;

	void setState(State s);
	void end(const QString &error);
	void failStart(const QString &error);
	void ensureStreamKey(Next next);
	void ensureTranscription(Next next);
	void createTranscription(Next next);
	void applyTranscriptionSettings(Next next);
	bool dropLimit(const QJsonDocument &doc);
	void attachBroadcast(Next next);
	void startRecording(bool takeover);
	QJsonObject recordingSettings() const;
	void subscribe();
	void openEvents();
	void onFrame(const QString &text);
	void onEvent(const QString &text);
	void onIngest(const QJsonObject &f);
	void showCaption(const QString &words, const QList<qint64> &seqs, int speaker);
	void clearCaption();
	void setNotes(const QString &language, const QString &feed);
	void pollIngest();
	void pollBroadcast();
	void resume();
	QString speakerLabel(int speaker) const;

	Api *api;
	Settings *settings;
	VocalStackOutput output;
	// The caption feed (the recording's subscribe socket) and the
	// transcription's event feed (where the stream's audio stands,
	// recordings starting and ending).
	WebSocket feed, events;
	QTimer ingestPoll, captionClear, resubscribe, reopenEvents, broadcastPoll;
	State st = State::Idle;
	QString recordingId;
	QString scriptId, followedScript;
	bool scriptRefused = false;
	qint64 lastSeq = -1;
	// The feed's cursor when it was (re)opened: frames at or below it
	// were seen before, and are not captions again.
	qint64 readyCursor = -1;
	// The rows the caption on screen was made from, and who spoke them.
	QList<qint64> shownSeqs;
	int shownSpeaker = -1;
	// The recording sends sentences (sentences.enabled): the caption is
	// made from them only, not also from the rows they are made of.
	bool sentences = false;
	// Each speaker's display number and name (transcript.speakerNumbers).
	struct Speaker {
		int number = 0;
		QString name;
	};
	QHash<int, Speaker> speakers;
	// Rows a sentence has taken in: the panel shows the sentence, not them.
	QSet<qint64> inSentence;
	void emitSpeakers();
	// The wait before a feed is opened again; it doubles while the feed
	// keeps closing.
	int resubscribeMs, reopenEventsMs;
	// A resume is on its way for a recording VocalStack paused.
	bool resuming = false;
	QString languageNote, feedNote;
	// The feed note is an error (VocalStack stopped), not a warning.
	bool feedError = false;
};
