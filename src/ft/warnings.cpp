#include "warnings.hpp"
#include "win.hpp"

#include <thread>

namespace ft {

	void warn(std::string_view title, std::string_view message) {
		std::thread([title = win::widen(title), message = win::widen(message)] {
			MessageBoxW(
				nullptr, message.c_str(), title.c_str(), MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST
			);
		}).detach();
	}

} // namespace ft
