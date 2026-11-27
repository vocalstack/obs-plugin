#pragma once

#include <QList>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QString>

class CaptionFeeds;

// What the plugin's scene sources show, kept on the Qt main thread and set
// by the dock: the chosen broadcast's link, how many follow it, the
// languages captions can be translated into, and where captions come from.
class SceneHub : public QObject {
	Q_OBJECT

public:
	static SceneHub *instance();

	void setFeeds(CaptionFeeds *feeds);
	// Null once the dock is gone (OBS closing), before the sources are.
	CaptionFeeds *feeds() const;

	// The chosen broadcast's link (empty: none chosen), and whether it
	// needs a password.
	void setBroadcast(const QString &url, bool isPrivate);
	QString broadcastUrl() const { return url; }
	bool broadcastPrivate() const { return priv; }
	// People on the link while live; -1 when not live.
	void setListeners(int count);
	int listeners() const { return listenersNow; }
	// (name, code) of every language captions can be translated into.
	void setTranslationLanguages(const QList<QPair<QString, QString>> &languages) { translation = languages; }
	QList<QPair<QString, QString>> translationLanguages() const { return translation; }

signals:
	void broadcastChanged();
	void listenersChanged();

private:
	QPointer<QObject> captionFeeds;
	QString url;
	bool priv = false;
	int listenersNow = -1;
	QList<QPair<QString, QString>> translation;
};

// "VocalStack Captions" and "VocalStack Broadcast Link", added to OBS's
// list of sources. Unlike the overlay on the preview, these are in the
// scene: what they show goes into the stream and recordings.
void register_scene_sources();
