#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

class Api;
class CaptionFeed;

// Captions in the languages something asks for while a transcription is
// live: the stream's own captions (CEA-608) and each caption source in a
// scene. One subscribe socket per language, shared by everything that
// wants it, opened while live and closed when nothing wants it any more.
// Every language besides the spoken one counts towards the transcription's
// limit of ten; a language VocalStack refuses is captioned as spoken.
class CaptionFeeds : public QObject {
	Q_OBJECT

public:
	explicit CaptionFeeds(Api *api, QObject *parent = nullptr);
	~CaptionFeeds() override;

	// Something wants captions in `lang` (empty: as spoken); each acquire
	// is matched by a release.
	void acquire(const QString &lang);
	void release(const QString &lang);
	// The transcription went live: every language wanted is subscribed.
	void start(const QString &transcriptionId);
	// It ended: the sockets close, the captions clear.
	void stop();

signals:
	// The words to caption in `lang` and who said them; empty words clear.
	void caption(const QString &lang, const QString &speaker, const QString &text);

private:
	Api *api;
	QString liveId;
	QHash<QString, CaptionFeed *> feeds;
	QHash<QString, int> wanted;
};
