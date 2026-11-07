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
	// The transcription's name in VocalStack; empty: "OBS on <host>".
	QString transcriptionName;
	// VocalStack's recording settings, sent with the transcription:
	// speaker labels (auto, on, off), latency mode (auto, fast, accuracy).
	QString speakerLabels = "auto";
	QString latencyMode = "auto";
	// The transcription cannot be archived in VocalStack while locked.
	bool locked = false;
	// The longest live recording the plan allows, in seconds, as
	// VocalStack last said (GET /v1/realtime/capabilities); 0: unknown.
	int maxDurationS = 0;
	bool sendVideo = false;
	bool followObs = true;
	// Install a newer release when one is out and nothing is being
	// transcribed (else the dock offers it).
	bool autoUpdate = true;
	bool showCaptions = true;
	// The dock was put on screen once; after that OBS keeps where it is.
	bool dockShown = false;

	static Settings load();
	void save() const;
};
