#include "settings.hpp"

#include <obs-module.h>
#include <obs.hpp>
#include <util/platform.h>

static QString path()
{
	char *p = obs_module_config_path("settings.json");
	QString s = QString::fromUtf8(p);
	bfree(p);
	return s;
}

static QString str(obs_data_t *d, const char *name)
{
	return QString::fromUtf8(obs_data_get_string(d, name));
}

Settings Settings::load()
{
	Settings s;
	OBSDataAutoRelease d = obs_data_create_from_json_file_safe(path().toUtf8().constData(), "bak");
	if (!d)
		return s;
	obs_data_set_default_bool(d, "followStreaming", true);
	obs_data_set_default_bool(d, "showCaptions", true);
	s.apiKey = str(d, "apiKey");
	s.account = str(d, "account");
	s.streamKey = str(d, "streamKey");
	s.streamKeyId = str(d, "streamKeyId");
	s.serverUrl = str(d, "serverUrl");
	s.transcriptionId = str(d, "transcriptionId");
	s.broadcastId = str(d, "broadcastId");
	s.attachedBroadcastId = str(d, "attachedBroadcastId");
	s.spokenLanguage = str(d, "spokenLanguage");
	s.captionLanguage = str(d, "captionLanguage");
	s.sendVideo = obs_data_get_bool(d, "sendVideo");
	s.followStreaming = obs_data_get_bool(d, "followStreaming");
	s.showCaptions = obs_data_get_bool(d, "showCaptions");
	return s;
}

void Settings::save() const
{
	char *dir = obs_module_config_path("");
	os_mkdirs(dir);
	bfree(dir);

	OBSDataAutoRelease d = obs_data_create();
	auto set = [&](const char *name, const QString &v) { obs_data_set_string(d, name, v.toUtf8().constData()); };
	set("apiKey", apiKey);
	set("account", account);
	set("streamKey", streamKey);
	set("streamKeyId", streamKeyId);
	set("serverUrl", serverUrl);
	set("transcriptionId", transcriptionId);
	set("broadcastId", broadcastId);
	set("attachedBroadcastId", attachedBroadcastId);
	set("spokenLanguage", spokenLanguage);
	set("captionLanguage", captionLanguage);
	obs_data_set_bool(d, "sendVideo", sendVideo);
	obs_data_set_bool(d, "followStreaming", followStreaming);
	obs_data_set_bool(d, "showCaptions", showCaptions);
	obs_data_save_json_safe(d, path().toUtf8().constData(), "tmp", "bak");
}
