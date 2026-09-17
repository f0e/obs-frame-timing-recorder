// stand-in for the one string helper presentdata uses
#pragma once
#include <string>
#include <windows.h>

namespace pmon::util::str
{
	inline std::string ToNarrow(const std::wstring& wide)
	{
		if (wide.empty())
			return {};
		int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
		std::string out(size, '\0');
		WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), out.data(), size, nullptr, nullptr);
		return out;
	}
}
