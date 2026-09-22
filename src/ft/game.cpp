#include "game.hpp"
#include "win.hpp"

#include <plugin-support.h>
#include <util/util_uint64.h>
#include <util/windows/window-helpers.h>

#include <tlhelp32.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace ft {

	namespace {

		// the last resort for a game whose window obs cannot match. two copies of the same exe are rare, and
		// logging both of them is harmless
		std::vector<uint64_t> by_executable(const char* executable) {
			HANDLE taken = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
			if (taken == INVALID_HANDLE_VALUE)
				return {};
			win::Handle snapshot{ taken };

			std::wstring wanted = win::widen(executable);
			std::vector<uint64_t> found;
			PROCESSENTRY32W entry{ .dwSize = sizeof(PROCESSENTRY32W) };
			for (BOOL more = Process32FirstW(snapshot.get(), &entry); more;
			     more = Process32NextW(snapshot.get(), &entry))
			{
				if (_wcsicmp(entry.szExeFile, wanted.c_str()) == 0)
					found.push_back(entry.th32ProcessID);
			}
			return found;
		}

		GameRecord* held(std::vector<GameRecord>& games, uint64_t process_id) {
			auto found = std::ranges::find(games, process_id, &GameRecord::process_id);
			return found == games.end() ? nullptr : &*found;
		}

		// the interval game-capture.c hands the hook, which is what it skips presents on. it's half an obs
		// frame, or a whole one with "limit capture framerate" on (reset_frame_interval, game-capture.c)
		uint64_t frame_interval(bool limit_framerate) {
			obs_video_info video{};
			if (!obs_get_video_info(&video) || !video.fps_num)
				return 0;

			uint64_t interval = util_mul_div64(video.fps_den, 1000000000ULL, video.fps_num);
			return limit_framerate ? interval : interval / 2;
		}

		// game capture hooks the game and copies its buffer; window capture is handed the window by the compositor
		CaptureKind kind_of(obs_source_t* capture) {
			const char* id = obs_source_get_id(capture);
			if (!id)
				return CaptureKind::OTHER;
			if (strcmp(id, "game_capture") == 0)
				return CaptureKind::HOOK;
			if (strcmp(id, "window_capture") == 0)
				return CaptureKind::WINDOW;
			return CaptureKind::OTHER;
		}

	} // namespace

	void CapturedGame::watch(obs_source_t* capture) {
		unwatch();
		if (!capture)
			return;

		kind = kind_of(capture);
		source = OBSGetWeakRef(capture);

		on_hooked.Connect(
			obs_source_get_signal_handler(capture),
			"hooked",
			[](void* param, calldata_t* params) {
				static_cast<CapturedGame*>(param)->hooked(
					calldata_string(params, "executable"),
					calldata_string(params, "title"),
					calldata_string(params, "class")
				);
			},
			this
		);

		// it may have hooked its window before the filter was added, and a source without the call is neither a
		// game capture nor a window capture
		calldata_t hook{};
		bool answered = proc_handler_call(obs_source_get_proc_handler(capture), "get_hooked", &hook);
		if (answered && calldata_bool(&hook, "hooked"))
			hooked(
				calldata_string(&hook, "executable"), calldata_string(&hook, "title"), calldata_string(&hook, "class")
			);
		else if (!answered)
			obs_log(
				LOG_WARNING,
				"the Frame Timing Probe is on \"%s\", which is neither a Game Capture nor a Window "
				"Capture source - game frames won't be logged",
				obs_source_get_name(capture)
			);
		calldata_free(&hook);
	}

	void CapturedGame::unwatch() {
		on_hooked.Disconnect();
		source = nullptr;
	}

	// what the capture source's own settings mean for the hook, which blur can't work out from anywhere else
	uint64_t CapturedGame::capture_flags() const {
		OBSSourceAutoRelease capture = obs_weak_source_get_source(source);
		if (!capture || kind != CaptureKind::HOOK)
			return 0;

		OBSDataAutoRelease settings = obs_source_get_settings(capture);
		return (obs_data_get_bool(settings, "limit_framerate") ? CAPTURE_LIMIT_FRAMERATE : 0) |
		       (obs_data_get_bool(settings, "sli_compatibility") ? CAPTURE_SHARED_MEMORY : 0);
	}

	void CapturedGame::hooked(const char* executable, const char* title, const char* window_class) {
		if (!executable || !*executable)
			return;

		// obs's own matcher, so it lands on the window obs captured: it weighs the exe as well as the class and
		// title, tolerates a title that has changed since the hook, and unwraps a uwp window onto the app's own
		HWND window = ms_find_window(
			INCLUDE_MINIMIZED, WINDOW_PRIORITY_EXE, window_class ? window_class : "", title ? title : "", executable
		);
		DWORD presenter = 0;
		if (window)
			GetWindowThreadProcessId(window, &presenter);

		std::vector<uint64_t> found = presenter ? std::vector<uint64_t>{ presenter } : by_executable(executable);
		if (found.empty()) {
			obs_log(LOG_WARNING, "obs captured %s, but it isn't running any more", executable);
			return;
		}

		uint64_t flags = capture_flags();
		uint64_t interval = kind == CaptureKind::HOOK ? frame_interval(flags & CAPTURE_LIMIT_FRAMERATE) : 0;

		std::lock_guard lock(mutex);
		for (uint64_t process_id : found) {
			// a re-hook, which is what a change to the capture's settings causes, brings the new settings with it
			if (GameRecord* existing = held(games, process_id)) {
				existing->flags = flags;
				existing->frame_interval = interval;
				existing->window = (uint64_t)(uintptr_t)window;
				continue;
			}

			GameRecord& record = games.emplace_back(GameRecord{
				.process_id = process_id,
				.capture = (uint64_t)kind,
				.window = (uint64_t)(uintptr_t)window,
				.flags = flags,
				.frame_interval = interval,
			});
			std::string_view{ executable }.copy(record.name, sizeof(record.name) - 1);
			obs_log(
				LOG_INFO,
				"logging the frames %s (%llu) draws, skipped on a %.2fms interval%s",
				executable,
				(unsigned long long)process_id,
				(double)interval / 1e6,
				(flags & CAPTURE_SHARED_MEMORY) ? ", in compatibility mode - the timing model doesn't cover that" : ""
			);
		}
	}

	bool CapturedGame::presented(uint64_t process_id) const {
		std::lock_guard lock(mutex);
		return std::ranges::find(games, process_id, &GameRecord::process_id) != games.end();
	}

	std::vector<GameRecord> CapturedGame::records() const {
		std::lock_guard lock(mutex);
		return games;
	}

} // namespace ft
