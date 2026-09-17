#include "process_names.hpp"
#include "win.hpp"

#include <tlhelp32.h>

#include <algorithm>
#include <cctype>

namespace ft {

namespace {

char lowered(char c)
{
	return (char)std::tolower((unsigned char)c);
}

} // namespace

bool never_the_game(std::string_view name)
{
	constexpr std::string_view others[] = {"dwm.exe", "obs64.exe"};
	return std::ranges::any_of(others, [&](std::string_view other) {
		return std::ranges::equal(name, other, {}, lowered);
	});
}

void ProcessNames::learn(uint64_t process_id)
{
	{
		std::lock_guard lock(mutex);
		if (names.contains(process_id))
			return;
	}
	refresh();

	// a process that's already gone stays unnamed rather than being looked up again for every present
	std::lock_guard lock(mutex);
	names.try_emplace(process_id, "");
}

std::string ProcessNames::name(uint64_t process_id) const
{
	std::lock_guard lock(mutex);
	auto found = names.find(process_id);
	return found == names.end() ? "" : found->second;
}

std::vector<ProcessRecord> ProcessNames::records(const std::vector<uint64_t> &process_ids) const
{
	std::lock_guard lock(mutex);

	std::vector<ProcessRecord> out;
	for (uint64_t process_id : process_ids) {
		auto found = names.find(process_id);
		if (found == names.end() || found->second.empty())
			continue;

		ProcessRecord record{.process_id = process_id};
		std::ranges::copy(std::string_view{found->second}.substr(0, sizeof(record.name) - 1), record.name);
		out.push_back(record);
	}
	return out;
}

void ProcessNames::refresh()
{
	// a process list snapshot names every process without opening any of them
	HANDLE taken = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (taken == INVALID_HANDLE_VALUE)
		return;
	Handle snapshot{taken};

	std::vector<std::pair<uint64_t, std::string>> found;
	PROCESSENTRY32W entry{.dwSize = sizeof(PROCESSENTRY32W)};
	for (BOOL more = Process32FirstW(snapshot.get(), &entry); more; more = Process32NextW(snapshot.get(), &entry))
		found.emplace_back(entry.th32ProcessID, narrow(entry.szExeFile));

	std::lock_guard lock(mutex);
	for (auto &[process_id, name] : found)
		names[process_id] = std::move(name);
}

} // namespace ft
