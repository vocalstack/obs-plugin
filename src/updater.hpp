#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

class Api;

// The plugin's releases (GitHub, as the Chrome extension's unpacked
// installs): whether a newer one is out, how old this one is next to it
// (by which part of the version moved: a newer patch is an update, a newer
// minor makes this one deprecated, a newer major unsupported), and
// installing it in one click. macOS runs the installer package into the
// user's Library; Linux installs the .deb with the desktop's permission
// prompt; Windows unpacks the zip and copies it into OBS's plugins once
// OBS quits (its DLL is held while OBS runs).
class Updater : public QObject {
	Q_OBJECT

public:
	enum class Age { Current, Available, Deprecated, Unsupported };
	enum class Step { Idle, Checking, Downloading, Installing, Installed, OnExit };

	explicit Updater(Api *api, QObject *parent = nullptr);

	// Ask GitHub (at most every 6 hours unless `force`).
	void check(bool force = false);
	// Download and install the newest release.
	void install();
	// OBS is quitting: finish a Windows update that waits for it.
	void finishOnExit();

	QString latest() const { return latestVersion; }
	Age age() const { return ageOf(latestVersion); }
	Step step() const { return st; }
	// What the dock says, and whether it is a problem.
	QString message() const { return text; }
	bool problem() const { return bad; }
	// Whether this platform can install the release itself.
	static bool canInstall();

signals:
	void changed();

private:
	static Age ageOf(const QString &latest);
	void say(Step s, const QString &message, bool problem = false);
	void verifyAndInstall(const QString &file);
	void installMac(const QString &file);
	void installLinux(const QString &file);
	void installWindows(const QString &file);

	Api *api;
	QTimer again;
	qint64 checkedAt = 0;
	Step st = Step::Idle;
	QString latestVersion, assetUrl, assetSha, text;
	// Windows: the unpacked release, copied in when OBS quits.
	QString staged;
	bool bad = false;
};
