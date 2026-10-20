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

#include "caption-overlay.hpp"
#include "dock.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static Dock *dock = nullptr;
static CaptionOverlay *overlay = nullptr;

static void on_frontend_event(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		overlay = CaptionOverlay::attach();
		if (dock)
			dock->setOverlay(overlay);
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		if (dock)
			dock->obsStreamingStarted();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		if (dock)
			dock->obsStreamingStopped();
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
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);
	obs_log(LOG_INFO, "plugin unloaded");
}
