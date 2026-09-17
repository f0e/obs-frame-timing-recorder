// stand-in for presentmon's exception utilities, without stack traces or status codes
#pragma once
#include "str/String.h"
#include <exception>
#include <string>
#include <utility>

namespace pmon::util
{
	class Exception : public std::exception
	{
	public:
		Exception() noexcept = default;
		Exception(std::string msg) noexcept : note_(std::move(msg)) {}
		const char* what() const noexcept override { return note_.c_str(); }

	private:
		std::string note_;
	};

	template<class E = Exception, typename... R> auto Except(R&&... args)
	{
		return E{std::forward<R>(args)...};
	}

	inline std::string ReportException(std::string note = {}, std::exception_ptr = {}) noexcept
	{
		return note;
	}

#define PM_DEFINE_EX_FROM(base, name) class name : public base { public: using base::base; }
#define PM_DEFINE_EX(name) class name : public ::pmon::util::Exception { public: using Exception::Exception; }

#define pmquell(stat) try { stat; } catch (...) {}
}
