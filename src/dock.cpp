#include "dock.hpp"

#include "api.hpp"
#include "caption-overlay.hpp"
#include "connect-login.hpp"
#include "transcript-panel.hpp"
#include "ui.hpp"
#include "updater.hpp"
#include "warm-ping.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDockWidget>
#include <QEvent>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHostInfo>
#include <QJsonArray>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QShortcut>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

#include <cstring>
#include <algorithm>
#include <initializer_list>

using ui::GAP_FIELD;
using ui::GAP_LABEL;
using ui::GAP_SECTION;

// Where an update is read about and downloaded by hand: the plugin's page
// on the VocalStack website.
static const char *PLUGIN_PAGE = "https://vocalstack.com/obs-studio-transcription-plugin/";
// Items of the broadcast list that are not broadcasts.
static const char *NEW_BROADCAST = "__new__";
static const char *LOADING = "__loading__";
// The list is asked for again when it opens, at most this often.
static const qint64 BROADCASTS_FRESH_MS = 10000;
// Below this width the two transcript buttons stack.
static const int NARROW = 340;

// The VocalStack logo, at 2x for sharp screens: white on OBS's dark themes,
// the dark one on a light theme.
static QPixmap logo_pixmap(bool dark)
{
	char *path = obs_module_file(dark ? "logo-white@2x.png" : "logo@2x.png");
	QPixmap pm(QString::fromUtf8(path ? path : ""));
	bfree(path);
	if (!pm.isNull())
		pm.setDevicePixelRatio(2);
	return pm;
}

// A link inside rich text, in the theme's brand colour (set again when the
// theme changes).
static QString anchor(const QWidget *w, const QString &href, const QString &text)
{
	return QStringLiteral("<a href=\"%1\" style=\"color:%2; text-decoration:none;\">%3</a>")
		.arg(href.toHtmlEscaped(), ui::Theme::of(w).brand, text.toHtmlEscaped());
}

static void set_level(QLabel *label, const char *level)
{
	if (label->property("level").toString() == QLatin1String(level))
		return;
	label->setProperty("level", level);
	ui::repolish(label);
}

static const char *level_name(Session::Level level)
{
	return level == Session::Level::Error ? "error" : level == Session::Level::Warn ? "warn" : "";
}

// A page that scrolls when the dock is short.
static QScrollArea *scrolling(QWidget *content)
{
	auto *area = new QScrollArea;
	area->setWidget(content);
	area->setWidgetResizable(true);
	area->setFrameShape(QFrame::NoFrame);
	area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	area->setProperty("role", "page");
	return area;
}

// The computer's name, without macOS's .local.
static QString host_name()
{
	QString host = QHostInfo::localHostName();
	if (host.endsWith(".local"))
		host.chop(6);
	return host;
}

// "42 min", "1 h 05 min", "40 s".
static QString duration_text(qint64 secs)
{
	if (secs < 60)
		return QObject::tr("%1 s").arg(secs);
	if (secs < 3600)
		return QObject::tr("%1 min").arg(secs / 60);
	return QObject::tr("%1 h %2 min").arg(secs / 3600).arg((secs / 60) % 60, 2, 10, QChar('0'));
}

Dock::Dock(QWidget *parent)
	: QWidget(parent),
	  api(new Api(this)),
	  settings(Settings::load()),
	  session(new Session(api, &settings, this)),
	  login(new ConnectLogin(api, this)),
	  updater(new Updater(api, this)),
	  pages(new QStackedWidget(this))
{
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(ui::MARGIN, ui::MARGIN, ui::MARGIN, ui::MARGIN);
	layout->setSpacing(0);
	layout->addSpacing(8);
	auto *head = new QHBoxLayout;
	logoLabel = new QLabel;
	logoLabel->setToolTip(QStringLiteral("VocalStack"));
	head->addWidget(logoLabel, 0, Qt::AlignVCenter);
	head->addStretch(1);
	// Navigation, not an action: quiet, so the eye goes to the form.
	auto *dashboard = ui::linkButton(tr("Dashboard ↗"), nullptr, "muted");
	dashboard->setToolTip(tr("Open the VocalStack dashboard"));
	// While transcribing, straight to that transcription's live page.
	connect(dashboard, &QToolButton::clicked, this, [this]() {
		const bool live = session->state() != Session::State::Idle && !settings.transcriptionId.isEmpty();
		QDesktopServices::openUrl(QUrl(api->appBase() + (live ? "/transcriptions/" + settings.transcriptionId + "/live" : QString())));
	});
	head->addWidget(dashboard, 0, Qt::AlignVCenter);
	layout->addLayout(head);
	layout->addSpacing(GAP_SECTION);
	layout->addWidget(pages, 1);

	// Captions show always, in the language spoken.
	settings.captionLanguage.clear();
	settings.showCaptions = true;
	pages->addWidget(buildSignedOut());
	pages->addWidget(buildSettings());
	pages->addWidget(buildSignedIn());
	pages->addWidget(buildBroadcastEditor());
	applyTheme();
	ui::tabFocusOnly(this);

	connect(updater, &Updater::changed, this, &Dock::showUpdate);
	updater->setMayInstall([this]() { return session->state() == Session::State::Idle; });
	connect(login, &ConnectLogin::finished, this,
		[this](const QString &key, const QString &account, const QString &error) {
			setSigningIn(false);
			if (!key.isEmpty()) {
				signedIn(key, account);
				// Back from the browser: OBS comes to the front.
				window()->raise();
				window()->activateWindow();
			} else {
				signInStatus->setText(error);
				set_level(signInStatus, error.isEmpty() ? "" : "error");
			}
		});
	connect(api, &Api::unauthorized, this,
		[this]() { signOut(tr("VocalStack no longer accepts this sign-in. Sign in again.")); });
	connect(session, &Session::stateChanged, this, &Dock::onState);
	connect(session, &Session::status, this, &Dock::onStatus);
	connect(session, &Session::notices, this, &Dock::showNotices);
	connect(session, &Session::spokenLanguageChanged, this, [this](const QString &lang) {
		spokenBox->setCurrentIndex(qMax(0, spokenBox->findData(lang)));
		updateLiveSummary();
	});
	connect(session, &Session::listeners, this, [this](int n) {
		liveListeners->setVisible(n >= 0);
		liveListeners->setText(tr("%n listening", nullptr, qMax(0, n)));
	});
	connect(session, &Session::caption, this, [this](const QString &speaker, const QString &text) {
		if (!qEnvironmentVariableIsEmpty("VOCALSTACK_DEBUG"))
			obs_log(LOG_INFO, "caption: %s %s", speaker.toUtf8().constData(), text.toUtf8().constData());
		if (overlay && settings.showCaptions)
			overlay->setCaption(speaker, text);
	});

	// Instant start: while the dock is on screen, signed in, idle and in
	// use, VocalStack keeps an engine loaded for the next Start.
	warm = new WarmPing(
		api, this,
		[this]() {
			return !settings.apiKey.isEmpty() && session->state() == Session::State::Idle && isVisible() &&
			       !window()->isMinimized();
		},
		[this]() { return settings.spokenLanguage; }, this);

	api->setKey(settings.apiKey);
	showSignedIn(!settings.apiKey.isEmpty());
	onState(session->state());
}

// One stylesheet for the dock, from the theme OBS is in; again, with the
// logo and the links, when OBS's theme changes between dark and light.
void Dock::applyTheme()
{
	const ui::Theme theme = ui::Theme::of(this);
	darkTheme = theme.dark;
	setStyleSheet(theme.stylesheet());
	logoLabel->setPixmap(logo_pixmap(theme.dark));
	followHint->setText(tr("Starts when OBS starts streaming or recording · %1")
				    .arg(anchor(this, "settings", tr("Change"))));
	joinLabel->setText(tr("No account? %1").arg(anchor(this, "join", tr("Create one free"))));
	if (edLinkNote->text().contains("<a "))
		edLinkNote->clear();
	showUpdate();
}

void Dock::changeEvent(QEvent *event)
{
	QWidget::changeEvent(event);
	if (event->type() == QEvent::PaletteChange && ui::Theme::of(this).dark != darkTheme)
		applyTheme();
}

void Dock::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	transcriptButtons->setDirection(width() < NARROW ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
}

void Dock::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	if (warm)
		warm->refresh();
}

void Dock::hideEvent(QHideEvent *event)
{
	QWidget::hideEvent(event);
	if (warm)
		warm->refresh();
}

QWidget *Dock::pageHeader(QLabel **titleLabel, QLabel **subtitleLabel)
{
	auto *w = new QWidget;
	auto *v = new QVBoxLayout(w);
	v->setContentsMargins(0, 0, 0, 0);
	v->setSpacing(6);
	auto *back = ui::linkButton(tr("‹  Back"));
	back->setToolTip(tr("Back (Esc)"));
	QFont bf = back->font();
	bf.setWeight(QFont::DemiBold);
	back->setFont(bf);
	connect(back, &QToolButton::clicked, this, &Dock::goBack);
	v->addWidget(back, 0, Qt::AlignLeft);
	*titleLabel = ui::title(QString());
	v->addWidget(*titleLabel);
	if (subtitleLabel) {
		*subtitleLabel = ui::muted();
		v->addWidget(*subtitleLabel);
	}
	return w;
}

void Dock::goBack()
{
	if (pages->currentIndex() == Editor && editDirty &&
	    QMessageBox::question(this, tr("Discard changes?"), tr("Discard your changes to this broadcast?"),
				  QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Discard)
		return;
	editDirty = false;
	pages->setCurrentIndex(Main);
}

// ---------------------------------------------------------------- signed out

QWidget *Dock::buildSignedOut()
{
	auto *content = new QWidget;
	auto *v = new QVBoxLayout(content);
	v->setContentsMargins(0, 0, 4, 0);
	v->setSpacing(0);

	v->addWidget(ui::title(tr("Live captions for your stream")));
	v->addSpacing(GAP_LABEL);
	v->addWidget(ui::muted(tr("Your audience reads live captions and translations at a link you share. You also "
				  "see them over the OBS preview; they never go into your stream.")));
	v->addSpacing(GAP_SECTION);

	signInButton = new QPushButton(tr("Sign in with VocalStack"));
	signInButton->setProperty("role", "primary");
	connect(signInButton, &QPushButton::clicked, this, [this]() {
		setSigningIn(true);
		login->start();
	});
	v->addWidget(signInButton);
	v->addSpacing(8);

	auto *waiting = new QHBoxLayout;
	signInStatus = new QLabel;
	signInStatus->setWordWrap(true);
	cancelSignIn = ui::linkButton(tr("Cancel"));
	cancelSignIn->hide();
	connect(cancelSignIn, &QToolButton::clicked, this, [this]() {
		login->cancel();
		setSigningIn(false);
		signInStatus->clear();
	});
	waiting->addWidget(signInStatus, 1);
	waiting->addWidget(cancelSignIn, 0, Qt::AlignTop);
	v->addLayout(waiting);
	signInSpin = new QTimer(this);
	signInSpin->setInterval(90);
	connect(signInSpin, &QTimer::timeout, this, [this]() {
		signInFrame++;
		signInStatus->setText(QStringLiteral("%1  %2").arg(ui::SPINNER.at(signInFrame % ui::SPINNER.size()),
								  tr("Continue in your browser…")));
	});
	v->addSpacing(GAP_FIELD);

	// The API key, for those who have one, behind a link.
	keyToggle = ui::linkButton(tr("Use an API key instead ▸"));
	connect(keyToggle, &QToolButton::clicked, this, [this]() {
		const bool show = !keyRow->isVisible();
		keyRow->setVisible(show);
		keyToggle->setText(show ? tr("Use an API key instead ▾") : tr("Use an API key instead ▸"));
		if (show)
			keyEdit->setFocus();
	});
	v->addWidget(keyToggle, 0, Qt::AlignLeft);
	keyRow = new QWidget;
	auto *kv = new QVBoxLayout(keyRow);
	kv->setContentsMargins(0, GAP_LABEL, 0, 0);
	kv->setSpacing(GAP_LABEL);
	auto *kh = new QHBoxLayout;
	keyEdit = new QLineEdit;
	keyEdit->setEchoMode(QLineEdit::Password);
	keyEdit->setPlaceholderText("vs_key_…");
	auto *useButton = new QPushButton(tr("Use key"));
	useButton->setProperty("role", "secondary");
	connect(useButton, &QPushButton::clicked, this, &Dock::useKey);
	connect(keyEdit, &QLineEdit::returnPressed, this, &Dock::useKey);
	kh->addWidget(keyEdit, 1);
	kh->addWidget(useButton);
	kv->addLayout(kh);
	kv->addWidget(ui::muted(tr("From Developers → API keys in the dashboard.")));
	keyRow->hide();
	v->addWidget(keyRow);
	v->addSpacing(GAP_SECTION);

	joinLabel = ui::muted();
	joinLabel->setTextFormat(Qt::RichText);
	connect(joinLabel, &QLabel::linkActivated, this, [this]() { QDesktopServices::openUrl(QUrl(api->appBase())); });
	v->addWidget(joinLabel);
	v->addStretch(1);
	return scrolling(content);
}

void Dock::setSigningIn(bool waiting)
{
	signInButton->setEnabled(!waiting);
	cancelSignIn->setVisible(waiting);
	set_level(signInStatus, "");
	if (waiting) {
		signInFrame = 0;
		signInStatus->setText(QStringLiteral("%1  %2").arg(ui::SPINNER.at(0), tr("Continue in your browser…")));
		signInSpin->start();
	} else {
		signInSpin->stop();
	}
}

// ---------------------------------------------------------------- main page

QWidget *Dock::buildSignedIn()
{
	auto *page = new QWidget;
	auto *pv = new QVBoxLayout(page);
	pv->setContentsMargins(0, 0, 0, 0);
	pv->setSpacing(0);

	auto *content = new QWidget;
	auto *v = new QVBoxLayout(content);
	v->setContentsMargins(0, 0, 4, 0);
	v->setSpacing(0);

	// Broadcast: the list, edit, and the link listeners open.
	auto *bv = new QVBoxLayout;
	bv->setSpacing(GAP_LABEL);
	bv->addWidget(ui::eyebrow(tr("Broadcast")));
	auto *row = new QHBoxLayout;
	row->setSpacing(6);
	broadcastBox = new ui::ReloadingCombo;
	broadcastBox->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	broadcastBox->setMinimumContentsLength(8);
	broadcastBox->addItem(tr("Loading…"), QString::fromLatin1(LOADING));
	connect(broadcastBox, &ui::ReloadingCombo::aboutToShowPopup, this, [this]() {
		if (QDateTime::currentMSecsSinceEpoch() - broadcastsLoadedAt > BROADCASTS_FRESH_MS)
			loadBroadcasts();
	});
	connect(broadcastBox, &QComboBox::activated, this, [this](int index) {
		const QString id = broadcastBox->itemData(index).toString();
		if (id == QLatin1String(LOADING))
			return;
		if (id == QLatin1String(NEW_BROADCAST)) {
			broadcastBox->setCurrentIndex(qMax(0, broadcastBox->findData(settings.broadcastId)));
			openBroadcastEditor(QString());
			return;
		}
		settings.broadcastId = id;
		settings.save();
		applyBroadcastDefaults(id);
		showBroadcastLink();
		updateLinks();
	});
	editBroadcastButton = new QPushButton(tr("Edit"));
	editBroadcastButton->setProperty("role", "edit");
	// As tall as the list beside it, whatever OBS's theme makes that.
	broadcastBox->installEventFilter(this);
	connect(editBroadcastButton, &QPushButton::clicked, this, [this]() {
		openBroadcastEditor(broadcastObjects.contains(settings.broadcastId) ? settings.broadcastId : QString());
	});
	row->addWidget(broadcastBox, 1);
	row->addWidget(editBroadcastButton);
	bv->addLayout(row);
	auto *linkRow = new QHBoxLayout;
	linkRow->setSpacing(4);
	broadcastLink = new ui::ElidedLabel;
	broadcastLink->setProperty("role", "muted");
	copyBroadcastButton = ui::linkButton(tr("Copy"));
	copyBroadcastButton->setToolTip(tr("Copy the broadcast's link, for listeners"));
	connect(copyBroadcastButton, &QToolButton::clicked, this, &Dock::copyBroadcastLink);
	// Retry a failed load, or make the first broadcast.
	broadcastAction = ui::linkButton(QString());
	broadcastAction->hide();
	connect(broadcastAction, &QToolButton::clicked, this, [this]() {
		if (broadcastsFailed)
			loadBroadcasts();
		else
			openBroadcastEditor(QString());
	});
	linkRow->addWidget(broadcastLink);
	linkRow->addWidget(copyBroadcastButton);
	linkRow->addWidget(broadcastAction);
	linkRow->addStretch(1);
	bv->addLayout(linkRow);
	// While live, in place of the fields that cannot change.
	liveSummary = ui::muted();
	liveSummary->hide();
	bv->addWidget(liveSummary);
	v->addLayout(bv);

	spokenBox = new ui::ReloadingCombo;
	spokenBox->addItem(tr("Loading…"), QString());
	connect(spokenBox, &ui::ReloadingCombo::aboutToShowPopup, this, [this]() {
		if (spokenLanguages.isEmpty())
			loadLists();
	});
	connect(spokenBox, &QComboBox::activated, this, [this]() {
		const QString lang = spokenBox->currentData().toString();
		const bool changed = lang != settings.spokenLanguage;
		settings.spokenLanguage = lang;
		settings.save();
		if (changed)
			warm->languageChanged();
	});
	spokenBlock = ui::field(v, tr("Spoken language"), spokenBox);
	spokenBlock->layout()->setContentsMargins(0, GAP_FIELD, 0, 0);

	sendSwitch = new ui::Segmented;
	sendSwitch->addOption(tr("Audio only"), false);
	sendSwitch->addOption(tr("Audio + video"), true, tr("Also keeps your video with the recording"));
	sendSwitch->setValue(settings.sendVideo);
	connect(sendSwitch, &ui::Segmented::changed, this, [this](const QVariant &value) {
		settings.sendVideo = value.toBool();
		settings.save();
	});
	sendBlock = ui::field(v, tr("Send"), sendSwitch);
	sendBlock->layout()->setContentsMargins(0, GAP_FIELD, 0, 0);
	v->addSpacing(GAP_SECTION);

	startButton = new QPushButton(tr("Start transcribing"));
	startButton->setProperty("role", "primary");
	connect(startButton, &QPushButton::clicked, this, &Dock::toggle);
	v->addWidget(startButton);
	stopSpin = new QTimer(this);
	stopSpin->setInterval(90);
	connect(stopSpin, &QTimer::timeout, this, [this]() {
		stopFrame++;
		startButton->setText(
			QStringLiteral("%1  %2").arg(ui::SPINNER.at(stopFrame % ui::SPINNER.size()), tr("Stopping…")));
	});

	// Under Start: whichever of these is showing, 10 px below it.
	auto *under = new QVBoxLayout;
	under->setContentsMargins(0, 10, 0, 0);
	under->setSpacing(8);
	followHint = ui::muted();
	followHint->setTextFormat(Qt::RichText);
	connect(followHint, &QLabel::linkActivated, this, [this]() { pages->setCurrentIndex(SettingsPage); });
	under->addWidget(followHint);
	idleStatus = new QLabel;
	idleStatus->setWordWrap(true);
	idleStatus->setTextFormat(Qt::RichText);
	connect(idleStatus, &QLabel::linkActivated, this, [this](const QString &link) {
		if (link == "update") {
			updateWanted = true;
			showUpdate();
		} else {
			QDesktopServices::openUrl(QUrl(link));
		}
	});
	idleStatus->hide();
	under->addWidget(idleStatus);

	// While live: a card under Stop.
	auto *card = new QFrame;
	card->setProperty("role", "card");
	liveCard = card;
	auto *grid = new QGridLayout(card);
	grid->setContentsMargins(12, 10, 10, 10);
	grid->setHorizontalSpacing(8);
	grid->setVerticalSpacing(4);
	auto *top = new QHBoxLayout;
	top->setSpacing(8);
	liveDot = new QLabel(QStringLiteral("●"));
	liveDot->setProperty("role", "dot");
	liveWord = ui::eyebrow(tr("Live"));
	liveClock = new QLabel;
	// Digits that do not jitter: tabular figures where Qt has them (6.7),
	// else room for the widest time.
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
	QFont clockFont = liveClock->font();
	clockFont.setFeature(QFont::Tag("tnum"), 1);
	liveClock->setFont(clockFont);
#endif
	liveClock->setMinimumWidth(liveClock->fontMetrics().horizontalAdvance(QStringLiteral("0:00:00")));
	captionsToggle = new QToolButton;
	captionsToggle->setText(QStringLiteral("CC"));
	captionsToggle->setCheckable(true);
	captionsToggle->setProperty("role", "toggle");
	captionsToggle->setChecked(settings.showCaptions);
	auto captionsTip = [this](bool on) {
		captionsToggle->setToolTip(on ? tr("Captions shown on the OBS preview (not in your stream)")
					      : tr("Show captions on the OBS preview"));
	};
	captionsTip(settings.showCaptions);
	connect(captionsToggle, &QToolButton::toggled, this, [this, captionsTip](bool on) {
		settings.showCaptions = on;
		captionsTip(on);
		if (!on && overlay)
			overlay->setCaption(QString(), QString());
	});
	top->addWidget(liveDot);
	top->addWidget(liveWord);
	top->addWidget(liveClock);
	top->addStretch(1);
	top->addWidget(captionsToggle);
	popOutButton = ui::linkButton(tr("Transcript ↗"), nullptr, "muted");
	popOutButton->setToolTip(tr("Show the live transcript in a window you can move to another monitor"));
	connect(popOutButton, &QToolButton::clicked, this, [this]() {
		if (transcriptPanel)
			transcriptPanel->popOut();
	});
	top->addWidget(popOutButton);
	grid->addLayout(top, 0, 0, 1, 2);
	liveStatus = new QLabel;
	liveStatus->setWordWrap(true);
	liveStatus->setTextFormat(Qt::RichText);
	liveStatus->setOpenExternalLinks(true);
	liveListeners = ui::muted();
	liveListeners->setWordWrap(false);
	liveListeners->hide();
	grid->addWidget(liveStatus, 1, 0);
	grid->addWidget(liveListeners, 1, 1, Qt::AlignRight | Qt::AlignTop);
	grid->setColumnStretch(0, 1);
	card->hide();
	under->addWidget(card);
	liveTick = new QTimer(this);
	liveTick->setInterval(1000);
	connect(liveTick, &QTimer::timeout, this, &Dock::tickLive);

	// VocalStack's warnings and errors while live, one callout each.
	noticesBox = new QVBoxLayout;
	noticesBox->setSpacing(6);
	under->addLayout(noticesBox);
	v->addLayout(under);

	// The transcription in VocalStack: open it, or share it.
	transcriptBlock = new QWidget;
	auto *tv = new QVBoxLayout(transcriptBlock);
	tv->setContentsMargins(0, GAP_SECTION, 0, 0);
	tv->setSpacing(GAP_LABEL);
	transcriptEyebrow = ui::eyebrow(tr("Last transcript"));
	tv->addWidget(transcriptEyebrow);
	transcriptButtons = new QBoxLayout(QBoxLayout::LeftToRight);
	transcriptButtons->setSpacing(6);
	openButton = new QPushButton(tr("Open transcript ↗"));
	openButton->setProperty("role", "secondary");
	connect(openButton, &QPushButton::clicked, this, &Dock::openTranscription);
	copyLinkButton = new QPushButton(tr("Copy share link"));
	copyLinkButton->setProperty("role", "secondary");
	copyLinkButton->setToolTip(tr("Copy a link anyone can read the transcription with"));
	// Wide enough for "✓ Link copied" too, so neither button jumps.
	const QFontMetrics fm(copyLinkButton->font());
	copyLinkButton->setMinimumWidth(
		qMax(fm.horizontalAdvance(tr("Copy share link")), fm.horizontalAdvance(tr("✓ Link copied"))) + 32);
	connect(copyLinkButton, &QPushButton::clicked, this, &Dock::copyTranscriptLink);
	transcriptButtons->addWidget(openButton, 1);
	transcriptButtons->addWidget(copyLinkButton, 1);
	tv->addLayout(transcriptButtons);
	v->addWidget(transcriptBlock);
	v->addStretch(1);
	pv->addWidget(scrolling(content), 1);

	// Pinned below: a newer release, and who is signed in.
	updateCallout = new ui::Callout;
	updateCallout->hide();
	updateButton = new QPushButton(tr("Update now"));
	updateButton->setProperty("role", "secondary");
	connect(updateButton, &QPushButton::clicked, this, [this]() {
		updateWanted = true;
		showUpdate();
	});
	updateCallout->addAction(updateButton);
	connect(updateCallout, &ui::Callout::dismissed, this, [this]() {
		updateDismissed = updateNoticeText;
		showUpdate();
	});
	connect(updateCallout, &ui::Callout::linkActivated, this,
		[](const QString &url) { QDesktopServices::openUrl(QUrl(url)); });
	updateSpin = new QTimer(this);
	updateSpin->setInterval(90);
	connect(updateSpin, &QTimer::timeout, this, [this]() {
		updateSpinFrame++;
		showUpdate();
	});
	auto *pinned = new QVBoxLayout;
	pinned->setContentsMargins(0, 8, 0, 0);
	pinned->addWidget(updateCallout);
	pv->addLayout(pinned);
	pv->addSpacing(8);
	pv->addWidget(ui::rule());
	pv->addSpacing(6);
	auto *foot = new QHBoxLayout;
	auto *account = new ui::ElidedLabel;
	account->setProperty("role", "muted");
	accountLabel = account;
	auto *settingsButton = ui::linkButton(QStringLiteral("⚙︎ ") + tr("Settings"));
	connect(settingsButton, &QToolButton::clicked, this, [this]() { pages->setCurrentIndex(SettingsPage); });
	foot->addWidget(account, 1);
	foot->addWidget(settingsButton);
	pv->addLayout(foot);
	return page;
}

// ---------------------------------------------------------------- settings

QWidget *Dock::buildSettings()
{
	auto *page = new QWidget;
	auto *outer = new QVBoxLayout(page);
	outer->setContentsMargins(0, 0, 0, 0);
	outer->setSpacing(GAP_FIELD);
	QLabel *heading;
	outer->addWidget(pageHeader(&heading, nullptr));
	heading->setText(tr("Settings"));
	settingsLiveNote = ui::muted(tr("Some settings can't change while transcribing."));
	set_level(settingsLiveNote, "warn");
	settingsLiveNote->hide();
	outer->addWidget(settingsLiveNote);
	new QShortcut(QKeySequence(Qt::Key_Escape), page, this, [this]() { goBack(); }, Qt::WidgetWithChildrenShortcut);

	auto *content = new QWidget;
	auto *v = new QVBoxLayout(content);
	v->setContentsMargins(0, 0, 4, 0);
	v->setSpacing(GAP_LABEL);
	// A section: its title above a card.
	auto section = [v](const QString &title, bool first = false) {
		if (!first)
			v->addSpacing(GAP_SECTION - GAP_LABEL);
		v->addWidget(ui::eyebrow(title));
		auto *card = new ui::Card;
		v->addWidget(card);
		return card;
	};

	// Transcription.
	auto *tx = section(tr("Transcription"), true);
	auto *nameBlock = new QVBoxLayout;
	nameBlock->setSpacing(GAP_LABEL);
	nameBlock->addWidget(new QLabel(tr("Name")));
	nameEdit = new QLineEdit;
	nameEdit->setPlaceholderText(tr("OBS on %1").arg(host_name()));
	nameEdit->setText(settings.transcriptionName);
	connect(nameEdit, &QLineEdit::editingFinished, this, [this]() {
		settings.transcriptionName = nameEdit->text().trimmed().left(200);
		settings.save();
	});
	nameBlock->addWidget(nameEdit);
	nameBlock->addWidget(ui::muted(tr("Optional. What the transcription is called in VocalStack.")));
	tx->addLayoutRow(nameBlock);
	lockBox = new ui::Switch;
	lockBox->setChecked(settings.locked);
	connect(lockBox, &ui::Switch::toggled, this, [this](bool on) {
		settings.locked = on;
		settings.save();
	});
	tx->addRow(ui::settingRow(tr("Lock against deletion"),
				  tr("It can't be deleted from the dashboard until unlocked."), lockBox));

	// Speaker labels.
	auto *sp = section(tr("Speaker labels"));
	auto *spBlock = new QVBoxLayout;
	spBlock->setSpacing(6);
	speakersSwitch = new ui::Segmented;
	speakersSwitch->addOption(tr("When needed"), "auto");
	speakersSwitch->addOption(tr("On"), "on");
	speakersSwitch->addOption(tr("Off"), "off");
	speakersSwitch->setValue(settings.speakerLabels.isEmpty() ? "auto" : settings.speakerLabels);
	connect(speakersSwitch, &ui::Segmented::changed, this, [this](const QVariant &value) {
		settings.speakerLabels = value.toString();
		settings.save();
	});
	spBlock->addWidget(speakersSwitch);
	spBlock->addWidget(ui::muted(tr("Follows the broadcast's default until you change it here.")));
	sp->addLayoutRow(spBlock);

	// Captions over the preview.
	auto *cc = section(tr("Captions"));
	auto *colorControls = new QWidget;
	auto *colorRow = new QHBoxLayout(colorControls);
	colorRow->setContentsMargins(0, 0, 0, 0);
	colorRow->setSpacing(6);
	captionColorReset = ui::linkButton(tr("Reset"), nullptr, "muted");
	connect(captionColorReset, &QToolButton::clicked, this, [this]() {
		settings.captionBackground.clear();
		settings.save();
		showCaptionColor();
	});
	captionColorButton = new QToolButton;
	captionColorButton->setFixedSize(44, 26);
	captionColorButton->setCursor(Qt::PointingHandCursor);
	connect(captionColorButton, &QToolButton::clicked, this, [this]() {
		const QColor was = settings.captionBackground.isEmpty() ? QColor(10, 12, 41, 190)
								       : QColor(settings.captionBackground);
		const QColor c = QColorDialog::getColor(was, this, tr("Caption background"), QColorDialog::ShowAlphaChannel);
		if (!c.isValid())
			return;
		settings.captionBackground = c.name(QColor::HexArgb);
		settings.save();
		showCaptionColor();
	});
	colorRow->addWidget(captionColorReset);
	colorRow->addWidget(captionColorButton);
	cc->addRow(ui::settingRow(tr("Background"), tr("The box behind the captions over the preview. Lower its opacity to see more of the scene."),
				  colorControls));
	showCaptionColor();

	// Behaviour.
	auto *be = section(tr("Behaviour"));
	followBox = new ui::Switch;
	followBox->setChecked(settings.followObs);
	connect(followBox, &ui::Switch::toggled, this, [this](bool on) {
		settings.followObs = on;
		settings.save();
		updateFollowHint();
	});
	be->addRow(ui::settingRow(tr("Follow OBS"), tr("Start and stop with streaming or recording."), followBox));
	autoUpdateBox = new ui::Switch;
	autoUpdateBox->setChecked(settings.autoUpdate);
	connect(autoUpdateBox, &ui::Switch::toggled, this, [this](bool on) {
		settings.autoUpdate = on;
		settings.save();
		showUpdate();
	});
	be->addRow(ui::settingRow(tr("Update automatically"), tr("Installs while idle; restart OBS to use it."),
				  autoUpdateBox));

	// Version.
	auto *ab = section(tr("About"));
	checkUpdatesButton = new QPushButton(tr("Check for updates"));
	checkUpdatesButton->setProperty("role", "secondary");
	connect(checkUpdatesButton, &QPushButton::clicked, this, [this]() {
		// A newer release waiting: this is Update now.
		if (updater->age() != Updater::Age::Current && updater->step() == Updater::Step::Idle) {
			updateWanted = true;
			showUpdate();
			return;
		}
		checkWanted = true;
		checkUpdatesButton->setEnabled(false);
		checkUpdatesResult->setText(tr("Checking…"));
		updater->check(true);
		if (updater->step() != Updater::Step::Checking)
			showUpdate(); // busy downloading or installing: say so now
	});
	auto *aboutRow = ui::settingRow(tr("VocalStack for OBS %1").arg(PLUGIN_VERSION), QString(), checkUpdatesButton);
	checkUpdatesResult = ui::muted();
	static_cast<QVBoxLayout *>(static_cast<QHBoxLayout *>(aboutRow->layout())->itemAt(0)->layout())
		->addWidget(checkUpdatesResult);
	ab->addRow(aboutRow);

	// Account.
	auto *ac = section(tr("Account"));
	signOutButton = new QPushButton(tr("Sign out"));
	signOutButton->setProperty("role", "secondary");
	connect(signOutButton, &QPushButton::clicked, this, [this]() { signOut(QString()); });
	auto *accountRow = ui::settingRow(QString(), QString(), signOutButton);
	auto *accountTexts = static_cast<QVBoxLayout *>(static_cast<QHBoxLayout *>(accountRow->layout())->itemAt(0)->layout());
	settingsAccount = static_cast<QLabel *>(accountTexts->itemAt(0)->widget());
	signOutNote = ui::muted(tr("Stop transcribing to sign out."));
	signOutNote->hide();
	accountTexts->addWidget(signOutNote);
	ac->addRow(accountRow);
	v->addStretch(1);

	outer->addWidget(scrolling(content), 1);
	return page;
}

// ---------------------------------------------------------------- state

void Dock::setOverlay(CaptionOverlay *o)
{
	overlay = o;
	if (overlay)
		overlay->setBackground(QColor(settings.captionBackground));
}

// The swatch shows the caption box's colour; Reset when it is not the
// default.
void Dock::showCaptionColor()
{
	const QColor c = settings.captionBackground.isEmpty() ? QColor(10, 12, 41, 190) : QColor(settings.captionBackground);
	captionColorButton->setStyleSheet(QStringLiteral("QToolButton { background: rgba(%1,%2,%3,%4); border: 1px solid "
							 "rgba(128,128,128,120); border-radius: 6px; }")
						  .arg(c.red())
						  .arg(c.green())
						  .arg(c.blue())
						  .arg(c.alpha()));
	captionColorButton->setToolTip(tr("Caption background: %1 at %2 %").arg(c.name()).arg(qRound(c.alphaF() * 100)));
	captionColorReset->setVisible(!settings.captionBackground.isEmpty());
	if (overlay)
		overlay->setBackground(QColor(settings.captionBackground));
}

void Dock::setTranscriptPanel(TranscriptPanel *panel)
{
	transcriptPanel = panel;
	connect(session, &Session::transcriptLine, panel, &TranscriptPanel::setLine);
	connect(session, &Session::transcriptRemoved, panel, &TranscriptPanel::removeLines);
	connect(session, &Session::transcriptCleared, panel, &TranscriptPanel::clear);
	connect(session, &Session::speakerLabels, panel, &TranscriptPanel::setSpeakerLabels);
}

void Dock::onStatus(const QString &text, Session::Level level)
{
	if (level != Session::Level::Ok)
		obs_log(LOG_WARNING, "%s", text.toUtf8().constData());
	if (session->state() == Session::State::Idle) {
		// A clean stop says itself in the transcript's heading instead.
		const QString shown = level == Session::Level::Ok ? QString() : text;
		idleStatus->setText(shown);
		set_level(idleStatus, level_name(level));
		idleStatus->setVisible(!shown.isEmpty());
		return;
	}
	liveStatus->setText(text);
	set_level(liveStatus, level_name(level));
	liveLevel = level;
	tickLive();
}

// The live card's dot and word (coral, with a slow pulse while all is well;
// amber while connecting or warning; red on a failure) and the time since
// going live.
void Dock::tickLive()
{
	const Session::State s = session->state();
	liveTicks++;
	const char *level = "live";
	if (s == Session::State::Starting || liveLevel == Session::Level::Warn)
		level = "warn";
	else if (liveLevel == Session::Level::Error)
		level = "error";
	set_level(liveDot, std::strcmp(level, "live") == 0 && liveTicks % 4 == 3 ? "dim" : level);
	set_level(liveWord, level);
	liveWord->setVisible(s != Session::State::Starting);
	liveWord->setText(s == Session::State::Stopping ? tr("Stopping") : tr("Live"));
	if (s == Session::State::Live && liveSince.isValid()) {
		const qint64 secs = liveSince.elapsed() / 1000;
		liveClock->setText(secs < 3600 ? QStringLiteral("%1:%2").arg(secs / 60).arg(secs % 60, 2, 10, QChar('0'))
					       : QStringLiteral("%1:%2:%3")
							 .arg(secs / 3600)
							 .arg((secs / 60) % 60, 2, 10, QChar('0'))
							 .arg(secs % 60, 2, 10, QChar('0')));
		liveClock->show();
	} else {
		liveClock->hide();
	}
}

void Dock::showNotices(const QStringList &warnings, const QStringList &errors)
{
	currentNotices = errors + warnings;
	// Later, not now: a callout's own ✕ may be what called this.
	while (QLayoutItem *item = noticesBox->takeAt(0)) {
		if (QWidget *w = item->widget()) {
			w->hide();
			w->deleteLater();
		}
		delete item;
	}
	auto add = [this, warnings, errors](const QString &note, const char *tone) {
		if (dismissedNotices.contains(note))
			return;
		obs_log(LOG_WARNING, "%s", note.toUtf8().constData());
		auto *c = new ui::Callout;
		c->setTone(tone);
		c->setText(note.toHtmlEscaped());
		connect(c, &ui::Callout::dismissed, this, [this, note, warnings, errors]() {
			dismissedNotices.append(note);
			showNotices(warnings, errors);
		});
		noticesBox->addWidget(c);
	};
	for (const QString &note : errors)
		add(note, "error");
	for (const QString &note : warnings)
		add(note, "warn");
}

// While live: the choices that cannot change, in one line.
void Dock::updateLiveSummary()
{
	const QString lang = spokenBox->currentData().toString().isEmpty() ? tr("Auto detect") : spokenBox->currentText();
	liveSummary->setText(QStringLiteral("%1 · %2").arg(lang, settings.sendVideo ? tr("Audio + video") : tr("Audio only")));
}

void Dock::onState(Session::State s)
{
	const bool idle = s == Session::State::Idle;
	const bool stopping = s == Session::State::Stopping;
	if (stopping && liveSince.isValid())
		lastDurationS = liveSince.elapsed() / 1000;
	if (stopping) {
		stopFrame = 0;
		startButton->setText(QStringLiteral("%1  %2").arg(ui::SPINNER.at(0), tr("Stopping…")));
		stopSpin->start();
	} else {
		stopSpin->stop();
		startButton->setText(idle ? tr("Start transcribing") : tr("■  Stop transcribing"));
	}
	startButton->setProperty("busy", stopping);
	ui::setRole(startButton, idle ? "primary" : "danger");
	startButton->setEnabled(!stopping);

	if (s == Session::State::Starting) {
		dismissedNotices.clear();
		lastDurationS = -1;
		liveStatus->setText(tr("Connecting…"));
		set_level(liveStatus, "warn");
		liveLevel = Session::Level::Warn;
		liveListeners->hide();
	}
	if (s == Session::State::Live && !liveSince.isValid())
		liveSince.start();
	if (idle)
		liveSince.invalidate();
	liveCard->setVisible(!idle);
	if (!idle) {
		idleStatus->hide();
		liveTick->start();
		tickLive();
	} else {
		liveTick->stop();
	}
	captionsToggle->setChecked(settings.showCaptions);

	// While live, the fields that cannot change give way to a summary.
	spokenBlock->setVisible(idle);
	sendBlock->setVisible(idle);
	updateLiveSummary();
	liveSummary->setVisible(!idle);
	for (QWidget *w : std::initializer_list<QWidget *>{broadcastBox, editBroadcastButton, speakersSwitch, lockBox,
							  nameEdit, signOutButton})
		w->setEnabled(idle);
	settingsLiveNote->setVisible(!idle);
	signOutNote->setVisible(!idle);
	updateFollowHint();
	updateLinks();
	showUpdate(); // idle again: an update waiting may install
	// Starting: canStart false; back to idle: true again if still in use.
	warm->refresh();
}

void Dock::toggle()
{
	if (settings.apiKey.isEmpty())
		return;
	if (session->state() == Session::State::Idle)
		session->start();
	else
		session->stop();
}

static bool obs_busy()
{
	return obs_frontend_streaming_active() || obs_frontend_recording_active();
}

void Dock::obsOutputStarted()
{
	if (settings.followObs && !settings.apiKey.isEmpty() && session->state() == Session::State::Idle)
		session->start();
	updateFollowHint();
}

void Dock::obsOutputStopped()
{
	if (settings.followObs && !obs_busy())
		session->stop();
	updateFollowHint();
}

void Dock::updateFollowHint()
{
	followHint->setVisible(settings.followObs && session->state() == Session::State::Idle && !obs_busy());
}

// The transcript buttons once there is a transcript; Edit or + New.
void Dock::updateLinks()
{
	const bool idle = session->state() == Session::State::Idle;
	transcriptBlock->setVisible(!settings.transcriptionId.isEmpty());
	transcriptEyebrow->setVisible(idle);
	transcriptEyebrow->setText(lastDurationS >= 0 ? tr("Last transcript · %1").arg(duration_text(lastDurationS))
						      : tr("Last transcript"));
	const bool chosen = broadcastObjects.contains(settings.broadcastId);
	editBroadcastButton->setText(chosen ? tr("Edit") : tr("+ New"));
	editBroadcastButton->setToolTip(chosen ? tr("Edit this broadcast: name, link, visibility, language, speaker labels")
					       : tr("Make a broadcast: a link listeners open"));
	copyBroadcastButton->setEnabled(chosen);
}

void Dock::openTranscription()
{
	if (!settings.transcriptionId.isEmpty())
		QDesktopServices::openUrl(QUrl(api->appBase() + "/transcriptions/" + settings.transcriptionId));
}

void Dock::copyBroadcastLink()
{
	const QString url = broadcastObjects.value(settings.broadcastId).value("shareUrl").toString();
	if (url.isEmpty())
		return;
	QGuiApplication::clipboard()->setText(url);
	ui::flash(copyBroadcastButton, tr("✓ Copied"));
}

// A share link of the transcription, made once per click (each is its own
// link, revocable in VocalStack).
void Dock::copyTranscriptLink()
{
	if (settings.transcriptionId.isEmpty())
		return;
	copyLinkButton->setEnabled(false);
	QJsonObject body{{"label", tr("OBS")}};
	api->post("/v1/transcriptions/" + settings.transcriptionId + "/share-links", body,
		  [this](int, const QJsonDocument &doc, const QString &error) {
			  copyLinkButton->setEnabled(true);
			  const QString url = doc.object().value("url").toString();
			  if (!error.isEmpty() || url.isEmpty()) {
				  onStatus(tr("Couldn't make a share link: %1")
						   .arg(error.isEmpty() ? tr("no link came back") : error),
					   Session::Level::Error);
				  return;
			  }
			  QGuiApplication::clipboard()->setText(url);
			  ui::flash(copyLinkButton, tr("✓ Link copied"));
		  });
}

// ---------------------------------------------------------------- updates

// The update notice: a newer release, how old this one is, and where the
// install stands. With automatic updates on, a newer release is installed
// as soon as nothing is being transcribed.
void Dock::showUpdate()
{
	const Updater::Age age = updater->age();
	const Updater::Step step = updater->step();
	const QString latest = updater->latest();
	// Settings → About follows the update as it goes: the check's answer,
	// the download and install with a spinner, then a check mark.
	{
		const bool working = step == Updater::Step::Downloading || step == Updater::Step::Installing;
		const bool installed = step == Updater::Step::Installed || step == Updater::Step::OnExit;
		const bool waiting = age != Updater::Age::Current && step == Updater::Step::Idle;
		QString about;
		if (step == Updater::Step::Checking && checkWanted)
			about = tr("Checking…");
		else if (working)
			about = QStringLiteral("%1  %2").arg(ui::SPINNER.at(updateSpinFrame % ui::SPINNER.size()),
							     step == Updater::Step::Downloading ? tr("Downloading %1…").arg(latest)
												: tr("Installing %1…").arg(latest));
		else if (installed)
			about = step == Updater::Step::OnExit ? tr("✓ %1 is ready: quit OBS to finish installing it.").arg(latest)
							      : tr("✓ %1 is installed. Restart OBS to use it.").arg(latest);
		else if (waiting && updater->problem())
			about = updater->message();
		else if (waiting && (updateWanted || settings.autoUpdate) && session->state() != Session::State::Idle)
			about = tr("%1 installs once you stop transcribing.").arg(latest);
		else if (waiting)
			about = tr("Version %1 is out.").arg(latest);
		else if (checkWanted && !updater->checkError().isEmpty())
			about = tr("Could not check: %1").arg(updater->checkError());
		else if (checkWanted)
			about = tr("You have the latest version.");
		else
			about = checkUpdatesResult->text().startsWith(tr("You have")) ? checkUpdatesResult->text() : QString();
		if (step != Updater::Step::Checking)
			checkWanted = false;
		checkUpdatesResult->setText(about);
		set_level(checkUpdatesResult, waiting && updater->problem() ? "warn" : "");
		checkUpdatesButton->setText(waiting && !updater->problem() ? tr("Update now") : tr("Check for updates"));
		checkUpdatesButton->setEnabled(!working && step != Updater::Step::Checking && !installed);
	}
	QString text = updater->message().toHtmlEscaped();
	bool problem = updater->problem();
	// A failed install: the page to download it from by hand.
	if (problem && !text.isEmpty())
		text += " " + anchor(this, PLUGIN_PAGE, tr("Download"));
	const bool pending = age != Updater::Age::Current && step == Updater::Step::Idle;
	if (pending && (settings.autoUpdate || updateWanted) && !settings.apiKey.isEmpty() && !problem &&
	    session->state() == Session::State::Idle) {
		updater->install(); // says how it goes through changed()
		return;
	}
	if (pending && updateWanted && text.isEmpty())
		text = tr("VocalStack for OBS %1 installs once you stop transcribing.").arg(latest);
	if (text.isEmpty() && pending) {
		switch (age) {
		case Updater::Age::Available:
			text = tr("VocalStack for OBS %1 is out (you have %2).").arg(latest, PLUGIN_VERSION);
			break;
		case Updater::Age::Deprecated:
			text = tr("VocalStack for OBS %1 is out; this version (%2) is old and misses newer features.")
				       .arg(latest, PLUGIN_VERSION);
			problem = true;
			break;
		case Updater::Age::Unsupported:
			text = tr("VocalStack for OBS %1 is out; this version (%2) is no longer supported.")
				       .arg(latest, PLUGIN_VERSION);
			problem = true;
			break;
		case Updater::Age::Current:
			break;
		}
	}
	// Working: a spinner before the words. Done: a check.
	const bool working = step == Updater::Step::Downloading || step == Updater::Step::Installing;
	const bool done = step == Updater::Step::Installed || step == Updater::Step::OnExit;
	if (working != updateSpin->isActive())
		working ? updateSpin->start() : updateSpin->stop();
	updateNoticeText = text;
	const bool dismissed = !text.isEmpty() && text == updateDismissed && !working;
	QString shown = text;
	if (working)
		shown = QStringLiteral("%1&nbsp;&nbsp;%2").arg(ui::SPINNER.at(updateSpinFrame % ui::SPINNER.size()), text);
	else if (done)
		shown = QStringLiteral("<span style='color:%1; font-weight:600;'>✓</span>&nbsp;&nbsp;%2")
				.arg(ui::Theme::of(this).ok, text);
	updateCallout->setTone(problem ? "warn" : "info");
	updateCallout->setText(shown);
	updateCallout->setDismissible(!working);
	updateCallout->setVisible(!text.isEmpty() && !dismissed);
	updateButton->setVisible(pending && !settings.autoUpdate && !updateWanted);
	// An unsupported version does not start, and says why where Start is.
	const bool blocked = age == Updater::Age::Unsupported && step != Updater::Step::Installed;
	const bool idle = session->state() == Session::State::Idle;
	startButton->setEnabled(!blocked && session->state() != Session::State::Stopping);
	startButton->setToolTip(blocked ? tr("This version is no longer supported: update to keep transcribing")
					: QString());
	if (blocked && idle) {
		idleStatus->setText(tr("Update to keep transcribing: this version is no longer supported. %1")
					    .arg(anchor(this, "update", tr("Update now"))));
		set_level(idleStatus, "error");
		idleStatus->show();
	}
}

// ---------------------------------------------------------------- sign-in

void Dock::showSignedIn(bool in)
{
	pages->setCurrentIndex(in ? Main : SignedOut);
	if (!in)
		return;
	const QString who = settings.account.isEmpty() ? tr("an API key") : settings.account;
	static_cast<ui::ElidedLabel *>(accountLabel)
		->setFullText(settings.account.isEmpty() ? tr("Signed in with an API key") : settings.account);
	settingsAccount->setText(tr("Signed in as <b>%1</b>").arg(who.toHtmlEscaped()));
	loadLists();
	loadBroadcasts();
	session->refreshLimits();
	updateLinks();
	// When signed in (nothing is sent before): whether a newer release is
	// out, now and every 8 hours.
	updater->check();
}

void Dock::signedIn(const QString &key, const QString &account)
{
	settings.apiKey = key;
	settings.account = account;
	settings.save();
	api->setKey(key);
	signInStatus->clear();
	showSignedIn(true);
	warm->refresh();
}

// A pasted key is checked with a read before it is kept.
void Dock::useKey()
{
	const QString key = keyEdit->text().trimmed();
	if (key.isEmpty())
		return;
	api->setKey(key);
	signInStatus->setText(tr("Checking the key…"));
	set_level(signInStatus, "");
	api->get("/v1/me", [this, key](int, const QJsonDocument &doc, const QString &error) {
		if (!error.isEmpty()) {
			api->setKey(settings.apiKey);
			signInStatus->setText(tr("VocalStack refused the key: %1").arg(error));
			set_level(signInStatus, "error");
			return;
		}
		const QJsonObject me = doc.object();
		QStringList scopes;
		for (const QJsonValue s : me.value("scopes").toArray())
			scopes.append(s.toString());
		QStringList missing;
		for (const QString &need : {QStringLiteral("write"), QStringLiteral("recordings:publish")})
			if (!scopes.contains("*") && !scopes.contains(need))
				missing.append(need);
		if (!missing.isEmpty()) {
			api->setKey(settings.apiKey);
			signInStatus->setText(tr("This key cannot transcribe: it lacks the %1 scope. Make one with read, "
						 "write and recordings:publish in Developers → API keys.")
						      .arg(missing.join(tr(" and "))));
			set_level(signInStatus, "error");
			return;
		}
		keyEdit->clear();
		signedIn(key, me.value("displayName").toString());
	});
}

// Sign-out forgets everything tied to the account, and removes the
// plugin's stream key from it.
void Dock::signOut(const QString &why)
{
	session->stop();
	if (!settings.streamKeyId.isEmpty() && why.isEmpty()) // a refused key cannot remove anything
		api->del("/v1/rtmp/keys/" + settings.streamKeyId, [](int, const QJsonDocument &, const QString &) {});
	const Settings kept = settings;
	settings = Settings();
	settings.sendVideo = kept.sendVideo;
	settings.followObs = kept.followObs;
	settings.speakerLabels = kept.speakerLabels;
	settings.latencyMode = kept.latencyMode;
	settings.autoUpdate = kept.autoUpdate;
	settings.transcriptionName = kept.transcriptionName;
	settings.showCaptions = true;
	broadcastObjects.clear();
	broadcastOrder.clear();
	broadcastsShown.clear();
	appliedBroadcastId.clear();
	settings.save();
	warm->refresh(); // canStart false while the key still works
	api->setKey(QString());
	showSignedIn(false);
	signInStatus->setText(why);
	set_level(signInStatus, why.isEmpty() ? "" : "error");
}

// ---------------------------------------------------------------- lists

void Dock::loadLists()
{
	api->get("/v1/languages", [this](int, const QJsonDocument &doc, const QString &error) {
		spokenBox->clear();
		spokenBox->addItem(tr("Auto detect"), QString());
		if (!error.isEmpty()) {
			// The saved language stays choosable; the list is asked for
			// again when it is opened.
			obs_log(LOG_WARNING, "languages: %s", error.toUtf8().constData());
			if (!settings.spokenLanguage.isEmpty())
				spokenBox->addItem(settings.spokenLanguage, settings.spokenLanguage);
			spokenBox->setCurrentIndex(qMax(0, spokenBox->findData(settings.spokenLanguage)));
			return;
		}
		const QJsonObject o = doc.object();
		QHash<QString, QString> names;
		for (const QJsonValue v : o.value("languages").toArray())
			names.insert(v.toObject().value("code").toString(), v.toObject().value("name").toString());
		spokenLanguages.clear();
		for (const QJsonValue c : o.value("transcription").toArray())
			spokenLanguages.append({names.value(c.toString(), c.toString()), c.toString()});
		std::sort(spokenLanguages.begin(), spokenLanguages.end());
		for (const auto &[name, code] : spokenLanguages)
			spokenBox->addItem(name, code);
		spokenBox->setCurrentIndex(qMax(0, spokenBox->findData(settings.spokenLanguage)));
		updateLiveSummary();
	});
}

// `choose`: the chosen broadcast's defaults are taken again (it was just
// made or edited). Otherwise only a broadcast chosen anew brings its
// defaults, so reopening the list keeps what the person set.
void Dock::loadBroadcasts(bool choose)
{
	broadcastsLoadedAt = QDateTime::currentMSecsSinceEpoch();
	api->get("/v1/broadcasts", [this, choose](int, const QJsonDocument &doc, const QString &error) {
		if (!error.isEmpty()) {
			obs_log(LOG_WARNING, "broadcasts: %s", error.toUtf8().constData());
			broadcastsFailed = true;
			showBroadcastLink();
			return;
		}
		broadcastsFailed = false;
		broadcastObjects.clear();
		broadcastOrder.clear();
		for (const QJsonValue v : doc.object().value("broadcasts").toArray()) {
			const QJsonObject b = v.toObject();
			if (b.value("archivedAt").isString() || b.value("lifecycle").toString() == "archived")
				continue;
			const QString id = b.value("id").toString();
			broadcastObjects.insert(id, b);
			broadcastOrder.append(id);
			const QUrl u(b.value("shareUrl").toString());
			if (!u.host().isEmpty())
				shareHost = u.host();
		}
		if (!settings.broadcastId.isEmpty() && !broadcastObjects.contains(settings.broadcastId)) {
			settings.broadcastId.clear(); // deleted or archived since
			settings.save();
		}
		fillBroadcasts();
		if (choose || settings.broadcastId != appliedBroadcastId)
			applyBroadcastDefaults(settings.broadcastId);
		showBroadcastLink();
		updateLinks();
	});
}

// The list: none, the broadcasts, and "New broadcast…" at its end. Left as
// it is when nothing in it changed (it may be open).
void Dock::fillBroadcasts()
{
	QString shown;
	for (const QString &id : broadcastOrder)
		shown += id + '\t' + broadcastObjects.value(id).value("name").toString() + '\n';
	if (shown == broadcastsShown && broadcastBox->findData(QString::fromLatin1(LOADING)) < 0) {
		broadcastBox->setCurrentIndex(qMax(0, broadcastBox->findData(settings.broadcastId)));
		return;
	}
	broadcastsShown = shown;
	const QSignalBlocker quiet(broadcastBox);
	broadcastBox->clear();
	broadcastBox->addItem(tr("No broadcast (save only)"), QString());
	for (const QString &id : broadcastOrder)
		broadcastBox->addItem(broadcastObjects.value(id).value("name").toString(), id);
	broadcastBox->insertSeparator(broadcastBox->count());
	broadcastBox->addItem(tr("+ New broadcast…"), QString::fromLatin1(NEW_BROADCAST));
	broadcastBox->setCurrentIndex(qMax(0, broadcastBox->findData(settings.broadcastId)));
}

// Under the list: the chosen broadcast's link (and Copy), or why there is
// none, with what to do about it.
void Dock::showBroadcastLink()
{
	auto say = [this](const QString &text, const char *level, const QString &action) {
		broadcastLink->setFullText(text);
		set_level(broadcastLink, level);
		copyBroadcastButton->hide();
		broadcastAction->setText(action);
		broadcastAction->setVisible(!action.isEmpty());
	};
	if (broadcastsFailed) {
		say(tr("Couldn't load broadcasts"), "error", tr("Retry"));
		return;
	}
	if (broadcastBox->findData(QString::fromLatin1(LOADING)) >= 0) {
		say(tr("Loading…"), "", QString());
		return;
	}
	if (broadcastOrder.isEmpty()) {
		say(tr("Listeners need a broadcast link to follow along."), "", tr("Create one"));
		return;
	}
	const QJsonObject b = broadcastObjects.value(settings.broadcastId);
	const QString url = b.value("shareUrl").toString();
	if (b.isEmpty() || url.isEmpty()) {
		say(tr("The transcript is saved; no one follows it live."), "", QString());
		return;
	}
	const QString bare = QString(url).remove(QRegularExpression("^https?://"));
	broadcastLink->setFullText(b.value("access").toString() == "private" ? tr("Private · %1").arg(bare) : bare);
	set_level(broadcastLink, "");
	broadcastAction->hide();
	copyBroadcastButton->show();
}

// The broadcast's own spoken language and speaker labels (and mode), as
// set in VocalStack, become the dock's. "None" changes nothing.
void Dock::applyBroadcastDefaults(const QString &id)
{
	appliedBroadcastId = id;
	if (id.isEmpty() || !broadcastObjects.contains(id))
		return;
	const QJsonObject s = broadcastObjects.value(id).value("settings").toObject();
	const QString lang = s.value("spokenLanguage").toString();
	const bool langChanged = lang != settings.spokenLanguage;
	settings.spokenLanguage = lang;
	if (s.value("speakerLabels").isString())
		settings.speakerLabels = s.value("speakerLabels").toString();
	if (s.value("latencyMode").isString())
		settings.latencyMode = s.value("latencyMode").toString();
	settings.save();
	spokenBox->setCurrentIndex(qMax(0, spokenBox->findData(settings.spokenLanguage)));
	speakersSwitch->setValue(settings.speakerLabels);
	updateLiveSummary();
	if (langChanged)
		warm->languageChanged();
}

// ---------------------------------------------------------------- broadcast editor

// A link made from a name, as the dashboard makes it: lower case, accents
// dropped, spaces to dashes, letters, digits and dashes only, at most 48.
static const int CODE_MAX = 48;
static QString slugify(const QString &title)
{
	QString r;
	for (const QChar c : title.trimmed().normalized(QString::NormalizationForm_D))
		if (c.category() != QChar::Mark_NonSpacing)
			r += c;
	r = r.toLower();
	r.replace(QRegularExpression("[\\s/]+"), "-");
	r.remove(QRegularExpression("[^a-z0-9-]"));
	r.replace(QRegularExpression("-+"), "-");
	if (r.size() > CODE_MAX) {
		r = r.left(CODE_MAX + 1);
		const int cut = int(r.lastIndexOf('-'));
		r = (cut >= 3 ? r.left(cut) : r.left(CODE_MAX));
		while (r.endsWith('-'))
			r.chop(1);
	}
	return r;
}

static const char *labels_note(const QString &value)
{
	if (value == "on")
		return QT_TR_NOOP("Each line in this transcript will be labelled with its speaker, even if only one person is talking.");
	if (value == "off")
		return QT_TR_NOOP("This transcript will be plain text with no speaker labels. Use this for a single speaker.");
	return QT_TR_NOOP("The voices in this recording are counted first. If there's more than one, each line is labelled "
			  "with its speaker; if not, the transcript is plain text.");
}

QWidget *Dock::buildBroadcastEditor()
{
	auto *page = new QWidget;
	auto *outer = new QVBoxLayout(page);
	outer->setContentsMargins(0, 0, 0, 0);
	outer->setSpacing(GAP_FIELD);
	outer->addWidget(pageHeader(&edTitle, &edSubtitle));
	new QShortcut(QKeySequence(Qt::Key_Escape), page, this, [this]() { goBack(); }, Qt::WidgetWithChildrenShortcut);

	auto *content = new QWidget;
	auto *v = new QVBoxLayout(content);
	v->setContentsMargins(0, 0, 4, 0);
	v->setSpacing(0);
	auto dirty = [this]() { editDirty = true; };

	edName = new QLineEdit;
	edName->setPlaceholderText(tr("A name for your broadcast…"));
	connect(edName, &QLineEdit::textEdited, this, [this, dirty]() {
		dirty();
		if (editingId.isEmpty() && !editCodeTouched)
			edCode->setText(slugify(edName->text()));
		updateBroadcastEditor();
	});
	ui::field(v, tr("Broadcast name"), edName);
	v->addSpacing(GAP_FIELD);

	// The link: the host, then the part that is the broadcast's own.
	auto *lv = new QVBoxLayout;
	lv->setSpacing(GAP_LABEL);
	edLinkEyebrow = ui::eyebrow(tr("Public link"));
	lv->addWidget(edLinkEyebrow);
	auto *linkRow = new QHBoxLayout;
	linkRow->setSpacing(0);
	edLinkPrefix = new ui::ElidedLabel;
	edLinkPrefix->setProperty("role", "muted");
	edCode = new QLineEdit;
	edCode->setPlaceholderText("my-broadcast-link");
	edCode->setMaxLength(CODE_MAX);
	connect(edCode, &QLineEdit::textEdited, this, [this, dirty](const QString &text) {
		dirty();
		edLinkNote->clear();
		edCode->setProperty("invalid", false);
		ui::repolish(edCode);
		const QString slug = slugify(text);
		editCodeTouched = !slug.isEmpty();
		if (!editCodeTouched && editingId.isEmpty())
			edCode->setText(slugify(edName->text()));
		updateBroadcastEditor();
	});
	linkRow->addWidget(edLinkPrefix);
	linkRow->addWidget(edCode, 1);
	lv->addLayout(linkRow);
	edLinkNote = ui::muted();
	edLinkNote->setTextFormat(Qt::RichText);
	// A suggestion offered for a link that is taken: a click takes it.
	connect(edLinkNote, &QLabel::linkActivated, this, [this](const QString &code) {
		edCode->setText(code);
		edCode->setProperty("invalid", false);
		ui::repolish(edCode);
		edLinkNote->clear();
		editCodeTouched = true;
		editDirty = true;
		edError->clear();
		updateBroadcastEditor();
	});
	lv->addWidget(edLinkNote);
	v->addLayout(lv);
	v->addSpacing(GAP_FIELD);

	auto *av = new QVBoxLayout;
	av->setSpacing(GAP_LABEL);
	av->addWidget(ui::eyebrow(tr("Visibility")));
	edAccess = new ui::Segmented;
	edAccess->addOption(tr("Public"), "public");
	edAccess->addOption(tr("Private"), "private");
	connect(edAccess, &ui::Segmented::changed, this, [this, dirty]() {
		dirty();
		updateBroadcastEditor();
	});
	av->addWidget(edAccess);
	edAccessNote = ui::muted();
	av->addWidget(edAccessNote);
	edPasswordBlock = new QWidget;
	auto *pwv = new QVBoxLayout(edPasswordBlock);
	pwv->setContentsMargins(0, 8, 0, 0);
	pwv->setSpacing(GAP_LABEL);
	pwv->addWidget(ui::eyebrow(tr("Password")));
	auto *pwRow = new QHBoxLayout;
	pwRow->setSpacing(4);
	edPassword = new QLineEdit;
	edPassword->setEchoMode(QLineEdit::Password);
	connect(edPassword, &QLineEdit::textEdited, this, [this, dirty]() {
		dirty();
		updateBroadcastEditor();
	});
	edPasswordShow = ui::linkButton(tr("Show"));
	connect(edPasswordShow, &QToolButton::clicked, this, [this]() {
		const bool hidden = edPassword->echoMode() == QLineEdit::Password;
		edPassword->setEchoMode(hidden ? QLineEdit::Normal : QLineEdit::Password);
		edPasswordShow->setText(hidden ? tr("Hide") : tr("Show"));
	});
	pwRow->addWidget(edPassword, 1);
	pwRow->addWidget(edPasswordShow);
	pwv->addLayout(pwRow);
	av->addWidget(edPasswordBlock);
	v->addLayout(av);
	v->addSpacing(GAP_FIELD);

	auto *sv = new QVBoxLayout;
	sv->setSpacing(GAP_LABEL);
	sv->addWidget(ui::eyebrow(tr("Default spoken language")));
	edSpokenMode = new ui::Segmented;
	edSpokenMode->addOption(tr("Auto detect"), "auto");
	edSpokenMode->addOption(tr("Custom"), "custom");
	sv->addWidget(edSpokenMode);
	edSpoken = new QComboBox;
	sv->addWidget(edSpoken);
	auto *spokenNote = ui::muted();
	sv->addWidget(spokenNote);
	auto spokenShown = [this, spokenNote]() {
		const bool custom = edSpokenMode->value().toString() == "custom";
		edSpoken->setVisible(custom);
		spokenNote->setText(custom ? tr("Every spoken segment is transcribed in this language.")
					   : tr("Each spoken segment gets its own detected language, so speakers can "
						"switch languages mid-conversation."));
	};
	connect(edSpokenMode, &ui::Segmented::changed, this, [spokenShown, dirty]() {
		dirty();
		spokenShown();
	});
	connect(edSpoken, &QComboBox::activated, this, dirty);
	spokenShown();
	v->addLayout(sv);
	v->addSpacing(GAP_FIELD);

	edLabels = new ui::Segmented;
	edLabels->addOption(tr("When needed"), "auto");
	edLabels->addOption(tr("On"), "on");
	edLabels->addOption(tr("Off"), "off");
	edLabelsNote = ui::muted();
	connect(edLabels, &ui::Segmented::changed, this, [this, dirty](const QVariant &value) {
		dirty();
		edLabelsNote->setText(tr(labels_note(value.toString())));
	});
	ui::field(v, tr("Default speaker labels"), edLabels, edLabelsNote);
	v->addStretch(1);
	outer->addWidget(scrolling(content), 1);

	// Save stays in sight below the scrolling form, with what it waits for.
	edError = new QLabel;
	edError->setWordWrap(true);
	auto *foot = new QVBoxLayout;
	foot->setSpacing(6);
	foot->addWidget(edError);
	edSave = new QPushButton;
	edSave->setProperty("role", "primary");
	connect(edSave, &QPushButton::clicked, this, &Dock::saveBroadcast);
	connect(edName, &QLineEdit::returnPressed, this, [this]() {
		if (edSave->isEnabled())
			saveBroadcast();
	});
	foot->addWidget(edSave);
	outer->addLayout(foot);
	return page;
}

// The editor filled from the broadcast with `id`, or empty for a new one.
void Dock::openBroadcastEditor(const QString &id)
{
	const QJsonObject b = broadcastObjects.value(id);
	editingId = b.isEmpty() ? QString() : id;
	const bool editing = !editingId.isEmpty();
	const QJsonObject bs = b.value("settings").toObject();
	editingOrigCode = b.value("code").toString();
	editWasPrivate = b.value("access").toString() == "private";
	editCodeTouched = false;
	edTitle->setText(editing ? tr("Edit broadcast") : tr("New broadcast"));
	edSubtitle->setText(editing ? QString() : tr("One link: attach as many live transcriptions as you like."));
	edSubtitle->setVisible(!editing);
	edSave->setText(editing ? tr("Save changes") : tr("Create broadcast"));
	edName->setText(b.value("name").toString());
	edLinkPrefix->setFullText((shareHost.isEmpty() ? QStringLiteral("live.vocalstack.com") : shareHost) + "/");
	edCode->setText(editingOrigCode);
	edCode->setProperty("invalid", false);
	ui::repolish(edCode);
	edAccess->setValue(editWasPrivate ? "private" : "public");
	edPassword->clear();
	edPassword->setEchoMode(QLineEdit::Password);
	edPasswordShow->setText(tr("Show"));
	edSpoken->clear();
	for (const auto &[name, code] : spokenLanguages)
		edSpoken->addItem(name, code);
	const QString lang = editing ? bs.value("spokenLanguage").toString() : settings.spokenLanguage;
	edSpokenMode->setValue(lang.isEmpty() ? "auto" : "custom");
	emit edSpokenMode->changed(edSpokenMode->value());
	edSpoken->setCurrentIndex(qMax(0, edSpoken->findData(lang.isEmpty() ? QStringLiteral("en") : lang)));
	const QString labels = bs.value("speakerLabels").toString(editing ? "auto" : settings.speakerLabels);
	edLabels->setValue(labels.isEmpty() ? "auto" : labels);
	edLabelsNote->setText(tr(labels_note(edLabels->value().toString())));
	ui::setRole(edError, "muted");
	set_level(edError, "");
	edError->clear();
	edLinkNote->clear();
	updateBroadcastEditor();
	editDirty = false;
	pages->setCurrentIndex(Editor);
	edName->setFocus();
}

// What is missing, said above Save; Save waits for it.
void Dock::updateBroadcastEditor()
{
	const bool isPrivate = edAccess->value().toString() == "private";
	const bool editing = !editingId.isEmpty();
	edLinkEyebrow->setText(isPrivate ? tr("Private link") : tr("Public link"));
	edPasswordBlock->setVisible(isPrivate);
	edPassword->setPlaceholderText(editing && editWasPrivate ? tr("Leave empty to keep the password")
								 : tr("At least three characters"));
	edAccessNote->setText(isPrivate ? tr("Listeners need the password to read the transcript.")
					: tr("Anyone with the link can access the broadcast."));

	const QString name = edName->text().trimmed();
	const QString code = edCode->text();
	const QString password = edPassword->text();
	QString why;
	if (name.isEmpty())
		why = tr("Name the broadcast; its link is made from the name.");
	else if (code.size() < 6)
		why = tr("The link needs at least six characters.");
	else if (isPrivate && password.size() < 3 && !(editing && editWasPrivate && password.isEmpty()))
		why = tr("A private broadcast needs a password of at least three characters.");
	edSave->setEnabled(why.isEmpty());
	edSave->setToolTip(why);
	// A server's refusal stays until the next try; otherwise, what Save
	// waits for.
	if (edError->property("level").toString() != "error") {
		ui::setRole(edError, "muted");
		edError->setText(why);
	}

	if (!edLinkNote->text().contains("<a ")) // a taken link's suggestions stay until one is picked
		edLinkNote->setText(code.size() > 0 && code.size() < 6
					    ? tr("Links need at least six characters.")
					    : tr("Share this link with your audience. The transcript and translations "
						 "appear there while you record."));
	set_level(edLinkNote, code.size() > 0 && code.size() < 6 ? "warn" : "");
	edPassword->setProperty("invalid", isPrivate && !password.isEmpty() && password.size() < 3);
	ui::repolish(edPassword);
}

void Dock::saveBroadcast()
{
	set_level(edError, "");
	updateBroadcastEditor();
	if (!edSave->isEnabled())
		return;
	const bool editing = !editingId.isEmpty();
	const QString name = edName->text().trimmed();
	const QString code = slugify(edCode->text());
	const bool isPrivate = edAccess->value().toString() == "private";
	const QString password = edPassword->text();
	const bool custom = edSpokenMode->value().toString() == "custom";
	QJsonObject bsettings{
		{"spokenLanguage", custom ? QJsonValue(edSpoken->currentData().toString()) : QJsonValue()},
		{"speakerLabels", edLabels->value().toString()},
	};
	QJsonObject body{{"name", name}, {"access", isPrivate ? "private" : "public"}};
	if (isPrivate && !password.isEmpty())
		body.insert("password", password);
	else if (!isPrivate)
		body.insert("password", QJsonValue());
	if (!editing || code != editingOrigCode)
		body.insert("code", code);
	if (!editing) {
		// As the dashboard makes one: titles, summaries and voices on.
		for (const char *f : {"summary", "title", "summaryTranslations", "voice"})
			bsettings.insert(f, true);
	}
	body.insert("settings", bsettings);
	const QString label = edSave->text();
	edSave->setEnabled(false);
	edSave->setText(editing ? tr("Saving…") : tr("Creating…"));
	auto done = [this, editing, label](int status, const QJsonDocument &doc, const QString &error) {
		edSave->setEnabled(true);
		edSave->setText(label);
		if (!error.isEmpty()) {
			if (status == 409) {
				// Said by the link itself, with the server's suggestions.
				QStringList links;
				for (const QJsonValue v : Api::errorDetails(doc).value("suggestions").toArray())
					links.append(anchor(this, v.toString(), v.toString()));
				edLinkNote->setText(links.isEmpty() ? tr("That link is taken. Pick another.")
								    : tr("That link is taken. Try %1").arg(links.join(", ")));
				set_level(edLinkNote, "warn");
				edCode->setProperty("invalid", true);
				ui::repolish(edCode);
				edError->clear();
			} else {
				ui::setRole(edError, "");
				edError->setText(editing ? tr("Could not save the broadcast: %1").arg(error)
							 : tr("Could not create the broadcast: %1").arg(error));
				set_level(edError, "error");
			}
			return;
		}
		settings.broadcastId = doc.object().value("id").toString();
		settings.save();
		editDirty = false;
		pages->setCurrentIndex(Main);
		loadBroadcasts(true); // and its defaults become the dock's
	};
	if (editing)
		api->patch("/v1/broadcasts/" + editingId, body, done);
	else
		api->post("/v1/broadcasts", body, QUuid::createUuid().toString(QUuid::WithoutBraces), done);
}

// ---------------------------------------------------------------- OBS

void Dock::shutdown()
{
	updater->finishOnExit();
	warm->end();
	login->cancel();
	session->stop();
}

void Dock::showFirstTime(QMainWindow *mainWindow)
{
	if (settings.dockShown)
		return;
	if (auto *dw = qobject_cast<QDockWidget *>(parentWidget())) {
		dw->setFloating(false);
		mainWindow->addDockWidget(Qt::RightDockWidgetArea, dw);
		dw->show();
	}
	settings.dockShown = true;
	settings.save();
}

void Dock::watchClose(QWidget *mainWindow)
{
	mainWindow->installEventFilter(this);
}

// Closing OBS while live: the close waits (at most a few seconds) for the
// recording to stop, or it would stay live until VocalStack noticed the
// stream was gone. OBS holds its own close the same way.
bool Dock::eventFilter(QObject *watched, QEvent *event)
{
	// Edit stays exactly as tall as the list beside it.
	if (watched == broadcastBox) {
		if (event->type() == QEvent::Resize || event->type() == QEvent::Show)
			editBroadcastButton->setFixedHeight(broadcastBox->height());
		return QWidget::eventFilter(watched, event);
	}
	if (event->type() != QEvent::Close || session->state() == Session::State::Idle || closeHeld)
		return QWidget::eventFilter(watched, event);
	closeHeld = true;
	event->ignore();
	auto *mainWindow = static_cast<QWidget *>(watched);
	auto closeAgain = [mainWindow]() { QMetaObject::invokeMethod(mainWindow, "close", Qt::QueuedConnection); };
	QTimer::singleShot(5000, this, closeAgain);
	connect(session, &Session::stateChanged, this, [closeAgain](Session::State s) {
		if (s == Session::State::Idle)
			closeAgain();
	});
	session->stop();
	return true;
}
