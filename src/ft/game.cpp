#include "game.hpp"
#include "win.hpp"

#include <plugin-support.h>
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

		bool holds(const std::vector<GameRecord>& games, uint64_t process_id) {
			return std::ranges::find(games, process_id, &GameRecord::process_id) != games.end();
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

		std::lock_guard lock(mutex);
		for (uint64_t process_id : found) {
			if (holds(games, process_id))
				continue;

			GameRecord& record = games.emplace_back(
				GameRecord{ .process_id = process_id, .capture = (uint64_t)kind, .window = (uint64_t)(uintptr_t)window }
			);
			std::string_view{ executable }.copy(record.name, sizeof(record.name) - 1);
			obs_log(LOG_INFO, "logging the frames %s (%llu) draws", executable, (unsigned long long)process_id);
		}
	}

	bool CapturedGame::presented(uint64_t process_id) const {
		std::lock_guard lock(mutex);
		return holds(games, process_id);
	}

	std::vector<GameRecord> CapturedGame::records() const {
		std::lock_guard lock(mutex);
		return games;
	}

} // namespace ft
