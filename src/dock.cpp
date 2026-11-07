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
#include <QDesktopServices>
#include <QEvent>
#include <QHBoxLayout>
#include <QHostInfo>
#include <QInputDialog>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpression>
#include <QUrl>
#include <QMainWindow>
#include <QPixmap>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <initializer_list>

// Caption box choices that are not a language.
static const char *CAPTIONS_OFF = "off";

static const char *LATEST_RELEASE_PAGE = "https://github.com/vocalstack/obs-plugin/releases/latest";

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
	auto *head = new QHBoxLayout;
	head->addWidget(logo(this));
	head->addStretch(1);
	layout->addLayout(head);
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
	connect(updateButton, &QPushButton::clicked, this, [this]() { updater->install(); });
	updateRow->addWidget(updateNotice, 1);
	updateRow->addWidget(updateButton);
	layout->addLayout(updateRow);
	connect(updater, &Updater::changed, this, &Dock::showUpdate);
	layout->addWidget(pages, 1);
	// Which build this is, at the foot of the dock.
	auto *version = new QLabel(tr("VocalStack for OBS %1").arg(PLUGIN_VERSION));
	version->setStyleSheet("color: #8b8b96;");
	version->setAlignment(Qt::AlignRight);
	layout->addWidget(version);
	pages->addWidget(buildSignedOut());
	pages->addWidget(buildSignedIn());

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
	connect(session, &Session::captionLanguageRefused, this, [this](const QString &message) {
		captionBox->setCurrentIndex(qMax(0, captionBox->findData(QString())));
		setStatus(sessionStatus, message.isEmpty() ? tr("VocalStack refused that caption language: captions as spoken")
							   : tr("Captions as spoken: %1").arg(message),
			  true);
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

	nameEdit = new QLineEdit;
	nameEdit->setPlaceholderText(QStringLiteral("OBS on %1").arg(QHostInfo::localHostName()));
	nameEdit->setText(settings.transcriptionName);
	nameEdit->setToolTip(tr("The transcription's name in VocalStack"));
	connect(nameEdit, &QLineEdit::editingFinished, this, [this]() {
		settings.transcriptionName = nameEdit->text().trimmed().left(200);
		settings.save();
	});
	form->addRow(tr("Name"), nameEdit);

	auto *broadcastRow = new QHBoxLayout;
	broadcastBox = new QComboBox;
	auto *refresh = new QToolButton;
	refresh->setText("↻");
	refresh->setToolTip(tr("Reload the broadcasts"));
	connect(refresh, &QToolButton::clicked, this, &Dock::loadBroadcasts);
	// A Connect key that reaches only what it creates sees no broadcast
	// made elsewhere: it can make its own.
	auto *add = new QToolButton;
	add->setText("+");
	add->setToolTip(tr("New broadcast"));
	connect(add, &QToolButton::clicked, this, &Dock::newBroadcast);
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

	captionBox = new QComboBox;
	form->addRow(tr("Captions"), captionBox);
	connect(captionBox, &QComboBox::activated, this, [this]() {
		const QString v = captionBox->currentData().toString();
		settings.showCaptions = v != CAPTIONS_OFF;
		if (overlay && !settings.showCaptions)
			overlay->setCaption(QString());
		session->setCaptionLanguage(settings.showCaptions ? v : settings.captionLanguage);
	});

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

	latencyBox = new QComboBox;
	latencyBox->addItem(tr("Automatic"), "auto");
	latencyBox->addItem(tr("Fast"), "fast");
	latencyBox->addItem(tr("Accurate"), "accuracy");
	latencyBox->setToolTip(tr("Automatic: the accurate model, the fast one only while it falls behind. "
				  "Fast: lowest latency. Accurate: never switches."));
	latencyBox->setCurrentIndex(qMax(0, latencyBox->findData(settings.latencyMode)));
	form->addRow(tr("Transcription"), latencyBox);
	connect(latencyBox, &QComboBox::activated, this, [this]() {
		settings.latencyMode = latencyBox->currentData().toString();
		settings.save();
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

	lockBox = new QCheckBox(tr("Lock the transcription against deletion"));
	lockBox->setChecked(settings.locked);
	connect(lockBox, &QCheckBox::toggled, this, [this](bool on) {
		settings.locked = on;
		settings.save();
	});
	v->addWidget(lockBox);

	followBox = new QCheckBox(tr("Start and stop with OBS streaming or recording"));
	followBox->setChecked(settings.followObs);
	connect(followBox, &QCheckBox::toggled, this, [this](bool on) {
		settings.followObs = on;
		settings.save();
		updateFollowHint();
	});
	v->addWidget(followBox);

	autoUpdateBox = new QCheckBox(tr("Install updates automatically"));
	autoUpdateBox->setToolTip(tr("A newer release is installed while nothing is being transcribed; restart OBS to use it"));
	autoUpdateBox->setChecked(settings.autoUpdate);
	connect(autoUpdateBox, &QCheckBox::toggled, this, [this](bool on) {
		settings.autoUpdate = on;
		settings.save();
		showUpdate();
	});
	v->addWidget(autoUpdateBox);

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

	// The plan's longest recording, once VocalStack has said.
	limitHint = new QLabel;
	limitHint->setWordWrap(true);
	limitHint->setStyleSheet("color: #8b8b96;");
	limitHint->hide();
	v->addWidget(limitHint);

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
	openButton = new QToolButton;
	openButton->setText(tr("Open in VocalStack"));
	connect(openButton, &QToolButton::clicked, this, &Dock::openTranscription);
	copyLinkButton = new QToolButton;
	copyLinkButton->setText(tr("Copy transcript link"));
	copyLinkButton->setToolTip(tr("A link anyone can read the transcription with"));
	connect(copyLinkButton, &QToolButton::clicked, this, &Dock::copyTranscriptLink);
	links->addWidget(openButton);
	links->addWidget(copyLinkButton);
	links->addStretch(1);
	v->addLayout(links);
	updateLinks();
	v->addStretch(1);

	// Who is signed in, at the foot.
	auto *account = new QHBoxLayout;
	accountLabel = new QLabel;
	accountLabel->setWordWrap(true);
	auto *signOutButton = new QToolButton;
	signOutButton->setText(tr("Sign out"));
	connect(signOutButton, &QToolButton::clicked, this, [this]() { signOut(QString()); });
	account->addWidget(accountLabel, 1);
	account->addWidget(signOutButton);
	v->addLayout(account);
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
	latencyBox->setCurrentIndex(qMax(0, latencyBox->findData(settings.latencyMode)));
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
	QString text = updater->message();
	bool problem = updater->problem();
	const bool pending = age != Updater::Age::Current && step == Updater::Step::Idle;
	if (pending && settings.autoUpdate && !settings.apiKey.isEmpty() && session->state() == Session::State::Idle &&
	    !problem) {
		updater->install(); // says how it goes through changed()
		return;
	}
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
		text += QStringLiteral(" <a href=\"%1\">Release notes</a>").arg(LATEST_RELEASE_PAGE);
	}
	updateNotice->setText(text);
	updateNotice->setStyleSheet(problem ? "color: #f0a63a;" : "");
	updateNotice->setVisible(!text.isEmpty());
	updateButton->setVisible(pending);
	updateButton->setEnabled(pending);
	// An unsupported version does not start.
	const bool blocked = age == Updater::Age::Unsupported && step != Updater::Step::Installed;
	startButton->setEnabled(!blocked && session->state() != Session::State::Stopping);
}

// The link buttons follow whether there is a transcription or a broadcast
// yet; the limit hint, the plan's longest recording.
void Dock::updateLinks()
{
	const bool tx = !settings.transcriptionId.isEmpty();
	openButton->setEnabled(tx);
	copyLinkButton->setEnabled(tx);
	copyBroadcastButton->setEnabled(broadcastLinks.contains(settings.broadcastId));
	if (settings.maxDurationS > 0) {
		const int h = settings.maxDurationS / 3600, m = (settings.maxDurationS % 3600) / 60;
		limitHint->setText(m == 0 ? tr("Your plan records up to %n hour(s) at a time.", nullptr, h)
					  : tr("Your plan records up to %1 h %2 min at a time.").arg(h).arg(m));
		limitHint->show();
	} else {
		limitHint->hide();
	}
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
	pages->setCurrentIndex(in ? 1 : 0);
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
		for (const auto &[name, code] : sorted(o.value("transcription").toArray()))
			spokenBox->addItem(name, code);
		spokenBox->setCurrentIndex(qMax(0, spokenBox->findData(settings.spokenLanguage)));

		captionBox->clear();
		captionBox->addItem(tr("Off"), CAPTIONS_OFF);
		captionBox->addItem(tr("As spoken"), QString());
		for (const auto &[name, code] : sorted(o.value("translation").toArray()))
			captionBox->addItem(tr("In %1").arg(name), code);
		captionBox->setCurrentIndex(settings.showCaptions
						    ? qMax(1, captionBox->findData(settings.captionLanguage))
						    : 0);
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
		for (const QJsonValue v : doc.object().value("broadcasts").toArray()) {
			const QJsonObject b = v.toObject();
			if (b.value("archivedAt").isString() || b.value("lifecycle").toString() == "archived")
				continue;
			broadcastBox->addItem(b.value("name").toString(), b.value("id").toString());
			broadcastLinks.insert(b.value("id").toString(), b.value("shareUrl").toString());
			broadcastSettings.insert(b.value("id").toString(), b.value("settings").toObject());
		}
		const int at = broadcastBox->findData(settings.broadcastId);
		broadcastBox->setCurrentIndex(qMax(0, at));
		if (at < 0 && !settings.broadcastId.isEmpty()) {
			settings.broadcastId.clear(); // deleted or archived since
			settings.save();
		}
		updateLinks();
	});
}

void Dock::newBroadcast()
{
	bool ok = false;
	const QString name = QInputDialog::getText(this, tr("New broadcast"), tr("Name"), QLineEdit::Normal,
						   QString(), &ok)
				     .trimmed();
	if (!ok || name.isEmpty())
		return;
	api->post("/v1/broadcasts", QJsonObject{{"name", name}},
		  [this](int, const QJsonDocument &doc, const QString &error) {
			  if (!error.isEmpty()) {
				  setStatus(sessionStatus, tr("Could not create the broadcast: %1").arg(error), true);
				  return;
			  }
			  settings.broadcastId = doc.object().value("id").toString();
			  settings.save();
			  loadBroadcasts();
		  });
}

void Dock::onState(Session::State s)
{
	const bool idle = s == Session::State::Idle;
	startButton->setText(idle ? tr("Start transcribing")
				  : s == Session::State::Stopping ? tr("Stopping…")
								  : tr("Stop transcribing"));
	startButton->setEnabled(s != Session::State::Stopping);
	updateFollowHint();
	for (QWidget *w : std::initializer_list<QWidget *>{broadcastBox, spokenBox, sendBox, nameEdit, speakersBox,
							  latencyBox, lockBox})
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
