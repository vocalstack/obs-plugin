#include "transcript-panel.hpp"

#include "ui.hpp"

#include <QDockWidget>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollBar>
#include <QSettings>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

// Only the newest lines are drawn; the whole transcript is in VocalStack.
static const int SHOWN_LINES = 400;
static const int FONT_MIN = 10, FONT_MAX = 48, FONT_DEFAULT = 18;
static const char *FONT_KEY = "VocalStack/transcriptFontPt";

TranscriptPanel::TranscriptPanel(QWidget *parent) : QWidget(parent)
{
	auto *v = new QVBoxLayout(this);
	v->setContentsMargins(ui::MARGIN, 8, ui::MARGIN, ui::MARGIN);
	v->setSpacing(8);

	auto *bar = new QHBoxLayout;
	bar->setSpacing(4);
	bar->addWidget(ui::eyebrow(tr("Live transcript")));
	bar->addStretch(1);
	auto *smaller = ui::linkButton(QStringLiteral("A−"), this, "muted");
	smaller->setToolTip(tr("Smaller text"));
	connect(smaller, &QToolButton::clicked, this, [this]() { setSize(fontPt - 2); });
	auto *bigger = ui::linkButton(QStringLiteral("A+"), this, "muted");
	bigger->setToolTip(tr("Larger text"));
	connect(bigger, &QToolButton::clicked, this, [this]() { setSize(fontPt + 2); });
	floatButton = ui::linkButton(tr("Pop out ↗"), this, "muted");
	floatButton->setToolTip(tr("Float the transcript in its own window, to move it to another monitor"));
	connect(floatButton, &QToolButton::clicked, this, [this]() {
		if (auto *dw = qobject_cast<QDockWidget *>(parentWidget()))
			dw->setFloating(!dw->isFloating());
	});
	bar->addWidget(smaller);
	bar->addWidget(bigger);
	bar->addWidget(floatButton);
	v->addLayout(bar);

	view = new QTextBrowser;
	view->setFrameShape(QFrame::NoFrame);
	view->setOpenLinks(false);
	view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	view->viewport()->setAutoFillBackground(false);
	view->setStyleSheet("QTextBrowser { background: transparent; }");
	v->addWidget(view, 1);
	empty = ui::muted(tr("The transcript shows here while you transcribe."));
	empty->setAlignment(Qt::AlignCenter);
	v->addWidget(empty, 1);
	view->hide();

	fontPt = QSettings("VocalStack", "OBS").value(FONT_KEY, FONT_DEFAULT).toInt();
	setSize(fontPt);
	dark = ui::Theme::of(this).dark;
	setStyleSheet(ui::Theme::of(this).stylesheet());
}

void TranscriptPanel::popOut()
{
	auto *dw = qobject_cast<QDockWidget *>(parentWidget());
	if (!dw)
		return;
	dw->show();
	dw->setFloating(true);
	if (dw->width() < 420 || dw->height() < 300)
		dw->resize(560, 420);
	dw->raise();
	dw->activateWindow();
}

void TranscriptPanel::changeEvent(QEvent *event)
{
	QWidget::changeEvent(event);
	if (event->type() == QEvent::PaletteChange && ui::Theme::of(this).dark != dark) {
		dark = ui::Theme::of(this).dark;
		setStyleSheet(ui::Theme::of(this).stylesheet());
		render();
	}
	if (event->type() == QEvent::ParentChange)
		if (auto *dw = qobject_cast<QDockWidget *>(parentWidget()))
			connect(dw, &QDockWidget::topLevelChanged, this, [this](bool floating) {
				floatButton->setText(floating ? tr("Dock ↙") : tr("Pop out ↗"));
			});
}

void TranscriptPanel::setSize(int points)
{
	fontPt = qBound(FONT_MIN, points, FONT_MAX);
	QSettings("VocalStack", "OBS").setValue(FONT_KEY, fontPt);
	QFont f = view->font();
	f.setPointSize(fontPt);
	view->setFont(f);
	render();
}

void TranscriptPanel::setLine(const QString &id, double start, int speaker, const QString &text)
{
	for (Line &l : lines)
		if (l.id == id) {
			if (!text.isEmpty())
				l.text = text;
			if (speaker >= 0)
				l.speaker = speaker;
			if (start >= 0 && start != l.start) {
				l.start = start;
				std::stable_sort(lines.begin(), lines.end(),
						 [](const Line &a, const Line &b) { return a.start < b.start; });
			}
			render();
			return;
		}
	if (text.trimmed().isEmpty())
		return;
	// In order of when it was said: a sentence goes where its first row was.
	Line line{id, start < 0 ? (lines.isEmpty() ? 0 : lines.last().start) : start, speaker, text};
	auto at = std::upper_bound(lines.begin(), lines.end(), line.start,
				   [](double s, const Line &l) { return s < l.start; });
	lines.insert(at, line);
	while (lines.size() > SHOWN_LINES * 2)
		lines.removeFirst();
	render();
}

void TranscriptPanel::removeLines(const QStringList &ids)
{
	const qsizetype before = lines.size();
	lines.removeIf([&ids](const Line &l) { return ids.contains(l.id); });
	if (lines.size() != before)
		render();
}

void TranscriptPanel::clear()
{
	lines.clear();
	render();
}

void TranscriptPanel::setSpeakerLabels(const QHash<int, QString> &l)
{
	labels = l;
	render();
}

// The lines as rich text: a speaker's name where the speaker changes, in
// brand blue; the view stays at the newest line unless it was scrolled up.
void TranscriptPanel::render()
{
	if (renderQueued)
		return;
	renderQueued = true;
	QTimer::singleShot(0, this, [this]() {
		renderQueued = false;
		QScrollBar *bar = view->verticalScrollBar();
		const bool atEnd = bar->value() >= bar->maximum() - 4;
		const QString brand = ui::Theme::of(this).brand;
		QString html;
		int lastSpeaker = -2;
		const qsizetype from = qMax<qsizetype>(0, lines.size() - SHOWN_LINES);
		for (qsizetype i = from; i < lines.size(); i++) {
			const Line &l = lines.at(i);
			const QString who = labels.value(l.speaker);
			QString p;
			if (!who.isEmpty() && l.speaker != lastSpeaker)
				p = QStringLiteral("<div style='color:%1; font-weight:600; font-size:%2pt; margin-top:10px;'>%3</div>")
					    .arg(brand)
					    .arg(qMax(FONT_MIN, fontPt * 3 / 4))
					    .arg(who.toHtmlEscaped());
			p += QStringLiteral("<p style='margin:0 0 6px 0;'>%1</p>").arg(l.text.toHtmlEscaped());
			html += p;
			lastSpeaker = l.speaker;
		}
		view->setHtml(html);
		view->setVisible(!lines.isEmpty());
		empty->setVisible(lines.isEmpty());
		// After the new text is laid out.
		if (atEnd)
			QTimer::singleShot(0, this, [bar]() { bar->setValue(bar->maximum()); });
	});
}
