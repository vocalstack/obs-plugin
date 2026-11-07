#include "caption-overlay.hpp"

#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QEvent>
#include <QMainWindow>
#include <QPainter>
#include <QPainterPath>
#include <QTextDocument>
#include <QTextOption>

CaptionOverlay *CaptionOverlay::attach()
{
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!main)
		return nullptr;
	auto *preview = main->findChild<QWidget *>("preview");
	if (!preview) {
		obs_log(LOG_WARNING, "caption overlay: no preview widget");
		return nullptr;
	}
	obs_log(LOG_INFO, "caption overlay attached to the preview");
	return new CaptionOverlay(main, preview);
}

CaptionOverlay::CaptionOverlay(QWidget *mainWindow, QWidget *preview_)
	: QWidget(mainWindow, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowTransparentForInput |
				      Qt::WindowDoesNotAcceptFocus | Qt::NoDropShadowWindowHint),
	  preview(preview_)
{
	setAttribute(Qt::WA_TranslucentBackground);
	setAttribute(Qt::WA_ShowWithoutActivating);
	setAttribute(Qt::WA_TransparentForMouseEvents);

	// The preview moves when the main window moves, and resizes when a dock
	// or the window does; watch both, and every ancestor in between.
	for (QWidget *w = preview; w; w = w->parentWidget())
		w->installEventFilter(this);

	follow();
}

void CaptionOverlay::setCaption(const QString &who, const QString &text)
{
	speaker = text.isEmpty() ? QString() : who;
	caption = text;
	update();
}

// The caption as rich text: the speaker in brand blue, then the words.
static QString caption_html(const QString &speaker, const QString &words, bool cut)
{
	QString html;
	if (!speaker.isEmpty())
		html = QStringLiteral("<span style='color:#55abe5; font-weight:600;'>%1:</span> ").arg(speaker.toHtmlEscaped());
	return html + (cut ? QStringLiteral("…") : QString()) + words.toHtmlEscaped();
}

// At most two lines, the newest words kept, centred near the bottom of the
// preview on VocalStack navy: readable over any picture without covering
// the scene.
void CaptionOverlay::paintEvent(QPaintEvent *)
{
	if (caption.isEmpty())
		return;
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	QFont font = p.font();
	font.setPixelSize(qBound(14, height() / 22, 32));
	const int pad = font.pixelSize() / 2;
	const int maxWidth = width() * 8 / 10;
	const QFontMetrics fm(font);

	QTextDocument doc;
	doc.setDefaultFont(font);
	doc.setDocumentMargin(0);
	doc.setDefaultStyleSheet("body { color: white; }");
	QTextOption opt(Qt::AlignHCenter);
	opt.setWrapMode(QTextOption::WordWrap);
	doc.setDefaultTextOption(opt);
	doc.setTextWidth(maxWidth);
	// Older words go first until two lines hold the rest.
	QStringList words = caption.split(' ', Qt::SkipEmptyParts);
	bool cut = false;
	doc.setHtml(caption_html(speaker, words.join(' '), false));
	while (doc.size().height() > 2.2 * fm.lineSpacing() && words.size() > 1) {
		words.removeFirst();
		cut = true;
		doc.setHtml(caption_html(speaker, words.join(' '), cut));
	}
	doc.setTextWidth(qMin<qreal>(maxWidth, doc.idealWidth() + 1));
	const QSizeF size = doc.size();

	QRectF box(0, 0, size.width() + 2 * pad, size.height() + pad);
	box.moveCenter(QPointF(width() / 2.0, 0));
	box.moveBottom(height() - height() / 14.0);
	QPainterPath path;
	const qreal radius = pad * 0.6;
	path.addRoundedRect(box, radius, radius);
	p.fillPath(path, QColor(10, 12, 41, 190));
	p.translate(box.left() + pad, box.top() + pad / 2.0);
	doc.drawContents(&p);
}

bool CaptionOverlay::eventFilter(QObject *, QEvent *event)
{
	switch (event->type()) {
	case QEvent::Move:
	case QEvent::Resize:
	case QEvent::Show:
	case QEvent::Hide:
	case QEvent::WindowStateChange:
		follow();
		break;
	default:
		break;
	}
	return false;
}

void CaptionOverlay::follow()
{
	if (!preview || !preview->isVisible() || preview->window()->isMinimized()) {
		hide();
		return;
	}
	const QRect at(preview->mapToGlobal(QPoint(0, 0)), preview->size());
	if (at != geometry())
		obs_log(LOG_DEBUG, "caption overlay at %d,%d %dx%d", at.x(), at.y(), at.width(), at.height());
	setGeometry(at);
	show();
	raise();
}
