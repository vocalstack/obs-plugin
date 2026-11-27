#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

class CaptionFeeds;

// Closed captions in OBS's own stream: the words VocalStack hears, sent as
// CEA-608 caption text with the stream's video (obs_output_output_caption_text2),
// so Twitch's and YouTube's players show them behind their CC button and
// YouTube keeps them with the recording of the stream. Off by default: the
// stream carries nothing of VocalStack's unless this is turned on.
//
// CEA-608 is two short rows of plain Latin text: the newest words that fit
// are sent, at most once a second (OBS shows each caption it is given for
// its duration before the next, so faster would fall behind).
class StreamCaptions : public QObject {
	Q_OBJECT

public:
	StreamCaptions(CaptionFeeds *feeds, QObject *parent = nullptr);
	~StreamCaptions() override;

	// On or off, and in which language (empty: as spoken).
	void configure(bool enabled, const QString &lang);

	// The caption text as CEA-608 can carry it: the newest words that fit
	// in two rows of 32, at most 128 bytes, ">> " where the speaker changes.
	static QString fit(const QString &speaker, const QString &text, bool speakerChanged);

private:
	void onCaption(const QString &lang, const QString &speaker, const QString &text);
	void flush();

	CaptionFeeds *feeds;
	bool enabled = false;
	QString lang;
	QTimer tick;
	// What should be on screen, and what was last sent.
	QString pending, sent;
	QString lastSpeaker;
};
