#pragma once

#include "session.hpp"
#include "settings.hpp"

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

private:
	QWidget *buildSignedOut();
	QWidget *buildSignedIn();
	void showSignedIn(bool in);
	void signedIn(const QString &key, const QString &account);
	void signOut(const QString &why);
	void useKey();
	void loadLists();
	void loadBroadcasts();
	void newBroadcast();
	void onState(Session::State s);
	void setStatus(QLabel *label, const QString &text, bool error);
	void updateFollowHint();

	Api *api;
	Settings settings;
	Session *session;
	ConnectLogin *login;
	CaptionOverlay *overlay = nullptr;
	bool closeHeld = false;

	QStackedWidget *pages;
	// Signed out.
	QPushButton *signInButton;
	QLineEdit *keyEdit;
	QLabel *signInStatus;
	// Signed in.
	QLabel *accountLabel;
	QComboBox *broadcastBox, *spokenBox, *captionBox, *sendBox;
	QCheckBox *followBox;
	QPushButton *startButton;
	QLabel *followHint, *sessionStatus;
};
