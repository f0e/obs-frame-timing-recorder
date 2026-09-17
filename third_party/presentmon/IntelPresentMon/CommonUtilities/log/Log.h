// stand-in for presentmon's logging library, which pulls in far more than presentdata needs. every log
// statement compiles to nothing
#pragma once
#include <string>

namespace pmon::util::log
{
	enum class Level
	{
		None,
		Fatal,
		Error,
		Warning,
		Info,
		Performance,
		Debug,
		Verbose,
		Verbose2,
	};

	struct GlobalPolicy
	{
		static GlobalPolicy& Get() noexcept
		{
			static GlobalPolicy policy;
			return policy;
		}
		Level GetLogLevel() const noexcept { return Level::None; }
	};

	struct NullEntry
	{
		template<typename... T> NullEntry& note(T&&...) noexcept { return *this; }
		template<typename... T> NullEntry& watch(T&&...) noexcept { return *this; }
		template<typename... T> NullEntry& code(T&&...) noexcept { return *this; }
		template<typename... T> NullEntry& hr(T&&...) noexcept { return *this; }
		NullEntry& diag() noexcept { return *this; }

		// presentdata's asserts read `condition || pmlog_warn(...)`
		operator bool() const noexcept { return false; }
	};
}

#define pmlog_(lvl) ::pmon::util::log::NullEntry{}
#define pmlog_fatal ::pmon::util::log::NullEntry{}.note
#define pmlog_error ::pmon::util::log::NullEntry{}.note
#define pmlog_warn ::pmon::util::log::NullEntry{}.note
#define pmlog_info ::pmon::util::log::NullEntry{}.note
#define pmlog_dbg ::pmon::util::log::NullEntry{}.note
#define pmlog_verb(vtag) ::pmon::util::log::NullEntry{}.note
#define pmwatch(expr) watch(#expr, (expr))
