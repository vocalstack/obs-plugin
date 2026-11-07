#include "ui.hpp"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QPointer>
#include <QPushButton>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace ui {

const QStringList SPINNER{"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};

Theme Theme::of(const QWidget *w)
{
	Theme t;
	t.dark = w->palette().window().color().lightness() < 128;
	if (t.dark) {
		t.brand = "#55abe5";
		t.brandRgb = "85,171,229";
		t.muted = "#848eae";
		t.warn = "#f0b429";
		t.warnRgb = "240,180,41";
		t.error = "#e8695e";
		t.errorRgb = "232,105,94";
		t.live = "#f78472";
		t.liveRgb = "247,132,114";
		t.ok = "#34d399";
		t.fgRgb = "255,255,255";
	} else {
		t.brand = "#2968a3";
		t.brandRgb = "41,104,163";
		t.muted = "#65729a";
		t.warn = "#8a5a00";
		t.warnRgb = "138,90,0";
		t.error = "#b92913";
		t.errorRgb = "185,41,19";
		t.live = "#b92913";
		t.liveRgb = "185,41,19";
		t.ok = "#047857";
		t.fgRgb = "0,0,0";
	}
	t.primary = "#2968a3";
	t.primaryHover = "#3382cc";
	t.primaryText = "#eaecfa";
	return t;
}

QString Theme::stylesheet() const
{
	// Alphas are 0–255: 10 ≈ 4 %, 18 ≈ 7 %, 26 ≈ 10 %, 31 ≈ 12 %, 56 ≈ 22 %.
	QString s = R"(
QLabel[role="eyebrow"] { color: MUTED; }
QLabel[role="muted"] { color: MUTED; }
QLabel[level="warn"] { color: WARN; }
QLabel[level="error"] { color: ERROR; }
QLabel[role="dot"][level="live"] { color: LIVE; }
QLabel[role="dot"][level="warn"] { color: WARN; }
QLabel[role="dot"][level="error"] { color: ERROR; }
QLabel[role="dot"][level="dim"] { color: rgba(LIVERGB,100); }

QPushButton[role="primary"] {
	background: PRIMARY; color: PRIMARYTEXT; border: none; border-radius: 8px;
	font-weight: 600; min-height: 40px; padding: 0 14px;
}
QPushButton[role="primary"]:hover { background: PRIMARYHOVER; }
QPushButton[role="primary"]:disabled { background: rgba(FGRGB,18); color: MUTED; }
QPushButton[role="danger"] {
	background: rgba(LIVERGB,56); color: LIVE; border: 1px solid rgba(LIVERGB,140);
	border-radius: 8px; font-weight: 600; min-height: 40px; padding: 0 14px;
}
QPushButton[role="danger"]:hover { background: rgba(LIVERGB,82); }
QPushButton[role="danger"]:disabled { color: MUTED; border-color: rgba(FGRGB,26); background: rgba(FGRGB,10); }
QPushButton[role="secondary"] { min-height: 32px; padding: 0 12px; }

QToolButton[role="link"] { color: BRAND; background: transparent; border: none; padding: 2px 4px; }
QToolButton[role="link"]:hover { color: PRIMARYHOVER; }
QToolButton[role="link"]:disabled { color: MUTED; }
QToolButton[role="icon"] { min-width: 32px; min-height: 32px; border-radius: 6px; }
QToolButton[role="close"] { color: MUTED; background: transparent; border: none; padding: 0 2px; }
QToolButton[role="close"]:hover { color: BRAND; }

QFrame[role="card"] { background: rgba(FGRGB,10); border: 1px solid rgba(FGRGB,18); border-radius: 10px; }
QFrame[role="callout"] { border: none; border-radius: 6px; border-left: 3px solid BRAND; background: rgba(BRANDRGB,31); }
QFrame[role="callout"][tone="warn"] { border-left-color: WARN; background: rgba(WARNRGB,31); }
QFrame[role="callout"][tone="error"] { border-left-color: ERROR; background: rgba(ERRORRGB,31); }

QFrame[role="segmented"] { background: rgba(FGRGB,13); border: 1px solid rgba(FGRGB,26); border-radius: 15px; }
QFrame[role="segmented"] QPushButton {
	border: none; border-radius: 13px; padding: 5px 12px; background: transparent; min-height: 20px;
}
QFrame[role="segmented"] QPushButton:hover { background: rgba(FGRGB,13); }
QFrame[role="segmented"] QPushButton:checked { background: BRAND; color: CHECKEDTEXT; font-weight: 600; }
QFrame[role="segmented"] QPushButton:disabled { color: MUTED; }
QFrame[role="segmented"] QPushButton:checked:disabled { background: rgba(BRANDRGB,82); }

QLineEdit[invalid="true"], QComboBox[invalid="true"] { border: 1px solid ERROR; border-radius: 4px; }
QScrollArea[role="page"], QScrollArea[role="page"] > QWidget > QWidget { background: transparent; }
)";
	s.replace("PRIMARYHOVER", primaryHover)
		.replace("PRIMARYTEXT", primaryText)
		.replace("PRIMARY", primary)
		.replace("CHECKEDTEXT", dark ? "#131326" : "#ffffff")
		.replace("BRANDRGB", brandRgb)
		.replace("BRAND", brand)
		.replace("MUTED", muted)
		.replace("WARNRGB", warnRgb)
		.replace("WARN", warn)
		.replace("ERRORRGB", errorRgb)
		.replace("ERROR", error)
		.replace("LIVERGB", liveRgb)
		.replace("LIVE", live)
		.replace("FGRGB", fgRgb);
	return s;
}

void repolish(QWidget *w)
{
	w->style()->unpolish(w);
	w->style()->polish(w);
	w->update();
}

void setRole(QWidget *w, const char *role)
{
	w->setProperty("role", role);
	repolish(w);
}

QLabel *eyebrow(const QString &text, QWidget *parent)
{
	auto *l = new QLabel(text, parent);
	l->setProperty("role", "eyebrow");
	QFont f = l->font();
	f.setPointSizeF(f.pointSizeF() * 0.82);
	f.setCapitalization(QFont::AllUppercase);
	f.setLetterSpacing(QFont::AbsoluteSpacing, 1.2);
	f.setWeight(QFont::DemiBold);
	l->setFont(f);
	return l;
}

QLabel *muted(const QString &text, QWidget *parent)
{
	auto *l = new QLabel(text, parent);
	l->setProperty("role", "muted");
	l->setWordWrap(true);
	return l;
}

QLabel *title(const QString &text, QWidget *parent)
{
	auto *l = new QLabel(text, parent);
	QFont f = l->font();
	f.setPointSizeF(f.pointSizeF() * 1.15);
	f.setWeight(QFont::DemiBold);
	l->setFont(f);
	l->setWordWrap(true);
	return l;
}

void field(QVBoxLayout *into, const QString &label, QWidget *control, QWidget *note)
{
	auto *block = new QVBoxLayout;
	block->setSpacing(GAP_LABEL);
	block->addWidget(eyebrow(label));
	block->addWidget(control);
	if (note)
		block->addWidget(note);
	into->addLayout(block);
}

QToolButton *linkButton(const QString &text, QWidget *parent)
{
	auto *b = new QToolButton(parent);
	b->setText(text);
	b->setAutoRaise(true);
	b->setCursor(Qt::PointingHandCursor);
	b->setProperty("role", "link");
	return b;
}

void flash(QAbstractButton *button, const QString &text, int ms)
{
	if (button->property("flashing").toBool())
		return;
	const QString was = button->text();
	button->setProperty("flashing", true);
	button->setText(text);
	QPointer<QAbstractButton> b(button);
	QTimer::singleShot(ms, button, [b, was]() {
		if (!b)
			return;
		b->setText(was);
		b->setProperty("flashing", false);
	});
}

Segmented::Segmented(QWidget *parent) : QFrame(parent), group(new QButtonGroup(this))
{
	setProperty("role", "segmented");
	group->setExclusive(true);
	auto *h = new QHBoxLayout(this);
	h->setContentsMargins(2, 2, 2, 2);
	h->setSpacing(0);
	connect(group, &QButtonGroup::buttonClicked, this,
		[this](QAbstractButton *b) { emit changed(b->property("value")); });
	setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
}

void Segmented::addOption(const QString &label, const QVariant &value)
{
	auto *b = new QPushButton(label, this);
	b->setCheckable(true);
	b->setCursor(Qt::PointingHandCursor);
	b->setProperty("value", value);
	group->addButton(b);
	layout()->addWidget(b);
	if (group->buttons().size() == 1)
		b->setChecked(true);
}

QVariant Segmented::value() const
{
	return group->checkedButton() ? group->checkedButton()->property("value") : QVariant();
}

void Segmented::setValue(const QVariant &value)
{
	for (QAbstractButton *b : group->buttons())
		if (b->property("value") == value) {
			b->setChecked(true);
			return;
		}
}

Callout::Callout(QWidget *parent) : QFrame(parent)
{
	setProperty("role", "callout");
	setProperty("tone", "info");
	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(10, 8, 6, 8);
	outer->setSpacing(6);
	auto *row = new QHBoxLayout;
	row->setSpacing(6);
	text = new QLabel(this);
	text->setWordWrap(true);
	text->setTextFormat(Qt::RichText);
	text->setTextInteractionFlags(Qt::TextBrowserInteraction);
	connect(text, &QLabel::linkActivated, this, &Callout::linkActivated);
	close = new QToolButton(this);
	close->setText(QStringLiteral("✕"));
	close->setToolTip(tr("Dismiss"));
	close->setAutoRaise(true);
	close->setProperty("role", "close");
	connect(close, &QToolButton::clicked, this, &Callout::dismissed);
	row->addWidget(text, 1);
	row->addWidget(close, 0, Qt::AlignTop);
	outer->addLayout(row);
	actions = new QWidget(this);
	auto *ah = new QHBoxLayout(actions);
	ah->setContentsMargins(0, 0, 0, 0);
	ah->addStretch(1);
	actions->hide();
	outer->addWidget(actions);
}

void Callout::setTone(const char *tone)
{
	if (property("tone").toString() == QLatin1String(tone))
		return;
	setProperty("tone", tone);
	repolish(this);
}

void Callout::setText(const QString &html)
{
	text->setText(html);
}

void Callout::setDismissible(bool on)
{
	close->setVisible(on);
}

void Callout::addAction(QWidget *w)
{
	auto *h = static_cast<QHBoxLayout *>(actions->layout());
	h->insertWidget(h->count() - 1, w);
	actions->show();
}

ElidedLabel::ElidedLabel(QWidget *parent) : QLabel(parent)
{
	setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	setTextInteractionFlags(Qt::TextSelectableByMouse);
}

void ElidedLabel::setFullText(const QString &text)
{
	full = text;
	setToolTip(text);
	refit();
}

QSize ElidedLabel::minimumSizeHint() const
{
	return QSize(20, QLabel::minimumSizeHint().height());
}

void ElidedLabel::resizeEvent(QResizeEvent *event)
{
	QLabel::resizeEvent(event);
	refit();
}

void ElidedLabel::refit()
{
	QLabel::setText(fontMetrics().elidedText(full, Qt::ElideMiddle, qMax(0, width())));
}

void ReloadingCombo::showPopup()
{
	emit aboutToShowPopup();
	QComboBox::showPopup();
}

} // namespace ui
