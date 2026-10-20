#pragma once

#include "settings.hpp"
#include "vocalstack-output.hpp"
#include "websocket.hpp"

#include <QObject>
#include <QTimer>

#include <functional>

class Api;

// One VocalStack session: the stream to VocalStack, the recording on the
// plugin's live transcription, and the caption feed. Start runs: stream
// key (made once, kept) → output → broadcast attached → recording started
// → captions subscribed. Stop runs it back.
class Session : public QObject {
	Q_OBJECT

public:
	enum class State { Idle, Starting, Live, Stopping };

	Session(Api *api, Settings *settings, QObject *parent = nullptr);

	State state() const { return st; }
	void start();
	void stop();
	// The caption language changed while live: follow it without a restart.
	void setCaptionLanguage(const QString &lang);

signals:
	void stateChanged(Session::State state);
	// What the dock shows under the button; `error` makes it red.
	void status(const QString &text, bool error);
	void caption(const QString &text);

private:
	using Next = std::function<void()>;

	void setState(State s);
	void end(const QString &error);
	void failStart(const QString &error);
	void ensureStreamKey(Next next);
	void ensureTranscription(Next next);
	void createTranscription(Next next);
	void attachBroadcast(Next next);
	void startRecording(bool takeover);
	void subscribe();
	void onFrame(const QString &text);
	void pollIngest();

	Api *api;
	Settings *settings;
	VocalStackOutput output;
	WebSocket feed;
	QTimer ingestPoll, captionClear, resubscribe;
	State st = State::Idle;
	QString recordingId;
	qint64 lastSeq = -1;
};
