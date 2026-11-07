#pragma once

#include "session.hpp"
#include "settings.hpp"

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QWidget>

class Api;
class CaptionOverlay;
class ConnectLogin;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QMainWindow;
class QPushButton;
class QStackedWidget;
class QTimer;
class QToolButton;
class QVBoxLayout;
class Updater;
class WarmPing;

namespace ui {
class Callout;
class ElidedLabel;
class ReloadingCombo;
class Segmented;
} // namespace ui

// The VocalStack dock: sign-in, then what to send and where, Start/Stop,
// and how it is going; settings and the broadcast editor are pages of
// their own.
class Dock : public QWidget {
	Q_OBJECT

public:
	explicit Dock(QWidget *parent = nullptr);

	// OBS started or stopped streaming or recording: with "Start and stop
	// with OBS" on, transcription starts with the first and stops when
	// neither runs.
	void obsOutputStarted();
	void obsOutputStopped();
	// OBS is closing: end the session.
	void shutdown();
	void watchClose(QWidget *mainWindow);
	// OBS adds a plugin's dock hidden: on the first run, put it on the
	// right of the main window.
	void showFirstTime(QMainWindow *mainWindow);
	void setOverlay(CaptionOverlay *overlay);
	// Start when idle, else stop: the dock's button, and the
	// "VocalStack: Start/Stop transcribing" hotkey.
	void toggle();

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;
	void changeEvent(QEvent *event) override;

private:
	enum Page { SignedOut, SettingsPage, Main, Editor };

	QWidget *buildSignedOut();
	QWidget *buildSignedIn();
	QWidget *buildSettings();
	QWidget *buildBroadcastEditor();
	// A page's header: ‹ back to the main page, a title, a subtitle.
	QWidget *pageHeader(QLabel **titleLabel, QLabel **subtitleLabel);
	void applyTheme();
	void showSignedIn(bool in);
	void signedIn(const QString &key, const QString &account);
	void signOut(const QString &why);
	void useKey();
	void setSigningIn(bool waiting);
	void loadLists();
	void loadBroadcasts();
	void fillBroadcasts();
	void showBroadcastLink();
	void copyBroadcastLink();
	void copyTranscriptLink();
	void openTranscription();
	void onState(Session::State s);
	void onStatus(const QString &text, Session::Level level);
	void showNotices(const QStringList &notes);
	void tickLive();
	void updateFollowHint();
	void updateLinks();
	void showUpdate();
	// A broadcast was chosen: its capture settings become the dock's.
	void applyBroadcastDefaults(const QString &id);
	void openBroadcastEditor(const QString &id);
	void updateBroadcastEditor();
	void saveBroadcast();

	Api *api;
	Settings settings;
	Session *session;
	ConnectLogin *login;
	CaptionOverlay *overlay = nullptr;
	WarmPing *warm = nullptr;
	Updater *updater;
	bool closeHeld = false;
	bool darkTheme = true;
	QStackedWidget *pages;

	// Each broadcast as listed, by id, in the list's order.
	QHash<QString, QJsonObject> broadcastObjects;
	QStringList broadcastOrder;
	QString shareHost;
	qint64 broadcastsLoadedAt = 0;
	// The languages a broadcast can be spoken in: (name, code), sorted.
	QList<QPair<QString, QString>> spokenLanguages;

	// Signed out.
	QPushButton *signInButton;
	QToolButton *cancelSignIn, *keyToggle;
	QWidget *keyRow;
	QLineEdit *keyEdit;
	QLabel *signInStatus;
	QTimer *signInSpin;
	int signInFrame = 0;

	// Main page.
	QLabel *accountLabel;
	ui::ReloadingCombo *broadcastBox;
	QComboBox *spokenBox;
	ui::Segmented *sendSwitch;
	QToolButton *editBroadcastButton, *copyBroadcastButton;
	ui::ElidedLabel *broadcastLink;
	QPushButton *startButton, *openButton, *copyLinkButton;
	QLabel *followHint, *idleStatus;
	QWidget *transcriptBlock;
	QLabel *transcriptEyebrow;
	// While live: the card under Stop.
	QWidget *liveCard;
	QLabel *liveDot, *liveWord, *liveClock, *liveListeners, *liveStatus;
	QToolButton *captionsToggle;
	QTimer *liveTick;
	QElapsedTimer liveSince;
	Session::Level liveLevel = Session::Level::Ok;
	int liveTicks = 0;
	QVBoxLayout *noticesBox;
	QStringList dismissedNotices;
	QStringList currentNotices;
	QTimer *stopSpin;
	int stopFrame = 0;

	// The update notice at the foot.
	ui::Callout *updateCallout;
	QPushButton *updateButton;
	QTimer *updateSpin;
	int updateSpinFrame = 0;
	QString updateNoticeText, updateDismissed;
	// "Update now" was clicked: the update installs once nothing is being
	// transcribed, even with automatic updates off.
	bool updateWanted = false;

	// Settings page.
	QLineEdit *nameEdit;
	ui::Segmented *speakersSwitch;
	QCheckBox *followBox, *lockBox, *autoUpdateBox;
	QPushButton *checkUpdatesButton;
	QLabel *checkUpdatesResult, *settingsAccount;
	// "Check for updates" was clicked: its answer shows under it.
	bool checkWanted = false;

	// The broadcast editor.
	QString editingId, editingOrigCode;
	bool editCodeTouched = false, editWasPrivate = false;
	QLabel *edTitle, *edSubtitle, *edLinkPrefix, *edLinkNote, *edNameNote, *edAccessNote, *edLabelsNote, *edError;
	QLineEdit *edName, *edCode, *edPassword;
	ui::Segmented *edAccess, *edSpokenMode, *edLabels;
	QComboBox *edSpoken;
	QPushButton *edSave;
};
