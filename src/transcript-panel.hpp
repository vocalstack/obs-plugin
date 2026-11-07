#pragma once

#include <QHash>
#include <QList>
#include <QWidget>

class QLabel;
class QTextBrowser;
class QToolButton;

// The running transcript in large type, in an OBS dock of its own
// ("VocalStack Transcript"): it can be floated and moved to another
// monitor, and OBS remembers where it was. Lines come from the session:
// rows as they are heard, then the sentence they belong to in their place.
class TranscriptPanel : public QWidget {
	Q_OBJECT

public:
	explicit TranscriptPanel(QWidget *parent = nullptr);

	// Shown, floated off OBS's window and raised: the live card's Pop out.
	void popOut();

public slots:
	void setLine(const QString &id, double start, int speaker, const QString &text);
	void removeLines(const QStringList &ids);
	void clear();
	void setSpeakerLabels(const QHash<int, QString> &labels);

protected:
	void changeEvent(QEvent *event) override;

private:
	struct Line {
		QString id;
		double start = 0;
		int speaker = -1;
		QString text;
	};

	void render();
	void setSize(int points);

	QList<Line> lines;
	QHash<int, QString> labels;
	QTextBrowser *view;
	QLabel *empty;
	QToolButton *floatButton;
	int fontPt;
	bool renderQueued = false;
	// The theme the stylesheet was made for: setting a stylesheet changes
	// the palette, so only a real switch between dark and light restyles.
	bool dark = true;
};
