#pragma once

#include "session.hpp"
#include "settings.hpp"

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
class Updater;
class WarmPing;

// The VocalStack dock: sign-in, then what to send and where, Start/Stop,
// and how it is going.
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

private:
	QWidget *buildSignedOut();
	QWidget *buildSignedIn();
	QWidget *buildSettings();
	void showSignedIn(bool in);
	void signedIn(const QString &key, const QString &account);
	void signOut(const QString &why);
	void useKey();
	void loadLists();
	void loadBroadcasts();
	// The broadcast editor, a page of its own: a new broadcast (`id`
	// empty) or the one with `id`, as on VocalStack's dashboard.
	QWidget *buildBroadcastEditor();
	void openBroadcastEditor(const QString &id);
	void saveBroadcast();
	void updateBroadcastEditor();
	void copyBroadcastLink();
	void copyTranscriptLink();
	void openTranscription();
	void onState(Session::State s);
	void setStatus(QLabel *label, const QString &text, bool error);
	void updateFollowHint();
	void updateLinks();
	void showUpdate();
	// A broadcast was chosen: its capture settings become the dock's.
	void applyBroadcastDefaults(const QString &id);

	Api *api;
	Settings settings;
	Session *session;
	ConnectLogin *login;
	CaptionOverlay *overlay = nullptr;
	WarmPing *warm = nullptr;
	Updater *updater;
	bool closeHeld = false;
	// "Update now" was clicked: the update installs once nothing is being
	// transcribed, even with automatic updates off.
	bool updateWanted = false;
	// "Check for updates" was clicked: its answer shows beside it.
	bool checkWanted = false;
	QPushButton *checkUpdatesButton;
	// The update notice: a spinner while it downloads or installs, a check
	// once installed, and × to put it away until it says something else.
	QTimer *updateSpin;
	int updateSpinFrame = 0;
	QString updateNoticeText, updateDismissed;
	QToolButton *updateDismiss;
	// The broadcast editor.
	QString editingId, editingOrigCode;
	bool editCodeTouched = false, editWasPrivate = false;
	QLabel *edTitle, *edLinkPrefix, *edAccessNote, *edLinkNote, *edError;
	QLineEdit *edName, *edCode, *edPassword;
	QComboBox *edAccess, *edSpoken, *edLabels;
	QPushButton *edSave;
	QToolButton *editBroadcastButton, *addBroadcastButton;
	QLabel *checkUpdatesResult;
	// Each broadcast's link and capture settings, by id, from the last load.
	QHash<QString, QString> broadcastLinks;
	QHash<QString, QJsonObject> broadcastSettings;
	// Each broadcast as listed, by id (for the editor).
	QHash<QString, QJsonObject> broadcastObjects;
	// The languages a broadcast can be spoken in: (name, code), sorted.
	QList<QPair<QString, QString>> spokenLanguages;

	QStackedWidget *pages;
	// Signed out.
	QPushButton *signInButton;
	QLineEdit *keyEdit;
	QLabel *signInStatus;
	// Signed in.
	QLabel *accountLabel;
	QComboBox *broadcastBox, *spokenBox, *sendBox, *speakersBox;
	QLineEdit *nameEdit;
	QCheckBox *followBox, *lockBox, *autoUpdateBox;
	QToolButton *copyBroadcastButton;
	QPushButton *copyLinkButton, *openButton;
	QLabel *listenersLabel;
	QPushButton *startButton, *updateButton;
	QLabel *followHint, *sessionStatus, *sessionNotice, *updateNotice;
};
