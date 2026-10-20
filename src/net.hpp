#pragma once

#include <QObject>
#include <QHash>
#include <QTimer>

#include <curl/curl.h>

#include <functional>

// libcurl, driven from Qt's event loop. OBS ships QtNetwork without a TLS
// backend on macOS ("No functional TLS backend was found"), so HTTPS and the
// caption socket's TLS go through libcurl, which OBS itself uses: the
// system's on macOS and Linux, OBS's own libcurl.dll on Windows.
class Net : public QObject {
	Q_OBJECT

public:
	using Done = std::function<void(CURLcode result)>;

	static Net &instance();

	// Runs `easy` until it is done, then calls `done` on the main thread.
	// The handle stays the caller's. With `keepAttached` (a CONNECT_ONLY
	// handle) it stays in the multi after it connects, where its connection
	// lives for curl_easy_send/recv, until remove().
	void add(CURL *easy, Done done, bool keepAttached = false);
	void remove(CURL *easy);

private:
	Net();
	~Net() override;
	void tick();

	CURLM *multi;
	QTimer timer;
	struct Transfer {
		Done done;
		bool keepAttached;
	};
	QHash<CURL *, Transfer> running;
};
