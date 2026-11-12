#pragma once

#include "session.hpp"
#include "settings.hpp"

#include <QElapsedTimer>
#include <QHash>
#include <QImage>
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
class QBoxLayout;
class QToolButton;
class QVBoxLayout;
class TranscriptPanel;
class Updater;
class WarmPing;

namespace ui {
class Callout;
class ElidedLabel;
class LanguageChip;
class ReloadingCombo;
class Segmented;
class Switch;
class WorkspaceMark;
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
	// The replay buffer or the virtual camera started or stopped: whether
	// OBS may be restarted changed.
	void obsActivityChanged();
	// OBS is closing: end the session.
	void shutdown();
	void watchClose(QWidget *mainWindow);
	// OBS adds a plugin's dock hidden: on the first run, put it on the
	// right of the main window.
	void showFirstTime(QMainWindow *mainWindow);
	void setOverlay(CaptionOverlay *overlay);
	// The transcript panel's dock: fed by the session, popped out from the
	// live card.
	void setTranscriptPanel(TranscriptPanel *panel);
	// Start when idle, else stop: the dock's button, and the
	// "VocalStack: Start/Stop transcribing" hotkey.
	void toggle();

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;
	void changeEvent(QEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;

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
	// `stay`: the page on screen stays (Settings, after a workspace change).
	void showSignedIn(bool in, bool stay);
	void signedIn(const QString &key, const QString &account, const Workspace &workspace, bool viaConnect);
	// The workspace the key belongs to, as the API just named it: kept, and
	// its icon fetched when it is another one than the icon kept.
	void setWorkspace(const Workspace &workspace);
	// Asks VocalStack which workspace the key belongs to (a rename or a new
	// icon in the dashboard reaches the plugin this way).
	void refreshWorkspace();
	void showWorkspace();
	// Settings' Change: the consent screen again, where another workspace
	// is picked; its answer is a new key that takes this one's place.
	void changeWorkspace();
	void workspaceChosen(const QString &key, const QString &account, const Workspace &workspace);
	void signOut(const QString &why);
	void useKey();
	void setSigningIn(bool waiting);
	void loadLists();
	void loadBroadcasts(bool choose = false);
	void fillBroadcasts();
	void showBroadcastLink();
	void copyBroadcastLink();
	void copyTranscriptLink();
	void openTranscription();
	void onState(Session::State s);
	void onStatus(const QString &text, Session::Level level);
	void showNotices(const QStringList &warnings, const QStringList &errors);
	// Back to the main page; from the editor, after asking when something
	// was changed.
	void goBack();
	void updateLiveSummary();
	void tickLive();
	void updateFollowHint();
	void updateLinks();
	void showUpdate();
	// What must stop before OBS may be restarted, as a sentence; empty when
	// nothing is in the way.
	QString restartBlocker() const;
	// Restart OBS, after an update was installed: OBS is asked to quit as
	// its own Quit does, and starts again once it has (shutdown()).
	void restartObs();
	// A broadcast was chosen: its capture settings become the dock's.
	void applyBroadcastDefaults(const QString &id);
	// The chosen broadcast's scripts not yet recorded, and which one Start
	// follows (by default the one due nearest to now within 12 h).
	void loadScripts();
	void showScripts();
	void startSession();
	void openBroadcastEditor(const QString &id);
	void updateBroadcastEditor();
	void saveBroadcast();

	Api *api;
	Settings settings;
	Session *session;
	ConnectLogin *login;
	// Change's own sign-in, so cancelling it leaves the key in use alone.
	ConnectLogin *workspaceLogin;
	bool changingWorkspace = false;
	QString workspaceError;
	// The workspace's own icon (null: none), and the icon being fetched.
	QImage workspaceIcon;
	QString workspaceIconWanted;
	CaptionOverlay *overlay = nullptr;
	WarmPing *warm = nullptr;
	Updater *updater;
	bool closeHeld = false;
	bool darkTheme = true;
	QStackedWidget *pages;
	QLabel *logoLabel, *joinLabel;

	// Each broadcast as listed, by id, in the list's order.
	QHash<QString, QJsonObject> broadcastObjects;
	QStringList broadcastOrder;
	QString shareHost;
	qint64 broadcastsLoadedAt = 0;
	// The broadcast whose defaults the dock last took, and the list as last
	// shown (ids and names), so a reload changes neither for nothing.
	QString appliedBroadcastId, broadcastsShown;
	bool broadcastsFailed = false;
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
	ui::WorkspaceMark *footMark;
	ui::ReloadingCombo *broadcastBox;
	ui::LanguageChip *spokenBox;
	// Auto detect or Custom, beside the chip; the language Custom goes back
	// to.
	ui::Segmented *spokenMode;
	QString lastCustomLanguage;
	void chooseSpoken(const QString &lang);
	ui::Segmented *sendSwitch;
	QPushButton *editBroadcastButton;
	QToolButton *copyBroadcastButton, *broadcastAction;
	QWidget *spokenBlock, *sendBlock;
	QLabel *liveSummary;
	QBoxLayout *transcriptButtons;
	qint64 lastDurationS = -1;
	ui::ElidedLabel *broadcastLink;
	QPushButton *startButton, *openButton, *copyLinkButton;
	QLabel *followHint, *idleStatus;
	QWidget *transcriptBlock;
	QLabel *transcriptEyebrow;
	// While live: the card under Stop.
	QWidget *liveCard;
	QLabel *liveDot, *liveWord, *liveClock, *liveListeners, *liveStatus;
	QToolButton *captionsToggle, *popOutButton;
	TranscriptPanel *transcriptPanel = nullptr;
	QTimer *liveTick;
	QElapsedTimer liveSince;
	Session::Level liveLevel = Session::Level::Ok;
	int liveTicks = 0;
	QVBoxLayout *noticesBox;
	QWidget *scriptBlock;
	QVBoxLayout *scriptRows, *scriptFoot;
	// Three scripts show at a time; more scroll, with a hint saying how
	// many and where.
	class QScrollArea *scriptScroll;
	QLabel *scriptCount, *scriptHint;
	void updateScriptHint();
	QList<QJsonObject> scripts;
	// The broadcast the scripts are for; the script picked ("" for none),
	// and whether the person picked it (else the default is kept fresh).
	QString scriptsFor, chosenScript;
	bool scriptPicked = false;
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
	// Restart OBS, under the notice once the update is installed (macOS and
	// Linux). Clicked: OBS was asked to quit and starts again when it does;
	// the wish lapses when OBS is still open half a minute later.
	QPushButton *restartButton;
	QTimer *restartLapse;
	bool restartWanted = false;

	// Settings page.
	QLineEdit *nameEdit;
	ui::Segmented *speakersSwitch;
	ui::Switch *followBox, *lockBox, *autoUpdateBox;
	QPushButton *checkUpdatesButton;
	QLabel *checkUpdatesResult, *settingsAccount, *settingsLiveNote;
	QPushButton *signOutButton;
	QWidget *workspaceSection;
	ui::WorkspaceMark *workspaceMark;
	ui::ElidedLabel *workspaceName;
	QLabel *workspaceNote;
	QPushButton *changeWorkspaceButton;
	QToolButton *captionColorButton, *captionColorReset;
	void showCaptionColor();
	QLabel *signOutNote;
	// "Check for updates" was clicked: its answer shows under it.
	bool checkWanted = false;

	// The broadcast editor.
	QString editingId, editingOrigCode;
	bool editCodeTouched = false, editWasPrivate = false, editDirty = false;
	QWidget *edPasswordBlock;
	QToolButton *edPasswordShow;
	QLabel *edLinkEyebrow;
	QLabel *edTitle, *edSubtitle, *edLinkNote, *edAccessNote, *edLabelsNote, *edError;
	ui::ElidedLabel *edLinkPrefix;
	QLineEdit *edName, *edCode, *edPassword;
	ui::Segmented *edAccess, *edSpokenMode, *edLabels;
	QComboBox *edSpoken;
	QPushButton *edSave;
};
