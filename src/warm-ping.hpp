#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>

class Api;
class QWidget;

// "Instant start": while someone is at the dock and could press Start, the
// plugin tells VocalStack (POST /v1/realtime/warm, canStart true), which
// keeps an engine loaded so that Start is instant. It starts and bills
// nothing. canStart goes true while the dock can start (signed in, idle, on
// screen) and was used in the last 2 minutes, at most every 30 s, and at
// once when the spoken language changes; false when that ends (the dock is
// hidden, transcription starts, sign-out, OBS closes). Errors are ignored:
// an older API answers 404.
class WarmPing : public QObject {
	Q_OBJECT

public:
	// `canStart`: the dock could start now. `language`: the spoken
	// language, empty for detect.
	WarmPing(Api *api, QWidget *area, std::function<bool()> canStart, std::function<QString()> language,
		 QObject *parent = nullptr);

	// Something changed `canStart`.
	void refresh();
	// The spoken language changed: say so now, not at the next ping.
	void languageChanged();
	// OBS is closing: canStart false, if it was true, and no more pings.
	void end();

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	bool active() const;
	void evaluate(bool now);
	void send(bool canStart);

	Api *api;
	QWidget *area;
	std::function<bool()> canStart;
	std::function<QString()> language;
	QTimer tick;
	QElapsedTimer lastUse, lastSent;
	bool sentTrue = false, ended = false;
};
