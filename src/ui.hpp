#pragma once

// The dock's visual system: VocalStack's tokens (from the web app's
// ds.css) for OBS's dark and light themes, one stylesheet that styles
// widgets by their `role` property, and the few components the dock is
// built from.

#include <QComboBox>
#include <QFrame>
#include <QLabel>
#include <QStringList>
#include <QVariant>

class QAbstractButton;
class QButtonGroup;
class QToolButton;
class QVBoxLayout;

namespace ui {

struct Theme {
	bool dark = true;
	QString brand, primary, primaryHover, primaryText, muted, warn, error, live, ok;
	// The same colours as "r,g,b" for rgba() with an alpha.
	QString brandRgb, warnRgb, errorRgb, liveRgb, fgRgb;

	static Theme of(const QWidget *w);
	QString stylesheet() const;
};

// Spinner frames, a check, the gaps of the 4 px grid.
extern const QStringList SPINNER;
constexpr int GAP_LABEL = 4, GAP_FIELD = 16, GAP_SECTION = 20, MARGIN = 12;

// Restyle `w` after a property it is styled by changed.
void repolish(QWidget *w);
void setRole(QWidget *w, const char *role);

// A small upper-case section label ("BROADCAST").
QLabel *eyebrow(const QString &text, QWidget *parent = nullptr);
// Helper text, muted.
QLabel *muted(const QString &text = QString(), QWidget *parent = nullptr);
// A page title (1.15×, semibold).
QLabel *title(const QString &text, QWidget *parent = nullptr);
// A label and its control (and a note), stacked: one block of a page, to
// show or hide as one.
QWidget *field(QVBoxLayout *into, const QString &label, QWidget *control, QWidget *note = nullptr);
// A hairline across the page.
QFrame *rule(QWidget *parent = nullptr);
// A secondary action drawn as brand-coloured text ("muted": grey text).
QToolButton *linkButton(const QString &text, QWidget *parent = nullptr, const char *tone = nullptr);
// Buttons take focus from the keyboard only, so a click leaves no ring.
void tabFocusOnly(QWidget *root);
// The button says `text` for a moment (✓ Copied), then what it said.
void flash(QAbstractButton *button, const QString &text, int ms = 1500);

// Two or three choices as pills, one of them on (the dashboard's switch).
class Segmented : public QFrame {
	Q_OBJECT

public:
	explicit Segmented(QWidget *parent = nullptr);
	void addOption(const QString &label, const QVariant &value);
	QVariant value() const;
	void setValue(const QVariant &value);

signals:
	void changed(const QVariant &value);

private:
	QButtonGroup *group;
};

// A note with a coloured bar: tone "info", "warn" or "error"; ✕ when it
// can be dismissed. Rich text; links are signalled, not opened.
class Callout : public QFrame {
	Q_OBJECT

public:
	explicit Callout(QWidget *parent = nullptr);
	void setTone(const char *tone);
	void setText(const QString &html);
	void setDismissible(bool on);
	// A row under the text (an Update now button).
	void addAction(QWidget *w);

signals:
	void dismissed();
	void linkActivated(const QString &link);

private:
	QLabel *text;
	QToolButton *close;
	QWidget *actions;
};

// One line of text that elides in its middle to fit (a long link).
class ElidedLabel : public QLabel {
	Q_OBJECT

public:
	explicit ElidedLabel(QWidget *parent = nullptr);
	void setFullText(const QString &text);
	QString fullText() const { return full; }
	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

protected:
	void resizeEvent(QResizeEvent *event) override;

private:
	void refit();
	QString full;
};

// A combo box that says when its list is about to open (to reload it).
class ReloadingCombo : public QComboBox {
	Q_OBJECT

public:
	using QComboBox::QComboBox;
	void showPopup() override;

signals:
	void aboutToShowPopup();
};

} // namespace ui
