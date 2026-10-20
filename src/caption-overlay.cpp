#include "caption-overlay.hpp"

#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QEvent>
#include <QMainWindow>
#include <QPainter>
#include <QPainterPath>

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

void CaptionOverlay::setCaption(const QString &text)
{
	caption = text;
	update();
}

// Wrapped to most of the preview's width, centred near its bottom, on a
// dark rounded box: readable over any picture.
void CaptionOverlay::paintEvent(QPaintEvent *)
{
	if (caption.isEmpty())
		return;
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	QFont font = p.font();
	font.setPixelSize(qBound(14, height() / 22, 32));
	p.setFont(font);

	const int pad = font.pixelSize() / 2;
	const int maxWidth = width() * 8 / 10;
	const QRect text = p.fontMetrics().boundingRect(QRect(0, 0, maxWidth, height()),
							 Qt::AlignHCenter | Qt::TextWordWrap, caption);
	QRect box(0, 0, text.width() + 2 * pad, text.height() + pad);
	box.moveCenter(QPoint(width() / 2, 0));
	box.moveBottom(height() - height() / 14);

	QPainterPath path;
	path.addRoundedRect(box, 6, 6);
	p.fillPath(path, QColor(0, 0, 0, 180));
	p.setPen(Qt::white);
	p.drawText(box, Qt::AlignCenter | Qt::TextWordWrap, caption);
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
