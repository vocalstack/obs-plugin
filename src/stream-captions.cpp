#include "stream-captions.hpp"

#include "caption-feeds.hpp"

#include <obs-frontend-api.h>
#include <obs.hpp>

#include <QStringList>

// CEA-608 rows: 32 characters each; two are shown. OBS keeps at most 128
// bytes of a caption's text.
static const int ROW_CHARS = 32;
static const int ROWS = 2;
static const int MAX_BYTES = 128;
static const int SEND_MS = 1000;
// A little under SEND_MS, so the captions OBS queues never pile up.
static const double SHOW_S = 0.9;

StreamCaptions::StreamCaptions(CaptionFeeds *feeds_, QObject *parent) : QObject(parent), feeds(feeds_)
{
	tick.setInterval(SEND_MS);
	connect(&tick, &QTimer::timeout, this, &StreamCaptions::flush);
	connect(feeds, &CaptionFeeds::caption, this, &StreamCaptions::onCaption);
}

StreamCaptions::~StreamCaptions()
{
	if (enabled)
		feeds->release(lang);
}

void StreamCaptions::configure(bool on, const QString &language)
{
	if (on == enabled && language == lang)
		return;
	if (enabled)
		feeds->release(lang);
	enabled = on;
	lang = language;
	pending.clear();
	lastSpeaker.clear();
	if (enabled) {
		feeds->acquire(lang);
		tick.start();
	} else {
		tick.stop();
		if (!sent.trimmed().isEmpty()) {
			pending = QStringLiteral(" ");
			flush(); // clears what viewers see
		}
	}
}

// How many rows `words` take when wrapped at ROW_CHARS on spaces, as
// libcaption wraps them.
static int rows_for(const QStringList &words)
{
	int rows = 0, col = 0;
	for (const QString &w : words) {
		const int len = int(w.size());
		if (col == 0) {
			rows++;
			col = len;
		} else if (col + 1 + len <= ROW_CHARS) {
			col += 1 + len;
		} else {
			rows++;
			col = len;
		}
		while (col > ROW_CHARS) { // a word longer than a row breaks
			rows++;
			col -= ROW_CHARS;
		}
	}
	return rows;
}

QString StreamCaptions::fit(const QString &speaker, const QString &text, bool speakerChanged)
{
	QStringList words = text.simplified().split(' ', Qt::SkipEmptyParts);
	if (words.isEmpty())
		return QString();
	const bool mark = speakerChanged && !speaker.isEmpty();
	if (mark)
		words.prepend(QStringLiteral(">>"));
	// The oldest words go first until the rest fit.
	while (words.size() > 1 && (rows_for(words) > ROWS || words.join(' ').toUtf8().size() > MAX_BYTES))
		words.removeFirst();
	QString out = words.join(' ');
	// One word too long for the limit: its end.
	while (out.toUtf8().size() > MAX_BYTES)
		out.remove(0, 1);
	return out;
}

void StreamCaptions::onCaption(const QString &language, const QString &speaker, const QString &text)
{
	if (!enabled || language != lang)
		return;
	if (text.isEmpty()) {
		pending = QStringLiteral(" "); // a blank caption takes the last one off screen
		lastSpeaker.clear();
		return;
	}
	const bool changed = !speaker.isEmpty() && !lastSpeaker.isEmpty() && speaker != lastSpeaker;
	if (!speaker.isEmpty())
		lastSpeaker = speaker;
	pending = fit(speaker, text, changed);
}

void StreamCaptions::flush()
{
	if (pending.isEmpty() || pending == sent)
		return;
	OBSOutputAutoRelease out = obs_frontend_get_streaming_output();
	if (!out || !obs_output_active(out))
		return; // kept: sent once OBS streams
	obs_output_output_caption_text2(out, pending.toUtf8().constData(), SHOW_S);
	sent = pending;
}
