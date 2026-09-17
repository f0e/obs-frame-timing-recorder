#include "warnings.hpp"
#include "win.hpp"

#include <obs-module.h>
#include <obs.hpp>
#include <util/platform.h>
#include <util/util.hpp>

#include <mutex>
#include <thread>

namespace ft {

namespace {

constexpr char SETTINGS_FILE[] = "settings.json";

std::mutex settings_mutex;

std::string settings_path()
{
	BPtr<char> path = obs_module_config_path(SETTINGS_FILE);
	return path ? path.Get() : "";
}

bool silenced(const std::string &setting)
{
	std::lock_guard lock(settings_mutex);
	OBSDataAutoRelease settings = obs_data_create_from_json_file_safe(settings_path().c_str(), "bak");
	return settings && obs_data_get_bool(settings, setting.c_str());
}

void silence(const std::string &setting)
{
	std::lock_guard lock(settings_mutex);
	std::string path = settings_path();

	if (BPtr<char> folder = obs_module_config_path(""))
		os_mkdirs(folder);

	OBSDataAutoRelease settings = obs_data_create_from_json_file_safe(path.c_str(), "bak");
	if (!settings)
		settings = obs_data_create();
	obs_data_set_bool(settings, setting.c_str(), true);
	obs_data_save_json_safe(settings, path.c_str(), "tmp", "bak");
}

} // namespace

void warn_once(std::string_view key, std::string_view title, std::string_view message)
{
	std::string setting = "dont_warn_" + std::string(key);
	if (silenced(setting))
		return;

	std::thread([setting, title = widen(title), message = widen(message)] {
		std::wstring text = message + L"\n\n" + widen(obs_module_text("Warning.Repeat"));
		int answer = MessageBoxW(nullptr, text.c_str(), title.c_str(),
					 MB_YESNO | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
		if (answer == IDNO)
			silence(setting);
	}).detach();
}

} // namespace ft
