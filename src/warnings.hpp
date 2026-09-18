#pragma once

#include <string>
#include <string_view>

namespace ft {

	// shows a warning in a message box of its own, so obs keeps running while it's up. the user can turn each
	// warning off from the box itself
	void warn_once(std::string_view key, std::string_view title, std::string_view message);

} // namespace ft
