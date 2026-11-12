#include "updater.hpp"

#include "api.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

static const char *RELEASES = "https://api.github.com/repos/vocalstack/obs-plugin/releases/latest";
// Asked again after 8 hours (GitHub allows 60 asks an hour per address).
static const qint64 FRESH_MS = 8 * 60 * 60 * 1000;
// How often the wall clock is looked at: after a sleep, the first look
// finds the 8 hours past and asks.
static const int TICK_MS = 5 * 60 * 1000;

#if defined(__APPLE__)
static const char *ASSET = "vocalstack-obs-macos.pkg";
#elif defined(_WIN32)
static const char *ASSET = "vocalstack-obs-windows-x64.zip";
#else
static const char *ASSET = "vocalstack-obs-linux-x86_64.deb";
#endif

Updater::Updater(Api *api_, QObject *parent) : QObject(parent), api(api_)
{
	again.setInterval(TICK_MS);
	connect(&again, &QTimer::timeout, this, [this]() { check(false); });
}

bool Updater::canInstall()
{
	return true;
}

static QList<int> parts(const QString &v)
{
	QList<int> out;
	for (const QString &p : v.split('.'))
		out.append(p.toInt());
	while (out.size() < 3)
		out.append(0);
	return out;
}

Updater::Age Updater::ageOf(const QString &latest)
{
	if (latest.isEmpty())
		return Age::Current;
	const QList<int> a = parts(PLUGIN_VERSION), b = parts(latest);
	if (b[0] != a[0])
		return b[0] > a[0] ? Age::Unsupported : Age::Current;
	if (b[1] != a[1])
		return b[1] > a[1] ? Age::Deprecated : Age::Current;
	return b[2] > a[2] ? Age::Available : Age::Current;
}

void Updater::say(Step s, const QString &message, bool problem)
{
	st = s;
	text = message;
	bad = problem;
	if (!message.isEmpty())
		obs_log(problem ? LOG_WARNING : LOG_INFO, "update: %s", message.toUtf8().constData());
	emit changed();
}

void Updater::check(bool force)
{
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	if (st != Step::Idle)
		return;
	if (!force && checkedAt && now - checkedAt < FRESH_MS)
		return;
	checkedAt = now;
	if (!again.isActive())
		again.start();
	st = Step::Checking;
	api->getUrl(RELEASES, [this](int, const QJsonDocument &doc, const QString &error) {
		if (st != Step::Checking)
			return;
		const QJsonObject release = doc.object();
		const QString tag = release.value("tag_name").toString();
		static const QRegularExpression version("^\\d+\\.\\d+\\.\\d+$");
		if (!error.isEmpty() || !version.match(tag).hasMatch()) {
			lastCheckError = error.isEmpty() ? tr("GitHub gave no release") : error;
			say(Step::Idle, QString());
			return;
		}
		lastCheckError.clear();
		latestVersion = tag;
		assetUrl.clear();
		assetSha.clear();
		for (const QJsonValue v : release.value("assets").toArray()) {
			const QJsonObject a = v.toObject();
			if (a.value("name").toString() == QLatin1String(ASSET))
				assetUrl = a.value("browser_download_url").toString();
		}
		// The release notes end with "    <file>: <sha256>" per file.
		const QRegularExpression sum(QRegularExpression::escape(QLatin1String(ASSET)) + ":\\s*([0-9a-f]{64})");
		const QRegularExpressionMatch m = sum.match(release.value("body").toString());
		if (m.hasMatch())
			assetSha = m.captured(1);
		if (age() == Age::Current) {
			say(Step::Idle, QString());
			return;
		}
		obs_log(LOG_INFO, "VocalStack for OBS %s is out (this is %s)", tag.toUtf8().constData(), PLUGIN_VERSION);
		say(Step::Idle, QString());
	});
}

void Updater::install()
{
	if (st != Step::Idle || latestVersion.isEmpty())
		return;
	if (!mayInstall()) {
		say(Step::Idle, tr("VocalStack for OBS %1 installs once you stop transcribing.").arg(latestVersion));
		return;
	}
	if (!downloaded.isEmpty() && downloadedVersion == latestVersion && QFile::exists(downloaded)) {
		verifyAndInstall(downloaded);
		return;
	}
	if (assetUrl.isEmpty() || assetSha.isEmpty()) {
		say(Step::Idle, tr("The release has no installer for this system yet"), true);
		return;
	}
	QDir dir(QStandardPaths::writableLocation(QStandardPaths::TempLocation));
	dir.mkpath("vocalstack-obs-update");
	const QString file = dir.filePath("vocalstack-obs-update/" + QString::fromLatin1(ASSET));
	QFile::remove(file);
	say(Step::Downloading, tr("Downloading VocalStack for OBS %1…").arg(latestVersion));
	api->download(assetUrl, file, [this, file](int, const QString &error) {
		if (!error.isEmpty()) {
			say(Step::Idle, tr("Could not download the update: %1").arg(error), true);
			return;
		}
		downloaded = file;
		downloadedVersion = latestVersion;
		// A transcription started meanwhile: installed once it stops.
		if (!mayInstall()) {
			say(Step::Idle, tr("VocalStack for OBS %1 installs once you stop transcribing.").arg(latestVersion));
			return;
		}
		verifyAndInstall(file);
	});
}

// The file is what the release says it is (SHA-256), then installed.
void Updater::verifyAndInstall(const QString &file)
{
	QFile f(file);
	if (!f.open(QIODevice::ReadOnly)) {
		say(Step::Idle, tr("Could not read the downloaded update"), true);
		return;
	}
	QCryptographicHash hash(QCryptographicHash::Sha256);
	hash.addData(&f);
	if (QString::fromLatin1(hash.result().toHex()) != assetSha) {
		QFile::remove(file);
		say(Step::Idle, tr("The downloaded update did not match the release's checksum; nothing was installed"),
		    true);
		return;
	}
	say(Step::Installing, tr("Installing VocalStack for OBS %1…").arg(latestVersion));
#if defined(__APPLE__)
	installMac(file);
#elif defined(_WIN32)
	installWindows(file);
#else
	installLinux(file);
#endif
}

// The same package the person would open, into their own Library (where
// the plugin lives): no administrator password.
void Updater::installMac(const QString &file)
{
	auto *p = new QProcess(this);
	connect(p, &QProcess::finished, this, [this, p](int code, QProcess::ExitStatus) {
		const QString out = QString::fromUtf8(p->readAllStandardError() + p->readAllStandardOutput()).trimmed();
		p->deleteLater();
		if (code != 0) {
			say(Step::Idle, tr("The installer failed (%1): %2").arg(code).arg(out.section('\n', -1)), true);
			return;
		}
		say(Step::Installed, tr("VocalStack for OBS %1 is installed. Restart OBS to use it.").arg(latestVersion));
	});
	connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError) {
		if (st == Step::Installing)
			say(Step::Idle, tr("Could not run the installer: %1").arg(p->errorString()), true);
	});
	p->start("/usr/sbin/installer", {"-pkg", file, "-target", "CurrentUserHomeDirectory"});
}

// The desktop asks for the password (pkexec), then apt installs the .deb.
void Updater::installLinux(const QString &file)
{
	auto *p = new QProcess(this);
	connect(p, &QProcess::finished, this, [this, p](int code, QProcess::ExitStatus) {
		const QString out = QString::fromUtf8(p->readAllStandardError()).trimmed();
		p->deleteLater();
		if (code != 0) {
			say(Step::Idle,
			    code == 126 || code == 127 ? tr("The update was not allowed to install")
						       : tr("The installer failed (%1): %2").arg(code).arg(out.section('\n', -1)),
			    true);
			return;
		}
		say(Step::Installed, tr("VocalStack for OBS %1 is installed. Restart OBS to use it.").arg(latestVersion));
	});
	connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError) {
		if (st == Step::Installing)
			say(Step::Idle, tr("Could not run the installer: %1").arg(p->errorString()), true);
	});
	p->start("pkexec", {"sh", "-c", "apt-get install -y \"$1\" || dpkg -i \"$1\"", "sh", file});
}

// The zip is unpacked now; its folder goes over OBS's when OBS quits
// (finishOnExit), with Windows's permission prompt, as the DLL is in use.
void Updater::installWindows(const QString &file)
{
	const QString dest = QFileInfo(file).dir().filePath("unpacked");
	QDir(dest).removeRecursively();
	auto *p = new QProcess(this);
	connect(p, &QProcess::finished, this, [this, p, dest](int code, QProcess::ExitStatus) {
		const QString out = QString::fromUtf8(p->readAllStandardError()).trimmed();
		p->deleteLater();
		if (code != 0 || !QDir(dest + "/vocalstack-obs").exists()) {
			say(Step::Idle, tr("Could not unpack the update: %1").arg(out.section('\n', -1)), true);
			return;
		}
		staged = dest;
		say(Step::OnExit, tr("VocalStack for OBS %1 is ready. Quit OBS to finish installing it (Windows asks "
				     "for permission), then start OBS again.")
					  .arg(latestVersion));
	});
	connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError) {
		if (st == Step::Installing)
			say(Step::Idle, tr("Could not unpack the update: %1").arg(p->errorString()), true);
	});
	p->start("powershell.exe", {"-NoProfile", "-NonInteractive", "-Command",
				    QStringLiteral("Expand-Archive -Force -LiteralPath '%1' -DestinationPath '%2'")
					    .arg(file, dest)});
}

void Updater::finishOnExit()
{
	if (st != Step::OnExit || staged.isEmpty())
		return;
#if defined(_WIN32)
	// Waits for OBS to let go of the DLL, then copies the folder in.
	const QString script = QDir(staged).filePath("update.cmd");
	QFile f(script);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
		return;
	const QString from = QDir::toNativeSeparators(staged + "/vocalstack-obs");
	f.write(QStringLiteral("@echo off\r\n"
			       ":wait\r\n"
			       "timeout /t 2 /nobreak >nul\r\n"
			       "tasklist /fi \"imagename eq obs64.exe\" | find /i \"obs64.exe\" >nul && goto wait\r\n"
			       "robocopy \"%1\" \"%ProgramData%\\obs-studio\\plugins\\vocalstack-obs\" /E /IS /IT /R:5 /W:2 >nul\r\n"
			       "exit /b 0\r\n")
			.arg(from)
			.toUtf8());
	f.close();
	QProcess::startDetached("powershell.exe",
				{"-NoProfile", "-NonInteractive", "-WindowStyle", "Hidden", "-Command",
				 QStringLiteral("Start-Process -Verb RunAs -WindowStyle Hidden -FilePath cmd.exe -ArgumentList "
						"'/c','\"%1\"'")
					 .arg(QDir::toNativeSeparators(script))});
#endif
}

// What starts OBS again: the .app on macOS (LaunchServices starts it as
// Finder would), the program itself on Linux. Empty where a restart is not
// offered.
static QStringList restart_command()
{
#if defined(__APPLE__)
	// …/OBS.app/Contents/MacOS/OBS
	QDir dir(QFileInfo(QCoreApplication::applicationFilePath()).absolutePath());
	if (dir.dirName() != "MacOS" || !dir.cdUp() || dir.dirName() != "Contents" || !dir.cdUp() ||
	    !dir.dirName().endsWith(".app") || !QFile::exists("/usr/bin/open"))
		return {};
	QStringList cmd{"/usr/bin/open", dir.absolutePath()};
	const QStringList args = QCoreApplication::arguments().mid(1);
	if (!args.isEmpty())
		cmd << "--args" << args;
	return cmd;
#elif defined(_WIN32)
	return {};
#else
	if (QFile::exists("/.flatpak-info") || qEnvironmentVariableIsSet("SNAP") ||
	    qEnvironmentVariableIsSet("APPIMAGE"))
		return {};
	const QString exe = QCoreApplication::applicationFilePath();
	if (exe.isEmpty() || !QFileInfo(exe).isExecutable())
		return {};
	return QStringList{exe} + QCoreApplication::arguments().mid(1);
#endif
}

bool Updater::canRestart()
{
	return !restart_command().isEmpty() && QFile::exists("/bin/sh");
}

// $1: this process, $2: how many seconds to wait for it to end, the rest:
// the command. OBS allows one instance, so the new one starts only once
// this one is gone; when it never ends, nothing is started.
static const char *RESTART_SCRIPT = "pid=$1; left=$2; shift 2; "
				    "while kill -0 \"$pid\" 2>/dev/null; do "
				    "[ \"$left\" -gt 0 ] || exit 1; left=$((left - 1)); sleep 1; "
				    "done; "
				    "exec \"$@\"";
static const int RESTART_WAIT_S = 60;

bool Updater::restartAfterExit()
{
	const QStringList cmd = restart_command();
	if (cmd.isEmpty())
		return false;
	QStringList args{"-c", RESTART_SCRIPT, "sh", QString::number(QCoreApplication::applicationPid()),
			 QString::number(RESTART_WAIT_S)};
	args << cmd;
	const bool ok = QProcess::startDetached("/bin/sh", args);
	obs_log(ok ? LOG_INFO : LOG_WARNING, "restart: %s", ok ? "OBS starts again once it has quit" : "could not start the helper");
	return ok;
}
