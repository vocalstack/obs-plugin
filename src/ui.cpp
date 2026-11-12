#include "ui.hpp"

#include <QLocale>

#include <QKeyEvent>

#include <QEnterEvent>

#include <QAbstractButton>
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
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
	// One "selected" blue for Start and the chosen pill: white on it is
	// about 6:1.
	t.primary = "#2968a3";
	t.primaryHover = "#2f75b5";
	t.primaryText = "#ffffff";
	return t;
}

QString Theme::stylesheet() const
{
	// Alphas are 0–255: 10 ≈ 4 %, 18 ≈ 7 %, 26 ≈ 10 %, 31 ≈ 12 %, 56 ≈ 22 %.
	QString s = R"(
QLabel[role="eyebrow"] { color: rgba(FGRGB,150); }
QLabel[role="eyebrow"][level="live"] { color: LIVE; }
QLabel[role="eyebrow"][level="warn"] { color: WARN; }
QLabel[role="eyebrow"][level="error"] { color: ERROR; }
QLabel[role="muted"] { color: MUTED; }
QLabel[level="warn"] { color: WARN; }
QLabel[level="error"] { color: ERROR; }
QLabel[role="dot"][level="live"] { color: LIVE; }
QLabel[role="dot"][level="warn"] { color: WARN; }
QLabel[role="dot"][level="error"] { color: ERROR; }
QLabel[role="dot"][level="dim"] { color: rgba(LIVERGB,110); }

QPushButton[role="primary"] {
	background: PRIMARY; color: PRIMARYTEXT; border: 2px solid transparent; border-radius: 8px;
	font-weight: 600; min-height: 36px; padding: 0 14px;
}
QPushButton[role="primary"]:hover { background: PRIMARYHOVER; }
QPushButton[role="primary"]:pressed { background: #1f5280; }
QPushButton[role="primary"]:focus { border-color: rgba(BRANDRGB,210); }
QPushButton[role="primary"]:disabled { background: rgba(FGRGB,18); color: MUTED; }
QPushButton[role="danger"] {
	background: rgba(LIVERGB,56); color: LIVE; border: 2px solid rgba(LIVERGB,140);
	border-radius: 8px; font-weight: 600; min-height: 36px; padding: 0 14px;
}
QPushButton[role="danger"]:hover { background: rgba(LIVERGB,82); }
QPushButton[role="danger"]:pressed { background: rgba(LIVERGB,110); }
QPushButton[role="danger"]:focus { border-color: LIVE; }
QPushButton[role="danger"]:disabled { color: MUTED; border-color: rgba(FGRGB,26); background: rgba(FGRGB,10); }
QPushButton[role="danger"][busy="true"]:disabled { background: rgba(LIVERGB,40); color: LIVE; border-color: rgba(LIVERGB,100); }
QPushButton[role="secondary"] { min-height: 28px; max-height: 28px; border-radius: 6px; padding: 0 12px; }
QPushButton[role="edit"] { border-radius: 6px; padding: 0 12px; }

QToolButton[role="link"] {
	color: BRAND; background: transparent; border: 1px solid transparent; border-radius: 4px;
	padding: 2px 4px; min-height: 22px;
}
QToolButton[role="link"]:hover { color: PRIMARYHOVERTEXT; background: rgba(FGRGB,13); }
QToolButton[role="link"]:pressed { background: rgba(FGRGB,20); }
QToolButton[role="link"]:focus { border-color: rgba(BRANDRGB,160); }
QToolButton[role="link"]:disabled { color: MUTED; }
QToolButton[role="link"][tone="muted"] { color: MUTED; }
QToolButton[role="link"][tone="muted"]:hover { color: BRAND; }
QToolButton[role="close"] { color: MUTED; background: transparent; border: none; border-radius: 4px; }
QToolButton[role="close"]:hover { color: palette(text); background: rgba(FGRGB,20); }
QToolButton[role="toggle"] {
	color: MUTED; background: transparent; border: 1px solid rgba(FGRGB,40); border-radius: 4px;
	padding: 1px 6px; font-weight: 600;
}
QToolButton[role="toggle"]:checked { background: PRIMARY; border-color: PRIMARY; color: #ffffff; }
QToolButton[role="toggle"]:focus { border-color: rgba(BRANDRGB,210); }

QFrame[role="scriptrow"] { background: rgba(FGRGB,8); border: 2px solid rgba(FGRGB,18); border-radius: 10px; }
QFrame[role="scriptrow"]:hover { background: rgba(FGRGB,14); }
QFrame[role="scriptrow"][selected="true"] { border-color: BRAND; background: rgba(BRANDRGB,22); }
QFrame[role="scriptrow"]:focus { border-color: rgba(BRANDRGB,210); }
QLabel[role="badge"] { border: 2px solid BRAND; border-radius: 8px; color: palette(text); background: transparent; }
QLabel[role="scripttag"] { color: BRAND; }
QFrame[role="card"] { background: rgba(FGRGB,10); border: 1px solid rgba(FGRGB,18); border-radius: 10px; }
QFrame[role="rule"] { background: rgba(FGRGB,18); border: none; min-height: 1px; max-height: 1px; }
QFrame[role="callout"] { border: none; border-radius: 6px; border-left: 3px solid BRAND; background: rgba(BRANDRGB,31); }
QFrame[role="callout"][tone="warn"] { border-left-color: WARN; background: rgba(WARNRGB,31); }
QFrame[role="callout"][tone="error"] { border-left-color: ERROR; background: rgba(ERRORRGB,31); }

QFrame[role="segmented"] { background: rgba(FGRGB,13); border: 1px solid rgba(FGRGB,26); border-radius: 18px; }
QFrame[role="segmented"] QPushButton {
	border: 2px solid transparent; border-radius: 14px; padding: 0 12px; background: transparent;
}
QFrame[role="segmented"] QPushButton:hover { background: rgba(FGRGB,13); }
QFrame[role="segmented"] QPushButton:pressed { background: rgba(FGRGB,20); }
QFrame[role="segmented"] QPushButton:focus { border-color: rgba(BRANDRGB,210); }
QFrame[role="segmented"] QPushButton:checked { background: SEGON; color: SEGONTEXT; font-weight: 600; }
QFrame[role="segmented"] QPushButton:disabled { color: MUTED; }
QFrame[role="segmented"] QPushButton:checked:disabled { background: rgba(BRANDRGB,70); color: rgba(255,255,255,200); }

QLineEdit[invalid="true"], QComboBox[invalid="true"] { border: 1px solid ERROR; border-radius: 4px; }
QScrollArea[role="page"], QScrollArea[role="page"] > QWidget > QWidget { background: transparent; }
)";
	s.replace("SEGONTEXT", dark ? "#13142b" : "#ffffff");
	s.replace("SEGON", dark ? "#6aaee6" : primary);
	s.replace("PRIMARYHOVERTEXT", dark ? "#85b4e0" : "#1f4e7a")
		.replace("PRIMARYHOVER", primaryHover)
		.replace("PRIMARYTEXT", primaryText)
		.replace("PRIMARY", primary)
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
	f.setPointSizeF(f.pointSizeF() * 0.78);
	f.setCapitalization(QFont::AllUppercase);
	f.setLetterSpacing(QFont::AbsoluteSpacing, 1.5);
	f.setWeight(QFont::Medium);
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

QWidget *field(QVBoxLayout *into, const QString &label, QWidget *control, QWidget *note)
{
	auto *block = new QWidget;
	auto *v = new QVBoxLayout(block);
	v->setContentsMargins(0, 0, 0, 0);
	v->setSpacing(GAP_LABEL);
	v->addWidget(eyebrow(label));
	v->addWidget(control);
	if (note)
		v->addWidget(note);
	into->addWidget(block);
	return block;
}

QFrame *rule(QWidget *parent)
{
	auto *f = new QFrame(parent);
	f->setProperty("role", "rule");
	f->setFixedHeight(1);
	return f;
}

QToolButton *linkButton(const QString &text, QWidget *parent, const char *tone)
{
	auto *b = new QToolButton(parent);
	b->setText(text);
	b->setAutoRaise(true);
	b->setCursor(Qt::PointingHandCursor);
	b->setProperty("role", "link");
	if (tone)
		b->setProperty("tone", tone);
	return b;
}

void tabFocusOnly(QWidget *root)
{
	for (QAbstractButton *b : root->findChildren<QAbstractButton *>())
		b->setFocusPolicy(Qt::TabFocus);
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
	h->setContentsMargins(3, 3, 3, 3);
	h->setSpacing(0);
	connect(group, &QButtonGroup::buttonClicked, this,
		[this](QAbstractButton *b) { emit changed(b->property("value")); });
	setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
}

void Segmented::addOption(const QString &label, const QVariant &value, const QString &tip)
{
	auto *b = new QPushButton(label, this);
	b->setToolTip(tip);
	b->setCheckable(true);
	b->setCursor(Qt::PointingHandCursor);
	b->setProperty("value", value);
	b->setFixedHeight(28);
	// As wide as its bold (selected) label, so it never clips.
	QFont bold = b->font();
	bold.setWeight(QFont::DemiBold);
	b->setMinimumWidth(QFontMetrics(bold).horizontalAdvance(label) + 2 * 12 + 8);
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
	outer->setContentsMargins(10, 6, 4, 8);
	outer->setSpacing(6);
	auto *row = new QHBoxLayout;
	row->setSpacing(4);
	text = new QLabel(this);
	text->setWordWrap(true);
	text->setTextFormat(Qt::RichText);
	text->setTextInteractionFlags(Qt::TextBrowserInteraction);
	text->setContentsMargins(0, 2, 0, 0);
	connect(text, &QLabel::linkActivated, this, &Callout::linkActivated);
	close = new QToolButton(this);
	close->setText(QStringLiteral("✕"));
	close->setToolTip(tr("Dismiss"));
	close->setAutoRaise(true);
	close->setFixedSize(24, 24);
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
	setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
	setTextInteractionFlags(Qt::TextSelectableByMouse);
}

void ElidedLabel::setFullText(const QString &text)
{
	full = text;
	setToolTip(text);
	updateGeometry();
	refit();
}

QSize ElidedLabel::sizeHint() const
{
	return QSize(fontMetrics().horizontalAdvance(full) + 4, QLabel::minimumSizeHint().height());
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
	QLabel::setText(fontMetrics().elidedText(full, mode, qMax(0, width())));
}

Switch::Switch(QWidget *parent) : QAbstractButton(parent)
{
	setCheckable(true);
	setCursor(Qt::PointingHandCursor);
	setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

QSize Switch::sizeHint() const
{
	return QSize(40, 22);
}

void Switch::paintEvent(QPaintEvent *)
{
	const Theme t = Theme::of(this);
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	const QRectF track = QRectF(rect()).adjusted(1, 1, -1, -1);
	const qreal r = track.height() / 2;
	QColor on(t.primary), off = palette().text().color();
	off.setAlpha(t.dark ? 60 : 50);
	if (!isEnabled()) {
		on.setAlpha(110);
		off.setAlpha(30);
	}
	p.setPen(Qt::NoPen);
	p.setBrush(isChecked() ? on : off);
	p.drawRoundedRect(track, r, r);
	if (hasFocus()) {
		QColor ring(t.brand);
		ring.setAlpha(210);
		p.setPen(QPen(ring, 2));
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(track.adjusted(-0.5, -0.5, 0.5, 0.5), r, r);
		p.setPen(Qt::NoPen);
	}
	const qreal d = track.height() - 4;
	const qreal x = isChecked() ? track.right() - 2 - d : track.left() + 2;
	p.setBrush(isEnabled() ? QColor(Qt::white) : QColor(255, 255, 255, 150));
	p.drawEllipse(QRectF(x, track.top() + 2, d, d));
}

ScriptRow::ScriptRow(const QDateTime &when, const QString &preview, const QString &detail, QWidget *parent)
	: QFrame(parent)
{
	setProperty("role", "scriptrow");
	setProperty("selected", false);
	setCursor(Qt::PointingHandCursor);
	setFocusPolicy(Qt::TabFocus);
	auto *h = new QHBoxLayout(this);
	h->setContentsMargins(10, 8, 12, 8);
	h->setSpacing(12);

	auto *badge = new QLabel;
	badge->setProperty("role", "badge");
	badge->setAlignment(Qt::AlignCenter);
	badge->setFixedSize(44, 46);
	const Theme t = Theme::of(this);
	if (preview.isEmpty())
		badge->setText(QStringLiteral("<span style='font-size:15pt;'>∅</span>"));
	else if (when.isValid())
		badge->setText(QStringLiteral("<div style='font-size:8pt; font-weight:600; letter-spacing:1px; color:%1;'>%2</div>"
					      "<div style='font-size:14pt; font-weight:600;'>%3</div>")
				       .arg(t.brand, QLocale::c().toString(when, "MMM").toUpper(), when.toString("dd")));
	else
		badge->setText(QStringLiteral("<span style='font-size:14pt;'>—</span>"));
	h->addWidget(badge);

	auto *v = new QVBoxLayout;
	v->setSpacing(2);
	auto *text = new ElidedLabel;
	QFont f = text->font();
	f.setItalic(!preview.isEmpty());
	f.setPointSizeF(f.pointSizeF() * 1.05);
	text->setFont(f);
	text->setTextInteractionFlags(Qt::NoTextInteraction);
	text->setElideMode(Qt::ElideRight);
	text->setFullText(preview.isEmpty() ? tr("No script") : preview);
	text->setToolTip(QString());
	text->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	v->addWidget(text);
	auto *tag = eyebrow(detail);
	if (!preview.isEmpty())
		tag->setProperty("role", "scripttag");
	v->addWidget(tag);
	h->addLayout(v, 1);
}

void ScriptRow::setSelected(bool on)
{
	setProperty("selected", on);
	repolish(this);
}

void ScriptRow::mousePressEvent(QMouseEvent *event)
{
	QFrame::mousePressEvent(event);
	emit clicked();
}

void ScriptRow::keyPressEvent(QKeyEvent *event)
{
	if (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
		emit clicked();
		return;
	}
	QFrame::keyPressEvent(event);
}

Card::Card(QWidget *parent) : QFrame(parent)
{
	setProperty("role", "card");
	rows = new QVBoxLayout(this);
	rows->setContentsMargins(14, 4, 14, 4);
	rows->setSpacing(0);
}

void Card::addRow(QWidget *row)
{
	auto *h = new QHBoxLayout;
	h->setContentsMargins(0, 0, 0, 0);
	h->addWidget(row);
	addLayoutRow(h);
}

void Card::addLayoutRow(QLayout *row)
{
	if (count++ > 0)
		rows->addWidget(rule());
	auto *pad = new QVBoxLayout;
	pad->setContentsMargins(0, 10, 0, 10);
	pad->addLayout(row);
	rows->addLayout(pad);
}

WorkspaceMark::WorkspaceMark(int side_, QWidget *parent) : QWidget(parent), side(side_)
{
	setFixedSize(side, side);
}

void WorkspaceMark::setWorkspace(const Workspace &w, const QImage &i)
{
	workspace = w;
	icon = i;
	update();
}

void WorkspaceMark::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	const QRectF box(0, 0, side, side);
	const qreal radius = qRound(side * 0.29);
	QPainterPath tile;
	tile.addRoundedRect(box, radius, radius);
	QColor ink = palette().text().color();
	auto inked = [ink](int alpha) {
		QColor c = ink;
		c.setAlpha(alpha);
		return c;
	};
	auto hairline = [&]() {
		p.setClipping(false);
		p.setPen(QPen(inked(26), 1));
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(box.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
	};
	p.setPen(Qt::NoPen);
	if (!icon.isNull()) {
		// The icon fills the square, cropped about its centre.
		const int crop = qMin(icon.width(), icon.height());
		const QRectF from((icon.width() - crop) / 2.0, (icon.height() - crop) / 2.0, crop, crop);
		p.setClipPath(tile);
		p.fillRect(box, inked(15));
		p.drawImage(box, icon, from);
		hairline();
		return;
	}
	if (workspace.personal) {
		p.fillPath(tile, inked(23));
		// A person, as the dashboard's icon: a head over shoulders, on a
		// 24-unit grid, drawn at 55 % of the tile.
		const qreal unit = side * 0.55 / 24.0;
		QPainterPath person;
		person.moveTo(20, 21);
		person.lineTo(20, 19);
		person.arcTo(QRectF(12, 15, 8, 8), 0, 90);
		person.lineTo(8, 15);
		person.arcTo(QRectF(4, 15, 8, 8), 90, 90);
		person.lineTo(4, 21);
		person.addEllipse(QPointF(12, 7), 4, 4);
		p.save();
		p.translate((side - 24 * unit) / 2.0, (side - 24 * unit) / 2.0);
		p.scale(unit, unit);
		p.setPen(QPen(inked(178), qMax(1.6, 1.1 / unit), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		p.setBrush(Qt::NoBrush);
		p.drawPath(person);
		p.restore();
		hairline();
		return;
	}
	// Initials in the workspace's colour on the same colour at 22 %; the
	// colour is deeper on a light theme, to stay readable.
	const bool dark = palette().window().color().lightness() < 128;
	const QColor colour = QColor::fromHsvF(float(workspace.hue() / 360.0), dark ? 0.5f : 0.62f, dark ? 0.86f : 0.5f);
	QColor wash = colour;
	wash.setAlphaF(0.22f);
	p.fillPath(tile, wash);
	QFont f = font();
	f.setPixelSize(qMax(7, qRound(side * 0.39)));
	f.setWeight(QFont::DemiBold);
	p.setFont(f);
	p.setPen(colour);
	p.drawText(box, Qt::AlignCenter, workspace.initials());
}

QWidget *settingRow(const QString &name, const QString &note, QWidget *control)
{
	auto *w = new QWidget;
	auto *h = new QHBoxLayout(w);
	h->setContentsMargins(0, 0, 0, 0);
	h->setSpacing(12);
	auto *texts = new QVBoxLayout;
	texts->setSpacing(2);
	auto *title = new QLabel(name);
	title->setWordWrap(true);
	texts->addWidget(title);
	if (!note.isEmpty())
		texts->addWidget(muted(note));
	h->addLayout(texts, 1);
	if (control)
		h->addWidget(control, 0, Qt::AlignVCenter);
	return w;
}

void ReloadingCombo::showPopup()
{
	emit aboutToShowPopup();
	QComboBox::showPopup();
}

LanguageChip::LanguageChip(QWidget *parent) : ReloadingCombo(parent)
{
	setCursor(Qt::PointingHandCursor);
	setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	setAttribute(Qt::WA_Hover);
	connect(this, &QComboBox::currentIndexChanged, this, [this]() { updateGeometry(); update(); });
}

void LanguageChip::setFlags(const QHash<QString, QString> &f)
{
	flags = f;
	updateGeometry();
	update();
}

QString LanguageChip::flagText() const
{
	const QString code = currentData().toString();
	const QString flag = flags.value(code);
	return code.isEmpty() || flag.isEmpty() ? QStringLiteral("🌐") : flag;
}

QString LanguageChip::codeText() const
{
	return currentData().toString().toUpper();
}

static const int CHIP_H = 36, CHIP_PAD = 14, CHIP_GAP = 8, CHEVRON_W = 10;
static const QString TRANSLATE_MARK = QStringLiteral("文A");

QSize LanguageChip::sizeHint() const
{
	const QFontMetrics fm(font());
	QFont big = font();
	big.setPointSizeF(big.pointSizeF() * 1.25);
	const QFontMetrics bm(big);
	int w = CHIP_PAD + fm.horizontalAdvance(TRANSLATE_MARK) + CHIP_GAP + bm.horizontalAdvance(flagText());
	if (!codeText().isEmpty()) {
		QFont bold = font();
		bold.setWeight(QFont::DemiBold);
		w += 6 + QFontMetrics(bold).horizontalAdvance(codeText());
	}
	w += CHIP_GAP + CHEVRON_W + CHIP_PAD;
	return QSize(w, CHIP_H);
}

void LanguageChip::enterEvent(QEnterEvent *event)
{
	hovered = true;
	update();
	ReloadingCombo::enterEvent(event);
}

void LanguageChip::leaveEvent(QEvent *event)
{
	hovered = false;
	update();
	ReloadingCombo::leaveEvent(event);
}

void LanguageChip::paintEvent(QPaintEvent *)
{
	const Theme t = Theme::of(this);
	const QStringList rgb = t.fgRgb.split(',');
	const QColor fg(rgb.value(0).toInt(), rgb.value(1).toInt(), rgb.value(2).toInt());
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	QColor bg = fg;
	bg.setAlpha(hovered ? 26 : 13);
	QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
	p.setPen(hasFocus() ? QPen(QColor(t.brand), 2) : QPen(Qt::NoPen));
	p.setBrush(bg);
	p.drawRoundedRect(r, r.height() / 2, r.height() / 2);

	QColor ink = fg;
	ink.setAlpha(isEnabled() ? 170 : 90);
	int x = CHIP_PAD;
	const QFontMetrics fm(font());
	p.setFont(font());
	p.setPen(ink);
	const int markW = fm.horizontalAdvance(TRANSLATE_MARK);
	p.drawText(QRect(x, 0, markW, height()), Qt::AlignVCenter, TRANSLATE_MARK);
	x += markW + CHIP_GAP;

	QFont big = font();
	big.setPointSizeF(big.pointSizeF() * 1.25);
	p.setFont(big);
	const int flagW = QFontMetrics(big).horizontalAdvance(flagText());
	p.drawText(QRect(x, 0, flagW, height()), Qt::AlignVCenter, flagText());
	x += flagW;

	if (!codeText().isEmpty()) {
		QFont bold = font();
		bold.setWeight(QFont::DemiBold);
		p.setFont(bold);
		QColor strong = fg;
		strong.setAlpha(isEnabled() ? 230 : 110);
		p.setPen(strong);
		const int codeW = QFontMetrics(bold).horizontalAdvance(codeText());
		x += 6;
		p.drawText(QRect(x, 0, codeW, height()), Qt::AlignVCenter, codeText());
		x += codeW;
	}
	x += CHIP_GAP;

	QPen chevron(ink, 1.6);
	chevron.setCapStyle(Qt::RoundCap);
	chevron.setJoinStyle(Qt::RoundJoin);
	p.setPen(chevron);
	p.setBrush(Qt::NoBrush);
	const qreal cy = height() / 2.0;
	const QPointF pts[3] = {{x + 0.0, cy - 2.5}, {x + CHEVRON_W / 2.0, cy + 2.5}, {x + qreal(CHEVRON_W), cy - 2.5}};
	p.drawPolyline(pts, 3);
}

} // namespace ui
