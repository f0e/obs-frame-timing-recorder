#pragma once

#include <string_view>

namespace ft {

	// shows a warning in a message box of its own, so obs keeps running while it's up
	void warn(std::string_view title, std::string_view message);

} // namespace ft
