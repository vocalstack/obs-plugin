/*
VocalStack for OBS
Copyright (C) 2026 VocalStack <support@vocalstack.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <obs.hpp>

#include <QMainWindow>
#include <QMetaObject>
#include <QWidget>

#include "caption-overlay.hpp"
#include "dock.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static Dock *dock = nullptr;
static CaptionOverlay *overlay = nullptr;
static obs_hotkey_id toggle_hotkey = OBS_INVALID_HOTKEY_ID;

static void on_toggle_hotkey(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed && dock)
		QMetaObject::invokeMethod(dock, []() { dock->toggle(); }, Qt::QueuedConnection);
}

// The hotkey's binding rides in the scene collection, as OBS's own do.
static void on_save(obs_data_t *save_data, bool saving, void *)
{
	if (saving) {
		OBSDataArrayAutoRelease keys = obs_hotkey_save(toggle_hotkey);
		obs_data_set_array(save_data, "vocalstack_toggle_hotkey", keys);
	} else {
		OBSDataArrayAutoRelease keys = obs_data_get_array(save_data, "vocalstack_toggle_hotkey");
		obs_hotkey_load(toggle_hotkey, keys);
	}
}

static void on_frontend_event(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		overlay = CaptionOverlay::attach();
		if (dock) {
			dock->setOverlay(overlay);
			auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
			dock->watchClose(main);
			dock->showFirstTime(main);
		}
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
		if (dock)
			dock->obsOutputStarted();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
		if (dock)
			dock->obsOutputStopped();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		if (dock)
			dock->shutdown();
		delete overlay;
		overlay = nullptr;
		break;
	default:
		break;
	}
}

bool obs_module_load(void)
{
	dock = new Dock();
	if (!obs_frontend_add_dock_by_id("vocalstack", obs_module_text("VocalStack"), dock)) {
		obs_log(LOG_ERROR, "could not add the VocalStack dock");
		delete dock;
		dock = nullptr;
	}
	obs_frontend_add_event_callback(on_frontend_event, nullptr);
	toggle_hotkey = obs_hotkey_register_frontend("vocalstack.toggle", obs_module_text("VocalStack.Toggle"),
						     on_toggle_hotkey, nullptr);
	obs_frontend_add_save_callback(on_save, nullptr);
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);
	obs_frontend_remove_save_callback(on_save, nullptr);
	obs_hotkey_unregister(toggle_hotkey);
	obs_log(LOG_INFO, "plugin unloaded");
}
