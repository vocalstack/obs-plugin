#pragma once

#include <QPointer>
#include <QWidget>

// Captions drawn over OBS's preview and nowhere else. The preview is a
// native surface (Metal/D3D/GL), so a child widget would sit under it; this
// is a frameless, see-through tool window of the main window that follows
// the preview's place on screen. Nothing here touches a scene or the
// rendered frame, so the stream and recordings never carry the captions.
class CaptionOverlay : public QWidget {
	Q_OBJECT

public:
	// The overlay for the main window's preview, or null when the preview
	// widget is not found (a frontend that renamed it).
	static CaptionOverlay *attach();

	void setCaption(const QString &text);

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;
	void paintEvent(QPaintEvent *event) override;

private:
	explicit CaptionOverlay(QWidget *mainWindow, QWidget *preview);
	void follow();

	QPointer<QWidget> preview;
	QString caption;
};
