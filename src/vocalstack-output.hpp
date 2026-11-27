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

	// `track`: OBS's audio track (mixer) to send, 1 to 6.
	bool start(const QString &serverUrl, const QString &streamKey, bool video, int track);
	void stop();
	bool active() const;

signals:
	// On the Qt main thread. `error` is empty for a stop that was asked
	// for; `keyRefused` when VocalStack does not know the stream key;
	// `lost` when the network failed (could not connect, or the
	// connection dropped and OBS's own retries ran out).
	void started();
	void stopped(const QString &error, bool keyRefused, bool lost);
	void reconnecting();
	// OBS's own reconnect got through.
	void reconnected();

private:
	static void onStart(void *data, calldata_t *cd);
	static void onStop(void *data, calldata_t *cd);
	static void onReconnect(void *data, calldata_t *cd);
	static void onReconnected(void *data, calldata_t *cd);

	OBSOutputAutoRelease output;
	OBSServiceAutoRelease service;
	OBSEncoderAutoRelease videoEncoder, audioEncoder;
};
