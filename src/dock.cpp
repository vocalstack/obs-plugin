#include "dock.hpp"

#include "api.hpp"
#include "caption-overlay.hpp"
#include "connect-login.hpp"
#include "ui.hpp"
#include "updater.hpp"
#include "warm-ping.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDockWidget>
#include <QEvent>
#include <QFontDatabase>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHostInfo>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

#include <initializer_list>

using ui::GAP_FIELD;
using ui::GAP_LABEL;
using ui::GAP_SECTION;

// Where an update is read about and downloaded by hand: the plugin's page
// on the VocalStack website.
static const char *PLUGIN_PAGE = "https://vocalstack.com/obs-studio-transcription-plugin/";
// The broadcast list's last entry, which opens the editor for a new one.
static const char *NEW_BROADCAST = "__new__";
// The list is asked for again when it opens, at most this often.
static const qint64 BROADCASTS_FRESH_MS = 10000;

// The VocalStack logo, at 2x for sharp screens: white on OBS's dark themes,
// the dark one on a light theme.
static QLabel *logo(QWidget *parent)
{
	const bool dark = parent->palette().window().color().lightness() < 128;
	char *path = obs_module_file(dark ? "logo-white@2x.png" : "logo@2x.png");
	QPixmap pm(QString::fromUtf8(path ? path : ""));
	bfree(path);
	auto *label = new QLabel(parent);
	if (!pm.isNull()) {
		pm.setDevicePixelRatio(2);
		label->setPixmap(pm);
	}
	label->setToolTip(QStringLiteral("VocalStack"));
	return label;
}

// A link inside rich text, in the theme's brand colour.
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
	head->addWidget(logo(this), 0, Qt::AlignVCenter);
	head->addStretch(1);
	auto *dashboard = new QPushButton(tr("Dashboard ↗"));
	dashboard->setProperty("role", "secondary");
	dashboard->setToolTip(tr("Open the VocalStack dashboard"));
	connect(dashboard, &QPushButton::clicked, this, [this]() { QDesktopServices::openUrl(QUrl(api->appBase())); });
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

// One stylesheet for the dock, from the theme OBS is in; again when OBS's
// theme changes between dark and light.
void Dock::applyTheme()
{
	const ui::Theme theme = ui::Theme::of(this);
	darkTheme = theme.dark;
	setStyleSheet(theme.stylesheet());
	followHint->setText(tr("Starts when OBS starts streaming or recording · %1")
				    .arg(anchor(this, "settings", tr("Change"))));
}

void Dock::changeEvent(QEvent *event)
{
	QWidget::changeEvent(event);
	if (event->type() == QEvent::PaletteChange && ui::Theme::of(this).dark != darkTheme)
		applyTheme();
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
	auto *h = new QHBoxLayout(w);
	h->setContentsMargins(0, 0, 0, 0);
	h->setSpacing(6);
	auto *back = new QToolButton;
	back->setText(QStringLiteral("‹"));
	back->setToolTip(tr("Back"));
	back->setAutoRaise(true);
	back->setFixedSize(28, 28);
	QFont bf = back->font();
	bf.setPointSizeF(bf.pointSizeF() * 1.6);
	back->setFont(bf);
	connect(back, &QToolButton::clicked, this, [this]() { pages->setCurrentIndex(Main); });
	h->addWidget(back, 0, Qt::AlignTop);
	auto *texts = new QVBoxLayout;
	texts->setSpacing(2);
	*titleLabel = ui::title(QString());
	texts->addWidget(*titleLabel);
	if (subtitleLabel) {
		*subtitleLabel = ui::muted();
		texts->addWidget(*subtitleLabel);
	}
	h->addLayout(texts, 1);
	return w;
}

// ---------------------------------------------------------------- signed out

QWidget *Dock::buildSignedOut()
{
	auto *page = new QWidget;
	auto *v = new QVBoxLayout(page);
	v->setContentsMargins(0, 0, 0, 0);
	v->setSpacing(0);

	v->addWidget(ui::title(tr("Live captions for your stream")));
	v->addSpacing(GAP_LABEL);
	v->addWidget(ui::muted(tr("Transcribe and translate live with VocalStack; captions show over the preview.")));
	v->addSpacing(GAP_SECTION);

	signInButton = new QPushButton(tr("Sign in with VocalStack"));
	signInButton->setProperty("role", "primary");
	connect(signInButton, &QPushButton::clicked, this, [this]() {
		setSigningIn(true);
		login->start();
	});
	v->addWidget(signInButton);
	v->addSpacing(6);

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
								  tr("Finish signing in in your browser…")));
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

	auto *join = ui::muted();
	join->setTextFormat(Qt::RichText);
	connect(join, &QLabel::linkActivated, this, [this]() { QDesktopServices::openUrl(QUrl(api->appBase())); });
	join->setText(tr("No account? %1").arg(anchor(this, "join", tr("Create one free"))));
	v->addWidget(join);
	v->addStretch(1);
	return page;
}

void Dock::setSigningIn(bool waiting)
{
	signInButton->setEnabled(!waiting);
	cancelSignIn->setVisible(waiting);
	set_level(signInStatus, "");
	if (waiting) {
		signInFrame = 0;
		signInStatus->setText(QStringLiteral("%1  %2").arg(ui::SPINNER.at(0), tr("Finish signing in in your browser…")));
		signInSpin->start();
	} else {
		signInSpin->stop();
	}
}

// ---------------------------------------------------------------- main page

QWidget *Dock::buildSignedIn()
{
	auto *page = new QWidget;
	auto *v = new QVBoxLayout(page);
	v->setContentsMargins(0, 0, 0, 0);
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
	connect(broadcastBox, &ui::ReloadingCombo::aboutToShowPopup, this, [this]() {
		if (QDateTime::currentMSecsSinceEpoch() - broadcastsLoadedAt > BROADCASTS_FRESH_MS)
			loadBroadcasts();
	});
	connect(broadcastBox, &QComboBox::activated, this, [this](int index) {
		const QString id = broadcastBox->itemData(index).toString();
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
	editBroadcastButton = new QToolButton;
	editBroadcastButton->setText(QStringLiteral("✎"));
	editBroadcastButton->setToolTip(tr("Edit broadcast"));
	editBroadcastButton->setProperty("role", "icon");
	connect(editBroadcastButton, &QToolButton::clicked, this,
		[this]() { openBroadcastEditor(settings.broadcastId); });
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
	linkRow->addWidget(broadcastLink, 1);
	linkRow->addWidget(copyBroadcastButton);
	bv->addLayout(linkRow);
	v->addLayout(bv);
	v->addSpacing(GAP_FIELD);

	spokenBox = new QComboBox;
	connect(spokenBox, &QComboBox::activated, this, [this]() {
		const QString lang = spokenBox->currentData().toString();
		const bool changed = lang != settings.spokenLanguage;
		settings.spokenLanguage = lang;
		settings.save();
		if (changed)
			warm->languageChanged();
	});
	ui::field(v, tr("Spoken language"), spokenBox);
	v->addSpacing(GAP_FIELD);

	sendSwitch = new ui::Segmented;
	sendSwitch->addOption(tr("Audio only"), false);
	sendSwitch->addOption(tr("Audio + video"), true);
	sendSwitch->setValue(settings.sendVideo);
	connect(sendSwitch, &ui::Segmented::changed, this, [this](const QVariant &value) {
		settings.sendVideo = value.toBool();
		settings.save();
	});
	ui::field(v, tr("Send to VocalStack"), sendSwitch);
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

	// Says the button need not be pressed when OBS will start it.
	followHint = ui::muted();
	followHint->setTextFormat(Qt::RichText);
	connect(followHint, &QLabel::linkActivated, this, [this]() { pages->setCurrentIndex(SettingsPage); });
	v->addSpacing(6);
	v->addWidget(followHint);

	// After a session: how it ended.
	idleStatus = new QLabel;
	idleStatus->setWordWrap(true);
	idleStatus->setTextFormat(Qt::RichText);
	idleStatus->setOpenExternalLinks(true);
	idleStatus->hide();
	v->addSpacing(6);
	v->addWidget(idleStatus);

	// While live: a card under Stop.
	auto *card = new QFrame;
	card->setProperty("role", "card");
	liveCard = card;
	auto *grid = new QGridLayout(card);
	grid->setContentsMargins(12, 10, 10, 10);
	grid->setHorizontalSpacing(6);
	grid->setVerticalSpacing(4);
	liveDot = new QLabel(QStringLiteral("●"));
	liveDot->setProperty("role", "dot");
	liveWord = ui::eyebrow(tr("Live"));
	liveClock = new QLabel;
	liveClock->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
	liveListeners = ui::muted();
	liveListeners->setWordWrap(false);
	liveListeners->hide();
	captionsToggle = new QToolButton;
	captionsToggle->setText(QStringLiteral("CC"));
	captionsToggle->setCheckable(true);
	captionsToggle->setChecked(settings.showCaptions);
	captionsToggle->setAutoRaise(true);
	captionsToggle->setToolTip(tr("Show captions over the preview"));
	connect(captionsToggle, &QToolButton::toggled, this, [this](bool on) {
		settings.showCaptions = on;
		if (!on && overlay)
			overlay->setCaption(QString(), QString());
	});
	auto *top = new QHBoxLayout;
	top->setSpacing(6);
	top->addWidget(liveDot);
	top->addWidget(liveWord);
	top->addWidget(liveClock);
	top->addStretch(1);
	top->addWidget(liveListeners);
	top->addWidget(captionsToggle);
	grid->addLayout(top, 0, 0);
	liveStatus = new QLabel;
	liveStatus->setWordWrap(true);
	liveStatus->setTextFormat(Qt::RichText);
	liveStatus->setOpenExternalLinks(true);
	grid->addWidget(liveStatus, 1, 0);
	card->hide();
	v->addSpacing(8);
	v->addWidget(card);
	liveTick = new QTimer(this);
	liveTick->setInterval(500);
	connect(liveTick, &QTimer::timeout, this, &Dock::tickLive);

	// VocalStack's warnings while live, one callout each.
	noticesBox = new QVBoxLayout;
	noticesBox->setSpacing(6);
	noticesBox->setContentsMargins(0, 8, 0, 0);
	v->addLayout(noticesBox);

	// The transcription in VocalStack: open it, or share it.
	transcriptBlock = new QWidget;
	auto *tv = new QVBoxLayout(transcriptBlock);
	tv->setContentsMargins(0, GAP_SECTION, 0, 0);
	tv->setSpacing(GAP_LABEL);
	transcriptEyebrow = ui::eyebrow(tr("Last transcript"));
	tv->addWidget(transcriptEyebrow);
	auto *links = new QHBoxLayout;
	openButton = new QPushButton(tr("Open transcript ↗"));
	openButton->setProperty("role", "secondary");
	connect(openButton, &QPushButton::clicked, this, &Dock::openTranscription);
	copyLinkButton = new QPushButton(tr("Share transcript"));
	copyLinkButton->setProperty("role", "secondary");
	copyLinkButton->setToolTip(tr("Copy a link anyone can read the transcription with"));
	connect(copyLinkButton, &QPushButton::clicked, this, &Dock::copyTranscriptLink);
	links->addWidget(openButton, 1);
	links->addWidget(copyLinkButton, 1);
	tv->addLayout(links);
	v->addWidget(transcriptBlock);
	v->addStretch(1);

	// A newer release: how it is going, and Update now when it does not
	// install by itself.
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
	v->addSpacing(GAP_FIELD);
	v->addWidget(updateCallout);
	v->addSpacing(8);

	// Who is signed in, and the settings.
	auto *foot = new QHBoxLayout;
	auto *account = new ui::ElidedLabel;
	account->setProperty("role", "muted");
	accountLabel = account;
	auto *settingsButton = ui::linkButton(tr("⚙ Settings"));
	connect(settingsButton, &QToolButton::clicked, this, [this]() { pages->setCurrentIndex(SettingsPage); });
	foot->addWidget(account, 1);
	foot->addWidget(settingsButton);
	v->addLayout(foot);
	return page;
}

// ---------------------------------------------------------------- settings

QWidget *Dock::buildSettings()
{
	auto *page = new QWidget;
	auto *outer = new QVBoxLayout(page);
	outer->setContentsMargins(0, 0, 0, 0);
	outer->setSpacing(GAP_SECTION);
	QLabel *heading;
	outer->addWidget(pageHeader(&heading, nullptr));
	heading->setText(tr("Settings"));

	auto *content = new QWidget;
	auto *v = new QVBoxLayout(content);
	v->setContentsMargins(0, 0, 4, 0);
	v->setSpacing(0);

	// Transcription.
	nameEdit = new QLineEdit;
	nameEdit->setPlaceholderText(tr("OBS on %1 (optional)").arg(QHostInfo::localHostName()));
	nameEdit->setText(settings.transcriptionName);
	connect(nameEdit, &QLineEdit::editingFinished, this, [this]() {
		settings.transcriptionName = nameEdit->text().trimmed().left(200);
		settings.save();
	});
	ui::field(v, tr("Transcription name"), nameEdit, ui::muted(tr("What the transcription is called in VocalStack.")));
	v->addSpacing(GAP_FIELD);
	lockBox = new QCheckBox(tr("Lock the transcription against deletion"));
	lockBox->setChecked(settings.locked);
	connect(lockBox, &QCheckBox::toggled, this, [this](bool on) {
		settings.locked = on;
		settings.save();
	});
	v->addWidget(lockBox);
	v->addSpacing(GAP_LABEL);
	v->addWidget(ui::muted(tr("It can't be deleted from the dashboard until it is unlocked.")));
	v->addSpacing(GAP_SECTION);

	speakersSwitch = new ui::Segmented;
	speakersSwitch->addOption(tr("When needed"), "auto");
	speakersSwitch->addOption(tr("On"), "on");
	speakersSwitch->addOption(tr("Off"), "off");
	speakersSwitch->setValue(settings.speakerLabels.isEmpty() ? "auto" : settings.speakerLabels);
	connect(speakersSwitch, &ui::Segmented::changed, this, [this](const QVariant &value) {
		settings.speakerLabels = value.toString();
		settings.save();
	});
	ui::field(v, tr("Speaker labels"), speakersSwitch,
		  ui::muted(tr("Follows the broadcast's default; a change here lasts until you choose another broadcast.")));
	v->addSpacing(GAP_SECTION);

	followBox = new QCheckBox(tr("Start and stop with OBS streaming or recording"));
	followBox->setChecked(settings.followObs);
	connect(followBox, &QCheckBox::toggled, this, [this](bool on) {
		settings.followObs = on;
		settings.save();
		updateFollowHint();
	});
	ui::field(v, tr("OBS"), followBox);
	v->addSpacing(GAP_SECTION);

	// Updates.
	auto *uv = new QVBoxLayout;
	uv->setSpacing(6);
	uv->addWidget(ui::eyebrow(tr("Updates")));
	autoUpdateBox = new QCheckBox(tr("Install updates automatically"));
	autoUpdateBox->setToolTip(tr("A newer release is installed while nothing is being transcribed; restart OBS to use it"));
	autoUpdateBox->setChecked(settings.autoUpdate);
	connect(autoUpdateBox, &QCheckBox::toggled, this, [this](bool on) {
		settings.autoUpdate = on;
		settings.save();
		showUpdate();
	});
	uv->addWidget(autoUpdateBox);
	checkUpdatesButton = new QPushButton(tr("Check for updates"));
	checkUpdatesButton->setProperty("role", "secondary");
	connect(checkUpdatesButton, &QPushButton::clicked, this, [this]() {
		checkWanted = true;
		checkUpdatesButton->setEnabled(false);
		checkUpdatesResult->setText(tr("Checking…"));
		updater->check(true);
		if (updater->step() != Updater::Step::Checking)
			showUpdate(); // busy downloading or installing: say so now
	});
	uv->addWidget(checkUpdatesButton, 0, Qt::AlignLeft);
	checkUpdatesResult = ui::muted();
	uv->addWidget(checkUpdatesResult);
	uv->addWidget(ui::muted(tr("Version %1").arg(PLUGIN_VERSION)));
	v->addLayout(uv);
	v->addSpacing(GAP_SECTION);

	// Account.
	auto *av = new QVBoxLayout;
	av->setSpacing(6);
	av->addWidget(ui::eyebrow(tr("Account")));
	settingsAccount = new QLabel;
	settingsAccount->setWordWrap(true);
	av->addWidget(settingsAccount);
	auto *signOutButton = new QPushButton(tr("Sign out"));
	signOutButton->setProperty("role", "secondary");
	connect(signOutButton, &QPushButton::clicked, this, [this]() { signOut(QString()); });
	av->addWidget(signOutButton, 0, Qt::AlignLeft);
	v->addLayout(av);
	v->addStretch(1);

	outer->addWidget(scrolling(content), 1);
	return page;
}

// ---------------------------------------------------------------- state

void Dock::setOverlay(CaptionOverlay *o)
{
	overlay = o;
}

void Dock::onStatus(const QString &text, Session::Level level)
{
	if (level != Session::Level::Ok)
		obs_log(LOG_WARNING, "%s", text.toUtf8().constData());
	if (session->state() == Session::State::Idle) {
		idleStatus->setText(text);
		set_level(idleStatus, level_name(level));
		idleStatus->setVisible(!text.isEmpty());
		return;
	}
	liveStatus->setText(text);
	set_level(liveStatus, level_name(level));
	liveLevel = level;
	tickLive();
}

// The live card's dot (coral and pulsing when all is well, amber while it
// connects or warns, red on a failure) and the time since going live.
void Dock::tickLive()
{
	const Session::State s = session->state();
	liveTicks++;
	const char *dot = "live";
	if (s == Session::State::Starting || liveLevel == Session::Level::Warn)
		dot = "warn";
	else if (liveLevel == Session::Level::Error)
		dot = "error";
	else if (liveTicks % 2)
		dot = "dim";
	set_level(liveDot, dot);
	liveWord->setText(s == Session::State::Starting ? tr("Starting") : s == Session::State::Stopping ? tr("Stopping") : tr("Live"));
	if (s == Session::State::Live && liveSince.isValid()) {
		const qint64 secs = liveSince.elapsed() / 1000;
		liveClock->setText(QStringLiteral("%1:%2:%3")
					   .arg(secs / 3600, 2, 10, QChar('0'))
					   .arg((secs / 60) % 60, 2, 10, QChar('0'))
					   .arg(secs % 60, 2, 10, QChar('0')));
		liveClock->show();
	} else {
		liveClock->hide();
	}
}

void Dock::showNotices(const QStringList &notes)
{
	currentNotices = notes;
	// Later, not now: a callout's own ✕ may be what called this.
	while (QLayoutItem *item = noticesBox->takeAt(0)) {
		if (QWidget *w = item->widget()) {
			w->hide();
			w->deleteLater();
		}
		delete item;
	}
	for (const QString &note : notes) {
		if (dismissedNotices.contains(note))
			continue;
		obs_log(LOG_WARNING, "%s", note.toUtf8().constData());
		auto *c = new ui::Callout;
		c->setTone("warn");
		c->setText(note.toHtmlEscaped());
		connect(c, &ui::Callout::dismissed, this, [this, note]() {
			dismissedNotices.append(note);
			showNotices(currentNotices);
		});
		noticesBox->addWidget(c);
	}
}

void Dock::onState(Session::State s)
{
	const bool idle = s == Session::State::Idle;
	const bool stopping = s == Session::State::Stopping;
	if (stopping) {
		stopFrame = 0;
		startButton->setText(QStringLiteral("%1  %2").arg(ui::SPINNER.at(0), tr("Stopping…")));
		stopSpin->start();
	} else {
		stopSpin->stop();
		startButton->setText(idle ? tr("Start transcribing") : tr("■  Stop transcribing"));
	}
	ui::setRole(startButton, idle ? "primary" : "danger");
	startButton->setEnabled(!stopping);

	if (s == Session::State::Starting) {
		dismissedNotices.clear();
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

	for (QWidget *w : std::initializer_list<QWidget *>{broadcastBox, spokenBox, sendSwitch, editBroadcastButton,
							  speakersSwitch, lockBox, nameEdit})
		w->setEnabled(idle);
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

// The transcript buttons once there is a transcript; the broadcast's own
// buttons when one is chosen.
void Dock::updateLinks()
{
	const bool idle = session->state() == Session::State::Idle;
	transcriptBlock->setVisible(!settings.transcriptionId.isEmpty());
	transcriptEyebrow->setVisible(idle);
	const bool chosen = broadcastObjects.contains(settings.broadcastId);
	editBroadcastButton->setEnabled(chosen && idle);
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
				  obs_log(LOG_WARNING, "share link: %s", error.toUtf8().constData());
				  ui::flash(copyLinkButton, tr("Couldn't make a link"), 2500);
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
	// The answer to "Check for updates", once the check is done.
	if (checkWanted && step != Updater::Step::Checking) {
		checkWanted = false;
		checkUpdatesButton->setEnabled(true);
		if (!updater->checkError().isEmpty())
			checkUpdatesResult->setText(tr("Could not check: %1").arg(updater->checkError()));
		else if (step == Updater::Step::Installed || step == Updater::Step::OnExit)
			checkUpdatesResult->setText(tr("%1 is installed; restart OBS to use it.").arg(latest));
		else if (step != Updater::Step::Idle)
			checkUpdatesResult->setText(tr("Updating to %1…").arg(latest));
		else if (age == Updater::Age::Current)
			checkUpdatesResult->setText(tr("You have the latest version (%1).").arg(PLUGIN_VERSION));
		else
			checkUpdatesResult->setText(tr("Version %1 is out.").arg(latest));
	}
	QString text = updater->message().toHtmlEscaped();
	bool problem = updater->problem();
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
			text = tr("VocalStack for OBS %1 is out; this version (%2) is no longer supported. Update to transcribe.")
				       .arg(latest, PLUGIN_VERSION);
			problem = true;
			break;
		case Updater::Age::Current:
			break;
		}
		text += " " + anchor(this, PLUGIN_PAGE, tr("Download"));
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
	// An unsupported version does not start.
	const bool blocked = age == Updater::Age::Unsupported && step != Updater::Step::Installed;
	startButton->setEnabled(!blocked && session->state() != Session::State::Stopping);
}

// ---------------------------------------------------------------- sign-in

void Dock::showSignedIn(bool in)
{
	pages->setCurrentIndex(in ? Main : SignedOut);
	if (!in)
		return;
	const QString who = settings.account.isEmpty() ? tr("an API key") : settings.account;
	static_cast<ui::ElidedLabel *>(accountLabel)->setFullText(settings.account.isEmpty() ? tr("Signed in with an API key") : settings.account);
	settingsAccount->setText(tr("Signed in to <b>%1</b>").arg(who.toHtmlEscaped()));
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
		if (!error.isEmpty())
			return;
		const QJsonObject o = doc.object();
		QHash<QString, QString> names;
		for (const QJsonValue v : o.value("languages").toArray())
			names.insert(v.toObject().value("code").toString(), v.toObject().value("name").toString());
		spokenLanguages.clear();
		for (const QJsonValue c : o.value("transcription").toArray())
			spokenLanguages.append({names.value(c.toString(), c.toString()), c.toString()});
		std::sort(spokenLanguages.begin(), spokenLanguages.end());
		spokenBox->clear();
		spokenBox->addItem(tr("Auto detect"), QString());
		for (const auto &[name, code] : spokenLanguages)
			spokenBox->addItem(name, code);
		spokenBox->setCurrentIndex(qMax(0, spokenBox->findData(settings.spokenLanguage)));
	});
}

void Dock::loadBroadcasts()
{
	broadcastsLoadedAt = QDateTime::currentMSecsSinceEpoch();
	api->get("/v1/broadcasts", [this](int, const QJsonDocument &doc, const QString &error) {
		if (!error.isEmpty()) {
			onStatus(tr("Could not load the broadcasts: %1").arg(error), Session::Level::Error);
			return;
		}
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
		applyBroadcastDefaults(settings.broadcastId);
		showBroadcastLink();
		updateLinks();
	});
}

// The list: None, the broadcasts, and "New broadcast…" at its end.
void Dock::fillBroadcasts()
{
	const QSignalBlocker quiet(broadcastBox);
	broadcastBox->clear();
	broadcastBox->addItem(tr("None"), QString());
	for (const QString &id : broadcastOrder)
		broadcastBox->addItem(broadcastObjects.value(id).value("name").toString(), id);
	broadcastBox->insertSeparator(broadcastBox->count());
	broadcastBox->addItem(tr("+ New broadcast…"), QString::fromLatin1(NEW_BROADCAST));
	broadcastBox->setCurrentIndex(qMax(0, broadcastBox->findData(settings.broadcastId)));
}

// The chosen broadcast's link under the list, with a lock when private.
void Dock::showBroadcastLink()
{
	const QJsonObject b = broadcastObjects.value(settings.broadcastId);
	const QString url = b.value("shareUrl").toString();
	if (b.isEmpty() || url.isEmpty()) {
		broadcastLink->setFullText(settings.broadcastId.isEmpty() ? tr("No broadcast: the transcript is only saved")
									  : QString());
		copyBroadcastButton->hide();
		return;
	}
	const QString bare = QString(url).remove(QRegularExpression("^https?://"));
	broadcastLink->setFullText(b.value("access").toString() == "private" ? QStringLiteral("🔒 ") + bare : bare);
	copyBroadcastButton->show();
}

// The broadcast's own spoken language and speaker labels (and mode), as
// set in VocalStack, become the dock's. "None" changes nothing.
void Dock::applyBroadcastDefaults(const QString &id)
{
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
	outer->setSpacing(GAP_SECTION);
	outer->addWidget(pageHeader(&edTitle, &edSubtitle));

	auto *content = new QWidget;
	auto *v = new QVBoxLayout(content);
	v->setContentsMargins(0, 0, 4, 0);
	v->setSpacing(0);

	edName = new QLineEdit;
	edName->setPlaceholderText(tr("A name for your broadcast…"));
	connect(edName, &QLineEdit::textEdited, this, [this]() {
		if (editingId.isEmpty() && !editCodeTouched)
			edCode->setText(slugify(edName->text()));
		updateBroadcastEditor();
	});
	edNameNote = ui::muted();
	edNameNote->hide();
	ui::field(v, tr("Broadcast name"), edName, edNameNote);
	v->addSpacing(GAP_FIELD);

	auto *lv = new QVBoxLayout;
	lv->setSpacing(GAP_LABEL);
	lv->addWidget(ui::eyebrow(tr("Link")));
	edLinkPrefix = ui::muted();
	lv->addWidget(edLinkPrefix);
	edCode = new QLineEdit;
	edCode->setPlaceholderText("my-broadcast-link");
	edCode->setMaxLength(CODE_MAX);
	connect(edCode, &QLineEdit::textEdited, this, [this](const QString &text) {
		edLinkNote->clear();
		edCode->setProperty("invalid", false);
		ui::repolish(edCode);
		const QString slug = slugify(text);
		editCodeTouched = !slug.isEmpty();
		if (!editCodeTouched && editingId.isEmpty())
			edCode->setText(slugify(edName->text()));
		updateBroadcastEditor();
	});
	lv->addWidget(edCode);
	edLinkNote = ui::muted();
	edLinkNote->setTextFormat(Qt::RichText);
	// A suggestion offered for a link that is taken: a click takes it.
	connect(edLinkNote, &QLabel::linkActivated, this, [this](const QString &code) {
		edCode->setText(code);
		edCode->setProperty("invalid", false);
		ui::repolish(edCode);
		edLinkNote->clear();
		editCodeTouched = true;
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
	connect(edAccess, &ui::Segmented::changed, this, [this]() { updateBroadcastEditor(); });
	av->addWidget(edAccess);
	edAccessNote = ui::muted();
	av->addWidget(edAccessNote);
	edPassword = new QLineEdit;
	edPassword->setEchoMode(QLineEdit::Password);
	connect(edPassword, &QLineEdit::textEdited, this, [this]() { updateBroadcastEditor(); });
	av->addWidget(edPassword);
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
	connect(edSpokenMode, &ui::Segmented::changed, this, spokenShown);
	connect(edSpoken, &QComboBox::activated, this, [this]() { updateBroadcastEditor(); });
	spokenShown();
	v->addLayout(sv);
	v->addSpacing(GAP_FIELD);

	edLabels = new ui::Segmented;
	edLabels->addOption(tr("When needed"), "auto");
	edLabels->addOption(tr("On"), "on");
	edLabels->addOption(tr("Off"), "off");
	edLabelsNote = ui::muted();
	connect(edLabels, &ui::Segmented::changed, this,
		[this](const QVariant &value) { edLabelsNote->setText(tr(labels_note(value.toString()))); });
	ui::field(v, tr("Default speaker labels"), edLabels, edLabelsNote);
	v->addStretch(1);
	outer->addWidget(scrolling(content), 1);

	// Save stays in sight below the scrolling form.
	edError = new QLabel;
	edError->setWordWrap(true);
	edError->setProperty("level", "error");
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
	edSubtitle->setText(editing ? editingOrigCode : tr("One link — attach as many live transcriptions as you like"));
	edSave->setText(editing ? tr("Save changes") : tr("Create broadcast"));
	edName->setText(b.value("name").toString());
	edLinkPrefix->setText((shareHost.isEmpty() ? QStringLiteral("live.vocalstack.com") : shareHost) + "/");
	edCode->setText(editingOrigCode);
	edCode->setProperty("invalid", false);
	ui::repolish(edCode);
	edAccess->setValue(editWasPrivate ? "private" : "public");
	edPassword->clear();
	edSpoken->clear();
	for (const auto &[name, code] : spokenLanguages)
		edSpoken->addItem(name, code);
	const QString lang = editing ? bs.value("spokenLanguage").toString() : settings.spokenLanguage;
	edSpokenMode->setValue(lang.isEmpty() ? "auto" : "custom");
	edSpokenMode->changed(edSpokenMode->value());
	edSpoken->setCurrentIndex(qMax(0, edSpoken->findData(lang.isEmpty() ? QStringLiteral("en") : lang)));
	const QString labels = bs.value("speakerLabels").toString(editing ? "auto" : settings.speakerLabels);
	edLabels->setValue(labels.isEmpty() ? "auto" : labels);
	edLabelsNote->setText(tr(labels_note(edLabels->value().toString())));
	edError->clear();
	edLinkNote->clear();
	updateBroadcastEditor();
	pages->setCurrentIndex(Editor);
	edName->setFocus();
}

// What is missing, said by the field at fault; Save waits for it.
void Dock::updateBroadcastEditor()
{
	const bool isPrivate = edAccess->value().toString() == "private";
	const bool editing = !editingId.isEmpty();
	edPassword->setVisible(isPrivate);
	edPassword->setPlaceholderText(editing && editWasPrivate ? tr("Leave empty to keep the password")
								 : tr("A password, at least three characters"));
	edAccessNote->setText(isPrivate ? tr("Listeners need the password to read the transcript.")
					: tr("Anyone with the link can access the broadcast."));

	const QString name = edName->text().trimmed();
	const QString code = edCode->text();
	const QString password = edPassword->text();
	QString why;
	if (name.isEmpty())
		why = tr("Name the broadcast first; its link is made from the name.");
	else if (code.size() < 6)
		why = tr("The link needs at least six characters.");
	else if (isPrivate && password.size() < 3 && !(editing && editWasPrivate && password.isEmpty()))
		why = tr("A private broadcast needs a password of at least three characters.");
	edSave->setEnabled(why.isEmpty());
	edSave->setToolTip(why);

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
	edSave->setEnabled(false);
	edError->clear();
	auto done = [this, editing](int status, const QJsonDocument &doc, const QString &error) {
		edSave->setEnabled(true);
		if (!error.isEmpty()) {
			const QJsonArray suggestions = Api::errorDetails(doc).value("suggestions").toArray();
			if (status == 409) {
				QStringList links;
				for (const QJsonValue v : suggestions)
					links.append(anchor(this, v.toString(), v.toString()));
				edLinkNote->setText(links.isEmpty() ? tr("That link is taken. Pick another.")
								    : tr("That link is taken. Try %1").arg(links.join(", ")));
				set_level(edLinkNote, "warn");
				edCode->setProperty("invalid", true);
				ui::repolish(edCode);
				edError->setText(tr("That link is taken."));
			} else {
				edError->setText(editing ? tr("Could not save the broadcast: %1").arg(error)
							 : tr("Could not create the broadcast: %1").arg(error));
			}
			return;
		}
		settings.broadcastId = doc.object().value("id").toString();
		settings.save();
		pages->setCurrentIndex(Main);
		loadBroadcasts(); // and its defaults become the dock's
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
