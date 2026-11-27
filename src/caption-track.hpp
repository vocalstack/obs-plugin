#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

// What to caption from one subscribe socket, in one language: the newest
// row the moment it is heard, replaced by its sentence when that comes,
// rewritten when the engine amends it or a person corrects it, and cleared
// a while after the last words. One for the preview overlay (in Session),
// one per language the stream's captions and the scene's caption sources
// ask for (CaptionFeeds).
class CaptionTrack : public QObject {
	Q_OBJECT

public:
	explicit CaptionTrack(QObject *parent = nullptr);

	// The language captions are in: empty, as spoken; else a translation
	// target ("es"), whose caption waits for its translation.
	void setLanguage(const QString &language) { lang = language; }
	QString language() const { return lang; }
	// A new transcription, or a socket opened afresh: nothing is shown.
	void reset();
	// One frame of the subscribe socket, replays already left out.
	void onFrame(const QJsonObject &frame);
	// Clear what is shown, now.
	void clear();

signals:
	// The words to caption, and who said them ("Speaker 2", a name) once
	// more than one speaker has been heard; empty words clear it.
	void caption(const QString &speaker, const QString &text);

private:
	void show(const QString &words, const QList<qint64> &seqs, int speaker);
	QString speakerLabel(int speaker) const;

	QString lang;
	QTimer hold;
	qint64 lastSeq = -1;
	// The rows the caption on screen was made from, and who spoke them.
	QList<qint64> shownSeqs;
	int shownSpeaker = -1;
	// The transcription sends sentences: the caption is made from them
	// only, not also from the rows they are made of.
	bool sentences = false;
	struct Speaker {
		int number = 0;
		QString name;
	};
	QHash<int, Speaker> speakers;
};
