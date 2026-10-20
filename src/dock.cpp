#include "dock.hpp"

#include "api.hpp"
#include "caption-overlay.hpp"
#include "connect-login.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

// Caption box choices that are not a language.
static const char *CAPTIONS_OFF = "off";

Dock::Dock(QWidget *parent)
	: QWidget(parent),
	  api(new Api(this)),
	  settings(Settings::load()),
	  session(new Session(api, &settings, this)),
	  login(new ConnectLogin(api, this)),
	  pages(new QStackedWidget(this))
{
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(8, 8, 8, 8);
	layout->addWidget(pages);
	pages->addWidget(buildSignedOut());
	pages->addWidget(buildSignedIn());

	connect(login, &ConnectLogin::finished, this,
		[this](const QString &key, const QString &account, const QString &error) {
			signInButton->setEnabled(true);
			if (!key.isEmpty())
				signedIn(key, account);
			else
				setStatus(signInStatus, error, !error.isEmpty());
		});
	connect(api, &Api::unauthorized, this,
		[this]() { signOut(tr("VocalStack no longer accepts this sign-in. Sign in again.")); });
	connect(session, &Session::stateChanged, this, &Dock::onState);
	connect(session, &Session::status, this,
		[this](const QString &text, bool error) { setStatus(sessionStatus, text, error); });
	connect(session, &Session::caption, this, [this](const QString &text) {
		if (overlay && settings.showCaptions)
			overlay->setCaption(text);
	});

	api->setKey(settings.apiKey);
	showSignedIn(!settings.apiKey.isEmpty());
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

	auto *top = new QHBoxLayout;
	accountLabel = new QLabel;
	accountLabel->setWordWrap(true);
	auto *signOutButton = new QToolButton;
	signOutButton->setText(tr("Sign out"));
	connect(signOutButton, &QToolButton::clicked, this, [this]() { signOut(QString()); });
	top->addWidget(accountLabel, 1);
	top->addWidget(signOutButton);
	v->addLayout(top);

	auto *form = new QFormLayout;
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

	auto *broadcastRow = new QHBoxLayout;
	broadcastBox = new QComboBox;
	auto *refresh = new QToolButton;
	refresh->setText("↻");
	refresh->setToolTip(tr("Reload the broadcasts"));
	connect(refresh, &QToolButton::clicked, this, &Dock::loadBroadcasts);
	broadcastRow->addWidget(broadcastBox, 1);
	broadcastRow->addWidget(refresh);
	form->addRow(tr("Broadcast"), broadcastRow);
	connect(broadcastBox, &QComboBox::activated, this, [this]() {
		settings.broadcastId = broadcastBox->currentData().toString();
		settings.save();
	});

	spokenBox = new QComboBox;
	form->addRow(tr("Spoken language"), spokenBox);
	connect(spokenBox, &QComboBox::activated, this, [this]() {
		settings.spokenLanguage = spokenBox->currentData().toString();
		settings.save();
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

	followBox = new QCheckBox(tr("Start and stop with OBS streaming"));
	followBox->setChecked(settings.followStreaming);
	connect(followBox, &QCheckBox::toggled, this, [this](bool on) {
		settings.followStreaming = on;
		settings.save();
	});
	v->addWidget(followBox);

	startButton = new QPushButton(tr("Start"));
	startButton->setMinimumHeight(32);
	connect(startButton, &QPushButton::clicked, this, [this]() {
		if (session->state() == Session::State::Idle)
			session->start();
		else
			session->stop();
	});
	v->addWidget(startButton);

	sessionStatus = new QLabel;
	sessionStatus->setWordWrap(true);
	v->addWidget(sessionStatus);
	v->addStretch(1);
	return page;
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
}

void Dock::signedIn(const QString &key, const QString &account)
{
	settings.apiKey = key;
	settings.account = account;
	settings.save();
	api->setKey(key);
	setStatus(signInStatus, QString(), false);
	showSignedIn(true);
}

// A pasted key is checked with a read before it is kept.
void Dock::useKey()
{
	const QString key = keyEdit->text().trimmed();
	if (key.isEmpty())
		return;
	api->setKey(key);
	setStatus(signInStatus, tr("Checking the key…"), false);
	api->get("/v1/broadcasts", [this, key](int, const QJsonDocument &, const QString &error) {
		if (!error.isEmpty()) {
			api->setKey(settings.apiKey);
			setStatus(signInStatus, tr("VocalStack refused the key: %1").arg(error), true);
			return;
		}
		keyEdit->clear();
		signedIn(key, QString());
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
	settings.followStreaming = kept.followStreaming;
	settings.save();
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
		for (const QJsonValue v : doc.object().value("broadcasts").toArray()) {
			const QJsonObject b = v.toObject();
			if (b.value("archivedAt").isString())
				continue;
			broadcastBox->addItem(b.value("name").toString(), b.value("id").toString());
		}
		const int at = broadcastBox->findData(settings.broadcastId);
		broadcastBox->setCurrentIndex(qMax(0, at));
		if (at < 0 && !settings.broadcastId.isEmpty()) {
			settings.broadcastId.clear(); // deleted or archived since
			settings.save();
		}
	});
}

void Dock::onState(Session::State s)
{
	const bool idle = s == Session::State::Idle;
	startButton->setText(idle ? tr("Start") : s == Session::State::Stopping ? tr("Stopping…") : tr("Stop"));
	startButton->setEnabled(s != Session::State::Stopping);
	broadcastBox->setEnabled(idle);
	spokenBox->setEnabled(idle);
	sendBox->setEnabled(idle);
}

void Dock::obsStreamingStarted()
{
	if (settings.followStreaming && !settings.apiKey.isEmpty())
		session->start();
}

void Dock::obsStreamingStopped()
{
	if (settings.followStreaming)
		session->stop();
}

void Dock::shutdown()
{
	session->stop();
	login->cancel();
}
