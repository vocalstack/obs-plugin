#include "dock.hpp"

#include "api.hpp"
#include "caption-overlay.hpp"
#include "connect-login.hpp"
#include "updater.hpp"
#include "warm-ping.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QCheckBox>
#include <QClipboard>
#include <QGuiApplication>
#include <QComboBox>
#include <QDockWidget>
#include <QFormLayout>
#include <QGroupBox>
#include <QDesktopServices>
#include <QEvent>
#include <QHBoxLayout>
#include <QHostInfo>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpression>
#include <QUrl>
#include <QUuid>
#include <QMainWindow>
#include <QPixmap>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <initializer_list>

// Where an update is read about and downloaded by hand: the plugin's page
// on the VocalStack website.
static const char *PLUGIN_PAGE = "https://vocalstack.com/obs-studio-transcription-plugin/";

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
	layout->setContentsMargins(8, 8, 8, 8);
	layout->addSpacing(12);
	auto *head = new QHBoxLayout;
	head->addWidget(logo(this), 0, Qt::AlignVCenter);
	head->addStretch(1);
	auto *dashboard = new QPushButton(tr("Dashboard"));
	dashboard->setToolTip(tr("Open the VocalStack dashboard"));
	connect(dashboard, &QPushButton::clicked, this, [this]() { QDesktopServices::openUrl(QUrl(api->appBase())); });
	head->addWidget(dashboard, 0, Qt::AlignVCenter);
	layout->addLayout(head);
	layout->addSpacing(14);
	connect(updater, &Updater::changed, this, &Dock::showUpdate);
	updater->setMayInstall([this]() { return session->state() == Session::State::Idle; });
	layout->addWidget(pages, 1);
	// Which build this is, at the foot of the dock.
	auto *version = new QLabel(tr("VocalStack for OBS %1").arg(PLUGIN_VERSION));
	version->setStyleSheet("color: #8b8b96;");
	version->setAlignment(Qt::AlignRight);
	layout->addWidget(version);
	// Captions show always, in the language spoken.
	settings.captionLanguage.clear();
	settings.showCaptions = true;
	pages->addWidget(buildSignedOut());
	pages->addWidget(buildSettings()); // before the signed-in page uses its widgets
	pages->addWidget(buildSignedIn());
	pages->addWidget(buildBroadcastEditor());

	connect(login, &ConnectLogin::finished, this,
		[this](const QString &key, const QString &account, const QString &error) {
			signInButton->setEnabled(true);
			if (!key.isEmpty()) {
				signedIn(key, account);
				// Back from the browser: OBS comes to the front.
				window()->raise();
				window()->activateWindow();
			} else
				setStatus(signInStatus, error, !error.isEmpty());
		});
	connect(api, &Api::unauthorized, this,
		[this]() { signOut(tr("VocalStack no longer accepts this sign-in. Sign in again.")); });
	connect(session, &Session::stateChanged, this, &Dock::onState);
	connect(session, &Session::status, this, [this](const QString &text, bool error) {
		if (error)
			obs_log(LOG_WARNING, "%s", text.toUtf8().constData());
		setStatus(sessionStatus, text, error);
	});
	connect(session, &Session::notice, this, [this](const QString &text) {
		if (!text.isEmpty())
			obs_log(LOG_WARNING, "%s", text.toUtf8().constData());
		sessionNotice->setText(text);
		sessionNotice->setVisible(!text.isEmpty());
	});
	connect(session, &Session::spokenLanguageChanged, this, [this](const QString &lang) {
		spokenBox->setCurrentIndex(qMax(0, spokenBox->findData(lang)));
	});
	connect(session, &Session::listeners, this, [this](int n) {
		listenersLabel->setVisible(n >= 0);
		listenersLabel->setText(tr("%n listening", nullptr, qMax(0, n)));
	});
	connect(session, &Session::limitsChanged, this, [this]() { updateLinks(); });
	connect(session, &Session::caption, this, [this](const QString &text) {
		if (!qEnvironmentVariableIsEmpty("VOCALSTACK_DEBUG"))
			obs_log(LOG_INFO, "caption: %s", text.toUtf8().constData());
		if (overlay && settings.showCaptions)
			overlay->setCaption(text);
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

QWidget *Dock::buildSignedOut()
{
	auto *page = new QWidget;
	auto *v = new QVBoxLayout(page);
	v->setContentsMargins(0, 0, 0, 0);

	auto *intro = new QLabel(tr("Transcribe this stream live with VocalStack, and see the captions over the preview."));
	intro->setWordWrap(true);
	v->addWidget(intro);

	signInButton = new QPushButton(tr("Sign in with VocalStack"));
	connect(signInButton, &QPushButton::clicked, this, [this]() {
		signInButton->setEnabled(false);
		setStatus(signInStatus, tr("Finish signing in in your browser…"), false);
		login->start();
	});
	v->addWidget(signInButton);

	auto *orLabel = new QLabel(tr("Or use an API key from Developers → API keys:"));
	orLabel->setWordWrap(true);
	v->addWidget(orLabel);
	auto *keyRow = new QHBoxLayout;
	keyEdit = new QLineEdit;
	keyEdit->setEchoMode(QLineEdit::Password);
	keyEdit->setPlaceholderText("vs_key_…");
	auto *useButton = new QPushButton(tr("Use key"));
	connect(useButton, &QPushButton::clicked, this, &Dock::useKey);
	connect(keyEdit, &QLineEdit::returnPressed, this, &Dock::useKey);
	keyRow->addWidget(keyEdit, 1);
	keyRow->addWidget(useButton);
	v->addLayout(keyRow);

	signInStatus = new QLabel;
	signInStatus->setWordWrap(true);
	v->addWidget(signInStatus);
	v->addStretch(1);
	return page;
}

QWidget *Dock::buildSignedIn()
{
	auto *page = new QWidget;
	auto *v = new QVBoxLayout(page);
	v->setContentsMargins(0, 0, 0, 0);

	auto *form = new QFormLayout;
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

	auto *broadcastRow = new QHBoxLayout;
	broadcastBox = new QComboBox;
	auto *refresh = new QToolButton;
	refresh->setText("↻");
	refresh->setToolTip(tr("Reload the broadcasts"));
	connect(refresh, &QToolButton::clicked, this, &Dock::loadBroadcasts);
	// A Connect key that reaches only what it creates sees no broadcast
	// made elsewhere: it can make its own.
	auto *add = addBroadcastButton = new QToolButton;
	add->setText(tr("+ New"));
	add->setToolTip(tr("New broadcast"));
	connect(add, &QToolButton::clicked, this, [this]() { openBroadcastEditor(QString()); });
	editBroadcastButton = new QToolButton;
	editBroadcastButton->setText(tr("Edit"));
	editBroadcastButton->setToolTip(tr("Edit this broadcast: name, link, visibility, language, speaker labels"));
	connect(editBroadcastButton, &QToolButton::clicked, this,
		[this]() { openBroadcastEditor(broadcastBox->currentData().toString()); });
	copyBroadcastButton = new QToolButton;
	copyBroadcastButton->setText(tr("Copy link"));
	copyBroadcastButton->setToolTip(tr("Copy the broadcast's link, for listeners"));
	connect(copyBroadcastButton, &QToolButton::clicked, this, &Dock::copyBroadcastLink);
	form->addRow(tr("Broadcast"), broadcastBox);
	// Its buttons on the line under it, with who is listening.
	listenersLabel = new QLabel;
	listenersLabel->setStyleSheet("color: #8b8b96;");
	listenersLabel->hide();
	broadcastRow->addWidget(add);
	broadcastRow->addWidget(editBroadcastButton);
	broadcastRow->addWidget(refresh);
	broadcastRow->addWidget(copyBroadcastButton);
	broadcastRow->addStretch(1);
	broadcastRow->addWidget(listenersLabel);
	form->addRow(QString(), broadcastRow);
	connect(broadcastBox, &QComboBox::activated, this, [this]() {
		settings.broadcastId = broadcastBox->currentData().toString();
		settings.save();
		applyBroadcastDefaults(settings.broadcastId);
		updateLinks();
	});

	spokenBox = new QComboBox;
	form->addRow(tr("Spoken language"), spokenBox);
	connect(spokenBox, &QComboBox::activated, this, [this]() {
		const QString lang = spokenBox->currentData().toString();
		const bool changed = lang != settings.spokenLanguage;
		settings.spokenLanguage = lang;
		settings.save();
		if (changed)
			warm->languageChanged();
	});

	sendBox = new QComboBox;
	sendBox->addItem(tr("Audio only"), false);
	sendBox->addItem(tr("Audio and video"), true);
	sendBox->setCurrentIndex(settings.sendVideo ? 1 : 0);
	form->addRow(tr("Send"), sendBox);
	connect(sendBox, &QComboBox::activated, this, [this]() {
		settings.sendVideo = sendBox->currentData().toBool();
		settings.save();
	});
	v->addLayout(form);

	startButton = new QPushButton(tr("Start transcribing"));
	startButton->setMinimumHeight(32);
	connect(startButton, &QPushButton::clicked, this, &Dock::toggle);
	v->addWidget(startButton);

	// Says the button need not be pressed when OBS will start it.
	followHint = new QLabel(tr("Starts when you start streaming or recording in OBS."));
	followHint->setWordWrap(true);
	followHint->setStyleSheet("color: #8b8b96;"); // muted on dark and light themes
	v->addWidget(followHint);
	updateFollowHint();

	sessionStatus = new QLabel;
	sessionStatus->setWordWrap(true);
	sessionStatus->setOpenExternalLinks(true);
	v->addWidget(sessionStatus);
	// What VocalStack warns about while live (the speech is another
	// language than the spoken language set), kept until the next Start.
	sessionNotice = new QLabel;
	sessionNotice->setWordWrap(true);
	sessionNotice->setStyleSheet("color: #f0a63a;"); // amber on dark and light themes
	sessionNotice->hide();
	v->addWidget(sessionNotice);

	// The transcription in VocalStack: open it, or share it.
	auto *links = new QHBoxLayout;
	openButton = new QPushButton(tr("Open in VocalStack"));
	openButton->setMinimumHeight(32);
	connect(openButton, &QPushButton::clicked, this, &Dock::openTranscription);
	copyLinkButton = new QPushButton(tr("Copy transcript link"));
	copyLinkButton->setMinimumHeight(32);
	copyLinkButton->setToolTip(tr("A link anyone can read the transcription with"));
	connect(copyLinkButton, &QPushButton::clicked, this, &Dock::copyTranscriptLink);
	links->addWidget(openButton, 1);
	links->addWidget(copyLinkButton, 1);
	v->addLayout(links);
	updateLinks();
	v->addStretch(1);

	// A newer release: what it is and how it is going, and the button that
	// installs it when it is not installed by itself.
	auto *updateRow = new QHBoxLayout;
	updateNotice = new QLabel;
	updateNotice->setWordWrap(true);
	updateNotice->setTextFormat(Qt::RichText);
	updateNotice->setOpenExternalLinks(true);
	updateNotice->hide();
	updateButton = new QPushButton(tr("Update now"));
	updateButton->hide();
	connect(updateButton, &QPushButton::clicked, this, [this]() {
		updateWanted = true;
		showUpdate();
	});
	updateDismiss = new QToolButton;
	updateDismiss->setText(QStringLiteral("✕"));
	updateDismiss->setToolTip(tr("Dismiss"));
	updateDismiss->setAutoRaise(true);
	updateDismiss->hide();
	connect(updateDismiss, &QToolButton::clicked, this, [this]() {
		updateDismissed = updateNoticeText;
		showUpdate();
	});
	updateSpin = new QTimer(this);
	updateSpin->setInterval(90);
	connect(updateSpin, &QTimer::timeout, this, [this]() {
		updateSpinFrame++;
		showUpdate();
	});
	updateRow->addWidget(updateNotice, 1);
	updateRow->addWidget(updateButton);
	updateRow->addWidget(updateDismiss, 0, Qt::AlignTop);
	v->addLayout(updateRow);

	// Who is signed in, at the foot, with the settings.
	auto *account = new QHBoxLayout;
	accountLabel = new QLabel;
	accountLabel->setWordWrap(true);
	auto *settingsButton = new QToolButton;
	settingsButton->setText(tr("Settings"));
	connect(settingsButton, &QToolButton::clicked, this, [this]() { pages->setCurrentIndex(1); });
	auto *signOutButton = new QToolButton;
	signOutButton->setText(tr("Sign out"));
	connect(signOutButton, &QToolButton::clicked, this, [this]() { signOut(QString()); });
	account->addWidget(accountLabel, 1);
	account->addWidget(settingsButton);
	account->addWidget(signOutButton);
	v->addLayout(account);
	return page;
}

// The settings, a page of their own: Back returns to the dock.
QWidget *Dock::buildSettings()
{
	auto *page = new QWidget;
	auto *v = new QVBoxLayout(page);
	v->setContentsMargins(0, 0, 0, 0);

	auto *back = new QPushButton(tr("‹ Back"));
	connect(back, &QPushButton::clicked, this, [this]() { pages->setCurrentIndex(2); });
	auto *top = new QHBoxLayout;
	top->addWidget(back);
	top->addStretch(1);
	v->addLayout(top);

	auto *tx = new QGroupBox(tr("Transcription"));
	auto *form = new QFormLayout(tx);
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	nameEdit = new QLineEdit;
	nameEdit->setPlaceholderText(tr("OBS on %1 (optional)").arg(QHostInfo::localHostName()));
	nameEdit->setText(settings.transcriptionName);
	nameEdit->setToolTip(tr("The transcription's name in VocalStack"));
	connect(nameEdit, &QLineEdit::editingFinished, this, [this]() {
		settings.transcriptionName = nameEdit->text().trimmed().left(200);
		settings.save();
	});
	form->addRow(tr("Name"), nameEdit);
	speakersBox = new QComboBox;
	speakersBox->addItem(tr("When needed"), "auto");
	speakersBox->addItem(tr("On"), "on");
	speakersBox->addItem(tr("Off"), "off");
	speakersBox->setToolTip(tr("Label each line with who is speaking: when a second speaker is heard, always, or never"));
	speakersBox->setCurrentIndex(qMax(0, speakersBox->findData(settings.speakerLabels)));
	form->addRow(tr("Speaker labels"), speakersBox);
	connect(speakersBox, &QComboBox::activated, this, [this]() {
		settings.speakerLabels = speakersBox->currentData().toString();
		settings.save();
	});
	lockBox = new QCheckBox(tr("Lock the transcription against deletion"));
	lockBox->setChecked(settings.locked);
	connect(lockBox, &QCheckBox::toggled, this, [this](bool on) {
		settings.locked = on;
		settings.save();
	});
	form->addRow(lockBox);
	v->addWidget(tx);

	auto *obs = new QGroupBox(tr("OBS"));
	auto *ov = new QVBoxLayout(obs);
	followBox = new QCheckBox(tr("Start and stop with OBS streaming or recording"));
	followBox->setChecked(settings.followObs);
	connect(followBox, &QCheckBox::toggled, this, [this](bool on) {
		settings.followObs = on;
		settings.save();
		updateFollowHint();
	});
	ov->addWidget(followBox);
	v->addWidget(obs);

	auto *updates = new QGroupBox(tr("Updates"));
	auto *uv = new QVBoxLayout(updates);
	autoUpdateBox = new QCheckBox(tr("Install updates automatically"));
	autoUpdateBox->setToolTip(tr("A newer release is installed while nothing is being transcribed; restart OBS to use it"));
	autoUpdateBox->setChecked(settings.autoUpdate);
	connect(autoUpdateBox, &QCheckBox::toggled, this, [this](bool on) {
		settings.autoUpdate = on;
		settings.save();
		showUpdate();
	});
	uv->addWidget(autoUpdateBox);
	auto *checkRow = new QHBoxLayout;
	checkUpdatesButton = new QPushButton(tr("Check for updates"));
	connect(checkUpdatesButton, &QPushButton::clicked, this, [this]() {
		checkWanted = true;
		checkUpdatesButton->setEnabled(false);
		checkUpdatesResult->setText(tr("Checking…"));
		updater->check(true);
		if (updater->step() != Updater::Step::Checking)
			showUpdate(); // busy downloading or installing: say so now
	});
	checkUpdatesResult = new QLabel;
	checkUpdatesResult->setWordWrap(true);
	checkUpdatesResult->setStyleSheet("color: #8b8b96;");
	checkRow->addWidget(checkUpdatesButton);
	checkRow->addWidget(checkUpdatesResult, 1);
	uv->addLayout(checkRow);
	v->addWidget(updates);
	v->addStretch(1);
	return page;
}

// The broadcast's own spoken language, speaker labels and transcription
// mode, as set in VocalStack, become the dock's. "None" changes nothing.
void Dock::applyBroadcastDefaults(const QString &id)
{
	if (id.isEmpty() || !broadcastSettings.contains(id))
		return;
	const QJsonObject s = broadcastSettings.value(id);
	const QString lang = s.value("spokenLanguage").toString();
	const bool langChanged = lang != settings.spokenLanguage;
	settings.spokenLanguage = lang;
	if (s.value("speakerLabels").isString())
		settings.speakerLabels = s.value("speakerLabels").toString();
	if (s.value("latencyMode").isString())
		settings.latencyMode = s.value("latencyMode").toString();
	settings.save();
	spokenBox->setCurrentIndex(qMax(0, spokenBox->findData(settings.spokenLanguage)));
	speakersBox->setCurrentIndex(qMax(0, speakersBox->findData(settings.speakerLabels)));
	if (langChanged)
		warm->languageChanged();
}

// The update row: a newer release, how old this one is, and where the
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
	QString text = updater->message();
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
		text += QStringLiteral(" <a href=\"%1\">%2</a>").arg(PLUGIN_PAGE, tr("Download"));
	}
	// Working: a spinner before the words. Done: a check.
	const bool working = step == Updater::Step::Downloading || step == Updater::Step::Installing;
	const bool done = step == Updater::Step::Installed || step == Updater::Step::OnExit;
	if (working != updateSpin->isActive())
		working ? updateSpin->start() : updateSpin->stop();
	updateNoticeText = text;
	const bool dismissed = !text.isEmpty() && text == updateDismissed && !working;
	static const QStringList frames{"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
	// The words are rich text already (the Download link).
	QString shown = text;
	if (working)
		shown = QStringLiteral("<span style='font-family: monospace;'>%1</span>&nbsp;&nbsp;%2")
				.arg(frames.at(updateSpinFrame % frames.size()), text);
	else if (done)
		shown = QStringLiteral("<span style='color: #3fb950; font-weight: bold;'>✓</span>&nbsp;&nbsp;%1").arg(text);
	updateNotice->setText(shown);
	updateNotice->setStyleSheet(problem ? "color: #f0a63a;" : "");
	updateNotice->setVisible(!text.isEmpty() && !dismissed);
	updateDismiss->setVisible(!text.isEmpty() && !dismissed && !working);
	updateButton->setVisible(pending && !settings.autoUpdate && !updateWanted && !dismissed);
	updateButton->setEnabled(pending);
	// An unsupported version does not start.
	const bool blocked = age == Updater::Age::Unsupported && step != Updater::Step::Installed;
	startButton->setEnabled(!blocked && session->state() != Session::State::Stopping);
}

// The link buttons follow whether there is a transcription or a broadcast
// yet.
void Dock::updateLinks()
{
	const bool tx = !settings.transcriptionId.isEmpty();
	openButton->setEnabled(tx);
	copyLinkButton->setEnabled(tx);
	copyBroadcastButton->setEnabled(broadcastLinks.contains(settings.broadcastId));
	editBroadcastButton->setEnabled(broadcastObjects.contains(settings.broadcastId) &&
					session->state() == Session::State::Idle);
}

void Dock::openTranscription()
{
	if (!settings.transcriptionId.isEmpty())
		QDesktopServices::openUrl(QUrl(api->appBase() + "/transcriptions/" + settings.transcriptionId));
}

void Dock::copyBroadcastLink()
{
	const QString url = broadcastLinks.value(settings.broadcastId);
	if (url.isEmpty())
		return;
	QGuiApplication::clipboard()->setText(url);
	setStatus(sessionStatus, tr("Broadcast link copied"), false);
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
			  if (!error.isEmpty()) {
				  setStatus(sessionStatus, tr("Could not make a link: %1").arg(error), true);
				  return;
			  }
			  const QString url = doc.object().value("url").toString();
			  if (url.isEmpty())
				  return;
			  QGuiApplication::clipboard()->setText(url);
			  setStatus(sessionStatus, tr("Transcript link copied"), false);
		  });
}

void Dock::setOverlay(CaptionOverlay *o)
{
	overlay = o;
}

void Dock::setStatus(QLabel *label, const QString &text, bool error)
{
	label->setText(text);
	label->setStyleSheet(error ? "color: #ff6b6b;" : "");
}

void Dock::showSignedIn(bool in)
{
	pages->setCurrentIndex(in ? 2 : 0);
	if (!in)
		return;
	accountLabel->setText(settings.account.isEmpty() ? tr("Signed in with an API key")
							 : tr("Signed in to <b>%1</b>").arg(settings.account.toHtmlEscaped()));
	loadLists();
	loadBroadcasts();
	session->refreshLimits();
	updateLinks();
	// When signed in (nothing is sent before): whether a newer release is
	// out, now and every 6 hours.
	updater->check();
}

void Dock::signedIn(const QString &key, const QString &account)
{
	settings.apiKey = key;
	settings.account = account;
	settings.save();
	api->setKey(key);
	setStatus(signInStatus, QString(), false);
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
	setStatus(signInStatus, tr("Checking the key…"), false);
	api->get("/v1/me", [this, key](int, const QJsonDocument &doc, const QString &error) {
		if (!error.isEmpty()) {
			api->setKey(settings.apiKey);
			setStatus(signInStatus, tr("VocalStack refused the key: %1").arg(error), true);
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
			setStatus(signInStatus,
				  tr("This key cannot transcribe: it lacks the %1 scope. Make one with read, write and "
				     "recordings:publish in Developers → API keys.")
					  .arg(missing.join(tr(" and "))),
				  true);
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
	broadcastLinks.clear();
	broadcastSettings.clear();
	settings.save();
	warm->refresh(); // canStart false while the key still works
	api->setKey(QString());
	showSignedIn(false);
	setStatus(signInStatus, why, !why.isEmpty());
}

void Dock::loadLists()
{
	api->get("/v1/languages", [this](int, const QJsonDocument &doc, const QString &error) {
		if (!error.isEmpty())
			return;
		const QJsonObject o = doc.object();
		QHash<QString, QString> names;
		for (const QJsonValue v : o.value("languages").toArray())
			names.insert(v.toObject().value("code").toString(), v.toObject().value("name").toString());
		auto sorted = [&](const QJsonArray &codes) {
			QList<QPair<QString, QString>> list;
			for (const QJsonValue c : codes)
				list.append({names.value(c.toString(), c.toString()), c.toString()});
			std::sort(list.begin(), list.end());
			return list;
		};

		spokenBox->clear();
		spokenBox->addItem(tr("Detect"), QString());
		spokenLanguages = sorted(o.value("transcription").toArray());
		for (const auto &[name, code] : spokenLanguages)
			spokenBox->addItem(name, code);
		spokenBox->setCurrentIndex(qMax(0, spokenBox->findData(settings.spokenLanguage)));

	});
}

void Dock::loadBroadcasts()
{
	api->get("/v1/broadcasts", [this](int, const QJsonDocument &doc, const QString &error) {
		broadcastBox->clear();
		broadcastBox->addItem(tr("None"), QString());
		if (!error.isEmpty()) {
			setStatus(sessionStatus, tr("Could not load the broadcasts: %1").arg(error), true);
			return;
		}
		broadcastLinks.clear();
		broadcastSettings.clear();
		broadcastObjects.clear();
		for (const QJsonValue v : doc.object().value("broadcasts").toArray()) {
			const QJsonObject b = v.toObject();
			if (b.value("archivedAt").isString() || b.value("lifecycle").toString() == "archived")
				continue;
			broadcastBox->addItem(b.value("name").toString(), b.value("id").toString());
			broadcastLinks.insert(b.value("id").toString(), b.value("shareUrl").toString());
			broadcastSettings.insert(b.value("id").toString(), b.value("settings").toObject());
			broadcastObjects.insert(b.value("id").toString(), b);
		}
		const int at = broadcastBox->findData(settings.broadcastId);
		broadcastBox->setCurrentIndex(qMax(0, at));
		if (at < 0 && !settings.broadcastId.isEmpty()) {
			settings.broadcastId.clear(); // deleted or archived since
			settings.save();
		}
		applyBroadcastDefaults(settings.broadcastId);
		updateLinks();
	});
}

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

// Where a broadcast's link lives (live.vocalstack.com), from a link
// VocalStack gave.
static QString share_host(const QHash<QString, QString> &links)
{
	for (const QString &url : links) {
		const QUrl u(url);
		if (!u.host().isEmpty())
			return u.host();
	}
	return QStringLiteral("live.vocalstack.com");
}

QWidget *Dock::buildBroadcastEditor()
{
	auto *page = new QWidget;
	auto *v = new QVBoxLayout(page);
	v->setContentsMargins(0, 0, 0, 0);

	auto *top = new QHBoxLayout;
	auto *back = new QPushButton(tr("‹ Back"));
	connect(back, &QPushButton::clicked, this, [this]() { pages->setCurrentIndex(2); });
	edTitle = new QLabel;
	edTitle->setStyleSheet("font-weight: bold;");
	top->addWidget(back);
	top->addWidget(edTitle, 1);
	v->addLayout(top);

	auto *about = new QGroupBox(tr("Broadcast"));
	auto *form = new QFormLayout(about);
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	edName = new QLineEdit;
	edName->setPlaceholderText(tr("A name for your broadcast…"));
	connect(edName, &QLineEdit::textEdited, this, [this]() {
		if (editingId.isEmpty() && !editCodeTouched)
			edCode->setText(slugify(edName->text()));
		updateBroadcastEditor();
	});
	form->addRow(tr("Name"), edName);

	auto *linkRow = new QHBoxLayout;
	edLinkPrefix = new QLabel;
	edLinkPrefix->setStyleSheet("color: #8b8b96;");
	edCode = new QLineEdit;
	edCode->setPlaceholderText("my-broadcast-link");
	edCode->setMaxLength(CODE_MAX);
	connect(edCode, &QLineEdit::textEdited, this, [this](const QString &text) {
		edLinkNote->clear();
		const QString slug = slugify(text);
		editCodeTouched = !slug.isEmpty();
		if (!editCodeTouched && editingId.isEmpty())
			edCode->setText(slugify(edName->text()));
		updateBroadcastEditor();
	});
	linkRow->setSpacing(0);
	linkRow->addWidget(edLinkPrefix);
	linkRow->addWidget(edCode, 1);
	form->addRow(tr("Link"), linkRow);
	edLinkNote = new QLabel;
	edLinkNote->setWordWrap(true);
	edLinkNote->setTextFormat(Qt::RichText);
	edLinkNote->setStyleSheet("color: #8b8b96;");
	// A suggestion offered for a link that is taken: a click takes it.
	connect(edLinkNote, &QLabel::linkActivated, this, [this](const QString &code) {
		edCode->setText(code);
		edLinkNote->clear();
		editCodeTouched = true;
		edError->clear();
		updateBroadcastEditor();
	});
	form->addRow(QString(), edLinkNote);

	edAccess = new QComboBox;
	edAccess->addItem(tr("Public"), "public");
	edAccess->addItem(tr("Private"), "private");
	connect(edAccess, &QComboBox::activated, this, [this]() { updateBroadcastEditor(); });
	form->addRow(tr("Visibility"), edAccess);
	edPassword = new QLineEdit;
	edPassword->setEchoMode(QLineEdit::Password);
	connect(edPassword, &QLineEdit::textEdited, this, [this]() { updateBroadcastEditor(); });
	form->addRow(QString(), edPassword);
	edAccessNote = new QLabel;
	edAccessNote->setWordWrap(true);
	edAccessNote->setStyleSheet("color: #8b8b96;");
	form->addRow(QString(), edAccessNote);
	v->addWidget(about);

	auto *defaults = new QGroupBox(tr("Defaults"));
	auto *df = new QFormLayout(defaults);
	df->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	edSpoken = new QComboBox;
	df->addRow(tr("Spoken language"), edSpoken);
	auto *spokenNote = new QLabel(tr("Auto detect gives each spoken segment its own detected language, so speakers "
					 "can switch languages mid-conversation."));
	spokenNote->setWordWrap(true);
	spokenNote->setStyleSheet("color: #8b8b96;");
	df->addRow(QString(), spokenNote);
	edLabels = new QComboBox;
	edLabels->addItem(tr("When needed"), "auto");
	edLabels->addItem(tr("On"), "on");
	edLabels->addItem(tr("Off"), "off");
	df->addRow(tr("Speaker labels"), edLabels);
	v->addWidget(defaults);

	edError = new QLabel;
	edError->setWordWrap(true);
	edError->setStyleSheet("color: #ff6b6b;");
	v->addWidget(edError);
	edSave = new QPushButton;
	edSave->setMinimumHeight(32);
	connect(edSave, &QPushButton::clicked, this, &Dock::saveBroadcast);
	connect(edName, &QLineEdit::returnPressed, this, &Dock::saveBroadcast);
	v->addWidget(edSave);
	v->addStretch(1);
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
	edSave->setText(editing ? tr("Save changes") : tr("Create broadcast"));
	edName->setText(b.value("name").toString());
	edLinkPrefix->setText(share_host(broadcastLinks) + "/");
	edCode->setText(editingOrigCode);
	edAccess->setCurrentIndex(editWasPrivate ? 1 : 0);
	edPassword->clear();
	edSpoken->clear();
	edSpoken->addItem(tr("Auto detect"), QString());
	for (const auto &[name, code] : spokenLanguages)
		edSpoken->addItem(name, code);
	edSpoken->setCurrentIndex(qMax(0, edSpoken->findData(editing ? bs.value("spokenLanguage").toString()
								      : settings.spokenLanguage)));
	edLabels->setCurrentIndex(
		qMax(0, edLabels->findData(bs.value("speakerLabels").toString(editing ? "auto" : settings.speakerLabels))));
	edError->clear();
	edLinkNote->clear();
	updateBroadcastEditor();
	pages->setCurrentIndex(3);
	edName->setFocus();
}

void Dock::updateBroadcastEditor()
{
	const bool isPrivate = edAccess->currentData().toString() == "private";
	const bool editing = !editingId.isEmpty();
	edPassword->setVisible(isPrivate);
	edPassword->setPlaceholderText(editing && editWasPrivate ? tr("Leave empty to keep the password")
								 : tr("A password, at least three characters"));
	edAccessNote->setText(isPrivate ? tr("Only people with the password can open the link. They are asked for it "
					     "before the transcript appears.")
					: tr("Anyone with the link can access the broadcast."));
	const QString code = edCode->text();
	if (!edLinkNote->text().contains("<a ")) // a taken link's suggestions stay until one is picked
		edLinkNote->setText(code.size() > 0 && code.size() < 6
					    ? tr("Links need at least six characters.")
					    : tr("Share this link with your audience. The transcript and translations "
						 "appear there while you record."));
}

void Dock::saveBroadcast()
{
	const bool editing = !editingId.isEmpty();
	const QString name = edName->text().trimmed();
	const QString code = slugify(edCode->text());
	const bool isPrivate = edAccess->currentData().toString() == "private";
	const QString password = edPassword->text();
	if (name.isEmpty()) {
		edError->setText(tr("Give the broadcast a name first."));
		return;
	}
	if (code.size() < 6) {
		edError->setText(tr("The broadcast link needs at least six characters."));
		return;
	}
	if (isPrivate && password.size() < 3 && !(editing && editWasPrivate && password.isEmpty())) {
		edError->setText(tr("A private broadcast needs a password of at least three characters."));
		return;
	}
	QJsonObject bsettings{
		{"spokenLanguage", edSpoken->currentData().toString().isEmpty() ? QJsonValue()
										: QJsonValue(edSpoken->currentData().toString())},
		{"speakerLabels", edLabels->currentData().toString()},
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
			if (status == 409 && !suggestions.isEmpty()) {
				QStringList links;
				for (const QJsonValue v : suggestions)
					links.append(QStringLiteral("<a href=\"%1\">%1</a>").arg(v.toString().toHtmlEscaped()));
				edLinkNote->setText(tr("That link is taken. Try %1").arg(links.join(", ")));
				edError->setText(tr("That link is taken. Pick another."));
			} else {
				edError->setText(editing ? tr("Could not save the broadcast: %1").arg(error)
							 : tr("Could not create the broadcast: %1").arg(error));
			}
			return;
		}
		settings.broadcastId = doc.object().value("id").toString();
		settings.save();
		pages->setCurrentIndex(2);
		setStatus(sessionStatus, editing ? tr("Broadcast saved") : tr("Broadcast created"), false);
		loadBroadcasts(); // and its defaults become the dock's
	};
	if (editing)
		api->patch("/v1/broadcasts/" + editingId, body, done);
	else
		api->post("/v1/broadcasts", body, QUuid::createUuid().toString(QUuid::WithoutBraces), done);
}

void Dock::onState(Session::State s)
{
	const bool idle = s == Session::State::Idle;
	startButton->setText(idle ? tr("Start transcribing")
				  : s == Session::State::Stopping ? tr("Stopping…")
								  : tr("Stop transcribing"));
	startButton->setEnabled(s != Session::State::Stopping);
	updateFollowHint();
	for (QWidget *w : std::initializer_list<QWidget *>{broadcastBox, spokenBox, sendBox, speakersBox, lockBox, nameEdit,
							  addBroadcastButton})
		w->setEnabled(idle);
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

void Dock::shutdown()
{
	updater->finishOnExit();
	warm->end();
	login->cancel();
	session->stop();
}

// Closing OBS while live: the close waits (at most a few seconds) for the
// recording to stop, or it would stay live until VocalStack noticed the
// stream was gone. OBS holds its own close the same way.
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
