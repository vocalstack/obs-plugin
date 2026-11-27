#include "caption-track.hpp"

#include <QJsonArray>

// A caption stays this long after the last words.
static const int CAPTION_HOLD_MS = 8000;

CaptionTrack::CaptionTrack(QObject *parent) : QObject(parent)
{
	hold.setSingleShot(true);
	hold.setInterval(CAPTION_HOLD_MS);
	connect(&hold, &QTimer::timeout, this, &CaptionTrack::clear);
}

void CaptionTrack::reset()
{
	hold.stop();
	lastSeq = -1;
	sentences = false;
	speakers.clear();
	clear();
}

void CaptionTrack::clear()
{
	hold.stop();
	shownSeqs.clear();
	shownSpeaker = -1;
	emit caption(QString(), QString());
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

static QList<qint64> seqs_of(const QJsonValue &v)
{
	QList<qint64> seqs;
	for (const QJsonValue s : v.toArray())
		seqs.append(s.toInteger());
	return seqs;
}

static int speaker_of(const QJsonObject &row)
{
	return row.value("speaker").isDouble() ? row.value("speaker").toInt() : -1;
}

// "Name" or "Speaker 2", from VocalStack's numbering; empty until more
// than one speaker has been heard (one speaker needs no label).
QString CaptionTrack::speakerLabel(int speaker) const
{
	if (speaker < 0 || speakers.size() < 2 || !speakers.contains(speaker))
		return QString();
	const Speaker &s = speakers.value(speaker);
	return s.name.isEmpty() ? tr("Speaker %1").arg(s.number) : s.name;
}

void CaptionTrack::show(const QString &words, const QList<qint64> &seqs, int speaker)
{
	if (words.isEmpty())
		return;
	shownSeqs = seqs;
	shownSpeaker = speaker;
	emit caption(speakerLabel(speaker), words);
	hold.start();
}

void CaptionTrack::onFrame(const QJsonObject &f)
{
	const QString type = f.value("type").toString();
	if (type == "transcript.committed") {
		const QJsonObject seg = f.value("segment").toObject();
		lastSeq = seg.value("seq").toInteger(lastSeq);
		// Shown the moment it is heard, as the dashboard does; the sentence
		// it belongs to takes its place when it comes. A translated caption
		// waits for its translation.
		if (lang.isEmpty() || seg.value("translations").toObject().contains(lang))
			show(shown(seg, lang), {lastSeq}, speaker_of(seg));
	} else if (type == "transcript.translated" && !lang.isEmpty()) {
		const qint64 seq = f.value("seq").toInteger(-1);
		if (!sentences && seq == lastSeq)
			show(f.value("translations").toObject().value(lang).toString(), {seq}, shownSpeaker);
	} else if (type == "transcript.sentence") {
		// Sent again as the sentence grows and when its translation comes.
		sentences = true;
		const QJsonObject sentence = f.value("sentence").toObject();
		if (sentence.value("status").toString() == "dropped")
			return;
		const QList<qint64> seqs = seqs_of(sentence.value("seqs"));
		// The caption follows the sentence that holds the newest row; an
		// older one, finished late, does not pull the caption back.
		if (seqs.contains(lastSeq) && (lang.isEmpty() || sentence.value("translations").toObject().contains(lang)))
			show(shown(sentence, lang), seqs, speaker_of(sentence));
	} else if (type == "transcript.amended") {
		// The row on screen, written again. A translated caption waits for
		// the amend's own translation (transcript.translated, same seq).
		const qint64 seq = f.value("seq").toInteger(-1);
		if (lang.isEmpty() && shownSeqs == QList<qint64>{seq})
			show(f.value("text").toString(), {seq},
			     f.value("speaker").isDouble() ? f.value("speaker").toInt() : shownSpeaker);
	} else if (type == "transcript.corrected") {
		// Somebody corrected a line in the dashboard.
		const qint64 seq = f.value("seq").toInteger(-1);
		if (lang.isEmpty() && shownSeqs.contains(seq)) {
			const QString sentenceText = f.value("sentenceText").toString();
			show(shownSeqs.size() > 1 && !sentenceText.isEmpty() ? sentenceText : f.value("text").toString(),
			     shownSeqs, shownSpeaker);
		}
	} else if (type == "transcript.retracted") {
		// Rows taken back (another language than the engine read); the
		// engine that reads it sends them again.
		const QList<qint64> seqs = seqs_of(f.value("seqs"));
		if (seqs.contains(lastSeq))
			lastSeq = -1; // its translation, if late, is not shown either
		for (qint64 seq : seqs)
			if (shownSeqs.contains(seq)) {
				clear();
				break;
			}
	} else if (type == "transcript.held") {
		// Rows from here wait until the language is known: no caption
		// meanwhile (the last one would sit over speech it is not).
		if (f.value("held").toBool())
			clear();
	} else if (type == "transcript.speakerNumbers") {
		// Every speaker's number and name, in full.
		speakers.clear();
		for (const QJsonValue v : f.value("speakers").toArray()) {
			const QJsonObject s = v.toObject();
			speakers.insert(s.value("speaker").toInt(), {s.value("number").toInt(), s.value("name").toString()});
		}
	} else if (type == "transcript.speakerNamed") {
		const int speaker = f.value("speaker").toInt();
		if (speakers.contains(speaker))
			speakers[speaker].name = f.value("name").toString();
	}
}
