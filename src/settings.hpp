#pragma once

#include <QString>

// What the plugin keeps between runs, in OBS's config folder
// (plugin_config/vocalstack-obs/settings.json). The API key and the stream
// key sit there as OBS keeps its own stream keys: readable by this user.
struct Settings {
	QString apiKey;
	QString account;
	// The app stream key this plugin made (POST /v1/rtmp/keys).
	QString streamKey, streamKeyId, serverUrl;
	// The live transcription each Start records into.
	QString transcriptionId;
	QString broadcastId;
	// The broadcast the transcription is attached to now, detached when
	// another is chosen.
	QString attachedBroadcastId;
	QString spokenLanguage;  // empty: detect
	QString captionLanguage; // empty: the spoken language
	bool sendVideo = false;
	bool followStreaming = true;
	bool showCaptions = true;
	// The dock was put on screen once; after that OBS keeps where it is.
	bool dockShown = false;

	static Settings load();
	void save() const;
};
