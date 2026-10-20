#include "vocalstack-output.hpp"

#include <plugin-support.h>

#include <QMetaObject>

VocalStackOutput::VocalStackOutput(QObject *parent) : QObject(parent) {}

VocalStackOutput::~VocalStackOutput()
{
	if (output) {
		signal_handler_t *sh = obs_output_get_signal_handler(output);
		signal_handler_disconnect(sh, "start", onStart, this);
		signal_handler_disconnect(sh, "stop", onStop, this);
		signal_handler_disconnect(sh, "reconnect", onReconnect, this);
		obs_output_force_stop(output);
	}
}

bool VocalStackOutput::active() const
{
	return output && obs_output_active(output);
}

// The canvas's output size, at most 1280 wide, in even pixels.
static void video_size(bool video, uint32_t &w, uint32_t &h)
{
	if (!video) {
		w = 160;
		h = 90;
		return;
	}
	obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	w = ovi.output_width;
	h = ovi.output_height;
	if (w > 1280) {
		h = uint32_t(uint64_t(h) * 1280 / w);
		w = 1280;
	}
	w &= ~1u;
	h &= ~1u;
}

bool VocalStackOutput::start(const QString &serverUrl, const QString &streamKey, bool video)
{
	if (active())
		return true;
	OBSDataAutoRelease serviceSettings = obs_data_create();
	obs_data_set_string(serviceSettings, "server", serverUrl.toUtf8().constData());
	obs_data_set_string(serviceSettings, "key", streamKey.toUtf8().constData());
	service = obs_service_create_private("rtmp_custom", "vocalstack_service", serviceSettings);

	OBSDataAutoRelease videoSettings = obs_data_create();
	obs_data_set_string(videoSettings, "rate_control", "CBR");
	obs_data_set_int(videoSettings, "bitrate", video ? 2500 : 16);
	obs_data_set_int(videoSettings, "keyint_sec", 2);
	obs_data_set_string(videoSettings, "preset", "veryfast");
	videoEncoder = obs_video_encoder_create("obs_x264", "vocalstack_video", videoSettings, nullptr);

	OBSDataAutoRelease audioSettings = obs_data_create();
	obs_data_set_int(audioSettings, "bitrate", 128);
	audioEncoder = obs_audio_encoder_create("ffmpeg_aac", "vocalstack_audio", audioSettings, 0, nullptr);

	if (!service || !videoEncoder || !audioEncoder) {
		obs_log(LOG_ERROR, "could not create the VocalStack service or encoders");
		return false;
	}
	uint32_t w, h;
	video_size(video, w, h);
	obs_encoder_set_scaled_size(videoEncoder, w, h);
	obs_encoder_set_video(videoEncoder, obs_get_video());
	obs_encoder_set_audio(audioEncoder, obs_get_audio());

	if (!output) {
		output = obs_output_create("rtmp_output", "vocalstack_output", nullptr, nullptr);
		if (!output) {
			obs_log(LOG_ERROR, "could not create the VocalStack RTMP output");
			return false;
		}
		signal_handler_t *sh = obs_output_get_signal_handler(output);
		signal_handler_connect(sh, "start", onStart, this);
		signal_handler_connect(sh, "stop", onStop, this);
		signal_handler_connect(sh, "reconnect", onReconnect, this);
	}
	obs_output_set_service(output, service);
	obs_output_set_video_encoder(output, videoEncoder);
	obs_output_set_audio_encoder(output, audioEncoder, 0);
	obs_output_set_reconnect_settings(output, 20, 3);

	if (!obs_output_start(output)) {
		const char *err = obs_output_get_last_error(output);
		obs_log(LOG_ERROR, "the VocalStack output did not start: %s", err ? err : "no reason given");
		return false;
	}
	obs_log(LOG_INFO, "streaming to VocalStack (%s, %ux%u)", video ? "audio and video" : "audio", w, h);
	return true;
}

void VocalStackOutput::stop()
{
	if (!active())
		return;
	obs_output_stop(output);
}

void VocalStackOutput::onStart(void *data, calldata_t *)
{
	auto *self = static_cast<VocalStackOutput *>(data);
	QMetaObject::invokeMethod(self, [self]() { emit self->started(); }, Qt::QueuedConnection);
}

void VocalStackOutput::onReconnect(void *data, calldata_t *)
{
	auto *self = static_cast<VocalStackOutput *>(data);
	QMetaObject::invokeMethod(self, [self]() { emit self->reconnecting(); }, Qt::QueuedConnection);
}

void VocalStackOutput::onStop(void *data, calldata_t *cd)
{
	auto *self = static_cast<VocalStackOutput *>(data);
	const long long code = calldata_int(cd, "code");
	QString error;
	const bool keyRefused = code == OBS_OUTPUT_BAD_PATH || code == OBS_OUTPUT_INVALID_STREAM;
	if (code != OBS_OUTPUT_SUCCESS) {
		const char *last = obs_output_get_last_error(self->output);
		switch (code) {
		case OBS_OUTPUT_BAD_PATH:
		case OBS_OUTPUT_INVALID_STREAM:
			error = QObject::tr("VocalStack refused the stream key");
			break;
		case OBS_OUTPUT_CONNECT_FAILED:
			error = QObject::tr("Could not connect to VocalStack");
			break;
		case OBS_OUTPUT_DISCONNECTED:
			error = QObject::tr("The connection to VocalStack was lost");
			break;
		default:
			error = last && *last ? QString::fromUtf8(last) : QObject::tr("The stream to VocalStack stopped");
		}
	}
	QMetaObject::invokeMethod(self, [self, error, keyRefused]() { emit self->stopped(error, keyRefused); },
				  Qt::QueuedConnection);
}
