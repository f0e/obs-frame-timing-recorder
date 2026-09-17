#pragma once

#include "records.hpp"

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ft {

// exe names by process id, remembered from while the processes were running, so a game that has closed by the
// time its log is written still has a name
class ProcessNames {
public:
	void learn(uint64_t process_id);
	std::string name(uint64_t process_id) const;
	std::vector<ProcessRecord> records(const std::vector<uint64_t> &process_ids) const;

private:
	void refresh();

	mutable std::mutex mutex;
	std::unordered_map<uint64_t, std::string> names;
};

// presents from processes that are never the game
bool never_the_game(std::string_view name);

} // namespace ft
