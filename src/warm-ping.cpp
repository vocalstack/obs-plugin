#include "warm-ping.hpp"

#include "api.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <QApplication>
#include <QEvent>
#include <QJsonObject>
#include <QWidget>

// The platform keeps the engine for 2 minutes after the last canStart true.
static const qint64 USED_WITHIN_MS = 2 * 60 * 1000;
static const qint64 PING_EVERY_MS = 30 * 1000;

WarmPing::WarmPing(Api *api, QWidget *area, std::function<bool()> canStart, std::function<QString()> language,
		   QObject *parent)
	: QObject(parent),
	  api(api),
	  area(area),
	  canStart(std::move(canStart)),
	  language(std::move(language))
{
	// Clicks, keys and scrolls anywhere in the dock count as using it.
	qApp->installEventFilter(this);
	tick.setInterval(5000);
	connect(&tick, &QTimer::timeout, this, [this]() { evaluate(false); });
	tick.start();
}

bool WarmPing::eventFilter(QObject *watched, QEvent *event)
{
	switch (event->type()) {
	case QEvent::MouseButtonPress:
	case QEvent::KeyPress:
	case QEvent::Wheel:
		if (watched->isWidgetType()) {
			auto *w = static_cast<QWidget *>(watched);
			if (w == area || area->isAncestorOf(w)) {
				const bool was = active();
				lastUse.start();
				if (!was)
					evaluate(false);
			}
		}
		break;
	default:
		break;
	}
	return QObject::eventFilter(watched, event);
}

bool WarmPing::active() const
{
	return !ended && lastUse.isValid() && lastUse.elapsed() < USED_WITHIN_MS && canStart();
}

void WarmPing::refresh()
{
	evaluate(false);
}

void WarmPing::languageChanged()
{
	evaluate(true);
}

void WarmPing::end()
{
	if (sentTrue)
		send(false);
	ended = true;
	tick.stop();
	qApp->removeEventFilter(this);
}

// true when it starts, then every 30 s while it lasts (`now`: at once);
// false once when it ends.
void WarmPing::evaluate(bool now)
{
	if (ended)
		return;
	if (active()) {
		if (now || !sentTrue || !lastSent.isValid() || lastSent.elapsed() >= PING_EVERY_MS)
			send(true);
	} else if (sentTrue) {
		send(false);
	}
}

void WarmPing::send(bool on)
{
	sentTrue = on;
	lastSent.start();
	if (api->key().isEmpty())
		return;
	QJsonObject body{{"clientKind", "obs_plugin"}, {"canStart", on}};
	const QString lang = language();
	if (!lang.isEmpty())
		body.insert("spokenLanguage", lang);
	// libcurl's multi handle on Qt's event loop: nothing waits on it.
	api->postQuiet("/v1/realtime/warm", body, [on](int status, const QJsonDocument &, const QString &error) {
		if (!error.isEmpty())
			obs_log(LOG_DEBUG, "warm ping (canStart %s): %d %s", on ? "true" : "false", status,
				error.toUtf8().constData());
	});
}
