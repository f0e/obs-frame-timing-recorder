#include "game.hpp"
#include "win.hpp"

#include <plugin-support.h>

#include <tlhelp32.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace ft {

namespace {

// the window the capture named belongs to the game, so its thread's process is the one presenting
HWND find_window(const char *title, const char *window_class)
{
	std::wstring wide_class = widen(window_class ? window_class : "");
	std::wstring wide_title = widen(title ? title : "");

	return FindWindowW(wide_class.empty() ? nullptr : wide_class.c_str(),
			   wide_title.empty() ? nullptr : wide_title.c_str());
}

// the title can have changed since it was hooked, so fall back to whoever is running that exe. two copies of
// the same game are rare, and logging both of them is harmless
std::vector<uint64_t> by_executable(const char *executable)
{
	HANDLE taken = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (taken == INVALID_HANDLE_VALUE)
		return {};
	Handle snapshot{taken};

	std::vector<uint64_t> found;
	PROCESSENTRY32W entry{.dwSize = sizeof(PROCESSENTRY32W)};
	for (BOOL more = Process32FirstW(snapshot.get(), &entry); more; more = Process32NextW(snapshot.get(), &entry)) {
		if (_stricmp(narrow(entry.szExeFile).c_str(), executable) == 0)
			found.push_back(entry.th32ProcessID);
	}
	return found;
}

GameRecord record_of(uint64_t process_id, CaptureKind capture, HWND window, const char *executable)
{
	GameRecord record{.process_id = process_id,
			  .capture = (uint64_t)capture,
			  .window = (uint64_t)(uintptr_t)window};
	std::string_view name = executable;
	std::ranges::copy(name.substr(0, sizeof(record.name) - 1), record.name);
	return record;
}

// game capture hooks the game and copies its buffer; window capture is handed the window by the compositor
CaptureKind kind_of(obs_source_t *capture)
{
	const char *id = obs_source_get_id(capture);
	if (!id)
		return CaptureKind::OTHER;
	if (strcmp(id, "game_capture") == 0)
		return CaptureKind::HOOK;
	if (strcmp(id, "window_capture") == 0)
		return CaptureKind::WINDOW;
	return CaptureKind::OTHER;
}

} // namespace

void CapturedGame::watch(obs_source_t *capture)
{
	unwatch();
	if (!capture)
		return;

	kind = kind_of(capture);

	signal_handler_t *signals = obs_source_get_signal_handler(capture);
	on_hooked.Connect(
		signals, "hooked",
		[](void *param, calldata_t *params) {
			static_cast<CapturedGame *>(param)->hooked(calldata_string(params, "executable"),
								   calldata_string(params, "title"),
								   calldata_string(params, "class"));
		},
		this);
	on_unhooked.Connect(signals, "unhooked", [](void *, calldata_t *) {}, this);

	// it may have hooked its window before the filter was added, and a source without the call is neither a
	// game capture nor a window capture
	calldata_t hook{};
	bool answered = proc_handler_call(obs_source_get_proc_handler(capture), "get_hooked", &hook);
	if (answered && calldata_bool(&hook, "hooked"))
		hooked(calldata_string(&hook, "executable"), calldata_string(&hook, "title"),
		       calldata_string(&hook, "class"));
	else if (!answered)
		obs_log(LOG_WARNING,
			"the Frame Timing Probe is on \"%s\", which is neither a Game Capture nor a Window "
			"Capture source - game frames won't be logged",
			obs_source_get_name(capture));
	calldata_free(&hook);
}

void CapturedGame::unwatch()
{
	on_hooked.Disconnect();
	on_unhooked.Disconnect();
}

void CapturedGame::hooked(const char *executable, const char *title, const char *window_class)
{
	if (!executable || !*executable)
		return;

	HWND window = find_window(title, window_class);
	DWORD presenter = 0;
	if (window)
		GetWindowThreadProcessId(window, &presenter);

	// the title can have changed since it was captured, so fall back to whoever runs that exe
	std::vector<uint64_t> found = presenter ? std::vector<uint64_t>{presenter} : by_executable(executable);
	if (found.empty()) {
		obs_log(LOG_WARNING, "obs captured %s, but it isn't running any more", executable);
		return;
	}

	std::lock_guard lock(mutex);
	for (uint64_t process_id : found) {
		bool known = std::ranges::any_of(games, [process_id](const GameRecord &game) {
			return game.process_id == process_id;
		});
		if (!known) {
			games.push_back(record_of(process_id, kind, window, executable));
			obs_log(LOG_INFO, "logging the frames %s (%llu) draws", executable,
				(unsigned long long)process_id);
		}
	}
}

bool CapturedGame::presented(uint64_t process_id) const
{
	std::lock_guard lock(mutex);
	return std::ranges::any_of(games,
				   [process_id](const GameRecord &game) { return game.process_id == process_id; });
}

std::vector<GameRecord> CapturedGame::records() const
{
	std::lock_guard lock(mutex);
	return games;
}

} // namespace ft
