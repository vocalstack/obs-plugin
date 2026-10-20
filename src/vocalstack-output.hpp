#pragma once

#include <obs.hpp>

#include <QObject>
#include <QString>

// The stream to VocalStack: an RTMP output of its own, next to OBS's main
// stream and with its own encoders, so it runs whether or not OBS streams
// elsewhere. OBS's RTMP output always carries video; with `video` off it
// sends a 160x90 picture at a few kbps, which VocalStack ignores.
class VocalStackOutput : public QObject {
	Q_OBJECT

public:
	explicit VocalStackOutput(QObject *parent = nullptr);
	~VocalStackOutput() override;

	bool start(const QString &serverUrl, const QString &streamKey, bool video);
	void stop();
	bool active() const;

signals:
	// On the Qt main thread. `error` is empty for a stop that was asked
	// for; `keyRefused` when VocalStack does not know the stream key.
	void started();
	void stopped(const QString &error, bool keyRefused);
	void reconnecting();

private:
	static void onStart(void *data, calldata_t *cd);
	static void onStop(void *data, calldata_t *cd);
	static void onReconnect(void *data, calldata_t *cd);

	OBSOutputAutoRelease output;
	OBSServiceAutoRelease service;
	OBSEncoderAutoRelease videoEncoder, audioEncoder;
};
