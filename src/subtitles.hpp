#pragma once

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QTimer>

class Api;
struct Settings;

// One timed line of a transcript, in seconds from wherever the caller
// counts from.
struct Cue {
	double start = 0, end = 0;
	QString text;
};

// The transcript snapshot's sentences as cues: `wallStart` (seconds since
// the transcription went live) where VocalStack gives it, else `start`;
// translated into `lang` when given and there, the speaker's name or
// "Speaker 2" ahead once more than one speaker was heard.
QList<Cue> transcript_cues(const QJsonObject &snapshot, const QString &lang);
QString srt_text(const QList<Cue> &cues);
QString vtt_text(const QList<Cue> &cues);
// A YouTube description: the title and summary VocalStack wrote (when they
// are ready), then chapters at the transcript's longer pauses, named by
// their first words. YouTube needs at least three chapters, the first at
// 0:00, each at least ten seconds long; a shorter transcript has none.
QString youtube_text(const QString &title, const QString &summary, const QList<Cue> &cues);

// Subtitle files beside each OBS recording, from the live transcriptions
// that ran while it recorded: <recording>.srt (and .vtt), and
// <recording>.<lang>.srt for a translation. Times follow the recording,
// pauses taken out. Written a little after the recording stops, when the
// last words are in.
class RecordingSubtitles : public QObject {
	Q_OBJECT

public:
	RecordingSubtitles(Api *api, Settings *settings, QObject *parent = nullptr);

	// From OBS's frontend events.
	void recordingStarted();
	void recordingPaused(bool paused);
	void recordingStopped();
	// From the session.
	void transcriptionLive(const QString &id, qint64 sinceMs);
	void transcriptionEnded(const QString &id);

signals:
	// The files were written (paths), or why they were not.
	void saved(const QStringList &paths);
	void failed(const QString &error);

private:
	struct Live {
		QString id;
		qint64 fromMs = 0, toMs = 0; // toMs 0: still live
	};
	struct Job {
		QString path; // the recording
		qint64 startMs = 0, endMs = 0;
		QList<QPair<qint64, qint64>> pauses;
		QList<Live> transcriptions;
		QString format, lang;
		int tries = 0;
	};
	void write(Job job);
	// The recording's time (s) of a moment on this computer's clock, or -1
	// when it was not recording then.
	static double recordingTime(const Job &job, qint64 ms);

	Api *api;
	Settings *settings;
	bool recording = false;
	qint64 recStartMs = 0, pausedAt = 0;
	QList<QPair<qint64, qint64>> pauses;
	QList<Live> live;
};
