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
class QPushButton;
class QStackedWidget;

// The VocalStack dock: sign-in, then what to send and where, Start/Stop,
// and how it is going.
class Dock : public QWidget {
	Q_OBJECT

public:
	explicit Dock(QWidget *parent = nullptr);

	// OBS's own Start/Stop Streaming, when "with OBS streaming" is on.
	void obsStreamingStarted();
	void obsStreamingStopped();
	// OBS is closing: end the session.
	void shutdown();
	void setOverlay(CaptionOverlay *overlay);

private:
	QWidget *buildSignedOut();
	QWidget *buildSignedIn();
	void showSignedIn(bool in);
	void signedIn(const QString &key, const QString &account);
	void signOut(const QString &why);
	void useKey();
	void loadLists();
	void loadBroadcasts();
	void onState(Session::State s);
	void setStatus(QLabel *label, const QString &text, bool error);

	Api *api;
	Settings settings;
	Session *session;
	ConnectLogin *login;
	CaptionOverlay *overlay = nullptr;

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
	QLabel *sessionStatus;
};
