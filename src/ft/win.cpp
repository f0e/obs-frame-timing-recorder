#include "win.hpp"

#include <thread>

namespace ft::win {

	std::wstring widen(std::string_view text) {
		if (text.empty())
			return {};

		int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
		std::wstring out(size, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), size);
		return out;
	}

	bool running_elevated() {
		HANDLE taken = nullptr;
		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &taken))
			return false;
		Handle token{ taken };

		TOKEN_ELEVATION elevation{};
		DWORD size = 0;
		return GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &size) &&
		       elevation.TokenIsElevated;
	}

	bool in_performance_log_users() {
		SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
		PSID group = nullptr;
		if (!AllocateAndInitializeSid(
				&authority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_LOGGING_USERS, 0, 0, 0, 0, 0, 0, &group
			))
			return false;

		BOOL member = FALSE;
		if (!CheckTokenMembership(nullptr, group, &member))
			member = FALSE;
		FreeSid(group);
		return member != FALSE;
	}

	void warn(std::string_view title, std::string_view message) {
		std::thread([title = win::widen(title), message = win::widen(message)] {
			MessageBoxW(
				nullptr, message.c_str(), title.c_str(), MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST
			);
		}).detach();
	}

} // namespace ft::win
