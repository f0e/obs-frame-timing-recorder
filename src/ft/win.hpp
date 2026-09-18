#pragma once

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace ft::win {

	inline int64_t qpc_now() {
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		return now.QuadPart;
	}

	inline int64_t qpc_frequency() {
		static const int64_t frequency = [] {
			LARGE_INTEGER value;
			QueryPerformanceFrequency(&value);
			return value.QuadPart;
		}();
		return frequency;
	}

	inline int64_t qpc_ticks(std::chrono::seconds seconds) {
		return seconds.count() * qpc_frequency();
	}

	// obs hands out utf-8, windows' apis want utf-16
	std::wstring widen(std::string_view text);
	std::string narrow(std::wstring_view text);

	inline std::filesystem::path as_path(std::string_view utf8) {
		return std::filesystem::path{ widen(utf8) };
	}

	// both let a process start an event tracing session
	bool running_elevated();
	bool in_performance_log_users();

	struct HandleCloser {
		void operator()(HANDLE handle) const noexcept {
			CloseHandle(handle);
		}
	};

	using Handle = std::unique_ptr<void, HandleCloser>;

} // namespace ft::win
