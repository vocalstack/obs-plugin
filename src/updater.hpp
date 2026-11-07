#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>

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

	// Ask GitHub (at most every 8 hours unless `force`).
	void check(bool force = false);
	// Whether an update may be downloaded and installed now: nothing is
	// being transcribed.
	void setMayInstall(std::function<bool()> may) { mayInstall = std::move(may); }
	// Download and install the newest release.
	void install();
	// OBS is quitting: finish a Windows update that waits for it.
	void finishOnExit();

	QString latest() const { return latestVersion; }
	Age age() const { return ageOf(latestVersion); }
	Step step() const { return st; }
	// What the dock says, and whether it is a problem.
	QString message() const { return text; }
	// Why the last check got no answer; empty when it did.
	QString checkError() const { return lastCheckError; }
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
	std::function<bool()> mayInstall = []() { return true; };
	// Every few minutes: whether 8 hours have passed by the wall clock,
	// which also counts the time a computer slept.
	QTimer again;
	qint64 checkedAt = 0;
	Step st = Step::Idle;
	QString latestVersion, assetUrl, assetSha, text;
	// A release downloaded and checked while something was being
	// transcribed: installed once nothing is.
	QString downloaded, downloadedVersion;
	// Windows: the unpacked release, copied in when OBS quits.
	QString staged;
	bool bad = false;
	QString lastCheckError;
};
