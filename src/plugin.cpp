#include "game_timing.hpp"
#include "logs.hpp"
#include "probe.hpp"
#include "recording.hpp"
#include "sidecar.hpp"
#include "warnings.hpp"
#include "win.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/util.hpp>

#include <chrono>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

MODULE_EXPORT const char* obs_module_description(void) {
	return obs_module_text("Plugin.Description");
}

namespace {

	using namespace ft;
	using namespace std::chrono_literals;

	void on_tick(void*, float) {
		tick_log.push({ qpc_now(), obs_get_video_frame_time(), obs_get_total_frames(), obs_get_lagged_frames() });
	}

	void warn_if_unprobed() {
		if (read_log.written() == 0)
			warn_once("no_probe", obs_module_text("Plugin.Name"), obs_module_text("Warning.NoProbe"));
	}

	void warn_about_game_timing() {
		if (game_timing_status() == GameTimingStatus::NO_PERMISSION)
			warn_once("no_permission", obs_module_text("Plugin.Name"), obs_module_text("Warning.NoPermission"));
	}

	// the replay can only reach back as far as the buffer holds, and saving takes a moment on top
	std::chrono::seconds replay_buffer_length() {
		OBSOutputAutoRelease output = obs_frontend_get_replay_buffer_output();
		if (!output)
			return 300s;

		OBSDataAutoRelease settings = obs_output_get_settings(output);
		int64_t held = obs_data_get_int(settings, "max_time_sec");
		return held > 0 ? std::chrono::seconds(held) : 300s;
	}

	void save_replay_sidecar() {
		int64_t saved = qpc_now();

		BPtr<char> replay = obs_frontend_get_last_replay();
		if (!replay)
			return;

		warn_if_unprobed();

		std::string path = std::string(replay.Get()) + std::string(SIDECAR_SUFFIX);
		if (write_replay_sidecar(path, saved, saved - qpc_ticks(replay_buffer_length() + 10s)))
			obs_log(LOG_INFO, "wrote %s", path.c_str());
		else
			obs_log(LOG_WARNING, "couldn't write %s", path.c_str());
	}

	void on_event(enum obs_frontend_event event, void*) {
		switch (event) {
			case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTING:
			case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTED: {
				OBSOutputAutoRelease output = obs_frontend_get_replay_buffer_output();
				replay_packets.attach(output);
				break;
			}
			case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STOPPED:
				replay_packets.detach();
				break;
			case OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED:
				save_replay_sidecar();
				break;
			case OBS_FRONTEND_EVENT_RECORDING_STARTING:
				recording_starting();
				break;
			case OBS_FRONTEND_EVENT_RECORDING_STARTED:
				recording_started();
				break;
			case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
				warn_if_unprobed();
				recording_stopped();
				break;
			case OBS_FRONTEND_EVENT_FINISHED_LOADING:
				warn_about_game_timing();
				break;
			case OBS_FRONTEND_EVENT_EXIT:
				replay_packets.detach();
				finish_recordings();
				stop_game_timing();
				break;
			default:
				break;
		}
	}

} // namespace

bool obs_module_load(void) {
	ft::register_probe();
	ft::start_probe_worker();
	ft::start_game_timing();
	obs_add_tick_callback(on_tick, nullptr);
	obs_frontend_add_event_callback(on_event, nullptr);
	obs_log(LOG_INFO, "loaded - add the Frame Timing Probe filter to the game capture source");
	return true;
}

void obs_module_unload(void) {
	obs_frontend_remove_event_callback(on_event, nullptr);
	obs_remove_tick_callback(on_tick, nullptr);
	ft::replay_packets.detach();
	ft::finish_recordings();
	ft::stop_game_timing();
	ft::stop_probe_worker();
}
