#include "subtitles.hpp"

#include "api.hpp"
#include "settings.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QSaveFile>
#include <QUrlQuery>

#include <algorithm>
#include <memory>

// The last words of a recording reach VocalStack a few seconds after it
// stops; its sentences settle a little after that.
static const int WRITE_AFTER_MS = 15000;
// Translations still on their way: asked for again this much later, twice.
static const int RETRY_AFTER_MS = 20000;
static const int MAX_TRIES = 3;

// ---------------------------------------------------------------- cues

QList<Cue> transcript_cues(const QJsonObject &snapshot, const QString &lang)
{
	QHash<int, QString> names;
	const QJsonArray speakers = snapshot.value("speakers").toArray();
	const bool label = speakers.size() > 1 && snapshot.value("speakerLabels").toString() != QLatin1String("off");
	for (const QJsonValue v : speakers) {
		const QJsonObject s = v.toObject();
		const QString name = s.value("name").toString();
		names.insert(s.value("speaker").toInt(),
			     name.isEmpty() ? QObject::tr("Speaker %1").arg(s.value("number").toInt()) : name);
	}
	QList<Cue> cues;
	for (const QJsonValue v : snapshot.value("sentences").toArray()) {
		const QJsonObject s = v.toObject();
		if (s.value("status").toString() == QLatin1String("dropped") || s.value("suppressed").toBool())
			continue;
		QString text = s.value("text").toString();
		if (!lang.isEmpty()) {
			const QString t = s.value("translations").toObject().value(lang).toString();
			if (!t.isEmpty())
				text = t;
		}
		text = text.simplified();
		if (text.isEmpty())
			continue;
		const double start = s.value("start").toDouble(), end = s.value("end").toDouble(start);
		Cue c;
		c.start = s.value("wallStart").isDouble() ? s.value("wallStart").toDouble() : start;
		c.end = c.start + qMax(0.5, end - start);
		const int speaker = s.value("speaker").toInt(-1);
		c.text = label && names.contains(speaker) ? names.value(speaker) + QStringLiteral(": ") + text : text;
		cues.append(c);
	}
	std::sort(cues.begin(), cues.end(), [](const Cue &a, const Cue &b) { return a.start < b.start; });
	// A cue ends before the next begins.
	for (int i = 0; i + 1 < cues.size(); i++)
		cues[i].end = qMin(cues[i].end, cues[i + 1].start);
	return cues;
}

static QString stamp(double s, QChar sep)
{
	const qint64 ms = qMax<qint64>(0, qRound64(s * 1000));
	return QStringLiteral("%1:%2:%3%4%5")
		.arg(ms / 3600000, 2, 10, QChar('0'))
		.arg((ms / 60000) % 60, 2, 10, QChar('0'))
		.arg((ms / 1000) % 60, 2, 10, QChar('0'))
		.arg(sep)
		.arg(ms % 1000, 3, 10, QChar('0'));
}

QString srt_text(const QList<Cue> &cues)
{
	QString out;
	int n = 0;
	for (const Cue &c : cues)
		out += QStringLiteral("%1\n%2 --> %3\n%4\n\n")
			       .arg(++n)
			       .arg(stamp(c.start, ','), stamp(c.end, ','), c.text);
	return out;
}

QString vtt_text(const QList<Cue> &cues)
{
	QString out = QStringLiteral("WEBVTT\n\n");
	for (const Cue &c : cues)
		out += QStringLiteral("%1 --> %2\n%3\n\n").arg(stamp(c.start, '.'), stamp(c.end, '.'), c.text);
	return out;
}

// ---------------------------------------------------------------- YouTube

static QString chapter_time(double s)
{
	const qint64 t = qMax<qint64>(0, qint64(s));
	if (t >= 3600)
		return QStringLiteral("%1:%2:%3")
			.arg(t / 3600)
			.arg((t / 60) % 60, 2, 10, QChar('0'))
			.arg(t % 60, 2, 10, QChar('0'));
	return QStringLiteral("%1:%2").arg(t / 60).arg(t % 60, 2, 10, QChar('0'));
}

// The first few words of a line, without a speaker's name ahead.
static QString chapter_name(const QString &line)
{
	QString text = line;
	const int colon = int(text.indexOf(QStringLiteral(": ")));
	if (colon > 0 && colon < 40)
		text = text.mid(colon + 2);
	QStringList words = text.split(' ', Qt::SkipEmptyParts);
	const bool cut = words.size() > 7;
	words = words.mid(0, 7);
	QString name = words.join(' ');
	while (!name.isEmpty() && QStringLiteral(".,;:!?…-").contains(name.back()))
		name.chop(1);
	return cut ? name + QStringLiteral("…") : name;
}

QString youtube_text(const QString &title, const QString &summary, const QList<Cue> &cues)
{
	QStringList parts;
	if (!title.isEmpty())
		parts << title;
	if (!summary.isEmpty())
		parts << summary;
	if (cues.size() >= 3) {
		// Times count from the transcription's start, as a recording of the
		// stream it started with does.
		const double zero = 0;
		const double length = cues.last().end - zero;
		// About one chapter every three minutes, three to fifteen of them.
		const int count = qBound(3, int(length / 180.0 + 0.5), 15);
		const double step = length / count;
		if (step >= 20) {
			QList<int> starts{0}; // the first line, shown at 0:00
			for (int k = 1; k < count; k++) {
				const double target = zero + k * step;
				// The line nearest the target that follows the longest pause.
				int best = -1;
				double bestScore = -1;
				for (int i = 1; i < cues.size(); i++) {
					const double at = cues[i].start;
					if (at < target - step / 3 || at > target + step / 3)
						continue;
					if (at - cues[starts.last()].start < 10)
						continue;
					const double gap = at - cues[i - 1].end;
					const double score = gap - qAbs(at - target) / step;
					if (score > bestScore) {
						bestScore = score;
						best = i;
					}
				}
				if (best > 0)
					starts << best;
			}
			if (starts.size() >= 3) {
				QStringList lines;
				for (int i : starts)
					lines << QStringLiteral("%1 %2").arg(i == 0 ? chapter_time(0) : chapter_time(cues[i].start - zero),
									  chapter_name(cues[i].text));
				parts << QObject::tr("Chapters") + QStringLiteral("\n") + lines.join('\n');
			}
		}
	}
	return parts.join(QStringLiteral("\n\n"));
}

// ---------------------------------------------------------------- files

RecordingSubtitles::RecordingSubtitles(Api *api_, Settings *settings_, QObject *parent)
	: QObject(parent),
	  api(api_),
	  settings(settings_)
{
}

void RecordingSubtitles::transcriptionLive(const QString &id, qint64 sinceMs)
{
	live.append({id, sinceMs, 0});
}

void RecordingSubtitles::transcriptionEnded(const QString &id)
{
	for (Live &l : live)
		if (l.id == id && !l.toMs)
			l.toMs = QDateTime::currentMSecsSinceEpoch();
	// Kept while a recording may still need them; older ones go.
	if (!recording)
		live.erase(std::remove_if(live.begin(), live.end(), [](const Live &l) { return l.toMs != 0; }),
			   live.end());
}

void RecordingSubtitles::recordingStarted()
{
	recording = true;
	recStartMs = QDateTime::currentMSecsSinceEpoch();
	pausedAt = 0;
	pauses.clear();
	live.erase(std::remove_if(live.begin(), live.end(), [](const Live &l) { return l.toMs != 0; }), live.end());
}

void RecordingSubtitles::recordingPaused(bool paused)
{
	if (!recording)
		return;
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	if (paused && !pausedAt)
		pausedAt = now;
	else if (!paused && pausedAt) {
		pauses.append({pausedAt, now});
		pausedAt = 0;
	}
}

void RecordingSubtitles::recordingStopped()
{
	if (!recording)
		return;
	recording = false;
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	if (pausedAt)
		pauses.append({pausedAt, now});
	pausedAt = 0;
	Job job;
	job.startMs = recStartMs;
	job.endMs = now;
	job.pauses = pauses;
	job.format = settings->subtitles;
	job.lang = settings->subtitleLanguage;
	for (const Live &l : live)
		if (l.fromMs < now && (!l.toMs || l.toMs > recStartMs))
			job.transcriptions.append(l);
	if (job.format.isEmpty() || job.format == QLatin1String("off") || job.transcriptions.isEmpty())
		return;
	char *path = obs_frontend_get_last_recording();
	job.path = QString::fromUtf8(path ? path : "");
	bfree(path);
	if (job.path.isEmpty())
		return;
	QTimer::singleShot(WRITE_AFTER_MS, this, [this, job]() { write(job); });
}

double RecordingSubtitles::recordingTime(const Job &job, qint64 ms)
{
	if (ms < job.startMs || ms > job.endMs)
		return -1;
	qint64 paused = 0;
	for (const auto &[from, to] : job.pauses) {
		if (ms >= from && ms < to)
			return -1;
		if (ms >= to)
			paused += to - from;
	}
	return (ms - job.startMs - paused) / 1000.0;
}

void RecordingSubtitles::write(Job job)
{
	job.tries++;
	// Each transcription read as spoken, and translated when asked for.
	struct Pending {
		int left = 0;
		bool pendingTranslation = false;
		QString error;
		QList<Cue> spoken, translated;
	};
	auto state = std::make_shared<Pending>();
	const QStringList langs = job.lang.isEmpty() ? QStringList{QString()} : QStringList{QString(), job.lang};
	state->left = int(job.transcriptions.size() * langs.size());

	auto finish = [this, job, state]() {
		if (!state->error.isEmpty()) {
			emit failed(tr("Couldn't save subtitles for the recording: %1").arg(state->error));
			return;
		}
		if (state->pendingTranslation && job.tries < MAX_TRIES) {
			QTimer::singleShot(RETRY_AFTER_MS, this, [this, job]() { write(job); });
			return;
		}
		auto sorted = [](QList<Cue> cues) {
			std::sort(cues.begin(), cues.end(), [](const Cue &a, const Cue &b) { return a.start < b.start; });
			return cues;
		};
		const QFileInfo rec(job.path);
		const QString base = rec.dir().filePath(rec.completeBaseName());
		QStringList written;
		QString error;
		auto save = [&](const QString &file, const QString &text) {
			QSaveFile f(file);
			if (f.open(QIODevice::WriteOnly) && f.write(text.toUtf8()) >= 0 && f.commit())
				written << file;
			else
				error = f.errorString();
		};
		const QList<QPair<QString, QList<Cue>>> sets{{QString(), sorted(state->spoken)},
							     {job.lang, sorted(state->translated)}};
		for (const auto &[lang, cues] : sets) {
			if (cues.isEmpty())
				continue;
			const QString stem = lang.isEmpty() ? base : base + '.' + lang;
			save(stem + QStringLiteral(".srt"), srt_text(cues));
			if (job.format == QLatin1String("srt+vtt"))
				save(stem + QStringLiteral(".vtt"), vtt_text(cues));
		}
		if (!error.isEmpty())
			emit failed(tr("Couldn't save subtitles for the recording: %1").arg(error));
		else if (!written.isEmpty()) {
			for (const QString &w : written)
				obs_log(LOG_INFO, "subtitles saved: %s", w.toUtf8().constData());
			emit saved(written);
		}
	};

	for (const Live &t : job.transcriptions)
		for (const QString &lang : langs) {
			QString path = "/v1/transcriptions/" + t.id + "/transcript";
			if (!lang.isEmpty()) {
				QUrlQuery q;
				q.addQueryItem("lang", lang);
				path += '?' + q.toString(QUrl::FullyEncoded);
			}
			api->get(path, [job, t, lang, state, finish](int, const QJsonDocument &doc, const QString &error) {
				if (!error.isEmpty())
					state->error = error;
				else {
					const QJsonObject snap = doc.object();
					if (!lang.isEmpty() && snap.value("translationPending").toBool())
						state->pendingTranslation = true;
					// Seconds since the transcription went live, to the
					// recording's own time.
					for (Cue c : transcript_cues(snap, lang)) {
						const double start = recordingTime(job, t.fromMs + qint64(c.start * 1000));
						if (start < 0)
							continue;
						const double end = recordingTime(job, t.fromMs + qint64(c.end * 1000));
						c.end = end < 0 ? start + (c.end - c.start) : end;
						c.start = start;
						(lang.isEmpty() ? state->spoken : state->translated).append(c);
					}
				}
				if (--state->left == 0)
					finish();
			});
		}
}
